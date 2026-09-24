#!/usr/bin/env python3
"""Channel persistence tests use only isolated temporary configuration files."""

import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shlex
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/picoclaw-channels.py"
spec = importlib.util.spec_from_file_location("picoclaw_channels", SCRIPT)
channels = importlib.util.module_from_spec(spec)
spec.loader.exec_module(channels)


class ChannelTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.home = self.root / "runtime"
        self.config = self.root / "config/config.json"
        self.security = self.config.with_name(".security.yml")
        self.config.parent.mkdir()
        self.home.mkdir()
        self.original = {
            "version": 3, "unknown": {"nested": [1, 2]},
            "agents": {"defaults": {"model_name": "test"}},
            "model_list": [{"model_name": "test", "model": "openai/test"}],
            "channel_list": {
                "telegram": {"enabled": True, "type": "telegram", "allow_from": ["123"],
                             "settings": {"proxy": "", "token": "old-inline-token", "custom": {"keep": True}}},
                "pico": {"enabled": True, "type": "pico", "settings": {"custom": 42}},
            },
        }
        self.config.write_text(json.dumps(self.original))
        self.original_security = (
            "# Secrets retained by the channel editor\n"
            "model_list:\n    test:0:\n        api_keys:\n            - model-secret\n"
            "channel_list:\n    telegram:\n        settings:\n            token: 'old-security-token'\n"
            "            custom_secret: keep-this\n"
            "    pico:\n        settings:\n            token: pico-secret\n"
            "tools:\n    web:\n        api_key: another-secret\n"
        ).encode()
        self.security.write_bytes(self.original_security)
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("PICOCLAW_")}
        self.env.update(PICOCLAW_HOME=str(self.home), PICOCLAW_CONFIG=str(self.config),
                        PICOCLAW_CONFIG_DIR=str(self.config.parent), PICOCLAW_STATE_DIR=str(self.root / "state"),
                        PICOCLAW_DOCKER="no")

    def tearDown(self):
        self.temp.cleanup()

    def save(self, name, fields, enabled=True):
        channels.save_channel(name, {"enabled": enabled, "fields": fields}, self.config, self.security, self.home)

    def run_helper(self, *args, payload=None, control=False):
        command = ["/bin/sh", str(ROOT / "scripts/picoclaw-control.sh")] if control else [sys.executable, str(SCRIPT)]
        return subprocess.run(command + list(args), input=payload, text=True, capture_output=True,
                              env=self.env, timeout=10)

    def snapshot(self):
        return self.config.read_bytes(), self.security.read_bytes()

    def test_load_all_channels_merges_nested_secrets_without_writes(self):
        before = self.snapshot()
        result = self.run_helper("load-channels", control=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        tokens = shlex.split(result.stdout)
        self.assertEqual(tokens[:3], ["PICOCLAW_CHANNELS", "1", "10"])
        self.assertIn("old-security-token", tokens)
        self.assertNotIn("old-inline-token", tokens)
        self.assertNotIn("model-secret", tokens)
        self.assertEqual(tokens[-1], "END_CHANNELS")
        self.assertEqual(self.snapshot(), before)
        self.assertFalse((self.root / "state").exists())

    def test_save_uses_runtime_schema_and_preserves_unrelated_fields(self):
        secret = 'new "private" \\ value: with # punctuation'
        self.save("telegram", {"token": secret, "allow_from": "123, 456,123", "base_url": "https://api.telegram.org"})
        saved = json.loads(self.config.read_text())
        entry = saved["channel_list"]["telegram"]
        self.assertEqual(entry["allow_from"], ["123", "456"])
        self.assertTrue(entry["enabled"])
        self.assertEqual(entry["type"], "telegram")
        self.assertNotIn("token", entry["settings"])
        self.assertEqual(entry["settings"]["custom"], {"keep": True})
        for key in ("model_list", "agents", "unknown"):
            self.assertEqual(saved[key], self.original[key])
        self.assertEqual(saved["channel_list"]["pico"], self.original["channel_list"]["pico"])
        updated = self.security.read_text()
        self.assertIn("model_list:\n    test:0:\n        api_keys:\n            - model-secret\n", updated)
        self.assertIn("    pico:\n        settings:\n            token: pico-secret\n", updated)
        self.assertIn("            custom_secret: keep-this\n", updated)
        _, values = channels.channel_values("telegram", saved["channel_list"], channels.SecurityDocument(self.security.read_bytes()))
        self.assertEqual(values["token"], secret)
        self.assertEqual(stat.S_IMODE(self.config.stat().st_mode), 0o600)
        self.assertEqual(stat.S_IMODE(self.security.stat().st_mode), 0o600)

    def test_each_channel_can_be_saved_with_correct_secret_locations(self):
        for name, schema in channels.SCHEMA.items():
            with self.subTest(channel=name):
                fields = {key: "credential-value" for key, (kind, default, required) in schema.items()
                          if required and kind in ("secret", "text")}
                self.save(name, fields)
                config = json.loads(self.config.read_text())
                entry = config["channel_list"][name]
                self.assertTrue(entry["enabled"])
                self.assertEqual(entry["type"], name)
                security = channels.SecurityDocument(self.security.read_bytes())
                for key, (kind, _, _) in schema.items():
                    if kind == "secret":
                        self.assertNotIn(key, entry["settings"])
                        self.assertEqual(security.scalar(["channel_list", name, "settings", key]), fields.get(key, ""))

    def test_partial_save_preserves_existing_credentials_and_disable_needs_no_key(self):
        self.save("telegram", {}, False)
        _, values = channels.channel_values("telegram", json.loads(self.config.read_text())["channel_list"],
                                            channels.SecurityDocument(self.security.read_bytes()))
        self.assertEqual(values["token"], "old-security-token")
        self.save("telegram", {"token": ""}, False)
        enabled, values = channels.channel_values("telegram", json.loads(self.config.read_text())["channel_list"],
                                                  channels.SecurityDocument(self.security.read_bytes()))
        self.assertFalse(enabled)
        self.assertEqual(values["token"], "")

    def test_weixin_binding_is_saved_for_gateway_and_survives_later_edits(self):
        token = "weixin-bound-private-token"
        account = "bound-account@im.bot"
        endpoint = "https://region.weixin.qq.com/"
        result = self.run_helper(
            "save-channel", "weixin",
            payload=json.dumps({"enabled": True, "fields": {
                "token": token, "account_id": account, "base_url": endpoint,
                "proxy": "http://127.0.0.1:7890", "allow_from": "123,456",
            }}),
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(token, result.stdout + result.stderr + self.config.read_text())
        saved = json.loads(self.config.read_text())["channel_list"]["weixin"]
        self.assertTrue(saved["enabled"])
        self.assertEqual(saved["type"], "weixin")
        self.assertEqual(saved["settings"]["account_id"], account)
        self.assertEqual(saved["settings"]["base_url"], endpoint)
        self.assertNotIn("token", saved["settings"])
        self.assertEqual(channels.SecurityDocument(self.security.read_bytes()).scalar(
            ["channel_list", "weixin", "settings", "token"]), token)

        # Changing network/access settings must retain the confirmed binding,
        # including a region-specific API host, for the next gateway restart.
        self.save("weixin", {"proxy": "", "allow_from": "456"})
        saved_config, _, security = channels.read_runtime(self.config, self.security, self.home)
        enabled, values = channels.channel_values("weixin", saved_config["channel_list"], security)
        self.assertTrue(enabled)
        self.assertEqual(values["token"], token)
        self.assertEqual(values["account_id"], account)
        self.assertEqual(values["base_url"], endpoint)
        self.assertEqual(values["proxy"], "")
        self.assertEqual(values["allow_from"], "456")

    def test_invalid_payloads_do_not_change_files(self):
        for name, fields, enabled in (
            ("missing", {}, True), ("telegram", {"unknown": "x"}, False),
            ("telegram", {"token": ""}, True), ("telegram", {"proxy": "file:///tmp/private"}, False),
            ("telegram", {"token": "embedded\nnewline-secret"}, False),
            ("discord", {"mention_only": "yes"}, False), ("line", {"webhook_port": "65536"}, False),
            ("line", {"webhook_path": "no-leading-slash"}, False),
            ("onebot", {"ws_url": "https://example.com"}, False),
        ):
            with self.subTest(channel=name, fields=list(fields)):
                before = self.snapshot()
                with self.assertRaises(ValueError):
                    self.save(name, fields, enabled)
                self.assertEqual(self.snapshot(), before)

    def test_cli_transports_secret_on_stdin_and_never_logs_it(self):
        secret = "sensitive-token-$()-`literal`"
        # Linux control lock can be delegated to the direct helper for this
        # black-box transport test; locking has a separate real flock test.
        self.env["PICOCLAW_UPDATE_INTERNAL"] = "1"
        result = self.run_helper("save-channel", "telegram", "--json-stdin", control=True,
                                 payload=json.dumps({"enabled": True, "fields": {"token": secret}}))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Restart the gateway", result.stdout)
        self.assertNotIn(secret, result.stdout + result.stderr + self.config.read_text())
        self.assertFalse((self.root / "state/activity.log").exists())
        result = self.run_helper("save-channel", "telegram", payload=secret)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(secret, result.stdout + result.stderr)

    def test_existing_and_legacy_security_support_single_quoted_keys_and_empty_maps(self):
        self.security.write_text("'channel_list':\n  'telegram':\n    settings: {}\nmodel_list: {}\n")
        self.save("telegram", {"token": "quoted-'token"})
        self.assertEqual(channels.SecurityDocument(self.security.read_bytes()).scalar(
            ["channel_list", "telegram", "settings", "token"]), "quoted-'token")
        self.security.unlink()
        legacy = self.home / ".security.yml"
        legacy.write_text("channels:\n  telegram:\n    token: legacy-token\n")
        output = channels.load_channels(self.config, self.security, self.home)
        self.assertIn('"legacy-token"', output)
        self.assertFalse(self.security.exists())
        self.save("telegram", {})
        saved = channels.SecurityDocument(self.security.read_bytes())
        self.assertEqual(saved.scalar(["channels", "telegram", "settings", "token"]), "legacy-token")
        self.assertIsNone(saved.node(["channels", "telegram", "token"]))
        self.assertIn("token: legacy-token", legacy.read_text())

    def test_duplicate_malformed_and_unsupported_security_fails_closed(self):
        invalid = [b"channel_list: {telegram: {settings: {token: inline-secret}}}\n",
                   b"channel_list:\n  telegram:\n    settings:\n      token: first\n      token: second\n",
                   b"channel_list:\n  telegram:\n    settings:\n      token: |\n        multiline\n",
                   b"channel_list: {}\nchannels: {}\n", b"not a mapping\n"]
        for data in invalid:
            with self.subTest(data=data[:40]):
                self.security.write_bytes(data)
                before = self.snapshot()
                with self.assertRaises(ValueError):
                    self.save("telegram", {"token": "new-secret"})
                self.assertEqual(self.snapshot(), before)
                self.assertEqual(list(self.config.parent.glob("*.json.*")), [])

    def test_failed_load_never_emits_partial_credentials(self):
        with self.security.open("a") as output:
            output.write("channel_list: {}\n")
        result = self.run_helper("load-channels")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        self.assertNotIn("old-security-token", result.stderr)

    def test_duplicate_json_fields_and_invalid_lists_fail_without_changes(self):
        original = self.snapshot()
        duplicate = '{"enabled":true,"fields":{"token":"first-secret","token":"second-secret"}}'
        result = self.run_helper("save-channel", "telegram", payload=duplicate)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("secret", result.stdout + result.stderr)
        self.assertEqual(self.snapshot(), original)
        self.original["channel_list"]["telegram"]["allow_from"] = [True, {"private": "value"}]
        self.config.write_text(json.dumps(self.original))
        before = self.snapshot()
        with self.assertRaises(ValueError):
            self.save("telegram", {"token": "new-secret"})
        self.assertEqual(self.snapshot(), before)

    def test_failed_config_commit_restores_security_and_removes_staging_files(self):
        before = self.snapshot()
        real_replace = os.replace

        def fail_config(source, destination):
            if destination == self.config:
                raise OSError("simulated storage failure")
            return real_replace(source, destination)

        with mock.patch.object(channels.os, "replace", side_effect=fail_config):
            with self.assertRaises(OSError):
                self.save("telegram", {"token": "new-secret"})
        self.assertEqual(self.snapshot(), before)
        self.assertEqual(set(p.name for p in self.config.parent.iterdir()), {"config.json", ".security.yml"})

    def test_load_and_save_obey_release_operation_lock(self):
        before = self.snapshot()
        with (self.home / ".update.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for command in (("load-channels",), ("save-channel", "telegram")):
                result = self.run_helper(*command, payload=json.dumps({"enabled": False, "fields": {}}))
                self.assertEqual(result.returncode, 75, result.stdout + result.stderr)
                self.assertNotIn("old-security-token", result.stdout + result.stderr)
        self.assertEqual(self.snapshot(), before)

    def test_fresh_configuration_and_failed_initial_commit(self):
        self.config.unlink()
        self.security.unlink()
        self.assertIn("PICOCLAW_CHANNELS 1 10", channels.load_channels(self.config, self.security, self.home))
        self.assertFalse(self.config.exists())
        real_replace = os.replace

        def fail_config(source, destination):
            if destination == self.config:
                raise OSError("simulated storage failure")
            return real_replace(source, destination)

        with mock.patch.object(channels.os, "replace", side_effect=fail_config):
            with self.assertRaises(OSError):
                self.save("telegram", {"token": "new-secret"})
        self.assertFalse(self.security.exists())
        self.save("line", {"channel_secret": "secret", "channel_access_token": "token"})
        saved = json.loads(self.config.read_text())
        self.assertEqual(saved["version"], 3)
        self.assertEqual(saved["channel_list"]["line"]["settings"]["webhook_port"], 18791)


if __name__ == "__main__":
    unittest.main()
