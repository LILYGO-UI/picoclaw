# LILYGO UI 开发规范

本文是模板及由模板创建的独立应用的唯一 UI 开发规范。AI agent 新增或
修改页面、组件、布局、样式、文案或字体时必须遵守本文。

本文中“必须”和“不得”是强制要求，“应”是默认实现方式。开始编辑前，
必须先检查当前页面、`src/components/` 中的现有能力和相关测试，不得假设
仓库提供了尚未实现的基类或响应式辅助 API。

## 架构边界

| 关注点 | 位置 | 规则 |
| --- | --- | --- |
| 页面状态和 UI | `src/pages/<page>/` | 页面私有的 Model、ViewModel 和 View 放在同一目录。 |
| 共享领域规则 | `src/domain/` | 仅在业务对象被多个页面实际共享时创建，不得依赖 LVGL。 |
| 通用组件和绑定 | `src/components/` | 复用现有组件和 Subject RAII 封装，明确所有权和销毁顺序。 |
| 生命周期和路由 | `src/app.cpp`、`src/app_router.*` | 应用生命周期和全局导航不得由页面 View 持有。 |

依赖方向必须是：

```text
View -> ViewModel -> Model
```

- Model 保存业务状态和领域规则，必须能在不初始化 LVGL 的情况下测试。
- ViewModel 将 Model 转换为展示状态，通过 Subject 发布变化，并提供用户命令。
- View 只负责 LVGL 对象创建、布局、事件转发和状态绑定，不得直接修改 Model。

数据流必须保持单向：

```text
用户输入 -> View 回调 -> ViewModel 命令 -> Model 更新
         -> Subject -> View 绑定或 observer -> LVGL 渲染
```

## 视觉规范

界面采用固定的 iOS 浅色视觉语言，不提供运行时主题切换。页面和组件必须复用
`src/components/components.hpp` 中的语义颜色，不得在页面中重复定义相同用途的颜色。

| 颜色角色 | 用途 | 色值 | 透明度 |
| --- | --- | --- | --- |
| 页面背景 | 页面根容器和分组区域 | `#F2F2F7` | `100%` |
| 容器背景 | 导航栏、面板和卡片 | `#FFFFFF` | `100%` |
| 容器标题/正文 | 标题、正文和主要数值 | `#1C1C1E` | `100%` |
| 次要文字 | 辅助说明、元数据 | `#636366` | `100%` |
| 分隔线 | 卡片内分隔线和边界 | `#D1D1D6` | `100%` |
| 主操作/强调 | 主要按钮、链接和选中态 | `#007AFF` | `100%` |
| 强调浅色 | 聚焦环和选中背景 | `#EAF3FF` | `100%` |
| 控件背景 | 输入框、次要控件 | `#EFEFF4` | `100%` |
| 危险/成功 | 错误和完成状态 | `#FF3B30` / `#248A3D` | `100%` |
| 日志背景/文字 | 日志滚动区域 | `#F7F7FA` / `#1C1C1E` | `100%` |

卡片使用白色、16px 圆角和无阴影平面；按钮使用 12px 圆角，按下时显示较深蓝色，
禁用时降低不透明度，键盘或辅助操作聚焦时显示浅蓝色描边。用户消息气泡使用
`#007AFF` 配白色文字，助手消息气泡使用白色配主要文字。不得使用渐变、材质阴影
或其他主题调色板替代这些角色。新增共享颜色时必须先定义语义角色、使用场景、
色值和透明度，并提升到 `src/components/`。

