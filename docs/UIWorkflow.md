# Shinkou Engine UI 专用工作流

这份文档是编辑器 UI 相关任务的项目级入口。它服务于 Windows 桌面引擎编辑器：窗体、标题栏、原生菜单和系统快捷键保持 Windows 语义；引擎内部 UI 采用扁平、克制、类似 Apple 的视觉语言，不把界面做成 macOS 仿制品，也不把编辑器做成网页卡片墙。

## 先分清 UI 边界

当前仓库的编辑器权威路径已经收敛到引擎内 retained UI；不要把历史独立
`ui/` 控件库的文档、类型或宿主重新接回引擎：

| 路径 | 作用 | 关键目录 |
| --- | --- | --- |
| `shinkou::ui` | 引擎侧 retained runtime、主题、输入桥、ImGui 适配和 `UiRenderList` | `engine/include/shinkou/ui`、`engine/src/ui` |
| `shinkou::editor::EditorUi` | 编辑器 shell、停靠布局、资源管理器、检查器、视口 overlay 和输入命中 | `engine/include/shinkou/editor/EditorUi.h`、`engine/src/editor/EditorUi.cpp` |
| `shinkou::editor::RenderView` | 视口物理像素 seam、坐标转换、正交/透视相机和网格几何 | `engine/include/shinkou/editor/RenderView.h`、`engine/src/editor/RenderView.cpp` |

历史 `shinkou::uikit` 独立库不属于当前构建图；发现旧引用时应删除或迁移到上述
边界，而不是增加第三套 widget/runtime。

当前编辑器的 retained UI 接入路径是：

```text
Engine::tick
  -> EditorLayer::process_input
  -> EditorUi::process_input -> UiRuntime::dispatch
  -> EditorLayer::draw -> EditorUi::build / UiRenderList
  -> Renderer::set_editor_viewport + set_editor_ui_render_list
  -> Renderer::submit
  -> IRenderBackend::render_editor_ui
  -> Windows 窗口像素
```

`RenderList` 有命令不等于窗口有像素；宿主生成、渲染器提交、后端绘制必须分别验证。

## 四个专用 skill

- `$shinkou-ui-design`：定义扁平视觉、Windows 窗体边界、编辑器排版、主题 token 和控件状态。
- `$shinkou-ui-integration`：追踪编译宏、生命周期、输入坐标、RenderList 生命周期和后端绘制。
- `$shinkou-ui-performance`：拆分帧耗时，控制整树重建、布局、文本、资源创建和 GPU/CPU 同步。
- `$shinkou-ui-visual-qa`：执行 headless 合约、宿主命令、实际窗口像素、DPI 和视觉回归检查。

## 标准工作流

### 1. 现状勘察

开始前阅读：

- `engine/docs/UISystem.md`
- `engine/docs/EditorUI.md`
- `engine/include/shinkou/editor/EditorUi.h`
- `engine/src/editor/EditorUi.cpp`
- `engine/src/editor/EditorLayer.cpp`
- `engine/src/render/Backends.cpp`

记录当前 UI 所属 namespace、启动目标、编译宏、渲染后端、窗口客户区尺寸/DPI、是否启用 ImGui，以及现有测试目标。不要以独立 Studio 能显示为理由推断引擎接入已经正确。

### 2. UI 设计契约

在改代码前写清楚：

1. shell 区域：顶部命令栏、左侧层级、中央信息/场景视口、右侧检查器、底部资源/控制台/诊断、状态栏；
2. 语义 token：窗口、普通 surface、提升 surface、正文、次要文字、强调色、危险色、分隔线；
3. 组件状态：normal、hover、pressed、focus、disabled、selected、checked；
4. 窄窗口时的收缩顺序和最小面板尺寸；
5. 100%/125% DPI、亮色/暗色/高对比度验收条件。

默认视觉约束：面板以色阶和 1 px 分隔线分层，窗体表面基本方正，圆角主要留给按钮和输入框；只保留一个主要动作，减少无意义阴影、渐变、漂浮卡片和过度动画。中心视口应是默认视觉重心。

### 3. 最小可见切片

