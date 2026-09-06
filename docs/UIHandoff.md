# Shinkou Engine 编辑器 UI 任务交接档案

更新时间：2026-09-05

2026-09-06 续接说明：当前实现和验收记录见 [UIImplementation.md](UIImplementation.md)，
包含新增交互脚本、场景文档、检查器、相机输入和 UI 分段计量。本文下面的“已完成 / 已验证 / 未完成”
描述交接时基线，不能直接当作最新代码状态；整阶段的 DPI、ECS/资源接缝和能力限制仍须按新记录核对。

本文档用于把当前编辑器 UI 工作交给后续智能体。后续智能体必须先阅读本文档、
`docs/UIWorkflow.md`、`docs/UIAcceptance.md`、`docs/EditorUIPlatformDraft.md` 和
`docs/UEEditorUISourceAudit.md`，再开始修改代码。

## 一、当前总目标

将 Shinkou Engine 编辑器建设为可长期演进的专业引擎 UI 平台：

- Windows 上保留原生窗口、标题栏和菜单语义；
- 引擎内部采用 retained UI、扁平克制、类似 Apple 的视觉语言；
- 编辑器 UI 与游戏世界渲染严格分区；
- 世界只能渲染在 `RenderView`/Viewport 子窗口内，不能把整个客户区当成游戏画面；
- 资源管理器必须是真实文件系统浏览器，可进入目录、刷新、右键、重命名、新建目录和删除；
- 资源图标必须来自真正的矢量资源管线；
- 所有交互必须有清晰的 hover、pressed、focus、selected 反馈；
- 性能目标是正常编辑器交互接近原生 Windows 体验，不能因为点击一次就出现秒级阻塞。

## 二、权威架构边界

当前权威路径只有这一条：

```text
Engine::tick
  -> EditorLayer::process_input
  -> EditorUi::process_input
  -> UiRuntime::dispatch
  -> EditorLayer::draw
  -> EditorUi::build / UiRenderList
  -> Renderer::set_editor_viewport
  -> Renderer::set_editor_ui_render_list
  -> Renderer::submit
  -> IRenderBackend::render_editor_ui
  -> Windows client surface
```

主要目录：

| 模块 | 权威位置 | 责任 |
|---|---|---|
| retained runtime | `engine/include/shinkou/ui`、`engine/src/ui` | UI 事件、主题、RenderList、基础组件 |
| 编辑器 shell | `engine/include/shinkou/editor/EditorUi.h`、`engine/src/editor/EditorUi.cpp` | 面板、资源管理器、检查器、层级、菜单、命中区域 |
| 编辑器宿主 | `engine/include/shinkou/editor/EditorLayer.h`、`engine/src/editor/EditorLayer.cpp` | 文件系统、世界同步、面板注册、渲染接缝 |
| 视口 | `engine/include/shinkou/editor/RenderView.h`、`engine/src/editor/RenderView.cpp` | 3D/正交视图、相机、网格和 viewport 坐标 |
| 矢量图标 | `engine/include/shinkou/editor/AssetIconLibrary.h`、`engine/src/editor/AssetIconLibrary.cpp` | SVG 加载、归一化路径、主题颜色角色 |
| D3D11 呈现 | `engine/src/render/Backends.cpp` | D2D 几何/文本栅格、UI surface、GPU 合成和局部上传 |

历史独立 `ui/` 库以及 `shinkou::uikit` 宿主已经从当前构建图废除。不要重新接回第二套
widget/runtime，也不要通过修改旧库绕过 `EditorUi`。

## 三、已完成内容

### 1. 资源图标矢量管线

资源位于：

```text
engine/resources/editor/icons/
  folder.svg
  image.svg
  mesh.svg
  scene.svg
  material.svg
  audio.svg
  video.svg
  script.svg
  font.svg
  archive.svg
  file.svg
```

当前流程是：

```text
SVG source
  -> AssetIconLibrary 初始化阶段读取
  -> 受控 SVG path parser
  -> viewBox 归一化到 0..1
  -> 不可变 shared path geometry
  -> UiRenderList::Path
  -> D3D11 ID2D1PathGeometry 有界缓存
```

重要约束：

- paint 热路径不允许读取文件、解析 SVG 或创建文件系统对象；
- 图标使用 `data-role="accent|ink|paper"` 参与主题着色；
- 当前 parser 支持编辑器内置资源所需的 `M/L/H/V/Z` 路径子集，不是完整 SVG 1.1；
- CMake 安装目录为 `share/shinkou/editor/icons`；
- `SHINKOU_EDITOR_ICON_ROOT` 可覆盖图标根目录，不能把运行时路径写死为某个用户目录；
- D3D11 的 geometry cache 必须在 device lost、resize/recover 时正确释放和重建。

相关测试：`engine/tests/AssetIconLibraryTests.cpp`。

### 2. 资源管理器视图

`EditorAssetView` 已包含三个模式：

- `SmallList`：当前目录直接子项，紧凑行列表，显示图标和名称；
- `LargeIcons`：当前目录直接子项，大图标网格，显示图标和名称；
- `Tree`：递归资源树，显示折叠箭头、层级导线、缩进、图标和名称。

