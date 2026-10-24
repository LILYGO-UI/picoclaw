#!/bin/sh

set -u

PICO_USER_HOME="${HOME:-/tmp}"
PICO_DATA_HOME="${XDG_DATA_HOME:-${PICO_USER_HOME}/.local/share}"
PICO_CONFIG_HOME="${XDG_CONFIG_HOME:-${PICO_USER_HOME}/.config}"
PICO_STATE_HOME="${XDG_STATE_HOME:-${PICO_USER_HOME}/.local/state}"
PICO_HOME="${PICOCLAW_HOME:-${PICO_DATA_HOME}/lilygo/ui/picoclaw}"
PICO_CONFIG_DIR="${PICOCLAW_CONFIG_DIR:-${PICO_CONFIG_HOME}/lilygo/ui/picoclaw}"
PICO_STATE_DIR="${PICOCLAW_STATE_DIR:-${PICO_STATE_HOME}/lilygo/ui/picoclaw}"
PICO_MANAGED_BIN="${PICO_HOME}/bin/picoclaw"
PICO_ACTIVE_BIN="${PICO_HOME}/bin/active-picoclaw"
# Debian installs the runtime beside this helper. Resolve it independently of
# the working directory and prefer package updates over old user downloads.
PICO_CONTROL_DIR="$(CDPATH= cd -P "$(dirname "$0")" && pwd)"
PICO_CONTROL_PATH="${PICO_CONTROL_DIR}/$(basename "$0")"
PICO_BUNDLED_BIN="${PICO_CONTROL_DIR}/picoclaw"
PICO_EXPLICIT_BIN=no
if [ -n "${PICOCLAW_BIN:-}" ]; then
    PICO_BIN="${PICOCLAW_BIN}"
    PICO_EXPLICIT_BIN=yes
elif [ -x "${PICO_ACTIVE_BIN}" ]; then
    PICO_BIN="${PICO_ACTIVE_BIN}"
elif [ -x "${PICO_BUNDLED_BIN}" ]; then
    PICO_BIN="${PICO_BUNDLED_BIN}"
elif [ -x "${PICO_MANAGED_BIN}" ]; then
    PICO_BIN="${PICO_MANAGED_BIN}"
elif PICO_PATH_BIN="$(command -v picoclaw 2>/dev/null)" && [ -x "${PICO_PATH_BIN}" ]; then
    PICO_BIN="${PICO_PATH_BIN}"
elif [ -x /usr/bin/picoclaw ]; then
    PICO_BIN=/usr/bin/picoclaw
else
    PICO_BIN="${PICO_MANAGED_BIN}"
fi
PICO_CONFIG="${PICOCLAW_CONFIG:-${PICO_CONFIG_DIR}/config.json}"
# PicoClaw resolves secrets relative to config.json, including custom paths.
PICO_SECURITY="$(dirname "${PICO_CONFIG}")/.security.yml"
PICO_LEGACY_SECURITY="${PICO_HOME}/.security.yml"
PICO_WORKSPACE="${PICO_HOME}/workspace"
PICO_LOG="${PICO_STATE_DIR}/activity.log"
PICO_GATEWAY_LOG="${PICO_STATE_DIR}/gateway.log"
PICO_GATEWAY_PID="${PICO_STATE_DIR}/gateway.pid"
PICO_GATEWAY_PORT="${PICOCLAW_GATEWAY_PORT:-18790}"
PICO_WEB_PORT="${PICOCLAW_WEB_PORT:-18800}"
PICO_UI_SETTINGS="${PICOCLAW_UI_SETTINGS:-${PICO_CONFIG_DIR}/ui-settings.conf}"
PICO_RELEASE_TAG="${PICOCLAW_RELEASE_TAG:-v0.3.1}"
PICO_DOCKER_IMAGE="${PICOCLAW_DOCKER_IMAGE:-docker.io/sipeed/picoclaw:latest}"
PICO_DOCKER_CONTAINER="${PICOCLAW_DOCKER_CONTAINER:-lilygo-picoclaw-gateway}"
# The Linux board runs PicoClaw directly. Do not let an environment copied
# from the macOS simulator redirect board controls to Docker.
PICO_PLATFORM="$(uname -s)"
PICO_UPDATE_SUPPORTED=no
case "${PICO_PLATFORM}" in
    Linux) PICO_DOCKER=no; PICO_UPDATE_SUPPORTED=yes ;;
    Darwin) PICO_DOCKER="${PICOCLAW_DOCKER:-yes}" ;;
    *) PICO_DOCKER="${PICOCLAW_DOCKER:-no}" ;;
esac
PICO_CA_FILE="${SSL_CERT_FILE:-}"
if [ -z "${PICO_CA_FILE}" ]; then
    for ca_candidate in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem; do
        if [ -r "${ca_candidate}" ]; then
            PICO_CA_FILE="${ca_candidate}"
            break
        fi
    done
fi
if [ -n "${PICO_CA_FILE}" ]; then
    export SSL_CERT_FILE="${PICO_CA_FILE}"
fi

ensure_dirs()
{
    mkdir -p "${PICO_HOME}/bin" "${PICO_CONFIG_DIR}" "$(dirname "${PICO_CONFIG}")" "${PICO_STATE_DIR}" "${PICO_WORKSPACE}" || return 1
    # Preserve keys saved by older helpers, without replacing current secrets.
    if [ ! -e "${PICO_SECURITY}" ] && [ -s "${PICO_LEGACY_SECURITY}" ]; then
        (umask 077; cp "${PICO_LEGACY_SECURITY}" "${PICO_SECURITY}") || return 1
        chmod 0600 "${PICO_SECURITY}" || return 1
    fi
}

# Read the small LVGL/FUI settings file when present.  Keeping this optional
# preserves the helper's environment-variable behaviour for headless installs.
ui_setting()
{
    key="${1:-}"
    [ -s "${PICO_UI_SETTINGS}" ] || return 1
    sed -n "s/^${key}=//p" "${PICO_UI_SETTINGS}" | head -n 1
}

