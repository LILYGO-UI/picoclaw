#!/usr/bin/env python3
"""Release discovery and transaction tests without network or runtime processes."""

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import tarfile
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("picoclaw_releases", ROOT / "scripts/picoclaw-releases.py")
releases = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(releases)


def release(tag="v0.3.1", assets=None, **values):
    return {"tag_name": tag, "name": tag, "assets": assets or [], "draft": False,
            "prerelease": False, "published_at": "2026-07-03T07:37:06Z", **values}


def elf(machine=183):
    header = bytearray(64)
    header[:7] = b"\x7fELF\x02\x01\x01"
    header[16:18] = (2).to_bytes(2, "little")
    header[18:20] = machine.to_bytes(2, "little")
    header[20:24] = (1).to_bytes(4, "little")
    return bytes(header)


def archive_bytes(binary=None, link_type=None):
    output = io.BytesIO()
    with tarfile.open(fileobj=output, mode="w:gz") as archive:
        member = tarfile.TarInfo("picoclaw")
        if link_type is not None:
            member.type = link_type
            member.linkname = "elsewhere"
            archive.addfile(member)
        else:
            data = elf() if binary is None else binary
            member.size = len(data)
            archive.addfile(member, io.BytesIO(data))
    return output.getvalue()


def asset(data=b"archive", digest=True):
    value = {"id": 123, "name": "picoclaw_Linux_arm64.tar.gz",
             "browser_download_url": "https://github.com/sipeed/picoclaw/releases/download/v0.3.1/picoclaw_Linux_arm64.tar.gz"}
    if digest:
        value["digest"] = "sha256:" + hashlib.sha256(data).hexdigest()
    return value


class RuntimeTestCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        environment = {
            "HOME": str(self.root),
            "PICOCLAW_HOME": str(self.root / "runtime"),
            "PICOCLAW_CONFIG_DIR": str(self.root / "config"),
            "PICOCLAW_STATE_DIR": str(self.root / "state"),
            "PICOCLAW_UPDATE_CONTROL": str(self.root / "package/picoclaw-control"),
            "PICOCLAW_UPDATE_PLATFORM": "Linux",
            "PICOCLAW_UPDATE_ARCH": "aarch64",
        }
        patch = mock.patch.dict(os.environ, environment, clear=True)
        patch.start()
        self.addCleanup(patch.stop)
        self.runtime = releases.Runtime()

    def capture_check(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            releases.check_updates(self.runtime)
        return output.getvalue()


class DiscoveryTests(RuntimeTestCase):
    def test_fetches_all_pages_before_reporting_complete(self):
        first = [release(f"v0.1.{index}") for index in range(100)]
        second = [release("nightly", prerelease=True), first[0], release("draft", draft=True)]
        with mock.patch.object(releases, "fetch_json", side_effect=[first, second]) as fetch:
            result = releases.all_releases()
        self.assertEqual(len(result), 101)
        self.assertEqual(result[-1]["tag_name"], "nightly")
        self.assertNotIn("draft", [item["tag_name"] for item in result])
        self.assertEqual(fetch.call_args_list, [
            mock.call(f"{releases.API}?per_page=100&page=1"),
            mock.call(f"{releases.API}?per_page=100&page=2"),
        ])

    def test_full_final_page_requires_empty_followup_page(self):
        page = [release(f"v0.1.{index}") for index in range(100)]
        with mock.patch.object(releases, "fetch_json", side_effect=[page, []]) as fetch:
            self.assertEqual(len(releases.all_releases()), 100)
        self.assertEqual(fetch.call_count, 2)

    def test_invalid_or_failed_later_page_does_not_publish_partial_list(self):
        first = [release(f"v0.1.{index}") for index in range(100)]
        for invalid in ({"message": "API error"}, [{"tag_name": "incomplete"}],
                        [release("bad\ntag")], releases.UpdateError("network interrupted")):
            with self.subTest(invalid=invalid), \
                    mock.patch.object(releases, "fetch_json", side_effect=[first, invalid]), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                with self.assertRaises(releases.UpdateError):
                    releases.check_updates(self.runtime)
                self.assertEqual(output.getvalue(), "")

    def test_invalid_and_truncated_json_have_actionable_errors(self):
        for data in (b'{"incomplete":', b"not JSON", b"\xff"):
            with self.subTest(data=data), mock.patch.object(releases, "read_url", return_value=data):
                with self.assertRaisesRegex(releases.UpdateError, "invalid release data"):
                    releases.fetch_json(releases.API)

    def test_latest_stable_is_not_the_first_release(self):
        listed = [release("nightly", [asset()], prerelease=True), release("v0.3.1", [asset()]),
                  release("v0.2.2-nightly.20260312.6612ca09", [asset()]), release("v0.0.1")]
        with mock.patch.object(releases, "fetch_json", side_effect=[listed, listed[1]]) as fetch, \
                mock.patch.object(self.runtime, "version", return_value="v0.2.9"):
            output = self.capture_check()
        self.assertIn("latest_stable=v0.3.1\n", output)
        self.assertIn("update_status=available\n", output)
        rows = [line.split("=", 1)[1].split("\t") for line in output.splitlines() if line.startswith("release=")]
        self.assertEqual(len(rows), 4)
        self.assertEqual(rows[0][2], "nightly")
        self.assertEqual(rows[2][2], "nightly")
        self.assertEqual(rows[3][-1], "no")
        self.assertTrue(output.endswith("end_releases=yes\n"))
        self.assertEqual(fetch.call_args_list[-1], mock.call(f"{releases.API}/latest"))

    def test_same_nightly_tag_compares_archive_digest(self):
        installed = self.root / "installed"
        installed.mkdir()
        self.runtime.binary = installed / "picoclaw"
        self.runtime.binary.write_bytes(elf())
        installed_digest = hashlib.sha256(b"current build").hexdigest()
        (installed / "release.json").write_text(json.dumps({"sha256": installed_digest, "tag": "nightly"}))
        for remote_digest, expected in ((installed_digest, "current"),
                                        (installed_digest.upper(), "current"), ("a" * 64, "available")):
            nightly_asset = asset()
            nightly_asset["digest"] = "sha256:" + remote_digest
            nightly = release("nightly", [nightly_asset], prerelease=True)
            with self.subTest(expected=expected), \
                    mock.patch.object(releases, "fetch_json", side_effect=[[nightly], release()]), \
                    mock.patch.object(self.runtime, "version", return_value="v0.3.1-nightly.20260702.hash"):
                self.assertIn(f"update_status={expected}\n", self.capture_check())

    def test_missing_or_malformed_nightly_digest_keeps_complete_release_list(self):
        for digest in (None, "", "sha256:short", "sha512:" + "a" * 64, 42):
            nightly_asset = asset()
            nightly_asset["digest"] = digest
            nightly = release("nightly", [nightly_asset], prerelease=True)
            with self.subTest(digest=digest), \
                    mock.patch.object(releases, "fetch_json", side_effect=[[nightly], release()]), \
                    mock.patch.object(releases, "installed_digest", return_value="a" * 64), \
                    mock.patch.object(self.runtime, "version", return_value="v0.3.1-nightly.20260702.hash"):
                output = self.capture_check()
                self.assertIn("update_status=unknown\n", output)
                self.assertIn("release=nightly\t", output)
                self.assertTrue(output.endswith("end_releases=yes\n"))

    def test_invalid_latest_release_does_not_publish_complete_result(self):
        with mock.patch.object(releases, "fetch_json", side_effect=[[release()], {"tag_name": "v0.3.1"}]), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            with self.assertRaisesRegex(releases.UpdateError, "invalid latest-release"):
                releases.check_updates(self.runtime)
            self.assertEqual(output.getvalue(), "")


class DigestTests(unittest.TestCase):
    def test_asset_digest_is_normalized_without_another_request(self):
        entry = asset()
        entry["digest"] = "sha256:" + "AB" * 32
        with mock.patch.object(releases, "read_url") as read:
            self.assertEqual(releases.digest_for(entry, release(assets=[entry])), "ab" * 32)
        read.assert_not_called()

    def test_falls_back_to_same_release_checksum_and_exact_filename(self):
        target = asset(digest=False)
        checksums = {"name": "picoclaw_0.3.1_checksums.txt", "browser_download_url": "https://example.com/checksums"}
        text = f'{"a" * 64}  other.tar.gz\n{"B" * 64} *{target["name"]}\n'
        with mock.patch.object(releases, "read_url", return_value=text.encode()) as read:
            self.assertEqual(releases.digest_for(target, release(assets=[target, checksums])), "b" * 64)
        read.assert_called_once_with(checksums["browser_download_url"], 1024 * 1024)

    def test_missing_mismatched_and_ambiguous_checksums_are_rejected(self):
        target = asset(digest=False)
        checksums = {"name": "sha256sums.txt", "browser_download_url": "https://example.com/checksums"}
        valid = f'{"a" * 64}  {target["name"]}\n'
        for text in ("", f'{"a" * 64}  prefix-{target["name"]}\n', valid + valid):
            with self.subTest(text=text), mock.patch.object(releases, "read_url", return_value=text.encode()):
                with self.assertRaisesRegex(releases.UpdateError, "no verifiable SHA-256"):
                    releases.digest_for(target, release(assets=[target, checksums]))
        with self.assertRaisesRegex(releases.UpdateError, "no verifiable SHA-256"):
            releases.digest_for(target, release(assets=[target]))


class HealthTests(RuntimeTestCase):
    def test_probes_effective_gateway_host_instead_of_config_host(self):
        self.runtime.config.parent.mkdir(parents=True)
        self.runtime.config.write_text(json.dumps({"gateway": {"host": "::1", "port": 26000}}))
        cases = [
            ("", "http://127.0.0.1:26000/health"),
            ("--debug --host 127.0.0.2", "http://127.0.0.2:26000/health"),
            ("--host=127.0.0.3", "http://127.0.0.3:26000/health"),
            ("--host 127.0.0.2 --host=::", "http://[::1]:26000/health"),
            ("--host=:: --host ::1", "http://[::1]:26000/health"),
        ]
        for arguments, expected_url in cases:
            self.runtime.ui_settings.write_text(f"arguments={arguments}\n")
            opener = mock.Mock()
            opener.open.return_value = io.BytesIO(b'{"status":"ok"}')
            with self.subTest(arguments=arguments), \
                    mock.patch.object(releases.urllib.request, "build_opener", return_value=opener) as build, \
                    mock.patch.object(releases.time, "sleep") as sleep:
                self.assertTrue(self.runtime.healthy())
                opener.open.assert_called_once_with(expected_url, timeout=1)
                self.assertEqual(build.call_args.args[0].proxies, {})
                sleep.assert_not_called()


class InstallTests(RuntimeTestCase):
    def setUp(self):
        super().setUp()
        self.previous = self.runtime.home / "versions/previous/picoclaw"
        self.previous.parent.mkdir(parents=True)
        self.previous.write_bytes(elf())
        self.runtime.binary = self.previous
        self.runtime.state.mkdir(parents=True)
        (self.runtime.state / "gateway.pid").write_text("4242\n")
        real_readlink = os.readlink

        def readlink(path, *args, **kwargs):
            if os.fspath(path) == "/proc/4242/exe":
                return str(self.previous)
            return real_readlink(path, *args, **kwargs)

        patch = mock.patch.object(releases.os, "readlink", side_effect=readlink)
        patch.start()
        self.addCleanup(patch.stop)
        self.runtime.active.parent.mkdir(parents=True)
        self.runtime.active.symlink_to(self.previous)
        self.runtime.config.parent.mkdir(parents=True)
        self.original_config = b'{"version":3,"gateway":{"port":18790},"custom":"preserve"}\n'
        self.runtime.config.write_bytes(self.original_config)
        self.runtime.config.chmod(0o640)
        self.security = self.runtime.config.with_name(".security.yml")
        self.original_security = b"model_list:\n  old:0:\n    api_keys: [test-only-key]\n"
        self.security.write_bytes(self.original_security)
        self.security.chmod(0o600)
        # Transaction rollback intentionally ignores termination signals in the
        # helper process. Do not alter the test runner's own signal disposition.
        patch = mock.patch.object(releases.signal, "signal")
        patch.start()
        self.addCleanup(patch.stop)

    @contextlib.contextmanager
    def transport(self, data=None, expected=None):
        data = archive_bytes() if data is None else data
        entry = asset(data)
        if expected is not None:
            entry["digest"] = "sha256:" + expected
        with mock.patch.object(releases, "fetch_json", return_value=release(assets=[entry])) as fetch, \
                mock.patch.object(releases, "request", side_effect=lambda _url: io.BytesIO(data)) as request:
            yield fetch, request

    def assert_original_restored(self):
        self.assertTrue(self.runtime.active.is_symlink())
        self.assertEqual(self.runtime.active.resolve(), self.previous)
        self.assertEqual(self.runtime.config.read_bytes(), self.original_config)
        self.assertEqual(self.security.read_bytes(), self.original_security)
        self.assertEqual(stat.S_IMODE(self.runtime.config.stat().st_mode), 0o640)
        self.assertEqual(stat.S_IMODE(self.security.stat().st_mode), 0o600)
        self.assertEqual(list((self.runtime.home / "versions").iterdir()), [self.previous.parent])

    def test_invalid_download_does_not_stop_gateway_or_replace_selection(self):
        cases = [(archive_bytes(), "0" * 64, "checksum mismatch"),
                 (archive_bytes(elf(62)), None, "compatible Linux executable"),
                 (archive_bytes(link_type=tarfile.SYMTYPE), None, "regular PicoClaw executable"),
                 (archive_bytes(link_type=tarfile.LNKTYPE), None, "regular PicoClaw executable")]
        for data, expected, error in cases:
            with self.subTest(error=error), self.transport(data, expected), \
                    mock.patch.object(self.runtime, "version") as version, \
                    mock.patch.object(self.runtime, "control_command") as control:
                with self.assertRaisesRegex(releases.UpdateError, error):
                    releases.install_release(self.runtime, "v0.3.1")
                version.assert_not_called()
                control.assert_not_called()
                self.assert_original_restored()

    def test_unsupported_cli_does_not_stop_gateway(self):
        with self.transport(), \
                mock.patch.object(self.runtime, "version", side_effect=releases.UpdateError("CLI unsupported")), \
                mock.patch.object(self.runtime, "control_command") as control:
            with self.assertRaisesRegex(releases.UpdateError, "CLI unsupported"):
                releases.install_release(self.runtime, "v0.3.1")
            control.assert_not_called()
        self.assert_original_restored()

    def test_success_selects_verified_binary_atomically_and_restarts_running_gateway(self):
        events = []

        def version(binary):
            self.assertEqual(binary.read_bytes(), elf())
            self.assertEqual(self.runtime.active.resolve(), self.previous)
            events.append("version")
            return "v0.3.1"

        def control(command):
            events.append(command)
            if command in ("status", "gateway-stop"):
                self.assertEqual(self.runtime.active.resolve(), self.previous)
            if command == "gateway-start":
                self.assertNotEqual(self.runtime.active.resolve(), self.previous)
                self.assertEqual(self.runtime.active.read_bytes(), elf())
            return "gateway=running\n" if command == "status" else ""

        def healthy():
            events.append("healthy")
            return True

        with self.transport() as (fetch, request), \
                mock.patch.object(self.runtime, "version", side_effect=version), \
                mock.patch.object(self.runtime, "control_command", side_effect=control), \
                mock.patch.object(self.runtime, "healthy", side_effect=healthy), \
                mock.patch.object(releases.os, "replace", wraps=os.replace) as replace, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            releases.install_release(self.runtime, "v0.3.1")
        self.assertEqual(events, ["version", "status", "gateway-stop", "gateway-start", "healthy"])
        selection_changes = [call for call in replace.call_args_list if call.args[1] == self.runtime.active]
        self.assertEqual(len(selection_changes), 1)
        fetch.assert_called_once_with(f"{releases.API}/tags/v0.3.1")
        request.assert_called_once()
        self.assertIn("Gateway restarted", output.getvalue())
        selected = self.runtime.active.resolve()
        self.assertEqual(stat.S_IMODE(selected.stat().st_mode), 0o755)
        metadata = json.loads(selected.with_name("release.json").read_text())
        self.assertEqual((metadata["tag"], metadata["asset_id"], metadata["version"]), ("v0.3.1", 123, "v0.3.1"))
        self.assertFalse(selected.with_name("release.tar.gz").exists())
        self.assertFalse(selected.with_name("config-backup").exists())
        self.assertEqual(self.runtime.config.read_bytes(), self.original_config)
        self.assertEqual(self.security.read_bytes(), self.original_security)

    def test_success_leaves_stopped_gateway_stopped(self):
        with self.transport(), mock.patch.object(self.runtime, "version", return_value="v0.3.1"), \
                mock.patch.object(self.runtime, "control_command", return_value="gateway=stopped\n") as control, \
                mock.patch.object(self.runtime, "healthy") as healthy, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            releases.install_release(self.runtime, "v0.3.1")
        control.assert_called_once_with("status")
        healthy.assert_not_called()
        self.assertNotEqual(self.runtime.active.resolve(), self.previous)
        self.assertIn("Gateway remains stopped", output.getvalue())

    def test_failed_health_restores_selection_config_secrets_and_running_gateway(self):
        events = []
        starts = 0

        def control(command):
            nonlocal starts
            events.append(command)
            if command == "gateway-start":
                starts += 1
                if starts == 1:
                    self.assertNotEqual(self.runtime.active.resolve(), self.previous)
                    self.runtime.config.write_text('{"migrated":"new runtime"}')
                    self.runtime.config.chmod(0o600)
                    self.security.write_text("migrated secrets")
                    self.security.chmod(0o644)
                else:
                    self.assertEqual(self.runtime.active.resolve(), self.previous)
                    self.assertEqual(self.runtime.config.read_bytes(), self.original_config)
                    self.assertEqual(self.security.read_bytes(), self.original_security)
            return "gateway=running\n" if command == "status" else ""

        def healthy():
            events.append("healthy")
            return starts == 2

        with self.transport(), mock.patch.object(self.runtime, "version", return_value="v0.3.1"), \
                mock.patch.object(self.runtime, "control_command", side_effect=control), \
                mock.patch.object(self.runtime, "healthy", side_effect=healthy):
            with self.assertRaisesRegex(releases.UpdateError, "health check.*Previous runtime restored.*Gateway restarted"):
                releases.install_release(self.runtime, "v0.3.1")
        self.assertEqual(events, ["status", "gateway-stop", "gateway-start", "healthy",
                                  "gateway-stop", "gateway-start", "healthy"])
        self.assert_original_restored()

    def test_failed_health_restores_bundled_fallback_and_removes_new_security_file(self):
        self.runtime.active.unlink()
        self.security.unlink()
        starts = 0

        def control(command):
            nonlocal starts
            if command == "gateway-start":
                starts += 1
                if starts == 1:
                    self.security.write_text("new credentials")
                else:
                    self.assertFalse(self.runtime.active.is_symlink())
                    self.assertFalse(self.security.exists())
            return "gateway=running\n" if command == "status" else ""

        with self.transport(), mock.patch.object(self.runtime, "version", return_value="v0.3.1"), \
                mock.patch.object(self.runtime, "control_command", side_effect=control), \
                mock.patch.object(self.runtime, "healthy", side_effect=[False, True]):
            with self.assertRaisesRegex(releases.UpdateError, "Previous runtime restored"):
                releases.install_release(self.runtime, "v0.3.1")
        self.assertEqual(starts, 2)
        self.assertFalse(self.runtime.active.is_symlink())
        self.assertFalse(self.security.exists())
        self.assertEqual(self.runtime.config.read_bytes(), self.original_config)

    def test_restore_failure_keeps_private_backups_restores_other_file_and_restarts(self):
        events = []
        starts = 0
        candidate_config = b'{"migrated":"new runtime"}'
        restore = releases.atomic_restore

        def control(command):
            nonlocal starts
            events.append(command)
            if command == "gateway-start":
                starts += 1
                if starts == 1:
                    self.runtime.config.write_bytes(candidate_config)
                    self.security.write_text("migrated credentials")
                else:
                    self.assertEqual(self.runtime.active.resolve(), self.previous)
                    self.assertEqual(self.security.read_bytes(), self.original_security)
            return "gateway=running\n" if command == "status" else ""

        def fail_config_restore(path, data, mode):
            if path == self.runtime.config:
                events.append("config-restore-failed")
                raise OSError("simulated full disk")
            if path == self.security:
                events.append("security-restored")
            return restore(path, data, mode)

        with self.transport(), mock.patch.object(self.runtime, "version", return_value="v0.3.1"), \
                mock.patch.object(self.runtime, "control_command", side_effect=control), \
                mock.patch.object(self.runtime, "healthy", side_effect=[False, True]) as healthy, \
                mock.patch.object(releases, "atomic_restore", side_effect=fail_config_restore):
            with self.assertRaisesRegex(releases.UpdateError, "recovery needs attention.*Configuration backups:") as caught:
                releases.install_release(self.runtime, "v0.3.1")
        self.assertEqual(starts, 2)
        self.assertEqual(healthy.call_count, 2)
        self.assertEqual(events, ["status", "gateway-stop", "gateway-start", "gateway-stop",
                                  "config-restore-failed", "security-restored", "gateway-start"])
        self.assertEqual(self.runtime.active.resolve(), self.previous)
        self.assertEqual(self.runtime.config.read_bytes(), candidate_config)
        self.assertEqual(self.security.read_bytes(), self.original_security)
        candidates = [path for path in (self.runtime.home / "versions").iterdir() if path != self.previous.parent]
        self.assertEqual(len(candidates), 1)
        backup = candidates[0] / "config-backup"
        self.assertIn(str(backup), str(caught.exception))
        self.assertEqual((backup / self.runtime.config.name).read_bytes(), self.original_config)
        self.assertEqual((backup / self.security.name).read_bytes(), self.original_security)
        self.assertEqual(stat.S_IMODE(backup.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((backup / self.runtime.config.name).stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE((backup / self.security.name).stat().st_mode), 0o600)
        self.assertEqual(json.loads((backup / "manifest.json").read_text()),
                         {str(self.runtime.config): 0o640, str(self.security): 0o600})

    def test_explicit_override_refuses_changes_before_any_network_or_gateway_call(self):
        self.runtime.explicit = True
        with self.transport() as (fetch, request), mock.patch.object(self.runtime, "control_command") as control:
            with self.assertRaisesRegex(releases.UpdateError, "Clear PICOCLAW_BIN"):
                releases.install_release(self.runtime, "v0.3.1")
            fetch.assert_not_called()
            request.assert_not_called()
            control.assert_not_called()
        self.assert_original_restored()

    def test_flock_rejects_concurrent_update_before_network_and_releases_afterward(self):
        with releases.update_lock(self.runtime), self.transport() as (fetch, request), \
                mock.patch.object(self.runtime, "control_command") as control:
            with self.assertRaisesRegex(releases.UpdateError, "Another runtime operation"):
                releases.install_release(self.runtime, "v0.3.1")
            fetch.assert_not_called()
            request.assert_not_called()
            control.assert_not_called()
        with releases.update_lock(self.runtime):
            self.assert_original_restored()


if __name__ == "__main__":
    unittest.main()
