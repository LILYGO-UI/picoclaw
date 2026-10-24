#!/usr/bin/env python3
"""Edit PicoClaw v3 channels without exposing credentials to argv or logs.

Schema: sipeed/picoclaw v0.3.1 pkg/config/config_channel.go and config.go.
Channel common fields live in channel_list.<id>; provider fields in settings.
SecureString fields live in .security.yml/channel_list/<id>/settings.
Only the selected YAML scalar spans are edited, preserving other credentials
and comments without requiring PyYAML on the board. Unsupported YAML layouts
fail closed instead of rewriting or guessing at private configuration.
"""

import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from urllib.parse import urlsplit


LIMIT = 1024 * 1024
# Each field: kind, default, required when enabled. All UI values are strings.
SCHEMA = {
    "telegram": {"token": ("secret", "", True), "base_url": ("http", "", False),
                 "proxy": ("proxy", "", False)},
    "discord": {"token": ("secret", "", True), "proxy": ("proxy", "", False),
                "mention_only": ("bool", "false", False)},
    "feishu": {"app_id": ("text", "", True), "app_secret": ("secret", "", True),
               "encrypt_key": ("secret", "", False), "verification_token": ("secret", "", False),
               "is_lark": ("bool", "false", False)},
    "weixin": {"token": ("secret", "", True), "account_id": ("text", "", False),
               "base_url": ("http", "https://ilinkai.weixin.qq.com/", False),
               "cdn_base_url": ("http", "https://novac2c.cdn.weixin.qq.com/c2c", False),
               "proxy": ("proxy", "", False)},
    "slack": {"bot_token": ("secret", "", True), "app_token": ("secret", "", True)},
    "dingtalk": {"client_id": ("text", "", True), "client_secret": ("secret", "", True)},
    "qq": {"app_id": ("text", "", True), "app_secret": ("secret", "", True)},
    "whatsapp": {"bridge_url": ("ws", "ws://localhost:3001", True)},
    "line": {"channel_secret": ("secret", "", True), "channel_access_token": ("secret", "", True),
             "webhook_host": ("text", "0.0.0.0", False), "webhook_port": ("port", "18791", False),
             "webhook_path": ("path", "/webhook/line", False)},
    "onebot": {"ws_url": ("ws", "ws://127.0.0.1:3001", True), "access_token": ("secret", "", False)},
}
for _fields in SCHEMA.values():
    _fields["allow_from"] = ("list", "", False)


def read_bytes(path):
    with path.open("rb") as source:
        data = source.read(LIMIT + 1)
    if len(data) > LIMIT:
        raise ValueError("Configuration exceeds the size limit")
    return data


def object_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate configuration key")
        result[key] = value
    return result


def json_object(data):
    value = json.loads(data, object_pairs_hook=object_pairs)
    if not isinstance(value, dict):
        raise ValueError("Expected an object")
    return value


def single_line(value):
    if not isinstance(value, str) or len(value.encode("utf-8")) > 4096 or any(ord(c) < 32 for c in value):
        raise ValueError("Expected a single line value")
    return value


def yaml_scalar(value):
    """Read the simple scalar form emitted by PicoClaw's yaml.v3 writer."""
    value = value.strip()
    if not value or value.startswith("#"):
        return ""
    if value.startswith('"'):
        result, offset = json.JSONDecoder().raw_decode(value)
        tail = value[offset:].strip()
        if not isinstance(result, str) or (tail and not tail.startswith("#")):
            raise ValueError("Unsupported YAML string")
        return single_line(result)
    if value.startswith("'"):
        match = re.fullmatch(r"'((?:[^']|'')*)'\s*(?:#.*)?", value)
        if not match:
            raise ValueError("Unsupported YAML string")
        return single_line(match[1].replace("''", "'"))
    if value[0] in "[{&*!|>@`%" or re.search(r":\s", value):
        raise ValueError("Unsupported YAML scalar")
    return single_line(re.split(r"\s+#", value, maxsplit=1)[0].rstrip())


