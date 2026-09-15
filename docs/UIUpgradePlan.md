# Shinkou Engine UI 升级计划

这份计划把“像 Unity 一样的编辑器工作流”拆成可验证的能力层，而不是一次性引入大量外部依赖。每一轮都必须同时交付代码、测试、构建证据、交互/视觉证据和风险审计记录；如果某个能力只有占位符或元数据支持，界面必须明确显示当前能力等级。

## 目标与边界

最终目标是形成一套长期可演进的编辑器 UI 平台：

- 项目文件可安全浏览、创建目录、重命名、删除、读取和写回，所有路径均受项目根目录约束。
- 资源拥有统一类型、元数据、导入状态、预览能力和错误状态，可连接 `AssetSystem`、场景、渲染、音频和构建系统。
- 图片、视频、音频、文本/代码、字体、材质、场景和模型通过可注册 provider 提供预览；UI 绘制只消费不可变快照，不在 paint 路径做磁盘 IO 或解码。
- 支持主流 Windows 编译/构建链路的发现、配置、执行、日志解析和诊断跳转，并保留安全的显式命令参数边界。
- Windows 原生窗口、D3D11 当前路径、未来的 Vulkan/D3D12/无图形后端都通过能力矩阵报告状态，编辑器核心不绑定某一个 UI 工具包。

本计划不承诺复制 Unity 的全部功能，也不把第三方编解码器、模型格式或 IDE 集成伪装成内置能力。每项能力都要经过 provider 注册、失败状态、资源上限和回归测试后才算完成。

## 当前架构基线

- UI 主链路：`Engine::tick -> EditorLayer -> EditorUi -> UiRuntime/UiRenderList -> Renderer -> IRenderBackend`。
- 编辑器文件边界：`FileSystemService`，相对项目根目录解析，拒绝路径穿越，原子写文本，提供扫描和变更轮询。
- 资源运行时：`AssetSystem`，已有 loader、processor、异步请求、依赖、manifest、重载和统计接口。
- 当前 UI 资产浏览器：支持列表/树/大图标视图、过滤、选择、文件夹导航、重命名、新建目录、删除和右键操作。
- 当前媒体模型：`MediaPanel` 已连接音频试听 transport、受限音频元数据/波形快照和定位；Windows Media Foundation 已提供视频首帧/元数据 provider，OBJ 与 GLTF/GLB 已提供受限模型几何快照。
- 当前限制：检查器仍以 retained 线框/首帧/缩略图为主；GLTF/GLB 已能发布受限图片 artifact 并走现有 WIC 图片 provider，但材质采样、GPU 模型预览、连续视频时间轴和完整 IDE/子进程生命周期仍未完成。编译器诊断过滤、受限 PID 状态跟踪、外部 IDE launcher、multi-profile 编辑/切换和诊断行级打开已完成首个闭环。

## 分阶段路线

### 第 0 轮：治理、边界和可审计基线（已完成，持续维护）

交付物：

- 明确 `engine` retained UI 为唯一权威 UI 路径；旧的独立 `ui/` 树不再重新接回主构建。
- 保留布局、渲染、输入、文件系统、媒体和编辑器交互的独立测试。
- 固定每轮验收门槛：配置/构建、全量 CTest、`git diff --check`、安全检查、视觉/交互复核、性能数据和审计台账。

验收门槛：

- `out/build/mingw-debug` 可重建。
- 全量 CTest 无失败。
- 任何新 provider 都有明确的失败、禁用和回滚路径。

### 第 1 轮：资源目录与预览能力契约（当前轮）

交付物：

- 统一 `AssetPreviewKind`、扩展名/MIME 分类和 `AssetPreviewCapability` 位标志。
- 对文件夹、图片、音频、视频、模型、场景、材质、shader、文本、字体、压缩包和未知二进制给出确定的 descriptor。
- 预览面板只显示真实能力：当前文件类先提供元数据能力，文件夹保留已有导航能力，未注册 provider 明确显示原因。
- 以单元测试锁定大小写扩展名、未知扩展名、文件夹和 capability 组合。

本轮不引入解码器，不在 UI 线程扫描文件，不修改资源文件格式。这样后续各 provider 可以独立审计和回滚。

### 第 2 轮：项目、构建和工具链工作流

本轮拆成三个可独立回滚的子阶段：

#### 第 2A 子阶段：工具链契约与构建计划

当前执行子阶段交付：

- 工具发现快照：CMake、Ninja、MSBuild、Clang、clang-cl、GCC、G++ 和 .NET SDK。
- 构建 profile 使用显式 executable/working directory/argument 数组，不生成 shell 字符串。
- CMake configure/build、Ninja、MSBuild 和 .NET build 的计划生成，以及项目根目录路径约束。
- GCC/Clang 风格和 MSVC 风格诊断解析，异常大行号/列号安全拒绝。

本子阶段不启动外部进程；进程生命周期、取消、stdout/stderr 管道和权限策略属于 2B。

#### 第 2B 子阶段：安全进程执行

交付物：

- 项目 manifest、构建 profile 和工作区设置的版本化 schema。
- 检测 MSVC、clang-cl、MinGW/GCC、Clang、CMake、Ninja、MSBuild 等工具，并以可读能力状态呈现。
- 异步进程 runner：可取消、超时、环境变量白名单、工作目录约束、stdout/stderr 分流和退出码。
- 编译日志解析为文件/行/列诊断，支持从诊断跳转到引擎内文本资源或外部 IDE。

安全门槛：不拼接未经校验的 shell 字符串；可执行文件和参数分别存储；禁止默认执行项目目录之外的脚本；所有进程操作写入审计日志。

#### 第 2C 子阶段：编辑器 Build 面板和 IDE 连接

交付物：

- retained Build 面板显示工具链、profile、任务状态、日志和诊断列表。
- 从诊断双击跳转文本资源；外部 VS/Rider/VS Code/clangd 打开动作走显式 launcher。
- `compile_commands.json` 导入/导出和工程设置绑定。

当前进度：Build retained 面板首个垂直切片、项目内文本资源诊断跳转、`compile_commands.json` 安全导入/导出、实时 stdout/stderr 快照、multi-profile 持久化/编辑和外部 IDE 行级打开已完成；增量诊断、过滤和 IDE 进程生命周期仍按本阶段继续实现。

### 第 3 轮：资源索引、导入和缓存

交付物：

- 后台增量索引，UI 只读取 immutable snapshot。
- 文件指纹、导入设置、派生资源缓存、缩略图缓存和依赖图。
- 与 `AssetSystem` 的 loader/processor/manifest 对接，支持新增、修改、删除和重命名后的稳定资源 ID。
- 大目录分页/虚拟化，索引失败、权限失败、文件锁定和损坏文件均可见。

性能门槛：首次扫描、增量扫描、内存峰值、列表重绘和缓存命中率记录 CSV；paint 路径零磁盘 IO、零同步解码。

### 第 4 轮：预览 provider

按风险由低到高实现并独立验收：

1. 文本/代码/JSON/YAML/CMake：安全读取上限、编码检测、只读预览、外部编辑器打开。
2. 图片：PNG/JPEG/BMP/TGA/WebP 等，尺寸/色彩空间元数据、缩放、棋盘格和 GPU 纹理缩略图。当前已完成 Windows WIC→retained D2D 的 PNG 首个垂直切片，后续补格式矩阵与跨后端上传。
3. 音频：WAV 优先，再接入 OGG/MP3/FLAC；波形、时长、播放/暂停/定位，并通过现有音频系统执行试听。
4. 视频：先按平台能力接入 Media Foundation 或受控 FFmpeg provider；首帧/时间轴/音频轨信息，解码失败可诊断。当前已完成 Windows Media Foundation 首帧与基础元数据首个垂直切片。
5. 字体、材质、场景和 shader：显示元数据和安全的结构化检查器。
6. 模型：OBJ/GLTF/GLB 优先，导入后放入隔离的 preview scene，支持相机轨道、包围盒、材质和网格统计；当前已完成 OBJ/GLTF/GLB 有界几何解析、结构化顶点/索引、隔离 preview scene 的 orbit/zoom/reset、GLTF 材质/纹理元数据和首个 WIC 纹理 artifact 预览切片，GPU preview target 和真实材质采样仍待实现；FBX 等格式必须有独立第三方依赖审计。

每个 provider 都必须提供：扩展名/MIME 注册、能力声明、异步生命周期、取消、资源大小上限、损坏输入处理、失败文本、缓存键和单元/集成测试。

### 第 5 轮：编辑器组合工作流

交付物：

- Content Browser 与 Inspector/Preview 面板的稳定选择模型、拖放模型和上下文命令。
- 图片缩放、音频 transport、视频时间线、模型 orbit、文本搜索/跳转等交互。
- 资源拖入场景时通过显式导入/实例化命令连接场景、渲染、材质和音频系统。
- 统一命令注册、快捷键序列化、事务 undo/redo 和脏状态提示。

### 第 6 轮：渲染后端和资源生命周期

交付物：

- D3D11 预览纹理/渲染目标与 GPU 资源生命周期。
- Vulkan、D3D12、Null 后端的能力矩阵、降级路径和一致的 retained draw list。
- device lost、窗口 resize、DPI 切换、异步上传、纹理回收和 backend shutdown 的测试。

### 第 7 轮：IDE 与编译器集成

交付物：

- Visual Studio、Rider、VS Code/clangd 的可选启动器与项目生成/打开动作。
- `compile_commands.json` 导入/导出，工具链 profile 与引擎工程设置关联。
- 诊断列表、过滤、双击跳转、编译任务状态、取消和失败重试。
- 外部 IDE 不可用时引擎内流程仍可运行；所有路径和参数均经过安全验证。

### 第 8 轮：性能、安全和发布验收

交付物：

- 路径穿越、符号链接、权限、恶意超大文件、畸形媒体/模型和进程注入风险审计。
- 资源 provider fuzz/smoke、长时间打开关闭、取消/重载和缓存一致性测试。
- 1280x720、1600x900 125% DPI、1024x768、浅色/深色/高对比度视觉矩阵。
- 变更日志、已知限制、回滚步骤、包体依赖清单和发布前证据包。

## 每轮固定执行顺序

1. 写清本轮能力边界、非目标、数据流和失败状态。
2. 先写/更新契约测试，再实现最小垂直切片。
3. 构建受影响目标，运行目标测试，再运行全量 CTest。
4. 做代码差异、安全边界、性能回归和 UI 视觉/交互审计。
5. 更新 `docs/UIUpgradeAudit.md`，记录证据、剩余风险和下一轮入口。
6. 只有达到门槛才把本轮标记为完成；否则保留为进行中，不用占位实现掩盖缺口。

## 当前执行安排

