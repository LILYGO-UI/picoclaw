#!/usr/bin/env python3
"""Black-box tests for scripts/picoclaw-control.sh.

The tests use a temporary XDG tree and small command shims.  No network access,
real PicoClaw installation, or user configuration is needed.
"""

import hashlib
import io
import json
import os
import fcntl
from pathlib import Path
import signal
import shutil
import shlex
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "scripts" / "picoclaw-control.sh"


class ControlHelperTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.helper = HELPER
        self.bin_dir = self.root / "bin"
        self.bin_dir.mkdir()
        self.system_path = os.environ.get("PATH", "/usr/bin:/bin")
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("PICOCLAW_")}
        self.env.update(
            PICOCLAW_HOME=str(self.root / "pico-home"),
            PICOCLAW_CONFIG_DIR=str(self.root / "config"),
            PICOCLAW_STATE_DIR=str(self.root / "state"),
            PICOCLAW_DOCKER="no",
            PATH=f"{self.bin_dir}:{self.system_path}",
        )
        # Route/hostname queries must not depend on the host's network.
        self.add_command("ip", "exit 0\n")
        self.add_command("hostname", "exit 0\n")
        # Model util-linux on a macOS test host using the same kernel flock
        # and session operations. No real gateway or container is required.
        self.add_python_command("flock", '''
import fcntl, subprocess, sys
assert sys.argv[1:5] == ["--close", "--nonblock", "--conflict-exit-code", "75"]
with open(sys.argv[5], "a") as lock:
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        sys.exit(75)
    sys.exit(subprocess.run(sys.argv[6:], close_fds=True).returncode)
''')
        self.add_python_command("setsid", '''
import os, sys
os.setsid()
os.execvp(sys.argv[1], sys.argv[1:])
''')
        self.managed = self.root / "pico-home" / "bin" / "picoclaw"
        self.capture = self.root / "download-url"
        self.env["CURL_CAPTURE"] = str(self.capture)
        self.env["TEST_ARCHIVE"] = str(self.root / "release.tar.gz")
        # Keep real extraction and hash verification; only replace transport.
        self.add_command("curl", '''
while [ "$#" -gt 0 ]; do
    case "$1" in
        -o) output="$2"; shift 2 ;;
        *) url="$1"; shift ;;
    esac
done
printf '%s\\n' "$url" > "$CURL_CAPTURE"
cp "$TEST_ARCHIVE" "$output"
''')

    def tearDown(self):
        if (self.root / "state" / "gateway.pid").exists():
            self.run_helper("gateway-stop")
        self.temp.cleanup()

    def add_command(self, name, body, executable=True):
        path = self.bin_dir / name
        path.write_text("#!/bin/sh\n" + body)
        if executable:
            path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def add_python_command(self, name, body):
        path = self.bin_dir / name
        path.write_text(f"#!{sys.executable}\n" + body)
        path.chmod(0o755)
        return path

    def run_helper(self, *args, check=False):
        return subprocess.run(
            ["/bin/sh", str(self.helper), *args],
            cwd=ROOT,
            env=self.env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=check,
            timeout=15,
        )

    def add_bundled_runtime(self, body='printf "picoclaw nightly\\n"\n'):
        package_dir = self.root / "package files"
        package_dir.mkdir(exist_ok=True)
        self.helper = package_dir / "picoclaw-control"
        self.helper.write_bytes(HELPER.read_bytes())
        binary = package_dir / "picoclaw"
        binary.write_text("#!/bin/sh\n" + body)
        binary.chmod(0o755)
        return binary

    def test_bundled_runtime_precedes_old_managed_and_path_binaries(self):
        self.add_bundled_runtime()
        self.managed.parent.mkdir(parents=True)
        self.managed.write_text('#!/bin/sh\nprintf "picoclaw old-managed\\n"\n')
        self.managed.chmod(0o755)
        self.add_command("picoclaw", 'printf "picoclaw path-runtime\\n"\n')
        result = self.run_helper("status", check=True)
        self.assertIn("version=picoclaw nightly", result.stdout)

    def test_selected_release_precedes_bundled_but_explicit_choices_win(self):
        self.add_bundled_runtime()
        self.managed.parent.mkdir(parents=True)
        release = self.add_command("release", 'printf "picoclaw selected-release\\n"\n')
        active = self.managed.parent / "active-picoclaw"
        active.symlink_to(release)
        self.assertIn("version=picoclaw selected-release", self.run_helper("status", check=True).stdout)
        config_dir = Path(self.env["PICOCLAW_CONFIG_DIR"])
        chosen = self.add_command("chosen", 'printf "picoclaw explicit-ui\\n"\n')
        (config_dir / "ui-settings.conf").write_text(f"binary_path={chosen}\n")
        self.assertIn("version=picoclaw explicit-ui", self.run_helper("status", check=True).stdout)
        self.env["PICOCLAW_BIN"] = str(release)
        self.assertIn("version=picoclaw selected-release", self.run_helper("status", check=True).stdout)
        self.env.pop("PICOCLAW_BIN")
        (config_dir / "ui-settings.conf").unlink()
        release.unlink()
        self.assertIn("version=picoclaw nightly", self.run_helper("status", check=True).stdout)

    def test_release_commands_forward_resolved_paths_and_literal_tag(self):
        self.mock_uname("Linux", "aarch64")
        bundled = self.add_bundled_runtime()
        updater = self.helper.parent / "picoclaw-releases.py"
        updater.write_text('''
import json, os, sys
names = ("PICOCLAW_HOME", "PICOCLAW_CONFIG", "PICOCLAW_CONFIG_DIR", "PICOCLAW_STATE_DIR",
         "PICOCLAW_UPDATE_BIN", "PICOCLAW_UPDATE_CONTROL", "PICOCLAW_UPDATE_PLATFORM",
         "PICOCLAW_UPDATE_ARCH", "PICOCLAW_UPDATE_EXPLICIT_BIN")
print(json.dumps({"arguments": sys.argv[1:], "environment": {name: os.environ[name] for name in names}}))
''')
        self.env["PICOCLAW_CONFIG"] = str(self.root / "custom config" / "board.json")
        # This is one argv value, including shell metacharacters. The shell
        # must pass it literally; release-tag validation belongs to Python.
        unexpected = self.root / "SHOULD_NOT_EXIST"
        tag = f"v1.2.3; touch {unexpected}"
        for command in (("check-updates",), ("install-release", tag)):
            result = json.loads(self.run_helper(*command, check=True).stdout)
            self.assertEqual(result["arguments"], list(command))
            received = result["environment"]
            for name in ("PICOCLAW_HOME", "PICOCLAW_CONFIG", "PICOCLAW_CONFIG_DIR", "PICOCLAW_STATE_DIR"):
                self.assertEqual(received[name], self.env[name])
            self.assertEqual(received["PICOCLAW_UPDATE_BIN"], str(bundled.resolve()))
            self.assertEqual(received["PICOCLAW_UPDATE_CONTROL"], str(self.helper.resolve()))
            self.assertEqual(received["PICOCLAW_UPDATE_PLATFORM"], "Linux")
            self.assertEqual(received["PICOCLAW_UPDATE_ARCH"], "aarch64")
            self.assertEqual(received["PICOCLAW_UPDATE_EXPLICIT_BIN"], "no")
        self.assertFalse(unexpected.exists())
        config_dir = Path(self.env["PICOCLAW_CONFIG_DIR"])
        config_dir.mkdir()
        (config_dir / "ui-settings.conf").write_text(f"binary_path={bundled}\n")
        result = json.loads(self.run_helper("check-updates", check=True).stdout)
        self.assertEqual(result["environment"]["PICOCLAW_UPDATE_EXPLICIT_BIN"], "yes")
        self.env["PICOCLAW_BIN"] = str(self.root / "explicit-missing-runtime")
        result = json.loads(self.run_helper("check-updates", check=True).stdout)
        self.assertEqual(result["environment"]["PICOCLAW_UPDATE_EXPLICIT_BIN"], "yes")
        self.assertEqual(result["environment"]["PICOCLAW_UPDATE_BIN"], self.env["PICOCLAW_BIN"])

    def test_release_commands_require_linux_and_report_python_failures(self):
        self.add_bundled_runtime()
        updater = self.helper.parent / "picoclaw-releases.py"
        updater.write_text('import sys\nprint("ERROR: release request failed")\nsys.exit(23)\n')
        self.mock_uname("Linux", "aarch64")
        self.assertIn("update_supported=yes", self.run_helper("status", check=True).stdout)
        result = self.run_helper("check-updates")
        self.assertEqual(result.returncode, 23)
        self.assertIn("release request failed", result.stdout)
        updater.unlink()
        result = self.run_helper("check-updates")
        self.assertEqual(result.returncode, 127)
        self.assertIn("release manager was not found", result.stdout)
        self.mock_uname("Darwin", "arm64")
        self.assertIn("update_supported=no", self.run_helper("status", check=True).stdout)
        for command in (("check-updates",), ("install-release", "v1.2.3")):
            result = self.run_helper(*command)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("native Linux only", result.stdout)

    def test_native_mutations_share_update_lock_and_internal_calls_can_bypass(self):
        self.mock_uname("Linux", "aarch64")
        self.add_bundled_runtime()
        lock_path = Path(self.env["PICOCLAW_HOME"]) / ".update.lock"
        lock_path.parent.mkdir()
        with lock_path.open("a") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for command in ("install", "gateway-start", "gateway-stop", "save-config"):
                result = self.run_helper(command)
                self.assertEqual(result.returncode, 75, result.stdout)
                self.assertIn("already in progress", result.stdout)
            self.assertFalse((Path(self.env["PICOCLAW_CONFIG_DIR"]) / "config.json").exists())
            self.env["PICOCLAW_UPDATE_INTERNAL"] = "1"
            self.assertIn("gateway=stopped", self.run_helper("gateway-stop", check=True).stdout)
            self.env.pop("PICOCLAW_UPDATE_INTERNAL")
        self.assertIn("gateway=stopped", self.run_helper("gateway-stop", check=True).stdout)

    def test_native_save_config_preserves_key_stdin_through_lock(self):
        self.mock_uname("Linux", "aarch64")
        secret = "private literal $value and `code`"
        result = subprocess.run(
            ["/bin/sh", str(self.helper), "save-config", "board-model", "openai/board-model",
             "https://example.com/v1", "--key-stdin"],
            cwd=ROOT, env=self.env, input=secret, text=True, capture_output=True, timeout=15, check=True,
        )
        security = Path(self.env["PICOCLAW_CONFIG_DIR"]) / ".security.yml"
        self.assertIn(secret, security.read_text())
        self.assertEqual(stat.S_IMODE(security.stat().st_mode), 0o600)
        self.assertNotIn(secret, result.stdout + result.stderr)

    def test_native_mutations_fail_when_flock_is_unavailable(self):
        self.mock_uname("Linux", "aarch64")
        minimal_path = self.root / "minimal-path"
        minimal_path.mkdir()
        for command in ("dirname", "basename", "mkdir"):
            (minimal_path / command).symlink_to(shutil.which(command))
        (minimal_path / "uname").symlink_to(self.bin_dir / "uname")
        self.env["PATH"] = str(minimal_path)
        for command in ("install", "gateway-start", "gateway-stop", "save-config"):
            result = self.run_helper(command)
            self.assertEqual(result.returncode, 127, result.stdout)
            self.assertIn("flock is required", result.stdout)

    def test_native_gateway_survives_termination_of_ui_process_group(self):
        self.mock_uname("Linux", "aarch64")
        self.add_bundled_runtime('''
case "$2" in
    version) printf 'picoclaw nightly\\n' ;;
    gateway)
        trap 'exit 0' TERM
        while :; do sleep 0.05; done
        ;;
esac
''')
        self.run_helper("save-config", "local", "ollama/test", "http://localhost:11434/v1", check=True)
        pid_file = self.root / "state" / "gateway.pid"
        process = subprocess.Popen(
            ["/bin/sh", str(self.helper), "gateway-start"], cwd=ROOT, env=self.env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True,
        )
        try:
            deadline = time.monotonic() + 5
            gateway_pid = None
            while time.monotonic() < deadline:
                if pid_file.exists():
                    gateway_pid = int(pid_file.read_text())
                    if os.getsid(gateway_pid) == gateway_pid:
                        break
                time.sleep(0.02)
            self.assertIsNotNone(gateway_pid)
            self.assertEqual(os.getsid(gateway_pid), gateway_pid)
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=5)
            os.kill(gateway_pid, 0)
            self.assertIn("gateway=running", self.run_helper("status", check=True).stdout)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                process.wait(timeout=5)
            process.stdout.close()
            self.run_helper("gateway-stop", check=True)

    def test_native_gateway_stop_keeps_pid_when_signal_fails_or_process_survives(self):
        self.mock_uname("Linux", "aarch64")
        self.env["PICOCLAW_UPDATE_INTERNAL"] = "1"
        self.add_command("ps", "printf 'S\\n'\n")
        self.add_command("sleep", "exit 0\n")
        # Override the shell builtin with a fixture; no process receives a
        # signal, and both denied and ineffective termination are exercised.
        wrapper = self.root / "stop-fixture"
        wrapper.write_text('''#!/bin/sh
kill() {
    if [ "$1" = "-0" ]; then return 0; fi
    return "$TEST_SIGNAL_RESULT"
}
. ''' + shlex.quote(str(HELPER)) + '\n')
        self.helper = wrapper
        pid_file = self.root / "state" / "gateway.pid"
        pid_file.parent.mkdir()
        pid_file.write_text("876543\n")
        for signal_result in ("0", "1"):
            with self.subTest(signal_result=signal_result):
                self.env["TEST_SIGNAL_RESULT"] = signal_result
                result = self.run_helper("gateway-stop")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("PID record has been kept", result.stdout)
                self.assertNotIn("gateway=stopped", result.stdout)
                self.assertEqual(pid_file.read_text(), "876543\n")

    def test_native_gateway_start_reports_immediate_runtime_failure(self):
        self.mock_uname("Linux", "aarch64")
        self.add_bundled_runtime('if [ "$2" = version ]; then printf "picoclaw fixture\\n"; else exit 42; fi\n')
        self.run_helper("save-config", "local", "ollama/test", "http://localhost:11434/v1", check=True)
        result = self.run_helper("gateway-start")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("gateway=running", result.stdout)
        self.assertFalse((self.root / "state" / "gateway.pid").exists())

    def test_docker_gateway_start_and_stop_confirm_actual_state(self):
        self.mock_uname("Darwin", "arm64")
        self.env["PICOCLAW_DOCKER"] = "yes"
        self.add_command("sleep", "exit 0\n")
        self.add_command("docker", '''
case "$1" in
    info|image) exit 0 ;;
    inspect)
        if [ "$2" = -f ]; then printf '%s\\n' "$TEST_DOCKER_RUNNING"; else exit 0; fi
        ;;
    rm) exit "$TEST_DOCKER_REMOVE_RESULT" ;;
    run) if [ "$2" != -d ]; then printf 'picoclaw fixture\\n'; fi ;;
    *) exit 99 ;;
esac
''')
        self.run_helper("save-config", "local", "ollama/test", "http://localhost:11434/v1", check=True)
        self.env.update(TEST_DOCKER_RUNNING="false", TEST_DOCKER_REMOVE_RESULT="0")
        result = self.run_helper("gateway-start")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Docker gateway exited", result.stdout)
        self.assertNotIn("gateway=running", result.stdout)
        self.env["TEST_DOCKER_RUNNING"] = "true"
        for remove_result in ("0", "1"):
            self.env["TEST_DOCKER_REMOVE_RESULT"] = remove_result
            result = self.run_helper("gateway-stop")
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("gateway=stopped", result.stdout)
        self.env.update(TEST_DOCKER_RUNNING="false", TEST_DOCKER_REMOVE_RESULT="0")
        self.assertIn("gateway=stopped", self.run_helper("gateway-stop", check=True).stdout)

    def test_explicit_runtime_choices_precede_bundled_runtime(self):
        self.add_bundled_runtime()
        selected = self.add_command("selected", 'printf "picoclaw selected\\n"\n')
        config_dir = Path(self.env["PICOCLAW_CONFIG_DIR"])
        config_dir.mkdir()
        (config_dir / "ui-settings.conf").write_text(f"binary_path={selected}\n")
        self.assertIn("version=picoclaw selected", self.run_helper("status", check=True).stdout)
        override = self.add_command("override", 'printf "picoclaw override\\n"\n')
        self.env["PICOCLAW_BIN"] = str(override)
        self.assertIn("version=picoclaw override", self.run_helper("status", check=True).stdout)

    def test_bundled_install_works_offline_without_downloading_or_replacing(self):
        binary = self.add_bundled_runtime()
        original = binary.read_bytes()
        self.add_command("ip", "exit 1\n")
        self.add_command("route", "exit 1\n")
        self.add_command("getent", "exit 1\n")
        result = self.run_helper("install", check=True)
        self.assertIn("Use Settings > Updates", result.stdout)
        self.assertIn("version=picoclaw nightly", result.stdout)
        self.assertFalse(self.capture.exists())
        self.assertFalse(self.managed.exists())
        self.assertEqual(binary.read_bytes(), original)

    def test_explicit_managed_install_keeps_bundled_runtime_selected(self):
        self.add_bundled_runtime()
        self.mock_uname("Linux", "aarch64")
        program = self.make_archive()
        self.env["PICOCLAW_RELEASE_URL"] = "https://example.com/custom-release.tar.gz"
        self.run_helper("install", check=True)
        self.assertEqual(self.managed.read_bytes(), program)
        self.assertEqual(self.capture.read_text().strip(), self.env["PICOCLAW_RELEASE_URL"])
        self.assertIn("version=picoclaw nightly", self.run_helper("status", check=True).stdout)

    def test_legacy_install_preserves_selected_release_without_bundle(self):
        self.mock_uname("Linux", "aarch64")
        selected = self.add_command("selected-release", 'printf "picoclaw selected-release\\n"\n')
        self.managed.parent.mkdir(parents=True)
        (self.managed.parent / "active-picoclaw").symlink_to(selected)
        downloaded = self.make_archive()
        result = self.run_helper("install", check=True)
        self.assertEqual(self.managed.read_bytes(), downloaded)
        self.assertIn("version=picoclaw selected-release", result.stdout)
        self.assertIn("version=picoclaw selected-release", self.run_helper("status", check=True).stdout)

    def test_linux_arm64_bundled_commands_are_native_for_all_docker_settings(self):
        self.mock_uname("Linux", "aarch64")
        docker_calls = self.root / "docker-calls"
        self.env["DOCKER_CALLS"] = str(docker_calls)
        self.add_command("docker", 'printf "%s\\n" "$*" >> "$DOCKER_CALLS"\nexit 99\n')
        calls = self.root / "runtime-calls"
        self.env["RUNTIME_CALLS"] = str(calls)
        loaded_config = self.root / "runtime-loaded-config"
        self.env["RUNTIME_LOADED_CONFIG"] = str(loaded_config)
        self.add_bundled_runtime('''
cat "$PICOCLAW_CONFIG" > "$RUNTIME_LOADED_CONFIG" || exit 65
printf '%s|%s|%s\\n' "$2" "$PICOCLAW_HOME" "$PICOCLAW_CONFIG" >> "$RUNTIME_CALLS"
case "$2" in
    version) printf 'picoclaw nightly\\n' ;;
    agent) printf 'reply\\n' ;;
    gateway)
        if read -r value; then
            printf 'stdin=unexpected-input\\n' >> "$RUNTIME_CALLS"
            exit 1
        fi
        printf 'stdin=closed\\n' >> "$RUNTIME_CALLS"
        trap 'printf "gateway=terminated\\n" >> "$RUNTIME_CALLS"; exit 0' TERM
        while :; do sleep 0.05; done
        ;;
esac
''')
        config = self.root / "existing config" / "board.json"
        config.parent.mkdir()
        config.write_text(json.dumps({
            "agents": {"defaults": {"model_name": "board-model"}},
            "model_list": [{"model_name": "board-model", "model": "ollama/board-model",
                            "api_base": "http://localhost:11434/v1"}],
            "preserve_this": {"custom": True},
        }, indent=2))
        security = config.parent / ".security.yml"
        security.write_text("existing private configuration\n")
        original_config, original_security = config.read_bytes(), security.read_bytes()
        self.env["PICOCLAW_CONFIG"] = str(config)
        pid_file = self.root / "state" / "gateway.pid"
        for docker_setting in (None, "no", "yes"):
            with self.subTest(docker_setting=docker_setting):
                if docker_setting is None:
                    self.env.pop("PICOCLAW_DOCKER", None)
                else:
                    self.env["PICOCLAW_DOCKER"] = docker_setting
                calls.unlink(missing_ok=True)
                status = self.run_helper("status", check=True).stdout
                self.assertIn("installed=yes", status)
                self.assertIn("model_name=board-model", status)
                installed = self.run_helper("install", check=True).stdout
                self.assertIn("Use Settings > Updates", installed)
                self.assertIn("installed=yes", installed)
                self.assertEqual(self.run_helper("ask", "hello", check=True).stdout, "reply\n")
                # Leave the UI's input pipe open while the helper exits. The
                # gateway must see /dev/null without waiting for that pipe.
                process = subprocess.Popen(
                    ["/bin/sh", str(self.helper), "gateway-start"], cwd=ROOT, env=self.env,
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                )
                gateway_pid = None
                try:
                    process.wait(timeout=5)
                    output = process.stdout.read()
                    self.assertEqual(process.returncode, 0, output)
                    self.assertIn("gateway=running", output)
                    self.assertIn("stdin=closed", calls.read_text().splitlines())
                    gateway_pid = int(pid_file.read_text())
                    os.kill(gateway_pid, 0)
                    self.assertEqual(os.getsid(gateway_pid), gateway_pid)
                finally:
                    process.stdin.close()
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                    process.stdout.close()
                    stopped = self.run_helper("gateway-stop", check=True).stdout
                self.assertIn("gateway=stopped", stopped)
                self.assertFalse(pid_file.exists())
                self.assertIn("gateway=terminated", calls.read_text().splitlines())
                runtime_calls = [line.split("|") for line in calls.read_text().splitlines() if "|" in line]
                self.assertTrue({"version", "agent", "gateway"}.issubset({call[0] for call in runtime_calls}))
                for _, runtime_home, runtime_config in runtime_calls:
                    self.assertEqual(runtime_home, self.env["PICOCLAW_HOME"])
                    self.assertEqual(runtime_config, str(config))
                self.assertEqual(loaded_config.read_bytes(), original_config)
                self.assertEqual(config.read_bytes(), original_config)
                self.assertEqual(security.read_bytes(), original_security)
                self.assertFalse(docker_calls.exists(), "Linux commands must not invoke Docker")
                self.assertFalse(self.capture.exists())

    def test_macos_simulator_defaults_to_docker_and_allows_native_override(self):
        self.mock_uname("Darwin", "arm64")
        self.add_bundled_runtime()
        docker_calls = self.root / "docker-calls"
        self.env["DOCKER_CALLS"] = str(docker_calls)
        self.add_command("docker", '''
printf '%s\\n' "$*" >> "$DOCKER_CALLS"
case "$1" in
    info|image|pull) exit 0 ;;
    run) printf 'picoclaw docker-simulator\\n' ;;
    inspect) printf 'false\\n' ;;
    *) exit 99 ;;
esac
''')
        for docker_setting in (None, "yes"):
            with self.subTest(docker_setting=docker_setting):
                if docker_setting is None:
                    self.env.pop("PICOCLAW_DOCKER", None)
                else:
                    self.env["PICOCLAW_DOCKER"] = docker_setting
                docker_calls.unlink(missing_ok=True)
                self.assertIn("version=picoclaw docker-simulator", self.run_helper("status", check=True).stdout)
                self.assertIn("version=picoclaw docker-simulator", self.run_helper("install", check=True).stdout)
                recorded = docker_calls.read_text()
                self.assertIn("run --rm --entrypoint picoclaw", recorded)
                self.assertIn("pull docker.io/sipeed/picoclaw:latest", recorded)
                self.assertFalse(self.capture.exists())
        self.env["PICOCLAW_DOCKER"] = "no"
        docker_calls.unlink()
        self.assertIn("version=picoclaw nightly", self.run_helper("status", check=True).stdout)
        self.assertFalse(docker_calls.exists())

    def test_status_finds_executable_picoclaw_on_path(self):
        self.add_command("picoclaw", 'printf "picoclaw 0.3.1\\n"\n')
        result = self.run_helper("status", check=True)
        self.assertIn("installed=yes", result.stdout)
        self.assertIn("version=picoclaw 0.3.1", result.stdout)

    def test_status_rejects_executable_with_failed_version(self):
        self.add_command("picoclaw", "exit 1\n")
        result = self.run_helper("status", check=True)
        self.assertIn("installed=no", result.stdout)

    def test_status_accepts_ollama_without_api_key(self):
        for model_id, api_base in (
            ("ollama/llama3.2", "http://127.0.0.1:11434/v1"),
            ("llama3.2", "http://localhost:11434/v1"),
        ):
            with self.subTest(model_id=model_id, api_base=api_base):
                saved = self.run_helper("save-config", "Local Ollama", model_id, api_base, check=True)
                self.assertIn("configuration saved", saved.stdout)
                result = self.run_helper("status", check=True)
                self.assertIn("model_key=ready", result.stdout)

    def mock_uname(self, system, machine):
        self.add_command("uname", f'\nif [ "$1" = "-s" ]; then printf "%s\\n" "{system}"; else printf "%s\\n" "{machine}"; fi\n')

    def test_ask_passes_conversation_session_to_runtime(self):
        self.add_command("picoclaw", 'printf "%s\\n" "$@"\n')
        self.run_helper("save-config", "local", "ollama/test", "http://localhost:11434/v1", check=True)
        for session in ("agent:main:ui:first-chat", "agent:main:ui:second-chat"):
            result = subprocess.run(
                ["sh", str(HELPER), "ask", "--prompt-stdin", session],
                env=self.env, input="hello $world", text=True, capture_output=True, timeout=15, check=True,
            )
            self.assertEqual(result.stdout.splitlines(), ["--no-color", "agent", "-s", session, "-m", "hello $world"])

    def test_ask_uses_explicit_model_alias_and_its_network_requirements(self):
        self.add_command("picoclaw", 'printf "%s\\n" "$@"\n')
        self.run_helper("save-config", "local", "ollama/local", "http://localhost:11434/v1", check=True)
        self.run_helper("save-config", "remote", "openai/remote", "https://remote.example/v1", "remote-secret", check=True)
        self.add_command("ip", "exit 1\n")
        self.add_command("route", "exit 1\n")
        self.add_command("getent", "exit 1\n")
        result = subprocess.run(
            ["/bin/sh", str(self.helper), "ask", "--prompt-stdin", "agent:main:ui:chosen", "local"],
            env=self.env, input="hello $world", text=True, capture_output=True, timeout=15, check=True,
        )
        self.assertEqual(result.stdout.splitlines(), ["--no-color", "agent", "--model", "local",
                                                      "-s", "agent:main:ui:chosen", "-m", "hello $world"])
        self.assertNotIn("remote-secret", result.stdout + result.stderr)
        offline = self.run_helper("ask", "hello", "session", "remote")
        self.assertNotEqual(offline.returncode, 0)
        self.assertIn("No network connection", offline.stdout)
        unknown = self.run_helper("ask", "hello", "session", "missing")
        self.assertNotEqual(unknown.returncode, 0)
        self.assertIn("Selected model is not configured", unknown.stdout)

    def test_save_key_next_to_config(self):
        for custom in (False, True):
            with self.subTest(custom=custom):
                config = self.root / ("custom" if custom else "config") / "config.json"
                self.env["PICOCLAW_CONFIG"] = str(config)
                result = self.run_helper("save-config", "test-model", "openai/test", "https://example.com/v1", "test-secret", check=True)
                security = config.parent / ".security.yml"
                self.assertIn("test-secret", security.read_text())
                self.assertEqual(stat.S_IMODE(security.stat().st_mode), 0o600)
                self.assertNotIn("test-secret", config.read_text())
                self.assertNotIn("test-secret", result.stdout)
                self.assertIn("model_key=ready", self.run_helper("status", check=True).stdout)

    def test_switch_models_preserves_config_and_reuses_saved_keys(self):
        self.run_helper("save-config", "first", "openai/first", "https://first.example/v1", "first-secret", check=True)
        config_path = self.root / "config" / "config.json"
        config = json.loads(config_path.read_text())
        config["agents"]["defaults"]["max_tokens"] = 1234
        config["gateway"]["port"] = 19876
        config["channel_list"] = {"telegram": {"enabled": True}}
        config["model_list"][0]["request_timeout"] = 45
        config_path.write_text(json.dumps(config))
        security_path = config_path.parent / ".security.yml"
        security_path.write_text(security_path.read_text() + "\nchannel_list:\n  telegram:\n    token: preserved-token\n")

        added = self.run_helper("save-config", "second", "openai/second", "https://second.example/v1", "second-secret", check=True)
        selected = self.run_helper("status", check=True).stdout
        self.assertIn("model_name=second\n", selected)
        self.assertIn("model_id=openai/second\n", selected)
        self.assertIn("api_base=https://second.example/v1\n", selected)
        self.assertIn("model_key=ready\n", selected)
        saved_keys = security_path.read_bytes()
        self.run_helper("save-config", "first", "openai/first", "https://first.example/v1", check=True)
        self.assertEqual(security_path.read_bytes(), saved_keys)
        selected = self.run_helper("status", check=True).stdout
        self.assertIn("model_name=first\n", selected)
        self.assertIn("model_id=openai/first\n", selected)
        self.assertIn("model_key=ready\n", selected)
        saved = json.loads(config_path.read_text())
        self.assertEqual([item["model_name"] for item in saved["model_list"]], ["first", "second"])
        self.assertEqual(saved["model_list"][0], config["model_list"][0])
        self.assertEqual(saved["agents"], config["agents"])
        self.assertEqual(saved["gateway"], config["gateway"])
        self.assertEqual(saved["channel_list"], config["channel_list"])
        for secret in ("first-secret", "second-secret", "preserved-token"):
            self.assertIn(secret, saved_keys.decode())
            self.assertNotIn(secret, config_path.read_text())
            self.assertNotIn(secret, added.stdout + selected)
        self.assertEqual(stat.S_IMODE(config_path.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(security_path.stat().st_mode), 0o600)

    def test_updating_model_key_preserves_other_security_fields(self):
        self.run_helper("save-config", "first", "openai/first", "https://first.example/v1", "old-secret", check=True)
        self.run_helper("save-config", "second", "openai/second", "https://second.example/v1", "second-secret", check=True)
        security_path = self.root / "config" / ".security.yml"
        security = security_path.read_text().replace("  first:0:\n", "  first:0:\n    proxy: http://proxy.example\n")
        security_path.write_text(security)
        result = self.run_helper("save-config", "first", "openai/new-first", "https://first.example/v2", "new-'secret", check=True)
        security = security_path.read_text()
        self.assertIn("proxy: http://proxy.example", security)
        self.assertIn("second-secret", security)
        self.assertIn("new-''secret", security)
        self.assertNotIn("old-secret", security)
        self.assertEqual(security.count("  first:0:"), 1)
        self.assertNotIn("new-", result.stdout)
        config = json.loads((self.root / "config" / "config.json").read_text())
        self.assertEqual(len(config["model_list"]), 2)
        self.assertEqual(config["model_list"][0]["model"], "openai/new-first")
        self.assertEqual(config["model_list"][1]["model"], "openai/second")

    def test_save_config_preserves_invalid_existing_files_and_reports_failure(self):
        self.run_helper("save-config", "first", "openai/first", "https://first.example/v1", "original-secret", check=True)
        config = self.root / "config" / "config.json"
        security = config.parent / ".security.yml"
        good_config = config.read_bytes()
        good_security = security.read_bytes()
        for broken_config in (b"{truncated", b"null", b'{"model_list": {}}'):
            with self.subTest(config=broken_config):
                config.write_bytes(broken_config)
                result = self.run_helper("save-config", "second", "openai/second", "https://second.example/v1", "new-secret")
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("OK:", result.stdout)
                self.assertEqual(config.read_bytes(), broken_config)
                self.assertEqual(security.read_bytes(), good_security)
        config.write_bytes(good_config)
        # A valid but unsupported YAML representation must not be replaced.
        broken_security = b'model_list: {first: {api_keys: [original-secret]}}\n'
        security.write_bytes(broken_security)
        result = self.run_helper("save-config", "second", "openai/second", "https://second.example/v1", "new-secret")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("OK:", result.stdout)
        self.assertNotIn("secret", result.stdout)
        self.assertEqual(config.read_bytes(), good_config)
        self.assertEqual(security.read_bytes(), broken_security)
        self.assertEqual(list(config.parent.glob("config.json.*")), [])
        self.assertEqual(list(config.parent.glob(".security.yml.*")), [])

    def test_failed_config_commit_restores_previous_security(self):
        self.run_helper("save-config", "first", "openai/first", "https://first.example/v1", "original-secret", check=True)
        config = self.root / "config" / "config.json"
        security = config.parent / ".security.yml"
        original_config, original_security = config.read_bytes(), security.read_bytes()
        self.env["TEST_REJECT_CONFIG_REPLACE"] = str(config)
        self.add_python_command("python3", '''
import os, sys
real_replace = os.replace
def reject_config_replace(source, destination):
    if str(destination) == os.environ["TEST_REJECT_CONFIG_REPLACE"]:
        raise OSError("Simulated configuration commit failure")
    return real_replace(source, destination)
os.replace = reject_config_replace
assert sys.argv[1] == "-c"
code = sys.argv[2]
sys.argv = ["-c"] + sys.argv[3:]
exec(compile(code, "<control>", "exec"))
''')
        result = self.run_helper("save-config", "second", "openai/second", "https://second.example/v1", "new-secret")
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("OK:", result.stdout)
        self.assertEqual(config.read_bytes(), original_config)
        self.assertEqual(security.read_bytes(), original_security)
        self.assertEqual(list(config.parent.glob("config.json.*")), [])
        self.assertEqual(list(config.parent.glob(".security.yml.*")), [])

    def exported_models(self):
        result = self.run_helper("export-models", check=True)
        lines = result.stdout.splitlines()
        self.assertRegex(lines[0], r"^PICOCLAW_MODELS 1 [0-9]+$")
        self.assertEqual(len(lines) - 1, int(lines[0].split()[-1]))
        records = [shlex.split(line) for line in lines[1:]]
        self.assertTrue(all(len(record) == 5 for record in records))
        return records

    def test_export_models_reads_all_keys_without_logging_or_mutation(self):
        first_key = 'first-"quoted"-\\backslash-密钥'
        second_key = "second-'quoted'-secret"
        self.run_helper("save-config", "first", "openai/first", "https://first.example/v1", first_key, check=True)
        self.run_helper("save-config", "second", "openai/second", "https://second.example/v1", second_key, check=True)
        config = self.root / "config" / "config.json"
        data = json.loads(config.read_text())
        data["model_list"][0]["api_key"] = "stale-inline-secret"
        data["model_list"].append({"model_name": "模型三", "model": "openai/third",
                                   "api_base": "https://third.example/v1", "api_key": "inline-secret"})
        config.write_text(json.dumps(data, ensure_ascii=False))
        tracked = (config, config.parent / ".security.yml", self.root / "state" / "activity.log")
        before = {path: path.read_bytes() for path in tracked}
        self.assertEqual(self.exported_models(), [
            ["first", "first", "openai/first", "https://first.example/v1", first_key],
            ["second", "second", "openai/second", "https://second.example/v1", second_key],
            ["模型三", "模型三", "openai/third", "https://third.example/v1", "inline-secret"],
        ])
        for path in tracked:
            self.assertEqual(path.read_bytes(), before[path])
        logs = self.run_helper("log", check=True).stdout
        for key in (first_key, second_key, "inline-secret", "stale-inline-secret"):
            self.assertNotIn(key, logs)
        self.assertNotIn("PICOCLAW_MODELS", logs)

    def test_export_models_supports_quoted_security_and_legacy_without_copying(self):
        config = self.root / "config" / "config.json"
        config.parent.mkdir()
        config.write_text(json.dumps({"model_list": [{"model_name": "first", "model": "openai/first",
                                                       "api_base": "https://first.example/v1"}]}))
        legacy = self.root / "pico-home" / ".security.yml"
        legacy.parent.mkdir()
        key = 'legacy-"quoted"-\\backslash'
        legacy.write_text('model_list:\n  "first:0":\n    api_keys:\n      - ' + json.dumps(key) + '\n')
        self.assertEqual(self.exported_models()[0][-1], key)
        security = config.parent / ".security.yml"
        self.assertFalse(security.exists())
        self.assertFalse((self.root / "state").exists())
        security.write_text("model_list:\n  'first:0':\n    api_keys:\n      - 'current-''secret'\n")
        self.assertEqual(self.exported_models()[0][-1], "current-'secret")

    def test_export_models_empty_config_is_read_only(self):
        self.assertEqual(self.exported_models(), [])
        self.assertFalse((self.root / "config").exists())
        self.assertFalse((self.root / "pico-home").exists())
        config = self.root / "config" / "config.json"
        config.parent.mkdir()
        config.write_text("")
        self.assertEqual(self.exported_models(), [])
        self.assertEqual(config.read_bytes(), b"")

    def test_export_models_rejects_corruption_and_limits_without_partial_secret_output(self):
        config = self.root / "config" / "config.json"
        config.parent.mkdir()
        model = {"model_name": "first", "model": "openai/first", "api_base": "https://first.example/v1",
                 "api_key": "private-secret"}
        cases = ["{broken", "null", json.dumps({"model_list": {}}),
                 json.dumps({"model_list": [model] * 129}),
                 json.dumps({"model_list": [model, dict(model, api_key="private-secret\nextra")]}),
                 json.dumps({"model_list": [dict(model, api_key="private-secret" + "a" * (1024 * 1024))]})]
        for contents in cases:
            with self.subTest(size=len(contents)):
                config.write_text(contents)
                result = self.run_helper("export-models")
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn("PICOCLAW_MODELS", result.stdout)
                self.assertNotIn("private-secret", result.stdout)
                self.assertEqual(config.read_text(), contents)
        config.write_text(json.dumps({"model_list": [model]}))
        security = config.parent / ".security.yml"
        for contents in ("malformed-private-secret\n", "model_list: {first: private-secret}\n",
                         "model_list:\n  first:0:\n    api_keys:\n      - |\n        private-secret\n",
                         "model_list:\n  first:0:\n    api_keys:\n      - 'private-secret\n"):
            security.write_text(contents)
            result = self.run_helper("export-models")
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("PICOCLAW_MODELS", result.stdout)
            self.assertNotIn("private-secret", result.stdout)
            self.assertEqual(security.read_text(), contents)

    def test_migrates_legacy_key_without_overwriting_current_security(self):
        legacy = self.root / "pico-home" / ".security.yml"
        legacy.parent.mkdir(parents=True)
        legacy.write_text("model_list:\n  test-model:0:\n    api_keys:\n      - 'legacy-secret'\n")
        self.run_helper("status", check=True)
        security = self.root / "config" / ".security.yml"
        self.assertEqual(security.read_text(), legacy.read_text())
        self.assertEqual(stat.S_IMODE(security.stat().st_mode), 0o600)
        security.write_text("current-secrets\n")
        self.run_helper("status", check=True)
        self.assertEqual(security.read_text(), "current-secrets\n")

    def make_archive(self, runnable=True):
        program = b'#!/bin/sh\nprintf "picoclaw 0.3.1\\n"\n' if runnable else b'#!/bin/sh\nexit 1\n'
        archive = Path(self.env["TEST_ARCHIVE"])
        with tarfile.open(archive, "w:gz") as output:
            info = tarfile.TarInfo("picoclaw")
            info.size = len(program)
            info.mode = 0o755
            output.addfile(info, io.BytesIO(program))
        self.env["PICOCLAW_RELEASE_SHA256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
        return program

    def test_install_supports_darwin_arm64_and_x86_64(self):
        for machine in ("arm64", "x86_64"):
            with self.subTest(machine=machine):
                self.mock_uname("Darwin", machine)
                self.make_archive()
                result = self.run_helper("install")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("installed=yes", result.stdout)
                self.assertIn(f"picoclaw_Darwin_{machine}.tar.gz", (self.root / "download-url").read_text())

    def test_install_supports_existing_linux_architectures(self):
        for machine, archive_arch in (("aarch64", "arm64"), ("x86_64", "x86_64"), ("armv7l", "armv7")):
            with self.subTest(machine=machine):
                self.mock_uname("Linux", machine)
                self.make_archive()
                result = self.run_helper("install")
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn(f"picoclaw_Linux_{archive_arch}.tar.gz", self.capture.read_text())

    def test_install_rejects_unsupported_platform(self):
        self.mock_uname("Darwin", "riscv64")
        result = self.run_helper("install")
        self.assertNotEqual(result.returncode, 0)
        self.assertRegex(result.stdout, r"unsupported (platform|architecture)")

    def test_install_unusable_binary_does_not_replace_existing(self):
        self.managed.parent.mkdir(parents=True)
        self.managed.write_text("old runtime\n")
        self.mock_uname("Darwin", "arm64")
        self.make_archive(runnable=False)
        result = self.run_helper("install")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot run", result.stdout)
        self.assertEqual(self.managed.read_text(), "old runtime\n")

    def test_install_rejects_checksum_mismatch(self):
        self.mock_uname("Darwin", "arm64")
        self.make_archive()
        self.env["PICOCLAW_RELEASE_SHA256"] = "0" * 64
        result = self.run_helper("install")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("checksum mismatch", result.stdout)
        self.assertFalse(self.managed.exists())


if __name__ == "__main__":
    unittest.main()