Settings 使用 iOS inset grouped 列表：分组标题位于白色圆角组外，组内用左侧标签、
右侧值和内缩细分隔线组织内容。正文与编辑值统一为 17px，长地址使用上下两行，
不再给每个输入框单独添加灰色底框。Gateway 运行控制和状态位于最上方，其他偏好分组展示；
模型配置统一在 Models 编辑。主导航仅提供 Chat、Models、Channels 和 Settings；
Logs 通过 Settings 二级菜单进入，返回操作回到 Settings。
主导航采用 iPhone 标签栏样式：白色背景与朝向内容的 1px 分隔线，图标上方、12px 标签下方，
当前页面的图标和标签为蓝色，其余为次要文字灰色；按压使用浅色背景反馈，键盘聚焦提供描边。
Chat 使用聊天气泡、Models 使用模型芯片、Channels 使用连接节点、Settings 使用齿轮，
图标由 LVGL 绘制，不依赖字体中的私有图标字形。竖屏底栏高 72px，四项均分可用宽度；
宽屏复用相同图标和标签作为 84px 右侧导航，每项命中高度 60px。软键盘打开时收起导航。
模型/通道编辑仍选中所属标签，Logs 选中 Settings。
状态分组保留 Gateway address；Language 和 Save 用于保存语言偏好。
目标板不提供 Web 管理服务，Settings 不显示 CONNECTION 分组及 Web host、Web port、Public access，
也不显示 RUNTIME 路径/参数分组。保存偏好时保留旧配置字段。
顶部 Settings 标题和 Save 操作保持可达。组宽度取滚动区域实际内容宽度，最大 680px 并居中，
不得按外层容器宽度计算导致右侧裁切。

Models 页点击配置条目进入编辑，列表不再摆放 Edit/Use 按钮。条目与左滑删除按钮均使用
16px 圆角，并裁切滑动内容的外侧圆角。编辑页复用 Settings 的分组行样式，提供可编辑的
Provider name、API base、API key/Test、Model/Fetch；供应商名称用作编辑页和配置条目的标题。
Model 使用同一个输入框进行手动填写与下拉选择，旁边提供 Fetch，不再单独显示 Model ID 行。
保存后列表自动定位到刚保存的配置，展示最新字段；状态刷新不得追加同名旧配置。
编辑页不提供别名、映射和 Delete model 控件，内部模型标识自动维护，删除通过列表左滑操作。
长地址单独占行，小屏上的输入控件与操作按钮仍保持可达。

Channels 页以可滚动列表显示通道名称和已保存的 Enabled/Disabled 状态，点击条目
进入带通道说明的配置表单。配置状态不得冒充实时连接状态。编辑页沿用 Settings 的分组行样式，
使用真实的 Enabled 开关，按通道显示所需字段，token 和 secret 使用密码输入框。
相关字段按 Credentials、Security、Access、Behavior、Connection、Webhook 和 Advanced
分组；分组标题和字段顺序参考 PicoClaw Web 配置页，避免把凭据、访问控制和网络地址混在一组。
Save 和返回固定在顶部，异步保存期间防止重复提交；保存失败显示错误并保留草稿。
保存成功须提示手动重启 Gateway 生效，不能暗示配置已立即应用或连接测试已通过。
列表刷新读取最新配置，不得覆盖正在编辑的草稿。键盘、滚动及横竖屏切换都必须保留输入，
并确保 Save 和当前聚焦字段可达。

微信编辑页额外显示 Bind Weixin、二维码、绑定状态、账号摘要和 Cancel 控件。点击绑定后，
ViewModel 通过独立 helper 异步获取腾讯 iLink 短时二维码，每两秒轮询 `wait`、`scaned`、`confirmed` 或
`expired` 状态；二维码只在等待或扫码状态显示。确认结果只更新草稿中的 token、account_id
和必要的 base URL，用户仍需 Save；取消、过期和失败不得清空已有草稿。二维码区域在小屏可滚动，
键盘聚焦代理字段时 Save、返回和字段保持可达。绑定会话随返回或切换页面取消，不能在销毁后的
View 上继续更新。

## 字体与文本

- 显示文本的 UI 源文件必须包含 `<cm0/typography.h>`。除下述应用专用字体场景外，
  LVGL 文本对象必须使用 `lilygo_ui_font_get(size)` 返回的 AppKit 字体。
- 必须在 `lv_init()` 完成后获取字体。`14`、`22`、`28`、`36` 和 `48` 像素是
  推荐的基础字号；产品层级或布局确有需要时，可向 `lilygo_ui_font_get(size)`
  传入其他正整数字号。
