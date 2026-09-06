# Shinkou Engine 编辑器 UI 平台起草档案（历史）

> 本档案早于 retained `EditorUi` 迁移，仅保留作设计历史。当前实现和验收以
> `docs/UIWorkflow.md`、`docs/UIAcceptance.md` 为准，不要执行其中保留
> `shinkou::uikit` 的旧方案。

> 状态：Draft 0.2（首个 Windows/D3D11 垂直切片已落地，待继续扩展）  
> 日期：2026-09-01  
> 适用范围：Windows 桌面编辑器、D3D11 首个完整呈现路径，后续扩展 D3D12/Vulkan  
> 当前决策：保留 `shinkou::uikit` 作为 UI 运行时原语，重写/扩展编辑器平台层，不删除 `ui/`

## 1. 目标与硬边界

本次建设的目标不是把整个窗口涂成一个游戏画布，而是建立专业引擎式的编辑器工作区：

- Windows 原生窗体、标题栏、菜单、窗口缩放和系统快捷键保持 Windows 语义；
- 编辑器客户区由稳定的 shell 管理，中心区域才是一个或多个明确的 `RenderView`；
- Hierarchy/World Outliner、Details/Inspector、Content Browser、Output Log、Profiler 等是编辑器 UI，不能被场景渲染覆盖；
- 场景渲染只写入视口声明的矩形、目标纹理和裁剪区，视口之外的像素由编辑器 shell/UI 绘制；
- 同一套模型和命令可被 UIKit 原生绘制、ImGui 兼容适配器和未来的其他适配器消费；
- 默认视觉是扁平、克制、类似 Apple 的色彩与留白，但不是仿 macOS 窗体：面板方正、1 px 分隔线、少量圆角、单一强调色、稳定密度；
- 先保证可见、可操作、可测量，再扩展高级渲染能力；Nanite、虚拟阴影贴图等不属于本阶段。

非目标：本阶段不引入新的桌面 UI 框架，不把 WPF Studio 链入引擎运行时，不复制 Unreal 的渲染实现，也不以“命令列表非空”代替实际窗口像素验收。

## 2. 对现有 UI 库的审计与取舍

### 2.1 现有资产

仓库中存在两条 UI 路径，必须保持边界：

| 路径 | 现状 | 结论 |
| --- | --- | --- |
| `shinkou::ui` | 引擎侧主题、组件模型、输入桥、ImGui 适配、媒体模型 | 保留为模型/兼容层；不直接和 UIKit 控件混用 |
| `shinkou::uikit` | `Widget` 树、`measure/arrange`、命中测试、焦点/捕获、StyleSheet、`RenderList`、CMake 测试 | 可作为编辑器 UI runtime 原语，但不能单独承担完整编辑器平台 |
| `ui/studio-wpf` | 独立 UI 设计工具 | 继续独立构建；不进入引擎运行时 |

`shinkou::uikit` 已经满足保留它所需的最小证据：它有独立静态库目标、独立测试、Windows 平台适配、保留式命令列表和当前 D3D11 原生绘制路径；当前窗口实际能看到 command/text 生成结果。因此“整个库不可用”这一判断不成立。

但它缺少专业编辑器平台的关键能力：

1. 视口不是普通装饰控件，而是带相机、渲染目标、坐标转换和渲染图依赖的 `RenderView`；
2. 停靠树目前偏数据结构，缺少实际拖拽 splitter、tab、面板生命周期和布局矩形输出的统一控制器；
3. Outliner、Details、资产和命令需要稳定快照、增量 revision、选择/撤销/命令总线；
4. UI 的逻辑坐标、窗口客户区像素、视口局部坐标、NDC、世界坐标尚未形成一份不可歧义的契约；
5. UI 后端能力矩阵不完整，D3D11 原生 UIKit 与 ImGui 回退不能代表 D3D12/Vulkan 已支持；
6. 现有部分宿主仍在绘制“视口占位网格”，没有把真实场景渲染限定到视口矩形。

所以本项目的取舍是：保留 `ui/` 的通用控件和绘制命令，重写编辑器壳和引擎视口接入；只有当替代实现通过全部回归和窗口像素验收后，才允许移除旧路径。禁止先删库再丢失现有能力。

## 3. 参考布局：Unreal 的可迁移部分

Unreal 的 Level Editor 采用清晰的职责分区：Menu Bar、Main Toolbar、Viewport Toolbar、Level Viewport、Outliner、Details、Content Drawer/Browser、Bottom Toolbar。视口是“进入世界的窗口”，Outliner 负责层级选择，Details 负责所选 Actor 属性，Content Browser 负责资产组织，底部区域承载命令行、Output Log、派生数据和版本控制状态。