先实现一个可证明的垂直切片：窗口背景、工具栏、一个左侧面板、中心视口占位、一个右侧面板、状态栏，以及至少一条文本和一个按钮。切片必须能通过同一条引擎渲染路径到达窗口，再扩展层级树、检查器、资源和诊断内容。

### 4. 接入验证

按以下顺序排查，不跨层猜测：

1. `EditorUi::build()` 后 `UiRenderList` 非空，且包含文本命令；
2. `EditorLayer::draw()` 设置当前帧的 UI list；
3. `Renderer::submit()` 在本帧捕获并清空该 list 指针；
4. 选定后端确实 override `IRenderBackend::render_editor_ui()`；
5. 后端绑定的是窗口当前 render target，且没有在 UI 后清屏或覆盖；
6. 后端没有返回错误，窗口客户区坐标与 UI layout 坐标一致；
7. 鼠标事件在 resize/DPI 变化后仍命中可见控件。

当前代码的已知边界必须明确写进每次报告：UIKit 原生绘制实现位于 Windows/D3D11 路径；基础接口的默认 `render_ui()` 是空操作，Vulkan、D3D12、Null/headless 不能因命令生成成功就宣称“已显示”。若需要跨后端支持，应单独设计 GPU UI batch 或对应后端 adapter，不要用静默空操作伪装兼容。

### 5. 性能验证

把一次帧拆成：模型同步、树重建/patch、布局、输入命中、paint 命令生成、渲染提交、文本/媒体工作。至少比较 idle、hover、输入筛选、选择对象、滚动长列表、切换面板和 resize。

重点关注当前实现的风险点：

- `EditorUi::build()` 使用 paint key 复用 retained `UiRenderList`；稳定帧不得因为 hover 以外的无关数据变化重建全部几何；
- 资源、层级和控制台均必须使用有界快照或可视范围，不能在 paint 热路径创建屏外控件；
- Windows/D3D11 的 retained compositor 复用 GPU UI surface；其 Direct2D raster/upload、composite 和 present 时间必须与布局、文本生成分开测量；
- 持久化、文件扫描、缩略图、媒体解码、shader 编译和设备等待不得出现在 paint 热路径。

目标是正常 Windows 编辑器在 60 FPS 下稳定运行、交互不会造成连续多帧卡顿；若硬件或场景需要放宽目标，报告中必须给出测量数据和原因。性能优化不能通过隐藏 UI、跳过错误或让命令列表为空来达成。

### 6. 视觉回归

最小矩阵：

| 场景 | 检查重点 |
| --- | --- |
| 1280x720，100% DPI | 默认排版、中心视口和所有文本 |
| 1600x900，125% DPI | 缩放、命中测试、字体与面板比例 |
| 约 1024x768 | 收缩、最小宽度、是否重叠/空白 |
| 亮色 / 暗色 / 高对比度 | token 对比度、状态可见性 |
| D3D11 / 其他后端 | 实际像素与明确的能力报告 |

每次 UI 改动至少提供：构建配置、后端、窗口尺寸/DPI、widget/command/text 统计、运行的测试及结果、实际截图或说明无法截图的原因。单元测试只能证明数据契约；截图只能证明视觉结果；两类证据需要同时保留。

当前项目提供 `Tools/UiCapture` 作为实际窗口验收工具：

```text
shinkou_ui_capture.exe shinkou_engine_sample.exe editor.bmp 5000 dx11
```

Shinkou/D3D11 窗口优先通过 `SHINKOU_UI_CAPTURE_PATH` 读取 swapchain 的 GPU
client-surface；普通 Windows 程序再依次使用 WindowDC、BitBlt 和 PrintWindow。
因此不能把被遮挡窗口的 GDI 空白图误判为引擎 UI 空白。捕获工具同时打印窗口标题、
尺寸、采样方式和输出路径，截图应使用 `view_image` 进行像素检查。

## 合并前硬门槛

- 不再出现“独立 UI 编辑器可用，但引擎只接了模型/命令没有像素”的状态；
- 不得重新引入已废弃的独立 `shinkou::uikit` 运行时，或把历史类型混入当前 retained 路径；
- 默认窗口仍使用 Windows 窗体和菜单语义；
- 亮/暗/高对比度和至少两种 DPI 尺寸下无文本裁切、重叠或不可操作控件；
- 输入、绘制、后端呈现和性能问题都有对应回归证据；
- 新的视觉规则落在 Theme/StyleSheet/共享组件层，不能散落为 renderer-specific magic numbers；
- 若某后端尚未支持 retained UI，必须显式报能力缺失或错误，不得静默空白。

