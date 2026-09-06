# UE 编辑器 UI 源码审计与 Shinkou 重构边界（历史）

> 本审计记录的是迁移前架构。旧 `ui/` / `UiKitPanelHost` 路径已移除；当前决策
> 和验收标准以 `docs/UIWorkflow.md`、`docs/UIAcceptance.md` 为准。

状态：源码审计第一阶段，尚未删除旧 UI，尚未开始替换实现。

日期：2026-09-01

## 1. 证据边界

当前工作区唯一 Git remote 是 `https://github.com/cuupe/Shinkou-Engine.git`。
本机没有 UnrealEngine 源码副本，也没有 `cuupe/UnrealEngine` 这个可访问仓库；对该地址执行只读 `git ls-remote` 返回 `Repository not found`。

因此本档案区分三种证据：

1. **已核实的 Epic 官方 API/架构文档**：可以作为职责和调用关系的依据。
2. **Epic 源码中的公开文件路径**：官方 API 页面列出的 Header/Source 路径，可用于对照源码。
3. **尚未取得的实现细节**：不能声称已经读取。若用户提供组织内 UE 镜像 URL、已授权源码目录或可用 remote，必须在第二阶段逐文件复核后再写实现。

Epic GitHub 页面明确说明 UnrealEngine 源码仓库需要将 Epic 账号与 GitHub ID 关联，并且要登录 GitHub 才能访问；当前环境没有该授权上下文。

## 2. UE 的实际分层，不是一个“把控件画上去”的窗口

### 2.1 Slate 应用层

UE 的核心不是 Windows 原生菜单控件堆叠，而是独立的 Slate 应用框架：

- `FSlateApplication`：应用级 tick、窗口、输入路由、焦点、鼠标捕获、菜单、调试/Widget Reflector 入口。
- `SWindow`：顶层 Slate 窗口，负责承载根 widget 和窗口级缓存/失效。
- `SWidget`：统一 widget 生命周期、属性、事件、desired size、绘制和可见性。
- `SPanel` / `SCompoundWidget` / `SLeafWidget`：分别承担多子节点、单内容和叶子控件的组合方式。

官方架构说明把一个 widget 的关键职责分成 `ComputeDesiredSize`、`ArrangeChildren`、`OnPaint` 和事件处理；子控件通过 slot 组合，而不是由父控件只接收一个字符串并硬编码子布局。

### 2.2 保留式布局/绘制和失效

Slate 不应每帧 `clear()` 后重新生成整棵编辑器树。UE 的 UI Invalidation 会缓存层级、布局和 paint 数据，只重算 dirty 子树；结构变化才触发 child invalidation，颜色等非布局变化只触发 paint invalidation，高频变化的 widget 才标记为 volatile。

这直接对应当前卡顿的主要根因：当前 `UiKitPanelHost` 在编辑器状态变化时整树重建，并把 retained widget tree 最终转成 D2D 命令；这既没有 UE 的 invalidation 粒度，也没有 UE 的 Slate renderer/batch 边界。

### 2.3 Docking/Tab 系统

UE 的编辑器布局不是固定的三列矩形。其核心抽象是：

- `FTabManager`：注册 tab spawner、打开/关闭/查找 tab、管理 tab manager 层级、保存/恢复布局。
- `FTabManager::FLayout`：可持久化的布局描述。
- `FArea`、`FSplitter`、`FStack`：布局树中的区域、分割器和 tab stack。
- `SDockTab`：带内容的 dockable tab，能被 tab stack 管理、激活、关闭、移动和恢复。

因此 Shinkou 不能只保留一个 `DockWorkspace::layout(Rect)`，而应当把“布局描述”和“当前 widget 实例”分离，并以稳定 tab ID 进行恢复。拖拽、浮动窗口、关闭确认、tab context menu 和异常布局回退必须是 docking 层职责。

### 2.4 Level Editor 视口

UE 的 Level Editor 视口也不是把整个主窗口交给游戏渲染。公开 API 显示它由两条链组成：

```text
SLevelViewport
  ├─ SViewport / FSceneViewport        // Slate widget 与渲染视口桥
  ├─ FLevelEditorViewportClient        // 编辑器相机、投影、输入、工具、选择
  └─ viewport overlay content          // 视口内的工具条、提示、轴/比例等 Slate 内容
```