# ConfigPage permits selecting a native core binary on desktop/Linux.  Honor
# that choice when it points to an executable; otherwise retain the selected
# release, bundled, managed, PATH, and /usr/bin discovery order above.
PICO_UI_BINARY_PATH="$(ui_setting binary_path 2>/dev/null || true)"
PICO_UI_ARGUMENTS="$(ui_setting arguments 2>/dev/null || true)"
if [ -z "${PICOCLAW_BIN:-}" ] && [ -n "${PICO_UI_BINARY_PATH}" ] && [ -x "${PICO_UI_BINARY_PATH}" ]; then
    PICO_BIN="${PICO_UI_BINARY_PATH}"
    PICO_EXPLICIT_BIN=yes
fi

log()
{
    ensure_dirs
    printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"${PICO_LOG}"
}

json_escape()
{
    printf '%s' "${1:-}" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

json_string_value()
{
    key="${1:-}"
    file="${2:-${PICO_CONFIG}}"
    [ -s "${file}" ] || return 1
    # Resolve metadata from the selected alias, not the first model in the
    # registry. JSON may also be compact or contain escaped strings.
    python3 -c '
import json, sys
try:
    with open(sys.argv[2], encoding="utf-8") as source:
        config = json.load(source)
    alias = sys.argv[3] or config.get("agents", {}).get("defaults", {}).get("model_name", "")
    models = config.get("model_list", [])
    model = next((item for item in models if item.get("model_name") == alias), {})
    value = alias if sys.argv[1] == "model_name" else model.get(sys.argv[1], "")
    if isinstance(value, str):
        print(value)
except (OSError, ValueError, TypeError, AttributeError):
    sys.exit(1)
' "${key}" "${file}" "${3:-}"
}

gateway_process_running()
{
    # Zombies have already stopped serving. Conversely a process that is
    # visible but cannot be signalled must not be mistaken for a stopped one.
    gateway_process_state="$(ps -o stat= -p "${1}" 2>/dev/null || true)"
    case "${gateway_process_state}" in *Z*) return 1 ;; esac
    kill -0 "${1}" 2>/dev/null || [ -n "${gateway_process_state}" ]
}

gateway_pid()
{
    pid="$(cat "${PICO_GATEWAY_PID}" 2>/dev/null || true)"
    case "${pid}" in
        ''|*[!0-9]*) return 1 ;;
    esac
    if gateway_process_running "${pid}"; then
        printf '%s\n' "${pid}"
        return 0
    fi
    rm -f "${PICO_GATEWAY_PID}"
    return 1
}

have_network()
{
    ip route get 1.1.1.1 >/dev/null 2>&1 && return 0
    route -n get default >/dev/null 2>&1 && return 0
    getent hosts github.com >/dev/null 2>&1 && return 0
    return 1
}

board_ip()
{
    hostname -I 2>/dev/null | awk '{print $1}' && return 0
    ip route get 1.1.1.1 2>/dev/null | sed -n 's/.* src \([^ ]*\).*/\1/p' | head -n 1
}