## 参考实现与 Shinkou 取舍

这套工作流参考了几个成熟开源引擎的边界，而不是复制它们的渲染实现：

- Godot 的 `Control`/`Container` 把最小尺寸、期望尺寸、最大尺寸和 size flags 作为布局契约，子控件尺寸变化会触发父容器重新排序；Shinkou retained runtime 对应使用 `layout`、`flex`、`min/max` 和显式 layout invalidation。参考：[Godot Control](https://github.com/godotengine/godot/blob/master/scene/gui/control.h)、[Godot Container](https://github.com/godotengine/godot/blob/master/scene/gui/container.cpp)。
- Bevy UI 将 Focus、准备、传播、布局、后布局和层叠拆成明确阶段，并同时提供 Flexbox/Grid 与逻辑 UI 缩放；Shinkou 当前以 `UiContext::dispatch`、`layout`、`tick`、`paint` 和 backend presentation contract 形成等价阶段。参考：[Bevy UI pipeline](https://github.com/bevyengine/bevy/blob/main/crates/bevy_ui/src/lib.rs)。
- O3DE LyShine 的演进方向强调 authored canvas/tree 与运行时实体上下文分离，且不把编辑器预览直接当成运行时渲染验证；Shinkou 因此保留 `EditorUi`、`UiRenderList` 和引擎 backend adapter 三个边界。参考：[O3DE UI prefab RFC](https://github.com/o3de/o3de/issues/19910)。

当前实现的能力选择是显式的：Windows/D3D11 使用原生 Direct2D retained command path；ImGui 是独立可选 adapter；D3D12、Vulkan 未实现对应 retained renderer 时必须报告能力缺失。`Renderer::submit()` 还会在没有场景 Present pass 时创建 UI-only terminal pass，确保编辑器不会依赖某个 sample 的场景回调才能显示 UI。

验收记录不得只写“能启动”：必须同时保留 command/text/backend trace、实际截图和测试结果。

## 资源图标与资源视图契约

资源管理器的图标不是运行时拼接文字，也不是依赖 Windows 文件关联图标。源资源位于
`engine/resources/editor/icons/*.svg`，由 `AssetIconLibrary` 在初始化阶段读取一次，解析成
带语义颜色角色的归一化路径；paint 阶段只把不可变路径引用写入 `UiRenderList::Path`。
Windows/D3D11 后端再将路径映射为有上限的 `ID2D1PathGeometry` 缓存。这样文件系统 IO、SVG
解析、几何创建和每帧分配都不进入资源列表的热路径。发布包把同一目录安装到
`share/shinkou/editor/icons`；`SHINKOU_EDITOR_ICON_ROOT` 可供其他平台或定制主题覆盖资源根。

资源浏览器必须提供三种互斥视图，并保持同一套文件操作命中区域：

- `SmallList`：当前目录的紧凑列表，每行只显示矢量图标和名称；
- `LargeIcons`：网格大图标视图，每格显示矢量图标和名称；
- `Tree`：递归树视图，使用折叠箭头、层级导线、缩进、矢量图标和名称。

三种视图均不显示文件大小列。过滤、排序、相对路径和图标类别索引按文件修订号、视图、
过滤词和折叠集合缓存；paint 只消费缓存后的可见范围。

## 交互反馈与局部重绘

hover、pressed、focus、selected 必须有可辨认但克制的背景/边框变化。纯状态变化记录旧、
新 region 的并集为 dirty rect；D3D11 后端只在 Direct2D 目标上清理和重绘该逻辑区域，并以
对应物理像素矩形调用 `UpdateSubresource`。布局、主题、资源内容、停靠拖动和选择等可能
影响多个面板的操作必须升级为 full repaint。稳定帧使用内容 hash 跳过 UI surface 重绘、
上传和 DirectWrite 绘制。文本格式按字体、字号、对齐和溢出策略缓存，避免点击一次就反复
创建/释放 DirectWrite COM 对象。