class SecurityDocument:
    """A span editor for block mappings; unrelated subtrees stay byte-for-byte."""

    def __init__(self, data=b""):
        if data.strip() == b"{}":
            data = b""
        self.lines = data.decode("utf-8").splitlines(keepends=True)
        if self.lines and not self.lines[-1].endswith("\n"):
            self.lines[-1] += "\n"
        self.children(0, len(self.lines), -1)

    @staticmethod
    def header(line):
        match = re.fullmatch(r"( *)(\"(?:[^\"\\]|\\.)*\"|'(?:[^']|'')*'|[^\s#][^\n]*?):(?:[ \t]+(.*)|[ \t]*)\n?", line)
        if not match:
            raise ValueError("Unsupported YAML mapping")
        key = yaml_scalar(match[2])
        if not key:
            raise ValueError("Empty YAML key")
        return len(match[1]), key, (match[3] or "").strip()

    def children(self, start, end, parent_indent):
        entries = []
        indent = None
        for index in range(start, end):
            line = self.lines[index]
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if parent_indent == -1 and line.strip() in ("---", "..."):
                # Appending after an explicit document terminator is unsafe.
                if line.strip() == "...":
                    raise ValueError("Explicit YAML document end is unsupported")
                if any(x.strip() and not x.lstrip().startswith("#") for x in self.lines[:index]):
                    raise ValueError("Multiple YAML documents are unsupported")
                continue
            if "\t" in line[:len(line) - len(line.lstrip())]:
                raise ValueError("Invalid YAML indentation")
            current = len(line) - len(line.lstrip(" "))
            if indent is None:
                indent = current
                if indent <= parent_indent or (parent_indent == -1 and indent != 0):
                    raise ValueError("Invalid YAML indentation")
            if current < indent:
                raise ValueError("Invalid YAML indentation")
            if current != indent:
                continue
            node_indent, key, value = self.header(line)
            if any(node[1] == key for node in entries):
                raise ValueError("Duplicate YAML mapping key")
            entries.append((index, key, value, node_indent))
        return [(index, key, value, node_indent,
                 entries[n + 1][0] if n + 1 < len(entries) else end)
                for n, (index, key, value, node_indent) in enumerate(entries)]

    def node(self, path, create=False):
        start, end, parent_indent = 0, len(self.lines), -1
        for depth, key in enumerate(path):
            entries = self.children(start, end, parent_indent)
            found = next((entry for entry in entries if entry[1] == key), None)
            if found is None:
                if not create:
                    return None
                indent = entries[0][3] if entries else (0 if parent_indent < 0 else parent_indent + 2)
                self.lines.insert(end, " " * indent + json.dumps(key, ensure_ascii=False) + ":\n")
                found = (end, key, "", indent, end + 1)
            index, _, value, indent, end = found
            if depth == len(path) - 1:
                return found
            if value and not value.startswith("#"):
                if re.fullmatch(r"\{\}\s*(?:#.*)?", value):
                    if create:
                        self.lines[index] = " " * indent + json.dumps(key, ensure_ascii=False) + ":\n"
                    else:
                        return None
                else:
                    raise ValueError("Unsupported YAML mapping layout")
            start, parent_indent = index + 1, indent
        return None

    def scalar(self, path):
        found = self.node(path)
        if found is None:
            return None
        index, _, value, _, end = found
        if any(line.strip() and not line.lstrip().startswith("#") for line in self.lines[index + 1:end]):
            raise ValueError("Unsupported multiline secret")
        return yaml_scalar(value)

    def set_scalar(self, path, value):
        index, key, _, indent, end = self.node(path, create=True)
        self.lines[index:end] = [" " * indent + json.dumps(key, ensure_ascii=False) + ": " +
                                 json.dumps(value, ensure_ascii=False) + "\n"]

    def bytes(self):
        return "".join(self.lines).encode("utf-8")

    def channel_root(self):
        roots = [key for _, key, _, _, _ in self.children(0, len(self.lines), -1)
                 if key in ("channel_list", "channels")]
        if len(roots) > 1:
            raise ValueError("Ambiguous channel security sections")
        return roots[0] if roots else "channel_list"


def runtime_paths():
    user_home = Path.home()
    pico_home = Path(os.environ.get("PICOCLAW_HOME", str(user_home / ".local/share/lilygo/ui/picoclaw")))
    config_dir = Path(os.environ.get("PICOCLAW_CONFIG_DIR", str(user_home / ".config/lilygo/ui/picoclaw")))
    config = Path(os.environ.get("PICOCLAW_CONFIG", str(config_dir / "config.json")))
    return pico_home, config, config.with_name(".security.yml")