`FSceneViewport` 实现 `ISlateViewport`，由 `SViewport` 需要绘制或处理输入时调用；`FLevelEditorViewportClient::CalcSceneView` 配置该视口的场景视图和投影矩阵。`FEditorViewportClient` 中还明确存在 viewport type、perspective/orthographic camera transform、scene view、safe frame 和 invalidate viewport widget 等职责。

对应到 Shinkou：场景必须先根据 dock 后的 viewport 内容矩形创建独立 render target/pass；UI 根窗口只负责 editor chrome。场景纹理和 UI 不能共用“整窗口就是游戏世界”的隐式尺寸。

## 3. 当前仓库的旧 UI 退役边界

### 3.1 需要退役的旧编辑器呈现链

```text
EditorLayer
  -> UiKitPanelHost
  -> shinkou::uikit::UiContext / Widget tree
  -> shinkou::uikit::RenderList
  -> D2D command translation in render/Backends.cpp
  -> editor UI texture / present
```

证据位置：

- `engine/include/shinkou/editor/UiKitPanelHost.h`
- `engine/src/editor/UiKitPanelHost.cpp`
- `engine/src/render/Backends.cpp` 中 `SHINKOU_WITH_UIKIT` 的 D2D UI 绘制分支
- `CMakeLists.txt` 中 `SHINKOU_ENABLE_UIKIT`、`shinkou_ui` 链接和 `SHINKOU_WITH_UIKIT`

该链应被完整移除，而不是继续给它加控件。特别是 `UiKitPanelHost` 不应成为新系统的兼容外观层。

### 3.2 可以复用但不能继续当 UI 的编辑器模型

以下模块是领域数据/命令/视口计算，不等同于旧 UI 库，可在源码审计后迁移到新 editor-ui 模块：

- `EditorUiModel`：场景/对象/文件/控制台等数据投影。
- `EditorCommandBus`：命令注册、执行和快捷键映射。
- `EditorSelection`：选择状态。
- `RenderView`：视图模式、坐标转换、网格/辅助线和视锥数据。
- `FileSystemService`：内容浏览数据源。

复用这些模块的前提是它们不依赖 `uikit` 类型、不驱动控件树重建，并且通过明确的 editor-ui adapter 暴露数据。

### 3.3 不应混为一谈的另一套 `engine/src/ui`

`engine/src/ui` 是另一套 `shinkou::ui` runtime，包含 `UiRuntime`、Theme、InputBridge、Render 和 ImGui adapter；它与 `ui/` 的 `shinkou::uikit` 不是同一类型系统。当前工程把两套 UI 都保留并接到同一个 editor 生命周期，造成重复的状态、渲染后端和失效语义。

重构目标不是把两套旧 runtime 互相转接，而是：

1. 先让新的 `editor_ui` 成为唯一编辑器 UI 运行时。
2. runtime 游戏 UI 与 editor UI 通过 viewport/window host 明确隔离。
3. 删除 `ui/` 独立库、Studio、C API 以及 `UiKitPanelHost` 后，再删除不再被引用的 `engine/src/ui` 旧 editor adapter。

## 4. Shinkou 新系统的源码驱动设计

新系统的命名不照搬 UE 源码，但职责按 UE 的边界实现：

```text
EditorApplication
  ├─ WindowHost                         // Win32 窗体，输入/DPI/交换链
  ├─ WidgetRoot / SWidget-like tree    // composition + slots
  ├─ LayoutEngine                       // desired size -> arrange -> geometry
  ├─ HitTestGrid / FocusManager         // 输入路由、焦点、捕获、导航
  ├─ InvalidationManager                // hierarchy/layout/paint/volatile
  ├─ CommandList + MenuBuilder          // 命令、菜单、快捷键
  ├─ DockManager                        // tab spawner、layout tree、持久化
  ├─ ViewportHost                       // 视口 widget 与 scene viewport 桥
  │    └─ EditorViewportClient          // camera/projection/grid/gizmo/input
  └─ SlateLikeRenderer                  // UI batch、clip、text、DPI、GPU upload
```

### 4.1 坐标系统

所有边界必须显式写出坐标空间：