所有模式都不显示文件大小，也不在资源项旁边绘制 `Folder`、`FILE` 等文字 marker。
SVG 缺失时才允许使用明确的 fallback，不能把 fallback 当作 SVG 验收证据。

当前资源管理器已经接入真实文件系统回调，支持已有的打开、进入目录、刷新、右键菜单、
重命名、新建文件夹、删除确认和返回父目录流程。资产索引按文件 revision、目录、过滤词、
视图和折叠集合缓存，绘制使用可见范围虚拟化。

示例程序支持：

```powershell
--asset-view small
--asset-view large
--asset-view tree
```

### 3. 交互和呈现性能

纯 hover/pressed/focus 状态变化会记录旧、新 region 的并集：

```text
PointerMove
  -> hotRegion 变化
  -> dirty rect = old region ∪ new region
  -> D2D 只清理/重绘 dirty clip
  -> D3D11 只 UpdateSubresource 对应物理矩形
```

布局、主题、资源内容、停靠拖动、选择和文件操作等可能影响多个面板的动作仍然升级为
full repaint，这是正确性要求，不要为了“看起来快”清空 RenderList 或跳过呈现。

当前还完成了：

- 脏区外的 D2D 命令跳过；
- DirectWrite 按字体、字号、对齐、溢出策略缓存 `IDWriteTextFormat`；
- SVG path geometry 有界缓存；
- 资源列表预处理缓存；
- 内容 hash 命中时跳过 UI surface 重绘和上传；
- 资源行、按钮和输入框的 hover/pressed/focus/selected 状态。

## 四、已验证证据

构建目录：

```text
C:/Users/Lenovo/Desktop/Shinkou/Shinkou Engine/out/build/ui-current
```

已执行并通过：

```powershell
cmake --build out/build/ui-current -j 2
ctest --test-dir out/build/ui-current --output-on-failure -j 2
```

当前结果：`39/39 tests passed`。

GPU 捕获命令模板：

```powershell
& '.\out\build\ui-current\shinkou_ui_capture.exe' `
  '.\out\build\ui-current\shinkou_engine_sample.exe' `
  'C:\Users\Lenovo\Desktop\Shinkou\Shinkou Engine\out\qa\ui-tree-final.bmp' `
  3000 --client-size 1280x720 --require-gpu -- `
  --editor dx11 --asset-view tree `
  --project 'C:\Users\Lenovo\Desktop\Shinkou\Shinkou Engine' --theme dark
