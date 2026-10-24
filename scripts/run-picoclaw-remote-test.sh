#!/bin/sh
set -eu

# Run the desktop SDL host-simulator against the remote PicoClaw gateway.

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=${PICOCLAW_SIMULATOR_BIN:-"$repo_dir/build/host-simulator/lilygo-ui-picoclaw"}
if [ ! -x "$binary" ]; then
    echo "SDL simulator not found: $binary" >&2
    echo "Build it with: cmake --build --preset host-simulator --parallel" >&2
    exit 1
fi

config_home=${XDG_CONFIG_HOME:-${HOME}/.config}
source_settings=${PICOCLAW_UI_SETTINGS:-"$config_home/lilygo/ui/picoclaw/ui-settings.conf"}
temp_root=${TMPDIR:-/tmp}
temp_dir=$(mktemp -d "$temp_root/picoclaw-remote.XXXXXX")
settings="$temp_dir/ui-settings.conf"
trap 'rm -rf "$temp_dir"' EXIT INT TERM

if [ -f "$source_settings" ]; then
    cp "$source_settings" "$settings"
else
    : > "$settings"
fi

SETTINGS_PATH="$settings" python3 - <<'PY'
from pathlib import Path
import os

path = Path(os.environ["SETTINGS_PATH"])
lines = path.read_text().splitlines() if path.exists() else []
values = {
    "host": "192.168.8.197",
    "port": "18790",
}
seen = set()
updated = []
for line in lines:
    key, separator, _ = line.partition("=")
    if separator and key in values:
        updated.append(f"{key}={values[key]}")
        seen.add(key)
    else:
        updated.append(line)
for key, value in values.items():
    if key not in seen:
        updated.append(f"{key}={value}")
path.write_text("\n".join(updated) + "\n")
PY

PICOCLAW_TRANSPORT=websocket \
PICOCLAW_UI_SETTINGS="$settings" \
LILYGO_PICOCLAW_CONTROL="$repo_dir/scripts/picoclaw-control.sh" \
PICOCLAW_GATEWAY_TOKEN="${PICOCLAW_GATEWAY_TOKEN:-picoclaw-android-local}" \
"$binary"