model_key_state()
{
    model_name="$(json_string_value model_name "${PICO_CONFIG}" 2>/dev/null || true)"
    [ -n "${model_name}" ] || { printf 'missing\n'; return; }

    # Ollama serves local models through an OpenAI-compatible endpoint and
    # does not require an API key.  Accept both the explicit ollama/ model
    # namespace and the conventional local Ollama endpoint, including the
    # Docker service name used by local deployments.
    model_id="$(json_string_value model "${PICO_CONFIG}" 2>/dev/null || true)"
    api_base="$(json_string_value api_base "${PICO_CONFIG}" 2>/dev/null || true)"
    case "${model_id}" in
        ollama/*)
            printf 'ready\n'
            return
            ;;
    esac
    case "${api_base}" in
        *://localhost|*://localhost:*|*://127.0.0.1|*://127.0.0.1:*|*://\[::1\]|*://\[::1\]:*|*://ollama|*://ollama:*)
            printf 'ready\n'
            return
            ;;
    esac

    [ -s "${PICO_SECURITY}" ] && awk -v target="${model_name}:0" '
        $0 == "  " target ":" { in_model=1; next }
        in_model && /^  [^ ].*:[[:space:]]*$/ { in_model=0 }
        in_model && /^[[:space:]]{4}api_keys:/ { found=1 }
        END { exit found ? 0 : 1 }
    ' "${PICO_SECURITY}" 2>/dev/null && {
        printf 'ready\n'
        return
    }
    printf 'missing\n'
}

docker_ready()
{
    [ "${PICO_DOCKER}" = "yes" ] && command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1
}

docker_exec()
{
    docker run --rm --entrypoint picoclaw \
        -e "PICOCLAW_HOME=${PICO_HOME}" -e "PICOCLAW_CONFIG=${PICO_CONFIG}" \
        -v "${PICO_HOME}:${PICO_HOME}" -v "${PICO_CONFIG_DIR}:${PICO_CONFIG_DIR}" \
        "${PICO_DOCKER_IMAGE}" "$@"
}

docker_gateway_running()
{
    [ "$(docker inspect -f '{{.State.Running}}' "${PICO_DOCKER_CONTAINER}" 2>/dev/null || true)" = true ]
}

status_cmd()
{
    ensure_dirs
    if [ "${PICO_DOCKER}" = yes ] && docker_ready && docker image inspect "${PICO_DOCKER_IMAGE}" >/dev/null 2>&1 && \
        version_output="$(docker_exec --no-color version 2>/dev/null)"; then
        installed=yes
        version="$(printf '%s\n' "${version_output}" | sed -n 's/.*\(picoclaw[[:space:]][^[:space:]]*\).*/\1/p' | head -n 1)"
        [ -n "${version}" ] || version=installed
    elif [ "${PICO_DOCKER}" != yes ] && [ -x "${PICO_BIN}" ] && version_output="$(PICOCLAW_HOME="${PICO_HOME}" PICOCLAW_CONFIG="${PICO_CONFIG}" \
        "${PICO_BIN}" --no-color version 2>/dev/null)"; then
        installed=yes
        version="$(printf '%s\n' "${version_output}" | sed -n 's/.*\(picoclaw[[:space:]][^[:space:]]*\).*/\1/p' | head -n 1)"
        [ -n "${version}" ] || version=installed
    else
        installed=no
        version='Not installed'
        if [ "${PICO_DOCKER}" = yes ] && ! docker_ready; then version='Docker is unavailable'; fi
        if [ "${PICO_DOCKER}" != yes ] && [ -x "${PICO_BIN}" ]; then version='PicoClaw cannot run on this host'; fi
    fi
    if [ "${PICO_DOCKER}" = yes ] && docker_ready && docker_gateway_running; then
        gateway=running
        pid="$(docker inspect -f '{{.State.Pid}}' "${PICO_DOCKER_CONTAINER}" 2>/dev/null || printf '%s' '-')"
    elif [ "${PICO_DOCKER}" != yes ] && pid="$(gateway_pid 2>/dev/null)"; then
        gateway=running
    else
        gateway=stopped
        pid=-
    fi
    if have_network; then network=ready; else network=missing; fi
    if [ -s "${PICO_CONFIG}" ]; then config=ready; else config=missing; fi
    ipaddr="$(board_ip)"
    [ -n "${ipaddr}" ] || ipaddr=device
    web_host="$(ui_setting host 2>/dev/null || true)"
    [ -n "${web_host}" ] || web_host="${ipaddr}"
    if [ "$(ui_setting public_mode 2>/dev/null || true)" = true ] && [ "${web_host}" = 0.0.0.0 ]; then
        # A wildcard is a bind address. Advertise the board's reachable IP in
        # the FUI endpoint/QR payload instead.
        web_host="${ipaddr}"
    fi
    configured_web_port="$(ui_setting port 2>/dev/null || true)"
    case "${configured_web_port}" in *[!0-9]*|'') configured_web_port="${PICO_WEB_PORT}" ;; esac
    model_name="$(json_string_value model_name "${PICO_CONFIG}" 2>/dev/null || true)"
    model_id="$(json_string_value model "${PICO_CONFIG}" 2>/dev/null || true)"
    api_base="$(json_string_value api_base "${PICO_CONFIG}" 2>/dev/null || true)"
    printf 'installed=%s\nversion=%s\ngateway=%s\npid=%s\nnetwork=%s\nconfig=%s\nmodel_key=%s\nurl=http://%s:%s\nweb_url=http://%s:%s\nmodel_name=%s\nmodel_id=%s\napi_base=%s\n' \
        "${installed}" "${version}" "${gateway}" "${pid}" "${network}" "${config}" "$(model_key_state)" \
        "${ipaddr}" "${PICO_GATEWAY_PORT}" "${web_host}" "${configured_web_port}" "${model_name}" "${model_id}" "${api_base}"
    printf 'update_supported=%s\n' "${PICO_UPDATE_SUPPORTED}"
}

release_cmd()
{
    if [ "${PICO_PLATFORM}" != Linux ]; then
        printf 'ERROR: release updates are supported on native Linux only\n'
        return 3
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        printf 'ERROR: Python 3 is required to manage PicoClaw releases\n'
        return 127
    fi
    if [ ! -r "${PICO_CONTROL_DIR}/picoclaw-releases.py" ]; then
        printf 'ERROR: PicoClaw release manager was not found\n'
        return 127
    fi
    PICOCLAW_HOME="${PICO_HOME}" PICOCLAW_CONFIG="${PICO_CONFIG}" \
        PICOCLAW_CONFIG_DIR="${PICO_CONFIG_DIR}" PICOCLAW_STATE_DIR="${PICO_STATE_DIR}" \
        PICOCLAW_UPDATE_BIN="${PICO_BIN}" PICOCLAW_UPDATE_CONTROL="${PICO_CONTROL_PATH}" \
        PICOCLAW_UPDATE_PLATFORM="${PICO_PLATFORM}" PICOCLAW_UPDATE_ARCH="$(uname -m)" \
        PICOCLAW_UPDATE_EXPLICIT_BIN="${PICO_EXPLICIT_BIN}" \
        python3 "${PICO_CONTROL_DIR}/picoclaw-releases.py" "$@"
}

download_file()
{
    url="${1}"
    output="${2}"
    if command -v curl >/dev/null 2>&1; then
        set -- -L --fail --connect-timeout 15 --max-time 300 -o "${output}"
        if [ -n "${PICO_CA_FILE}" ]; then set -- "$@" --cacert "${PICO_CA_FILE}"; fi
        curl "$@" "${url}"
        return $?
    fi
    if command -v wget >/dev/null 2>&1; then
        set -- -O "${output}"
        if [ -n "${PICO_CA_FILE}" ]; then set -- "$@" --ca-certificate="${PICO_CA_FILE}"; fi
        wget "$@" "${url}"
        return $?
    fi
    return 127
}

