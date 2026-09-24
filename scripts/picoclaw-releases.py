#!/usr/bin/env python3
"""GitHub release discovery and transactional updates for the native runtime."""

import contextlib
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import ssl
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.github.com/repos/sipeed/picoclaw/releases"
MAX_JSON = 16 * 1024 * 1024
MAX_ARCHIVE = 256 * 1024 * 1024


class UpdateError(Exception):
    pass


def one_line(value, limit=240):
    return " ".join(str(value).split())[:limit]


def request(url):
    if urllib.parse.urlparse(url).scheme != "https":
        raise UpdateError("Release downloads require HTTPS.")
    headers = {"User-Agent": "lilygo-ui-picoclaw", "Accept": "application/vnd.github+json"}
    ca_file = os.environ.get("SSL_CERT_FILE")
    context = ssl.create_default_context(cafile=ca_file or None)
    try:
        return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30, context=context)
    except urllib.error.HTTPError as error:
        if error.code in (403, 429):
            raise UpdateError("GitHub rate limit or access denied. Try checking again later.") from error
        raise UpdateError(f"GitHub request failed (HTTP {error.code}).") from error
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise UpdateError(f"Could not reach GitHub: {one_line(error)}") from error


def read_url(url, limit=MAX_JSON):
    with request(url) as response:
        data = response.read(limit + 1)
    if len(data) > limit:
        raise UpdateError("GitHub response exceeds the supported size.")
    return data


def fetch_json(url):
    try:
        return json.loads(read_url(url))
    except (ValueError, UnicodeError) as error:
        raise UpdateError("GitHub returned invalid release data.") from error


def valid_release(value):
    return (isinstance(value, dict) and isinstance(value.get("tag_name"), str)
            and bool(value["tag_name"]) and len(value["tag_name"]) <= 160
            and not any(ord(c) < 32 or ord(c) == 127 for c in value["tag_name"])
            and isinstance(value.get("assets"), list))


def all_releases():
    releases, seen = [], set()
    for page in range(1, 101):
        data = fetch_json(f"{API}?per_page=100&page={page}")
        if not isinstance(data, list) or not all(valid_release(item) for item in data):
            raise UpdateError("GitHub returned an incomplete or invalid release list.")
        for item in data:
            if not item.get("draft") and item["tag_name"] not in seen:
                releases.append(item)
                seen.add(item["tag_name"])
        if len(data) < 100:
            return releases
    raise UpdateError("Too many releases to load completely; no partial list was applied.")


def release_by_tag(tag):
    data = fetch_json(f"{API}/tags/{urllib.parse.quote(tag, safe='')}")
    if not valid_release(data) or data["tag_name"] != tag or data.get("draft"):
        raise UpdateError("The selected release is no longer available. Check for updates again.")
    return data


def channel(release):
    if "nightly" in release["tag_name"].lower():
        return "nightly"
    return "prerelease" if release.get("prerelease") else "stable"


