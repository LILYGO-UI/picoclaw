#!/usr/bin/env python3
"""Small, non-interactive iLink QR binding helper.

The PicoClaw core already implements this flow in Go, but the LILYGO UI uses
the packaged control helper rather than importing the core's Go packages.  The
two commands in this module deliberately keep the QR session in the caller:

    weixin-bind-start  (JSON object on stdin)
    weixin-bind-poll   (JSON object on stdin)

Values are emitted in a tiny quoted line protocol consumed by the C++ model;
secrets are never written to logs or included in an exception message.
"""

from __future__ import annotations

import json
import re
import sys
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlsplit, urlunsplit
from urllib.request import HTTPRedirectHandler, ProxyHandler, Request, build_opener


DEFAULT_BASE_URL = "https://ilinkai.weixin.qq.com/"
BOT_TYPE = "3"
APP_ID = "bot"
CLIENT_VERSION = "131329"
MAX_BODY = 256 * 1024
MAX_VALUE = 4096
ALLOWED_STATUS = {"wait", "scaned", "confirmed", "expired", "scaned_but_redirect"}


class BindingError(Exception):
    """An error safe to return to the UI (never contains a response body)."""


class TransientBindingError(BindingError):
    """A network timeout/error for which polling should retain its state."""


def _json_stdin() -> dict[str, Any]:
    try:
        raw = sys.stdin.read(MAX_BODY + 1)
        if len(raw.encode("utf-8")) > MAX_BODY:
            raise BindingError("binding request is too large")
        value = json.loads(raw)
    except (OSError, ValueError) as exc:
        raise BindingError("invalid binding request") from exc
    if not isinstance(value, dict) or len(value) > 8:
        raise BindingError("invalid binding request")
    return value


def _string(value: Any, *, required: bool = False, name: str = "value") -> str:
    if value is None:
        value = ""
    if not isinstance(value, str) or len(value.encode("utf-8")) > MAX_VALUE:
        raise BindingError(f"invalid {name}")
    if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in value):
        raise BindingError(f"invalid {name}")
    if required and not value.strip():
        raise BindingError(f"missing {name}")
    return value


def _base_url(value: Any) -> str:
    raw = _string(value, name="base URL").strip() or DEFAULT_BASE_URL
    parsed = urlsplit(raw)
    # The iLink API is HTTPS-only.  Reject credentials, query fragments,
    # and non-default ports so a malformed redirect cannot become an SSRF.
    if parsed.scheme.lower() != "https" or parsed.username or parsed.password:
        raise BindingError("invalid base URL")
    try:
        host = parsed.hostname
        port = parsed.port
    except ValueError as exc:
        raise BindingError("invalid base URL") from exc
    if not host or port not in (None, 443) or parsed.query or parsed.fragment:
        raise BindingError("invalid base URL")
    host = host.lower().rstrip(".")
    if host != "weixin.qq.com" and not host.endswith(".weixin.qq.com"):
        raise BindingError("invalid base URL")
    path = parsed.path or "/"
    if not path.endswith("/"):
        path += "/"
    return urlunsplit(("https", host, path, "", ""))


def _proxy(value: Any) -> str:
    raw = _string(value, name="proxy").strip()
    if not raw:
        return ""
    parsed = urlsplit(raw)
    if parsed.scheme.lower() not in ("http", "https"):
        raise BindingError("invalid proxy")
    if not parsed.hostname or parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise BindingError("invalid proxy")
    return raw


def _redirect_base(host_value: Any) -> str:
    host = _string(host_value, required=True, name="redirect host").strip().rstrip(".").lower()
    # Upstream returns a host (not a URL).  Only Tencent's Weixin domains are
    # accepted, and urlsplit rejects ports, userinfo, paths, and control data.
    if not re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)*", host):
        raise BindingError("invalid redirect host")
    if host != "weixin.qq.com" and not host.endswith(".weixin.qq.com"):
        raise BindingError("invalid redirect host")
    return f"https://{host}/"