install_cmd()
{
    ensure_dirs
    if [ "${PICO_DOCKER}" = yes ]; then
        if ! docker_ready; then
            printf 'ERROR: Docker is unavailable\n'
            return 2
        fi
        if ! docker pull "${PICO_DOCKER_IMAGE}"; then
            printf 'ERROR: Docker image pull failed\n'
            return 5
        fi
        status_cmd
        return $?
    fi
    if [ -x "${PICO_BUNDLED_BIN}" ] && \
        [ -z "${PICOCLAW_RELEASE_URL:-}${PICOCLAW_RELEASE_TAG:-}${PICOCLAW_RELEASE_SHA256:-}" ]; then
        printf 'Use Settings > Updates to check releases and install a selected version.\n'
        status_cmd
        return $?
    fi
    if ! have_network; then
        printf 'ERROR: no network connection\n'
        return 2
    fi
    platform="$(uname -s)"
    architecture="$(uname -m)"
    case "${platform}/${architecture}" in
        Darwin/arm64|Darwin/aarch64)
            archive=picoclaw_Darwin_arm64.tar.gz
            release_sha256=30266e4edd6b0dfb871c6ac58e69ffdf8039fc65f23f2612f8792f704930765b
            ;;
        Darwin/x86_64|Darwin/amd64)
            archive=picoclaw_Darwin_x86_64.tar.gz
            release_sha256=57d4621f912bec29912bd0cdb52658c7410fb0beaaac7fffc85083d264b55127
            ;;
        Linux/aarch64|Linux/arm64)
            archive=picoclaw_Linux_arm64.tar.gz
            release_sha256=e78ab0de05d000698ff8b32458e71cb3134e52e9eaf540e3323a72e9914de060
            ;;
        Linux/x86_64|Linux/amd64)
            archive=picoclaw_Linux_x86_64.tar.gz
            release_sha256=86f0fe84aa55279352edbe13adfca484a03bcf189e2030b25e34628089069c3f
            ;;
        Linux/armv7l|Linux/armv7)
            archive=picoclaw_Linux_armv7.tar.gz
            release_sha256=1472e9913d800cc5bf7193e736d85007968dabb58973d0cdac02c7232dcf9b0b
            ;;
        *) printf 'ERROR: unsupported platform %s/%s\n' "${platform}" "${architecture}"; return 3 ;;
    esac
    url="${PICOCLAW_RELEASE_URL:-https://github.com/sipeed/picoclaw/releases/download/${PICO_RELEASE_TAG}/${archive}}"
    expected_sha256="${PICOCLAW_RELEASE_SHA256:-${release_sha256}}"
    temp_dir="$(mktemp -d "${TMPDIR:-/tmp}/lilygo-picoclaw.XXXXXX")" || return 4
    trap 'rm -rf -- "${temp_dir}"' EXIT HUP INT TERM
    log "Downloading ${url}"
    if ! download_file "${url}" "${temp_dir}/${archive}"; then
        printf 'ERROR: download failed\n'
        return 5
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual_sha256="$(sha256sum "${temp_dir}/${archive}" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
        actual_sha256="$(shasum -a 256 "${temp_dir}/${archive}" | awk '{print $1}')"
    else
        printf 'ERROR: SHA-256 tool is unavailable\n'
        return 6
    fi
    if [ "${actual_sha256}" != "${expected_sha256}" ]; then
        printf 'ERROR: runtime checksum mismatch\n'
        return 6
    fi
    if ! tar -xzf "${temp_dir}/${archive}" -C "${temp_dir}"; then
        printf 'ERROR: archive extraction failed\n'
        return 7
    fi
    found="$(find "${temp_dir}" -type f -name picoclaw | head -n 1)"
    if [ -z "${found}" ]; then
        printf 'ERROR: picoclaw binary not found in archive\n'
        return 8
    fi
    if ! chmod 0755 "${found}" || ! PICOCLAW_HOME="${PICO_HOME}" PICOCLAW_CONFIG="${PICO_CONFIG}" \
        "${found}" --no-color version >/dev/null 2>&1; then
        printf 'ERROR: downloaded PicoClaw cannot run on %s/%s\n' "${platform}" "${architecture}"
        return 9
    fi
    staged_bin="$(mktemp "${PICO_MANAGED_BIN}.XXXXXX")" || return 10
    if ! cp "${found}" "${staged_bin}" || ! chmod 0755 "${staged_bin}" || \
        ! mv -f "${staged_bin}" "${PICO_MANAGED_BIN}"; then
        rm -f "${staged_bin}"
        printf 'ERROR: could not install PicoClaw\n'
        return 10
    fi
    if [ ! -x "${PICO_ACTIVE_BIN}" ] && [ ! -x "${PICO_BUNDLED_BIN}" ]; then
        PICO_BIN="${PICO_MANAGED_BIN}"
    fi
    log "Installed ${PICO_RELEASE_TAG} to ${PICO_MANAGED_BIN}"
    status_cmd
}