本项目只迁移这些结构和工作流，不复制其品牌和实现：

```text
Windows native title/menu
┌─────────────────────────────────────────────────────────────────────┐
│ Level tabs / project context                                       │
├─────────────────────────────────────────────────────────────────────┤
│ Main toolbar: save | modes | play/pause/step | build | settings     │
├───────────────┬──────────────────────────────────────┬──────────────┤
│ Modes /        │ Viewport toolbar                    │ World        │
│ World Outliner │ perspective/orthographic | lit | gizmo│ Outliner     │
│                ├──────────────────────────────────────┤──────────────┤
│                │ RenderView: actual scene only         │ Details      │
│                │ grid/axes/gizmos/selection            │ Inspector    │
├───────────────┴──────────────────────────────────────┴──────────────┤
│ Content Browser / Asset Browser     | Output Log / Console / Profiler │
├─────────────────────────────────────────────────────────────────────┤
│ status: backend | fps | selection | dirty | input                    │
└─────────────────────────────────────────────────────────────────────┘
```

可迁移原则：视口默认占据主要面积；工具栏是紧凑的命令带；面板用停靠、标签和 splitter 组织；同一选择同步 Outliner 与 Details；视口支持 Perspective 与 Front/Side/Top 正交视图；Content Browser 支持搜索、过滤、目录和缩略图；底部栏承载诊断而不是抢占默认视口。

参考资料：

