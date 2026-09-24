#!/usr/bin/env python3
"""Offline tests for the iLink QR binding helper.

No request is made to Tencent.  The HTTP boundary is replaced with fixtures so
the tests cover wire shaping, redirect validation, and transient polling.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import unittest
from unittest import mock
from urllib.error import URLError


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("picoclaw_weixin", ROOT / "scripts" / "picoclaw-weixin.py")
assert SPEC and SPEC.loader
wx = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wx)


class WeixinBindingHelperTests(unittest.TestCase):
    def run_command(self, command: str, payload: dict, response=None):
        output = io.StringIO()
        with mock.patch.object(wx, "_get", return_value=response), contextlib.redirect_stdout(output), mock.patch.object(
            wx.sys, "stdin", io.StringIO(json.dumps(payload))
        ):
            code = wx.main(["picoclaw-weixin.py", command])
        return code, output.getvalue()

    def test_start_shapes_qr_response_without_credentials(self):
        code, output = self.run_command(
            "weixin-bind-start",
            {"proxy": ""},
            {"qrcode": "qr-token", "qrcode_img_content": "https://weixin.qq.com/scan?x=1"},
        )
        self.assertEqual(code, 0)
        self.assertIn('status "wait"', output)
        self.assertIn('qrcode "qr-token"', output)
        self.assertIn('qr_url "https://weixin.qq.com/scan?x=1"', output)
        self.assertNotIn("ERROR", output)

    def test_confirmed_response_contains_token_for_draft_only(self):
        code, output = self.run_command(
            "weixin-bind-poll",
            {"qrcode": "qr-token"},
            {
                "status": "confirmed",
                "bot_token": "bot-secret",
                "ilink_bot_id": "account-1",
                "ilink_user_id": "user-1",
                "baseurl": "https://ilinkai.weixin.qq.com/",
            },
        )
        self.assertEqual(code, 0)
        self.assertIn('status "confirmed"', output)
        self.assertIn('token "bot-secret"', output)
        self.assertIn('account_id "account-1"', output)

    def test_confirmed_response_preserves_region_specific_runtime_endpoint(self):
        # QR polling may be redirected independently of the messaging API.
        # The account must be saved with the server's confirmed baseurl.
        code, output = self.run_command(
            "weixin-bind-poll",
            {"qrcode": "qr-token", "base_url": "https://poll.weixin.qq.com/"},
            {
                "status": "confirmed",
                "bot_token": "region-bot-secret",
                "ilink_bot_id": "region-account",
                "baseurl": "https://region.weixin.qq.com/",
            },
        )
        self.assertEqual(code, 0)
        self.assertIn('base_url "https://region.weixin.qq.com/"', output)
        self.assertNotIn("poll.weixin.qq.com", output)

    def test_confirmed_response_without_baseurl_keeps_current_poll_endpoint(self):
        code, output = self.run_command(
            "weixin-bind-poll",
            {"qrcode": "qr-token", "base_url": "https://region.weixin.qq.com/"},
            {"status": "confirmed", "bot_token": "bot-secret", "ilink_bot_id": "account-1"},
        )
        self.assertEqual(code, 0)
        self.assertIn('base_url "https://region.weixin.qq.com/"', output)

    def test_redirect_host_switches_only_to_official_domain(self):
        code, output = self.run_command(
            "weixin-bind-poll",
            {"qrcode": "qr-token"},
            {"status": "scaned_but_redirect", "redirect_host": "ilink2.weixin.qq.com"},
        )
        self.assertEqual(code, 0)
        self.assertIn('status "scaned_but_redirect"', output)
        self.assertIn('base_url "https://ilink2.weixin.qq.com/"', output)

        code, output = self.run_command(
            "weixin-bind-poll",
            {"qrcode": "qr-token"},
            {"status": "scaned_but_redirect", "redirect_host": "evil.example"},
        )
        self.assertNotEqual(code, 0)
        self.assertNotIn("evil.example", output)

    def test_network_timeout_is_a_waiting_result(self):
        output = io.StringIO()
        with mock.patch.object(wx, "_get", side_effect=wx.TransientBindingError("network")), contextlib.redirect_stdout(output), mock.patch.object(
            wx.sys, "stdin", io.StringIO(json.dumps({"qrcode": "qr-token"}))
        ):
            code = wx.main(["picoclaw-weixin.py", "weixin-bind-poll"])
        self.assertEqual(code, 0)
        self.assertIn('status "wait"', output.getvalue())

    def test_base_url_proxy_and_request_limits_reject_unsafe_values(self):
        for value in ("http://ilinkai.weixin.qq.com/", "https://evil.example/", "https://ilinkai.weixin.qq.com:444/"):
            with self.subTest(value=value):
                with self.assertRaises(wx.BindingError):
                    wx._base_url(value)
        with self.assertRaises(wx.BindingError):
            wx._proxy("socks5://127.0.0.1:1080")
        with self.assertRaises(wx.BindingError):
            wx._redirect_base("ilinkai.weixin.qq.com:443")

    def test_get_uses_official_headers_escaped_query_and_eight_second_timeout(self):
        seen = {}

        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

            def read(self, limit):
                seen["limit"] = limit
                return b'{"status":"wait"}'

        class Opener:
            def open(self, request, timeout):
                seen["request"] = request
                seen["timeout"] = timeout
                return Response()

        with mock.patch.object(wx, "_opener", return_value=Opener()):
            result = wx._get(
                "https://ilinkai.weixin.qq.com/",
                "ilink/bot/get_qrcode_status",
                {"qrcode": "a/b?x&y"},
                "",
            )
        self.assertEqual(result["status"], "wait")
        self.assertEqual(seen["timeout"], 8)
        request = seen["request"]
        self.assertIn("qrcode=a%2Fb%3Fx%26y", request.full_url)
        self.assertEqual(request.get_header("Ilink-app-id"), "bot")
        self.assertEqual(request.get_header("Ilink-app-clientversion"), "131329")
        self.assertIsNone(request.get_header("Authorization"))
        self.assertIsNone(request.get_header("X-wechat-uin"))

    def test_get_rejects_oversized_and_malformed_responses_without_echoing_body(self):
        class Response:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

            def read(self, limit):
                return b"private-token" + b"x" * (wx.MAX_BODY + 1)

        class Opener:
            def open(self, request, timeout):
                return Response()

        with mock.patch.object(wx, "_opener", return_value=Opener()):
            with self.assertRaises(wx.BindingError) as raised:
                wx._get("https://ilinkai.weixin.qq.com/", "ilink/bot/get_qrcode_status", {"qrcode": "q"}, "")
        self.assertNotIn("private-token", str(raised.exception))

        class BadResponse(Response):
            def read(self, limit):
                return b'{"bot_token":"private-token"'

        class BadOpener(Opener):
            def open(self, request, timeout):
                return BadResponse()

        with mock.patch.object(wx, "_opener", return_value=BadOpener()):
            with self.assertRaises(wx.BindingError) as raised:
                wx._get("https://ilinkai.weixin.qq.com/", "ilink/bot/get_qrcode_status", {"qrcode": "q"}, "")
        self.assertNotIn("private-token", str(raised.exception))

    def test_redirect_handler_never_follows_a_location(self):
        opener = wx._opener("")
        handler = next(item for item in opener.handlers if isinstance(item, wx.HTTPRedirectHandler))
        with self.assertRaises(URLError):
            handler.redirect_request(None, None, 302, "Found", {}, "https://evil.example/leak")

    def test_malformed_and_oversized_requests_are_sanitized(self):
        for payload in ("{", "x" * (wx.MAX_BODY + 1)):
            output = io.StringIO()
            with contextlib.redirect_stdout(output), mock.patch.object(wx.sys, "stdin", io.StringIO(payload)):
                code = wx.main(["picoclaw-weixin.py", "weixin-bind-start"])
            self.assertNotEqual(code, 0)
            self.assertNotIn("private", output.getvalue())


if __name__ == "__main__":
    unittest.main()