class Runtime:
    def __init__(self):
        user_home = Path(os.environ.get("HOME", "/tmp"))
        self.home = Path(os.environ.get("PICOCLAW_HOME", str(user_home / ".local/share/lilygo/ui/picoclaw"))).absolute()
        config_dir = Path(os.environ.get("PICOCLAW_CONFIG_DIR", str(user_home / ".config/lilygo/ui/picoclaw")))
        self.ui_settings = Path(os.environ.get("PICOCLAW_UI_SETTINGS", str(config_dir / "ui-settings.conf"))).absolute()
        self.config = Path(os.environ.get("PICOCLAW_CONFIG", str(config_dir / "config.json"))).absolute()
        self.state = Path(os.environ.get("PICOCLAW_STATE_DIR", str(user_home / ".local/state/lilygo/ui/picoclaw"))).absolute()
        self.control = Path(os.environ.get("PICOCLAW_UPDATE_CONTROL", str(Path(__file__).with_name("picoclaw-control")))).absolute()
        self.binary = Path(os.environ.get("PICOCLAW_UPDATE_BIN", str(self.control.with_name("picoclaw")))).absolute()
        self.system = os.environ.get("PICOCLAW_UPDATE_PLATFORM", platform.system())
        self.arch = os.environ.get("PICOCLAW_UPDATE_ARCH", platform.machine())
        self.explicit = os.environ.get("PICOCLAW_UPDATE_EXPLICIT_BIN") == "yes"
        self.active = self.home / "bin/active-picoclaw"

    def asset_name(self):
        arch = {"aarch64": "arm64", "arm64": "arm64", "x86_64": "x86_64", "amd64": "x86_64",
                "armv7l": "armv7", "armv7": "armv7"}.get(self.arch)
        return f"picoclaw_Linux_{arch}.tar.gz" if self.system == "Linux" and arch else None

    def asset(self, release):
        name = self.asset_name()
        return next((a for a in release["assets"] if isinstance(a, dict) and a.get("name") == name), None) if name else None

    def version(self, binary=None):
        try:
            result = subprocess.run([str(binary or self.binary), "--no-color", "version"],
                                    env=os.environ.copy(), capture_output=True, text=True, timeout=15)
        except (OSError, subprocess.TimeoutExpired) as error:
            raise UpdateError("PicoClaw cannot run on this board.") from error
        match = re.search(r"\bpicoclaw\s+(\S+)", result.stdout, re.IGNORECASE)
        if result.returncode or not match:
            raise UpdateError("This release does not support the required PicoClaw CLI on this board.")
        return match.group(1)

    def control_command(self, command):
        env = dict(os.environ, PICOCLAW_UPDATE_INTERNAL="1")
        result = subprocess.run(["/bin/sh", str(self.control), command], env=env,
                                capture_output=True, text=True, timeout=45)
        if result.returncode:
            raise UpdateError(f"{command} failed: {one_line(result.stdout or result.stderr, 500)}")
        return result.stdout

    def gateway_running(self):
        running = "gateway=running" in self.control_command("status").splitlines()
        if running and Path("/proc").is_dir():
            try:
                pid = int((self.state / "gateway.pid").read_text().strip())
                executable = Path(os.readlink(f"/proc/{pid}/exe"))
                if executable != self.binary.resolve():
                    raise UpdateError("Gateway PID belongs to a different executable. Check its status before updating.")
            except (OSError, ValueError) as error:
                raise UpdateError("Could not verify the running Gateway process. Check its status before updating.") from error
        return running

    def healthy(self):
        try:
            config = json.loads(self.config.read_text())
            port = int(config.get("gateway", {}).get("port", 18790))
            # Match helper argv: --host 0.0.0.0 followed by optional UI args.
            # Cobra uses the last --host occurrence, overriding config.host.
            host = "0.0.0.0"
            arguments = ""
            if self.ui_settings.exists():
                for line in self.ui_settings.read_text().splitlines():
                    if line.startswith("arguments="):
                        arguments = line[len("arguments="):]
                        break
            values = arguments.split()
            for index, value in enumerate(values):
                if value.startswith("--host="):
                    host = value[len("--host="):]
                elif value == "--host" and index + 1 < len(values):
                    host = values[index + 1]
            if host in ("0.0.0.0", ""):
                host = "127.0.0.1"
            elif host == "::":
                host = "::1"
            if ":" in host:
                host = f"[{host}]"
        except (OSError, ValueError, TypeError):
            return False
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        for _ in range(20):
            try:
                with opener.open(f"http://{host}:{port}/health", timeout=1) as response:
                    data = json.loads(response.read(65536))
                    if data.get("status") == "ok":
                        return True
            except (OSError, ValueError, urllib.error.URLError):
                pass
            time.sleep(0.5)
        return False