1. **WindowPhysicalPx**：Win32 client rect 和交换链像素。
2. **EditorDip**：`physical / dpiScale` 后的 Slate-like 逻辑单位。
3. **WidgetLocalDip**：widget 自身左上角为原点。
4. **ViewportLocalPx**：场景目标内部像素，用于鼠标反投影。
5. **World**：场景世界坐标。
6. **View/NDC**：透视或正交投影后的渲染坐标。

窗口边框和 editor UI 占用的矩形不得进入 World/ViewportLocalPx。只有 `ViewportHost` 的 content rect 允许生成场景 pass；鼠标必须先经过 hit-test，未命中 editor chrome 才转换为视口局部坐标。

### 4.2 视图模式

- Perspective：由位置、旋转、FOV、near/far 生成 view/projection。
- Front/Back/Left/Right/Top/Bottom：由正交平面、中心、zoom、near/far 生成矩阵。
- 每个 viewport client 自己持有 transform 和 mode；切换模式只使 viewport scene invalidation，不重建整个 editor widget tree。
- 网格、轴线、单位刻度是视口 overlay/render feature，不是全窗口 UI 背景。

### 4.3 性能规则

- 稳定帧不允许 rebuild editor hierarchy。
- 动态 FPS/时间/状态文本只触发 leaf paint invalidation。
- 场景数据和 UI 数据不得在每个 paint command 中重新分配字符串。
- UI renderer 每帧最多一次动态 glyph/texture upload；批处理按 clip/texture/material 分组。
- 场景 viewport 可以独立刷新；没有场景变化时不得重画所有 editor chrome。
- 将输入延迟分成 Win32 event、UI route、viewport client、command execution 四段计时。
- 以 1280x720、1600x900、100%/150% DPI 为最小验收矩阵；稳定编辑器帧 UI CPU 目标先定为 `< 2 ms`，随后用实测修订。

## 5. 删除与实现顺序

在取得 UE 源码镜像或用户确认使用官方公开 API 文档作为唯一外部依据之前，不执行删除。取得后按以下顺序执行：

1. 记录当前基线：构建、CTest、启动、窗口截图、输入和 GPU/CPU 帧时间。
2. 新增 `editor-ui` 骨架和源码映射档案，先不接旧 `UiKitPanelHost`。
3. 实现最小 Slate-like root/window/widget/layout/invalidation/input 管线。
4. 实现 dock tree、tab spawner、layout persistence 和菜单命令。
5. 接入显式 `ViewportHost + EditorViewportClient + SceneViewport`，验证场景只出现在 viewport content rect。
6. 将层级、Details、Content Browser、Output Log 做成独立 tab 内容；每个面板有自己的模型、命令和失效域。
7. 通过完整构建、CTest、Windows 实际截图和交互矩阵后，再移除 `ui/`、Studio、C API、`UiKitPanelHost` 和未使用的旧 adapter。
8. 删除后跑全量引用扫描，禁止残留 `SHINKOU_WITH_UIKIT`、`UiKitPanelHost`、`shinkou::uikit` 的 editor 依赖。

## 6. 验收门槛

以下任一项失败都不接受“完成”：

- 启动后窗口可见、可拖拽、可点击、可键盘操作，不能黑屏/假死。
- 内容区域有场景且只占 viewport content rect；菜单、tab、面板和状态栏不会被场景覆盖。
- Perspective 与六个正交视图可切换；网格和轴线跟随视口坐标，不污染 editor chrome。
- Outliner/Details/Content Browser/Output Log 可独立关闭、恢复、移动和持久化。
- 稳定帧不发生整棵 widget tree rebuild；测试能报告 invalidation 类型、widget 数量、layout/paint 时间。
- 100% 和 150% DPI 下点击坐标、文本、分割器和视口反投影一致。
- 实际 Windows 像素截图通过，不以“命令列表非空”替代视觉验收。

## 7. 当前阻塞点

要完成用户要求的“必须看你组织内 UE 源码”，还缺以下任一项：

- 组织内 UnrealEngine 镜像的准确 HTTPS/SSH URL；或
- 本机已授权 UnrealEngine 源码目录；或
- 允许本项目把你提供的 UE remote 加入只读 remote。

在补齐之前，本档案可以作为架构依据，但不能宣称完成了组织内源码逐文件审计，也不应据此贸然删除整个旧 UI 树。