第 1 轮资源预览契约、第 2A 工具链计划、第 2B Windows 进程执行链、第 3.1 不可变资源索引首个垂直切片、第 3.2 选中资源 AssetSystem 桥接、第 3.3 编辑器 manifest immutable snapshot、第 3.4 manifest 稳定 AssetId、第 3.5 manifest 指纹增量缓存、第 3.6 manifest 受限读回/校验、第 3.7 编辑器 manifest seed 连接、第 3.8 typed source artifact processor/loader、第 3.9 typed artifact bounded descriptor、第 3.10 OBJ provider 消费 AssetSystem typed payload、第 3.11 WIC image provider 消费 AssetSystem typed payload、第 4.1 音频 transport 首个垂直切片、第 4.2 Windows 图片缩略图首个垂直切片、第 4.3 音频元数据/波形首个垂直切片、第 4.4 Windows Media Foundation 视频首帧/元数据首个垂直切片、第 4.5 OBJ 模型线框/统计首个垂直切片、第 4.6 结构化网格与隔离模型预览场景首个垂直切片、第 4.7 GLTF/GLB 基础几何 provider 首个垂直切片、第 4.8 GLTF 材质/纹理元数据首个垂直切片、第 4.9 视频时间点帧预览首个垂直切片、第 4.10 AudioSystem cursor/seek transport、第 2C.1 Build profile/IDE launcher、第 2C.6 multi-profile/诊断筛选/IDE 生命周期、第 2C.7 solution/compile database 自动关联、第 2C.8 GLTF image artifact → WIC preview、第 2C.9 GLTF material → texture artifact 选择、第 4.11 renderer-owned model geometry pass、第 4.12 D3D11 BGRA8 base-color texture sampling、第 4.13 glTF TEXCOORD_0 → D3D11 vertex input、第 4.14 glTF NORMAL → D3D11 lit preview 和第 4.15 glTF metallic/roughness factor lit material 已完成并有独立审计证据。第 3 轮仍需正式 derived cache、重命名迁移、依赖失效、normal map/metallic/roughness 纹理、完整 BRDF、完整 sampler/色彩空间、独立 offscreen model target、资源拖入场景和音视频连续播放/同步。每轮保持小步提交，提交与 GitHub 推送在用户明确要求时执行。

### 第 4.6 子阶段：结构化网格与隔离模型预览场景

本阶段先完成模型预览的场景边界，不引入 GPU mesh 或真正活动场景实例化：

- OBJ provider 除统计和线框外发布不可变顶点/三角形索引快照。
- `EditorModelPreviewScene` 只持有 immutable geometry 和独立相机状态，提供 orbit、zoom、reset 与有界投影线框。
- Inspector 模型预览使用中键拖拽、滚轮和 Reset；资源查看不会创建、删除或修改活动 `World` 实体。
- provider 几何、scene camera、UI retained snapshot 各自可独立测试；无有效几何时不得注册交互区域或伪造成功状态。

非目标：GLTF/GLB 解析、材质/纹理/法线/动画、GPU mesh cache、D3D/Vulkan preview render target，以及视频连续解码均不在本子阶段伪装完成。

### 第 4.7 子阶段：GLTF/GLB 基础几何 provider

已完成能力边界：

- `.gltf` 支持项目内相对 `.bin` 和 base64 buffer；`.glb` 支持 JSON chunk 与单个内嵌 BIN chunk。
- 读取 `buffers`、`bufferViews`、`accessors`、`meshes/primitives`，支持 TRIANGLES、POSITION `FLOAT/VEC3` 和 8/16/32 位 scalar indices。
- 结果统一发布为 `EditorModelPreviewSnapshot`，复用隔离 preview scene 的 orbit/zoom/reset 和线框路径。
- 输入 JSON 深度、对象/数组、字符串、source/buffer、accessor 数量和 aggregate buffer bytes 均有上限；失败状态保持可读且不会写入活动场景。

非目标：节点层级变换、skins/animations、材质/纹理/法线/切线、稀疏 accessor、非 TRIANGLES mode、GPU mesh/cache 和真实 3D render target。

## 第 3.2 子阶段：选中资源 AssetSystem 桥接

状态：已完成（统一资源加载/缓存契约首个垂直切片；正式 importer/manifest 和派生资源缓存仍未完成）。

范围与非目标：

- 编辑器选中资源通过空类型 `AssetKey` 请求 `AssetSystem`，由扩展名完成 canonicalize；raw loader 负责建立可观察的异步加载、缓存、格式和源哈希结果。
- `EditorAssetPreviewUiState` 发布 AssetSystem loading/ready/error、格式和 source hash；Inspector 将 bridge 状态与现有文本/图片/音频/视频/模型 provider 状态合并展示。
- `AssetSystem` 与编辑器使用同一个项目根；Engine shutdown 先收束 EditorLayer 的 preview future，再关闭 AssetSystem worker，避免资源任务访问已销毁系统。

非目标：本阶段不替换 provider 的解码/解析路径，不把 raw bytes 直接交给 retained paint，不实现 importer/manifest 写入、派生缓存、依赖图或资源拖入场景。

代码变更：

- `EditorLayer` 新增 selected-resource bridge state 和 non-blocking future polling；AssetSystem 不可用、未索引、失败和成功均有显式状态，provider 仍可独立失败/回退。
- `Engine` 在 editor 模式下把 `editorProjectRoot` 作为默认 AssetSystem project root，并调整 shutdown 顺序；显式 `assets.projectRoot` 仍优先。
- Inspector status line 显示 AssetSystem 状态和 canonical format；不创建活动 `World` 实体，也不改变音频/视频/模型 provider 的所有权。

契约与集成测试：

- `EditorInteractionTests` 创建真实临时项目和 AssetSystem，验证文本 provider 与 AssetSystem 同时就绪、format 为 `raw`、source hash 非零。
- 目标构建及 UI model/interaction 回归通过；完整构建、全量 CTest、`git diff --check` 作为本轮关闭门槛。

安全/性能审计：

- AssetSystem 仍通过其项目根/挂载解析和内置 source-size 上限读取；编辑器不把用户路径拼成 shell 命令，也不在 paint 线程同步加载资源。
- 每次选中资源最多发起一个共享 future；切换资源丢弃旧观察句柄但由 AssetSystem 自己收束 worker，EditorLayer shutdown 明确等待当前 future。
- bridge 只发布标量状态和不可变 AssetData 元数据，不把完整 payload 复制到 `EditorUiModel`；provider 的独立读取边界保持不变。

未完成风险/下一步：

- 当前仍是 raw bridge，AssetSystem 未消费 GLTF/图片/音频等 provider 的结构化快照；同一资源存在 provider IO 与 raw cache 两条读取路径，后续需要正式 importer/loader 连接以消除重复读取。
- 尚未实现 manifest 增量更新、稳定资源 ID、派生缩略图缓存、依赖失效传播和拖入场景实例化。
- 下一步优先做材质/纹理 accessor 的结构化 provider，或先为连续视频时间轴建立独立 bounded decoder session；两者完成后再进入正式 importer/manifest。

### 第 3.3 子阶段：编辑器 AssetSystem manifest immutable snapshot

状态：已完成（异步 manifest 枚举与编辑器状态连接；正式 manifest 写回/稳定 ID/importer artifact 仍未完成）。

范围与非目标：

- 编辑器初始化、显式刷新和项目根变化会请求 `AssetSystem::scan_sources()`，worker 线程生成 `AssetManifestEntry` 的 immutable snapshot，主线程只发布数量和状态。
- 状态栏/控制台显示扫描中、不可用、失败和 ready 资源数量；选中资源 raw bridge 与 manifest snapshot 可同时工作。
- manifest dirty 状态与文件树 `assetsDirty_` 解耦，文件树异步等待期间不会重复启动 manifest 扫描；资源变更操作会显式使 manifest 失效。
- 本阶段不在每帧扫描、不在 paint 路径 hash 文件、不默认写入 manifest 文件；正式增量 manifest、稳定 AssetId、派生缓存和 importer/loader artifact 仍是后续轮次。

### 第 3.4 子阶段：manifest 稳定 AssetId

已完成能力边界：

- `AssetManifestEntry` 发布与 `AssetSystem::AssetLoadResult` 相同的稳定 `AssetId`，由规范化 `AssetKey { uri, type }` 计算，不依赖物理路径或扫描顺序。
- `scan_sources()` 与 `write_manifest()` 保留同一个 ID；编辑器 immutable manifest 因此可以作为后续增量索引、派生缓存和拖入场景的身份基础。
- 通过 AssetSystem standalone test 验证扫描条目 ID 与实际加载结果一致，并验证写回 manifest 包含 ID 字段。

非目标：本阶段不实现 manifest 读回、删除/重命名历史迁移、稳定 ID 冲突数据库或正式 importer artifact；增量 hash 复用单独在第 3.5 子阶段完成。

安全/性能审计：

- manifest 扫描只走 AssetSystem 已登记的扩展名和 mount，完全异步；generation 过期结果不会覆盖新项目状态。
- AssetSystem pointer 更换前、EditorLayer shutdown 时等待 manifest future，确保 worker 不访问悬空系统；失败只保留可读状态，不影响 provider 预览。

下一入口：把 manifest 条目与稳定资源 ID/增量失效传播绑定，再将 GLTF image metadata 和图片 provider/派生缓存正式接入。

### 第 3.5 子阶段：manifest 指纹增量缓存

已完成能力边界：

- `AssetSystem::scan_sources()` 按规范化 `AssetKey` 保留源路径、写入时间戳和文件大小指纹；未变化条目直接复用 immutable manifest entry，不重复读取源文件内容。
- 文件新增或时间戳/大小变化时只重算对应条目；缓存条目会按当前 mount 扫描结果裁剪，删除或不再登记的资源不会永久留在缓存中。
- `last_manifest_scan_stats()` 发布本次条目数、cache hit 和 cache miss，供编辑器性能审计和后续 UI 状态采样使用；`verifyCacheByContentHash` 开启时强制重新读取内容。

非目标：本阶段不实现文件系统事件驱动扫描、manifest JSON 读回、重命名历史迁移、内容 hash 碰撞数据库或 importer/derived artifact；这些仍需下一轮连接。

### 第 3.6 子阶段：manifest 受限读回/校验

已完成能力边界：

- 新增 `AssetSystem::read_manifest()`，读取上限 64 MiB、JSON 深度/成员/数组/字符串均有界，拒绝截断、尾随数据、重复键和非法类型。
- 校验 manifest version、canonical virtual URI、非空 type/source、整数 hash/timestamp/size、重复 key/ID，以及 `AssetId == make_id(AssetKey)`；source path 只作为描述字段，不被信任为读取授权。
- 读回失败返回结构化可读错误且不产生部分有效结果；旧 manifest 不会覆盖实时扫描 snapshot。

非目标：本阶段不自动把 manifest 读回结果合并到编辑器索引、不做重命名历史迁移、不根据 manifest source 路径读文件、不实现 importer/loader artifact；seed 连接属于下一阶段。

### 第 3.7 子阶段：编辑器 manifest seed 连接

已完成能力边界：

- 编辑器 manifest worker 在实时 `scan_sources()` 前检查项目内 `.shinkou/manifest.json`，通过 `read_manifest()` 校验后调用 `seed_manifest_cache()`。
- seed 只提供候选 fingerprint；随后仍由当前 AssetSystem mount 重新遍历并比较 source path、timestamp、size，新增、变化和删除资源以实时扫描为准。
- Inspector/控制台状态明确区分 `validated cache` 与 `readback fallback`，manifest 读回失败不会阻塞实时扫描，也不会把 source path 直接交给读取或进程系统。

非目标：本阶段不自动写回 manifest、不迁移重命名历史、不实现 watcher 事件队列或正式 importer/loader artifact；这些继续排在增量导入阶段。