export_models_cmd()
{
    # This response contains secrets for the UI import operation only. Do not
    # run directory migration, status reporting, or activity logging here.
    python3 -c '
import json, re, sys
from pathlib import Path

LIMIT = 1024 * 1024

def read_private(path):
    with path.open("rb") as source:
        data = source.read(LIMIT + 1)
    if len(data) > LIMIT:
        raise ValueError("Input is too large")
    return data.decode("utf-8")

def scalar(text):
    text = text.strip()
    if text.startswith(chr(34)):
        value, end = json.JSONDecoder().raw_decode(text)
        rest = text[end:].strip()
        if not isinstance(value, str) or (rest and not rest.startswith("#")):
            raise ValueError("Unsupported quoted value")
        return value
    if text.startswith(chr(39)):
        value, offset = "", 1
        while offset < len(text):
            if text[offset] == chr(39):
                if offset + 1 < len(text) and text[offset + 1] == chr(39):
                    value += chr(39)
                    offset += 2
                    continue
                rest = text[offset + 1:].strip()
                if rest and not rest.startswith("#"):
                    raise ValueError("Unsupported quoted value")
                return value
            value += text[offset]
            offset += 1
        raise ValueError("Unterminated quoted value")
    value = re.split(r"\s+#", text, maxsplit=1)[0].strip()
    if not value or value[0] in "[{&*!|>" or ": " in value:
        raise ValueError("Unsupported scalar value")
    return value

def security_keys(path):
    if not path.exists():
        return {}
    lines = read_private(path).splitlines()
    roots = []
    for index, line in enumerate(lines):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if "\t" in line[:len(line) - len(line.lstrip())]:
            raise ValueError("Unsupported indentation")
        if not line[0].isspace():
            if line.strip() in ("---", "..."):
                continue
            header = re.match(r"^(.+?):\s*(.*)$", line)
            if not header:
                raise ValueError("Invalid security document")
            if scalar(header.group(1)) == "model_list":
                tail = header.group(2).strip()
                if tail and not tail.startswith("#") and tail != "{}":
                    raise ValueError("Unsupported model security layout")
                roots.append(index)
    if not roots:
        return {}
    if len(roots) != 1:
        raise ValueError("Duplicate model security sections")
    start = roots[0]
    end = next((i for i in range(start + 1, len(lines))
                if lines[i] and not lines[i][0].isspace() and not lines[i].startswith("#")), len(lines))
    keys, entry, in_keys, key_fields = {}, None, False, set()
    for line in lines[start + 1:end]:
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        indentation = len(line) - len(line.lstrip())
        text = line.strip()
        if indentation == 2:
            header = re.fullmatch(r"(.+?):\s*(?:#.*)?", text)
            if not header:
                raise ValueError("Unsupported model security entry")
            entry = scalar(header.group(1))
            if entry in keys:
                raise ValueError("Duplicate model security entry")
            keys[entry] = []
            in_keys = False
        elif entry is None:
            raise ValueError("Invalid model security structure")
        elif indentation == 4 and not text.startswith("-"):
            header = re.match(r"^(.+?):\s*(.*)$", text)
            if not header:
                raise ValueError("Unsupported model security field")
            in_keys = scalar(header.group(1)) == "api_keys"
            if in_keys:
                if entry in key_fields:
                    raise ValueError("Duplicate API key field")
                key_fields.add(entry)
                tail = header.group(2).strip()
                if tail and not tail.startswith("#") and tail != "[]":
                    raise ValueError("Unsupported API key sequence")
        elif in_keys:
            if indentation not in (4, 6) or not text.startswith("- "):
                raise ValueError("Unsupported API key sequence")
            keys[entry].append(scalar(text[2:]))
        elif indentation < 4:
            raise ValueError("Invalid security indentation")
    return {entry: values[0] for entry, values in keys.items() if values}

def quoted(value):
    if not isinstance(value, str) or any(c in value for c in "\r\n\0"):
        raise ValueError("Invalid model field")
    return chr(34) + value.replace("\\", "\\\\").replace(chr(34), "\\" + chr(34)) + chr(34)

try:
    config_path, security_path, legacy_path = map(Path, sys.argv[1:4])
    if not config_path.exists():
        models = []
    else:
        raw = read_private(config_path)
        if not raw.strip():
            models = []
        else:
            config = json.loads(raw)
            if not isinstance(config, dict):
                raise ValueError("Invalid configuration")
            models = config.get("model_list", [])
    if not isinstance(models, list) or len(models) > 128:
        raise ValueError("Invalid model list")
    keys = security_keys(security_path if security_path.exists() else legacy_path) if models else {}
    records = []
    for model in models:
        if not isinstance(model, dict):
            raise ValueError("Invalid model entry")
        alias = model.get("model_name", "")
        model_id, api_base = model.get("model", ""), model.get("api_base", "")
        if not all(isinstance(value, str) and value for value in (alias, model_id, api_base)):
            raise ValueError("Incomplete model metadata")
        api_key = keys.get(alias + ":0", model.get("api_key", ""))
        records.append(" ".join(quoted(value) for value in (alias, alias, model_id, api_base, api_key)))
    output = "PICOCLAW_MODELS 1 " + str(len(records)) + "\n"
    if records:
        output += "\n".join(records) + "\n"
    if len(output.encode("utf-8")) > LIMIT:
        raise ValueError("Export is too large")
except (OSError, ValueError, TypeError):
    print("ERROR: could not read configured models. Check configuration files.", file=sys.stderr)
    sys.exit(1)
sys.stdout.write(output)
' "${PICO_CONFIG}" "${PICO_SECURITY}" "${PICO_LEGACY_SECURITY}"
}