def digest_for(asset, release):
    digest = asset.get("digest", "")
    if isinstance(digest, str) and re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest):
        return digest[7:].lower()
    checksum_assets = [a for a in release["assets"] if isinstance(a, dict)
                       and re.search(r"(checksums.*\.txt|sha256sums(\.txt)?)$", a.get("name", ""), re.I)]
    for checksums in checksum_assets:
        lines = read_url(checksums["browser_download_url"], 1024 * 1024).decode("utf-8").splitlines()
        matches = []
        for line in lines:
            match = re.fullmatch(r"([0-9a-fA-F]{64})\s+\*?(.+)", line.strip())
            if match and match.group(2) == asset["name"]:
                matches.append(match.group(1).lower())
        if len(matches) == 1:
            return matches[0]
    raise UpdateError("This release has no verifiable SHA-256 for the board archive.")


def download(asset, destination, expected):
    digest, total = hashlib.sha256(), 0
    started = time.monotonic()
    with request(asset["browser_download_url"]) as response, destination.open("wb") as output:
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if time.monotonic() - started > 300:
                raise UpdateError("Runtime download timed out. Try again.")
            if total > MAX_ARCHIVE:
                raise UpdateError("The runtime archive exceeds the supported size.")
            output.write(chunk)
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise UpdateError("Runtime checksum mismatch. The release may have changed; check again.")


def extract_binary(archive, destination, arch):
    with tarfile.open(archive, "r:gz") as source:
        members = [m for m in source.getmembers() if m.name.removeprefix("./") == "picoclaw"]
        if len(members) != 1 or not members[0].isfile() or members[0].size > MAX_ARCHIVE:
            raise UpdateError("Archive does not contain one regular PicoClaw executable.")
        with source.extractfile(members[0]) as data, destination.open("wb") as output:
            shutil.copyfileobj(data, output)
    with destination.open("rb") as binary:
        header = binary.read(24)
    elf_class, machine = {"aarch64": (2, 183), "arm64": (2, 183), "x86_64": (2, 62), "amd64": (2, 62),
                          "armv7l": (1, 40), "armv7": (1, 40)}[arch]
    if (len(header) < 24 or header[:4] != b"\x7fELF" or header[4:7] != bytes((elf_class, 1, 1))
            or int.from_bytes(header[18:20], "little") != machine):
        raise UpdateError("The downloaded binary is not a compatible Linux executable.")
    destination.chmod(0o755)