### 第 3.8 子阶段：typed source artifact processor/loader

已完成能力边界：

- AssetSystem 内置 processor/loader 为 text、texture、model、audio、video、font、shader、material、scene 建立 typed source artifact；扩展名 canonicalize 不再默认全部落到 `raw`。
- processor 在受限源读取后发布稳定 format 和版本化 cache artifact，loader 复用不可变 payload；现有自定义 processor/loader 注册仍可覆盖内置类型。
- 编辑器 selected-resource bridge 现在显示 canonical typed format（例如 `.txt` 为 `text`、`.png` 为 `texture`），而实际图片/音频/视频/模型解码仍由各自 bounded provider 完成。

非目标：本阶段不把源字节伪装成解码纹理、可播放音频或 GPU mesh；图片像素、音频 waveform/video frame artifact 和正式 persisted derived cache 仍需独立 importer/loader 实现。GLTF 的首个结构化 image artifact 在后续 2C.8 单独完成。

### 第 3.9 子阶段：typed artifact bounded descriptor

已完成能力边界：

- `AssetArtifact`/`AssetData` 增加可选的 `metadataFormat` 与 bounded `metadata` 描述块，并把它纳入 versioned disk cache；缓存失效仍由 source fingerprint、processor/loader version 和依赖指纹共同决定。
- 内置 typed source processor 发出 text 行数、PNG/JPEG 尺寸、WAV 参数和 OBJ 顶点/面数的 descriptor；识别失败时保留通用 source descriptor，不影响源 payload 的 typed format。
- 编辑器 Inspector 只显示 descriptor schema 与字节数，保持 provider decoded snapshot、GPU 资源和 retained paint 的边界。

非目标：本阶段不把描述块当作最终 importer 结果，不解析 GLTF 材质纹理到 AssetSystem，不创建 GPU 纹理/mesh，不替换现有图片、音频、视频、模型 provider。

### 第 3.10 子阶段：OBJ provider 消费 AssetSystem typed payload

已完成能力边界：

- `EditorModelPreview` 新增 bounded byte-source seam；OBJ provider 可以消费 AssetSystem `model` typed payload，沿用相同的顶点、面、取消、几何上限和 immutable wireframe snapshot 契约。
- 当编辑器已连接且初始化完成的 AssetSystem 时，OBJ preview worker 优先等待该系统的 `model` future；没有连接、根目录正在切换或资源不是 OBJ 时，保留原有 project-safe FileSystem provider 回退。
- GLTF/GLB 的 source-byte provider 和 image artifact 在后续 2C.8 完成；本阶段只保证 OBJ byte-source seam，不把它扩展成材质或 GPU 导入结果。

非目标：本阶段不创建 GPU mesh、不修改活动 World、不把 OBJ 材质/纹理或动画写入 AssetSystem，也不改变 retained paint 的 worker/immutable 边界。

### 第 3.11 子阶段：WIC image provider 消费 AssetSystem typed payload

已完成能力边界：

- `EditorImagePreview` 增加 `load_editor_image_preview_bytes()`，使用与路径 provider 相同的 WIC 尺寸、像素、缩放和 thumbnail 预算；Windows worker 通过 bounded `IWICStream` 解码 immutable AssetSystem bytes。
- 编辑器连接 AssetSystem 且资源为 PNG/JPEG/TGA/DDS/KTX/KTX2 时，图片 preview worker 优先请求 `texture` artifact；未连接、根目录切换或其他情况保留 project-safe 文件 provider 回退。
- 非 Windows 仍返回明确 provider unavailable，不伪造图片像素；renderer-owned D3D11 bitmap cache 和 retained `UiImageSnapshot` 边界不变。

非目标：本阶段不实现纹理压缩格式全矩阵、颜色管理、GPU texture upload、材质采样或 GLTF image URI/bufferView 自动绑定。

### 第 4.8 子阶段：GLTF 材质/纹理元数据 provider

状态：已完成（材质/纹理/image 元数据首个垂直切片；纹理解码、采样和 GPU material preview 仍未完成）。

范围与非目标：

- GLTF/GLB provider 有界读取 `images`、`samplers`、`textures` 和 `materials`，发布 name、相对/data image URI、MIME、source/sampler、PBR baseColor、metallic/roughness、alphaMode、doubleSided 以及材质对纹理的引用。
- 非项目相对 image URI、缺失引用、无效 alphaMode、非有限或越界 PBR 值均失败；metadata 进入不可变模型快照，Inspector 显示材质/纹理数量。
- 本阶段不解码图片、不创建 GPU texture/sampler/material、不执行节点变换/动画，也不把资源实例化到活动场景。

安全与性能门槛：metadata 表项上限 4096，复用 JSON 深度/字符串/源文件上限；image URI 只校验项目相对或 data URI，不发起网络访问；paint 只读取 immutable snapshot。

下一入口：将 GLTF image metadata 与现有图片 provider/AssetSystem 派生缓存对接，再在 renderer-owned preview target 中实现受控材质采样；视频连续时间轴仍作为独立 decoder session 轮次。

### 第 4.9 子阶段：视频时间点帧预览

状态：已完成（Media Foundation bounded seek/frame 首个垂直切片；连续播放、音视频同步和硬件解码矩阵仍未完成）。

范围与非目标：

- 新增 `load_editor_video_frame` 时间点 provider；`MediaSeek` 在视频资源上启动异步目标帧读取，连续拖动会取消旧请求并合并最新目标。
- 视频快照增加 `frameTime`，retained Media 面板继续只消费 immutable `UiImageSnapshot`；元数据与首帧/目标帧共享同一 bounded decoder 生命周期。
- 本阶段不实现自动 Play/Pause 连续解码、音视频时钟同步、音频轨输出、关键帧精确度保证或 FFmpeg/硬件 codec 选择。

安全/性能审计：

- 目标时间被限制在有限 duration，帧输出继续受 512×512、源尺寸 4096 和 512 MiB 文件上限约束；取消在 decoder worker 生命周期内传播。
- Media Foundation COM/reader/sample 句柄仍只存在 worker；selection/root/shutdown 会取消并等待 frame future，旧 generation 不得覆盖当前视频。

下一入口：建立独立 bounded decoder session 以支持播放/暂停和音视频同步，再补 MP4/MOV/MKV/WebM 格式/系统 codec 矩阵与渲染后端上传。

### 第 2C.1 子阶段：Build profile 持久化与外部 IDE launcher

已完成能力边界：

- `EditorBuildProfileStore` 以版本化、受限 JSON 保存 `.shinkou/build-profile.json`；profile 中只保存项目相对路径、工具类型和参数数组，拒绝绝对路径、路径穿越、重复 JSON key、超长字符串和超量参数。
- `EditorToolIntegration` 发现 Visual Studio、Rider、VS Code 和 clangd，并把项目根、profile project/build 目录和当前选中文件转换成可审计的 `EditorIdeLaunchPlan`；VS Code 支持受限的 `--goto file:line:column`。
- Windows launcher 通过 `CreateProcessW` 直接启动可执行文件，参数不经过 shell；Build 菜单、native menu 和 retained Build 面板都提供 launcher/profile save/reload 入口，并公开明确的不可用/失败状态。

非目标：本阶段不自动安装 IDE、不执行 shell 脚本、不猜测用户的解决方案结构，不把 clangd 当作完整 IDE；IDE 进程生命周期跟踪与诊断过滤不在本历史切片内，已由后续 2C.6 完成，非 Windows launcher 仍留到后续切片。

验收门槛：

- profile round-trip、重复 key/路径穿越拒绝、IDE discovery/plan、错误 launch 和进程状态查询均有 `EditorBuildSystemTests` 覆盖。
- `EditorInteractionTests` 验证 Build 面板命令真实写入/读回项目内 profile、诊断筛选命令与 retained region；Build UI 只消费状态快照，launcher 不在 paint/input 中执行。
- 全量构建、CTest 和 `git diff --check` 必须重新执行；Windows 直启只允许在已发现的 regular executable 上发生。

下一入口：solution/compile database 自动关联与 clangd 配置生成已在 2C.7 完成；资源侧继续进入 GLTF image/material derived artifact 或 bounded 连续媒体 decoder session。

### 第 2C.6 子阶段：multi-profile 编辑、诊断筛选与 IDE 生命周期

本轮已完成能力边界：

- `.shinkou/build-profile.json` 支持受限 version 2 profile set，保存最多 32 个 profile 与 selected id；version 1 单 profile 文件仍可读，损坏数据不会覆盖当前内存状态。
- retained Build 面板提供 profile 选择和受限字段编辑（Name、Build Directory），输入仍经过项目相对路径与长度校验，修改先进入内存，显式 Save 才写盘。
- 诊断项保留引擎内资源跳转，同时提供 IDE 动作；VS Code 使用 `--goto`，Rider 使用 line/column 参数，Visual Studio 通过显式 `/Command` 参数打开文件，文件与位置均限制在项目根目录内。
- Build 诊断栏提供 All/Error/Warning/Note 筛选、分级计数和行选中态；实时 stdout/stderr 解析增量更新计数，展示项与 live buffer 均有 256 条上限，完整总数不受展示上限影响。
- Windows 外部 IDE 进程保存 PID 并以 250ms 节流查询 Running/Exited/NotFound/QueryFailed 状态；仅查询进程，不提供终止或接管外部工具的能力。

审计门槛：

- `EditorBuildSystemTests` 覆盖 profile set round-trip、selected id 完整性、IDE 行级计划、直接 launcher、PID 状态查询和旧安全边界。
- `EditorInteractionTests` 覆盖 profile selector、retained 输入提交、保存/读回、诊断筛选命令和现有 Build 控件可见性。
- 全量构建、CTest、三种窗口主题/尺寸捕获和 `git diff --check` 均作为本轮门禁；诊断过滤、增量统计和 IDE 生命周期跟踪已完成，下一切片进入 solution 自动探测与 clangd 配置生成。

### 第 2C.7 子阶段：solution/compile database 自动关联与 clangd 配置

本轮已完成能力边界：

- 有界发现项目根内的 CMake、Visual Studio solution/project、.NET、Meson、Cargo、`compile_commands.json` 和 `.clangd` 文件；跳过 `.git`、`.shinkou`、`node_modules`，深度和条目数均有限制。
- 优先使用 profile 的 project/build directory，其次使用项目根候选，生成确定性的推荐 project file 与 compile database；profile 路径越界、符号链接逃逸和非项目文件均拒绝。
- Build 面板、主菜单和 Windows native Build 菜单提供 Discover Projects、Associate Recommended Project、Generate `.clangd`；关联只更新内存 profile，仍需显式 Save 才持久化。
- `.clangd` 只在显式命令下生成；生成前通过现有受限 `EditorCompileCommands` parser 验证数据库，写入通过 `FileSystemService::write_text_atomic`，不执行 shell 或外部工具。

审计门槛：

- `EditorProjectIntegrationTests` 覆盖候选优先级、隐藏目录排除、profile association、compile database 验证、clangd 内容和写盘安全边界。
- `EditorInteractionTests` 覆盖 retained project discovery/association/clangd command、profile 状态和项目根 `.clangd` 结果。
- 本轮继续执行全量构建、CTest、UI capture 和 `git diff --check`；下一入口为结构化 GLTF image/material 或音频 waveform derived artifact。