def read_runtime(config_path, security_path, pico_home):
    config = json_object(read_bytes(config_path)) if config_path.exists() else {}
    channels = config.get("channel_list", {})
    if not isinstance(channels, dict):
        raise ValueError("Invalid channel list")
    # Refrain from silently migrating unrelated v1/v2 model/tool settings.
    if config and config.get("version", 3) != 3:
        raise ValueError("PicoClaw v3 configuration is required")
    if "channels" in config and "channel_list" not in config:
        raise ValueError("Legacy channel configuration requires runtime migration")
    source = security_path if security_path.exists() else pico_home / ".security.yml"
    security = SecurityDocument(read_bytes(source) if source.exists() else b"")
    return config, channels, security


def channel_values(channel_id, channels, security):
    config = channels.get(channel_id, {})
    if not isinstance(config, dict) or config.get("type", channel_id) != channel_id:
        raise ValueError("Conflicting channel type")
    settings = config.get("settings", {}) or {}
    if not isinstance(settings, dict):
        raise ValueError("Invalid channel settings")
    enabled = config.get("enabled", False)
    if not isinstance(enabled, bool):
        raise ValueError("Invalid channel state")
    values = {}
    for key, (kind, default, _) in SCHEMA[channel_id].items():
        value = (config if key == "allow_from" else settings).get(key, config.get(key, default))
        if kind == "secret":
            secret = security.scalar([security.channel_root(), channel_id, "settings", key])
            if secret is None:
                secret = security.scalar([security.channel_root(), channel_id, key])
            if secret is not None:
                value = secret
        if value is None:
            value = default
        if kind == "bool":
            if not isinstance(value, bool) and value not in ("true", "false"):
                raise ValueError("Invalid boolean")
            value = "true" if value is True or value == "true" else "false"
        elif kind == "list":
            if isinstance(value, list):
                if any(not isinstance(item, (str, int)) or isinstance(item, bool) for item in value):
                    raise ValueError("Invalid allow list item")
                value = ",".join(single_line(str(item)) for item in value)
            elif not isinstance(value, str):
                raise ValueError("Invalid allow list")
        elif kind == "port" and isinstance(value, int) and not isinstance(value, bool):
            value = str(value)
        values[key] = single_line(value)
    return enabled, values


def quote(value):
    # std::quoted escapes quotes and backslashes, not JSON Unicode sequences.
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def load_channels(config_path, security_path, pico_home):
    _, channels, security = read_runtime(config_path, security_path, pico_home)
    output = [f"PICOCLAW_CHANNELS 1 {len(SCHEMA)}"]
    for channel_id in SCHEMA:
        enabled, values = channel_values(channel_id, channels, security)
        output.append(f"{quote(channel_id)} {int(enabled)} {len(values)}")
        output.extend(f"{quote(key)} {quote(value)}" for key, value in values.items())
    output.append("END_CHANNELS")
    return "\n".join(output) + "\n"


def field_value(kind, value):
    value = single_line(value)
    if kind == "bool":
        if value not in ("true", "false"):
            raise ValueError("Invalid boolean value")
        return value == "true"
    if kind == "port":
        if not value.isdigit() or not 1 <= int(value) <= 65535:
            raise ValueError("Invalid webhook port")
        return int(value)
    if kind == "list":
        return list(dict.fromkeys(item.strip() for item in value.split(",") if item.strip()))
    if kind in ("http", "ws", "proxy") and value:
        address = urlsplit(value)
        schemes = {"http", "https"} if kind == "http" else {"ws", "wss"} if kind == "ws" else {
            "http", "https", "socks5", "socks5h"}
        if address.scheme not in schemes or not address.hostname or any(c.isspace() for c in value):
            raise ValueError("Invalid channel URL")
        address.port  # Reject malformed or out-of-range URL ports too.
    if kind == "path" and value and not value.startswith("/"):
        raise ValueError("Invalid webhook path")
    return value


def stage(path, data, temporary):
    descriptor, name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    temporary.append(name)
    with os.fdopen(descriptor, "wb") as output:
        os.fchmod(output.fileno(), 0o600)
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    return name


def commit(config_path, config_data, security_path, security_data):
    temporary = []
    try:
        config_stage = stage(config_path, config_data, temporary)
        original_security = read_bytes(security_path) if security_path.exists() else None
        security_stage = stage(security_path, security_data, temporary)
        backup = stage(security_path, original_security, temporary) if original_security is not None else None
        os.replace(security_stage, security_path)
        try:
            os.replace(config_stage, config_path)
        except OSError:
            if backup is not None:
                os.replace(backup, security_path)
            else:
                security_path.unlink()
            raise
    finally:
        for name in temporary:
            with contextlib.suppress(FileNotFoundError):
                os.unlink(name)


