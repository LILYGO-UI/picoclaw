# picoclaw_fui 迁移说明

当前页面参考 `sipeed/picoclaw_fui` 的 Flutter 实现（已核对 commit `d689c94`），重点对应
`lib/src/ui/chat_page.dart`、`dashboard_page.dart`、`config_page.dart` 和
`log_page.dart`。LVGL 的目标视口为 320x568、568x1232、1232x568 和
1024x768，因此布局使用 Flex 和滚动容器，不复制 Android 的整套像素坐标。

## 聊天页映射

| Flutter UI | LVGL 实现 |
| --- | --- |
| AppBar 连接状态 pill | `picoclaw_connection_badge` |
| 新建对话按钮 | `picoclaw_new_chat`，保留旧记录并创建独立会话 |
| 历史会话 | `picoclaw_history`，切换并继续已有会话；记录自动保存，重启后恢复 |
| assistant CircleAvatar | `picoclaw_assistant_avatar` |
| user CircleAvatar | `picoclaw_user_avatar` |
| 16px 圆角消息气泡 | `picoclaw_assistant_message` / `picoclaw_user_message` |
| 24px 输入框和发送按钮 | `picoclaw_message_input` / `picoclaw_send_button` |

Android 客户端使用的 Pico Protocol 为：

```text
ws://127.0.0.1:18790/pico/ws?session_id=<uuid>
Authorization: Bearer <token>
message.send -> message.create/message.update
typing.start/typing.stop/error/pong
```

当前 CM0 版本默认通过 `PicoClawClient` 的异步 helper 执行 `ask`，这样可以在
Linux/CM0 上复用现有的进程生命周期和安全的 stdin 传参。设备部署可设置
`PICOCLAW_TRANSPORT=websocket`，启用内置的无第三方依赖 POSIX WebSocket
传输；`PICOCLAW_GATEWAY_TOKEN` 用于 Authorization Bearer。消息状态已经按
Flutter 端拆分为发送中、思考中、成功回复和错误回复，传输层与 Model/View
保持解耦。

## 运行与配置映射

Flutter 端的 Dashboard 运行控制和状态集中到 Settings 最上方，Logs 通过 Settings
二级菜单进入，可单独滚动查看和刷新活动日志，返回时回到 Settings。
一级导航为 Chat、Models、Channels、Settings，聊天页保留连接状态和新建会话操作。
Settings 保存语言偏好，模型配置统一在 Models 编辑，不再重复提供 MODEL 配置。
Gateway 状态、启停和刷新操作位于第一组，其下状态分组展示 Gateway address。
Models 页面参考 Web 管理端的 `/models` 卡片和编辑表单：
模型按 OpenAI、Anthropic、Google Gemini、DeepSeek、Ollama
五组排列，每组下方提供「＋」添加配置。组内竖屏单列、宽屏两列。新增、编辑、导入和
切换模型由独立的模型记录支持，API Key 默认隐藏，保存按钮固定在顶部。
Flutter 的 gateway 端口为 18790，Web 管理端口
为 18800；当前 `runtime.url` 表示 gateway 地址，不能当作 Web 管理端地址。

Android 服务使用 Foreground Service 启动 `libpicoclaw.so` 和
`libpicoclaw-web.so`。CM0 仍使用原生 Linux PicoClaw 和 `picoclaw-control.sh`，
不依赖 Docker 或 Android MethodChannel；目标板不提供 Web 管理服务，
`web_url` 保留为兼容的 Web 端点元数据，不在 Settings 状态分组展示。

## Settings 映射

LVGL Settings 不显示 `ConfigPage` 的 Address 区域对应的 CONNECTION 分组，
也不提供 Web host、Web port 和 Public access 控件，因为目标板没有 Web 管理服务。
Language 继续显示并保存，Gateway 状态、启停和刷新保持不变；设置页不提供 WebSocket 端口配置。
`PicoClawSettings` 仍兼容原有配置，字段对应关系为：

| FUI 字段 | LVGL 字段 | 默认值 |
| --- | --- | --- |
| host | `host` | `127.0.0.1` |
| port | `port` | `18800` |
| public mode | `public_mode` | `false` |
| language | `language` | `en` |

Settings 不再显示 RUNTIME 路径/参数分组。已有配置文件中的 `host`、`port`、
`public_mode`、`binary_path` 和 `arguments` 保持兼容，保存语言偏好时保留这些值。

配置持久化到 `$PICOCLAW_UI_SETTINGS`；未设置时使用
`$XDG_CONFIG_HOME/lilygo/ui/picoclaw/ui-settings.conf`。该文件不保存 API key，
由 helper 在 `status` 时读取旧 host/port 配置并生成兼容的 Web endpoint 元数据。
旧 Web port 当前只用于保存兼容配置和生成 `web_url`，不改变服务监听端口；
当前 helper 不启动独立 Web 服务，重启 Gateway 也不会应用此端口。
界面采用固定的 iOS 风格浅色设计，不再提供多主题选择。旧版本配置中的
`theme=` 行会被安全忽略，其余字段照常加载，并在下次保存时移除该行；
Language 仍按原有流程保存。