非目标与剩余风险：

- 当前不解析 `.sln` 内部 project graph、不生成 Visual Studio solution、不写回 clangd toolchain flags，也不跟踪 compile database 文件变化。
- 非 Windows IDE launcher 仍未实现；Windows PID 查询仍只覆盖被启动的单个进程，不代表整个 IDE 子进程树。

### 第 2C.8 子阶段：GLTF image artifact → WIC 图片预览

状态：已完成（首个结构化纹理 artifact 垂直切片；多纹理选择、GPU 材质采样和正式 derived cache 仍未完成）。

范围与实现：

- `EditorModelPreviewSnapshot` 新增 immutable `EditorModelTextureArtifact`，从 glTF/GLB 的项目内相对 URI、base64 data URI 或 bufferView 提取受限 encoded image bytes，并保留 image index、URI 和 MIME 关联。
- GLTF/GLB provider 增加 AssetSystem source-byte overload；模型容器可由 AssetSystem typed `model` payload 提供，外部 buffer/image 仍经过 `FileSystemService` 项目根边界解析。
- 模型预览完成后只选择第一项有效纹理 artifact，异步调用现有 `load_editor_image_preview_bytes()`/WIC，结果进入 retained Inspector 的纹理缩略图和明确状态；paint 不读文件、不解码、不创建 GPU 资源。

安全与性能门槛：

- 单个 image artifact 32 MiB、全部 image artifacts 64 MiB；沿用 GLTF 128 MiB source/buffer、JSON 深度/字符串/数组和 WIC 64 MiB/512×512 thumbnail 上限。
- data URI 只接受 base64；外部 URI 不允许 scheme、绝对路径或项目根外路径；旧 generation、selection、source stamp 和 shutdown 均不会把异步结果写入当前 Inspector。
- artifact 使用 `shared_ptr<const vector<uint8_t>>` 发布，WIC worker 与 retained snapshot 生命周期隔离；没有 artifact 或 WIC 不可用时显示失败/无 payload，不伪造纹理成功。

契约与证据：

- `EditorGltfPreviewTests` 覆盖外部项目内图片、GLB source-byte overload、data URI artifact、artifact 与 image metadata 数量/字节一致性，以及原有越界 URI/损坏 GLB/取消路径。
- 本轮已执行目标 GLTF 测试、全量构建、CTest、`git diff --check` 和 Windows retained UI capture；CTest `51/51` 通过。GPU capture 仍降级为 GDI，只记录 host/command evidence，不宣称 GPU 像素证据。

非目标与下一入口：

- 本轮不实现材质 shader 采样、normal/metallic/roughness 多纹理选择、GPU texture upload、纹理缓存持久化或节点/动画导入；首张 artifact 只是连接 provider 的可审计切片。
- 本阶段已继续完成材质引用选择与多纹理缩略图切换；下一入口是 renderer-owned preview target，并行继续音频/视频连续 transport 与资源拖入场景事务。

### 第 2C.9 子阶段：GLTF material → texture artifact 选择

状态：已完成（材质引用选择、WIC 缩略图切换和 AssetSystem 集成测试闭环；GPU 材质采样仍未完成）。

范围与实现：

- `EditorAssetPreviewUiState` 发布当前 material、texture、image index、material label、texture label 和 Base Color/Normal role。
- 模型 Inspector 增加 material/texture 上一项/下一项 retained controls；材质切换会优先选择其 baseColorTexture，再回退 normalTexture/首个 texture，纹理切换按 texture table 有界循环。
- 选择结果通过 image index 找对应 immutable artifact，并复用 WIC memory decoder；旧 future 由 generation、path、source stamp 校验，选择不会触碰活动 World。

契约与证据：

- `EditorInteractionTests` 使用 AssetSystem typed model source 的双材质/双纹理 GLTF fixture，验证 material → baseColor texture → image artifact、WIC snapshot、retained selector region 和材质切换后的第二纹理引用。
- 本轮末全量构建成功，CTest `51/51` 通过（`37.29 sec`），`git diff --check` 通过；Windows UI capture 记录 dark `184` commands / `41` text、light `195` commands / `41` text，均为 `GdiWindowSurface`，GPU backend 能力按实际 `surface-kind` 记录，不把 GDI capture 当 GPU 证据。

非目标与下一入口：

- 本轮不实现 shader material evaluation、颜色空间/采样器完整语义、normal/metallic/roughness 专用显示、GPU texture upload、动画或节点导入。
- 下一轮进入 renderer-owned model preview target 的能力矩阵；若 GPU 目标仍不可用，保留线框 + WIC artifact fallback，同时继续音视频连续 transport 和资源拖入场景事务。

### 第 4.10 子阶段：AudioSystem cursor/seek transport

状态：已完成首个真实音频 transport 时钟闭环；连续视频播放、音视频同步和更多 codec 仍未完成。

范围与实现：

- `IAudioBackend` 增加兼容的可选 `seek()`、`cursor_seconds()` 和 `supports_cursor()` seam；旧 backend 不需要立即实现即可继续编译，并明确报告零时钟能力而不是伪造硬件位置。
- Miniaudio backend 通过 decoder data format 和 PCM-frame seek 实现真实定位，通过 `ma_sound_get_cursor_in_seconds()` 发布播放游标；`AudioSystem` 对秒数做有限/非有限值校验。
- `EditorLayer::MediaSeek` 在音频 voice 存在时同时更新 `MediaPanel` 和 AudioSystem voice；每帧同步从 backend cursor 回写 timeline，Play/Pause/Stop/Loop/Volume 仍沿用 UI bus voice。

契约与证据：

- `AudioSystemTests` 使用 fake backend 验证播放后 cursor 前进、seek 回写和 `supports_cursor()` 能力声明。
- `EditorAudioPreviewTests` 使用 fake cursor backend 验证音频资源播放后 seek 进入真实 voice transport，并继续验证 WAV 元数据、波形、取消、项目根边界和生命周期清理。
- 本轮最终全量构建成功，CTest `51/51` 通过（`33.11 sec`），`git diff --check` 通过；dark/light retained capture 分别记录 `184/41` 与 `195/41` commands/text，均为 `GdiWindowSurface`，未宣称无 cursor backend 支持连续时间线。

安全/性能审计：

- seek 秒数在 UI 归一化输入、音频 duration 和 backend PCM frame 三层进行非有限/非负校验；不把 UI 字符串传给 shell 或文件系统。
- cursor 查询只读取活动 voice 状态，不在 paint 中打开文件或解码；AudioSystem 负责 voice handle 生命周期，资源切换、shutdown 和 backend 回收不会保留悬空句柄。
- backend 不支持 cursor 时保留 MediaPanel 的模型状态并报告能力缺失；不以本地 wall clock 冒充解码器位置。

下一入口：建立 bounded audio/video decoder session 和时钟策略，再进入 renderer-owned model preview target 与 GPU material sampling。

### 第 4.11 子阶段：renderer-owned model geometry pass

状态：已完成首个 Renderer/RenderGraph 模型几何闭环；本轮没有把 base-color 常量误报为纹理采样，也没有宣称独立 Inspector render target 已完成。

范围与实现：

- 新增 `EditorModelPreviewRenderer`，消费 immutable `EditorModelPreviewSnapshot` 与 `EditorModelPreviewSceneState`，在 `EditorLayer::draw` 的 world pass 已构建之后查找第一个 `ColorAttachment0`，追加一个不清屏的 `editor_model_preview` graphics pass。
- 通过已有 `EditorViewportSeam` 约束 viewport/scissor；模型几何、索引、相机矩阵和选中材质 base color 使用 Renderer persistent buffer，HLSL vertex/fragment shader、pipeline 和 material 也由 Renderer 持有，资源可随 Renderer recovery 恢复。
- 当前真实 GPU shader 路径只启用 DirectX 11；Null、Vulkan、D3D12、设备未就绪、没有颜色目标或颜色格式不兼容时，保留 Inspector 线框/WIC 预览并发布明确的 fallback/waiting 状态。
- Inspector 新增 GPU preview 状态行，区分 geometry uploaded、base color applied 和实际 texture sampling；没有有效纹理 artifact 时明确显示 fallback，不把白色占位纹理误报为实际材质纹理。

契约与审计门槛：

- `EditorModelPreviewRendererTests` 使用 Null Renderer 验证能力回退、不向已有 graph 伪造 GPU pass，以及非法 snapshot 的显式拒绝；`EditorInteractionTests` 保持通过，验证 retained editor 与模型选择链路不回归。
- 本轮完整构建成功；CTest 首次顺序运行注册的 52 项中 51 项通过，既有 `shinkou_math_parallel_tests` 在全量负载下超时；随后隔离复跑该测试在 0.06 秒内通过，最终以 `--timeout 180` 完整复跑得到 `52/52`、总计 `17.82 sec`。审计保留首次时序异常，不把它隐藏为不存在。
- `git diff --check` 通过，仅有 Windows 工作树既存的 LF→CRLF 提示；dark D3D11 请求捕获记录 `184` commands、`41` text、`11` assets、viewport `225,67,383.333,184`、`dpi=1.5`、`captured=1`，但返回 `GdiWindowSurface`/退出码 1，未作为 GPU 像素证据。

安全/性能审计：

- GPU upload 前校验 immutable snapshot 的 vertex/index 数量上限、每个顶点有限性和每个 index 的范围；最大约束为 2,000,000 vertices / 6,000,000 indices，拒绝整数溢出和越界引用。
- 模型 pass 不读文件、不解码、不执行命令；shader/material 资源不接受用户路径，颜色格式来自已导入的 graph texture 描述，失败只回退 retained/WIC 状态。
- 几何只在 snapshot revision 变化时上传，相机/视口尺寸变化只更新 bounded constant buffer；每帧最多追加一个模型 pass/一次 indexed draw，不清空场景颜色目标，不覆盖 shell/UI 区域。

未完成风险/下一入口：

- 当前 D3D11 shader path 没有 headless shader execution 或可用 GPU readback；本机 UI capture 仍为 GDI surface，因此“pass 已加入 graph”和“GPU 像素可见”必须继续分开报告。
- 已选纹理 artifact 已通过有界 WIC BGRA8 snapshot 上传为 D3D11 persistent texture，并由线性 clamp sampler 与 pixel shader 采样；没有 artifact 时只使用 1x1 白色 fallback，状态仍明确标记 artifact unavailable。normal/metallic/roughness、sampler/色彩空间完整语义、灯光/深度、动画/节点、独立 offscreen Inspector target 和拖入场景事务仍未完成。
- 下一轮优先建立真实模型 UV/纹理坐标与更完整的 PBR/material slot 能力，再增加 Vulkan/D3D12 shader capability matrix；音视频 bounded decoder session 与 A/V clock 继续并行但不混入本轮 pass。

### 第 4.12 子阶段：D3D11 BGRA8 base-color texture sampling

状态：已完成 D3D11 texture upload、sampler/material binding 和 pixel shader sampling 的首个可审计垂直切片；本轮仍不宣称完整 PBR 或窗口 GPU 像素可见。

范围与实现：