- AppKit 在首次请求某个字号时创建并缓存对应的完整字体链，后续相同字号复用该
  缓存。不得根据视口尺寸连续计算字号，也不得为相近文本层级引入大量不同字号；
  每个不同字号都会占用额外字体缓存，直至字体系统反初始化。
- 设置文本样式前必须确认字体指针非空。返回的字体对象及其 fallback 链由 AppKit
  共享和管理，应用不得修改或释放。
- 相同信息层级在同一应用中应使用一致的字号。缩小字号不得作为修复溢出的
  首选方案；应先精简文案、调整布局，或按产品要求换行和截断。
- 不得在公共 UI 中引用 `lv_font_montserrat_*`，也不得复制、改名或随应用重新打包
  AppKit 字体。

AppKit 在运行时通过 LVGL FreeType 加载共享字体文件。`lilygo_ui_font_get()` 返回
以 Inter 为主字体、Source Han Sans CN 为中文 fallback、Font Awesome 为图标字形
fallback 的字体链。中西文混排必须使用同一个字体链，不得为了切换字体拆分标签，
也不得依赖操作系统字体发现或默认字体回退。

### 应用专用字体

仅当 AppKit 运行时字体确实缺少应用所需字形时，才可在 `assets/fonts/` 提供
应用专用的 TTF、OTF 或 WOFF 字体。不得复制或重新打包 AppKit 已提供的 Inter、
Source Han Sans CN 或 Font Awesome 字体。应用专用字体必须满足：

- 通过 `lv_freetype_font_create()` 按当前 UI 字号在运行时加载；
- 将应用专用字体作为引用它的 LVGL 对象的主字体，并将其 `fallback` 设为同字号的
  `lilygo_ui_font_get(size)` 结果；
- 不得修改 `lilygo_ui_font_get()` 返回的共享字体对象或它的 fallback 链；
- 字体对象的生命周期必须不短于引用它的 LVGL 对象；移除所有引用后，必须通过
  `lv_freetype_font_delete()` 释放；
- 字体文件必须通过应用的安装和打包规则部署到稳定的运行时路径；
- 可通过可重复的子集化流程仅保留所需字形以减小安装体积，但不得手工修改生成的
  字体文件。

AppKit 的 Inter、Source Han Sans CN 和 Font Awesome 字体按 SIL Open Font License 1.1
分发。应用不得将它们描述为“无版权字体”或单独销售。包含字体的分发物必须
保留 `NOTICE.md` 和 `OFL-1.1.txt`；模板通过 AppKit 安装辅助函数包含共享字体的
许可文件。新增应用专用字体时，必须同时记录字体来源，并补充该字体的许可文件
和打包规则。

## 响应式布局

页面沿用 `picoclaw_fui` 的 600dp 移动端断点：320x568 和 568x1232 使用紧凑的
纵向内容与底部标签栏，1232x568 和 1024x768 使用更宽的卡片和右侧导航栏。

- 页面主体必须使用 LVGL Flex 或 Grid，并结合百分比尺寸、`LV_SIZE_CONTENT`、
  最小/最大尺寸、对齐、间距和滚动容器分配空间。
- 不得用大量绝对坐标组织页面主体，不得为横屏、竖屏或不同尺寸复制整套
  页面。
- 固定像素值只能用于有明确约束的尺寸，例如图标、边框、间距、控件下限和设计
  token。
- 连续尺寸变化应由 layout 自动处理。只有导航方向、栏数、内容优先级或交互
  方式发生结构变化时才设置断点。
- 断点必须依据父容器的实际可用宽高，不得依据 CPU 架构、设备型号或应用展示
  模式。可复用组件不得假设自己占满 display。
- 尺寸变化的布局更新必须可重复执行，不得把重建整个页面作为常规适配方式，
  并必须保留业务状态、焦点、滚动位置和用户输入。

响应式结果必须保证：

