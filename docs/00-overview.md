# PicoClaw 应用总览

本项目是面向 LILYGO UI 的独立 PicoClaw 应用。它参考
`T-Display-K230/k230_launcher` 中的 PicoClaw 交互流程，但不引用 Launcher 源码，
也不使用原实现中的 `/root/app`、launcher preferences 或 riscv64 运行文件。

## 产品范围

应用启动时默认打开 Chat，按 Chat/Models/Channels/Settings 顺序提供四个一级界面：

- `Chat`：显示当前模型和运行状态，维护本次会话消息，并调用 PicoClaw agent。聊天页采用
  Android 客户端常见的卡片、圆角气泡和紧邻上下文栏的“新建对话”操作；点击加号会保留
  旧记录并新建独立会话，通过 History 下拉列表切换和续聊。记录自动保存，重启后恢复；
  长对话通过 Earlier messages 查看更早消息。页面通过 iPhone 风格的图标加文字标签栏提供四个页面的导航，空会话提供快捷
  提问入口，点击后会直接聚焦输入框并打开软键盘。
  顶部 Model 下拉框可直接选择已保存的模型；切换保留会话、历史位置和输入草稿，
  后端确认所选模型配置可用后才恢复发送。发送中不能切换模型。
- `Models`：新增、编辑、删除各模型的名称、别名、模型 ID、API Base 和 API Key。
  支持导入设备现有模型配置；列表仅显示密钥掩码，编辑时可临时显示。
  按 OpenAI、Anthropic、Google Gemini、DeepSeek、Ollama 顺序分组，每组下方的「＋」
  按钮添加该供应商的配置，并预填对应 API 地址与模型格式。点击配置条目直接进入编辑，
  Save 保存记录，对话页的 Model 下拉框选择用于聊天的模型。
  竖屏单列、宽屏在组内排两列，编辑表单保持单列滚动。
  编辑页采用与 Settings 一致的分组行样式，提供 API 地址、API Key 与 Test、模型选择与 Fetch；
  Custom model 可手填模型 ID，名称和别名位于详情区。获取的模型列表仅用于匹配的地址和 Key，
  更换凭据后不会混入旧结果。
  获取、选择和保存模型列表中的 ID 时不添加供应商前缀，供应商分类单独记录。
  配置行向左滑动可显示删除按钮，点击后确认删除；右滑收起，纵向滑动仍浏览列表。
  条目和删除按钮统一使用 16px 圆角。
  同时只展开一条配置，删除后保留列表位置并同步更新 Chat 的模型选项。
- `Channels`：集中配置聊天通道。列表展示通道名称和已保存的启用状态，点击条目进入
  对应表单，填写机器人 token、应用凭据、允许访问的用户及连接选项。凭据默认掩码显示，
  Enabled 开关控制通道是否启用，Save 异步保存；失败时显示错误并保留草稿。表单按
  Credentials、Security、Access、Behavior、Connection、Webhook 和 Advanced 分类，
  与 PicoClaw Web 配置页保持相同的信息层级。
  提供飞书/Lark、微信、Discord、Telegram、Slack、钉钉、QQ、WhatsApp、LINE 和 OneBot。
  列表可刷新外部修改的配置。保存后须在 Settings 中手动停止并重新启动 Gateway，使配置生效。
  微信表单提供 Bind Weixin：直接从腾讯 iLink 获取短时二维码，等待扫码和确认后把账号凭据写入草稿；
  取消、二维码过期或轮询失败会隐藏二维码并保留原草稿，确认后仍须 Save。普通频道不执行
  连接测试或测试消息，微信二维码是明确的账号授权流程。若 Gateway 正在运行，微信保存后会
  自动重启以加载新凭据。
- `Settings`：最上方集中显示 Gateway 状态、启停和刷新操作；其下状态分组保留 Gateway 地址。
  检查 GitHub Releases 并选择运行时版本，Language 和 Save 用于保存语言偏好。
  通过 Logs 二级菜单滚动查看和刷新活动日志，返回后回到 Settings。
  模型配置统一在 Models 编辑，Settings 不再重复提供 MODEL 配置。
  目标板不提供 Web 管理服务，因此不显示 CONNECTION 分组及 Web host、Web port、Public access；
  同样不显示路径和启动参数的 RUNTIME 分组。旧配置字段在保存偏好时保持兼容。

默认提供 OpenAI、Anthropic、Google Gemini、DeepSeek 和 Ollama 的配置模板，另有
OpenAI-compatible 自定义模板。模板使用 PicoClaw 的 `provider/model` 模型 ID；用户
可修改显示名、模型别名、模型 ID、API base 和 API key。Ollama 使用本地 API，默认
不需要 API key。

| 提供商 | 模型 ID 示例 | 默认 API base | 密钥 |
| --- | --- | --- | --- |
| OpenAI | `openai/gpt-4o-mini` | `https://api.openai.com/v1` | 必需 |
| Anthropic | `anthropic/claude-3-5-sonnet-latest` | `https://api.anthropic.com/v1` | 必需 |
| Google Gemini | `gemini/gemini-2.0-flash` | `https://generativelanguage.googleapis.com/v1beta` | 必需 |
| DeepSeek | `deepseek/deepseek-chat` | `https://api.deepseek.com/v1` | 必需 |
| Ollama | `ollama/llama3.2` | `http://127.0.0.1:11434/v1` | 不需要 |