- `EditorModelPreviewRenderer::render()` 接收选中的 immutable `ui::UiImageSnapshot`；只接受 `revision != 0`、尺寸有效且不超过 `512x512` 的 BGRA8 payload。
- Renderer 创建/复用 `bgra8` texture、linear/clamp sampler 和 material descriptor；没有实际 artifact 时创建 1x1 白色 fallback，避免 shader 绑定缺失，同时 `textureSampled` 只在真实 snapshot 成功接入时为 true。
- D3D11 pixel shader 增加 `Texture2D`/`SamplerState` 与 `TEXCOORD0`；当前 UV 是由模型位置生成的受限临时坐标，真实 glTF `TEXCOORD_0`、色彩空间和完整 sampler 语义留到后续轮次。

契约与证据：

- `EditorModelPreviewRendererTests` 的原生路径输出 `native-d3d11-attempted=1 native-d3d11-pass=1 native-texture-sampled=1`；测试同时保留 Null backend fallback 和非法 geometry 拒绝断言。
- 最终 `cmake --build out/build/mingw-debug -j 4` 成功，CTest `52/52 passed`、总计 `37.00 sec`，`git diff --check` 返回 0（仅有 Windows LF→CRLF 提示）；dark D3D11 capture 仍返回 `GdiWindowSurface`，只作为 host/retained command evidence。

安全/性能审计：

- WIC snapshot 在 shader 资源创建前校验 revision、像素字节数、`512x512` 尺寸上限和 row pitch；不在 paint 中读文件、不执行外部命令、不把用户路径交给 renderer。
- 纹理和 sampler 是 renderer-owned persistent resources，只有尺寸、revision 或设备 recovery 需要时才重建/更新；图形 pass 不清屏，保留线框/WIC fallback 和可读失败状态。

非目标与下一入口：

- 本轮未完成真实 UV buffer、normal/metallic/roughness、PBR lighting/depth、颜色空间转换、Vulkan/D3D12 shader path、独立 offscreen Inspector target、动画/节点和拖入场景事务。
- 下一轮从真实 glTF UV/材质 slot 映射与 renderer capability matrix 进入；音视频连续 decoder session/A-V clock 仍单独推进。

### 第 4.13 子阶段：glTF TEXCOORD_0 → D3D11 vertex input

状态：已完成真实 glTF `TEXCOORD_0` 的有界解析、immutable snapshot 传递和 D3D11 vertex input；OBJ 或无 UV 资源继续使用显式 position-derived fallback。

范围与实现：

- glTF provider 支持 float `VEC2` `TEXCOORD_0` accessor，校验 accessor 数量与 POSITION 一致、stride/bufferView 边界、偏移溢出和有限浮点值；每个 primitive 的 UV 与顶点按同一顺序进入 immutable snapshot。
- `EditorModelPreviewRenderer` 将 position 与 UV 打包成 persistent vertex buffer；D3D11 pipeline input layout 增加 `POSITION`/`TEXCOORD0`，pixel shader 使用真实 UV 采样 base-color texture。
- 无 UV provider 不伪造 glTF 坐标，renderer 只生成有界的 position-derived fallback；真实 UV 是否应用由 render state 单独报告。

契约与证据：

- `EditorGltfPreviewTests` fixture 同时覆盖 glTF 和 GLB，断言三项真实 UV 值进入 snapshot；`EditorModelPreviewRendererTests` 断言 native D3D11 path 的 `textureCoordinatesApplied`，并继续覆盖 Null fallback、纹理采样和非法 geometry。
- 4.13 目标编译成功；最终 `cmake --build out/build/mingw-debug -j 4` 成功，CTest `52/52 passed`、总计 `45.96 sec`，聚焦 interaction/glTF/renderer 测试均通过。

安全/性能审计：

- UV accessor 复用 glTF source/buffer/accessor 上限和 cancel seam，不发起网络访问、不访问项目根外文件；GPU upload 前再次校验 UV 数量和有限性。
- vertex stride、D3D11 input layout 和 pipeline cache key 一致更新；UV 只在 geometry revision 变化时重新上传，不增加每帧文件 IO 或解码。

非目标与下一入口：

- 本轮未实现 UV transform、normal/tangent、normal/metallic/roughness 纹理、PBR lighting/depth、完整 sampler/色彩空间、Vulkan/D3D12 shader path 或独立 offscreen target。
- 下一轮进入 material slot/PBR 的有界扩展，或并行推进连续音视频 decoder session；任何跨后端路径必须先建立独立 capability/compile/readback 证据。

### 第 4.14 子阶段：glTF NORMAL → D3D11 lit model preview

状态：已完成真实 glTF `NORMAL` 的有界解析、immutable snapshot 传递和 D3D11 vertex input；模型 pass 增加固定低强度 diffuse light 以帮助观察朝向，不宣称完整 PBR。

范围与实现：

- glTF provider 支持 float `VEC3` `NORMAL` accessor，校验数量与 POSITION 一致、stride/bufferView 边界、有限值和非零长度，并在发布前归一化。
- `EditorModelPreviewRenderer` 将 position、UV、normal 打包为 persistent vertex buffer；D3D11 input layout 增加 `NORMAL`，vertex shader 传递法线，fragment shader 计算有界固定 diffuse 光照后再乘 base-color factor/纹理。
- 无 NORMAL 资源使用稳定的 `(0,0,1)` preview fallback；`normalsApplied` 单独报告是否真的使用 provider 法线。

契约与证据：

- `EditorGltfPreviewTests` 的 glTF/GLB fixture 断言三项归一化法线；`EditorModelPreviewRendererTests` 断言 native D3D11 `normalsApplied`，并继续覆盖 UV、纹理采样、Null fallback 和非法 geometry。
- 最终全量构建成功；第一次全量 CTest 的既有 `shinkou_math_parallel_tests` 在 180 秒时序门槛超时，隔离复跑 `1/1` 在 `0.04 sec` 通过，随后完整重跑得到 `52/52 passed`、总计 `18.99 sec`。

安全/性能审计：

- NORMAL accessor 复用 bounded source/buffer/accessor/cancel 边界；零长度、NaN/Inf 和数量不一致均拒绝，不让非法向量进入 shader。
- vertex input layout、stride 和 pipeline cache key 同步更新；法线只在 geometry revision 改变时上传，fixed light 不引入每帧文件 IO 或额外解码。

非目标与下一入口：

- 本轮未实现 tangent/normal map、metallic/roughness 纹理、真正的 PBR BRDF、深度/阴影/灯光实体、UV transform、颜色空间/sampler 完整语义、Vulkan/D3D12 shader path 或 offscreen target。
- 下一轮进入 material slot/PBR 的最小可验证切片，或并行推进连续音视频 decoder session；继续保留 backend capability/fallback 矩阵。

### 第 4.15 子阶段：glTF metallic/roughness factor lit material

状态：已完成首个材质因子驱动的 renderer-owned lit preview；这是受限的金属度/粗糙度响应，不宣称完整 PBR BRDF、normal map 或真实灯光实体。

范围与实现：

- `EditorModelMaterialPreview` 已有的 `metallicFactor`/`roughnessFactor` 在选中 material 时进入 persistent `PreviewMaterial` constant buffer，统一做有限性与 `[0,1]`/最小粗糙度校验。
- D3D11 fragment shader 使用 albedo、normal、camera direction、固定 preview light、metallic 和 roughness 计算受限 diffuse/specular 响应；没有 material 时继续使用安全默认材质。
- Inspector 状态区分普通 geometry/material ready 和 `metallic/roughness factors applied`，Null/Vulkan/D3D12/设备未就绪继续回退 retained/WIC 预览。

契约与证据：

- `EditorModelPreviewRendererTests` 使用明确的 metallic `0.25` / roughness `0.75` material fixture，断言 `materialFactorsApplied`，并报告 `native-material-factors=1`；shader 契约修复后原生 D3D11 geometry/material/texture smoke 全部恢复。
- 最终 `cmake --build out/build/mingw-debug -j 4` 成功，CTest `52/52 passed`、总计 `39.14 sec`；交互测试继续通过，`git diff --check` 保持通过。

安全/性能审计：

- 材质因子只来自 immutable snapshot 的有界 material table，不读路径、不执行命令、不在 paint 中解码；NaN/Inf 和越界因子回退到有限默认值。
- material constant buffer 只在 snapshot revision 或 material selection 变化时更新；固定光照不创建每帧 GPU 资源，shader/material 仍由 renderer 生命周期管理。

非目标与下一入口：

- 本轮未实现 metallic/roughness texture、normal map/tangent、完整 GGX/IBL/BRDF、深度/阴影/真实灯光实体、颜色空间/sampler 完整语义、Vulkan/D3D12 shader path 或独立 offscreen target。
- 下一轮优先接入 normal map 与 material texture slot 的有界选择，再建立 offscreen Inspector target；连续音视频 decoder/A-V clock 继续独立推进。

### 第 4.16 子阶段：material texture role / normal map preview

执行索引补充：第 4.16 纹理角色/Normal map preview 已完成并有独立审计证据。

状态：已完成 Base Color 与 Normal 纹理角色的显式 GPU 分流；Normal 使用独立纹理/采样器绑定和受限切线基预览，不宣称完整 glTF tangent/UV transform/PBR 语义。

范围与实现：

- EditorModelPreviewTextureRole 进入 renderer API；EditorLayer 根据选中 material slot 的 Base Color / Normal 角色传递语义，未关联纹理保留 Base Color 兼容回退。
- D3D11 material 同时维护底色与法线纹理资源；Normal 使用单独的 t5/s6 描述符槽，避免与现有 SceneFrame b2 冲突；fragment shader 用 ddx/ddy 从位置和 UV 构造预览切线基后采样 Normal。
- retained asset preview state 暴露 role-applied、base-color-sampled、normal-sampled 三种状态，状态行不再把 Normal 纹理报告成底色采样。

契约与证据：

- EditorModelPreviewRendererTests 使用明确的 1×1 decoded image 连续渲染 Base Color → Normal：Base Color 仅报告 baseColorTextureSampled，Normal 仅报告 normalTextureSampled，并检查 normal texture sampled 状态。
- 中途原生 smoke 捕获描述符 space=0 slot=2 冲突，修复为 t5/s6 后 native-d3d11-pass=1 native-material-factors=1 native-texture-sampled=1；目标编译和完整构建成功。
- 最终 CTest 52/52 passed、总计 41.96 sec；shinkou_editor_interaction_tests 通过 14.35 sec，保留 renderer/interaction 双层证据。

安全/性能审计：

- 角色来自 immutable glTF material/texture table，不执行路径、shell 或解码操作；纹理仍由既有 WIC bounded snapshot 提供，GPU 上传保持 512×512 上限。
- 角色切换会使 material constant buffer 和两套纹理资源的 revision 失效；Flat normal fallback 为固定 1×1，未引入每帧文件 IO 或无限增长资源。
- 未关联角色的纹理仍安全走底色回退；Normal 采样只在真实 Normal 角色且 snapshot 有效时开启，失败状态不会追加伪造 render-graph pass。

非目标与下一入口：