def save_channel(channel_id, payload, config_path, security_path, pico_home):
    if channel_id not in SCHEMA:
        raise ValueError("Unknown channel")
    if set(payload) != {"enabled", "fields"} or not isinstance(payload["enabled"], bool):
        raise ValueError("Invalid channel payload")
    fields = payload["fields"]
    if not isinstance(fields, dict) or not set(fields) <= set(SCHEMA[channel_id]):
        raise ValueError("Unknown channel field")
    config, channels, security = read_runtime(config_path, security_path, pico_home)
    _, values = channel_values(channel_id, channels, security)
    values.update(fields)
    normalized = {}
    for key, value in values.items():
        kind, _, required = SCHEMA[channel_id][key]
        normalized[key] = field_value(kind, value)
        if payload["enabled"] and required and not value.strip():
            raise ValueError("A required channel credential or address is missing")
    if not config:
        config = {"version": 3, "agents": {"defaults": {
            "workspace": str(pico_home / "workspace"), "restrict_to_workspace": True,
            "max_tokens": 2048, "context_window": 32768, "temperature": 0.7, "max_tool_iterations": 20}},
            "gateway": {"host": "0.0.0.0", "port": int(os.environ.get("PICOCLAW_GATEWAY_PORT", "18790")),
                        "log_level": "warn"}, "model_list": [], "channel_list": {}}
    channels = config.setdefault("channel_list", {})
    channel = channels.setdefault(channel_id, {})
    channel.update(type=channel_id, enabled=payload["enabled"])
    settings = channel.setdefault("settings", {})
    if settings is None:
        settings = channel["settings"] = {}
    for key, value in normalized.items():
        kind = SCHEMA[channel_id][key][0]
        if key == "allow_from":
            channel[key] = value
        elif kind == "secret":
            # Do not retain plaintext copies of the selected channel's secrets.
            settings.pop(key, None)
            channel.pop(key, None)
            security.set_scalar([security.channel_root(), channel_id, "settings", key], value)
            # A flat legacy scalar would otherwise remain as a second secret.
            legacy = security.node([security.channel_root(), channel_id, key])
            if legacy:
                del security.lines[legacy[0]:legacy[4]]
        else:
            settings[key] = value
            channel.pop(key, None)
    config_data = (json.dumps(config, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    config_path.parent.mkdir(parents=True, exist_ok=True)
    commit(config_path, config_data, security_path, security.bytes())


@contextlib.contextmanager
def runtime_lock(pico_home, write):
    # Linux control.sh already holds the updater's lock around mutations.
    if os.environ.get("PICOCLAW_UPDATE_INTERNAL") == "1":
        yield
        return
    lock_path = pico_home / ".update.lock"
    if write:
        pico_home.mkdir(parents=True, exist_ok=True)
    if not write and not lock_path.exists():
        yield
        return
    descriptor = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600) if write else os.open(lock_path, os.O_RDONLY)
    try:
        fcntl.flock(descriptor, (fcntl.LOCK_EX if write else fcntl.LOCK_SH) | fcntl.LOCK_NB)
        yield
    finally:
        os.close(descriptor)


def main(argv):
    try:
        pico_home, config_path, security_path = runtime_paths()
        if argv == ["load-channels"]:
            with runtime_lock(pico_home, False):
                output = load_channels(config_path, security_path, pico_home)
            sys.stdout.write(output)
        elif len(argv) == 2 and argv[0] == "save-channel":
            data = sys.stdin.buffer.read(LIMIT + 1)
            if len(data) > LIMIT:
                raise ValueError("Payload exceeds the size limit")
            payload = json_object(data)
            with runtime_lock(pico_home, True):
                save_channel(argv[1], payload, config_path, security_path, pico_home)
            print("OK: channel configuration saved. Restart the gateway to apply changes.")
        else:
            raise ValueError("Unsupported channel command")
    except BlockingIOError:
        print("ERROR: a PicoClaw update or gateway operation is already in progress", file=sys.stderr)
        return 75
    except (OSError, ValueError, TypeError, KeyError, UnicodeError, RecursionError):
        # Parse errors may contain tokens or input fragments. Never print them.
        print("ERROR: could not read or save channels. Check fields, configuration files and storage.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