def atomic_link(path, target):
    temporary = path.with_name(path.name + f".tmp.{os.getpid()}")
    try:
        temporary.symlink_to(target)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_restore(path, data, mode):
    descriptor, name = tempfile.mkstemp(prefix=f".{path.name}.restore-", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
            output.flush()
            os.fchmod(output.fileno(), mode)
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def installed_digest(runtime):
    try:
        metadata = runtime.binary.resolve().with_name("release.json")
        digest = json.loads(metadata.read_text())["sha256"]
        if isinstance(digest, str) and re.fullmatch(r"[0-9a-f]{64}", digest):
            return digest
    except (OSError, ValueError, KeyError):
        pass
    # Standard deb layout; also works with a package extracted into a staging tree.
    provenance = runtime.control.parent.parent.parent / "share/doc/lilygo-ui-picoclaw/picoclaw/runtime-source.txt"
    if runtime.binary.resolve() == runtime.control.with_name("picoclaw").resolve():
        try:
            match = re.search(r"^Archive SHA-256: ([0-9a-f]{64})$", provenance.read_text(), re.M)
            if match:
                return match.group(1)
        except OSError:
            pass
    return None


def check_updates(runtime):
    releases = all_releases()
    latest = fetch_json(f"{API}/latest")
    if not valid_release(latest):
        raise UpdateError("GitHub returned invalid latest-release data.")
    try:
        current = runtime.version()
    except UpdateError:
        current = ""
    status, message = "unknown", "Versions loaded. Select a release to install."
    if not runtime.asset_name():
        status, message = "unsupported", "Release installation is available on native Linux."
    elif runtime.explicit:
        status, message = "custom", "Clear the custom binary path to use managed release updates."
    elif not current:
        status, message = "not-installed", "PicoClaw is not installed. Select a release to install."
    elif "nightly" in current.lower():
        nightly = next((r for r in releases if r["tag_name"] == "nightly"), None)
        asset = runtime.asset(nightly) if nightly else None
        known = installed_digest(runtime)
        digest = asset.get("digest") if asset else None
        expected = digest[7:].lower() if isinstance(digest, str) and re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest) else ""
        if known and expected:
            status = "current" if known == expected else "available"
            message = "Nightly is up to date." if status == "current" else "A newer nightly build is available."
        else:
            message = "Nightly versions loaded. Select nightly to fetch its current build."
    else:
        current_match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", current)
        latest_match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", latest["tag_name"])
        if current_match and latest_match:
            newer = tuple(map(int, latest_match.groups())) > tuple(map(int, current_match.groups()))
            status = "available" if newer else "current"
            message = f"Update available: {latest['tag_name']}." if newer else "The stable runtime is up to date."
    message += f" Latest stable: {latest['tag_name']}."
    lines = ["updates=ready", f"latest_stable={latest['tag_name']}", f"update_status={status}",
             f"update_message={one_line(message)}"]
    for release in releases:
        # Legacy bare binaries use a different CLI/configuration schema. Keep
        # every release visible while marking missing compatible archives.
        supported = runtime.asset(release) is not None and not runtime.explicit
        fields = [release["tag_name"], one_line(release.get("name") or release["tag_name"]),
                  channel(release), one_line(release.get("published_at", ""), 40), "yes" if supported else "no"]
        lines.append("release=" + "\t".join(fields))
    lines.append("end_releases=yes")
    result = "\n".join(lines) + "\n"
    if len(result.encode()) > 1024 * 1024:
        raise UpdateError("The complete release list is too large to display.")
    print(result, end="")


