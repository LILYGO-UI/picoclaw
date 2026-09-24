#!/usr/bin/env python3
"""Exercise provider queries through the packaged control helper with fake HTTP."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ProviderTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixtures = self.root / "responses.json"
        self.calls = self.root / "calls.jsonl"
        self.key = 'private-test-key-with-"quotes"-and-\\slashes'
        shim = self.root / "curl"
        shim.write_text(f"#!{sys.executable}\n" + '''
import json, os, sys
from pathlib import Path
arguments = sys.argv[1:]
assert arguments[arguments.index("--config") + 1] == "-"
assert "--location" not in arguments and "-L" not in arguments
url = arguments[arguments.index("--url") + 1]
headers = sys.stdin.read()
assert os.environ["TEST_PROVIDER_KEY"] not in " ".join(arguments)
with open(os.environ["TEST_PROVIDER_CALLS"], "a") as output:
    output.write(json.dumps({"url": url, "headers": headers}) + "\\n")
responses = json.loads(Path(os.environ["TEST_PROVIDER_FIXTURES"]).read_text())
response = responses.get(url, {"status": 22})
if response.get("status", 0):
    print(os.environ["TEST_PROVIDER_KEY"], file=sys.stderr)
    sys.exit(response["status"])
body = response["body"]
print(body if isinstance(body, str) else json.dumps(body))
''')
        shim.chmod(0o755)
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("PICOCLAW_")}
        self.env.update(
            PATH=str(self.root) + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin"),
            PICOCLAW_HOME=str(self.root / "runtime"),
            PICOCLAW_CONFIG_DIR=str(self.root / "config"),
            PICOCLAW_STATE_DIR=str(self.root / "state"),
            TEST_PROVIDER_FIXTURES=str(self.fixtures), TEST_PROVIDER_CALLS=str(self.calls),
            TEST_PROVIDER_KEY=self.key,
        )

    def query(self, base, responses, command="test-connection", key=None):
        self.fixtures.write_text(json.dumps({url: {"body": body} for url, body in responses.items()}))
        result = subprocess.run(
            ["/bin/sh", str(ROOT / "scripts/picoclaw-control.sh"), command, base, "--key-stdin"],
            input=self.key if key is None else key, capture_output=True, text=True, env=self.env, timeout=15,
        )
        self.assertNotIn(self.key, result.stdout + result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        return result

    def requests(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def test_deepseek_balance_and_structured_model_ids(self):
        base = "https://api.deepseek.com/v1"
        result = self.query(base + "/models/", {
            base + "/models": {"data": [{"id": "model,with{punctuation}"}, {"id": "second/model"}]},
            "https://api.deepseek.com/user/balance": {"is_available": True, "balance_infos": [
                {"currency": "CNY", "total_balance": "12.34"}, {"currency": "USD", "total_balance": "0"}]},
        }, "fetch-models")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "connection=ok\nquota=available\nbalance=CNY\t12.34\n"
                         "balance=USD\t0.00\nmodel=model,with{punctuation}\nmodel=second/model\n")
        self.assertIn("Authorization: Bearer private-test-key", self.requests()[0]["headers"])

    def test_balance_errors_do_not_hide_a_working_connection(self):
        for balance in ({"balance_infos": [{"currency": "USD", "total_balance": "NaN"}]},
                        {"balance_infos": [{"currency": "USD", "total_balance": True}]},
                        {"balance_infos": [{"currency": "secret", "total_balance": "1"}]},
                        {"balance_infos": None}, {"error": self.key}, "not JSON"):
            with self.subTest(balance=balance):
                result = self.query("https://api.deepseek.com/v1", {
                    "https://api.deepseek.com/v1/models": {"data": []},
                    "https://api.deepseek.com/user/balance": balance,
                })
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "connection=ok\nquota=unavailable\n")
        result = self.query("https://api.deepseek.com/v1", {"https://api.deepseek.com/v1/models": {"data": []}})
        self.assertEqual(result.stdout, "connection=ok\nquota=unavailable\n")

    def test_failed_auth_or_invalid_models_never_reports_connected(self):
        for body in (None, "<html>sign in</html>", {"error": self.key}, {"data": {}},
                     {"data": [{"id": "model\nbalance=USD\t100"}]}, {"success": False, "data": []}):
            responses = {} if body is None else {"https://api.deepseek.com/v1/models": body}
            result = self.query("https://api.deepseek.com/v1", responses)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "connection=failed\nquota=unavailable\n")
        self.assertTrue(all(call["url"].endswith("/models") for call in self.requests()))

    def test_official_providers_without_balance_api_and_ollama(self):
        cases = [
            ("https://api.openai.com/v1", "https://api.openai.com/v1/models", {"data": [{"id": "gpt-test"}]},
             "unsupported", "Authorization: Bearer"),
            ("https://api.anthropic.com/v1", "https://api.anthropic.com/v1/models", {"data": [{"id": "claude-test"}]},
             "unsupported", "x-api-key:"),
            ("https://generativelanguage.googleapis.com/v1beta", "https://generativelanguage.googleapis.com/v1beta/models",
             {"models": [{"name": "models/gemini-test"}]}, "unsupported", "x-goog-api-key:"),
            ("http://device.local:11434/v1", "http://device.local:11434/api/tags",
             {"models": [{"name": "llama3.2:latest"}]}, "unmetered", "Accept:"),
        ]
        for base, url, body, quota, header in cases:
            with self.subTest(base=base):
                previous = len(self.requests())
                result = self.query(base, {url: body}, "fetch-models", key="" if quota == "unmetered" else None)
                self.assertEqual(result.returncode, 0)
                self.assertIn("quota=" + quota + "\n", result.stdout)
                self.assertEqual(len(self.requests()), previous + 1)
                self.assertIn(header, self.requests()[-1]["headers"])
        self.assertNotIn("Authorization", self.requests()[-1]["headers"])

    def test_openrouter_key_limit_or_account_credits(self):
        base = "https://openrouter.ai/api/v1"
        responses = {base + "/models": {"data": []}, base + "/key": {"data": {"limit_remaining": 0}}}
        self.assertIn("balance=USD\t0.00\n", self.query(base, responses).stdout)
        responses[base + "/key"] = {"data": {"limit_remaining": None}}
        responses[base + "/credits"] = {"data": {"total_credits": 20, "total_usage": 3.25}}
        self.assertIn("balance=USD\t16.75\n", self.query(base, responses).stdout)

    def test_siliconflow_and_compatible_gateway_units(self):
        for host, currency in (("api.siliconflow.cn", "CNY"), ("api.siliconflow.com", "USD")):
            base = "https://" + host + "/v1"
            result = self.query(base, {base + "/models": {"data": []},
                                      base + "/user/info": {"data": {"totalBalance": "0.005", "balance": "0"}}})
            self.assertIn("balance=" + currency + "\t0.005\n", result.stdout)
        result = self.query("https://gateway.test/prefix/v1", {
            "https://gateway.test/prefix/v1/models": {"data": []},
            "https://gateway.test/prefix/dashboard/billing/subscription": {"hard_limit_usd": 10},
            "https://gateway.test/prefix/dashboard/billing/usage": {"total_usage": 125},
        })
        self.assertIn("balance=USD\t8.75\n", result.stdout)

    def test_invalid_urls_and_header_injection_do_not_send_requests(self):
        for base, key in (("file:///tmp/data", self.key), ("https://user:pass@api.deepseek.com/v1", self.key),
                          ("https://api.deepseek.com/v1?api_key=secret", self.key),
                          ("https://api.deepseek.com:invalid/v1", self.key),
                          ("https://api.deepseek.com/v1", "key\nheader = bad")):
            self.assertNotEqual(self.query(base, {}, key=key).returncode, 0)
        self.assertFalse(self.requests())

    def test_cc_switch_usage_paths_and_field_precedence(self):
        cases = (
            ({"remaining": "530.00518592", "balance": "999", "unit": "USD", "isValid": True},
             "balance=USD\t530.00518592\n"),
            ({"remaining": 0, "quota": {"remaining": 5}, "balance": 10}, "balance=USD\t0.00\n"),
            ({"remaining": None, "quota": {"remaining": "12.5", "unit": "CNY"}}, "balance=CNY\t12.50\n"),
            ({"balance": 2.75, "is_active": True}, "balance=USD\t2.75\n"),
            ({"data": {"remaining": 0, "unit": "EUR"}}, "balance=EUR\t0.00\n"),
        )
        for base in ("https://usage.test", "https://usage.test/v1", "https://usage.test/v1/",
                     "https://usage.test/v1/models"):
            for body, expected in cases:
                with self.subTest(base=base, body=body):
                    previous = len(self.requests())
                    model_url = base.rstrip('/')
                    if not model_url.endswith('/models'):
                        model_url += '/models'
                    result = self.query(base, {model_url: {"data": []}, "https://usage.test/v1/usage": body})
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("quota=available\n" + expected, result.stdout)
                    requests = self.requests()[previous:]
                    self.assertEqual([call['url'] for call in requests], [model_url, "https://usage.test/v1/usage"])
                    self.assertIn("Authorization: Bearer private-test-key", requests[-1]["headers"])

    def test_sub2api_unlimited_subscription_is_not_negative_wallet_credit(self):
        base = "https://usage.test/v1"
        responses = {base + "/models": {"data": []}, base + "/usage": {
            "isValid": True, "mode": "unrestricted", "remaining": -1, "unit": "USD", "subscription": {
                "daily_limit_usd": None, "weekly_limit_usd": None, "monthly_limit_usd": None}}}
        result = self.query(base, responses)
        self.assertEqual(result.stdout, "connection=ok\nquota=unlimited\n")
        del responses[base + "/usage"]["subscription"]
        result = self.query(base, responses)
        self.assertIn("quota=available\nbalance=USD\t-1.00\n", result.stdout)

    def test_cc_switch_usage_inactive_or_malformed_results(self):
        base = "https://usage.test/v1"
        for body in ({"remaining": 10, "isValid": False}, {"balance": 10, "is_active": False},
                     {"remaining": "NaN"}, {"quota": {"remaining": True}},
                     {"remaining": 10, "unit": "USD\nconnection=failed"}, {"data": []}):
            result = self.query(base, {base + "/models": {"data": []}, base + "/usage": body})
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "connection=ok\nquota=unavailable\n")

    def test_cc_switch_general_balance_and_legacy_billing_fallback(self):
        base = "https://gateway.test/prefix/v1"
        root = "https://gateway.test/prefix"
        result = self.query(base, {base + "/models": {"data": []},
                                  root + "/user/balance": {"balance": "6.25", "is_active": True}})
        self.assertIn("quota=available\nbalance=USD\t6.25\n", result.stdout)
        result = self.query(base, {base + "/models": {"data": []}, base + "/usage": {"unexpected": True},
                                  root + "/dashboard/billing/subscription": {"hard_limit_usd": 100},
                                  root + "/dashboard/billing/usage": {"total_usage": 125}})
        self.assertIn("quota=available\nbalance=USD\t98.75\n", result.stdout)


if __name__ == "__main__":
    unittest.main()