def _opener(proxy: str):
    # urllib's ProxyHandler supports HTTP(S) proxies.  iLink's documented
    # setting is HTTP; reject other schemes above rather than silently ignore.
    class NoRedirectHandler(HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            raise URLError("redirect rejected")

    return build_opener(ProxyHandler({"http": proxy, "https": proxy} if proxy else {}), NoRedirectHandler())


def _get(base_url: str, endpoint: str, query: dict[str, str], proxy: str) -> dict[str, Any]:
    url = base_url.rstrip("/") + "/" + endpoint.lstrip("/")
    if query:
        url += "?" + urlencode(query)
    request = Request(
        url,
        method="GET",
        headers={"iLink-App-Id": APP_ID, "iLink-App-ClientVersion": CLIENT_VERSION},
    )
    try:
        with _opener(proxy).open(request, timeout=8) as response:
            data = response.read(MAX_BODY + 1)
    except (HTTPError, URLError, TimeoutError, OSError) as exc:
        # Do not include HTTP response bodies: they may echo QR tokens.
        raise TransientBindingError("Weixin service unavailable") from exc
    if len(data) > MAX_BODY:
        raise BindingError("Weixin response is too large")
    try:
        decoded = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise BindingError("invalid Weixin response") from exc
    if not isinstance(decoded, dict):
        raise BindingError("invalid Weixin response")
    return decoded


def _quote(value: str) -> str:
    # std::quoted handles this exact escaping and does not interpret JSON's
    # \\uXXXX sequences.  Values are prevalidated to contain no controls.
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _emit(values: dict[str, str]) -> None:
    print("PICOCLAW_WEIXIN_BINDING 1")
    for key, value in values.items():
        if not re.fullmatch(r"[a-z_]+", key):
            continue
        print(f"{key} {_quote(value)}")
    print("END_PICOCLAW_WEIXIN_BINDING")


def _start(req: dict[str, Any]) -> None:
    base = _base_url(req.get("base_url"))
    proxy = _proxy(req.get("proxy"))
    response = _get(base, "ilink/bot/get_bot_qrcode", {"bot_type": BOT_TYPE}, proxy)
    qrcode = _string(response.get("qrcode"), required=True, name="qrcode")
    qr_url = _string(response.get("qrcode_img_content"), required=True, name="QR URL")
    _emit({"status": "wait", "qrcode": qrcode, "qr_url": qr_url, "base_url": base})


def _poll(req: dict[str, Any]) -> None:
    base = _base_url(req.get("base_url"))
    proxy = _proxy(req.get("proxy"))
    qrcode = _string(req.get("qrcode"), required=True, name="qrcode")
    try:
        response = _get(base, "ilink/bot/get_qrcode_status", {"qrcode": qrcode}, proxy)
    except TransientBindingError:
        # A timed out/temporarily unavailable poll is normal while a QR is
        # waiting. Keep the caller's current state and try again later.
        _emit({"status": "wait", "base_url": base})
        return
    status = _string(response.get("status"), required=True, name="status")
    if status not in ALLOWED_STATUS:
        raise BindingError("unknown Weixin binding status")
    values: dict[str, str] = {"status": status, "base_url": base}
    if status == "scaned_but_redirect":
        values["base_url"] = _redirect_base(response.get("redirect_host"))
        values["redirect_host"] = _string(response.get("redirect_host"), required=True, name="redirect host")
    if status == "confirmed":
        token = _string(response.get("bot_token"), required=True, name="bot token")
        account = _string(response.get("ilink_bot_id"), required=True, name="account ID")
        values["token"] = token
        values["account_id"] = account
        values["user_id"] = _string(response.get("ilink_user_id"), name="user ID")
        returned_base = _string(response.get("baseurl"), name="base URL").strip()
        if returned_base:
            values["base_url"] = _base_url(returned_base)
    _emit(values)


def main(argv: list[str]) -> int:
    if len(argv) != 2 or argv[1] not in ("weixin-bind-start", "weixin-bind-poll"):
        print("usage: picoclaw-weixin.py {weixin-bind-start|weixin-bind-poll}", file=sys.stderr)
        return 64
    try:
        request = _json_stdin()
        if argv[1] == "weixin-bind-start":
            _start(request)
        else:
            _poll(request)
        return 0
    except BindingError as exc:
        print(f"ERROR: {exc}")
        return 1
    except Exception:
        # Keep parser/URL implementation details (and any accidental input
        # echo from a platform error) out of the UI and activity log.
        print("ERROR: Weixin binding failed")
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