- 文本按产品要求换行或截断，不与相邻内容重叠；
- 主要操作始终可见、可达，并有足够的命中区域；
- 内容增多、文案变长或字体回退时，容器可扩展、重排或滚动；
- 横竖屏和尺寸切换后业务状态和用户输入不丢失。

## LVGL 对象和生命周期

- View 必须在调用者传入的 `parent` 下创建顶层对象，不得假设 parent 是当前
  screen 或整个 display。
- 页面 View 必须保存自己的顶层 LVGL 对象，并提供可重复调用的销毁流程。
- LVGL 父对象负责删除子对象。销毁父对象后必须清空所有缓存的子对象指针，
  不得重复删除。
- 事件回调使用 `user_data` 时，其指向对象的生命周期必须覆盖事件注册期间。
- 优先使用 LVGL 对象绑定的 observer。全局 observer、输入监听器、timer 和其他非对象
  绑定资源必须在销毁时显式注销。
- ViewModel 必须比绑定其 Subject 的 View 存活更久。Subject 应使用现有 RAII 封装，
  不得复制或移动底层 `lv_subject_t`。
- 延迟页面切换时，不得在当前 LVGL observer 或事件回调中同步删除触发回调的
  页面。
- 需要被渲染测试定位的关键对象必须设置稳定、有语义的 `lv_obj_set_name_static()`
  名称。

## 新增页面

新增页面前必须先检查 `src/pages/picoclaw/` 和现有测试。一个完整页面通常包含：

1. `src/pages/<page>/<page>_model.cpp/.hpp`：页面私有状态和领域规则；
2. `src/pages/<page>/<page>_view_model.cpp/.hpp`：命令、展示状态和 Subject；
3. `src/pages/<page>/<page>_view.cpp/.hpp`：LVGL 对象、绑定、回调和响应式布局；
4. `src/app.cpp` 中的创建和销毁，多页面应用则按需新增 `src/app_router.cpp/.hpp`；
5. Model、ViewModel 和用户可见行为的测试；
6. 修改后的预览图和用户文档。

页面私有 Model 与 ViewModel/View 放在同一目录，但仍必须编译到不同依赖层。纯 Model
代码不得因为目录内聚而链接 LVGL。

## 新增通用组件

只有组件被多个页面使用，或它明确承担通用绑定、样式或资源生命周期时，才放入
`src/components/`。否则应保持在所属页面目录。

通用组件必须：

- 在调用者传入的 parent 下构建；
- 以父容器的可用尺寸进行响应式布局；
- 复用语义颜色和 AppKit 字体；
- 声明所有权、回调 `user_data`、observer、timer 和销毁责任；
- 在最长文案、字体回退和支持视口下不发生重叠、裁切或不可达操作。

## 验收

每次 UI 修改必须至少完成：

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

现有测试包括：

- `picoclaw-core`：不初始化 LVGL 的 Model 和异步客户端测试；
- `picoclaw-view-model`：ViewModel 命令和 Subject 发布测试；
- `picoclaw-render-compact-portrait`：`320x568` 小屏渲染、键盘和频道配置交互测试；
- `picoclaw-render-portrait`：`568x1232` 竖屏渲染和交互测试；
- `picoclaw-render-landscape`：`1232x568` 横屏渲染和交互测试；
- `picoclaw-render-desktop`：`1024x768` 桌面渲染和交互测试。

测试通过不能替代视觉检查。AI agent 在修改 UI 时还必须检查横竖屏渲染产物，
并确认：

- 布局不依赖单一固定分辨率；
- 文本无缺字、错误回退、重叠或裁切；
- 主要操作可见、可达，按钮文案和命中区域正常；
- 尺寸或方向变化后布局正确重排，业务状态、焦点、滚动位置和用户输入保留；
- 页面背景、容器、文本、边框和主操作使用正确的语义颜色；
- 关闭页面后无野指针、重复删除、未注销 observer、timer 或输入监听器。

修改 SDK、工具链、安装或打包相关 UI 资源时，还必须完成 `cm0-cross` 构建和
Debian 打包验证。