- [Unreal Editor Interface](https://dev.epicgames.com/documentation/unreal-engine/unreal-editor-interface?lang=en-US)
- [Level Editor](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-editor-in-unreal-engine)
- [Using Editor Viewports](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-editor-viewports-in-unreal-engine)
- [Viewport Controls](https://dev.epicgames.com/documentation/en-us/unreal-engine/viewport-controls-in-unreal-engine)
- [Outliner](https://dev.epicgames.com/documentation/en-us/unreal-engine/outliner-in-unreal-engine)
- [Level Editor Details Panel](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-editor-details-panel-in-unreal-engine?lang=en-US)

## 4. 分层系统构成

### 4.1 应用与生命周期

```text
EditorApplication / Engine
  ├─ WindowInputBridge          // Win32 消息、DPI、客户区尺寸
  ├─ EditorCommandBus           // 菜单、快捷键、按钮、面板命令统一入口
  ├─ EditorSelection             // 单选/多选/同步 Outliner 与 Details
  ├─ EditorWorkspace             // 停靠树、标签、splitter、浮动窗口、持久化
  ├─ EditorUiModel               // world/asset/console/diagnostic 快照与 revision
  ├─ EditorViewportController    // 相机、模式、网格、gizmo、视口输入
  ├─ EditorRenderView            // 每个视口的 target/rect/scissor/camera 描述
  ├─ shinkou::uikit              // 控件树、布局、命中测试、RenderList
  ├─ UiRendererAdapter           // UIKit native / ImGui / future GPU batch
  └─ UiTelemetry                 // 分阶段 CPU/GPU/命令/输入统计
```

引擎帧顺序固定为：

```text
Window.process_events
  -> resize/DPI snapshot
  -> InputSystem.poll
  -> EditorCommandBus + ViewportController input dispatch
  -> World update
  -> EditorUiModel.sync (revision short-circuit)
  -> RenderGraph begin
  -> for each visible RenderView: scene + grid/gizmo passes in view rect
  -> Editor shell layout/paint to UI command list
  -> Renderer.submit: graph execute -> scene present -> UI overlay -> Present once
```

任何阶段不得在 paint 热路径中扫描全项目、写布局文件、等待 GPU、编译 shader 或创建大量短命窗口控件。

### 4.2 编辑器 shell 与停靠工作区

核心区域固定为：

1. native Windows chrome/menu：系统负责窗口边框、菜单和窗口管理；
2. level/project tab strip：当前场景、脏状态、工作区；
3. main toolbar：保存、模式、运行/暂停/单步、构建、设置；
4. left dock：Modes/World Outliner；
5. center dock：Viewport tabs 和 RenderView；
6. right dock：World Outliner（可选）与 Details/Inspector；默认至少保证一个层级树和一个检查器；
7. bottom dock：Content Browser、Output Log、Profiler、Render Graph；
8. status bar：backend、DPI、FPS/frame time、selection、dirty、输入统计。

`DockWorkspace` 的责任从“序列化数据结构”扩展为“可计算布局”：输入客户区矩形，输出每个 panel 的 `DockRect`、active tab 和 splitter hit zone。每个叶子声明最小宽高；resize 时先满足最小尺寸，再按 ratio 分配；不足时按优先级折叠 bottom -> left auxiliary -> right auxiliary，不允许把中心视口压成 0。

持久化仍为版本化 JSON，保存发生在显式 save、workspace reset 或 shutdown，不发生在每帧。未知字段忽略，非法树恢复到默认布局并写诊断。

### 4.3 编辑器模型与命令

- `EditorUiModel` 只持有可渲染快照、过滤结果、选择和 revision，不在 UI paint 中直接遍历 ECS；
- `WorldOutlinerModel` 输出稳定的层级行：entity id、名称、父子关系、可见/启用、选中状态；长列表必须虚拟化；
- `DetailsModel` 根据选中对象的组件/PropertyDescriptor 输出分组、字段类型、单位、可编辑性和错误状态；
- `AssetBrowserModel` 在后台更新文件索引/缩略图，UI 线程只消费快照；
- `OutputLogModel` 使用有界 ring buffer，避免 console 无限增长；
- `EditorCommandBus` 是唯一修改入口，支持来源（menu/shortcut/toolbar/viewport/panel）、可撤销 transaction、失败诊断；
- selection 从 Outliner、RenderView、Details 三向同步，命令执行后只触发相关 revision。

### 4.4 RenderView 与实际场景呈现

每个视口必须有独立描述：

```cpp
struct EditorRenderView {
    ViewId id;
    ViewMode mode;                  // Perspective / Front / Side / Top
    Rect clientRectPx;              // 窗口客户区像素，包含 shell 偏移
    Rect viewportRectPx;            // 视口内容像素，不含 panel chrome
    Rect scissorRectPx;
    CameraState camera;
    GridSettings grid;
    GizmoSettings gizmo;
    RenderTargetBinding target;
    bool renderWorld;
    bool showGrid;
    bool showSelection;
};
```

render graph 只允许使用 `viewportRectPx` 和 `scissorRectPx`：

- 先设置视口 target、viewport、scissor；
- 场景 pass 只清理/写入视口目标（若是 swapchain 直写，则只清理 scissor 区）；
- 以 `EditorRenderView.camera` 提取场景，不能默认使用整个窗口的宽高；
- grid/axis/selection/gizmo 属于编辑器视图 overlay，必须使用同一视口坐标变换，不能用 UI 全屏坐标假画；
- shell UI 在场景 pass 后提交，UI 不应重置或清理场景目标；
- 最终 Present 只能发生一次，且必须包含 editor shell 和已渲染视口。

第一阶段可以用现有 ForwardRenderer、sprite/mesh 资源和明确的 scissor 证明真实世界被限制在视口；不要求 Nanite。没有合格场景 pass 时，必须显示“视口无场景 pass”的诊断，而不能静默显示假的全屏网格。

### 4.5 坐标系统契约

统一使用左上角原点、Y 向下的窗口/UI 像素坐标；所有转换必须显式命名：

| 空间 | 原点/方向 | 用途 |
| --- | --- | --- |
| `WindowClientPx` | 客户区左上，物理像素，Y 向下 | Win32 事件、swapchain、最终截图 |
| `UiLogicalPx` | 客户区左上，逻辑像素，Y 向下 | UIKit layout；`physical = logical * dpiScale` |
| `ViewportLocalPx` | 当前视口内容左上，物理像素 | 视口鼠标、gizmo、网格绘制 |
| `Ndc` | 图形 API 约定 | 投影后的裁剪空间，转换集中在 renderer |
| `World` | 引擎世界单位，右手/轴约定由 math/render 明确 | ECS、场景和拾取 |

基本公式：

```text
windowPx = inputClientPx
uiLogical = windowPx / dpiScale
viewportLocalPx = windowPx - viewportRectPx.topLeft
viewportUv = viewportLocalPx / viewportRectPx.size
```

只有 `viewportLocalPx` 落在视口内容矩形内时，事件才进入 `ViewportController`；面板先消费自己的事件。窗口 resize 或 DPI 变化时，先更新 snapshot，再 layout，再 hit-test，再 dispatch，不能沿用上一帧矩形。

透视视图使用 camera position/orientation + vertical FOV + near/far + aspect；正交视图使用 axis（Top=Z、Front=X、Side=Y）、orthographic size、中心点和 near/far。网格步长随 zoom 使用 1/2/5 级别选择，主线、次线、X/Y/Z 轴使用语义颜色；不得把一个固定 32 px 十字线冒充真实世界网格。

### 4.6 UIKit 与后端适配

`RenderList` 是编辑器 shell 的 retained frame command list，生命周期仅到本帧 `Renderer::submit()` 完成。Windows/D3D11 当前实现采用“Direct2D CPU DIB + 一次 GPU 纹理上传 + 一次 premultiplied-alpha fullscreen 合成”的稳定适配器：D2D 不直接绑定 DXGI swapchain，避免 D2D/DXGI target ownership 导致 UI 黑屏或设备错误；UI 资源在首帧建立，后续帧只重用 brush/font/GPU state 并上传一张 UI 层。微软对 Direct2D/D3D11 互操作也要求明确设备/target 生命周期，详见 [Direct2D and Direct3D interoperation](https://learn.microsoft.com/en-us/windows/win32/direct2d/direct2d-and-direct3d-interoperation-overview)。

`RenderList` 与场景提交顺序固定为：scene pass -> editor viewport lifetime marker -> `ui_overlay_present` -> one Present。编辑器路径将 `RenderView` 的物理客户区矩形转成 D3D11 viewport/scissor，并通过 strict target seam 绑定窗口 backbuffer；shell 的其余区域由 retained UI 控制。捕获工具优先尝试完整 GPU readback，在当前机器的驱动拒绝 CPU-readable D3D11 staging 时，保存同帧 `EngineUiDib` 作为明确标注的 UI-only 视觉证据，不将其冒充完整场景截图。

后端能力明确列出：

| 后端 | UIKit native | ImGui fallback | 当前阶段结论 |
| --- | --- | --- | --- |
| Windows/D3D11 | 已有 Direct2D/DXGI 路径 | 可选 | 首个完整验收后端 |
| D3D12 | 未完成 | 依构建/适配器 | 必须显式报告，不得假称可见 |
| Vulkan | 未完成 native UIKit | 依构建/适配器 | 必须显式报告 |
| Null/headless | 不呈现像素 | 不呈现像素 | 只做命令/模型合约测试 |

UI adapter 必须保证：命令计数、文本计数、后端、DPI、客户区尺寸、第一条错误都可观测。D3D11 路径应批量/缓存 brush、字体和 clip 状态，避免每条命令创建资源或无条件 Flush；后续考虑统一 GPU UI batch，但不能为了性能删掉 UI。

## 5. 视觉规范

- surface 层级：`window`、`surface`、`surface-elevated`，通过色阶和 1 px separator 区分；
- 正文、次要文字、强调、危险、选中、焦点都使用 semantic token；
- 顶部只保留一个明显 primary action（运行/播放）；保存、单步、工具按钮为 secondary；
- shell panel 基本方正，按钮/输入 3–4 px 圆角；不使用渐变、玻璃拟态、厚重阴影和卡片墙；
- 行高、padding、separator 和图标/文字基线统一，保证密集但可扫描；
- 关键操作必须有文字 fallback，不依赖 emoji 或平台随机 glyph；
- dark/light/high-contrast token 同构；100% 与 125% DPI 不允许文字裁切或控件重叠；
- 窄窗口收缩顺序必须可预测：先隐藏辅助 bottom tabs，再折叠 left auxiliary，最后缩短 right inspector；中心 RenderView 保持最小可操作尺寸。

## 6. 性能预算与测量

每帧记录以下 stage：model sync、workspace/layout、input/hit-test、paint、render submit、backend UI、GPU execute。至少测量 idle、hover、输入筛选、选中对象、长列表滚动、切换面板、resize、100%/125% DPI。

阶段目标（首个 Windows/D3D11 slice）：

- 空闲编辑器稳定 60 FPS；UI 更新不产生连续多帧整树重建；
- 稳定帧 UI 不扫描整个项目、不复制全量字符串、不等待 GPU；
- Outliner/Content/Console 的控件数与可见行数成正比，而不是与总条目数成正比；
- 普通 1280x720 场景 UI command/text 计数稳定且可解释；
- resize/DPI 只在几何确实变化时触发全局 layout；
- D3D11 UI backend 不对每个 rect/text 建立临时 GPU/D2D 资源；
- 所有优化都必须保留“可见像素”和命令/后端证据。

## 7. 并行工作包与文件边界

起草档案落盘后才允许启动以下并行智能体。每个智能体必须先读本文件和相关 skill，只修改负责范围，完成后报告测试和未解决风险。

### A：Workspace/Docking

负责 `engine/include/shinkou/editor/DockLayout.h`、`engine/src/editor/DockLayout.cpp`、对应 dock 测试和独立文档；实现布局矩形输出、splitter 拖动、tab 激活、最小尺寸、窄窗口折叠和持久化回归。不得修改 Renderer 或 UiKitPanelHost。

### B：RenderView/坐标/网格

负责新增 `engine/include/shinkou/editor/RenderView.h`、`engine/src/editor/RenderView.cpp` 及其测试；实现 Perspective/Front/Side/Top、窗口/UI/viewport/world 转换、网格级别和 scissor/viewport 描述。不得把场景渲染写进 UI paint，不得修改 DockLayout。

### C：Editor models/命令/面板数据

负责 `EditorUiModel`、selection/command 数据接口及对应测试，必要时新增同目录模型文件；实现稳定快照、revision、有界 console、Outliner/Details/Asset 数据契约。不得修改 backend 绘制。

### D：UIKit host/性能/视觉切片

负责 `engine/include/shinkou/editor/UiKitPanelHost.h`、`engine/src/editor/UiKitPanelHost.cpp`、UIKit 相关测试和共享 StyleSheet；按 Unreal 分区重排 shell，保持 UIKit 可见且虚拟化，接入 RenderView 的矩形信息但不直接实现 GPU 场景 pass。不得修改 RenderGraph。

### E：Backend/Engine integration

负责 `engine/include/shinkou/render/*`、`engine/src/render/*`、`engine/src/core/Engine.cpp` 及集成测试；把 RenderView 的 viewport/scissor/target 接入 render graph，保证 UI 在场景之后呈现、Present 一次，并明确 D3D11/D3D12/Vulkan/Null 能力。不得重写 UIKit 控件。

### F：Capture/QA/Audit

负责 `Tools/UiCapture/*`、QA 文档和新增可独立运行的验收脚本/测试；覆盖 1280x720、1600x900 125% DPI、约 1024x768、暗/亮/高对比度，输出截图与 command/backend trace。不得为了让截图好看修改业务 UI。

## 8. 合并与审计闸门

合并顺序：先 A/B/C 的纯数据和测试，再 D 的 shell，再 E 的渲染接入，最后 F 的窗口证据。每次合并都必须通过：

1. 生成：首帧有 widget、command、text；
2. 输入：resize/DPI 后坐标仍命中对应控件或视口；
3. 提交：本帧 RenderList 生命周期正确，Renderer 捕获后清空；
4. 场景：真实 scene pass 只使用 RenderView rect/scissor，shell 区域不被场景覆盖；
5. 后端：选定 backend override 生效，能力缺失显式诊断；
6. 视觉：截图中明确看到 Windows chrome、工具栏、Outliner、RenderView、Details、Content/Console、status；
7. 性能：idle/hover/scroll/resize 有 stage 统计，不能只给总耗时；
8. 回归：standalone UIKit、engine headless、host command、Windows pixel capture 全部有结果。

以下情况直接打回，不进入下一阶段：

- 用全屏游戏画布覆盖编辑器 UI；
- UI command 非空但没有真实窗口像素证据；
- 用固定 1280x720 坐标或固定十字线替代坐标系统；
- D3D12/Vulkan/Null 静默显示为空却报告成功；
- 每帧整树重建、全项目扫描、逐命令创建资源或 GPU/CPU 等待；
- 视觉变成圆角卡片墙、渐变玻璃、假 macOS chrome 或不可读的 icon-only 工具栏；
- 破坏既有用户改动、删除 `ui/` 但没有经过替代路径全量回归。

## 9. 初始验收命令

```powershell
cmake --build out/build/engine-uikit --target shinkou_engine_sample shinkou_ui_capture
ctest --test-dir out/build/engine-uikit --output-on-failure
ctest --test-dir out/build/shinkou-ui --output-on-failure
out/build/engine-uikit/Tools/UiCapture/shinkou_ui_capture.exe `
  out/build/engine-uikit/engine/shinkou_engine_sample.exe `
  $env:USERPROFILE\Desktop\shinkou-editor-ui.bmp 5000 dx11
```

最终报告必须包含实际 build directory、后端、窗口客户区尺寸、DPI、UI command/text、RenderView rect、scene pass/present pass、测试结果和截图绝对路径。当前首个垂直切片的严格审计结论：功能/模型/渲染图/窗口 UI 测试全部通过；D3D11 编辑器连续 60 帧运行通过；完整 GPU client readback 在当前驱动上仍为环境限制，捕获器已明确降级为 `EngineUiDib`，因此不能把当前机器的视觉证据标记为“完整 GPU 帧通过”。