- 本轮未实现完整 glTF tangent accessor、normal scale、UV transform、metallic/roughness texture slot、完整 GGX/IBL/BRDF、深度/阴影/真实灯光实体、Vulkan/D3D12 shader path 或独立 offscreen target。
- 下一轮优先接入 metallic/roughness texture 的独立角色与 material slot 状态，再建立 Inspector offscreen target；连续音视频 decoder/A-V clock 和资源拖入场景事务继续独立推进。

### 第 4.17 子阶段：glTF metallic/roughness texture slot

执行索引补充：第 4.17 metallic/roughness texture slot 已完成并有独立审计证据。

状态：已完成 glTF metallicRoughnessTexture 的有界解析、Inspector 角色识别和 D3D11 独立采样；这仍是因子调制的受限预览，不宣称完整 PBR。

范围与实现：

- EditorModelMaterialPreview 新增 metallicRoughnessTexture 索引；glTF pbrMetallicRoughness 解析器做对象、索引和 texture table 边界校验。
- EditorLayer 的 material texture 选择会优先显示 Base Color、Normal 或 Metallic/Roughness 角色；renderer API 新增对应角色，未关联纹理继续安全回退 Base Color。
- D3D11 为 Metallic/Roughness 维护独立 texture/sampler 和 graph read dependency；shader 按 glTF 约定采样 B 通道金属度、G 通道粗糙度并乘以 material factors。
- retained preview state 新增 metallic/roughness texture sampled 标志，状态文本和聚焦测试输出均可区分该路径。

契约与证据：

- glTF provider fixture 包含 metallicRoughnessTexture index，并断言解析后的 material slot 为 0。
- EditorModelPreviewRendererTests 连续执行 Base Color、Normal、Metallic/Roughness 三种角色，断言只有对应 sampled 标志为真。
- 聚焦输出为 native-d3d11-attempted=1 native-d3d11-pass=1 native-material-factors=1 native-texture-sampled=1 native-metallic-roughness-texture=1；全量构建成功，CTest 52/52 passed、总计 42.67 sec。

安全/性能审计：

- metallic/roughness slot 只来自 immutable glTF metadata；索引越界、非对象和缺失 payload 均拒绝或回退，不执行额外路径访问。
- 纹理仍受 WIC 解码和 GPU 512×512 上传上限约束；角色切换刷新 material constant buffer 与独立 texture revision，稳定 sampler 不按帧重建。
- graph 同时声明三类纹理资源的 ShaderRead 依赖；Normal/Metallic/Roughness 资源不会被误报为 Base Color。

非目标与下一入口：

- 本轮未实现完整 GGX/IBL/BRDF、遮挡/发光/透明混合、normal scale、UV transform、tangent accessor、颜色空间/sampler 完整语义、Vulkan/D3D12 shader path 或独立 offscreen Inspector target。
- 下一轮优先建立独立 offscreen model Inspector target 与 GPU readback/视觉验证；随后推进资源拖入场景事务和连续音视频 decoder/A-V clock。

### 第 4.18 子阶段：独立 offscreen model Inspector target

执行索引补充：第 4.18 独立离屏模型预览目标与合成链路已实现，待全量构建和 CTest 完成后封存本轮证据。

状态：已完成 D3D11 的独立 persistent offscreen color target、pass-local target bind 和 editor viewport composite；这一步解决模型预览对场景 color target 的隐式依赖，但仍不宣称 GPU readback 已完成。

范围与实现：

- `RenderCapabilities` 新增 `supportsEditorOffscreenTarget`；`IRenderBackend` 新增 `bind_editor_render_target`，空 color handle 表示交换链，非空 handle 表示当前 editor pass 的显式离屏目标。普通场景仍走原有 strict editor seam。
- `EditorModelPreviewRenderer` 为模型预览维护 BGRA8 persistent target，按物理 viewport 尺寸重建，尺寸上限为 4096×4096；模型 pass 写入该目标，随后由无顶点输入的 fullscreen composite pass 将目标合成回编辑器 viewport。
- composite shader/material/pipeline 与模型 lit shader 独立管理；target、材质、pipeline 和 shader 均随 renderer 生命周期销毁，目标尺寸或 format 变化不会复用不兼容资源。
- retained Inspector state 新增 `offscreenTargetReady` 与 `offscreenCompositeApplied`，状态行明确报告 `offscreen target composited`；Null、Vulkan、D3D12 或能力缺失时继续保留既有 fallback。

契约与证据：

- `EditorModelPreviewRendererTests` 的 native D3D11 fixture 不再创建场景 color target，直接验证模型 renderer 能建立两 pass 链：`editor_model_preview` 与 `editor_model_preview_composite`；连续材质角色切换保持离屏 target 状态。
- 目标编译与全量构建 `cmake --build out/build/mingw-debug -j 4` 均通过；聚焦 renderer 输出包含 `native-d3d11-pass=1`、`native-metallic-roughness-texture=1` 和 `native-offscreen-executed=1`，全量 CTest `52/52 passed`、总计 `19.63 sec`。

安全/性能审计：

- 离屏尺寸由有限的物理 viewport 转换而来，非有限、零值或超过 4096 的尺寸直接拒绝；未引入新路径访问、命令执行或外部依赖。
- target 是 renderer-owned persistent resource，正常帧只 import，不重复创建；只有尺寸/格式变化时销毁并重建。render graph 显式声明 ColorAttachment→ShaderRead 依赖，避免读写同一 D3D11 view。
- pass-local bind 只被 preview callback 调用，普通场景仍由 `set_editor_viewport` 的 strict seam 保护；composite 回到空 target（交换链）后才绘制，降低 UI/场景串写风险。
- 宿主 capture 输出 `commands=184 text=41 assets=11 visible-assets=11 viewport=225,67,383.333,184 dpi=1.5`；图像检查确认 surface-kind 为 `GdiWindowSurface`，因此只记录窗口/retained UI/fallback 证据，不宣称 GPU 像素结果。

非目标与下一入口：

- 本轮未实现独立 GPU readback、像素级 visual oracle、model depth target、完整 GGX/IBL/BRDF、透明/动画/节点、Vulkan/D3D12 offscreen shader path。
- 下一轮优先增加受控 readback API 与 D3D11 staging copy，形成离屏 target 的像素证据；随后推进资源拖入场景事务和连续音视频 decoder/A-V clock。

### 第 4.19 子阶段：受控 GPU readback / visual oracle 基础

执行索引补充：第 4.19 D3D11 staging readback 已实现并通过原生无窗口 smoke；它是诊断/自动化验证 API，不在正常编辑器帧自动执行。

状态：已完成 `TextureReadbackRequest` / `TextureReadback`、Renderer 转发和 D3D11 staging copy，支持指定 mip/layer/矩形与最大字节数；本轮证明 CPU 能稳定取得离屏纹理数据，并在 native smoke 中确认 64×64 readback 有非零绘制输出，但不把它扩展成完整视觉 oracle。

范围与实现：

- RenderBackend 能力新增 `supportsTextureReadback`；Renderer 只允许读取已登记的 persistent Texture2D，并在进入 backend 前校验 mip/layer、矩形、溢出和 maxBytes。
- D3D11 readback 限定 single-sample 的 RGBA8/BGRA8/RGBA16F/R32F color texture，创建临时 staging texture，执行区域 copy，Map 后按紧密 row pitch 返回 CPU bytes；深度、压缩和 MSAA 资源明确拒绝。
- 模型 renderer 暴露其 persistent offscreen color handle，原生测试在提交离屏→合成 graph 后读取完整 64×64 区域，校验尺寸、row pitch、数据有效性、64×64×4 字节上限和至少一个非零像素。

契约与证据：

- 聚焦 renderer 输出为 `native-d3d11-attempted=1 native-d3d11-pass=1 native-material-factors=1 native-texture-sampled=1 native-metallic-roughness-texture=1 native-offscreen-executed=1 native-offscreen-readback=1 native-offscreen-pixel-activity=1 native-uploaded-texture-readback=1`；全量构建 `cmake --build out/build/mingw-debug -j 4` 通过，CTest `52/52 passed`、0 failures、总计 `8.36 sec`。
- readback API 失败时返回清空的结果和明确 `last_error`；不支持 readback 的 Null/Vulkan/D3D12 仍走 capability fallback。D3D11 聚焦输出包含 `native-offscreen-readback=1 native-offscreen-pixel-activity=1 native-uploaded-texture-readback=1`。

安全/性能审计：

- `maxBytes`、区域边界和 size_t 溢出在 Renderer 与 D3D11 backend 双层校验；staging 纹理在成功/失败所有分支释放，Map 失败时不错误 Unmap。
- readback 是显式同步操作，不在 paint/input/tick 默认路径调用；它可能等待 GPU，后续 visual oracle 应放在 QA/开发命令或低频审计任务中。
- 只允许 renderer-owned handle，不能通过 readback 引入文件、网络、shell 或第三方解码副作用。

非目标与下一入口：

- 本轮未实现异步 readback、跨后端 readback、颜色空间转换、PNG/视频导出或自动像素阈值 oracle；当前非零检查只是最低限度的绘制活性信号，宿主 GDI capture 仍不能代替 GPU readback。
- 下一轮可在该 API 上增加低频模型 preview 像素断言/渲染回归 artifact，再推进资源拖入场景事务、深度/完整 PBR 与连续音视频 decoder/A-V clock。

### 第 4.20 子阶段：Project 资源拖入视口与场景引用事务

目标：把 Project 资源浏览器与当前场景建立一条可撤销、可审计的最小连接。拖入动作只创建项目相对路径的 `AssetReferenceComponent`，不在 UI paint/input 路径打开文件、不把解码器或 GPU 句柄写入场景文档；后续模型实例化、音频源绑定、视频材质和 prefab/scene instancing 继续拆分为独立轮次。

实现范围：

- 资源行左键按下后保留拖拽源，跨越 5 logical px 才进入拖拽态；视口内显示 retained drop cue，PointerUp 只接受当前 `viewport_rect()` 内的有限 XY 平面坐标。
- Drop 入口在 EditorLayer 侧做项目根边界、存在性、目录、有限坐标和资源类型复核，只接受 Model/Image/Audio/Video/Material；路径 canonicalization 后存为 project-relative generic path。
- 创建对象前使用已有 `EditorDocument` checkpoint，创建对象名与 5-unit bounded XY 映射，选中对象并通过同一 `document_changed()` 路径接入 undo/redo；AssetReference 属性可被场景 capture/restore。
- 非目标：操作系统外部文件拖放、物理射线/深度落点、模型/音频运行时实例化、prefab/scene 嵌套、异步导入和跨项目引用。

审计与验证安排：

- 单元/交互：源资源过滤、PointerDown/Move/Up、引用路径、XY 映射、状态消息、Undo/Redo；交互回归额外验证未知扩展名拒绝，目录、越界路径和无效坐标由 EditorLayer 入口的独立边界校验覆盖，后续补充专门拒绝分支测试。
- 集成：完整构建与 CTest；视觉：视口 cue、DPI 1.5、dark theme、Project/Scene 面板切换；安全：只允许 `FileSystemService` 项目根内已有文件，paint 不做 IO。
- 本轮实际证据：全量 `cmake --build out/build/mingw-debug -j 4` 通过；`ctest --test-dir out/build/mingw-debug --output-on-failure` 为 `52/52` 通过、0 失败、总计 `8.36 sec`；原生 capture 的 dark 1280×720 与 light 1600×900 均报告 `surface-kind=GdiWindowSurface`、DPI `144`，因此截图只作为窗口/布局证据。
- 下一入口：把 `AssetReferenceComponent` 对接稳定 AssetId/manifest，随后为模型资源增加 renderer-owned scene instance；音频资源进入 AudioSystem source/clip 绑定前先建立生命周期和取消审计。