```

已验证输出包含：

```text
native-ui=1
viewport-scissor=1
captured=1
client=1280x720
surface=1921x1080
dpi=144
```

现有截图：

- `out/qa/ui-small.png`
- `out/qa/ui-large.png`
- `out/qa/ui-tree-final.png`

注意：当前机器实际 DPI 是 144 / 150%，因此这不是 100% 或 125% DPI 的通过证据；后续智能体
必须补齐真实 DPI 矩阵，不能只改变命令行尺寸后声称 DPI 已验收。

安装演练目录：

```text
out/install/ui-current/share/shinkou/editor/icons/
```

其中已确认安装 11 个 SVG 文件。

## 五、当前未完成项与已知风险

### P0：真实操作验收尚未自动化

`Tools/UiCapture` 当前主要负责启动进程和读取真实 GPU client surface，不会自动执行：

- 点击 Small/Large/Tree；
- 双击目录和文件；
- 右键空白区、文件、目录；
- 滚轮、拖动 scrollbar、拖动 splitter；
- F2/Enter/Escape/Delete 和文本输入。

后续应增加独立的 UI interaction capture/automation 工具，使用临时项目执行这些操作，
记录操作前后状态、回调参数和截图。不要直接在用户真实项目上做删除验收。

### P0：性能尚无 UI 分段 p50/p95 证据

当前 capture 的 render graph timing 不能代表 UI timing。仍需增加可查询的指标：

- `EditorUi::process_input`；
- 资产索引重建；
- layout；
- paint command 生成；
- D2D raster；
- full/dirty upload 字节数；
- UI composite；
- Present 阻塞；
- idle、hover、输入、过滤、滚动、resize 的 p50/p95/max。

禁止用 command count、进程启动时间或一次截图代替性能结论。

### P1：SVG parser 不是完整 SVG

如果后续需要外部主题或复杂图标，应选择固定版本的成熟 parser，或扩展当前 IR 以支持：
曲线、transform、stroke、opacity、基本 shape 和错误回退。扩展时必须保持：解析只发生在
资源加载/刷新阶段，不能进入 `paint()`。

### P1：D3D11 仍是 D2D DIB + GPU texture 过渡架构

当前 D3D11 UI 仍会把 retained command 栅格到 DIB，再合成到 GPU texture。dirty upload 已
降低交互变化的上传范围，但 composite 仍使用全屏 quad；后续若继续极限优化，应考虑 GPU
native UI vertex/index batch 或 tile cache，并保留当前 capture 作为正确性基线。

### P1：资源树 UX 仍需深化

后续检查并实现：

- 搜索结果自动补齐祖先目录，避免深层文件变成孤立节点；
- Chevron 至少整行高度、约 20 px 命中范围；
- 方向键、Home/End、PageUp/PageDown、Enter、F2、Delete、Escape；
- 选中项在滚动和视图切换后保持路径一致；
- scrollbar thumb 可拖动，而不仅是装饰；
- 窄窗口时不要把 Refresh 挤压成难以理解的 `R...`；
- Small/Large/Tree 按钮的文本、可访问性和 focus ring 在窄窗口仍清晰。

### P1：主题和 DPI 矩阵缺证据

必须实测并截图：

- 1280×720，100% DPI；
- 1600×900，125% DPI；
- 1024×768，100% DPI；
- dark、light、high-contrast；
- D3D11 实际 GPU surface；
- 其他后端明确 unsupported，而不是静默黑屏。

## 六、建议的并行任务拆分

后续智能体应使用不重叠写集，并在合并前分别提供测试证据。

### Agent A：交互自动化和真实操作验收

写集：`Tools/UiCapture/`、`docs/UIAcceptance.md`、新增 interaction tests。

任务：增加鼠标/键盘脚本能力，验证视图切换、目录导航、右键、重命名、新建文件夹、删除
确认、滚动和焦点。操作必须使用临时项目，输出前后截图和结构化日志。

### Agent B：资源树 UX

写集：`engine/src/editor/EditorUi.cpp`、必要时 `engine/include/shinkou/editor/EditorUi.h`。

任务：祖先补齐过滤、键盘导航、scrollbar 拖动、选中保持、Chevron 命中区和窄窗口布局。
必须保留三种视图和无文件大小要求。

### Agent C：性能计量和 UI 热路径

写集：`engine/include/shinkou/render/RenderBackend.h`、`engine/src/render/Backends.cpp`、
`engine/include/shinkou/editor/EditorLayer.h`、`engine/src/editor/EditorLayer.cpp`、
性能测试/报告文件。

任务：增加 UI 阶段计时、full/dirty upload 字节统计、缓存命中率和 p50/p95 报告；检查
brush/gradient/text/path cache 上限，以及 D3D11 immediate context 状态清理成本。

### Agent D：SVG 资源管线

写集：`engine/include/shinkou/editor/AssetIconLibrary.h`、对应 `.cpp`、
`engine/resources/editor/icons/`、图标测试。

任务：评估当前 path-only parser 是否足够；若扩展，增加坏 SVG、曲线、相对坐标、transform、
主题角色和 last-known-good 回退测试。不得引入每帧 IO。

### Agent E：跨平台资源定位和后端能力

写集：CMake、资源定位代码、能力文档和安装测试。

任务：验证 Windows/Linux/macOS 资源定位策略，确保不写死用户路径；确认 D3D12/Vulkan/Null
没有 retained UI 时输出明确 capability/error。

## 七、后续智能体的强制工作流

1. 阅读本档案和四个 Shinkou UI skill：`shinkou-ui-design`、`shinkou-ui-integration`、
   `shinkou-ui-performance`、`shinkou-ui-visual-qa`。
2. 先用 `rg` 阅读目标代码和现有 dirty worktree，不得 `git reset --hard`、`git checkout --`
   或删除不属于自己任务的改动。
3. 先写清任务的输入、输出、写集和验收条件，再修改代码。
4. 不把同步文件扫描、SVG 解析、字体读取、设备等待放进 paint 热路径。
5. 不使用空 RenderList、跳过错误、关闭 UI 或缩小窗口来伪造性能提升。
6. 每个实现任务至少执行：

   ```powershell
   cmake --build out/build/ui-current -j 2
   ctest --test-dir out/build/ui-current --output-on-failure -j 2
   ```

7. Windows/D3D11 UI 修改必须再运行 `shinkou_ui_capture --require-gpu` 并使用 `view_image`
   检查实际像素。
8. 报告必须区分 H（headless）、G（GPU capture）、O（实际操作）、D（DPI/后端）证据，
   缺失层级只能标记 `inconclusive`，不能写“通过”。
9. 最终审计重点检查：
   - 是否重新引入独立 UI 库；
   - 世界是否越过 viewport 子区域；
   - 资源项是否又出现大小列或文字 marker；
   - hover 是否又触发整屏上传；
   - 是否有同步 IO/解析进入 paint；
   - 未支持后端是否静默黑屏；
   - 测试、截图和 timing 是否互相匹配。

## 八、最终交付判定

只有同时满足以下条件，才允许把本阶段标记为完成：

- CTest 全部通过；
- SVG 源资源、解析、Path command、backend geometry 和安装路径均有证据；
- Small、Large、Tree 三种视图均有实际像素截图；
- 真实文件操作有临时项目 O 证据；
- 视口边界有 GPU capture G 证据；
- 至少一轮 100%/125%/150% DPI 真实矩阵；
- 性能报告包含 idle/hover/input/scroll/resize 的 p50/p95/max；
- 没有旧 UI 库回接、没有黑屏静默回退、没有整屏游戏世界覆盖编辑器 UI；
- 所有未支持后端和已知限制在报告中明确列出。