@contextlib.contextmanager
def update_lock(runtime):
    runtime.home.mkdir(parents=True, exist_ok=True)
    with (runtime.home / ".update.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise UpdateError("Another runtime operation is in progress. Try again when it finishes.") from error
        yield


def install_release(runtime, tag):
    if not runtime.asset_name():
        raise UpdateError("Version installation is supported only for the native Linux runtime.")
    if runtime.explicit:
        raise UpdateError("Clear PICOCLAW_BIN and the Settings binary path before installing a managed release.")
    if not tag or len(tag) > 160 or any(ord(c) < 32 for c in tag):
        raise UpdateError("Select a valid release first.")
    with update_lock(runtime):
        release = release_by_tag(tag)
        asset = runtime.asset(release)
        if not asset:
            raise UpdateError("This release has no compatible runtime archive for this board.")
        expected = digest_for(asset, release)
        runtime.active.parent.mkdir(parents=True, exist_ok=True)
        versions = runtime.home / "versions"
        versions.mkdir(parents=True, exist_ok=True)
        candidate = Path(tempfile.mkdtemp(prefix="release-", dir=versions))
        previous = os.readlink(runtime.active) if runtime.active.is_symlink() else None
        if runtime.active.exists() and previous is None:
            shutil.rmtree(candidate)
            raise UpdateError("The managed runtime selection is not a symlink; it was left unchanged.")
        switched, stopped, running, keep_candidate = False, False, False, False
        backups = {}
        backup_dir = candidate / "config-backup"
        try:
            archive = candidate / "release.tar.gz"
            download(asset, archive, expected)
            binary = candidate / "picoclaw"
            extract_binary(archive, binary, runtime.arch)
            version = runtime.version(binary)
            archive.unlink()
            (candidate / "release.json").write_text(json.dumps({"tag": tag, "asset_id": asset.get("id"),
                                                               "sha256": expected, "version": version}) + "\n")
            running = runtime.gateway_running()
            if running:
                # Roll back even if stop times out after terminating the process.
                stopped = True
                runtime.control_command("gateway-stop")
            backup_dir.mkdir(mode=0o700)
            for path in (runtime.config, runtime.config.with_name(".security.yml")):
                backups[path] = (path.read_bytes(), path.stat().st_mode & 0o777) if path.exists() else None
                if backups[path] is not None:
                    atomic_restore(backup_dir / path.name, backups[path][0], 0o600)
            (backup_dir / "manifest.json").write_text(json.dumps({str(path): saved[1] if saved else None
                                                               for path, saved in backups.items()}) + "\n")
            switched = True
            atomic_link(runtime.active, str(binary))
            if running:
                runtime.control_command("gateway-start")
                if not runtime.healthy():
                    raise UpdateError("The updated Gateway did not pass its health check.")
            keep_candidate = True
            shutil.rmtree(backup_dir)
            print(f"OK: Installed {one_line(tag)} (picoclaw {one_line(version)})." +
                  (" Gateway restarted." if running else " Gateway remains stopped."))
        except BaseException as error:
            rollback_errors = []
            if switched or stopped:
                # Application shutdown may signal the helper group. Complete
                # restoration even when a second termination signal arrives.
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                if switched:
                    try:
                        runtime.control_command("gateway-stop")
                    except Exception as failure:
                        rollback_errors.append(str(failure))
                    try:
                        if previous is None:
                            runtime.active.unlink(missing_ok=True)
                        else:
                            atomic_link(runtime.active, previous)
                    except OSError as failure:
                        keep_candidate = True
                        raise UpdateError(f"Update failed and could not restore the previous selection: {one_line(failure)}. "
                                          f"Configuration backups: {backup_dir}") from error
                    for path, saved in backups.items():
                        try:
                            if saved is None:
                                path.unlink(missing_ok=True)
                            else:
                                atomic_restore(path, saved[0], saved[1])
                        except OSError as failure:
                            rollback_errors.append(f"Could not restore {path.name}: {one_line(failure)}")
                if running:
                    try:
                        runtime.control_command("gateway-start")
                        if not runtime.healthy():
                            raise UpdateError("The previous Gateway did not pass its health check.")
                    except Exception as failure:
                        rollback_errors.append(str(failure))
                if rollback_errors:
                    keep_candidate = True
                    raise UpdateError(f"Update failed: {one_line(error)} Previous runtime selected, but recovery needs attention: "
                                      + one_line("; ".join(rollback_errors), 400)
                                      + f". Configuration backups: {backup_dir}") from error
                raise UpdateError(f"Update failed: {one_line(error)} Previous runtime restored." +
                                  (" Gateway restarted." if running else "")) from error
            raise
        finally:
            # A filesystem failure during rollback must never leave the
            # active symlink pointing at a directory we then delete.
            if not keep_candidate and runtime.active.resolve() != candidate / "picoclaw":
                shutil.rmtree(candidate, ignore_errors=True)


def main():
    def cancel(_signum, _frame):
        raise UpdateError("Update cancelled.")
    signal.signal(signal.SIGTERM, cancel)
    signal.signal(signal.SIGINT, cancel)
    os.umask(0o077)
    try:
        runtime = Runtime()
        if len(sys.argv) == 2 and sys.argv[1] == "check-updates":
            check_updates(runtime)
        elif len(sys.argv) == 3 and sys.argv[1] == "install-release":
            install_release(runtime, sys.argv[2])
        else:
            raise UpdateError("Usage: picoclaw-releases.py {check-updates|install-release TAG}")
        return 0
    except (UpdateError, OSError, ValueError, KeyError, tarfile.TarError, subprocess.SubprocessError) as error:
        print(f"ERROR: {one_line(error, 900)}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