### 第 4.21 子阶段：Manifest-backed 资源身份与统一拖放入口

目标：让场景引用同时保存可读的项目相对路径和 `AssetSystem` manifest 生成的稳定 `AssetId`。所有编辑器内拖放和未来的操作系统拖放适配器都必须复用同一入口，避免不同入口出现不同的路径边界、类型或撤销语义。

实现范围：

- `AssetReferenceComponent` 增加序列化的 `assetId` 属性；修改路径时自动清空旧 ID，旧的 path-only 场景仍可恢复并保留 `assetId=0` 的迁移状态。
- `EditorLayer` 在 AssetSystem 已连接且 manifest ready 时按规范化物理源路径和资源类型匹配 manifest 条目；匹配失败拒绝写入，未连接 AssetSystem 时保留 path-only 兼容能力。
- `create_asset_reference_at_viewport()` 成为 Project 行、原生拖放以及后续脚本桥接可复用的统一 ingress；其内部仍只做一次 checkpoint、一次对象创建和一次 bounded XY 映射。
- 目录资源也保留拖拽路径，使 UI 能把目录拖到视口并得到明确拒绝，而普通单击仍保持导航语义。
- 非目标：manifest 自动重写、重命名历史迁移、远程/跨项目引用、模型运行时实例化、AudioSystem voice 创建和视频材质绑定。

审计与验证安排：

- 单元/交互：断言 manifest AssetId 被写入、场景 JSON 包含 `assetId`、Undo/Redo 保留身份；未知扩展名、目录、项目外路径和非法 viewport 坐标均断言拒绝且对象数不变。
- 集成：完整构建与 CTest；视觉继续验证 retained drop cue、主题、DPI 和 Project/Scene 切换；性能确认 manifest 只在显式扫描/初始化时读取，drop 只做有界的当前快照匹配。
- 失败路径：manifest 未 ready、根目录重连、身份匹配失败均返回明确状态，不创建对象、不写场景、不触发导入或外部进程。
- 本轮实际证据：`shinkou_editor_interaction_tests` 通过 `1/1`（`11.84 sec`）；最新全量 `ctest` 为 `52/52` 通过、0 失败、总计 `39.46 sec`；完整构建 `cmake --build out/build/mingw-debug -j 4` 通过。
- 下一入口：模型资源使用该 AssetId 建立 renderer-owned scene instance；音频资源在独立轮次建立 AssetId→AudioSystem clip 的生命周期/取消契约。

### 第 4.22 子阶段：AssetId 驱动的模型场景实例渲染

目标：把 4.21 中已经写入场景的模型引用继续推进到可渲染链路。World 文档仍只保存相对路径与稳定 AssetId；异步解析快照、GPU buffer、shader/pipeline 和 RenderGraph pass 由 EditorLayer/renderer 生命周期分别管理。

实现范围：

- 新增 `EditorModelSceneRenderer`，按 AssetId/revision 缓存已校验的顶点/索引 buffer，按普通 GameObject 的 world transform 更新 object buffer；同一 AssetId 可被多个场景对象引用，实例在一个 editor scene pass 中提交。
- EditorLayer 对活动 `AssetReferenceComponent` 读取 manifest model 条目，通过 AssetSystem typed request 获取不可变模型字节，再复用 OBJ/glTF provider 异步解析；generation、source stamp、path 和取消 token 防止旧结果写回。
- 场景 pass 使用 `RenderScene` 当前 editor camera 的 view-projection 和 `Renderer::set_editor_viewport` seam，插入现有 RenderGraph，并在 EditorLayer shutdown 前排空异步任务、释放 renderer-owned 场景缓存。
- 当前仅在已证明的 Windows/D3D11 路径创建 POSITION-only shader/pipeline；Null、D3D12、Vulkan 或 viewport seam 不可用时保留 World 引用并报告明确 fallback，不宣称已完成跨后端像素呈现。
- 非目标：深度 target/遮挡、完整材质和纹理、动画/节点、多种模型 primitive、prefab/scene 嵌套、OS 文件拖放和运行时 AssetId 实例化；这些继续拆成可审计子阶段。

审计与验证安排：

- 单元/交互：拖入模型后等待异步 scene cache ready，断言稳定 AssetId 引用只产生一个加载快照；Null renderer 断言不提交伪 GPU draw 且返回 fallback；Undo/Redo 与旧的 path/assetId 契约继续覆盖。
- 集成：完整构建和 52 项 CTest；EditorLayer→RenderScene→EditorModelSceneRenderer→RenderGraph→backend 的调用顺序作为 integration trace；D3D11 仅使用受限的 2M vertices/6M indices 和有限矩阵/索引校验。
- 视觉：1280×720、DPI 144、dark/tree capture 保留 retained command、text、viewport 和窗口证据；本机此次 capture 仍返回 `GdiWindowSurface`，因此只记录宿主布局证据，不把它当作 GPU scene pixel proof。
- 下一入口：为 Audio AssetId 建立 AudioSystem clip/source 绑定、取消和卸载契约，然后再回到模型深度/材质和跨后端 shader 能力。

### 第 4.23 子阶段：音频资源引用与 AudioSystem 生命周期绑定

目标：让编辑器媒体预览使用 manifest-backed 音频身份时复用 `AudioSystem` 的 `AudioAssetId`，同时保持场景文档不保存 voice、decoder 或后端对象。

实现范围：

- `EditorLayer` 为 manifest audio AssetId 维护受控的 `AssetId → AudioAssetId` clip cache；播放前通过当前安全解析路径加载 clip，再通过 `AudioSystem::play(AudioAssetId, AudioPlayParams)` 创建 UI bus voice。
- path-only 兼容预览使用 editor-owned 临时 clip，停止、切换资源、项目根变化、音频系统替换和编辑器关闭时释放；manifest-backed clip 在当前编辑器会话复用，并在重连/关闭时统一卸载。
- 原有播放/暂停/停止/循环/音量/seek 控件不变，继续使用 `AudioSystem` 的 voice transport；当系统不可用或 backend 不支持时保留诚实的 unavailable 状态。
- 非目标：场景 AudioSource 组件、运行时自动播放、流式 decoder 的后台预取、3D 空间混音和跨项目音频引用；这些需要独立的文档/运行时生命周期。

审计与验证安排：

- 单元/集成：保留 WAV provider、fake backend、seek/暂停/停止状态回归，新增播放后 AudioSystem asset pool 增长、停止后 path-only clip 回收断言。
- 安全/性能：clip cache 只保存整数句柄，不把文件句柄或 decoder 写入 UI/World；加载入口仍受项目根校验，重复播放不重复注册同一路径，所有 unload 发生在系统切换/停止/关闭边界。
- 下一入口：manifest audio binding 稳定后，再设计可序列化的 AudioSource component 与场景运行时同步；模型方向并行推进 depth/material/texture。

### 第 4.24 子阶段：编辑器场景 color target 到 backbuffer 的宿主呈现

目标：补齐“场景 pass 已提交但窗口仍只显示 UI”的宿主集成链路，让 editor 分支把当前场景 color target 显式呈现到编辑器 backbuffer，再交给 retained UI overlay；该轮只修正呈现顺序和 uniform 隔离，不扩大模型材质范围。

实现范围：

- `EngineSample` 的 editor 分支新增独立 `editor_scene_present` pass：导入场景 color target 和 present material，绑定独立 identity scene/object uniform，并用 `bind_editor_render_target` + fullscreen sprite 将场景结果提交到编辑器 backbuffer。
- 不复用 forward renderer 的场景相机 uniform，避免 present pass 为了 identity transform 修改后续 UI 或场景数据；原有 `editor_viewport_lifetime` 空 pass 移除，场景 present、model scene、UI overlay 形成可读的顺序链。
- 保持当前 color target 的尺寸、D3D11 编辑器 viewport seam 和后端能力约束；本轮不引入通用跨后端 present API、深度 attachment、模型材质/纹理或 OS 文件拖放。

审计与验证安排：