## 工程结构

```text
src/domain/picoclaw_client.*             异步命令执行和进程生命周期
src/domain/picoclaw_protocol.*           Pico Protocol 会话、帧编码和事件解析
src/pages/picoclaw/picoclaw_model.*      会话、配置和运行状态规则
src/pages/picoclaw/picoclaw_view_model.* 展示状态与用户命令
src/pages/picoclaw/picoclaw_view.*       LVGL 页面、响应式布局和软键盘
scripts/picoclaw-control.sh               PicoClaw 配置与运行时控制协议
scripts/picoclaw-releases.py              GitHub 版本查询、校验、原子切换与失败回滚
```

数据只沿以下方向流动：

```text
用户操作 -> View -> ViewModel -> Model / PicoClawClient
运行结果 -> ViewModel -> Subject / revision -> View
```

后台线程不调用 LVGL。View 使用定时器在 LVGL 线程中获取命令结果，更新 Subject
并重绘消息列表。应用关闭时会先终止仍在执行的 helper 进程组，再销毁
ViewModel 和 Subject。

## 运行时与数据目录

控制脚本遵守 XDG base directory：

```text
$XDG_DATA_HOME/lilygo/ui/picoclaw/    PicoClaw runtime、workspace
$XDG_CONFIG_HOME/lilygo/ui/picoclaw/  config.json、.security.yml
$XDG_STATE_HOME/lilygo/ui/picoclaw/   activity.log、gateway.log、gateway.pid
```

CM0 的目标架构是 ARM64，deb 默认内置官方 nightly `picoclaw_Linux_arm64.tar.gz` 的核心程序。
目标板直接运行原生 Gateway，不使用 Docker。Settings 的 Updates 分组可查询所有
GitHub Releases，区分稳定版、预发布和 nightly，并选择版本更新。下载与 SHA-256、
架构及 CLI 验证完成后才切换运行时；原来运行中的 Gateway 会重启并进行健康检查，
失败则恢复旧选择与配置并尝试重启旧 Gateway。原来停止的 Gateway 保持停止。
用户安装的版本保存在 runtime 下的 `versions/`，由 `bin/active-picoclaw` 原子选择，
优先于 deb 内置版本，且无需 root 权限。更新和 Gateway 控制共享文件锁。
开发机仿真连接真实的 PicoClaw 运行时，编译 UI 不会自动安装后端。
macOS 仿真默认使用 `docker.io/sipeed/picoclaw:latest`，不会安装 Mac 原生二进制；
首次在设置页点击 `Install / update`，或运行 `sh scripts/picoclaw-control.sh install`。
配置、密钥、工作区和日志通过卷挂载保存在应用私有目录，Gateway 使用受控容器运行。
API key 写入权限为 `0600` 的 `.security.yml`，状态输出和日志不显示 key 内容。
Models 页的记录保存在同一配置目录的 `models.conf`，通过私有临时文件原子保存，权限为 `0600`。
`PICOCLAW_MODELS_PATH` 可覆盖此路径；设置了 `PICOCLAW_UI_SETTINGS` 时，默认与该文件同目录。
导入从现有 `config.json` 与对应 `.security.yml` 读取模型，已有记录和密钥优先保留。
Chat 模型选择使用已保存的配置，切换时保留其他模型、密钥与服务配置。
CLI 每条消息显式指定已确认的模型别名；本地 WebSocket 模式自动重启受控 Gateway 并核对配置。
切换失败时阻止发送，重新选择成功后恢复；远程 WebSocket 地址不使用本地切换操作。

## 响应式设计

- 竖屏设置页采用单栏卡片和纵向滚动。
- 模型供应商分组始终按顺序纵向排列，组内配置在 320x568、568x1232 下使用单列，横屏最多两列；编辑时保存和返回固定在顶部，
  API 地址和密钥单独占行，键盘展开和尺寸变化保留草稿。
- 频道列表与配置表单纵向滚动，保存和返回始终可达；小屏键盘展开后聚焦字段位于键盘上方，
  横竖屏切换保留正在编辑的值和启用状态。
- 宽屏使用右侧 84px 导航栏，设置页保持单列滚动，避免在小屏上把 FUI 表单挤成多栏。
- 聊天页的消息区使用剩余高度，输入框和主要发送操作始终可达。
- Chat 标题栏右侧放置 Model、History、连接状态和新建对话操作，移除单独的模型就绪状态卡片；
  短屏输入时临时收起 History 选择，为消息区留出空间，Model 和新建对话仍可操作。
- 软键盘作为页面布局的一部分出现，会压缩可滚动内容而不是遮挡输入框。
- 所有文本使用 AppKit 的 `lilygo_ui_font_get()` 字体链。

## 验证

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

测试覆盖纯 Model 规则、异步命令到 ViewModel 的发布、聊天/模型/频道/设置导航，以及
`320x568`、`568x1232`、`1232x568`、`1024x768` 四个视口的像素渲染。
