#!/usr/bin/env python3
"""Query provider connectivity and remaining credit without exposing credentials."""

import json
import re
import subprocess
import sys
from decimal import Decimal, InvalidOperation
from urllib.parse import urlsplit, urlunsplit


class ProviderError(Exception):
    pass


def request_json(url, headers, timeout=12):
    # curl reads credentials from stdin, keeping keys out of process arguments.
    config = "".join('header = "' + (name + ": " + value).replace("\\", "\\\\").replace('"', '\\"') + '"\n'
                     for name, value in headers.items())
    try:
        result = subprocess.run(
            ["curl", "-fsS", "--max-time", str(timeout), "--proto", "=http,https",
             "--config", "-", "--url", url],
            input=config.encode(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout + 2,
        )
        if result.returncode or len(result.stdout) > 2 * 1024 * 1024:
            raise ProviderError()
        data = json.loads(result.stdout, parse_float=Decimal)
        if not isinstance(data, dict) or data.get("error") or data.get("success") is False:
            raise ProviderError()
        return data
    except (OSError, subprocess.TimeoutExpired, ValueError) as error:
        raise ProviderError() from error


def amount(value):
    if isinstance(value, bool) or not isinstance(value, (str, int, Decimal)):
        raise ProviderError()
    try:
        number = Decimal(value)
        if not number.is_finite() or abs(number) >= Decimal("1e16"):
            raise ProviderError()
        return number
    except InvalidOperation as error:
        raise ProviderError() from error


def balances(entries):
    result = []
    for currency, value in entries:
        if not isinstance(currency, str) or not re.fullmatch(r"[A-Z]{3}", currency):
            raise ProviderError()
        formatted = format(amount(value), ".8f").rstrip("0").rstrip(".")
        if "." not in formatted:
            formatted += ".00"
        elif len(formatted.rsplit(".", 1)[1]) == 1:
            formatted += "0"
        result.append((currency, formatted))
    if not result or len(result) > 4:
        raise ProviderError()
    return "available", result


def usage_balance(response):
    # Match cc-switch's common usage scripts, including a zero remaining balance.
    data = response.get("data", response)
    if not isinstance(data, dict):
        raise ProviderError()
    if data.get("isValid") is False or data.get("is_active") is False:
        return "unavailable", []
    quota = data.get("quota")
    if not isinstance(quota, dict):
        quota = {}
    remaining = next((value for value in (data.get("remaining"), quota.get("remaining"), data.get("balance"))
                      if value is not None), None)
    unit = next((value for value in (data.get("unit"), quota.get("unit")) if value is not None), "USD")
    remaining = amount(remaining)
    # Sub2API uses -1 only for a subscription with no configured spending limits.
    # A negative wallet balance is still an actual balance, including exactly -1.
    if remaining == -1 and data.get("mode") == "unrestricted" and isinstance(data.get("subscription"), dict):
        return "unlimited", []
    return balances([(unit, remaining)])


def compatible_quota(root, headers):
    for path in ("/v1/usage", "/user/balance"):
        try:
            return usage_balance(request_json(root + path, headers, timeout=6))
        except (ProviderError, KeyError, TypeError, ValueError):
            continue
    # Legacy OpenAI-compatible billing gateways return USD limits and usage in cents.
    subscription = request_json(root + "/dashboard/billing/subscription", headers)
    limit = amount(subscription["hard_limit_usd"])
    usage = request_json(root + "/dashboard/billing/usage", headers)
    return balances([("USD", limit - amount(usage["total_usage"]) / 100)])


def quota_status(base, host, headers, local):
    if local:
        return "unmetered", []
    if host in ("api.openai.com", "api.anthropic.com", "generativelanguage.googleapis.com"):
        return "unsupported", []
    root = base[:-3] if base.endswith("/v1") else base
    try:
        if host == "api.deepseek.com":
            data = request_json(root + "/user/balance", headers)
            return balances([(entry["currency"], entry["total_balance"]) for entry in data["balance_infos"]])
        if host == "openrouter.ai":
            data = request_json(base + "/key", headers)["data"]
            if not isinstance(data, dict):
                raise ProviderError()
            if data.get("limit_remaining") is not None:
                return balances([("USD", data["limit_remaining"])])
            credits = request_json(base + "/credits", headers)["data"]
            return balances([("USD", amount(credits["total_credits"]) - amount(credits["total_usage"]))])
        if host in ("api.siliconflow.cn", "api.siliconflow.com"):
            data = request_json(base + "/user/info", headers)["data"]
            return balances([("CNY" if host.endswith(".cn") else "USD", data["totalBalance"])])
        return compatible_quota(root, headers)
    except (ProviderError, KeyError, TypeError, ValueError):
        return "unavailable", []


def query(command, api_base, api_key):
    if any(ord(character) < 32 or ord(character) == 127 for character in api_base + api_key):
        raise ProviderError()
    parsed = urlsplit(api_base)
    if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username or parsed.password \
            or parsed.query or parsed.fragment:
        raise ProviderError()
    host = parsed.hostname.lower()
    path = parsed.path.rstrip("/")
    if path.endswith("/models"):
        path = path[:-7]
    base = urlunsplit((parsed.scheme, parsed.netloc, path, "", ""))
    headers = {"Accept": "application/json"}
    if host == "generativelanguage.googleapis.com":
        headers["x-goog-api-key"] = api_key
    elif host == "api.anthropic.com":
        headers.update({"x-api-key": api_key, "anthropic-version": "2023-06-01"})
    elif api_key:
        headers["Authorization"] = "Bearer " + api_key
    local = parsed.port == 11434
    models_url = (base[:-3] if base.endswith("/v1") else base) + "/api/tags" if local else base + "/models"
    response = request_json(models_url, headers)
    entries = response.get("data", response.get("models"))
    if not isinstance(entries, list) or any(not isinstance(entry, dict) for entry in entries):
        raise ProviderError()
    models = []
    for entry in entries:
        identifier = entry.get("id", entry.get("name"))
        if not isinstance(identifier, str) or not identifier or len(identifier) > 255 \
                or any(ord(character) < 32 or ord(character) == 127 for character in identifier):
            raise ProviderError()
        if host == "generativelanguage.googleapis.com" and identifier.startswith("models/"):
            identifier = identifier[7:]
        if identifier not in models:
            models.append(identifier)
    quota, available = quota_status(base, host, headers, local)
    print("connection=ok")
    print("quota=" + quota)
    for currency, value in available:
        print("balance=" + currency + "\t" + value)
    if command == "fetch-models":
        for identifier in models:
            print("model=" + identifier)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("test-connection", "fetch-models"):
        return 2
    try:
        query(sys.argv[1], sys.argv[2], sys.stdin.read())
    except (ProviderError, ValueError):
        print("connection=failed")
        print("quota=unavailable")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