save_config_cmd()
{
    ensure_dirs || return 1
    model_name_raw="${1:-picoclaw-agent}"
    model_id_raw="${2:-openai/gpt-4o-mini}"
    api_base_raw="${3:-https://api.openai.com/v1}"
    api_key_raw="${4:-}"
    if ! command -v python3 >/dev/null 2>&1; then
        printf 'ERROR: Python 3 is required to preserve PicoClaw model configuration\n'
        return 127
    fi
    # Keys travel through stdin. Preserve the complete runtime configuration
    # and edit only the selected entry in PicoClaw security YAML. In
    # particular, choosing an existing model with an empty key reuses its key.
    printf '%s' "${api_key_raw}" | python3 -c '
import json, os, re, sys, tempfile
from pathlib import Path

config_path, security_path = map(Path, sys.argv[1:3])
alias, model_id, api_base, workspace, port = sys.argv[3:8]
api_key = sys.stdin.read()
temporary_files = []

def stage(path, data):
    fd, name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    temporary_files.append(name)
    with os.fdopen(fd, "wb") as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    return name

def yaml_key(value):
    value = value.strip()
    if value.startswith(chr(34)):
        return json.loads(value)
    if value.startswith(chr(39)) and value.endswith(chr(39)):
        return value[1:-1].replace(chr(39) * 2, chr(39))
    return value

def replace_model_key(original):
    lines = original.decode("utf-8").splitlines(keepends=True)
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    for line in lines:
        if (re.match(r"^[^\s#]", line) and
                not re.match(r"^(?:---|\.\.\.)\s*$", line) and
                not re.match(r"^[^:]+:\s*", line)):
            raise ValueError("Unsupported security layout")
    roots = [i for i, line in enumerate(lines) if re.match(r"^model_list\s*:", line)]
    if len(roots) > 1:
        raise ValueError("Duplicate model security sections")
    if roots:
        start = roots[0]
        if not re.match(r"^model_list\s*:\s*(?:#.*)?$", lines[start]):
            if re.match(r"^model_list\s*:\s*\{\}\s*(?:#.*)?$", lines[start]):
                lines[start] = "model_list:\n"
            else:
                raise ValueError("Unsupported model security layout")
    else:
        start = len(lines)
        lines.append("model_list:\n")
    end = next((i for i in range(start + 1, len(lines))
                if re.match(r"^[^\s#]", lines[i])), len(lines))
    entries = []
    for i in range(start + 1, end):
        match = re.match(r"^  (\S.*):\s*(?:#.*)?$", lines[i])
        if re.match(r"^  [^\s#]", lines[i]) and not match:
            raise ValueError("Unsupported model security entry")
        if match and yaml_key(match.group(1)) == alias + ":0":
            entries.append(i)
    if len(entries) > 1:
        raise ValueError("Duplicate model security entries")
    key_lines = ["    api_keys:\n", "      - " + chr(39) +
                 api_key.replace(chr(39), chr(39) * 2) + chr(39) + "\n"]
    if not entries:
        lines[end:end] = ["  " + alias + ":0:\n"] + key_lines
    else:
        entry_start = entries[0]
        entry_end = next((i for i in range(entry_start + 1, end)
                          if re.match(r"^  \S", lines[i])), end)
        fields = [i for i in range(entry_start + 1, entry_end)
                  if re.match(r"^    api_keys\s*:", lines[i])]
        if len(fields) > 1:
            raise ValueError("Duplicate API key fields")
        if fields:
            field_start = fields[0]
            field_end = next((i for i in range(field_start + 1, entry_end)
                              if re.match(r"^    \S", lines[i])), entry_end)
            lines[field_start:field_end] = key_lines
        else:
            lines[entry_start + 1:entry_start + 1] = key_lines
    return "".join(lines).encode("utf-8")

try:
    if any(c in value for value in (alias, model_id, api_base, api_key) for c in "\r\n\0"):
        raise ValueError("Model fields must be single line")
    if not alias or ":" in alias or any(c in alias for c in "\"#{}[]&*!|>%@`" + chr(39)):
        raise ValueError("Invalid model alias")
    if config_path.exists():
        config = json.loads(config_path.read_text(encoding="utf-8"))
    else:
        config = {
            "version": 3,
            "agents": {"defaults": {
                "workspace": workspace, "restrict_to_workspace": True,
                "max_tokens": 2048, "context_window": 32768,
                "temperature": 0.7, "max_tool_iterations": 20}},
            "gateway": {"host": "0.0.0.0", "port": int(port), "log_level": "warn"},
            "model_list": [], "channel_list": {}}
    if not isinstance(config, dict):
        raise ValueError("Invalid configuration")
    agents = config.setdefault("agents", {})
    if not isinstance(agents, dict):
        raise ValueError("Invalid agents configuration")
    defaults = agents.setdefault("defaults", {})
    models = config.setdefault("model_list", [])
    if not isinstance(defaults, dict) or not isinstance(models, list) or any(
            not isinstance(model, dict) for model in models):
        raise ValueError("Invalid model configuration")
    matching = [model for model in models if model.get("model_name") == alias]
    if len(matching) > 1:
        raise ValueError("Ambiguous model alias")
    model = matching[0] if matching else {}
    model.update(model_name=alias, model=model_id, api_base=api_base)
    if not matching:
        models.append(model)
    defaults["model_name"] = alias
    config_data = (json.dumps(config, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    config_stage = stage(config_path, config_data)
    security_stage = security_backup = None
    if api_key:
        original_security = security_path.read_bytes() if security_path.exists() else None
        security_data = replace_model_key(original_security or b"")
        security_stage = stage(security_path, security_data)
        if original_security is not None:
            security_backup = stage(security_path, original_security)
        os.replace(security_stage, security_path)
    try:
        os.replace(config_stage, config_path)
    except OSError:
        # Restore the prior key file if the paired configuration commit fails.
        if security_stage:
            if security_backup:
                os.replace(security_backup, security_path)
            else:
                security_path.unlink()
        raise
except (OSError, ValueError, TypeError):
    print("ERROR: could not save model configuration. Check configuration files and storage.",
          file=sys.stderr)
    sys.exit(1)
finally:
    for name in temporary_files:
        try:
            os.unlink(name)
        except FileNotFoundError:
            pass
' "${PICO_CONFIG}" "${PICO_SECURITY}" "${model_name_raw}" "${model_id_raw}" \
        "${api_base_raw}" "${PICO_WORKSPACE}" "${PICO_GATEWAY_PORT}" || return 1
    log "Saved model configuration for ${model_name_raw}"
    printf 'OK: configuration saved\n'
}

provider_query_cmd()
{
    provider_helper="${PICO_CONTROL_DIR}/picoclaw-provider.py"
    command -v python3 >/dev/null 2>&1 || { printf 'Python 3 is required to query the model provider.\n'; return 127; }
    [ -r "${provider_helper}" ] || { printf 'Provider helper was not found.\n'; return 127; }
    if [ "${3:-}" = "--key-stdin" ]; then
        python3 "${provider_helper}" "${1}" "${2:-}"
    else
        python3 "${provider_helper}" "${1}" "${2:-}" </dev/null
    fi
}

channels_cmd()
{
    channels_helper="${PICO_CONTROL_DIR}/picoclaw-channels.py"
    command -v python3 >/dev/null 2>&1 || { printf 'Python 3 is required to configure channels.\n'; return 127; }
    [ -r "${channels_helper}" ] || { printf 'Channel helper was not found.\n'; return 127; }
    # The complete payload, including credentials, stays on stdin. Neither
    # loading nor saving channels writes configuration values to activity.log.
    PICOCLAW_HOME="${PICO_HOME}" PICOCLAW_CONFIG="${PICO_CONFIG}" \
        PICOCLAW_GATEWAY_PORT="${PICO_GATEWAY_PORT}" \
        python3 "${channels_helper}" "$@"
}

weixin_bind_cmd()
{
    weixin_helper="${PICO_CONTROL_DIR}/picoclaw-weixin.py"
    command -v python3 >/dev/null 2>&1 || { printf 'Python 3 is required for Weixin QR binding.\n'; return 127; }
    [ -r "${weixin_helper}" ] || { printf 'Weixin binding helper was not found.\n'; return 127; }
    # Request JSON remains on stdin. qrcode and confirmed bot_token values
    # never become shell arguments, activity-log entries, or process listings.
    python3 "${weixin_helper}" "$@"
}

test_connection_cmd()
{
    provider_query_cmd test-connection "${1:-}" "${2:-}"
}

fetch_models_cmd()
{
    provider_query_cmd fetch-models "${1:-}" "${2:-}"
}
ask_cmd()
{
    ensure_dirs || return 1
    if [ "${PICO_DOCKER}" = yes ]; then
        if ! docker_ready || ! docker image inspect "${PICO_DOCKER_IMAGE}" >/dev/null 2>&1; then
            printf 'PicoClaw is not installed.\n'
            return 2
        fi
    elif [ ! -x "${PICO_BIN}" ]; then
        printf 'PicoClaw is not installed.\n'
        return 2
    fi
    if [ ! -s "${PICO_CONFIG}" ]; then printf 'Configure a model and API key first.\n'; return 3; fi
    # Local Ollama does not require an Internet route. Remote providers still
    # need a usable network connection before invoking the runtime.
    ask_model_alias="${3:-}"
    ask_model_id="$(json_string_value model "${PICO_CONFIG}" "${ask_model_alias}" 2>/dev/null || true)"
    ask_api_base="$(json_string_value api_base "${PICO_CONFIG}" "${ask_model_alias}" 2>/dev/null || true)"
    if [ -n "${ask_model_alias}" ] && { [ -z "${ask_model_id}" ] || [ -z "${ask_api_base}" ]; }; then
        printf 'Selected model is not configured. Save it before sending a message.\n'
        return 3
    fi
    local_ollama=no
    case "${ask_model_id}" in ollama/*) local_ollama=yes ;; esac
    case "${ask_api_base}" in *://localhost*|*://127.0.0.1*|*://\[::1\]*|*://ollama*) local_ollama=yes ;; esac
    if [ "${local_ollama}" != yes ] && ! have_network; then printf 'No network connection.\n'; return 4; fi
    prompt="${1:-}"
    session="${2:-cli:default}"
    if [ -z "${prompt}" ]; then printf 'Message is empty.\n'; return 5; fi
    log "Agent request started"
    set -- --no-color agent
    if [ -n "${ask_model_alias}" ]; then set -- "$@" --model "${ask_model_alias}"; fi
    set -- "$@" -s "${session}" -m "${prompt}"
    if [ "${PICO_DOCKER}" = yes ]; then
        docker_exec "$@"
    else
        PICOCLAW_HOME="${PICO_HOME}" PICOCLAW_CONFIG="${PICO_CONFIG}" \
            "${PICO_BIN}" "$@"
    fi
    result=$?
    log "Agent request finished with status ${result}"
    return "${result}"
}

gateway_start_cmd()
{
    ensure_dirs || return 1
    if [ "${PICO_DOCKER}" = yes ]; then
        if ! docker_ready || ! docker image inspect "${PICO_DOCKER_IMAGE}" >/dev/null 2>&1; then
            printf 'ERROR: install the PicoClaw Docker image first\n'
            return 2
        fi
        if [ ! -s "${PICO_CONFIG}" ]; then printf 'ERROR: configure a model first\n'; return 3; fi
        if docker_gateway_running; then status_cmd; return 0; fi
        docker rm -f "${PICO_DOCKER_CONTAINER}" >/dev/null 2>&1 || true
        if ! docker run -d --name "${PICO_DOCKER_CONTAINER}" --restart unless-stopped \
            -p "${PICO_GATEWAY_PORT}:${PICO_GATEWAY_PORT}" --entrypoint picoclaw \
            -e "PICOCLAW_HOME=${PICO_HOME}" -e "PICOCLAW_CONFIG=${PICO_CONFIG}" \
            -v "${PICO_HOME}:${PICO_HOME}" -v "${PICO_CONFIG_DIR}:${PICO_CONFIG_DIR}" \
            "${PICO_DOCKER_IMAGE}" gateway ${PICO_UI_ARGUMENTS} >/dev/null; then
            printf 'ERROR: Docker gateway could not start\n'
            return 4
        fi
        sleep 1
        if ! docker_gateway_running; then
            printf 'ERROR: Docker gateway exited before it became ready\n'
            return 4
        fi
        status_cmd
        return $?
    fi
    if [ ! -x "${PICO_BIN}" ]; then printf 'ERROR: PicoClaw is not installed\n'; return 2; fi
    if [ ! -s "${PICO_CONFIG}" ]; then printf 'ERROR: configure a model first\n'; return 3; fi
    if gateway_pid >/dev/null 2>&1; then status_cmd; return 0; fi
    # Arguments are configured in FUI's Settings page. Intentional word
    # splitting maps the space-separated launch argument field to argv items.
    gateway_extra_args=""
    if [ -n "${PICO_UI_ARGUMENTS}" ]; then gateway_extra_args="${PICO_UI_ARGUMENTS}"; fi
    if [ "${PICO_PLATFORM}" = Linux ]; then
        if ! command -v setsid >/dev/null 2>&1; then
            printf 'ERROR: setsid is required to start the native PicoClaw gateway\n'
            return 127
        fi
        PICOCLAW_HOME="${PICO_HOME}" PICOCLAW_CONFIG="${PICO_CONFIG}" \
            nohup setsid "${PICO_BIN}" --no-color gateway --host 0.0.0.0 ${gateway_extra_args} </dev/null >"${PICO_GATEWAY_LOG}" 2>&1 &
    else
        PICOCLAW_HOME="${PICO_HOME}" PICOCLAW_CONFIG="${PICO_CONFIG}" \
            nohup "${PICO_BIN}" --no-color gateway --host 0.0.0.0 ${gateway_extra_args} </dev/null >"${PICO_GATEWAY_LOG}" 2>&1 &
    fi
    printf '%s\n' "$!" >"${PICO_GATEWAY_PID}"
    sleep 1
    if ! gateway_pid >/dev/null 2>&1; then
        tail -20 "${PICO_GATEWAY_LOG}" 2>/dev/null || true
        return 4
    fi
    log "Gateway started"
    status_cmd
}

gateway_stop_cmd()
{
    if [ "${PICO_DOCKER}" = yes ]; then
        if ! docker_ready; then
            printf 'ERROR: Docker is unavailable; gateway stop could not be confirmed\n'
            return 2
        fi
        if docker inspect "${PICO_DOCKER_CONTAINER}" >/dev/null 2>&1; then
            if ! docker rm -f "${PICO_DOCKER_CONTAINER}" >/dev/null 2>&1; then
                printf 'ERROR: Docker gateway could not stop\n'
                return 4
            fi
        fi
        if docker_gateway_running; then
            printf 'ERROR: Docker gateway is still running\n'
            return 4
        fi
        status_cmd
        return $?
    fi
    if pid="$(gateway_pid 2>/dev/null)"; then
        if ! kill "${pid}" >/dev/null 2>&1 && gateway_process_running "${pid}"; then
            printf 'ERROR: gateway could not stop; its PID record has been kept\n'
            return 4
        fi
        sleep 1
        if gateway_process_running "${pid}"; then
            if ! kill -9 "${pid}" >/dev/null 2>&1 && gateway_process_running "${pid}"; then
                printf 'ERROR: gateway could not stop; its PID record has been kept\n'
                return 4
            fi
            sleep 1
        fi
        if gateway_process_running "${pid}"; then
            printf 'ERROR: gateway is still running; its PID record has been kept\n'
            return 4
        fi
        rm -f "${PICO_GATEWAY_PID}" || return 4
        log "Gateway stopped"
    fi
    status_cmd
}

log_cmd()
{
    tail -80 "${PICO_LOG}" "${PICO_GATEWAY_LOG}" 2>/dev/null || true
}

# Serialize native mutations with the release manager's fcntl.flock lock.
# --close keeps its descriptor out of the detached gateway. The updater calls
# these commands internally while holding the same lock itself.
case "${1:-status}" in
    install|gateway-start|gateway-stop|save-config|save-channel)
        if [ "${PICO_PLATFORM}" = Linux ] && [ "${PICOCLAW_UPDATE_INTERNAL:-}" != 1 ]; then
            ensure_dirs || exit 1
            if ! command -v flock >/dev/null 2>&1; then
                printf 'ERROR: flock is required to coordinate PicoClaw updates and gateway controls\n'
                exit 127
            fi
            flock --close --nonblock --conflict-exit-code 75 "${PICO_HOME}/.update.lock" \
                env PICOCLAW_UPDATE_INTERNAL=1 /bin/sh "${PICO_CONTROL_PATH}" "$@"
            lock_result=$?
            if [ "${lock_result}" -eq 75 ]; then
                printf 'ERROR: a PicoClaw update or gateway operation is already in progress\n'
            fi
            exit "${lock_result}"
        fi
        ;;
esac

case "${1:-status}" in
    status) status_cmd ;;
    export-models) export_models_cmd ;;
    load-channels) channels_cmd load-channels ;;
    save-channel)
        if [ "${3:-}" != "--json-stdin" ] || [ "$#" -ne 3 ]; then
            printf 'ERROR: channel configuration must be provided on stdin\n'
            exit 64
        fi
        channels_cmd save-channel "${2:-}" ;;
    weixin-bind-start|weixin-bind-poll)
        if [ "$#" -ne 1 ]; then
            printf 'ERROR: Weixin binding request must be provided on stdin\n'
            exit 64
        fi
        weixin_bind_cmd "${1}" ;;
    install) install_cmd ;;
    check-updates) release_cmd check-updates ;;
    install-release) release_cmd install-release "${2:-}" ;;
    save-config)
        shift
        if [ "${4:-}" = "--key-stdin" ]; then
            api_key_stdin="$(cat)"
            save_config_cmd "${1:-}" "${2:-}" "${3:-}" "${api_key_stdin}"
        else
            save_config_cmd "$@"
        fi
        ;;
    test-connection)
        shift
        test_connection_cmd "${1:-}" "${2:-}"
        ;;
    fetch-models)
        shift
        fetch_models_cmd "${1:-}" "${2:-}"
        ;;
    ask)
        shift
        if [ "${1:-}" = "--prompt-stdin" ]; then
            prompt_stdin="$(cat)"
            ask_cmd "${prompt_stdin}" "${2:-cli:default}" "${3:-}"
        else
            ask_cmd "${1:-}" "${2:-cli:default}" "${3:-}"
        fi
        ;;
    gateway-start) gateway_start_cmd ;;
    gateway-stop) gateway_stop_cmd ;;
    log) log_cmd ;;
    *)
        printf 'Usage: %s {status|export-models|load-channels|save-channel CHANNEL --json-stdin|weixin-bind-start|weixin-bind-poll|install|check-updates|install-release TAG|save-config MODEL_NAME MODEL_ID API_BASE API_KEY|test-connection API_BASE|fetch-models API_BASE|ask PROMPT|gateway-start|gateway-stop|log}\n' "$0"
        exit 64
        ;;
esac