- 集成：全量构建必须包含 sample、capture、renderer 和全部测试；直接运行 `shinkou_engine_sample --frames 1 --editor dx11`，检查 native UI、viewport scissor、RenderGraph pass/draw trace。
- 视觉：至少覆盖 1280×720 dark/tree 与 1600×900 light/tree；capture 必须使用绝对输出路径并报告 `mode=EngineGpuReadback` / `surface-kind=GpuClientSurface`，避免把 child 工作目录下的 GDI fallback 当作 GPU 证据。
- 安全/性能：present 只读已存在的 renderer-owned target，不访问文件系统、不启动进程；identity uniform 为有界小 buffer，pass 不复制场景数据或创建每帧持久资源。
- 本轮实际证据：完整构建通过；CTest `52/52` 通过、0 失败、总计 `21.11 sec`；direct sample 输出 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=1 passes=4 draws=3`，其中包含 `editor_scene_present draws=1`；dark capture `1280×720` 与 light capture `1600×900` 均退出 `0`，分别报告 GPU readback、DPI `144`，dark retained trace 为 `commands=184 text=41 assets=11 viewport=225,67,383.333,184`，light 为 `commands=195 text=41 assets=11 viewport=225,67,596.667,304`。
- 下一入口：把 present seam 提炼成 renderer-owned 的通用 editor scene presentation contract，再补 D3D11 depth/clear policy、模型材质/纹理和跨后端 shader；并继续处理 OS drag/drop adapter 与 AudioSource runtime binding。

### 第 4.25 子阶段：Windows 原生文件拖放接入统一资源引用入口

目标：让用户可以像 Unity Project 窗口一样把 Windows 资源文件拖到编辑器视口，同时保证原生消息不绕过现有项目根、资源类型、manifest、坐标和 Undo/Redo 校验。

实现范围：

- `platform::Window` 启用 `WM_DROPFILES`，只读取最多 64 个文件、每个最多 32768 个 UTF-16 字符，转换为 UTF-8 client-area path/pixel 后交给宿主；窗口销毁前停用接收并清除回调，避免生命周期悬挂。
- 新增无 `editor::World` 命名污染的 `CoordinateSpaces.h`，把 Window client pixel → editor logical pixel 转换作为公共坐标契约；Engine 按当前 DPI 与用户 UI scale 把原生事件送入 `EditorLayer::create_asset_reference_from_window_drop()`。
- `FileSystemService::project_relative_existing()` 对绝对路径先 canonicalize，再进行项目根边界检查；只有项目内已存在文件才转为相对路径，随后完全复用 4.21 的 manifest AssetId、类型过滤、viewport 落点与可撤销场景引用事务。
- 本轮明确不复制外部文件、不接受项目外路径/目录、不导入远程资源，也不在窗口过程、paint 或 input 循环中解码媒体或创建 GPU/Audio 句柄。

审计与验证安排：

- 单元：文件系统绝对路径/项目外/相对路径边界，Window adapter 回调与 destroy 后回调清理。
- 集成：EditorInteraction 用绝对项目内模型路径验证 native ingress、AssetId、对象创建与 Undo；用项目外路径验证对象数保持不变；全量构建与 CTest。
- 视觉/平台：保留 4.24 的 GPU surface capture 作为窗口宿主证据；下一次 Windows 手工 QA 需要真实 Explorer 拖放到 1280×720 与 DPI 144 视口，记录 native message → logical point → scene transaction trace。
- 本轮实际证据：完整构建通过；focused `shinkou_editor_interaction_tests` `1/1` passed、`6.81 sec`；全量 CTest `53/53` passed、0 失败、总计 `10.75 sec`；`shinkou_window_file_drop_tests` 与 `shinkou_file_system_tests` `2/2` passed、`1.05 sec`。
- 下一入口：补充通用 renderer editor scene presentation contract 和真实 OS drag/drop 手工证据；并行进入可序列化 AudioSource runtime binding、模型 depth/material/texture。

### 第 4.26 子阶段：可序列化 AudioSource 与场景音频生命周期桥接

目标：把音频从“编辑器预览可播放”推进到场景级组件，让 World 只保存可迁移的音频意图，`AudioSceneSystem` 负责把它连接到 `AudioSystem` 的 clip/voice 生命周期。

实现范围：

- 新增 `AudioSourceComponent`，序列化 `clipPath`、稳定 `assetId`、bus、`playOnStart`、loop、volume、pitch、spatialized 和 streaming；组件不保存 `AudioAssetId`、`AudioVoiceId` 或 backend/decoder 指针。
- `World` 注册 `AudioSource` 类型，继续复用通用反射/`EditorDocument` capture，因此场景 JSON 可以保存和恢复音频源配置，旧场景不需要额外迁移步骤。
- 新增 `AudioSceneSystem`：按规范化项目相对路径共享 clip 引用，按对象生命周期创建/停止 voice，支持 play-on-start、bus、循环、音量、音高、空间位置更新；对象禁用、删除、路径/策略改变、播放结束和引擎关闭时释放引用。
- `Engine` 在每帧 `AudioSystem::update` 后同步 World，在音频系统关闭前先清理场景句柄；当 `AudioConfig.assetRoot` 未显式设置时使用 editor/assets project root，保证运行时相对路径解析一致。
- 非目标：本轮不做 AssetSystem manifest 的 `assetId` 反向校验、不做 listener/3D 衰减模型、不做 streaming 预取/解码进度、不把 AudioSource 做成专用 Inspector 面板，也不宣称音频文件已经在 tick 内解码。

审计与验证安排：

- 单元：fake backend 覆盖场景创建、同路径 clip 共享、禁用/切换路径、voice 结束后的 clip 回收、手动 play/stop、对象删除、shutdown 和 JSON 序列化。
- 集成：完整构建、`shinkou_audio_scene_system_tests` 专项测试、全量 CTest；sample 走真实 Engine→World→AudioSceneSystem→AudioSystem 初始化/关闭路径。
- 安全/性能：只接受非空项目相对路径，bus/volume/pitch 有界钳制；scene bridge 只保存有界句柄和小型配置，不在每帧读取文件内容、不启动进程、不访问网络；相同路径在会话内复用一个 AudioSystem clip。
- 本轮实际证据：完整构建通过；专项 CTest `1/1` passed、`1.20 sec`；全量 CTest `54/54` passed、0 失败、总计 `40.10 sec`；direct sample `--frames 1 --editor dx11` 退出 `0`，报告 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=1 passes=4 draws=3`，包含 `editor_scene_present`。
- 下一入口：接入 `AssetSystem` manifest 的 `assetId → audio clip` 校验与失效通知，补 AudioSource 专用 Inspector/总线选择；随后推进 listener/3D spatial、streaming policy，以及模型 depth/material/texture。

### 第 4.27 子阶段：AudioSource manifest 身份校验与失效诊断

目标：让带有稳定 `assetId` 的场景音频源真正绑定到当前 AssetSystem manifest，禁止 path 与身份错配时创建 voice，同时允许 manifest 异步扫描完成后安全重试。

实现范围：

- `AssetSystem` 增加 `manifest_ready()` 与 `find_manifest()` 只读查询；扫描开始时暂时撤销 ready，完成后以 immutable snapshot 发布 ready，挂载变化和 shutdown 清理旧缓存，查询不接触扫描锁而不阻塞主循环。
- `AudioSceneSystem` 接收可选 AssetSystem 解析器；当 `AudioSource.assetId != 0` 时要求 manifest 条目存在、类型为 `audio`、源文件仍是项目内普通文件，并将 manifest source 与 `clipPath` canonicalize 后比较；不满足条件不加载 clip、不创建 voice。
- manifest 尚未就绪使用 pending 状态，下一次同步会重试；未知 ID、错误类型、缺失文件和 path/ID 不一致进入明确失败诊断。`assetId == 0` 继续保留 path-only 兼容行为。
- AssetId 变化纳入 source binding 配置变更；Engine 每帧把 `assets_` 传入 AudioSceneSystem，保持 World 只存路径、身份和播放策略，不保存运行时句柄。
- 非目标：本轮不修改 retained UI 像素，不增加导入/复制事务，不做 manifest 自动重写、重命名迁移、音频 decoder 预取、3D listener 或专用 Inspector 控件。

审计与验证安排：

- 单元：AssetSystem manifest ready/id 查询、mount/shutdown 失效；AudioScene 覆盖 pending→ready 重试、同路径共享、错误类型/未知 ID/path mismatch 拒绝、缺失文件、结束 voice 回收和 JSON 契约。
- 集成：完整构建、AudioScene 与 AssetSystem focused tests、全量 CTest；sample 重新 clean-first 链接后走真实 Engine 初始化、AudioSystem/AssetSystem/Editor 生命周期和 DX11 UI host 冒烟。
- 安全/性能：AssetId 校验只在 source 启动或显式 play 边界读取有限 manifest/file metadata；不在 paint/input 中扫描文件、不启动进程、不访问网络；manifest 查询不会在扫描锁竞争时阻塞引擎帧。
- 本轮实际证据：完整构建通过；`shinkou_audio_scene_system_tests` 与 `shinkou_assets_tests` focused `2/2` passed、`2.29 sec`；全量 CTest `54/54` passed、0 失败、总计 `19.45 sec`；sample clean-first 重链通过，exe 非空，单帧 `--frames 1 --editor dx11` 退出 `0` 并报告 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 passes=4 draws=3`，短多帧 `--frames 10 --editor dx11` 退出 `0` 并报告 `frames=10 passes=40 draws=30`、`editor-ui-commands=184 text=41 assets=11 visible-assets=11`，包含 `editor_scene_present`。
- 下一入口：把 AssetSystem manifest 身份变化转换为可观察的 editor/runtime invalidation 事件，补 AudioSource clip picker、bus 下拉和错误态 Inspector；之后实现 listener/3D spatial 与 streaming policy，再回到模型 depth/material/texture。

### 第 4.28 子阶段：manifest revision 驱动的 AudioSource 主动失效

目标：让 manifest 重建、挂载变化、seed 和 shutdown 能以轻量 revision 传播到运行时，使已经绑定的 AudioSource 在下一次 Engine tick 中释放旧 voice，并按新快照安全重试。

实现范围：

- `AssetSystem` 增加单调 `manifest_revision()`；manifest 开始扫描或发生挂载/seed/shutdown 边界时推进版本，查询仍只读取 immutable snapshot，不暴露内部 map。
- `AudioSceneSystem::SourceBinding` 保存绑定时的 manifest revision；revision 变化会主动停止并释放 voice/clip，`playOnStart` 源按新 manifest 重新校验，pending/invalid 状态继续可诊断。
- `AudioSceneDiagnostics` 增加 `invalidatedSources`，区分 manifest 变化导致的重绑定与普通加载失败；World/JSON 仍只保存场景意图，不保存后端句柄。
- 非目标：本轮不引入跨线程回调、不改 retained UI 像素、不做重命名迁移事务和专用 AudioSource Inspector；下一轮在 editor 事务层补 clip picker、bus 下拉、错误态和 Undo/Redo。

审计与验证安排：

- 单元：AssetSystem scan/seed/shutdown revision 单调性；AudioScene 覆盖播放中 revision 变化的停播重绑、manifest 删除失败、资源恢复重试，以及现有共享 clip、voice 回收、JSON 和 shutdown 契约。
- 集成：完整构建、AudioScene/AssetSystem focused tests、全量 CTest、DX11 editor sample 多帧生命周期烟测。
- 安全/性能：revision 是原子标量，不在 Engine tick 注册回调或取得 manifest 扫描锁；voice 失效先释放运行时资源，再从当前 immutable snapshot 校验 path/ID；不新增文件写入、网络、shell 或第三方依赖。
- 实际证据将在实现完成后回填到 `docs/UIUpgradeAudit.md`，包含构建、focused/full CTest、样例帧数和 UI/render graph trace。

### 第 4.29 子阶段：AudioSource 专用 Inspector 资源选择与总线控制

目标：把 AudioSource 从通用字符串属性提升为可发现、可校验、可撤销的编辑器控制，同时保持项目相对路径、AssetId 和运行时 AudioSceneSystem 的单一契约。

实现范围：

- Inspector 隐藏原始 `assetId` 字段，`clipPath` 使用来自 immutable manifest 的有界音频资源选择器；选择资源时以项目相对路径写回 `clipPath`，并自动联动稳定 `assetId`。
- `bus` 提供 Master、Music、SFX、Voice、Ambient、UI 选择；资源状态在组件顶部显示 Ready、path/AssetId mismatch、identity unavailable 或 path-only 等可诊断信息。
- retained UI 增加语义选择按钮、受边界约束的 popup、滚动、Escape/外部点击关闭和 DPI-safe 布局；选择仍通过现有 EditorLayer checkpoint/Undo 事务提交。
- 非目标：本轮不解码音频、不增加播放 transport、listener/3D 衰减、streaming 进度、manifest 自动重写或外部导入复制。

审计与验证安排：

- 单元/交互：EditorInteraction 覆盖 picker 打开、manifest 音频选择、项目相对路径与 AssetId 联动、bus 提交、状态文案、原始 assetId 隐藏以及既有属性 Undo/Redo。
- 集成：完整构建、全量 CTest、D3D11 editor sample 多帧 smoke；验证 UI retained command、native-ui、viewport-scissor 与 `editor_scene_present` trace。
- 安全：选择项只来自 immutable、最多 256 项的 manifest 快照；paint 不做文件 IO、进程启动、网络访问；写回路径经过 UTF-8 lexical normalization，并保持项目边界。
- 性能/视觉：静态 choice snapshot 在帧间复用；popup 高度最多 220 logical px，长列表滚动而不是无限绘制；沿用 flat native Windows surface/border/accent token 和现有 DPI scale。
- 失败状态与回滚：manifest 未就绪时保留明确状态并允许之后刷新；选择失败不改变 World；回滚可移除 choice metadata 与 specialized renderer，恢复通用属性编辑。
- 下一入口：AudioSource playback transport、listener/3D spatial 参数与 streaming policy；并行完善 manifest rename/import migration 和模型材质/纹理预览。
