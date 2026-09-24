# PicoClaw for LILYGO UI

PicoClaw is a standalone, responsive LILYGO UI application for chatting with a
local [PicoClaw](https://github.com/sipeed/picoclaw) runtime. It provides model
provider and chat channel configuration, runtime installation, gateway controls, activity logs,
and a touch-friendly chat interface in one AppKit application.

The product identity is:

```text
Package and executable: lilygo-ui-picoclaw
Application ID:         cc.lilygo.ui.PicoClaw
```

## Features

- Persistent chat history with separate user and PicoClaw messages.
- Select a saved model directly from the **Model** menu in Chat. Switching
  keeps the conversation and unsent draft, and sending resumes after the
  selected configuration is confirmed. The list includes provider names.
- **New chat** preserves previous conversations; use **History** to switch and
  continue them. Long conversations offer **Earlier messages** above the latest
  32 messages, and each conversation has its own backend session.
- Responsive portrait and landscape layouts with an integrated LVGL keyboard.
- Four main pages: **Chat**, **Models**, **Channels**, and **Settings**.
- iPhone-style navigation with evenly spaced icons and labels, a blue active
  tab, and a white bar with a fine separator. Chat uses a speech bubble, Models
  a chip, Channels connected nodes, and Settings a gear. Wide screens use the
  same labeled items in a side rail; the keyboard temporarily hides navigation.
- iOS-style Settings with inset grouped rows, language preferences, and a
  persistent Save action. Gateway status, start/stop,
  and refresh controls appear first; open **Settings → Logs** to view and
  refresh activity logs. Runtime details include the Gateway address.
  Long values use full-width rows on narrow screens.
- Ready-to-use profiles for OpenAI, Anthropic, Google Gemini, DeepSeek, and
  Ollama, plus a custom OpenAI-compatible profile.
- Provider name, API base, model ID, and API key editing.
- A Models page to add, edit, remove, and import saved model configurations.
  Each model retains its own API key; select it from the Chat model menu.
  Models are grouped in order under OpenAI, Anthropic, Google Gemini, DeepSeek,
  and Ollama. Each group has a **+** button that prefills its provider's API base
  and model format. Portrait uses a single column; wide screens show two
  configuration columns within each group. Editing keeps Save and the focused
  field visible above the keyboard. Saving returns to the updated configuration
  in the list.
- Tap a configuration to edit it. Set a provider name for the editor and
  configuration titles. The editor follows Settings with API base,
  a masked API key and **Test**, and one editable **Model** field with **Fetch**.
  Type a model ID directly or use its dropdown to select a fetched model. Internal model aliases are
  managed automatically; the editor has no alias, mapping, or delete controls.
  Fetched IDs are kept as returned when selected and saved; no provider prefix
  is added. Provider grouping is stored separately from the model ID.
- Swipe a model configuration left to reveal **Delete**, then confirm its
  removal. Swipe right to close it; vertical scrolling still browses the list.
- Each configuration shows its API connection indicator and remaining credit.
  Entering **Models** automatically checks saved configurations in the background,
  sharing one query for models with the same API base and key. Remote presets
  without a key are skipped; local Ollama is checked without one. Navigation,
  editing and chat remain available while results update in place. Returning to
  **Models** refreshes the results and reuses any checks still in progress.
  Use a configuration's refresh icon or the editor's **Test** / **Fetch** to
  update it manually.
  Results are scoped to the API base and key for the current app session.
  DeepSeek, OpenRouter, SiliconFlow, Sub2API and compatible billing gateways expose
  balances; other providers show unavailable/not provided, and local Ollama
  shows not metered. A failed balance query does not mark a working API offline.
  Custom providers first query `GET /v1/usage` using the configured Bearer API
  key, accepting `remaining`, `quota.remaining` or `balance` with `unit`
  (default USD), as in cc-switch usage scripts. An API base ending in `/v1`
  does not add that suffix twice. `/user/balance` and the legacy
  `/dashboard/billing/subscription` + `/dashboard/billing/usage` APIs remain
  fallbacks. Sub2API's unrestricted subscription with `remaining: -1` displays
  **Unlimited**. Arbitrary cc-switch scripts and New API's account access-token
  queries are not imported; they may require different credentials from an API key.
- Non-blocking runtime status, install, agent, gateway, and log commands.
- A **Channels** page to configure chat integrations. Select a channel, edit its
  credentials and options, turn **Enabled** on or off, then press **Save**.
  Related options are grouped into Credentials, Security, Access, Behavior,
  Connection, Webhook, and Advanced sections to match the PicoClaw web UI.
  Secret fields are masked, and failed saves retain the draft for correction.
  Restart the Gateway from **Settings** after saving to apply the new configuration.
- App-private XDG configuration, workspace, state, and runtime directories.
- The ARM64 device deb includes the PicoClaw runtime, so installing or launching
  it on the board does not require a runtime download. Standalone runtime
  downloads remain available for Linux (ARM64, x86_64, ARMv7). The macOS
  simulator uses the official PicoClaw Docker image by default, so no native
  PicoClaw binary is installed on the Mac.

## Build and test

Requirements are CMake 3.21 or newer, a C++17 compiler, `pkg-config`, SDL2,
LPM, Python 3 for tests, and the checked-in AppKit submodule.

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

The render tests exercise chat, models, channels, and settings navigation at `320x568`,
`568x1232`, `1232x568`, and `1024x768`. Their PPM snapshots are written under
`build/host-simulator/`.

[Settings preview](assets/previews/settings-ios.png)

[Navigation preview](assets/previews/navigation-ios.png)

[Models preview](assets/previews/models.png) · [Model editor](assets/previews/model-editor.png)

[Swipe to delete a model](assets/previews/model-swipe-delete.png)

[Chat model selector](assets/previews/chat-models.png)

[Channels preview](assets/previews/channels.png) · [Channel editor](assets/previews/channel-editor.png)

[Weixin QR binding preview](assets/previews/weixin-binding.png)

The host simulator connects to a real PicoClaw runtime on the development
machine. Building the UI does not install that runtime. On macOS, install
Docker Desktop first. On first launch, Settings shows `Not installed`; use
**Install / update** there, or run:

```sh
sh scripts/picoclaw-control.sh install
sh scripts/picoclaw-control.sh status
```

The macOS installer pulls `docker.io/sipeed/picoclaw:latest` and mounts the
application's existing configuration, security file, workspace, and logs into
the container. Gateway control uses a managed Docker container named
`lilygo-picoclaw-gateway`. Restart the simulator after installing from a
terminal so it refreshes the runtime status. Save a model configuration and
API key in **Models**, then select it from Chat's **Model** menu before chatting.

The bundled profiles use PicoClaw's provider/model prefixes and provider
endpoints:

| Provider | Model example | API base | API key |
| --- | --- | --- | --- |
| OpenAI | `openai/gpt-4o-mini` | `https://api.openai.com/v1` | Required |
| Anthropic | `anthropic/claude-3-5-sonnet-latest` | `https://api.anthropic.com/v1` | Required |
| Google Gemini | `gemini/gemini-2.0-flash` | `https://generativelanguage.googleapis.com/v1beta` | Required |
| DeepSeek | `deepseek/deepseek-chat` | `https://api.deepseek.com/v1` | Required |
| Ollama | `ollama/llama3.2` | `http://127.0.0.1:11434/v1` | Not required |

Ollama must be running on the same host as the PicoClaw runtime. When the
runtime runs in Docker, set the profile's API base to an address reachable from
the container (for example `http://host.docker.internal:11434/v1`).

An existing PicoClaw binary can also be selected with `PICOCLAW_BIN` or the
Settings binary path. Otherwise the helper prefers the runtime beside the
installed control helper, then its legacy managed installation, then `PATH`.
A release explicitly installed from **Updates** takes precedence over the
bundled runtime. Custom binary paths must be cleared before installing a release.

Build and package for the device with:

```sh
cmake --preset cm0-cross
cmake --build --preset cm0-cross --parallel
cpack --config build/cm0-cross/CPackConfig.cmake -B dist
```

Linux ARM64 builds bundle the
[`nightly` ARM64 release](https://github.com/sipeed/picoclaw/releases/download/nightly/picoclaw_Linux_arm64.tar.gz)
by default. CMake downloads and caches the archive on the build machine, checks
that the core is an ARM64 ELF executable, and installs it alongside the helper
at `/usr/lib/lilygo-ui-picoclaw/picoclaw` (with the standard `/usr` preset).
The upstream license, README, and archive provenance are included in the deb.
The macOS simulator does not download or bundle the Linux binary.

For an offline or reproducible build, supply a saved archive and its SHA-256:

```sh
cmake --preset cm0-cross \
  -DLILYGO_PICOCLAW_RUNTIME_ARCHIVE=/absolute/path/picoclaw_Linux_arm64.tar.gz \
  -DLILYGO_PICOCLAW_RUNTIME_SHA256=<archive-sha256>
cmake --build --preset cm0-cross --parallel
cpack --config build/cm0-cross/CPackConfig.cmake -B dist
```

The `nightly` URL changes over time. Keep the archive and checksum together to
rebuild the same version. To fetch a newer nightly at the same URL, remove
`build/cm0-cross/picoclaw-runtime/downloads/` before reconfiguring and update
the expected checksum if one was set. `LILYGO_PICOCLAW_RUNTIME_URL` selects
another archive source, and `LILYGO_PICOCLAW_BUNDLE_RUNTIME=OFF` disables bundling
when a runtime is provisioned separately. The bundled copy remains available
after an in-app update; updating the deb does not replace an explicitly selected
user release.

Install on the target board and open **PicoClaw** from Launcher:

```sh
sudo apt install ./lilygo-ui-picoclaw_0.1.0_arm64.deb
lilygo-ui-picoclaw
```

The application loads the launching user's existing configuration. On a fresh
installation, add a provider configuration in **Models**, enter the model/API details,
and press **Save**, then select it from Chat's **Model** menu. Chat can then invoke
the bundled agent directly; use **Settings → Start gateway** to start the Gateway
with the same configuration. The Gateway can be stopped from the top of Settings.
Installing the deb does not start a system
service or create configuration under root's home directory.

On the Linux board, Gateway runs as a native background process using the
selected release, or `/usr/lib/lilygo-ui-picoclaw/picoclaw` initially. The helper
starts it with `nohup` and `setsid`, passes `PICOCLAW_HOME` and `PICOCLAW_CONFIG`,
and stops the recorded PID. The Gateway survives closing the UI.
Docker is not used or required on the board, even if `PICOCLAW_DOCKER=yes`
is present in the environment. The same controls are available from a terminal
under the same user account as the UI:

```sh
/usr/lib/lilygo-ui-picoclaw/picoclaw-control gateway-start
/usr/lib/lilygo-ui-picoclaw/picoclaw-control status
/usr/lib/lilygo-ui-picoclaw/picoclaw-control gateway-stop
```

For a board with configuration in a custom directory, launch with
`PICOCLAW_CONFIG_DIR=/path/to/config lilygo-ui-picoclaw`. Both the interface and
control helper use that directory for `ui-settings.conf` and `config.json`,
with `.security.yml` beside the JSON file. Existing configurations and keys
remain in the user's writable directories across package upgrades.

## Configure chat channels

Open **Channels** to view the available integrations and their configured
enabled state: Feishu / Lark, Weixin, Discord, Telegram, Slack, DingTalk, QQ,
WhatsApp, LINE, and OneBot. Select an integration to edit its channel-specific fields, such
as its bot token or application credentials, allowed senders, and connection
options. The **Enabled** switch controls whether PicoClaw starts that channel.
The list shows configuration state; it does not verify that the channel is
connected to its service.

Press **Save** to write the channel settings to the runtime configuration.
Saving is asynchronous; any error is shown in the editor and keeps the entered
values available to correct and retry. The Save and back actions stay reachable
while the keyboard is open, and resizing the window preserves the draft.
Use the list's refresh action to reload configuration changed outside the app.

Open Weixin and press **Bind Weixin**, then scan the QR code with WeChat and
confirm on your phone. The app contacts Tencent's iLink service directly; the
Web UI and Gateway do not need to be running. It checks every two seconds and
expires the code after five minutes. **New QR code** and **Cancel** are available
while waiting. The confirmed account appears in the form; its token stays hidden.
Enable the channel and press **Save** to store the credentials privately. If a
Gateway is already running, the app restarts it automatically after a Weixin
save so the new token is loaded.
Canceling, expiry, or failure preserves the previous configuration. Other
channel changes still require a Gateway restart after saving.

Changes take effect when the Gateway next starts. If it is running, use
**Settings → Stop gateway**, then **Start gateway** after saving. Channel setup does
not test ordinary channel connectivity or send a test message; Weixin binding
is the channel's explicit QR authorization flow.

## Check and install Gateway updates

On the board, open **Settings → Updates → Check for updates**.
The application reads every page of the public
[GitHub Releases](https://github.com/sipeed/picoclaw/releases) API and lists
stable, prerelease, and nightly versions. It shows the latest stable version
and whether the installed stable/nightly runtime has an update. Select a
version and press **Install selected** to install that exact tag.
Versions without a matching platform archive remain visible but cannot be
installed. Older archives with an incompatible CLI fail validation before the
active runtime is changed.

Downloads use the selected release's SHA-256 digest, with its checksum file
as a fallback. Missing checksums, network errors, GitHub rate limits, and
incomplete version responses produce an error and preserve the existing
selection. A mutable `nightly` is checked by its archive digest, so a new build
under the same tag can be detected.

The update is stored under `$PICOCLAW_HOME/versions/` (by default
`~/.local/share/lilygo/ui/picoclaw/versions/`), with
`bin/active-picoclaw` selecting it atomically. It does not require root or
change the files owned by the deb. A running Gateway stays available during
the download, then restarts with the new binary and existing configuration.
If startup or the health check fails, the updater restores the previous
selection and configuration and attempts to restart the previous Gateway.
A stopped Gateway remains stopped. Update and Gateway control operations
share a lock so they cannot switch the runtime concurrently.

The terminal equivalents, run as the UI user, are:

```sh
/usr/lib/lilygo-ui-picoclaw/picoclaw-control check-updates
/usr/lib/lilygo-ui-picoclaw/picoclaw-control install-release v0.3.1
```

Python 3 and util-linux are installed as deb dependencies. Version selection
is for the native Linux runtime; the macOS simulator retains its Docker image
update action.

[Updates preview](assets/previews/updates.png)

## Runtime data

The installed control helper follows the XDG base-directory specification:

```text
$XDG_DATA_HOME/lilygo/ui/picoclaw/       runtime and workspace
$XDG_CONFIG_HOME/lilygo/ui/picoclaw/     config.json and .security.yml
$XDG_STATE_HOME/lilygo/ui/picoclaw/      logs and gateway PID
```

Conversation titles, messages, and the selected session are saved automatically
in `$XDG_STATE_HOME/lilygo/ui/picoclaw/conversations.dat` with mode `0600`.
`PICOCLAW_STATE_DIR` overrides that directory, and `PICOCLAW_HISTORY_DIR` can
override just the conversation archive. Storage errors are shown in the chat
status; an unreadable archive is preserved instead of replaced. A response
interrupted by closing the app is marked as interrupted when reopened.

UI preferences, including language and legacy FUI-compatible settings, are stored in
`$XDG_CONFIG_HOME/lilygo/ui/picoclaw/ui-settings.conf`, or at the path given by
`PICOCLAW_UI_SETTINGS`. The application uses one fixed iOS-style light
appearance; selectable themes were removed. Legacy `theme=` lines are ignored
when loading and omitted the next time settings are saved.
Settings has no Connection group or Web host, Web port, and Public access controls:
the target board does not provide a Web management service. Legacy `host`, `port`,
`public_mode`, `binary_path`, and `arguments` values remain compatible and are
preserved when saving preferences. The legacy Web port only supplies Web URL
metadata; it does not change the Gateway listening port.

The Models page stores provider display names, internal aliases, model IDs, API bases, and API keys in
`models.conf` beside these preferences, using atomic writes and mode `0600`.
`PICOCLAW_MODELS_PATH` overrides this file. Keys are masked in the list and
editor, with an explicit Show action while editing. **Import existing models**
reads the runtime's `config.json` and `.security.yml`, adds missing models and
fills missing keys for matching entries while preserving existing saved values.
**Save** keeps a model in this library; the Chat model selector applies it to
the runtime and verifies that it is ready before enabling Send.
Runtime status refreshes preserve edited entries instead of adding an old
configuration with the same internal alias. Saving repairs duplicate internal
aliases left by earlier versions without moving keys between configurations.
Switching models preserves other runtime models, keys, and service settings.
CLI messages explicitly specify the confirmed model alias. In local WebSocket
mode, model switching also restarts the managed Gateway and verifies its
configuration. Remote WebSocket gateways cannot be switched through these
local controls. A failed switch leaves sending disabled until a model is
successfully selected again. Editing a selected model's API details also
requires reselecting it to apply the changes.

The API key is stored in PicoClaw's private `.security.yml` beside `config.json`
with mode `0600`. Existing keys in the runtime directory are copied automatically
when the configuration directory has no security file yet.
API keys and chat prompts are sent to the helper over standard input, so they
are not interpolated into shell commands or exposed in the process list. The
provider key is not printed in status or activity output.

The runtime helper honors these deployment overrides:

```text
PICOCLAW_BIN
PICOCLAW_HOME
PICOCLAW_CONFIG
PICOCLAW_CONFIG_DIR
PICOCLAW_STATE_DIR
PICOCLAW_UI_SETTINGS
PICOCLAW_GATEWAY_PORT
PICOCLAW_TRANSPORT
PICOCLAW_GATEWAY_TOKEN
PICOCLAW_RELEASE_TAG
PICOCLAW_RELEASE_URL
PICOCLAW_RELEASE_SHA256
PICOCLAW_DOCKER
PICOCLAW_DOCKER_IMAGE
PICOCLAW_DOCKER_CONTAINER
```

`PICOCLAW_DOCKER` defaults to `yes` on macOS. Set it to `no` when using a native
macOS development binary. Linux always uses native processes and ignores this
override; the Docker image and container settings apply to the macOS simulator.

`LILYGO_PICOCLAW_CONTROL` can select a different control helper for integration
testing or a customized deployment.

## Architecture

The UI follows the repository's Model-View-ViewModel boundary:

```text
LVGL View -> PicoClawViewModel -> PicoClawModel
                         |
                         +-> PicoClawClient -> picoclaw-control / Pico Protocol WebSocket
```

`PicoClawClient` runs helper commands on one worker thread and publishes results
back through polling on the LVGL thread. Closing the application terminates the
active helper process group before joining the worker. The application consumes
AppKit only through `find_package(LilyGoUI CONFIG REQUIRED)` and has no source or
build dependency on Launcher.

The interaction model was adapted from the PicoClaw feature in
`Xinyuan-LilyGO/T-Display-K230` at commit
`bb831ab358b66f5bd9a87ecd7c580fee4537492e`, with K230-specific paths, launcher
preferences, and the riscv64 runtime removed.

## Android FUI reference

The Android/Flutter reference implementation is maintained in
[`sipeed/picoclaw_fui`](https://github.com/sipeed/picoclaw_fui). Its chat UI is in
[`lib/src/ui/chat_page.dart`](https://github.com/sipeed/picoclaw_fui/blob/main/lib/src/ui/chat_page.dart),
and the service/configuration flows are in `dashboard_page.dart`, `config_page.dart`
and `log_page.dart`. This project ports the same chat affordances and runtime
controls to LVGL: a connection pill, new-conversation action, assistant/user
avatars, rounded message bubbles, and a pill-shaped composer. Gateway controls
and status are at the top of Settings, with activity logs in its **Logs** submenu.
Model configuration is managed in **Models**. The CM0 backend keeps the
existing Linux helper path; the Android WebSocket protocol and service bridge
are documented in [docs/picoclaw-fui-port.md](docs/picoclaw-fui-port.md).
