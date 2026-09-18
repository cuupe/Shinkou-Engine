# Shinkou Engine UI 升级审计台账

本文件是每轮升级的证据入口。审计只记录已执行或已验证的事实；“计划”“占位”“未验证”不能写成“已支持”。

## 基线审计：第 0 轮

| 项目 | 结果 | 证据/说明 |
|---|---|---|
| 权威 UI 边界 | 通过 | `Engine::tick -> EditorLayer -> EditorUi -> UiRuntime/UiRenderList -> Renderer -> IRenderBackend`；当前实现位于 `engine/`。 |
| 文件安全边界 | 通过 | `FileSystemService` 将路径限制在项目根目录，提供原子写和变更扫描；已有 `shinkou_file_system_tests`。 |
| retained 布局增量 | 通过 | 清洁子树复用 arranged rect，变化的 flow/flex sibling 仍递归；已有 `UiRuntimeTests` 回归覆盖。 |
| 构建基线 | 通过 | `out/build/mingw-debug` 已完成构建。 |
| 自动化测试基线 | 通过 | 最近一次全量 CTest：50/50；包含 AssetSystem manifest、GLTF/GLB provider、视频时间点帧、编辑器交互和既有 UI/渲染测试。 |
| 视觉证据 | 部分通过 | 已有 dark/light/high-contrast 证据；100%/125% DPI 仍需补充稳定截图和交互记录。 |
| 真实预览 provider | 进行中 | 文本、WIC 图片、音频 transport/波形、Media Foundation 时间点帧、OBJ/GLTF 几何与隔离模型场景已接入；GPU 材质采样、连续视频/A-V 同步和更多 codec 仍未完成。 |
| 外部编译器/IDE | 进行中 | 工具链发现、受控进程 runner、诊断模型、`compile_commands.json` 读写、multi-profile 编辑/切换、诊断过滤、受限 PID 状态跟踪、项目文件发现/关联、clangd 配置写回和行级 IDE launcher 已存在；solution 内部 project graph、toolchain flags 和 IDE 子进程树语义仍未完成。 |

## 第 1 轮：资源/预览契约

状态：已完成（本轮契约层）；真实 provider 留待后续轮次。

范围：只建立确定性的资源分类、MIME/标题/状态和 capability bit，不引入解码器，不执行文件 IO，不声称已具备真实缩略图、播放或模型渲染。

验收清单：

- [x] `AssetPreviewKind` 覆盖文件夹、文本、图片、音频、视频、模型、场景、材质、shader、字体、压缩包和未知二进制。
- [x] 扩展名分类大小写不敏感，未知扩展名稳定落入 Binary。
- [x] descriptor 从 `FileEntry` 构造，paint 路径不访问磁盘。
- [x] capability 明确区分 Metadata、Thumbnail、Interactive；当前文件类只有 Metadata，文件夹保留导航 Interactive。
- [x] Inspector 展示 descriptor 的真实标题和状态，不再把缺失 provider 写成已完成预览。
- [x] 目标测试、全量测试、构建和 `git diff --check` 均通过：`shinkou_asset_preview_tests` 通过；当时全量 CTest 42/42 通过。
- [x] 已记录本轮未解决风险和第 2 轮输入；真实 provider、工具链进程和 MIME/文件头校验留到后续轮次。

本轮风险：

- 扩展名只能表达初步类型，真正导入时仍需 MIME、文件头和 provider 验证。
- `.json` 可能是场景、材质或普通配置，本轮按文本处理，后续由 manifest/importer 提供更强类型。
- 二进制格式的预览必须受大小、时间和解码资源上限约束，不能直接在 UI 线程打开。

## 第 2A 子阶段：工具链契约与构建计划

状态：已完成。

已实现：

- 工具发现快照覆盖 CMake、Ninja、MSBuild、Clang、clang-cl、GCC、G++ 和 .NET SDK。
- profile 生成 CMake configure/build、Ninja、MSBuild 和 .NET build 的显式参数数组。
- source/build/project 路径必须落在项目根目录内；NUL 参数和越界路径被拒绝。
- GCC/Clang 与 MSVC 诊断格式解析为统一文件/行/列/严重级别模型。
- 极大行号/列号不会抛异常或污染诊断列表。
- `EditorLayer` 在初始化和项目根切换时持有同一 build-system 快照；刷新动作不是 paint 路径操作。

验证证据：

- `shinkou_editor_build_system_tests` 覆盖工具发现、构建计划、越界路径、MSVC/GCC 诊断和畸形数字输入。
- 第 2A 的完整构建和 CTest 结果：`cmake --build out/build/mingw-debug -j 2` 通过；当时全量 CTest 42/42 通过；`git diff --check` 通过。

剩余风险/下一步：

- 当前只生成计划，还没有真正的异步进程执行、取消、超时、stdout/stderr 管道和环境白名单。
- 发现路径来自 PATH 与项目 `.shinkou/tools`/`Tools` 目录，Visual Studio 安装目录和 Windows 注册表发现留待后续增强。
- retained Build 面板和诊断双击跳转尚未接入，暂不宣称编辑器内可执行构建。

## 第 2B 子阶段：异步进程执行

状态：已完成（Windows 执行链）；Build 面板的丰富诊断交互属于 2C。

已实现：

- Windows 使用 `CreateProcessW`、独立 stdout/stderr 管道和明确的 argv，不经过 shell 拼接。
- 构建任务在后台 `std::async` 顺序执行 configure→build，UI 线程只轮询 future 结果。
- 支持取消、超时、非零退出码、启动失败、输出大小上限和统一诊断归并。
- 默认只继承小型 OS 安全环境变量白名单；工具链变量可以显式加入，环境覆盖单独传递。
- retained UI、原生菜单和 Ctrl+B 均可触发 Build；Cancel Build/Refresh Toolchains 也经过统一 `EditorCommand`。
- 编辑器关闭前请求取消并等待后台任务收束，不把子进程句柄留到 UI 生命周期之外。

验证证据：

- `shinkou_editor_build_system_tests` 实际验证 `cmd.exe` stdout、受控环境变量、非零退出、超时、启动前取消，以及当前环境 CMake `--version`。
- 全量构建/CTest 在 2B 收尾后重新执行并通过；本轮再次验证为 50/50；`git diff --check` 通过。
- UI capture 输出：D3D11 `device-ready=1`、`editor-ui-commands=184`、`editor-ui-text=41`、DPI 1.5；当前捕获工具回退为 `GdiWindowSurface`，`--require-gpu` 返回 6，因此本轮保留 headless/命令证据，不能把 GDI 黑客户区当作新的 GPU 像素证据。

剩余风险/下一步：

- 当前默认 profile 还没有持久化的用户配置、Visual Studio 安装目录发现和 MSVC `INCLUDE/LIB` profile 编辑界面。
- stdout/stderr 已归并到结果，但尚未在 retained Build 面板中提供实时日志滚动、诊断过滤和双击跳转。
- 2C 继续把 build profile、工具链列表、任务状态和 diagnostics 接入 Settings/Build 面板，再进入资源索引与真实预览 provider。

## 第 2C 子阶段：retained Build 面板首个垂直切片

状态：进行中（面板首个垂直切片、文本预览/诊断跳转、compile commands 连接和实时日志已完成，外部 launcher 仍未完成）。

已实现：

- `EditorBuildUiState` 作为 retained model 快照，包含 profile 名称、任务状态、工具链可用性、诊断摘要和受限输出行。
- Build 面板接入停靠 workspace、Window 菜单、Windows 原生菜单、工具栏 Build 命令和 `Ctrl+B` 的统一 `EditorCommand` 路由。
- Build/Cancel/Refresh 三个按钮拥有 retained hit-test region；执行 Build 会自动显示并激活 Build 标签，不改变其他面板拓扑。
- 布局序列化增加 `showBuild` 字段并提升 `layoutVersion` 到 3；旧布局缺少字段时仍使用隐藏默认值。
- build state 只在快照内容变化时递增 `EditorUiModel::revision`，避免每帧复制/重绘 Build 面板；输出行限制为最近 12 行，每行限制 240 字符。

验证证据：

- `EditorUiModelTests` 验证 build state 的变更 revision 和相同快照稳定性。
- `EditorInteractionTests` 验证 Build 面板显隐、停靠激活和 Build/Refresh retained 控件注册。
- 受影响目标构建通过；目标测试 3/3 通过；当时全量 CTest 43/43 通过。
- 本轮未新增真实 GPU 像素证据；仍沿用 2B 的事实：D3D11 设备就绪，但捕获工具拿到 GDI surface，不能把黑色 GDI 客户区当作 retained UI 像素成功。

安全/性能审计：

- 面板只消费不可变的 `EditorBuildUiState` 内容；不在 paint 路径发现工具、启动进程、读取文件或解析诊断。
- 工具可执行文件路径、工作目录和参数仍由 2A/2B 的 build system/process runner 负责；面板没有新增 shell 拼接入口。
- 输出、诊断、工具链展示均有数量/长度上限；UI revision 对相同状态去重。

未完成风险/下一步：

- Build 面板已能消费进程 runner 的 stdout/stderr 增量快照；该历史切片先接入诊断跳转，诊断过滤和行级定位选中态已在后续 2C.6 完成。
- 在该历史阶段 build profile 尚未在设置界面持久化编辑；实时日志、诊断过滤/行级定位和 VS/Rider/VS Code/clangd launcher 已由后续切片接入。
- 下一垂直切片曾计划实现外部 IDE launcher 与 profile 绑定；该入口已完成，后续继续按第 3/4 轮推进真实图片/音频/视频/模型 provider。

## 第 2C.1 子阶段：文本资源预览与诊断跳转

状态：已完成（文本预览 provider 的首个受限切片；多媒体/模型 provider 未完成）。

已实现：

- `FileSystemService::read_text_limited` 在既有项目根安全边界内读取文本前缀，单次上限 64 KiB；完整 `read_text` 仍保持 16 MiB 超限失败语义。
- 资源单击后由 `EditorLayer` 后台读取文本，结果带 generation/source stamp，过期结果不会覆盖新选择；UI model 只接收不可变的 preview snapshot。
- Inspector 对文本资源显示真实代码行、行号和截断/失败状态；非文本资源仍明确显示 provider 尚未注册。
- Build diagnostics 保存文件、行、列结构；有项目内文件的诊断行可通过 retained 命令打开 Inspector，目标路径必须是项目根内现有文件。

验证证据：

- `FileSystemTests` 覆盖限长读取和截断标记。
- `EditorUiModelTests` 覆盖 preview snapshot 的 revision 去重。
- `EditorInteractionTests` 覆盖异步 `.txt` 预览、OpenAsset 路由和项目根外路径拒绝。
- 本轮受影响目标测试 3/3 通过；当时完整构建通过，全量 CTest 43/43 通过，`git diff --check` 通过。

安全/性能审计：

- 读取在后台 future 执行，paint 不打开文件、不解码、不等待；关闭编辑器前显式收束 preview future。
- 文本字节、展示行数和单行长度均有上限；generation 与 source stamp 防止快速切换或文件变更造成旧结果回写。
- `OpenAsset` 拒绝绝对路径、路径穿越、目录和项目根外资源；诊断路径来自 build parser 后仍经过 FileSystemService 再验证。

未完成风险/下一步：

- 当前仅实现文本资源 provider；图片尚无像素缩略图，音频尚未接入 `AudioSystem` transport，视频尚无解码帧，模型尚无隔离 preview scene。
- 诊断跳转已能选中文件，但尚未定位到代码行、过滤诊断或保持行级选中态。
- `compile_commands.json` 的导入/导出已接入 Build 面板；默认使用项目根安全文件系统和受限 JSON parser，尚未绑定 profile 或外部 IDE launcher。
- 下一轮先完成 Build 实时日志，再进入第 3 轮资源索引/immutable manifest 快照，并把图片 provider 接到 D3D11 UI texture seam。

## 第 2C.2 子阶段：compile_commands.json 导入/导出

状态：已完成（项目内编译命令数据连接；不执行导入的命令）。

已实现：

- `EditorCompileCommands` 兼容标准 `arguments` 数组和常见 `command` 字符串，归一化 directory/file 到项目根内绝对路径。
- JSON 输入、命令数量、参数数量和单项路径均有限制；拒绝重复字段、非法字符串、NUL、项目根外路径和畸形引号。
- Build 面板提供 Import/Export Commands，状态展示命令数量；`EditorCommand`、Windows 原生菜单、ImGui 菜单和 retained 命令路由一致。
- 导入内容只作为 IDE/诊断数据保存；不会被当作 shell 字符串执行，导出统一使用 JSON `arguments` 数组。

验证证据：

- `EditorCompileCommandsTests` 覆盖两种输入格式、路径归一化、序列化往返、根外拒绝和导出拒绝。
- `EditorInteractionTests` 覆盖编辑器内导入一条命令、导出到项目内文件和 Build 面板命令链。
- 完整构建通过；当时全量 CTest 43/43 通过；`git diff --check` 通过。

安全/性能审计：

- parser 不调用 shell、不启动进程、不访问项目根外文件；文件读写继续由 `FileSystemService` 的路径检查和原子写负责。
- 16 MiB 输入上限、32768 条命令上限和 4096 参数上限抑制资源放大；UI 只消费计数/状态快照，不在 paint 解析 JSON。
- 导入失败不会覆盖上一份有效命令快照；导出失败会清空待写 JSON 并保留原快照。

未完成风险/下一步：

- 尚未将 command database 绑定到 build profile 的编译数据库选择，也没有根据它启动外部 IDE。
- `command` 字符串采用安全的轻量 tokenization，不模拟所有 shell 语义；复杂 shell wrapper 需要明确失败并由用户改用 `arguments`。

## 第 2C.3 子阶段：Build 实时 stdout/stderr 快照

状态：已完成（增量输出链路；诊断仍在进程结束后统一解析）。

已实现：

- `EditorBuildProcessOptions::outputCallback` 在 Windows 管道读取每个 bounded chunk 时分别报告 stdout/stderr；异常 observer 不会中断进程收束。
- `EditorLayer` 以带 mutex 的共享 live snapshot 接收 worker 输出，按行限制 240 字符、最多 128 行，并保留未换行的 partial preview。
- Build retained model 只在 live revision 改变时同步，最终进程结果仍作为完成态快照；取消、超时、失败和 shutdown 都会 flush pending 行并等待 future。

验证证据：

- `EditorBuildSystemTests` 覆盖 stdout/stderr callback、环境白名单、成功和非零退出路径。
- `EditorInteractionTests` 仍通过 Build 面板命令注册、停靠切换、资源预览和 compile commands 交互回归。
- 完整构建通过；当时全量 CTest 43/43 通过；`git diff --check` 通过。

安全/性能审计：

- callback 只接收进程 runner 已读取的临时 chunk；UI 对象不在 worker 线程访问，主线程仅复制有界 snapshot。
- 行数、单行长度和 revision 更新均有上限/去重策略，不把构建输出无限累积到 retained tree。
- 进程 runner 继续使用独立 executable/argument array、环境 allow-list、超时和取消；实时链路没有新增 shell 入口。

未完成风险/下一步：

- 在该旧切片收尾时，诊断仍在进程完成后解析；增量诊断过滤、行级定位选中态和实时错误计数已在后续 2C.6 完成。
- 在该旧切片收尾时，外部 IDE launcher 和 profile 持久化绑定仍未完成；后续 2C.1/2C.6 已补齐，图片/音频/视频/模型真实 provider 则由后续轮次继续推进。

## 第 3.1 子阶段：不可变资源索引快照

状态：已完成（首个索引垂直切片；增量导入、指纹缓存和派生资源缓存仍未完成）。

已实现：

- 后台文件扫描同时生成 `EditorAssetIndexSnapshot`，包含稳定 revision、资源 descriptor、写入时间戳和相对路径查找表。
- snapshot 构建完成后以 `shared_ptr<const ...>` 交给 UI；扫描结果发布后不可变，Inspector 预览请求不再在线性扫描 `projectFiles_`。
- 资源预览请求通过索引条目检查类型和 source stamp；快速切换、重新扫描和旧异步结果仍由 generation/source stamp 规则淘汰。
- 项目根切换、布局载入和布局重置会清空旧索引，避免跨项目复用资源描述。

验证证据：

- `EditorAssetIndexTests` 覆盖 revision、路径查找、descriptor 稳定性和源文件列表变更后的 snapshot 隔离。
- `EditorInteractionTests` 覆盖带索引的异步资源预览与现有交互命令回归。
- 受影响目标构建通过；目标测试 2/2 通过；全量构建通过；全量 CTest 44/44 通过；`git diff --check` 通过。

安全/性能审计：

- 索引构建只消费 `FileEntry` 扫描结果和纯函数 descriptor，不在 paint 路径访问磁盘或执行解码。
- lookup key 使用项目内相对路径；预览内容读取仍由 `FileSystemService` 的项目根边界和受限读取接口负责。
- snapshot 生命周期由共享指针管理，旧异步任务只能发布带 generation/source stamp 的结果；当前索引仍是全量扫描，尚未宣称增量扫描或稳定资源 ID。

未完成风险/下一步：

- 尚未接入增量扫描、文件指纹、导入设置、缩略图缓存和 `AssetSystem` manifest/loader/processor。
- 真实图片、音频、视频和模型 provider 尚未完成；下一入口优先选择一个有现成引擎连接点的 provider，并同时建立其失败、取消和资源上限测试。

## 第 4.1 子阶段：音频预览与 AudioSystem 连接

状态：已完成（安全试听 transport 首个垂直切片；解码元数据、波形、定位和其他媒体 provider 仍未完成）。

已实现：

- 音频资源被选中后，Media Preview retained 面板展示路径、可用性、连接状态和播放状态；未连接 AudioSystem 时明确报告不可用。
- Play/Pause/Stop/Loop/Volume 命令通过 `EditorCommand` 进入 `EditorLayer`，播放实际调用现有 `AudioSystem::play`，暂停/继续/停止和音量调用对应 voice API。
- Engine 在编辑器初始化后注入 `AudioSystem`，关闭顺序调整为先收束编辑器试听 voice，再关闭音频后端；项目根切换、布局重载和资源切换都会停止旧 voice。
- 音频播放使用 `FileSystemService::resolve_existing` 得到项目根内 canonical 文件路径，AudioSystem 以 streaming 参数加载，不把用户路径拼接成 shell 命令。

验证证据：

- `EditorAudioPreviewTests` 使用 fake backend 验证 editor→AudioSystem 的 Play/Pause/Stop 状态闭环和可用资源状态。
- `EditorInteractionTests` 验证无 AudioSystem 注入时音频资源的诚实失败状态与 retained transport 控件注册。
- `FileSystemTests` 验证 `resolve_existing` 对项目内文件和项目根外路径的边界；`EditorUiModelTests` 验证媒体快照 revision 去重。
- 本轮目标测试 2/2 通过；最终全量构建和 CTest 45/45 作为收尾门槛，GPU 像素证据仍沿用上一轮声明，不把 headless 命令证据当作 GPU 截图。

安全/性能审计：

- UI paint 只消费 `EditorMediaUiState`；文件存在性检查和 AudioSystem 调用发生在命令/帧同步路径，不在 retained 绘制函数中解码或打开文件。
- 试听句柄由 AudioSystem 管理；编辑器切换资源、项目根和 shutdown 都清理 voice，fake backend 覆盖异常生命周期的核心状态转换。
- 当前不读取完整 PCM、不生成波形、不承诺 OGG/MP3/FLAC 解码成功；下一轮必须为元数据/波形增加大小、时长、取消和损坏输入上限。

未完成风险/下一步：

- Media Preview 目前只把 transport 连接到 AudioSystem；时长仍未知，Timeline/seek 尚未绑定真实音频数据。
- 图片已完成 Windows WIC→retained D2D 缩略图首个垂直切片；仍需补齐色彩空间/棋盘格/缓存策略和跨后端 GPU 上传。
- 视频尚无首帧解码，模型尚无隔离 preview scene；第 2C 的诊断过滤、profile 持久化和 IDE launcher 仍需继续推进。

## 第 4.2 子阶段：Windows 图片缩略图与 retained UI 位图桥

状态：已完成（PNG/WIC 首个受限 provider；更多格式、导入缓存和跨后端上传仍未完成）。

已实现：

- `EditorImagePreview` 在后台线程通过 Windows Imaging Component 解码首帧，限制源文件 64 MiB、源尺寸 16384、缩略图最长边 512 和 512×512 像素预算。
- 解码结果以 `shared_ptr<const ui::UiImageSnapshot>` 发布，包含 source stamp/generation、源尺寸和 premultiplied BGRA8 像素；选择切换、项目根变更和失效索引会淘汰旧结果。
- Inspector 只消费 immutable snapshot；图片可用时提交 retained `DrawCommandType::Image`，加载中、provider 不可用和损坏输入均显示明确状态。
- Windows D3D11 retained 后端把快照转换为有界 Direct2D bitmap cache，复用同一 UI surface；没有快照时保留稳定占位框，不伪装成已解码图片。
- 新增 provider 单元测试和编辑器交互测试；CMake 仅在 Windows 链接 `windowscodecs`，非 Windows 返回显式 provider unavailable。

验证证据：

- `shinkou_editor_image_preview_tests` 使用真实 1×1 PNG fixture 验证 WIC 解码、尺寸、像素快照和项目根外拒绝；无 provider 平台验证明确失败而非伪成功。
- `EditorInteractionTests` 验证图片选择、异步状态收敛、Inspector retained image command，以及 provider 缺失时的诚实错误状态。
- 受影响目标构建和目标测试 2/2 通过；全量构建、CTest 46/46 和 `git diff --check` 均通过，本轮审计关闭。

安全/性能审计：

- worker 只读取 `FileSystemService::resolve_existing` 返回的项目根内 canonical 路径；paint 不访问磁盘、不调用 WIC、不等待 future。
- 解码维度、源文件大小、输出像素和后端 bitmap cache 均有硬上限；过期 generation/source stamp 结果不能覆盖当前选择。
- WIC COM 生命周期严格绑定 worker 线程；D2D cache 在 UI target 重建时释放，选择大量图片不会无限增长。

未完成风险/下一步：

- 当前以 WIC 解码为 Windows provider，尚未报告 EXIF 方向、色彩空间、动画帧，也未完成 JPEG/WebP/TGA 的逐格式验收矩阵。
- D3D11 走 retained D2D bitmap；Vulkan/D3D12/Null 仍依赖能力降级，尚未实现统一 backend GPU texture upload contract。
- 第 4 轮下一入口是音频元数据/波形与取消边界，随后实现视频首帧和模型隔离 preview scene；第 2C/3 轮的 IDE launcher、增量索引和资源缓存并行保留。

## 第 4.3 子阶段：音频元数据、波形快照与定位

状态：已完成（WAV/Miniaudio 首个受限 provider；格式矩阵、缓存和视频/模型 provider 仍未完成）。

已实现：

- 新增 `EditorAudioPreview` provider，在后台通过现有 Miniaudio 解码器读取项目根内音频，发布不可变的通道数、采样率、总帧数、时长和有界峰值数组。
- provider 不物化完整 PCM：峰值按最多 512 个 bin 采样，每个 bin 最多读取 4096 帧；源文件限制 256 MiB、通道限制 32，未知/损坏/越界输入返回可读错误。
- 音频 preview future 带 generation、source stamp 和共享取消标记；资源切换、项目根/布局变更和 shutdown 会取消或收束旧任务，过期结果不能回写当前资源。
- Media Preview retained state 增加 waveform/loading/status 快照；波形点击以 0..1 归一化坐标发送 `MediaSeek`，再映射到真实时长，播放 transport 仍由 `AudioSystem` 执行。
- 修复文本异步预览与图片请求交错时的 loading 状态回退；项目根、布局重载、资源失效和资源类型切换统一清理旧媒体描述与 transport 状态。

验证证据：

- `shinkou_editor_audio_preview_tests` 使用真实 8 kHz PCM WAV fixture 验证元数据、时长、峰值、项目根外拒绝、畸形输入、取消语义和 AudioSystem transport/seek 集成。
- `EditorInteractionTests` 验证音频资源无 AudioSystem 时的诚实不可用状态、媒体控件注册，以及音频切换到图片时异步预览状态最终收敛。
- 本轮受影响目标构建通过；目标 CTest `shinkou_editor_audio_preview_tests` 与 `shinkou_editor_interaction_tests` 均通过。随后完成全量构建、CTest 46/46 通过和 `git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- worker 只通过 `FileSystemService::resolve_existing` 取得项目根内路径；没有 shell、外部命令或 paint 路径文件 IO。
- 解码器、源文件、通道数、峰值数组和每 bin 读取均有硬上限；取消检查位于 decoder 初始化前及每个采样 bin，避免切换资源后继续无界工作。
- retained UI 只消费 `shared_ptr<const EditorAudioPreviewSnapshot>`；波形绘制按面板宽度限制竖线数量，未把完整采样数据复制到 UI 模型。

未完成风险/下一步：

- OGG/MP3/FLAC 等格式是否可解码取决于当前 Miniaudio 构建配置，尚未建立逐格式 fixture 矩阵；重复 seek 对超长压缩音频的成本仍需 profiling 和缓存策略。
- 后续第 4.10 已把音频 seek 同步到 AudioSystem voice；本历史切片本身仍只记录 waveform provider，视频音轨共用 transport 仍未实现。
- 视频首帧/时间轴、模型隔离 preview scene、音频波形持久缓存、增量资源索引和外部 IDE launcher 仍是后续入口。

## 第 4.4 子阶段：Windows 视频首帧与基础元数据

状态：已完成（Windows Media Foundation 首帧/元数据首个垂直切片；连续播放与跨格式矩阵仍未完成）。

已实现：

- 新增 `EditorVideoPreview` provider，在 Windows worker 线程通过 Media Foundation Source Reader 读取项目根内视频的尺寸、时长、帧率和音频流数量，并请求 RGB32 首帧。
- 首帧按最长边 512、源尺寸 4096 和 512×512 RGBA 像素预算约束，转换成不可变的 `UiImageSnapshot`；视频结果携带 generation/source stamp，旧选择的 decoder 结果不会写回当前资源。
- Media Preview 现在区分 Audio/Video 状态：视频资源自动激活媒体页，加载中/成功/解码失败均有状态；成功时 retained 面板显示首帧，视频时间轴命令可复用已有归一化 seek 状态模型。
- 资源切换、项目根/布局变更和 shutdown 会取消并收束视频 future；非 Windows 构建返回明确 provider unavailable，不伪装成视频帧。

验证证据：

- `shinkou_editor_video_preview_tests` 运行时生成最小无压缩 AVI fixture，验证 Media Foundation 真实解码、2×2 首帧、时长/帧率、取消、畸形视频和项目根外路径拒绝。
- `EditorInteractionTests` 验证损坏视频进入 Video Media Preview 并报告 provider unavailable，而不是停留在未注册的元数据占位；`EditorUiModelTests` 继续验证 video snapshot 参与 retained revision 去重。
- 本轮目标构建和目标测试已通过；随后全量构建通过、CTest 47/47 通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- worker 只将 `FileSystemService::resolve_existing` 返回的 canonical 路径交给 Media Foundation，不调用 shell、不让 retained paint 线程持有 COM/decoder 句柄。
- 文件大小 512 MiB、源尺寸 4096、输出帧 512×512 和最多 32 个音频流探测均有上限；首帧读取前后检查取消标志，COM/MF shutdown 与资源释放顺序固定。
- 当前只提交一帧快照和标量元数据，不缓存完整视频帧或把 decoder 对象放进 UI model；D3D11 后端复用已有 BGRA retained image seam。

未完成风险/下一步：

- 当前 Media Foundation 切片只显示首帧；尚未实现连续时间轴解码、视频播放/暂停、音视频同步和真实 decoder seek。
- Windows codec availability 由系统 Media Foundation 组件决定；MP4/MOV/MKV/WebM 等格式尚未建立逐格式 fixture/硬件解码矩阵，FFmpeg provider 仍未接入。
- 模型隔离 preview scene、材质/网格统计、资源导入缓存、增量索引和外部 IDE launcher 仍未完成。

## 第 4.5 子阶段：OBJ 模型线框与网格统计

状态：已完成（OBJ 首个受限 provider；GLTF/GLB、材质和真正隔离 3D 场景仍未完成）。

已实现：

- 新增 `EditorModelPreview` provider，在后台解析项目根内 OBJ 的顶点、面索引和对象/组标记；支持正索引、负索引和多边形扇形三角化。
- provider 发布不可变顶点数、三角形数、对象数、包围盒和归一化线框段；源文件 128 MiB、顶点 500000、三角形 1000000、行长 4096 和线框段 8192 均有上限。
- Inspector 在模型资源加载完成后绘制线框投影并显示顶点/三角形统计；解析错误、越界索引、取消和空几何均显示失败状态，不把模型伪装成已导入场景。
- generation/source stamp、取消标志、项目根/布局/资源切换和 shutdown 生命周期与其他 provider 一致，模型快照参与 asset preview revision 去重。

验证证据：

- `shinkou_editor_model_preview_tests` 使用运行时 OBJ fixture 验证顶点/三角形/对象/包围盒/线框、取消、畸形顶点和项目根外拒绝。
- `EditorInteractionTests` 验证模型资源在编辑器内最终产生有效线框快照；`EditorUiModelTests` 继续验证模型快照参与 retained 状态去重。
- 本轮目标构建和目标测试已通过；随后全量构建通过、CTest 48/48 通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- provider 只读取 `FileSystemService::resolve_existing` 返回的项目内文件，不调用外部解析器，不在 paint 路径读取文本或构建几何。
- 解析采用固定容量上限和有界 face token 数量；负索引先检查最小整数边界，非法/越界索引不会进入顶点数组。
- retained UI 只消费 shared immutable snapshot；线框投影最多提交 8192 条线段，避免恶意模型把绘制命令无限放大。

未完成风险/下一步：

- 当前只支持 OBJ 线框投影，不支持材质、纹理、法线、骨骼、动画、GLTF/GLB 或真正的隔离 3D preview scene/camera orbit。
- 线框是展示快照，不会自动创建场景实体，也没有连接 `AssetSystem` importer/manifest 和 GPU mesh cache。
- 下一模型入口是 GLTF/GLB 结构化解析与隔离 preview scene；同时继续补视频连续时间轴、增量资源缓存和外部 IDE launcher。

## 第 4.6 子阶段：结构化网格与隔离模型预览场景

状态：已完成（OBJ geometry + renderer-neutral preview scene 首个垂直切片；GLTF/GLB、材质和 GPU preview target 仍未完成）。

范围与非目标：

- 本轮只把 OBJ 的顶点/三角形索引发布为 immutable geometry，并建立不依赖活动 `World` 的 preview scene/camera seam。
- 本轮不创建实体、不写入活动场景、不接入 GPU mesh/cache、不声称已有真实 3D render target。

代码变更：

- `EditorModelPreviewSnapshot` 增加结构化顶点和索引，仍保留有界线框快照用于 retained UI 降级路径。
- 新增 `EditorModelPreviewScene`，包含独立 yaw/pitch/distance 相机、投影 revision、orbit/zoom/reset 和最多 8192 条投影线段。
- `EditorUi` 为模型预览注册中键拖拽、滚轮缩放和 Reset 区域；`EditorLayer` 只更新 scene state，再把 immutable retained state 发布给 `EditorUiModel`。
- 预览场景没有 `World*`、Entity、GPU handle、文件句柄或 decoder 成员，资源预览与活动场景生命周期隔离。

契约与集成测试：

- `shinkou_editor_model_preview_tests` 验证 OBJ 结构化 geometry、统计、取消、畸形输入和项目根外拒绝。
- 新增 `shinkou_editor_model_preview_scene_tests` 验证 geometry 不变、camera orbit/zoom/reset、投影 revision 和 clear 生命周期。
- `EditorInteractionTests` 验证模型预览中键 orbit 会更新隔离相机、Reset 恢复默认相机，同时活动 `World` 对象数保持不变。

安全/性能审计：

- 结构化数组沿用 provider 的 128 MiB、500000 顶点、1000000 三角形和 8192 线段上限；snapshot validity 检查避免三角形索引长度乘法溢出。
- paint 路径只读取 shared immutable scene state，不访问磁盘、不解析 OBJ、不执行外部进程；投影输出有固定段数上限。
- orbit/zoom 只更新相机标量并生成新的有界投影快照；活动 `World` 不参与资源预览数据流。

视觉/交互审计：

- Inspector 模型区域保留浅色/深色线框配色；模型加载失败仍显示可读状态而不提供伪造 Reset 成功态。
- 中键拖拽和滚轮路由到模型区域，不复用活动场景 viewport 的相机；Reset 为独立可聚焦区域。
- 本轮完成 headless interaction coverage；真实窗口/DPI/多后端截图矩阵仍属于第 8 轮视觉验收。

验证证据：

- 受影响目标构建通过，三个模型/UI 目标测试通过；全量构建通过，CTest `49/49` 通过，`git diff --check` 通过，本轮审计关闭。

未完成风险/下一步：

- 当前投影仍是 retained 线框，不支持背面剔除、材质、纹理、法线、骨骼、动画、GLTF/GLB 或真实 GPU 预览目标。
- 下一模型入口是有界 GLB/GLTF container + JSON/accessor metadata provider，再接材质和 renderer-owned preview target；视频连续时间轴、增量资源缓存和外部 IDE launcher 继续按计划排队。

## 第 4.7 子阶段：GLTF/GLB 基础几何 provider

状态：已完成（GLTF/GLB 基础 geometry 首个垂直切片；节点变换、材质/纹理、动画和 GPU preview target 仍未完成）。

范围与非目标：

- 本轮支持 `.gltf` 项目内相对 `.bin`、base64 buffer，以及 `.glb` JSON + 单 BIN chunk；只接受 TRIANGLES、POSITION `FLOAT/VEC3` 和 8/16/32 位 scalar indices。
- 本轮不宣称完整 glTF 导入：节点层级/变换、法线/切线、稀疏 accessor、skins/animations、材质/纹理、非 TRIANGLES mode、GPU mesh cache 和活动场景实例化均未实现。

代码变更：

- 新增 `EditorGltfPreview` provider 和受限 JSON/GLB reader，结果统一进入 `EditorModelPreviewSnapshot` 的 immutable vertices/indices/bounds/wireframe。
- `EditorModelPreview` 根据扩展名分发 OBJ、GLTF 和 GLB；Inspector 继续使用同一个隔离 `EditorModelPreviewScene`，并显示来源格式、顶点/三角形/网格统计。
- CMake 新增 `shinkou_editor_gltf_preview_tests`，provider 不引入第三方 JSON/模型依赖，也不调用外部命令。

契约与集成测试：

- `.gltf + triangle.bin` fixture 验证外部项目内 buffer、POSITION accessor、16 位索引和材质/网格统计。
- `.glb` fixture 验证 JSON chunk、内嵌 BIN chunk、容器长度、首帧取消、损坏容器和项目根外路径。
- OBJ、model preview scene 和 EditorInteraction 既有回归继续通过，确保扩展名分发不改变已有模型/交互行为。

安全/性能审计：

- JSON 最大 128 MiB、深度 32、对象成员 4096、数组 500000、字符串 1 MiB；源文件/单 buffer/aggregate buffers 均受 128 MiB 上限约束。
- 每个 bufferView/accessor 都检查整数溢出、buffer 边界和 view 内边界；外部 URI 必须经过 `FileSystemService::resolve_existing`，拒绝绝对路径、URI scheme 和项目外路径。
- worker 执行解析和文件 IO，paint 只消费 immutable snapshot；geometry/indices/线框都有固定数量上限，取消检查位于 buffer、vertex、index 和线框循环。

验证证据：

- 目标构建通过；GLTF、OBJ、隔离 preview scene 和编辑器交互四项测试通过。
- 全量构建通过，CTest `50/50` 通过，`git diff --check` 通过；本轮审计关闭。下一步是更新 AssetSystem importer/manifest 接口或进入材质/纹理 accessor 轮次。

未完成风险/下一步：

- 当前 GLTF/GLB 几何只保留 mesh-local positions/indices，忽略 node transform；材质计数仅为元数据，尚未加载材质参数或纹理。
- `AssetSystem` 目前仍把 gltf/glb 登记为 raw 类型，尚未将 provider 作为正式 processor/loader/cache artifact；这是第 3 轮资源连接的下一入口。

## 第 3.2 子阶段：选中资源 AssetSystem 桥接

状态：已完成（raw resource contract 首个垂直切片；正式 importer/manifest、派生缓存和拖入场景仍未实现）。

范围与实现：

- `EditorLayer` 对当前选中、已索引的项目资源发起一次空类型 `AssetKey` 请求，由 `AssetSystem` 按扩展名 canonicalize；结果只发布 loading/ready/error、format 和 source hash。
- 现有文本/图片/音频/视频/模型 provider 保持自己的 bounded IO/decoder 和 immutable snapshot；AssetSystem bridge 不把 raw bytes 复制进 retained UI，也不改变 provider 成功条件。
- `Engine` 在 editor 模式下使用 `editorProjectRoot` 作为默认 AssetSystem root，显式 `EngineConfig.assets.projectRoot` 优先；shutdown 改为先 `EditorLayer::shutdown()`，再 `AssetSystem::shutdown()`，覆盖 preview future 的生命周期。
- Inspector status line 在 bridge 已连接时显示 `AssetSystem ready/loading/failed` 及 canonical format，未连接、未索引和项目切换状态保持可读。

验证证据：

- `EditorInteractionTests` 创建真实临时 project + `AssetSystem`，验证选中文本同时产出文本行、`assetSystemStatus == "AssetSystem ready"`、`format == "raw"` 和非零 source hash；测试结束按 editor→AssetSystem 顺序关闭。
- 目标构建 `shinkou_engine` + `shinkou_editor_interaction_tests` 通过；目标 CTest：UI model 与 interaction `2/2` 通过。
- 随后执行全量构建、全量 CTest 和 `git diff --check`；本轮最终证据写入完成后关闭。

安全/性能审计：

- 资源读取继续走 AssetSystem 的项目 root/mount 解析、source-size 上限和内置 raw loader；没有 shell 拼接、外部进程或 paint-thread 同步 IO。
- 每个选中资源仅维护一个共享 future；切换资源丢弃旧观察句柄但不绕过 AssetSystem worker 回收，shutdown 先等待当前观察 future，避免系统销毁后回调访问悬空对象。
- bridge 只复制标量状态和字符串格式，不复制 payload；代价是当前 provider 与 raw bridge 仍会对同一源文件各读一次，属于已记录的后续优化点。

失败/回滚路径：

- AssetSystem 未连接或未初始化：显示明确 unavailable 状态，现有 provider 仍可继续工作。
- 资源未进入编辑器索引：显示 waiting/not indexed，不发起越界路径请求。
- raw load 失败：显示错误并保留 provider 独立错误；移除 `set_asset_system` 注入即可回到原有 provider-only 行为。

未完成风险/下一步：

- glTF、图片、音频等仍未作为结构化 AssetSystem processor/loader artifact；暂时存在 provider IO 与 raw cache 的重复读取。
- manifest 增量写回、稳定资源 ID、依赖失效传播、缩略图派生缓存、材质/纹理 accessor 和资源拖入场景仍待后续轮次。
- 下一入口：优先选择材质/纹理 accessor 的结构化 provider，或独立实现 bounded video timeline decoder session，再回到正式 importer/manifest 对接。

## 第 3.3 子阶段：编辑器 AssetSystem manifest immutable snapshot

状态：已完成（异步 manifest 枚举与编辑器状态连接；正式 manifest 写回、稳定 ID 和 importer artifact 未完成）。

范围与实现：

- 编辑器初始化、显式刷新和项目根变化请求 `AssetSystem::scan_sources()`，worker 线程生成 `AssetManifestEntry` immutable snapshot，主线程只发布资源数量和状态。
- 状态栏/控制器可观察 scanning、unavailable、failed 和 ready；选中资源 raw bridge 与 manifest snapshot 可同时工作。
- manifest 自己维护 dirty 状态，与文件树的异步扫描状态解耦；文件树等待期间不会重复启动 manifest worker。
- 本阶段不在每帧扫描、不在 paint 路径 hash 文件、不默认写入 manifest 文件；正式增量 manifest、稳定 AssetId、派生缓存和 importer/loader artifact 排入后续轮次。

验证证据：

- `EditorInteractionTests` 使用真实临时项目和 `AssetSystem`，验证 manifest count 大于零且状态包含 `AssetSystem manifest ready`，并与选中资源 raw bridge 同时通过。
- 目标构建及 GLTF/interaction 回归通过；全量构建通过，CTest `50/50` 通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- 扫描只处理 AssetSystem 已登记扩展名和 mount，完全在 worker future 中运行；generation 过期结果不会覆盖新项目状态。
- manifest dirty 状态只在初始化、显式刷新、项目根变化、重命名、新建目录和删除后置位；扫描完成后保持 ready，不会随 `assetsDirty_` 的文件树刷新循环重复 hash。
- AssetSystem pointer 更换前、EditorLayer shutdown 时等待 manifest future，确保 worker 不访问悬空系统；失败只保留可读状态，不影响 provider 预览。
- 当前 `scan_sources()` 会读取并 hash 每个登记文件，适合显式/初始化扫描但还不是增量索引；下一轮必须引入指纹复用和变更队列，避免大工程重复 IO。

失败/回滚路径：

- AssetSystem 未连接/未初始化时显示明确状态；文件扫描失败不会阻塞文本、图片、音频、视频或模型 provider。
- 移除 `set_asset_system` 或 manifest future 即回退到原有 FileSystemService 资产浏览，不改变 retained UI 数据结构。

未完成风险/下一步：

- 当前 snapshot 只有会话级条目，尚未写回 manifest、分配稳定资源 ID、记录依赖失效或复用 source hash。
- 下一步把 manifest 条目接入增量索引和正式 importer/loader artifact，再将 GLTF image metadata 与图片 provider/派生缓存连接。

## 第 3.4 子阶段：manifest 稳定 AssetId

状态：已完成（稳定身份首个垂直切片；manifest 读回、增量失效和正式 importer artifact 未完成）。

范围与实现：

- `AssetManifestEntry` 新增 `id`，由规范化 `AssetKey { uri, type }` 计算，与 `AssetLoadResult::id` 使用同一 `AssetSystem::make_id` 规则。
- `scan_sources()` 返回稳定 ID；`write_manifest()` 将 ID 写入每条 JSON 记录；ID 不依赖物理路径、文件时间或本次扫描排序。
- 编辑器无需复制额外 payload 即可通过 immutable manifest 保留稳定资源身份，为后续增量索引、派生缓存和拖入场景提供契约。

契约与测试：

- `shinkou_assets_tests` 扫描真实临时 mount，验证 `project://hello.txt` 的 manifest ID 与实际加载结果 ID 一致，并验证 manifest 文件包含 `id` 字段。
- 受影响 assets/engine 目标构建通过；本轮最终全量构建、CTest 和 `git diff --check` 作为关闭门槛。

安全/性能审计：

- ID 输入是已规范化的虚拟 URI 与 canonical type，不使用用户提供的物理路径拼接，不新增文件读取或外部进程。
- 计算为常数空间 FNV-1a 哈希；manifest worker 仍承担文件内容 hash，ID 本身不会增加 paint 路径工作。
- 哈希碰撞仍由当前 AssetSystem key/index 契约承担；尚未引入持久化冲突表，正式生产 manifest 需要在下一阶段补充冲突诊断与读回校验。

失败/回滚路径：

- 旧 manifest 没有 `id` 字段时不影响当前 runtime `request()`；重新扫描即可生成新字段。
- 若后续读取发现 ID 与规范化 key 不匹配，应拒绝该 manifest 条目并回退到实时扫描，不覆盖当前 immutable snapshot。

未完成风险/下一步：

- 当前只写出 ID，不读取/合并 manifest，也没有重命名历史、删除墓碑、碰撞诊断或正式 importer/loader artifact。
- 下一步将稳定 ID 绑定到文件变更队列、派生缓存和正式 importer/loader artifact，再连接 GLTF image/图片 provider。

## 第 3.5 子阶段：manifest 指纹增量缓存

状态：已完成（按源指纹复用 manifest entry；文件事件扫描、manifest 读回和正式 importer artifact 未完成）。

范围与实现：

- `AssetSystem::scan_sources()` 为每个规范化 `AssetKey` 缓存 source path、timestamp、size、hash 和 stable ID；下次扫描若三项物理指纹未变化则复用完整 entry。
- 新增 `AssetManifestScanStats` 和 `last_manifest_scan_stats()`，让测试和后续 editor profiler 能观察 entries、cacheHits、cacheMisses；开启 `verifyCacheByContentHash` 时跳过复用并重读内容。
- 扫描结束按当前发现的 key 清理缓存中的删除/取消登记条目；manifest 输出格式和 immutable editor snapshot 保持兼容。

契约与测试：

- `shinkou_assets_tests` 首次扫描验证两个资源产生 miss，第二次扫描验证全部 hit；修改 `shared.bin` 后验证只重算变更条目、未变化文本条目仍命中，并检查新 source size。
- stable AssetId 与 manifest JSON 字段测试继续通过；目标 assets 测试通过，全量构建通过，CTest `50/50` 通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- cache key 是规范化虚拟 `AssetKey`，不会把物理路径或用户输入拼接为命令；扫描仍受原有 mount、扩展名和单文件 `MaxAssetBytes` 限制。
- cache 只在串行 manifest scan 临界区读写，避免两个扫描 worker 交叉覆盖；UI/paint 不访问缓存、不读取文件、不等待锁。
- timestamp/size 模式是性能优化而非篡改检测；启用 `verifyCacheByContentHash` 可恢复每次内容校验，后续仍需事件/哈希一致性策略覆盖极端时间戳伪装。

失败/回滚路径：

- 无法读取变化文件时该条目被跳过并保留可读扫描结果，不用旧 entry 冒充新内容；下一次扫描仍会 miss 重试。
- 删除或取消登记的 key 在扫描结束清除；旧 manifest JSON 没有缓存统计字段也不影响读取前的实时扫描路径。

未完成风险/下一步：

- 当前仍需递归遍历目录，尚未接入 FileSystem watcher 事件队列、manifest 读回/校验、重命名迁移和依赖失效传播。
- 下一步将 stable ID、增量 manifest 与正式 importer/loader artifact 绑定，再复用同一缓存为 GLTF image、图片缩略图和材质派生资源服务。

## 第 3.6 子阶段：manifest 受限读回/校验

状态：已完成（校验 API 首个垂直切片；编辑器 seed、manifest 迁移和正式 importer artifact 未完成）。

范围与实现：

- 新增 `AssetSystem::read_manifest()`，对 manifest 文件设 64 MiB 上限，JSON parser 限制深度、成员数、数组项和字符串大小，并拒绝尾随数据、重复 JSON key 和非法数字。
- 读回验证 version 1、canonical virtual URI、非空 type/source、整数 hash/timestamp/size、重复 key/ID，以及 stable `AssetId` 与 `AssetKey` 的一致性。
- source path 只被保存为 manifest 描述，不参与文件打开、mount 授权或命令执行；失败时返回完整错误和空的有效结果，不覆盖任何现有 snapshot。

契约与测试：

- `shinkou_assets_tests` 写出 manifest 后读回并验证两条资源、ID 和 key；构造 ID 不匹配的 manifest，验证 reader 拒绝并报告可读错误。
- 目标 assets 测试通过；全量构建通过，CTest `50/50` 通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- reader 不调用网络、shell 或外部 parser；只读取调用者指定的文件，编辑器接入时仍必须先通过 `FileSystemService` 项目根边界。
- 所有数字以无符号整数 token 解析，避免 double 精度损失破坏 64 位 AssetId；URI 规范化后重新计算 ID，防止伪造身份。
- parser 结果在校验完成前不会发布；数组、对象、字符串和文件总量上限抑制恶意 manifest 的内存放大。

失败/回滚路径：

- 旧版本缺失 `id`、非 canonical URI、重复身份、畸形 JSON 或超限文件均返回失败；调用方应回退到实时 `scan_sources()`，不能信任旧数据。
- 即使 manifest 中 source 路径指向项目外，reader 也不会访问该路径；后续 seed 必须以当前 mount 的扫描结果为准。

未完成风险/下一步：

- 当前 reader 尚未把有效条目 seed 回 manifest fingerprint cache，也没有编辑器内的 manifest 状态/迁移命令。
- 下一步将通过当前 AssetSystem mount 和 source fingerprint 对读回条目做 revalidation，再安全地供 EditorLayer 增量扫描使用。

## 第 3.7 子阶段：编辑器 manifest seed 连接

状态：已完成（校验 manifest 到编辑器实时扫描的 seed 连接；自动写回、重命名迁移和正式 importer artifact 未完成）。

范围与实现：

- `EditorLayer::request_asset_manifest_scan()` 在 worker 中检查项目内 `.shinkou/manifest.json`；成功读回后调用 `AssetSystem::seed_manifest_cache()`，再执行当前 mount 的 `scan_sources()`。
- seed 只进入 AssetSystem 的 bounded fingerprint cache；实时扫描仍比较当前 source path、timestamp、size，并清理删除/未登记 key，因此旧 manifest 不能直接替代当前文件系统事实。
- manifest 状态 ready 文本追加 `(validated cache)`；读回失败则追加 `(readback fallback: ...)`，但不阻塞有效的实时扫描结果。

契约与集成测试：

- `EditorInteractionTests` 在临时项目预先写出 `.shinkou/manifest.json`，验证编辑器 manifest ready 状态包含 `validated cache`，同时保留原有资源计数、typed bridge 和连续 ready 稳定性检查。
- `shinkou_assets_tests` 验证 `seed_manifest_cache()` 后下一次扫描命中全部条目；目标 interaction/assets 测试通过，全量构建通过，CTest `50/50` 通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- editor 只构造项目根下固定的 `.shinkou/manifest.json` 路径；readback parser 和 seed 都不执行 source path，不调用 shell/网络。
- seed 发生在 AssetSystem worker 内，完成后仍由当前 mount 扫描做 source fingerprint revalidation；paint、input 和 retained render list 不持有 manifest 文件句柄。
- manifest 不存在或损坏时只走 fallback 实时扫描；状态可见，旧条目不会直接发布为当前 snapshot。

未完成风险/下一步：

- 当前每次仍会递归遍历 mount，未接入 watcher 事件队列、manifest 原子写回、重命名迁移、依赖失效或 importer/loader artifact。
- 下一步进入正式增量导入：让 provider 结构化结果成为 AssetSystem artifact，并为派生缩略图/材质缓存建立 stable ID 失效传播。

## 第 3.8 子阶段：typed source artifact processor/loader

状态：已完成（类型化源工件首个垂直切片；结构化解码与 GPU 资源实例化仍未完成）。

范围与实现：

- `AssetSystem` 为 text、texture、model、audio、video、font、shader、material、scene 建立内置 processor/loader；扩展名 canonicalize 不再把这些资源全部归入 `raw`。
- processor 在已有源文件大小上限和 worker 读取边界内复制不可变源字节，并发布 canonical format；disk cache 继续使用 processor/loader version、source fingerprint 和依赖指纹做失效判断。
- editor selected-resource bridge 只发布 typed format、source hash 和状态，不把源字节带入 retained paint；应用仍可通过 `register_processor`/`register_loader` 覆盖任意类型。

契约与集成测试：

- `shinkou_assets_tests` 使用 `.png` fixture 验证默认扩展映射为 `texture`，并保留原始 payload；已有自定义 `text` processor 覆盖路径继续通过。
- `EditorInteractionTests` 验证选中的 `.txt` 资源发布 `text` format，同时图片、音频、视频、OBJ/GLTF preview provider 仍走各自 bounded snapshot 路径。
- 定向构建、全量构建、CTest `50/50` 和 `git diff --check` 均通过，本轮审计关闭。

安全/性能审计：

- typed processor 不执行解码、网络访问、外部进程或 GPU 创建；源读取复用 AssetSystem 的 `MaxAssetBytes`，cache payload 继续有界。
- loader 只发布不可变字节和 canonical format；没有把源 bytes 冒充已解码纹理、可播放音频、视频帧或 mesh。
- paint/input 不触碰文件句柄和 processor；自定义 importer 必须通过同一 processor/loader 版本和依赖失效契约接入。

失败/回滚路径：

- 找不到 source、processor 或 loader 时返回现有 AssetLoadResult failure，旧的 ready 数据保持一致的 stale/failed 语义。
- 删除 typed registration 或注册自定义 processor 会回退到应用显式提供的类型实现；未注册类型仍按原有 raw/无 processor 规则处理。

未完成风险/下一步：

- 当前 typed artifact 还不是结构化 GLTF/image/audio/video artifact；材质采样、GPU texture、连续 decoder session、派生缩略图缓存和 importer 依赖提取仍待实现。
- 下一轮应把一个真实 provider 的受限结构化结果序列化为版本化 artifact，并让 editor 使用同一 AssetId/manifest 失效传播，而不是再建立第二套缓存。

## 第 3.9 子阶段：typed artifact bounded descriptor

状态：已完成（源描述块首个垂直切片；正式 provider importer 与 GPU 资源仍未完成）。

范围与实现：

- `AssetArtifact`/`AssetData` 增加可选 `metadataFormat` 与 immutable `metadata` 字符串，磁盘 cache version 从 5 提升到 6，描述块和 payload 使用同一 source/processor/loader/依赖失效契约。
- 内置 typed processor 发布 bounded descriptors：text 行数；PNG/JPEG 宽高、位深/组件；WAV channels/sample rate/bits/data bytes；OBJ vertices/faces。未识别格式保留通用 source byte descriptor。
- `EditorLayer` 将 descriptor schema/size 转成 presentation-only `EditorAssetPreviewUiState`，Inspector 只显示 schema 与字节数，不在 paint 读取文件或解析 descriptor。

契约与集成测试：

- `shinkou_assets_tests` 验证 PNG、WAV、OBJ descriptors、typed format 和 cache round-trip；损坏 cache 仍从源安全恢复。
- `EditorInteractionTests` 验证选中文本资源能发布 `shinkou.asset.text.v1` 与非空 descriptor，并继续通过 manifest seed、图像/音频/视频/模型 preview 回归。
- 定向构建和 direct tests 通过；随后应重新执行全量构建、CTest `50/50` 与 `git diff --check` 作为本轮最终门禁。

安全/性能审计：

- 所有 descriptor parser 只读已加载 source bytes，复用 AssetSystem source 上限；PNG/JPEG/WAV/OBJ 读取均做 offset/长度检查，OBJ 统计另有 8 MiB 扫描上限。
- descriptor 最大写入/读回上限为 1 MiB；metadata 只作为 opaque cache data，不执行 JSON、脚本、网络或外部进程。
- renderer、audio backend、Media Foundation、WIC 和 active World 不被 AssetSystem processor 触碰；编辑器仍只消费 immutable state。

失败/回滚路径：

- 结构化识别失败时回退到 `shinkou.asset.source.v1`，typed payload 仍可加载；越界/损坏 cache 直接 miss 并重新处理 source。
- 旧 cache version 不匹配时按正常 cache miss 重新生成，不影响当前 ready/stale/error 状态语义。

未完成风险/下一步：

- descriptor 还没有统一 typed schema registry，也没有把 GLTF image/material、图片像素、音频 waveform、视频 frame 或模型几何写成正式 derived artifact。
- 下一入口是选择一个真实 provider（优先图片/OBJ 的 immutable snapshot）接入 versioned derived cache，并将 source rename/delete 传播到同一 AssetId 的依赖图；之后再进行 GPU material preview 与连续视频 decoder session。

## 第 3.10 子阶段：OBJ provider 消费 AssetSystem typed payload

状态：已完成（OBJ 真实 provider bridge 首个垂直切片；GLTF/image/audio/video 的正式 derived artifact 仍未完成）。

范围与实现：

- `load_editor_obj_preview_bytes()` 与原文件 provider 共享同一个 bounded OBJ stream parser，发布相同 immutable geometry/wireframe snapshot。
- `EditorLayer::request_model_preview()` 在 AssetSystem 已初始化、根目录稳定且资源扩展名为 `.obj` 时请求 `model` artifact；worker 等待 immutable bytes 后再解析。AssetSystem 不可用或资源不是 OBJ 时回到现有 FileSystem provider。
- `EditorInteractionTests` 注册自定义 model processor，故意返回不同尺寸的三角形；模型 Inspector 的 bounds 断言只能由 AssetSystem payload 路径满足，证明不是仅测试文件回读。

契约与集成测试：

- `EditorModelPreviewTests` 验证 byte-source seam 的几何、bounds 和取消契约；原有 project-safe file provider、损坏 OBJ 与路径越界检查继续通过。
- `EditorInteractionTests` 验证 AssetSystem model processor 的 bounds、模型隔离相机 orbit/reset 和活动 World 不变；目标构建与 direct tests 通过。
- 最终全量构建通过；CTest 使用单项 60 秒超时保护以 `50/50` 通过，`git diff --check` 通过；当前阶段不宣称 GPU mesh、GLTF image 或连续媒体导入完成。

安全/性能审计：

- AssetSystem future 和 OBJ parser 都在 worker 线程，paint/input 只读取 immutable snapshot；源 bytes 仍受 AssetSystem `MaxAssetBytes` 与模型顶点/三角形/行长度上限约束。
- `AssetSystem*` future 在 EditorLayer shutdown 前收束；旧 generation、source stamp 或 selection 不得覆盖当前模型 snapshot。
- 自定义 processor 只影响其明确注册的 `model` 类型；没有 AssetSystem、根目录切换或 GLTF/GLB 资源时不改变原有回退行为。

失败/回滚路径：

- typed future 失败、缺少 bytes 或 payload 不是可解析 OBJ 时显示现有 model preview error；移除 AssetSystem 连接即可回退到 FileSystem provider。
- GLTF/GLB 不走此路径，避免把 JSON/bin/image 复合资源错误地当作单一 OBJ payload。

未完成风险/下一步：

- 目前只有 OBJ 消费 AssetSystem typed payload；图片像素、GLTF buffer/image/material、音频 waveform/decoded stream、视频 frame/decoder session 还没有统一 derived artifact。
- 下一步应把 WIC 图片 snapshot 或 GLTF image metadata 接入同一版本化 derived cache，并在 renderer-owned preview target 中建立受控 GPU texture/material 生命周期。

## 第 3.11 子阶段：WIC image provider 消费 AssetSystem typed payload

状态：已完成（Windows WIC 内存字节 bridge 首个垂直切片；GPU texture/material 与 GLTF image artifact 仍未完成）。

范围与实现：

- `load_editor_image_preview_bytes()` 使用与文件 provider 相同的 source dimension、decoded pixel、thumbnail dimension 和 WIC conversion 限制；AssetSystem bytes 通过 bounded `IWICStream` 解码。
- `EditorLayer::request_image_preview()` 在 AssetSystem 已初始化且扩展名属于 texture 类型时请求 `texture` artifact；没有稳定 AssetSystem 时回退到现有 `FileSystemService` provider。
- D3D11 Direct2D bitmap cache、`UiImageSnapshot` 和 retained Inspector 绘制接口没有改变；非 Windows 保持明确 unavailable。

契约与集成测试：

- `EditorImagePreviewTests` 验证文件入口和 byte-source 入口的 generation/source stamp、1×1 PNG snapshot 或平台 unavailable 状态。
- `EditorInteractionTests` 注册自定义 texture processor，将项目中的非图片占位源替换为 bounded PNG payload；Windows 下图片预览因此只能通过 AssetSystem memory path 成功，模型/manifest/媒体回归继续通过。
- 定向构建和 direct image/interaction tests 通过；最终全量构建通过，CTest `50/50`（单项 60 秒超时保护）通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- WIC stream 只引用 worker 生命周期内的 immutable bytes；source file、AssetSystem future、COM/WIC decoder 都不进入 paint/input。
- bytes 上限 64 MiB、decoded dimension 16384、decoded pixel 256 MiB、thumbnail 512×512 和 output buffer 上限继续生效；WIC 句柄在成功/失败路径统一释放。
- AssetSystem 替换/EditorLayer shutdown 前等待 image/model/asset futures，旧 selection、generation 或 root 状态不能覆盖当前 image snapshot。

失败/回滚路径：

- AssetSystem future 失败、WIC 不支持格式或 stream 初始化失败时显示现有 unavailable/error 状态；移除 AssetSystem 连接回退到项目文件 provider。
- 非 Windows 不创建伪造像素；DDS/KTX 等未被系统 WIC 支持时保留明确失败，不把 descriptor 当作解码结果。

未完成风险/下一步：

- 图片 typed payload 尚未形成跨平台结构化 pixel artifact，GPU upload、颜色空间、压缩纹理矩阵和 GLTF image URI/bufferView 复用仍待实现。
- 下一入口是把音频 metadata/waveform 或 GLTF image/material 结构化结果接入同一 derived cache，并继续补 build profile 持久化与外部 IDE launcher。

## 第 4.8 子阶段：GLTF 材质/纹理元数据 provider

状态：已完成（metadata 首个垂直切片；纹理解码、采样和 GPU material preview 未宣称完成）。

范围与实现：

- `EditorModelPreviewSnapshot` 新增 immutable material/texture/image metadata：名称、PBR baseColorFactor、metallic/roughness、alphaMode、doubleSided、texture source/sampler 和 image URI/MIME/bufferView。
- GLTF/GLB provider 解析 `images`、`samplers`、`textures`、`materials`，并把合法引用绑定到快照；Inspector 模型统计显示 `mats` 与 `tex` 数量。
- image URI 只接受项目相对路径或 data URI；本轮不会打开图片、网络请求、创建 GPU 资源或修改活动 `World`。

契约与集成测试：

- `EditorGltfPreviewTests` 使用 GLTF/GLB triangle fixture 验证材质名称、baseColor texture、alphaMode、doubleSided、texture source、image MIME 及统计数量。
- 同一测试验证 `../outside.png` image URI 被拒绝；OBJ、model scene 和 EditorInteraction 回归继续通过。
- 本轮目标构建与 GLTF/OBJ/interaction 三项测试通过；全量构建通过，CTest `50/50` 通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- metadata arrays 限制为 4096；JSON 已有 128 MiB source、32 层深度、1 MiB string 和数组上限；PBR 数值必须 finite 且在 [0,1]。
- 所有 texture/image 引用经过表边界检查；URI scheme、绝对路径和项目外路径不会进入文件读取；provider 不调用网络或外部进程。
- metadata 只在 worker 线程解析并以 shared immutable vector 发布；retained paint 不解析 JSON、不读文件，统计渲染仍是常数级文本。

失败/回滚路径：

- 缺失 image/texture/sampler 引用、非法 alphaMode/PBR 数值和非项目 URI 返回明确 provider error，几何快照不会部分发布。
- 移除 metadata 字段或使用没有这些字段的旧 GLTF 仍得到空 metadata vectors；OBJ provider 行为不变。

未完成风险/下一步：

- image URI 尚未交给现有图片 provider 解码，`bufferView` image 尚未转换为图片快照；材质尚未创建 renderer resource 或参与 shading。
- 节点 transform、skins/animations、完整 sampler 参数、KHR 扩展和正式 AssetSystem importer/derived cache 仍待实现。
- 下一轮选择正式纹理解码/GPU material preview 或连续视频时间轴 decoder session，并继续保持 AssetSystem manifest 对接排在资源实例化之前。

## 第 4.9 子阶段：视频时间点帧预览

状态：已完成（Media Foundation bounded seek/frame 首个垂直切片；连续播放、音视频同步和 codec 矩阵仍未完成）。

范围与实现：

- `EditorVideoPreview` 增加按秒请求单帧的 `load_editor_video_frame`，复用受限源尺寸/文件/输出像素预算；快照发布 `frameTime`。
- `EditorLayer::MediaSeek` 对视频资源创建异步 frame future；拖动期间取消旧请求并只保留最新目标，generation/path/source stamp 不匹配的结果不会写回当前预览。
- Media Foundation reader、COM 初始化、sample/buffer 只存在 worker；retained media panel 继续只读取 immutable snapshot，首帧和 seek 帧使用同一状态通道。

契约与集成测试：

- `EditorVideoPreviewTests` 的双帧无压缩 AVI fixture 验证首帧 metadata 与 `load_editor_video_frame` 的 generation/source stamp/frame snapshot；非 Windows 明确报告 provider unavailable。
- `EditorInteractionTests`、GLTF/模型回归不受视频 future 改动影响；目标 video/interaction 测试通过。
- 最终全量构建通过，CTest `50/50` 通过，`git diff --check` 通过，本轮审计关闭。

安全/性能审计：

- seek seconds 按有限 duration clamp；源文件 512 MiB、尺寸 4096、输出 512×512 预算和取消检查保持不变。
- 旧 frame future 在 selection/root/shutdown 中取消并收束；EditorLayer shutdown 在 AssetSystem shutdown 前等待所有视频任务。
- 连续拖动不会在每个鼠标事件上并发创建无限 decoder；单个正在运行的 future 通过 pending target 合并后续请求。

失败/回滚路径：

- provider seek 失败显示 `Video seek unavailable`，保留最近一帧或首帧，不伪造成功时间。
- 非 Windows 或系统 codec 不可用时保留明确 unavailable；移除 seek future 即回退到首帧 preview。

未完成风险/下一步：

- 当前仅按需解码单帧，不支持连续播放、暂停、音视频同步、精确帧时间承诺和音频轨输出。
- MP4/MOV/MKV/WebM 的系统 codec/硬件解码矩阵、renderer-owned video texture upload 和正式 AssetSystem derived frame cache 仍未完成。
- 下一步建立 bounded decoder session，再把帧时间线与音频 transport/渲染后端能力矩阵连接。

## 第 2C.6 子阶段：multi-profile 编辑、诊断筛选与 IDE 生命周期

状态：已完成（profile set round-trip、retained 字段编辑、诊断筛选/增量计数/行选中、IDE discovery/plan、Windows direct launcher 和受限进程生命周期跟踪均已实现）。

范围与实现：

- 新增 `EditorBuildProfileStore`，以 version 2 JSON 保存最多 32 个 profile 和 selected id，同时兼容读取 version 1 单 profile；`EditorLayer` 的 Build 菜单、native menu 和 retained Build 面板支持保存/重新加载。
- retained Build 面板提供 profile selector，以及 Name/Build Directory 的受限输入提交；字段修改只更新内存 profile，显式 Save 才写入项目配置。
- 新增 `EditorToolIntegration`，发现 Visual Studio、Rider、VS Code、clangd，并根据项目根、build profile 和当前选中文件生成不可变 `EditorIdeLaunchPlan`；VS Code、Rider、Visual Studio 均接受有限行列定位参数。
- Windows launcher 使用 `CreateProcessW` 的直接 executable/argument/working-directory 边界启动外部工具，不经过 `cmd.exe` 或 PowerShell；非 Windows 和工具缺失均发布明确状态。
- Build 诊断栏提供 All/Error/Warning/Note 筛选、真实总数与分级计数、行选中态和 IDE 跳转；实时输出按完整行增量解析，展示诊断限制为 256 条而总数继续累计。
- launcher 保存外部 PID；主线程每 250ms 最多查询一次 `PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE`，只发布 Running/Exited/NotFound/QueryFailed 状态，不保存可写控制句柄。
- Build UI 只读取 `profileStatus`、`ideStatus` 和 IDE/toolchain presentation snapshots；profile 读写与进程创建不进入 paint/input。

契约与测试：

- `EditorBuildSystemTests` 验证 profile JSON/set round-trip、selected id 完整性、参数数组、重复 key 拒绝、项目外路径拒绝、IDE discovery、VS Code `--goto`、Rider/Visual Studio 行级参数、非法 launch plan、`query_process(0)` 和 Windows direct launch 后的 PID 状态读取。
- `EditorInteractionTests` 验证 retained Build panel 的 profile selector/输入控件能在项目根内写入 `.shinkou/build-profile.json`，随后 reload 并恢复字段，以及诊断筛选 region/命令能更新 editor state；既有编译命令、资源预览、AssetSystem bridge 和模型/图片回归继续执行。
- 本轮目标测试、全量构建和 CTest 通过；CTest 以单项 60 秒上限执行并返回成功，当前注册测试数为 50；`git diff --check` 通过（仅保留 Git 的 LF→CRLF 提示）。
- 视觉/宿主 QA：D3D11 请求的 `1280x720` dark 路径生成 `184` 个 UI command、`41` 个 text command 和非空 retained viewport，报告 `dpi=1.5`；捕获器仍返回 `WindowDC/GdiWindowSurface` 且 `--require-gpu` 返回 1，因此 BMP 不作为 GPU 像素通过证据。`1600x900` light/force-GDI 生成 `195` 个 command，`1024x768` high-contrast/force-GDI 生成 `181` 个 command，均报告正确主题、DPI 与非空 viewport；GDI BMP 仅作为宿主/命令证据。

安全/性能审计：

- profile JSON 上限 1 MiB、字符串 16 KiB、参数 128 项/项 16 KiB、对象 64 成员、深度 16；重复 key、NUL、绝对路径和 `..` 穿越均拒绝。
- IDE discovery 只检查项目 `.shinkou/tools`、`Tools` 和显式/进程环境 PATH 中的 regular file；launcher 不拼接 shell 命令，工作目录固定为项目根，选中文件必须位于项目根内。
- launcher 只保留 PID 并用受限查询权限读取状态；进程轮询节流且不提供 terminate/wait/输入管道，因此不能把“已启动”误报为“编译成功”，也不会接管外部 IDE 生命周期。
- 增量诊断仅消费输出回调的完整行，live lines/diagnostics 各有固定上限；计数与解析均在互斥锁内更新，paint 只读快照，不持有外部进程或编译器句柄。

失败/回滚路径：

- 缺少 profile 文件继续使用内存默认 profile；损坏/超限 profile 不覆盖当前 profile，并在 Build 状态和 Console 中报告错误。
- 工具不可发现、路径越界、参数含 NUL、非 Windows 或 `CreateProcessW` 失败均返回 unavailable/failed，不回退到 shell 执行。
- 删除 `.shinkou/build-profile.json` 即回到默认 profile；移除 launcher 命令不影响现有 CMake/MSBuild build runner。

未完成风险/下一步：

- Visual Studio solution/project 文件发现、profile 推荐关联、compile database 优先级选择和 `.clangd` 生成已在后续 2C.7 完成；当前 PID 查询仍只回答被启动进程本身的状态。
- 下一步进入结构化 GLTF image/material 或音频 waveform derived artifact，再回补 solution 内部 project graph、toolchain flags 和非 Windows launcher。

## 第 2C.7 子阶段：solution/compile database 自动关联与 clangd 配置

状态：已完成（项目文件发现、profile 推荐关联、compile database 校验、`.clangd` 规划/显式写入和 retained/native command 均已实现；solution 内部 graph 与跨平台 launcher 仍为后续能力）。

范围与实现：

- 新增 `EditorProjectIntegration`，在项目根内有界发现 CMake、Visual Studio solution/project、.NET、Meson、Cargo、`compile_commands.json` 和 `.clangd`；跳过 `.git`、`.shinkou`、`node_modules`，扫描最多 4096 项、递归深度 8、展示候选 256 项。
- profile 的 build directory compile database 优先于根目录和其他候选；profile 指定 project file 优先作为推荐，否则选择根目录 solution、CMake 或其他受支持 project 文件。
- Build retained UI、Editor Build 菜单和 Windows native Build 菜单均提供 Discover Projects、Associate Recommended Project、Generate `.clangd`；关联只更新当前内存 profile，显式 Save 才写回 `.shinkou/build-profile.json`。
- clangd plan 复用 `EditorCompileCommands::parse` 验证文件大小、路径和 arguments；输出固定为项目根 `.clangd`，写入复用 `FileSystemService::write_text_atomic`，不执行 shell 或外部工具。

契约与测试：

- `EditorProjectIntegrationTests` 验证根 solution 优先级、profile association、build-directory compile database 优先级、隐藏目录排除、越界 profile 拒绝、clangd YAML 内容和原子写入结果。
- `EditorInteractionTests` 验证 retained project command regions、发现/关联状态、`.clangd` 项目根文件结果，并继续覆盖既有 profile、资源预览、AssetSystem 和模型回归。
- 本轮目标测试、全量构建和 CTest 均通过：注册测试 51 项，结果 `51/51`；`git diff --check` 仍为最终门禁。
- 视觉/宿主 QA：`1280x720` dark/D3D11 请求生成 `184` command、`41` text、非空 viewport，报告 `dpi=1.5`，但 capture 返回 `WindowDC/GdiWindowSurface`，`--require-gpu` 返回 1，故不作为 GPU 像素通过证据；`1600x900` light/force-GDI 生成 `195` command，`1024x768` high-contrast/force-GDI 生成 `181` command，均报告正确主题、DPI 与非空 viewport。当前 GDI BMP 无法被图像读取器可靠解码，因此保留 command/host trace，像素检查标为 inconclusive。

安全/性能审计：

- 发现不跟随目录符号链接，候选路径经过 `weakly_canonical` 和项目根包含检查；profile build/project 路径越界、隐藏目录和非 regular file 均不会成为候选。
- 扫描只读取文件元数据；解析 compile database 的输入上限为既有 16 MiB/32768 条/4096 参数约束，UI 只接收 project discovery snapshot。
- `.clangd` 输出路径固定为项目根 `.clangd`，不接受 UI 任意输出路径；写盘使用临时文件 + rename，生成失败不覆盖已有配置。

未完成风险/下一步：

- 当前只发现 solution/project 文件，不解析 `.sln` 内部 project graph，不生成 Visual Studio solution，也不写入 clangd 的 toolchain flags 或 compile database watch。
- 非 Windows IDE launcher 和 IDE 子进程树语义仍未完成；下一入口为结构化 GLTF image/material 或音频 waveform derived artifact。

## 第 2C.8 子阶段：GLTF image artifact → WIC 图片预览

状态：已完成首个可验证闭环；材质采样、GPU 资源和正式 derived cache 仍明确未完成。

范围与实现：

- `EditorModelTextureArtifact` 作为 renderer-neutral immutable payload 加入模型快照；GLTF/GLB provider 处理项目内相对 image URI、base64 data URI 和 bufferView image，并保留 image index/URI/MIME。
- 增加 `load_editor_gltf_preview_bytes()`，编辑器在 AssetSystem 已初始化时为 OBJ、GLTF、GLB 请求 typed `model` source payload；GLTF 的外部 buffer/image 仍通过项目根受限的 `FileSystemService` 解析。
- 模型快照到达后，EditorLayer 以独立 generation/stamp/path 管理纹理预览 future，第一项有效 artifact 进入现有 WIC memory decoder；`EditorAssetPreviewUiState` 发布缩略图尺寸、snapshot 和明确的 `Texture preview ready/unavailable` 状态，retained UI 只消费这些状态。

契约与测试证据：

- `EditorGltfPreviewTests` 新增项目内 PNG artifact、GLB source-byte overload、data URI `AA==` 解码，以及 artifact 数量/字节校验；原有 geometry、metadata、取消、损坏 GLB 和越界 URI 断言保留。
- 目标测试命令：`shinkou_editor_gltf_preview_tests.exe`，结果 `Editor glTF/GLB preview provider passed`。
- 本轮最终全量构建通过；CTest `51/51` 通过（总计约 41.12 秒），`git diff --check` 通过并仅报告既存的 LF→CRLF 工作树提示，未发现 whitespace error。

安全审计：

- GLTF 单个 image artifact 32 MiB、aggregate 64 MiB；源/JSON/buffer 和 WIC 输入/thumbnail 原有限额仍生效。base64 只接受明确 `;base64` data URI。
- 外部 image URI 必须为项目相对路径，禁止 scheme、绝对路径、NUL 和根外解析；AssetSystem bytes 仅作为不可变源输入，不绕过外部 image 的 FileSystemService 边界。
- 异步纹理结果同时检查 generation、selected path 和 model source stamp；selection/root/shutdown 会使旧结果失效并等待 future，不在 paint/input 中读文件或调用 WIC。

性能与失败路径：

- artifact 只复制一次受限 encoded bytes，WIC 缩放沿用 512×512 输出像素预算；shared immutable vectors 避免 retained paint 复制或重复解析。
- 没有 image artifact、WIC 不可用、损坏 payload 或超限输入均保留模型几何快照并显示纹理失败/无 payload 状态；不会伪造 GPU texture/material 成功。
- 当前只自动预览第一项有效 artifact；这是明确的垂直切片限制，不等同于完整 glTF material preview。

视觉/宿主证据：

- 模型 Inspector 追加纹理 thumbnail/status 绘制路径，保留原有线框、统计、Reset 和中键/滚轮交互；后续 Windows capture 仍须记录主题、DPI、viewport 与 backend surface-kind。
- 2C.8 capture：dark `1280x720 --require-gpu` 生成 `184` commands、`41` text、`225,67,383.333,184` viewport、`dpi=1.5`，但返回 `WindowDC/GdiWindowSurface` 且退出码 1；light `1600x900 --force-gdi` 生成 `195` commands、`41` text、`225,67,596.667,304` viewport、`dpi=1.5`，退出码 0。BMP 读取器仍报告无效 base64，因此这些只作为 host/retained command evidence，不能宣称 GPU 像素验证。

剩余风险/下一入口：

- 尚未建立 material → texture → artifact 的可选列表，没有 normal/metallic/roughness 多纹理选择、采样器语义、颜色空间、GPU upload、derived cache 或节点/动画导入。
- 下一轮先补 artifact 选择与材质引用可视化，再进入 renderer-owned preview target；音频 waveform 已接入现有音频试听，但连续视频/音频 transport 和资源拖入场景事务仍需独立轮次。

## 第 2C.9 子阶段：GLTF material → texture artifact 选择

状态：已完成材质引用选择与纹理切换垂直切片；GPU 材质采样、完整 sampler 语义和 renderer-owned preview target 仍未完成。

范围与实现：

- `EditorAssetPreviewUiState` 新增 material/texture/image index、名称标签和 `Base Color`/`Normal` role；模型 Inspector 增加 retained 上一项/下一项按钮。
- material 选择优先解析 `baseColorTexture`，否则使用 `normalTexture` 或第一个 texture；texture 选择按有界 texture table 循环，并按 image index 选择对应 `EditorModelTextureArtifact`。
- WIC memory future 复用 2C.8 的 immutable artifact seam；每次选择增加 generation、清除旧 snapshot，并在结果回写前检查 generation、selected path、model source stamp；按钮动作只改变 editor preview state，不修改活动 `World`。

契约与集成测试：

- `EditorInteractionTests` 的 AssetSystem model processor 对 `.gltf` 保留 typed source bytes，fixture 包含两个 material、两个 texture、两个 data-URI image；测试验证首个 material/base-color/image、WIC snapshot、四个 selector regions，并点击 material-next 验证第二个 texture/image 引用。
- `EditorGltfPreviewTests` 继续验证 image artifact 的外部 URI、data URI、bufferView、GLB source-byte overload、安全失败和 metadata 引用边界。
- 2C.9 目标编译与 `EditorInteractionTests`、`EditorGltfPreviewTests` 已通过；最终全量构建成功，CTest `51/51` 通过（总计 `37.29 sec`），`git diff --check` 通过且仅报告既存的 LF→CRLF 工作树提示。

安全与性能审计：

- 选择索引全部在 immutable snapshot table 范围内；负 delta 使用有界取模，空 material/texture table 的按钮为安全 no-op。
- 选择不会复制或解码整个模型；只有选中的 encoded artifact 进入 WIC worker，旧任务通过 generation 丢弃，shutdown 等待 texture future。
- image artifact 的 32 MiB 单项/64 MiB aggregate、WIC 512×512 thumbnail 和项目路径边界沿用 2C.8；没有 artifact 时显示明确 payload unavailable，不伪造材质成功。

视觉/宿主证据：

- Inspector 保留线框、thumbnail、统计、Reset 与中键/滚轮；material/texture selector regions 已由 interaction test 检查。
- 本轮 dark `1280x720 --require-gpu` 生成 `184` commands、`41` text、`11` assets、`225,67,383.333,184` viewport，报告 `dpi=1.5`、`surface-kind=GdiWindowSurface`，退出码 1；light `1600x900 --force-gdi` 生成 `195` commands、`41` text、`11` assets、`225,67,596.667,304` viewport，报告 `dpi=1.5`、`surface-kind=GdiWindowSurface`，退出码 0。两次均报告 `captured=1`；BMP 读取器仍无法可靠解码，因此只作为 host/retained command evidence，不宣称 GPU 像素验证。旧 UIKit 文档/host 文件缺失继续列为集成审计限制。

剩余风险/下一入口：

- 尚未显示所有纹理 slot 的专用面板，未实现 normal/metallic/roughness 语义、颜色空间、完整 sampler 参数、shader evaluation、GPU upload、动画/节点导入或 persisted derived cache。
- 下一轮进入 renderer-owned model preview target 能力矩阵；GPU 不可用时保持线框 + WIC artifact fallback，并继续音视频连续 transport 与资源拖入场景事务。

## 第 4.10 子阶段：AudioSystem cursor/seek transport

状态：已完成首个真实音频 transport 时钟闭环；连续视频播放、音视频同步和更多 codec 仍未完成。

范围与实现：

- `IAudioBackend` 增加兼容的可选 `seek()`、`cursor_seconds()` 和 `supports_cursor()` seam；旧 backend 保持默认 no-op/zero 能力，编辑器不会把它误报为真实硬件位置。
- Miniaudio backend 通过 decoder data format 将秒数转换为 PCM frame seek，并通过 `ma_sound_get_cursor_in_seconds()` 发布真实播放游标；`AudioSystem` 统一处理非有限和负数输入。
- `EditorLayer::MediaSeek` 在活动音频 voice 上调用 AudioSystem seek；媒体状态同步从支持 cursor 的 backend 回写 timeline，Play/Pause/Stop/Loop/Volume 仍走 UI bus voice。

契约与测试：

- `AudioSystemTests` 验证 fake backend 的 cursor 前进、seek 回写和能力声明。
- `EditorAudioPreviewTests` 验证音频资源播放后的 seek 进入真实 voice transport，并保留 WAV 元数据、波形、取消、项目根边界和生命周期清理断言。
- 目标编译和两项聚焦测试已通过；最终全量构建成功，CTest `51/51` 通过（总计 `33.11 sec`），`git diff --check` 通过且仅报告既存的 LF→CRLF 工作树提示。

安全/性能审计：

- seek 输入在 UI 归一化值、已知 duration 和 backend PCM frame 三层进行有限/非负约束；不把 UI 字符串传给 shell 或文件系统。
- cursor 查询只读取活动 voice 状态，不在 paint 中打开文件或解码；voice handle 的清理仍由 AudioSystem 统一负责。
- 不支持 cursor 的 backend 保留明确能力缺失，不用 wall clock 或 UI delta 冒充 decoder position；资源切换、shutdown 和 voice 回收不会保留悬空句柄。

视觉/宿主证据：

- dark `1280x720 --require-gpu` 生成 `184` commands、`41` text、`11` assets、`225,67,383.333,184` viewport，报告 `dpi=1.5`、`surface-kind=GdiWindowSurface`，退出码 1。
- light `1600x900 --force-gdi` 生成 `195` commands、`41` text、`11` assets、`225,67,596.667,304` viewport，报告 `dpi=1.5`、`surface-kind=GdiWindowSurface`，退出码 0。
- 两次均报告 `captured=1`；当前 BMP 读取器仍无法可靠解码，因此保留 host/retained command evidence，不宣称 GPU 像素验证。

未完成风险/下一入口：

- 尚未建立 bounded audio/video decoder session、跨媒体时钟策略、视频连续播放/暂停、音频轨输出或 codec 能力矩阵。
- 下一入口继续 renderer-owned model preview target 和 GPU material sampling，同时把音视频 transport 统一到可审计的 decoder session。

## 第 4.11 子阶段：renderer-owned model geometry pass

### 实现范围

- `EditorModelPreviewRenderer` 位于 `shinkou::editor`，保持 provider snapshot 与 renderer resource 生命周期分离；`EditorLayer::draw` 只在模型 snapshot/scene state 有效时调用它。
- 渲染器扫描当前 `RenderGraph` 的颜色 attachment，追加 `editor_model_preview` pass，并使用已有 editor viewport seam 的物理像素 viewport/scissor。pass `clearAttachments=false`，因此不会清空世界或覆盖编辑器 shell。
- DirectX 11 路径创建 persistent vertex/index/constant buffers、HLSL shaders、pipeline 与 material；相机由 immutable model bounds + retained orbit/zoom state 生成 view-projection，材质当前应用 `baseColorFactor`。
- UI 只接收 `modelGpuPreviewReady`、`modelGpuMaterialApplied`、`modelGpuTextureSampled` 和 status；当后端/设备/目标不满足条件时，Inspector 仍显示线框/WIC artifact，并显示明确的 `fallback`/`waiting` 原因。

### 证据

- 聚焦测试：`shinkou_editor_model_preview_renderer_tests.exe` 输出 `Editor model preview renderer capability fallback passed native-d3d11-attempted=1 native-d3d11-pass=1 native-texture-sampled=1`；Null backend 下没有追加 GPU pass，非法索引 snapshot 被拒绝，原生 D3D11 路径创建并接入了 shader/pipeline/material/texture pass。
- `shinkou_editor_interaction_tests.exe` 输出 `Editor document, real filesystem, input routing, inspector, undo and simulation passed`。
- 完整构建：`cmake --build out/build/mingw-debug -j 4` 成功，包含 `shinkou_engine_sample`、`shinkou_ui_capture` 和 52 个测试目标。
- CTest 首次顺序门禁为 `51/52`：唯一异常是既有 `shinkou_math_parallel_tests` 在全量负载下达到 60 秒超时；无失败断言。隔离复跑命令 `ctest --test-dir out/build/mingw-debug -R '^shinkou_math_parallel_tests$' --timeout 180 --output-on-failure` 结果 `1/1 passed`、总计 `0.06 sec`；随后完整复跑以 `--timeout 180` 得到 `52/52 passed`、总计 `17.82 sec`。
- `git diff --check` 返回通过；输出只有既存 LF→CRLF 工作树提示。

### 安全与性能审计

- snapshot 上传前检查 `valid()`、2,000,000 vertex / 6,000,000 index 硬上限、有限浮点位置和 index < vertex count；字节数乘法有 `size_t` 溢出保护。
- renderer pass 没有文件系统、WIC、AssetSystem、shell 或 IDE 调用；选中 material index 只在 immutable material table 内读取，颜色被限制到 `[0,1]`。
- persistent resource 只在 geometry revision、camera projection key 或 material revision 变化时更新；不重复创建每帧 GPU resource，不在模型 pass 清屏。
- 图形能力矩阵仍显式：只对已就绪 D3D11 + editor viewport seam + rgba8/bgra8 graph target 尝试 shader pass；Null/Vulkan/D3D12/目标缺失都走可见 fallback，不静默伪造成功。

### Windows 视觉/宿主审计

- 捕获命令使用 D3D11 请求：`1280x720 --require-gpu --editor dx11 --asset-view tree --theme dark`。输出：`184` commands、`41` text、`11` assets、viewport `225,67,383.333,184`、`dpi=1.5`、`captured=1`。
- 实际 surface 为 `GdiWindowSurface`，捕获器退出码为 1；`view_image` 可看到 GDI 回退的黑色 surface/窗口 chrome，但不能把它当作 D3D11 GPU 像素验证。该证据只证明窗口启动、host/UI command 生成和能力失败被明确报告。
- `ui/docs/Architecture.md` 与历史 `engine/src/editor/UiKitPanelHost.cpp` 在当前工作树不存在；审计仍以实际 retained `shinkou::ui` path 和当前 `RenderBackend`/`Renderer` seam 为准，不把缺失历史文件当作现行链路。

### 未完成风险与下一轮

- 4.12 已实现有界 WIC BGRA8 artifact → D3D11 texture upload → linear/clamp sampler → pixel shader sampling；仍未实现真实 glTF UV、独立 offscreen Inspector target、深度/灯光/PBR、节点/动画和拖入场景事务。
- D3D11 HLSL pass 没有在本机得到 GPU readback 证据；Vulkan/D3D12 需要各自 shader source/bytecode 和 descriptor capability 后再进入矩阵，不能沿用 D3D11 代码冒充支持。
- 下一轮目标：继续扩展 material slot/PBR，并为失败/设备恢复保留线框/WIC 回退和可审计状态。

## 第 4.12 子阶段：D3D11 BGRA8 base-color texture sampling

### 实现与范围

- 选中的 `ui::UiImageSnapshot` 通过 `EditorLayer` 传入 `EditorModelPreviewRenderer`；snapshot 必须是 immutable、有效 BGRA8 payload，且宽高不超过 `512x512`。
- renderer 创建或复用 `bgra8` texture、linear/clamp sampler、material descriptor 和 HLSL pixel shader 资源；shader 现在从 `BaseColorTexture.Sample(BaseColorSampler, uv)` 取得颜色并乘以 base color factor。
- 没有有效 image artifact 时使用 1x1 白色 GPU fallback，但 `textureSampled=false`，UI status 继续明确显示 artifact unavailable；不会把 fallback 冒充成真实纹理。

### 契约与证据

- `EditorModelPreviewRendererTests` 同时覆盖 Null backend capability fallback、非法 geometry 拒绝、原生 D3D11 geometry/material pass 和 1x1 BGRA8 texture sampling。最终聚焦输出应为：`native-d3d11-attempted=1 native-d3d11-pass=1 native-texture-sampled=1`。
- 4.12 代码包含在 `shinkou_engine_sample`、`shinkou_ui_capture` 和测试目标的完整构建中；最终 `cmake --build out/build/mingw-debug -j 4` 成功，CTest `52/52 passed`、总计 `37.00 sec`，`git diff --check` 返回 0（仅有 Windows LF→CRLF 提示）。

### 安全、性能与回退审计

- snapshot 在 renderer 资源创建前校验 revision、像素 buffer 精确大小、尺寸和 512×512 上限；没有文件 IO、WIC decode、shell 或 IDE 调用进入 paint/render pass。
- texture/sampler/material 是 persistent renderer resources；尺寸变化、revision 变化或 recovery 才触发重建/更新，geometry 与 camera 仍按 revision/key 有界更新。
- 只在 D3D11 device ready、editor viewport seam、rgba8/bgra8 color target 和资源创建全部成功时追加 graph pass；Null/Vulkan/D3D12/unsupported target 继续保留 retained/WIC fallback。

### Windows 视觉/宿主证据

- 最终 capture 命令仍请求 `1280x720 --require-gpu --editor dx11 --theme dark`，报告 `184` commands、`41` text、`11` assets、viewport `225,67,383.333,184`、`dpi=1.5`、`captured=1`。
- 实际 surface 仍为 `GdiWindowSurface`，退出码为 1；BMP 可见窗口菜单和黑色 GDI surface，但没有 D3D11 GPU readback，因此只证明宿主启动、retained UI command 生成和能力降级，不把它当作模型/纹理 GPU 像素验证。

### 视觉宿主证据与未完成项

- 本轮仍未取得可靠的 GPU window readback；Windows capture 若返回 `GdiWindowSurface`，只记录窗口、DPI、retained command 与 capability fallback evidence，不宣称最终像素呈现。
- 真实 glTF `TEXCOORD_0`、normal/metallic/roughness/PBR/light/depth、完整色彩空间和 sampler 语义、Vulkan/D3D12 shader path、独立 offscreen Inspector target、动画/节点、连续音视频 decoder/A-V clock 和资源拖入场景事务仍未完成。

## 第 4.13 子阶段：glTF TEXCOORD_0 → D3D11 vertex input

### 实现与范围

- `EditorGltfPreview` 新增 float `VEC2` `TEXCOORD_0` accessor 读取，沿用 bounded bufferView、stride、offset、finite-value 和取消检查；primitive 没有 UV 时补零坐标，但只有至少一个真实 UV accessor 时才发布 `textureCoordinates` snapshot。
- `EditorModelPreviewRenderer` 把 `POSITION + TEXCOORD0` 打包为 `PreviewVertex`，D3D11 `PipelineDesc` 通过追加的 `vertexTextureCoordinates` 字段选择双元素 input layout；真实 UV 进入 vertex shader 后再交给 pixel shader 采样。
- OBJ/无 UV 快照保留位置派生坐标回退；这不是 glTF UV 的伪造，`textureCoordinatesApplied` 单独记录实际 snapshot 是否提供了 UV。

### 契约与证据

- `EditorGltfPreviewTests` 的三角形 glTF/GLB fixture 现含三项 TEXCOORD_0 数据，断言 snapshot 的 UV 数量与值；目标输出 `Editor glTF/GLB preview provider passed`。
- `EditorModelPreviewRendererTests` 断言原生 D3D11 render state 的 `textureCoordinatesApplied`、geometry/material/texture 路径，同时保留 Null backend fallback 和非法索引拒绝；输出 `native-d3d11-attempted=1 native-d3d11-pass=1 native-texture-sampled=1`。
- 最终构建 `cmake --build out/build/mingw-debug -j 4` 成功；全量 CTest `52/52 passed`、总计 `45.96 sec`；`git diff --check` 返回 0，仅有 Windows LF→CRLF 提示。

### 安全、性能与视觉审计

- provider 不在 accessor 解析阶段解码图片、不执行 shell、不读项目根外路径；UV 数量跟 POSITION 一致，读取前后均有边界/有限性检查。
- pipeline cache key 包含 vertex input layout 形态；D3D11 vertex buffer stride 与 input layout offset 均为固定 `POSITION(12 bytes)+TEXCOORD0(8 bytes)`，只在 geometry revision 改变时更新。
- UI capture 仍可能返回 `GdiWindowSurface`；若没有 GPU readback，只记录 retained command、DPI、surface-kind 和 capability fallback，不宣称纹理 UV 的最终窗口像素。

### 未完成项

- UV transform、normal/tangent、normal/metallic/roughness、PBR lighting/depth、完整 sampler/色彩空间、Vulkan/D3D12 shader path、独立 offscreen Inspector target、动画/节点、连续音视频 decoder/A-V clock 和资源拖入场景事务仍未完成。

## 第 4.14 子阶段：glTF NORMAL → D3D11 lit model preview

### 实现与范围

- `EditorGltfPreview` 新增 float `VEC3` `NORMAL` accessor 读取，沿用 bounded bufferView、stride、offset、finite-value、非零长度和取消检查；发布前归一化，primitive 没有 NORMAL 时使用稳定 fallback，但只有真实 NORMAL accessor 才发布 `normals` snapshot。
- `EditorModelPreviewRenderer` 把 `POSITION + TEXCOORD0 + NORMAL` 打包为 `PreviewVertex`，D3D11 `PipelineDesc` 通过追加的 `vertexNormals` 字段选择三元素 input layout；vertex shader 传递法线，pixel shader 使用固定方向的低强度 diffuse 光照帮助观察模型朝向。
- 这是 renderer-owned lit preview seam，不是完整 PBR；`normalsApplied` 单独记录 provider 法线是否实际进入 GPU vertex payload。

### 契约与证据

- `EditorGltfPreviewTests` 的 glTF/GLB fixture 现含三项法线并断言归一化结果；`EditorModelPreviewRendererTests` 断言 native D3D11 的 `normalsApplied`、UV、geometry/material/texture 路径，并保留 Null backend fallback 与非法索引拒绝。
- 最终 `cmake --build out/build/mingw-debug -j 4` 成功；一次全量 CTest 在既有 `shinkou_math_parallel_tests` 上 180 秒超时，隔离复跑 `1/1 passed`、`0.04 sec`，随后完整重跑 `52/52 passed`、总计 `18.99 sec`。

### 安全、性能与视觉审计

- NORMAL accessor 数量必须跟 POSITION 一致，向量必须有限且长度大于 epsilon；provider 不解码图片、不执行 shell、不访问项目根外路径。
- D3D11 input layout offset 为固定 `POSITION(12)+TEXCOORD0(8)+NORMAL(12)`，pipeline cache key 包含 layout 形态；vertex payload 只按 geometry revision 更新。
- 本轮最终 UI capture 命令报告 `184` commands、`41` text、`11` assets、viewport `225,67,383.333,184`、`dpi=1.5`、`captured=1`，实际为 `GdiWindowSurface`/退出码 1；GDI surface 只证明窗口/retained command/fallback，不证明 lit preview 的 GPU 像素。

### 未完成项

- tangent/normal map、metallic/roughness 纹理、真正 PBR BRDF、深度/阴影/灯光实体、UV transform、完整色彩空间和 sampler 语义、Vulkan/D3D12 shader path、独立 offscreen Inspector target、动画/节点、连续音视频 decoder/A-V clock 和资源拖入场景事务仍未完成。

## 第 4.15 子阶段：glTF metallic/roughness factor lit material

### 实现与范围

- 选中的 `EditorModelMaterialPreview` 的 metallic/roughness factors 进入 persistent `PreviewMaterial` constant buffer；CPU 端校验 finite、`[0,1]` 和最小 roughness，避免非法因子进入 shader。
- D3D11 fragment shader 使用 albedo、真实 normal、camera direction、固定 preview light、metallic 和 roughness 计算受限 diffuse/specular 响应；material 缺失时使用默认非金属、粗糙材质。
- `EditorLayer` 将 `materialFactorsApplied` 回写 retained asset preview state，状态行报告 `metallic/roughness factors applied`；其他 backend 继续明确 fallback。

### 契约与证据

- `EditorModelPreviewRendererTests` 使用 metallic `0.25`、roughness `0.75` fixture，断言 `materialFactorsApplied`、真实 UV/normal、原生 D3D11 pass 和纹理采样；最终输出包含 `native-material-factors=1 native-texture-sampled=1`。
- 中途测试捕获到 fragment shader 缺失 `SceneFrame` cbuffer 的真实失败并完成修复；修复后目标编译、完整构建成功，CTest `52/52 passed`、总计 `39.14 sec`，交互回归通过。

### 安全、性能与视觉审计

- factor 只从 immutable material table 读取，不进行文件 IO、shell 或路径解析；非法数值通过有限默认值处理。
- material constant buffer 只在 geometry revision/material selection 变化时更新；shader/pipeline/material 仍由 Renderer 持有，失败时不追加伪造 graph pass。
- 最终 UI capture 报告 `184` commands、`41` text、`11` assets、viewport `225,67,383.333,184`、`dpi=1.5`、`captured=1`，实际为 `GdiWindowSurface`/退出码 1；不能把固定光照或材质状态当作 GPU 窗口像素证据。

### 未完成项

- metallic/roughness texture、normal map/tangent、完整 GGX/IBL/BRDF、深度/阴影/真实灯光实体、颜色空间和 sampler 完整语义、Vulkan/D3D12 shader path、独立 offscreen Inspector target、动画/节点、连续音视频 decoder/A-V clock 和资源拖入场景事务仍未完成。

## 第 4.16 子阶段：material texture role / normal map preview

### 实现与范围

- 新增 EditorModelPreviewTextureRole，EditorLayer 按 material slot 把选中的纹理标成 BaseColor 或 Normal；未知/未关联纹理保持兼容性的 Base Color 回退。
- D3D11 renderer 同时维护 Base Color 与 Normal 两套 texture/sampler；Normal shader 绑定使用 t5/s6，并以 ddx/ddy 从 world position/UV 构造受限预览切线基，再执行 tangent-space normal 采样。
- retained UI state 新增 role-applied、base-color-sampled、normal-sampled 标志，GPU 状态行明确报告 normal texture sampled，避免 Normal 被伪报告为 Base Color。

### 契约与证据

- EditorModelPreviewRendererTests 使用同一 1×1 decoded image 连续渲染 Base Color 与 Normal：前者断言 baseColorTextureSampled=1 且 normalTextureSampled=0，后者相反，并断言状态含 normal texture sampled。
- 原生 smoke 首次暴露描述符布局冲突：Normal t2 与 SceneFrame b2 在当前统一 slot 校验中冲突；修复至 t5/s6 后重新编译，输出 native-d3d11-attempted=1 native-d3d11-pass=1 native-material-factors=1 native-texture-sampled=1。
- 目标构建成功；全量 CTest 52/52 passed、总计 41.96 sec；交互测试通过 14.35 sec。

### 安全、性能与视觉审计

- role 只由 immutable material/texture metadata 驱动；WIC 仍负责有界解码，GPU 上传保持 512×512 限制，Normal 缺失时使用固定 1×1 flat-normal，不访问额外路径。
- material role 改变会刷新 constant buffer 的 normalMapEnabled，纹理 revision 按角色分别缓存；正常帧不做文件 IO，不重复创建稳定 sampler。
- 这一轮沿用最终 UI capture 的宿主限制：184 commands、41 text、11 assets、viewport 225,67,383.333,184、DPI 1.5，实际 GdiWindowSurface/退出码 1；因此只把它作为 window/retained-command/fallback evidence，不宣称 GPU 像素验证。

### 未完成项

- 完整 glTF tangent accessor、normal scale、UV transform、metallic/roughness texture slot、完整 GGX/IBL/BRDF、深度/阴影/真实灯光实体、颜色空间/sampler 完整语义、Vulkan/D3D12 shader path、独立 offscreen Inspector target、动画/节点、连续音视频 decoder/A-V clock 和资源拖入场景事务仍未完成。

## 第 4.17 子阶段：glTF metallic/roughness texture slot

### 实现与范围

- EditorModelMaterialPreview 新增 metallicRoughnessTexture；glTF pbrMetallicRoughness 解析器校验对象、索引和 texture table 边界。
- EditorLayer 将选中 material texture 显示为 Base Color、Normal 或 Metallic/Roughness，renderer API 和 retained state 增加对应角色及 sampled 标志。
- D3D11 renderer 为 Metallic/Roughness 使用独立 texture/sampler 与 render-graph ShaderRead 依赖；shader 采样 B 通道金属度、G 通道粗糙度，再与 material factors 相乘。

### 契约与证据

- glTF provider fixture 增加 metallicRoughnessTexture index，并断言 material slot 解析为 0。
- 同一 provider fixture 将该 slot 改为越界索引，断言返回明确的 metallicRoughnessTexture index is invalid，而不是继续生成 snapshot。
- renderer 测试连续覆盖 Base Color、Normal、Metallic/Roughness 三种角色，断言角色之间不会串用 sampled 状态。
- 聚焦输出：native-d3d11-attempted=1 native-d3d11-pass=1 native-material-factors=1 native-texture-sampled=1 native-metallic-roughness-texture=1；全量 CTest 52/52 passed、总计 42.67 sec。
- 中途测试捕获到状态文本契约未包含 sampled 语义，已修复为明确的 metallic/roughness texture sampled 状态后重新通过。

### 安全、性能与视觉审计

- slot 索引来源于 immutable glTF material table；非对象、越界和缺失 payload 不会进入 shader；上传继续受 512×512 限制。
- 三类 texture revision 独立缓存，角色切换会刷新 material constant buffer；稳定 sampler 不在正常帧重复创建。
- render graph 明确声明 Metallic/Roughness 纹理和采样器的 ShaderRead；失败时保留 fallback，不伪造 GPU pass。
- 本轮宿主 capture 仍沿用 GdiWindowSurface 限制：只能证明窗口启动、DPI、retained command 与 fallback，不能证明 GPU 窗口像素。

### 未完成项

- 完整 GGX/IBL/BRDF、遮挡/发光/透明混合、normal scale、UV transform、tangent accessor、完整颜色空间/sampler 语义、Vulkan/D3D12 shader path、独立 offscreen Inspector target、动画/节点、连续音视频 decoder/A-V clock 和资源拖入场景事务仍未完成。

## 第 4.18 子阶段：独立 offscreen model Inspector target

### 实现与范围

- `RenderCapabilities` 增加 `supportsEditorOffscreenTarget`；`IRenderBackend::bind_editor_render_target` 是显式的 pass-local target seam。D3D11 实现校验 color/depth handle、绑定 RTV/DSV、按请求清理；空 color handle 只恢复交换链，不改变 editor viewport/scissor 规则。
- `EditorModelPreviewRenderer` 不再扫描或依赖场景 color attachment；D3D11 路径按 editor viewport 创建 persistent BGRA8 离屏色彩目标，限制到 4096×4096。模型 pass 写该目标，composite pass 读取它并以 fullscreen SV_VertexID 三角形回写 editor viewport。
- composite shader、pipeline、material 与 model lit 资源分开创建/销毁；target 尺寸或 format 改变时重建 target，并在下一帧重新建立 graph import/bindings。Inspector retained state 回写 target-ready/composite-applied 两项证据。

### 契约与证据

- `EditorModelPreviewRendererTests` 的 native D3D11 smoke 不再添加 fixture color target，直接验证 renderer 能追加 2 个 pass；Base Color、Normal、Metallic/Roughness 连续切换的 sampled/role 状态继续保持正确。
- 目标构建、全量构建和聚焦测试均已通过：输出包含 `native-d3d11-attempted=1 native-d3d11-pass=1 native-material-factors=1 native-texture-sampled=1 native-metallic-roughness-texture=1 native-offscreen-executed=1`；全量 CTest `52/52 passed`、0 failures、总计 `19.63 sec`。

### 安全、性能与视觉审计

- viewport 转换使用 finite/ceil/clamp，超过 4096 或无效尺寸直接拒绝；offscreen resource 仍在 renderer 资源命名空间内，不产生外部文件、shell 或网络副作用。
- persistent target 仅在尺寸/format 变化时创建，普通帧复用并由 graph import；模型写入与 composite 读取之间声明显式资源状态迁移，避免 D3D11 同时绑定 RTV/SRV。
- pass-local target API 只在模型 preview callback 中使用；场景 pass 的 strict target 逻辑没有放宽。沿用宿主 capture 限制：GdiWindowSurface 只能作为窗口/DPI/retained command/fallback 证据，不能替代 GPU readback 的像素证明。
- 宿主 UI capture 记录 `editor-ui-commands=184 editor-ui-text=41 editor-ui-assets=11 editor-ui-visible-assets=11 editor-ui-viewport=225,67,383.333,184 editor-ui-dpi=1.5`，窗口 `1280×720` client、DPI `144`；输出 surface-kind 为 `GdiWindowSurface`，截图已检查，未将黑色 GDI 合成面误报为 GPU preview 像素。

### 失败状态与回滚路径

- D3D11 capability 缺失、目标创建失败、目标绑定失败或 shader/material/pipeline 创建失败时，renderer 返回明确 status，不追加伪造 ready 状态；EditorLayer 保留 provider/WIC 预览并清空 GPU capability flags。
- target 生命周期由 `EditorModelPreviewRenderer::clear` 管理；pipeline 重建只释放 preview/composite pipeline 资源，不触碰 provider snapshot 或项目文件。

### 未解决风险

- 当前没有 GPU readback/staging copy 和像素级 visual oracle；D3D11 compositor 的实际窗口像素仍需后续 readback 轮验证。
- 目标格式固定为 D3D11 editor swapchain 的 BGRA8；Vulkan/D3D12 尚未有等价 offscreen shader/bind path。深度、完整 BRDF、透明、动画/节点仍不在本轮范围。

## 第 4.19 子阶段：受控 GPU readback / visual oracle 基础

### 实现与范围

- `TextureReadbackRequest` 规定 persistent Texture2D、mip/layer、可选矩形和 `maxBytes`；`Renderer::read_texture` 先验证 persistent resource、区域边界、字节溢出，再转发到 backend。
- D3D11 `read_texture` 校验 single-sample 和受支持 color formats，创建临时 CPU-readable staging texture，使用 `CopySubresourceRegion` + `Map(D3D11_MAP_READ)`，返回紧密排列的 rows；Map/Copy/资源类型失败均有明确错误。
- `supportsTextureReadback` 与 offscreen target 能力独立报告；`EditorModelPreviewRenderer` 仅暴露 offscreen handle 供 QA 读取，正常 EditorLayer 帧不触发同步 readback。

### 契约与证据

- 聚焦测试在提交 2-pass offscreen/composite graph 后读取完整 64×64 区域，并断言 `valid`、尺寸、row pitch、至少一个非零字节和紧密字节上限；另用 4×4 初始纹理验证 readback 上传回路。输出包含 `native-offscreen-executed=1 native-offscreen-readback=1 native-offscreen-pixel-activity=1 native-uploaded-texture-readback=1`；非零断言证明结果不是只有资源创建而没有绘制活性。
- 之前的 4.18 目标切换、材质角色、glTF slot 和全量 UI/资源测试继续保留；最终全量构建通过，CTest `52/52 passed`、0 failures、总计 `8.36 sec`。

### 安全、性能与视觉审计

- Renderer/backend 双层执行 maxBytes 和边界校验，避免 staging 分配或 row copy 无界增长；只有允许的 RGBA8/BGRA8/RGBA16F/R32F color data 进入结果。
- readback 明确是可能阻塞 GPU 的诊断操作，不放入 paint/input/普通 tick；staging texture 使用后立即释放，不加入 persistent resource pool。
- 本轮没有自动把 readback 结果写文件或执行颜色/区域阈值比较；非零检查只作为最低限度的 draw-activity oracle。宿主 capture 的 `GdiWindowSurface` 仍只证明窗口、DPI、retained command 和 fallback，不能冒充 GPU 窗口像素。

### 失败状态与回滚路径

- backend 不支持、handle 非 persistent、mip/layer/region 越界、MSAA/深度/压缩格式、Map/Copy 失败时结果清空并保留错误字符串；不会改变模型 preview graph 或项目文件。

### 未解决风险

- 目前只实现 D3D11 同步 readback，仍缺异步 staging/fence、Vulkan/D3D12 实现、颜色空间转换和正式的像素阈值 visual oracle；完整 PBR、深度、透明、动画/节点仍未完成。

## 第 4.20 子阶段：Project 资源拖入视口与场景引用事务

### 实现与范围

- `EditorUi` 在 retained Project 行上保留拖拽源；左键移动超过 5 logical px 后显示视口 drop cue，释放时通过既有 pointer capture 把源路径和视口坐标交给 `EditorLayer`。
- `EditorLayer::drop_asset_to_viewport` 在写入场景前复核 active world、project-relative normalized path、项目文件存在性、目录状态、有限坐标和 `AssetPreviewKind`；只接受 Model/Image/Audio/Video/Material。
- 合法 drop 创建带 `Transform` 与 `AssetReferenceComponent` 的普通场景对象，使用视口内 bounded 5-unit XY 映射；路径只保存 generic project-relative string，组件属性支持场景序列化/恢复。
- 创建前复用 `EditorDocument` checkpoint，完成后复用 `document_changed()`，所以对象创建、选择和引用路径可通过同一 Undo/Redo 文档事务回滚/重做。

### 契约与证据

- `EditorInteractionTests` 过滤 `assets/preview.obj`，模拟资源行 PointerDown、跨视口 PointerMove 和 PointerUp，断言对象数、`AssetReferenceComponent::path()`、正向 XY 放置、状态消息以及 Undo/Redo 后引用恢复。
- 同一交互回归再拖入 `assets/Folder-extra.txt`，断言未知扩展名不创建对象并报告 `not instantiable`；目录、项目外路径和无效坐标已在 `EditorLayer` 入口执行保护，专门拒绝分支测试列入下一轮。
- 交互测试还保持了资源浏览器、场景打开、构建面板、Inspector 和媒体/模型预览链路；当前 focused test 已通过：`Editor document, real filesystem, input routing, inspector, undo and simulation passed`。
- 全量验证已完成：`cmake --build out/build/mingw-debug -j 4` 成功；`ctest --test-dir out/build/mingw-debug --output-on-failure` 为 `52/52` 通过、0 失败、`8.36 sec`；中途一次全量观察出现数学并行测试异常长尾，隔离复跑 `1/1` 通过，随后完整复跑正常完成。
- 视觉检查继续使用窗口/DPI/retained-command 证据；drop cue 是 retained draw list 的 UI 状态，不把 GDI window capture 当成 GPU scene pixel 证据。

### 安全、性能与视觉审计

- 不接受绝对路径、`..`/`../` 逃逸路径、不存在资源、目录、有限性失败或未支持扩展；拒绝分支不创建对象、不写文档、不触发导入或外部进程。
- UI paint 只读取已准备的 `FileEntry`/asset index 和 retained state；实际路径验证与场景修改在 EditorLayer callback，单次 drop 至多一次 checkpoint、一次对象创建和一次文档变更。
- 资源引用不持有文件句柄、解码器、音频 voice 或 GPU 资源；因此可安全撤销，且后续 AssetSystem/AudioSystem/renderer 绑定可以独立审计。

### 失败状态与回滚路径

- 无 active scene、项目边界失败、目录/类型/坐标不合法时显示明确 `Drop rejected` 状态并保持当前 world 不变。
- 组件或 Transform 创建失败会弹出当前 checkpoint、销毁临时对象并报告 `Drop failed`；正常成功路径使用 Undo/Redo 恢复完整场景快照。

### 未解决风险与下一轮

- 当前是 Project 浏览器到 scene reference 的内部拖放，不含 OS 文件拖放、raycast/depth 落点、prefab/scene instance、稳定 AssetId 依赖追踪或运行时资源实例化。
- 下一轮先把引用升级为 manifest-backed AssetId 并增加拒绝分支断言，再接入 model scene instance；AudioSystem source/clip 绑定、视频材质和异步导入保持独立生命周期。

## 第 4.21 子阶段：Manifest-backed 资源身份与统一拖放入口

### 实现与范围

- `AssetReferenceComponent` 现在同时保存 project-relative `path` 与 `AssetSystem` manifest 的稳定 `assetId`；路径被修改时会清空旧身份，path-only 的旧场景可迁移为 `assetId=0`。
- `EditorLayer` 在 AssetSystem 已初始化且 manifest ready 时，按规范化物理源路径与资源类型匹配 manifest 条目；没有 AssetSystem 的纯编辑器测试仍支持 path-only 兼容模式。
- 新增 `create_asset_reference_at_viewport(World&, path, point)` 作为统一 ingress，Project retained 行与未来原生 OS 拖放桥接都复用同一安全检查、checkpoint 和 Undo/Redo 事务。
- 目录行保留拖拽源但不改变单击导航语义，因此目录拖到视口会进入同一拒绝分支并返回可读状态。

### 契约与证据

- `EditorInteractionTests` 计算并断言 `preview.obj` 的 manifest AssetId，确认引用对象携带该 ID，场景 JSON 含 `assetId`，Undo/Redo 后 path 与 ID 均保持一致。
- 同一测试继续覆盖未知扩展名拒绝，并通过统一 ingress 专门覆盖目录、项目外路径和非法 viewport 坐标；所有拒绝均断言 world 对象数不变。
- focused interaction test 已通过：`1/1 Test #43 ... Passed`，总计 `11.84 sec`；构建目标 `shinkou_editor_interaction_tests` 通过。
- 最新全量回归已通过：`52/52`，0 失败，总计 `39.46 sec`；其中 `shinkou_editor_interaction_tests` 通过 `11.98 sec`。
- 代码质量：`git diff --check` 通过；临时调试标记复查为零。

### 安全、性能与视觉审计

- 场景只保存字符串和整数身份，不持有文件句柄、decoder、AudioSystem voice 或 GPU resource；manifest 仅通过 AssetSystem 的 immutable scan snapshot 读取。
- drop 匹配是用户动作触发的有限快照遍历，不进入 paint、input 的逐事件 IO 路径；未 ready、根重连和 ID 缺失都拒绝并保持场景不变。
- path 仍经过 lexical/project-root 双重边界校验；manifest 条目还必须匹配当前资源类型，避免把一个源文件错误绑定成另一种 loader 类型。
- retained drop cue 仍只证明 UI 状态；GDI capture 不作为 GPU scene pixel 证据，DPI/主题/窗口证据沿用第 4.20 矩阵。

## 第 4.22 子阶段：AssetId 驱动的模型场景实例渲染

### 实现与范围

- `EditorModelSceneRenderer` 将 manifest `AssetId` 作为 GPU geometry cache key，验证 immutable model snapshot 的 vertex/index 数量、有限顶点、索引范围和矩阵后才创建或更新 persistent buffers；同一资源的多个 GameObject 共享 geometry，object buffer 按对象身份复用。
- `EditorLayer::sync_model_scene_assets` 对活动场景引用建立 AssetSystem typed model request 和异步 provider 解析，记录 generation/source stamp/path/cancel token；`poll_model_scene_assets` 只接收与当前记录完全匹配的结果。
- 通过 `RenderScene::extract` 获取编辑器相机 view-projection，场景 renderer 创建 graph-owned material 并在已有 editor viewport seam 上追加 `editor_model_scene` pass；UI list 仍在随后阶段交给 `Renderer::submit`，没有改变 UI presentation 顺序。
- `EditorLayer::shutdown(render::Renderer*)` 与 `Engine::shutdown` 连接，先等待 scene worker，再释放场景 renderer 的 geometry/object/shader 资源；无 renderer 指针的旧调用仍保持兼容。

### 契约与证据

- `EditorInteractionTests` 在实际拖入 `assets/preview.obj`、Undo/Redo 和 manifest ID round-trip 之后等待 scene cache，断言 `model_scene_loaded_asset_count()==1`；Null backend 的 `model_scene_instance_count()==0` 且状态包含 `GPU model scene fallback`，证明没有把资源准备误报成 GPU 呈现。
- `cmake --build out/build/mingw-debug -j 4` 最终全量构建通过；focused `shinkou_editor_interaction_tests` 为 `1/1` passed、`7.07 sec`；最终全量 CTest 为 `52/52` passed、0 failures、总计 `30.56 sec`。
- 本轮窗口 capture：`shinkou_ui_capture` + D3D11 参数、1280×720 logical client、tree、dark，child trace 为 `editor-ui-commands=184 editor-ui-text=41 editor-ui-assets=11 editor-ui-visible-assets=11 editor-ui-viewport=225,67,383.333,184 editor-ui-dpi=1.5`；窗口报告 `client=1280x720 dpi=144 surface-kind=GdiWindowSurface`。已检查 BMP，但因是 GDI surface 只作为窗口/布局/retained UI 证据，不作为 GPU scene 像素通过。
- `git diff --check`、临时调试标记复查和已有 preview/renderer/render-graph 测试保持通过；新源文件由现有 `SHINKOU_ENGINE_SOURCES` glob 纳入构建。

### 安全、性能与视觉审计

- 模型文件读取仍在 AssetSystem/provider worker，paint、input、RenderGraph callback 不访问项目路径；场景只保存 path + AssetId，worker 结果需同时匹配 generation/path/stamp，避免切换项目或重载后的旧快照污染当前场景。
- 上传上限为 2,000,000 vertices / 6,000,000 indices，所有 size 计算先做溢出检查；非法浮点、越界索引、无效矩阵、空资源句柄和 shader/pipeline 创建失败均只拒绝当前实例，不能扩大分配或写入 World。
- geometry 以 AssetId/revision 缓存、object buffer 以 objectId 缓存；每帧只更新小型 scene/object uniform buffer，不重复解析模型。inactive asset/object 会在 `prune` 中释放 persistent GPU buffer，避免编辑器长时间操作后的无界增长。
- 当前 pass 绑定空 editor target 且不使用 depth attachment，这是明确的阶段性限制：模型可以绘制到视口，但暂不承诺与现有场景做深度遮挡；D3D12/Vulkan/Null 返回显式能力 fallback。

### 失败状态与回滚路径

- AssetSystem 未连接、manifest 未 ready、ID 不是 model、异步请求失败、provider 解析失败或结果过期时，不创建 GPU 资源、不修改 World，只保持引用和可读状态；set_project_root/set_asset_system 会取消并 drain worker。
- shader/pipeline、buffer upload 或 graph import 失败时只跳过该实例并发布状态；场景 renderer 的 persistent cache 可由 prune/clear 回收，World 文档可通过现有 Undo/Redo 回滚。
- 若需要回退本轮，可移除 `EditorModelSceneRenderer` 的 pass 接入并保留 4.21 的 path+AssetId 引用格式；不需要迁移场景文件，也不会影响 Inspector 独立模型预览。

### 未解决风险与下一轮

- D3D11 scene pass 仍是 POSITION-only 固定预览材质，没有 depth target、PBR/glTF material、纹理、动画、节点层级、透明和多 primitive material 分组；GPU capture 在本机仍未取得 EngineGpuReadback surface。
- scene cache 目前按当前活动引用 prune，跨场景共享资源、重命名历史、依赖失效传播和磁盘导入数据库仍属于 AssetSystem 后续工作。
- 下一轮建立 `AssetId → AudioSystem clip/source` 绑定，明确 voice 生命周期、取消、unload、播放头同步和文档只存引用；随后回到模型深度/材质和 OS drag/drop adapter。

## 第 4.23 子阶段：音频资源引用与 AudioSystem 生命周期绑定

### 实现与范围

- `EditorLayer` 新增 manifest audio `AssetId → AudioAssetId` clip cache。播放前先通过 `FileSystemService::resolve_existing` 完成项目边界校验，再以 `AudioSystem::load` 获取可复用的 clip handle，并调用 `play(AudioAssetId, AudioPlayParams)`，不再让每次预览播放隐式创建未跟踪的 path asset。
- `stop_audio_preview` 会停止 voice；path-only 临时 clip 立即 unload；manifest-backed clip 由 `clear_audio_asset_bindings` 在 AssetSystem/AudioSystem 替换、项目根切换和 editor shutdown 时批量 unload。现有 cursor/seek、pause/resume、volume 和 UI bus 语义保持不变。
- 场景仍只保存 `AssetReferenceComponent` 的 path + manifest AssetId；`AudioAssetId`、voice 和 backend decoder 都是 EditorLayer 的会话态，不进入 scene JSON。

### 契约与证据

- `EditorAudioPreviewTests` 使用可观测 fake audio backend，保留 WAV provider、异步 media preview、seek、play/pause/stop 回归，并新增断言：播放创建一个 AudioSystem clip，停止后 path-only clip pool 回收到 `asset_count()==0`。
- 目标构建与 focused `shinkou_editor_audio_preview_tests` 已通过：`1/1` passed、`2.57 sec`；4.22 的最终全量构建/CTest 证据仍为 `52/52` passed、0 failures、`30.56 sec`。
- 不可用 AudioSystem 的既有 interaction 路径继续报告 `AudioSystem is not connected`，不会把音频文件存在误报成可播放；本轮没有引入新的外部依赖或 shell/网络操作。

### 安全、性能与视觉审计

- path-only 临时句柄的所有权明确归 EditorLayer；manifest cache 的所有权也集中在同一个 binding table，系统切换前先 stop/unload，避免悬空 voice 或旧系统句柄。
- `AudioSystem::load` 的路径去重和池上限继续生效；编辑器重复播放同一 manifest AssetId 复用 handle，停止释放 path-only clip，不增加每帧扫描、文件读取或解码器创建。
- 音频控制仍通过 retained media panel 的已有命中区域与 `AudioSystem` transport seam；没有改变 D3D11 UI presentation，因此 4.22 的 GDI capture 限定仍适用。

### 失败状态与回滚路径

- 音频系统未连接、资源越过项目根、clip load 或 voice creation 失败时，状态变为 stopped/unavailable，不修改 World；临时句柄在失败分支立即释放。
- 若回退本轮，可恢复 `start_audio_preview` 的 path overload，同时保留场景与 UI 数据格式；不会影响模型 scene renderer 或 Inspector media provider。

### 未解决风险与下一轮

- 目前只绑定编辑器预览 clip，不创建场景 AudioSource，不做运行时自动播放、3D spatial source、streaming prefetch 或 AssetSystem 依赖失效通知。
- 下一轮为 AudioSource 设计可序列化 path/AssetId/voice policy 并接入 world lifecycle；模型并行补 depth target、glTF material/texture 和多后端 shader 能力。

## 第 4.24 子阶段：编辑器场景 color target 到 backbuffer 的宿主呈现

### 实现与范围

- `engine/samples/EngineSample.cpp` 在 editor 分支增加 `editor_scene_present` RenderGraph pass。它导入 forward scene color target 与 present material，声明 scene/object uniform 读依赖，调用 `Renderer::bind_editor_render_target`，再用 fullscreen sprite 把 color target 送到编辑器 backbuffer。
- present pass 使用独立的 identity `SceneFrameData` / `ObjectFrameData` buffer；没有复用或覆盖 forward renderer 的场景相机 buffer，避免改变场景 pass、model scene pass 或 retained UI overlay 的坐标契约。
- 移除旧的 `editor_viewport_lifetime` 空 side-effect pass，保留顺序为 scene draw → scene present → model/editor viewport work → UI overlay；sample 结束时显式销毁新增的 transient/persistent handles。

### 契约与证据

- direct smoke：`shinkou_engine_sample.exe --frames 1 --editor dx11` 退出 `0`，输出 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=1 passes=4 draws=3`；trace 包含 `editor_scene_present draws=1`，证明 present callback 在 D3D11 native window 路径实际执行。
- 全量构建：`cmake --build out/build/mingw-debug --config Debug --parallel 4` 通过，sample 与 `shinkou_ui_capture` 均重新链接。
- 全量测试：`ctest --test-dir out/build/mingw-debug -C Debug --output-on-failure` 为 `52/52` passed、0 failures、总计 `21.11 sec`；其中 editor interaction 仍通过，未改变已有 4.20–4.23 契约。
- GPU 视觉证据：绝对输出路径下的 dark/tree `1280×720` capture 退出 `0`，报告 `mode=EngineGpuReadback surface-kind=GpuClientSurface client=1280x720 dpi=144 captured=1`，child trace 为 `commands=184 text=41 assets=11 viewport=225,67,383.333,184`；light/tree `1600×900` 也退出 `0`，报告 `mode=EngineGpuReadback surface-kind=GpuClientSurface client=1600x900 dpi=144 captured=1`，trace 为 `commands=195 text=41 assets=11 viewport=225,67,596.667,304`。dark BMP 已通过图像查看工具检查；light capture 的 GPU header/metadata 有效，但查看器拒绝该大尺寸 BMP，因此不把它额外宣称为像素人工检查通过。

### 安全、性能与视觉审计

- pass 只读取 RenderGraph 中已经存在的 color target/material/uniform，不在渲染回调中访问项目路径、AssetSystem、shell 或网络；新增两个 uniform buffer 大小固定且单帧只上传 identity 数据。
- scene camera uniform 与 present identity uniform 分离，降低“呈现 pass 改写共享 buffer”导致的跨帧/跨 pass 污染风险；pass 顺序由 sample 的明确 build 顺序与 side effect 保证。
- capture 使用 `--require-gpu` 和绝对 BMP 路径，实际获得 `GpuClientSurface`；此前相对路径导致的 GDI fallback 只保留为失败教训，不作为本轮 GPU 通过证据。

### 失败状态与回滚路径

- 非 D3D11、editor target 不可用、material/buffer 创建失败时沿用既有 backend fallback，不在 sample 中伪造 GPU 通过；场景和 UI 数据不被修改。
- 若回退本轮，只需移除 `editor_scene_present` 接入和两个 identity uniform 资源；4.22 model scene、4.23 audio binding、World 文档和 AssetId 格式均可独立保留。

### 未解决风险与下一轮

- 当前仍是 sample-level present seam，不是所有宿主都自动获得的通用 renderer contract；模型 scene pass 仍只在 D3D11 使用 POSITION-only 固定材质、无 depth/material/texture/animation。
- light 大尺寸 BMP 尚未通过人工像素查看器检查；下一轮应将 GPU capture 结果标准化为稳定的 PNG/readback artifact，并加入清屏/非零像素和 viewport 内容 oracle。
- 下一轮入口：提炼通用 editor scene presentation API，并为模型补 depth target、材质/纹理以及至少一个跨后端实现；音频继续进入可序列化 AudioSource 和运行时生命周期。

## 第 4.25 子阶段：Windows 原生文件拖放接入统一资源引用入口

### 实现与范围

- `platform::Window` 在 Windows 创建窗口后调用 `DragAcceptFiles`，在 `WM_DROPFILES` 中最多处理 64 个文件，每个路径最多 32768 个 UTF-16 字符；路径通过 `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)` 转换，消息完成后始终 `DragFinish`。
- native adapter 只把 UTF-8 absolute path 与 client-area physical pixel 交给 Engine；`EditorLayer::create_asset_reference_from_window_drop` 通过 `FileSystemService::project_relative_existing` canonicalize 并验证项目根，再按当前 DPI × 用户 UI scale 转成 logical point，最后调用原有 `create_asset_reference_at_viewport`。
- `CoordinateSpaces.h` 抽出 `WindowClientPx`/`UiLogicalPx` 和转换函数，避免为了复用坐标 API 引入 `RenderView.h` 的 `editor::World` 别名，从而修复实际编译时对 `shinkou::World` 的遮蔽风险。
- Engine 在 editor window 生命周期内安装回调，shutdown 在销毁 HWND 前清空回调；目录、项目外文件、不可用文件、未知类型和 manifest 不匹配仍由已有 editor ingress 拒绝。

### 契约与证据

- `shinkou_window_file_drop_tests` 验证 adapter 回调保留 path 与 client 坐标，并验证 `Window::destroy()` 后晚到 dispatch 不再触发回调。
- `shinkou_file_system_tests` 新增项目内绝对路径转相对路径、项目外路径拒绝、相对路径拒绝；canonical boundary 逻辑在文件存在性检查前完成，不允许通过 symlink/junction 绕出 root。
- `EditorInteractionTests` 用绝对项目内 `preview.obj` 验证 native ingress 创建对象、保留 manifest AssetId，并用 Undo 回滚；项目外 absolute path 验证对象数不变。
- 最终构建：`cmake --build out/build/mingw-debug --config Debug --parallel 4` 通过。
- 最终全量 CTest：`53/53` passed、0 failures、总计 `10.75 sec`；focused editor interaction `1/1` passed、`6.81 sec`；文件系统与 Window adapter focused `2/2` passed、`1.05 sec`。

### 安全、性能与视觉审计

- native 消息处理有文件数、路径长度和 UTF-8 有效性上限；不读取文件内容、不启动外部进程、不创建 decoder/GPU handle，均由编辑器后续异步/类型链路处理。
- canonical absolute path 与 canonical project root 比较后才生成 project-relative path；路径包含外部 junction/symlink 目标时拒绝，场景只通过既有 checkpoint 保存 path + AssetId。
- 每个 drop 只做一次坐标转换和一次统一 ingress；窗口过程不直接触碰 World，UI paint 不新增 IO。实际 Explorer → HWND 的手工拖放尚未在自动化环境执行，这一点保持为未验证项。
- `WM_DROPFILES` 接入不改变 4.24 的 GPU scene presentation；既有 dark/light capture 继续提供 `GpuClientSurface`/DPI 144 证据，但不冒充本轮 OS 手工拖放视觉通过。

### 失败状态与回滚路径

- 绝对路径不存在、canonicalization 失败、超出项目根、目录、未知扩展、manifest 未 ready 或 viewport 坐标非法时，返回明确状态且不创建对象、不写场景；`DragFinish` 仍执行。
- Window 关闭/销毁时取消 native drop acceptance、清除 callback；如果消息在编辑器已 teardown 后到达，dispatch 不再调用已释放宿主。
- 若回退本轮，可移除 Window 的 `WM_DROPFILES` 分支与 Engine wiring，保留 `CoordinateSpaces` 和 FileSystem API；4.21 retained drag/drop、AssetId、模型/音频链路不受影响。

### 未解决风险与下一轮

- 当前只接受“已位于项目根内”的外部文件，尚未提供 Unity 式“拖入即复制/导入”事务、导入队列、重名策略、进度和依赖刷新；这需要单独的可恢复文件复制设计。
- 自动化测试覆盖了 native adapter 的 dispatch seam，但没有真实 Explorer OLE/WM_DROPFILES 手工 capture；下一轮补 Windows UI QA，并把拖放后的 status/scene count 纳入 capture trace。
- 下一轮入口：将 4.24 sample-level scene present 提炼为通用 renderer contract，同时设计可序列化 AudioSource 的 clip/voice policy；模型继续补 depth/material/texture 与跨后端能力。

## 第 4.26 子阶段：可序列化 AudioSource 与场景音频生命周期桥接

### 实现与范围

- `AudioSourceComponent` 只保存 `clipPath`、`assetId` 和播放策略字段；World 注册该类型后，通用反射和 `EditorDocument` 自动覆盖 scene JSON capture/restore。运行时句柄集中由 `AudioSceneSystem` 持有，不进入 World。
- `AudioSceneSystem` 将规范化项目相对路径映射到共享 clip binding，并为每个 GameObject 管理独立 voice binding；它处理 play-on-start、禁用/激活、路径或播放策略变化、手动 play/stop、对象删除、播放结束和 shutdown 的 stop/unload 顺序。
- Engine 在 `AudioSystem::update` 后同步场景，在 `AudioSystem::shutdown` 前销毁场景绑定；未提供 audio asset root 时回退到 editor/assets project root，避免相对路径在 Engine 配置下失配。
- 明确非目标：`assetId` 尚未向 AssetSystem manifest 做反向确认；listener/3D 衰减、streaming 预取、专用 Inspector UX 和真实音频解码/播放设备能力继续单独立项。

### 契约与证据

- `shinkou_audio_scene_system_tests` 使用 fake backend 验证一个场景源自动播放、同路径双对象只注册一个 clip、禁用与路径变更的 refcount、结束 voice 后 clip 回收、手动播放/停止、对象删除、shutdown 和 `AudioSource` JSON 字段。
- 构建：`cmake --build out/build/mingw-debug --config Debug --parallel 4` 通过。
- 专项：`ctest --test-dir out/build/mingw-debug -R shinkou_audio_scene_system_tests --output-on-failure` 为 `1/1` passed、总计 `1.20 sec`。
- 全量：`ctest --test-dir out/build/mingw-debug --output-on-failure` 为 `54/54` passed、0 failures、总计 `40.10 sec`。
- Engine smoke：`shinkou_engine_sample.exe --frames 1 --editor dx11` 退出 `0`，`device-ready=1`、`native-ui=1`、`viewport-scissor=1`、`passes=4`、`draws=3`，trace 包含 `editor_scene_present`；该证据验证 Engine 生命周期接线，但不代表真实音频文件已在 sample 中播放。

### 安全、性能与视觉审计

- path 输入为空、绝对路径或含项目外上跳段时不创建 source；bus、volume、pitch 在进入 AudioSystem 前有界化。此轮没有新增文件复制、文件内容读取、shell、网络或第三方依赖。
- clip table 以规范化路径去重并以引用计数回收；voice 和 clip 是会话态，World 不会持有悬空后端对象。播放结束后同步检查 Invalid/Stopped/Finished，释放已失效 voice 的 clip。
- 同步只遍历 World 的 AudioSource 组件并更新小型参数；空间 voice 只提交变换/播放参数，不在 UI paint 或 render pass 中做 IO。没有改变 retained UI 像素呈现，因此继续沿用 4.24 GPU capture 证据和限制。

### 失败状态与回滚路径

- audio backend 未初始化、clip 无法注册、voice 创建失败或路径非法时，`AudioSceneDiagnostics` 记录失败/错误并保持 World 不变；失败分支会释放已经取得的 clip binding。
- 禁用、删除、路径切换、外部停止、自然结束和 Engine shutdown 都有明确的 stop/unload 清理路径；回退时可移除 AudioSceneSystem 的 Engine 接线与 AudioSource 注册，不影响既有编辑器媒体预览、模型预览和 AssetId 场景引用格式。

### 未解决风险与下一轮

- `assetId` 当前是可序列化身份字段，但 AudioSceneSystem 仍按安全 path 载入，尚未阻止 path 与 manifest 身份不一致；下一轮接入 AssetSystem manifest 校验、重命名/失效通知和导入错误状态。
- AudioSource 目前可由通用属性反射编辑，尚无专用的 clip picker、bus 下拉、preview transport、listener/3D 可视化和 streaming 进度；这些需要先定义 editor/runtime 双向事务。
- 本轮没有新增视觉像素变化；继续把 4.24 的 GPU readback/scene presentation 作为 UI 宿主证据，同时规划模型 depth/material/texture 与跨后端渲染审计。

## 第 4.27 子阶段：AudioSource manifest 身份校验与失效诊断

### 实现与范围

- AssetSystem manifest 增加 ready/id 查询契约。扫描过程中 ready 变为 false，完成扫描后恢复；挂载变更和 shutdown 清空 manifest cache，避免旧项目/旧 mount 的身份继续被接受。
- AudioSceneSystem 在 `assetId != 0` 时校验当前 manifest 条目的 `audio` 类型、项目内普通源文件以及 canonical source path；校验失败不会获得 AudioSystem clip 或 voice。`assetId == 0` 的 path-only AudioSource 仍可运行，保持旧场景兼容。
- pending 与 invalid 分离：扫描未完成、AssetSystem 尚未连接或查询锁暂不可用时保持 pending 并允许下一帧重试；未知身份、错误资源类型、缺失文件和 path/ID 不一致记为失败并保留明确 `lastError`。
- Engine 接线由 `audioScene_.sync(world_, audio_, &assets_)` 完成，AudioSource binding 把 AssetId 变化视为配置变化；World/scene JSON 仍不包含 AudioAssetId、AudioVoiceId、decoder 或后端指针。

### 契约与证据

- `shinkou_audio_scene_system_tests` 创建真实临时项目 manifest，验证 loop/other 两个 audio identity、同路径 clip 共享、manifest pending 后重试、禁用/重新激活、播放结束回收、错误 identity 拒绝、手动播放/停止、对象删除和 shutdown。
- `shinkou_assets_tests` 验证 scan 后 `manifest_ready()`、按 ID 查询、seed cache 查询，以及 shutdown 后 ready 被撤销。
- 构建：`cmake --build out/build/mingw-debug --config Debug --parallel 4` 通过；为修复中断遗留的零字节 sample，额外执行 `cmake --build out/build/mingw-debug --config Debug --target shinkou_engine_sample --clean-first --parallel 4` 并成功重新链接。
- focused：`ctest --test-dir out/build/mingw-debug -R "shinkou_(audio_scene_system|assets)_tests" --output-on-failure` 为 `2/2` passed、总计 `2.29 sec`。
- 全量：`ctest --test-dir out/build/mingw-debug --output-on-failure` 为 `54/54` passed、0 failures、总计 `19.45 sec`。
- Engine smoke：当前非零 sample 的单帧 `--frames 1 --editor dx11` 退出 `0`，报告 `device-ready=1`、`native-ui=1`、`viewport-scissor=1`、`passes=4`、`draws=3`；短多帧 `--frames 10 --editor dx11` 退出 `0`，报告 `frames=10`、`passes=40`、`draws=30`，UI trace 为 `commands=184 text=41 assets=11 visible-assets=11`，并包含 `editor_scene_present`。

### 安全、性能与视觉审计

- manifest 查询只暴露 immutable snapshot 中复制出的 `AssetManifestEntry`，不暴露内部容器；音频同步不取得扫描锁，避免长时间磁盘扫描拖入 Engine tick。扫描和挂载变化边界明确撤销 ready，降低 stale identity 风险。
- 校验使用 AssetSystem 的项目根解析和 filesystem canonical path；只在播放启动边界确认普通文件，拒绝绝对/上跳的 AudioSource path、非 audio 条目和 path/ID 错配。此轮无 shell、网络、外部进程或新增第三方依赖。
- 音频 clip table 仍按规范化 path 去重并引用计数回收；voice 结束后释放 clip。未改变 retained UI/render pass，DX11 sample 证据只证明当前 UI host/lifecycle 正常，不宣称真实音频设备像素或播放质量。

### 失败状态与回滚路径

- AssetSystem 未连接、manifest 扫描未完成、未知 AssetId、错误类型、文件缺失和 canonical path mismatch 都不会修改 World；pending 会重试，invalid 会在 diagnostics 中明确报告。
- 如果 AssetSystem 扫描或挂载更新发生在旧 source 运行期间，本轮通过下一次 source 重新同步前的 ready 边界阻止新绑定使用已撤销 manifest；已有 voice 的主动失效事件仍是下一轮工作。
- 回滚可移除 AudioSceneSystem 的 AssetSystem 参数、manifest 查询 API 和 Engine pointer wiring，恢复 4.26 的 path-based scene audio；已有 `AudioSource` JSON 字段仍可读取。

### 未解决风险与下一轮

- 当前 manifest 变化还没有通过 AssetEvent/专用 revision 事件主动通知正在播放的 voice；source 已有句柄可能继续运行到下一次显式重同步，下一轮补失效事件和可观测状态。
- AudioSource 仍依赖通用属性编辑，没有 clip picker、总线下拉、播放预览按钮、错误行和 AssetId/path 联动清除；需要在 UI 事务层实现并覆盖 Undo/Redo。
- `find_manifest` 是当前 cache 的按 ID 查询，不是持久化数据库或导入索引；重命名、复制/导入、依赖图和多 mount 身份策略继续列入后续资源系统阶段。

## 第 4.28 子阶段：manifest revision 驱动的 AudioSource 主动失效

### 实现与范围

- AssetSystem 增加原子 `manifest_revision()`。扫描开始、挂载变化、seed 和 shutdown 都推进 revision；manifest ready 仍由 immutable snapshot 发布控制，AudioSceneSystem 不读取扫描锁保护的内部容器。
- AudioSceneSystem 的 SourceBinding 保存绑定时 revision；当带 `assetId` 的源检测到 revision 变化时，先停止并释放旧 voice/clip，再按当前快照重试。删除或错配条目不会创建 voice，恢复资源并重新扫描后 `playOnStart` 可恢复。
- 新增 `AudioSceneDiagnostics::invalidatedSources`，用于把 manifest 失效重绑定与普通 `failedSources` 区分；World、场景 JSON 和 retained UI 没有引入运行时句柄或 raw payload。
- 非目标：没有新增跨线程 listener，没有做重命名迁移/依赖图事务，没有修改 UI 像素；AudioSource 专用 Inspector 控件和 Undo/Redo 属于下一轮。

### 契约与证据

- `shinkou_assets_tests` 验证 scan 后 revision 推进、seed 后再次推进、shutdown 后 ready 撤销且 revision 改变。
- `shinkou_audio_scene_system_tests` 验证播放中的 manifest refresh 会主动停播并重绑；删除已绑定音频后进入 invalid、不创建 voice；恢复并重新 scan 后重新播放；同时保留共享 clip、pending→ready、播放结束回收、手动 play/stop、对象删除、shutdown 和 JSON 契约。
- 构建：`cmake --build out/build/mingw-debug --config Debug --parallel 4` 通过。
- focused：`ctest --test-dir out/build/mingw-debug -R "shinkou_(audio_scene_system|assets)_tests" --output-on-failure` 为 `2/2` passed、总计 `0.78 sec`。
- 全量：`ctest --test-dir out/build/mingw-debug --output-on-failure` 为 `54/54` passed、0 failures、总计 `42.64 sec`。
- Engine smoke：`out/build/mingw-debug/shinkou_engine_sample.exe --frames 10 --editor dx11` 退出 `0`，报告 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=10 passes=40 draws=30`，UI trace 为 `commands=184 text=41 assets=11 visible-assets=11`，render graph 含 `editor_scene_present`。

### 安全、性能与视觉审计

- revision 使用原子读写，source binding 只比较整数版本，不在 Engine tick 等待 manifest scan mutex；长时间扫描期间 ready=false，新的 ID 绑定保持 pending。
- 失效顺序固定为 stop voice → release clip → 使用当前 snapshot 校验；path/ID 错配、删除文件、错误类型和未知 ID 均不会触碰 World 或创建后端 voice。
- 本轮没有新增文件操作、进程启动、网络访问或第三方依赖；没有修改 retained draw list。DX11 sample 证明 editor lifecycle、UI host、scene presentation 和 render graph 正常，不宣称音频设备质量。

### 失败状态与回滚路径

- manifest revision 变化但新快照未就绪：源进入 pending，下次 sync 重试；新快照缺失或 path mismatch：源进入 invalid/failed diagnostics，不保留旧 voice。
- 回滚可移除 revision 字段及 `manifestChanged` 分支，恢复 4.27 的 ID 校验与显式 sync 失效行为；公共 `AudioSource` JSON 契约不变。

### 未解决风险与下一轮

- revision 是 polling 观察点而非跨线程 AssetEvent callback；如果未来需要后台即时通知，应设计有界事件/队列和主线程消费边界，避免在 worker 回调触碰 AudioSystem。
- AudioSource 仍缺少专用 clip picker、bus 下拉、错误态和播放预览按钮；下一轮实现 editor command/undo 事务，让 clipPath 与 assetId 联动并在 Inspector 呈现 pending/invalid。
- manifest 仍是当前 scan cache，不是持久化导入数据库；重命名迁移、复制导入、依赖图和多 mount 身份策略继续后置。

## 第 4.29 子阶段：AudioSource 专用 Inspector 资源选择与总线控制

### 实现与范围

- `EditorUiModel` 为 AudioSource 生成专用资源状态字段，隐藏原始 `assetId`，并为 `clipPath`/`bus` 发布 immutable choice snapshot。音频 choice 最多 256 项，显示值统一为项目相对路径；总线选项为 Master、Music、SFX、Voice、Ambient、UI。
- `EditorUi` 使用 retained button/popup 绘制选择器：popup 最大高度 220 logical px，长列表可滚动，支持 Escape 与外部点击关闭，选择项只为当前可见行注册交互区域。
- `EditorLayer` 在 manifest 完成/失败/重置时同步刷新 choice snapshot；选择 clip 通过现有 checkpoint/undo 事务写回 UTF-8 规范化项目相对 `clipPath`，并自动绑定对应 manifest `AssetId`。媒体音频预览也按实际源文件路径反查 manifest identity。
- `AudioSource` 资源状态置于组件字段顶部，默认 Inspector 视口即可看到 Ready、path-only、identity unavailable 和 mismatch 等状态；不在 World 中增加运行时句柄。

### 契约与证据

- `shinkou_editor_interaction_tests.exe` 通过：选择 `assets/preview.wav` 后 `clipPath` 与预期 AssetId 同步，`bus` 选择 Master 提交，原始 `assetId` 不再暴露，状态文本包含 `Ready · assets/preview.wav`，既有 name/transform 编辑和 Undo/Redo 仍通过。
- 构建：`cmake --build out/build/mingw-debug -j 2` 通过，包含 sample、全部测试和 benchmark targets。
- 全量：`ctest --test-dir out/build/mingw-debug --output-on-failure` 为 `55/55` passed、0 failures、总计 `44.99 sec`。
- Engine smoke：`out/build/mingw-debug/shinkou_engine_sample.exe dx11 --frames 10` 退出 `0`，输出 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=10 passes=40 draws=30`；UI trace 为 `editor-ui-commands=184 editor-ui-text=41 editor-ui-assets=11 editor-ui-visible-assets=11`，render graph 含 `editor_scene_present`。

### 安全、性能与视觉审计

- choice 数据只由已完成扫描的 immutable manifest 转换而来，并限制为最多 256 项；popup 行和文字来自受控 URI/路径，不执行 shell、网络、进程或未授权文件读取。
- `project_relative_existing()` 先进行 canonical boundary check；AudioSource 写回只使用项目相对 UTF-8 路径，`assetId` 通过同一 choice snapshot 绑定，避免虚拟 `project://` URI 与运行时 path contract 不一致。
- 选择器不会在 paint 中重建 manifest；总线 choice 为静态共享快照，popup bounded/clipped，滚动状态纳入 paint key。现有 checkpoint 保证选择失败不会污染 World，且可撤销。
- 视觉遵循 Shinkou UI design 的 Windows-first flat surface/border/accent 语言；按钮有文字下拉指示，popup 有边界、焦点/hover 状态和 DPI-safe logical coordinates。D3D11 smoke 证明宿主与渲染图链路正常，但不代替真实音频设备质量测试。

### 失败状态与回滚路径

- manifest 未连接、正在扫描、扫描失败、资源超出项目根或资源缺失时，choice 会清空或显示明确状态；已有 path-only AudioSource 不被破坏，带 ID 的 mismatch 仍可诊断。
- 选择器回调失败、无效 field 或未知 choice 不修改 World；成功编辑沿用 EditorLayer 的 checkpoint/undo 栈。回滚可移除 choice metadata、popup 绘制和 AudioSource 特化分支，恢复通用文本输入。

### 未解决风险与下一轮

- 当前 picker 选择后只负责配置联动，不提供 Inspector 内播放/暂停 transport、listener 可视化、3D 衰减曲线或 streaming 进度；下一轮进入 AudioSource playback/空间音频控制。
- 当前 manifest 转换以现有项目 root 为边界，多 mount 显示策略、重命名迁移、导入复制和依赖图仍需资源数据库阶段定义。
- 当前样例 smoke 仍是 retained command/render graph 证据；真实 Windows Explorer 拖放、音频设备输出和模型材质 GPU 预览继续作为平台/媒体专项 QA。

## 第 4.30 子阶段：AudioSource 场景播放传输与 Inspector 控制

### 实现与范围

- `AudioSceneSystem` 增加对象级 `pause`、`resume` 和 `transport_state`；场景音频的运行时 voice 仍只由 bridge 持有，World/scene JSON 不增加句柄字段。
- `Engine` 将 `AudioSceneSystem` 注入 `EditorLayer`。AudioSource Inspector 在组件顶部显示 transport 状态，并提供 Play、Pause、Stop 三个 retained controls；Play 对 Paused 状态走 Resume，其它状态按当前资源身份重新启动。
- 控件命令使用 `audio-source:<ObjectId>` target，与 Media 面板预览 transport 分离；运行时播放/暂停/停止不进入 Undo，因为它们改变的是会话态 voice 而非场景配置。
- 视觉实现遵循本轮 UI design 约束：复用现有行高和按钮层级，使用文字标签而非平台/emoji 图标，维持 focus/pressed/selected 的 retained 状态。

### 契约与证据

- `shinkou_audio_scene_system_tests` 通过：transport 状态从 Playing → Paused → Playing → Stopped，Stop 后 clip/voice 资源回收；现有 manifest identity、revision invalidation、共享 clip、pending/invalid 和 JSON 契约保持通过。
- `shinkou_editor_interaction_tests` 通过：fake AudioBackend 由 Inspector 点击真实驱动 AudioSceneSystem，Play/Pause/Resume/Stop 均验证对象级 scene voice 状态；Project Media 预览通道仍独立。
- 构建：`cmake --build out/build/mingw-debug -j 2` 通过，sample、测试和 benchmark targets 均完成链接。
- focused：`ctest --test-dir out/build/mingw-debug -R "shinkou_(audio_scene_system|editor_interaction)_tests" --output-on-failure` 为 `2/2` passed、总计 `14.87 sec`。
- 全量：`ctest --test-dir out/build/mingw-debug --output-on-failure` 为 `55/55` passed、0 failures、总计 `48.61 sec`。
- Engine smoke：`out/build/mingw-debug/shinkou_engine_sample.exe dx11 --frames 10` 退出 `0`，输出 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=10 passes=40 draws=30`；UI trace 为 `editor-ui-commands=184 editor-ui-text=41 editor-ui-assets=11 editor-ui-visible-assets=11`，render graph 含 `editor_scene_present`。

### 安全、性能与视觉审计

- EditorLayer 只接受 `audio-source:<ObjectId>` 有界 target，并在 World 中确认对象与 AudioSource 组件存在；未连接 subsystem、非法 target 和不满足状态转换的操作只产生诊断，不创建新句柄。
- Play/Pause/Resume/Stop 通过 AudioSceneSystem 主线程 API 执行；没有在 paint/input 路径扫描文件、解码媒体、访问网络、启动 shell 或持久化运行时句柄。
- transport state 是 bounded presentation string；Inspector controls 复用 retained command map，列表/按钮不引入每帧 manifest rebuild；D3D11 smoke 的 UI/scene pass 链路保持稳定。
- 视觉审计覆盖 Inspector 默认视口内状态可见、三按钮固定同一行、dark theme/DPI 1.5 的逻辑坐标；按钮使用 flat surface/border/accent 语言，非平台依赖符号。

### 失败状态与回滚路径

- AudioSystem 未初始化显示 Unavailable；对象不存在、组件禁用或资源身份不可用时 Play 返回失败并保留 manifest 诊断；Pause/Resume 只接受对应 voice 状态，Stop 释放 scene binding。
- 回滚路径是移除 EditorLayer 的 AudioSceneSystem 注入、transport state field 和 target 分发，保留 4.29 的资源选择/AssetId 联动，不影响 Media 面板预览。

### 未解决风险与下一轮

- transport 当前没有 seek/cursor 或真实时长显示；AudioSystem 已有 cursor API，但需要定义 backend capability、时间轴和场景 voice 的一致语义后再开放。
- Inspector 还没有 listener、min/max distance、rolloff、velocity 等空间音频参数；下一轮先定义序列化组件字段与 AudioScene/AudioSystem 的参数边界，再做空间调试可视化。
- CTest 与 sample 仍不能替代真实 Windows 音频设备、Explorer 拖放和 GPU 模型材质质量 QA；这些继续保留为平台专项入口。

## 第 4.31 子阶段：AudioSource 空间参数与 Camera Listener 绑定

### 实现与范围

- `AudioSourceComponent` 增加可序列化 `minDistance`、`maxDistance`、`rolloff`；AudioSceneSystem 将它们映射到 `AudioPlayParams`，并把空间配置变化纳入 source binding 重启条件。
- AudioSceneSystem 每次同步从首个 active/visible 渲染 Camera 读取位置和旋转，推导 forward/up 并提交给 `AudioSystem::set_listener`；无有效 Camera 时提交默认 Listener，diagnostics 暴露 `listenerBound` 与 `listenerObject`。
- Inspector 沿用通用数值编辑器暴露三个空间字段；运行时对 NaN/无穷、负值、过大距离和 max 小于 min 做边界归一化。World/scene JSON 仍只保存配置意图，不保存 voice、backend 或 decoder 句柄。
- 非目标：没有新增 listener gizmo、衰减曲线、多 Listener 策略、velocity 编辑、seek/cursor 或 streaming 进度；这些继续按计划拆分。

### 契约与证据

- `shinkou_audio_scene_system_tests` fake backend 捕获首个 Camera 的 Listener 姿态、AudioSource 的 play/set_spatial 距离参数，验证 Camera 禁用后的默认 Listener、NaN/反向距离归一化和空间配置重绑。
- `shinkou_editor_interaction_tests` 验证 Inspector 中 min/max distance 与 rolloff 字段可见且可提交，同时保留 4.29 picker、4.30 scene transport 和既有 Undo/Redo。
- 构建：`cmake --build out/build/mingw-debug -j 2` 通过；最终代码 focused CTest `2/2` passed、总计 `15.41 sec`；全量 CTest `55/55` passed、0 failures、总计 `26.52 sec`。
- Engine smoke：最终重链后的 `out/build/mingw-debug/shinkou_engine_sample.exe dx11 --frames 10` 退出 `0`，报告 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=10 passes=40 draws=30`；render graph 含 `editor_scene_present`，UI trace 为 `editor-ui-commands=184 editor-ui-text=41 editor-ui-assets=11 editor-ui-visible-assets=11 editor-ui-dpi=1.5`。现有并行 Physics/CMake 改动保持未纳入本轮提交。

### 安全、性能与视觉审计

- Listener 只读取 World 已有 ECS Camera/Transform，不做文件 IO、进程启动、网络访问或 manifest 锁等待；每帧提交固定大小 `AudioListener`，不建立临时容器。
- 空间数值由序列化属性范围和 AudioSceneSystem 双层边界保护；maxDistance 始终不小于 minDistance，backend 不接收非有限值。
- Inspector 复用 Windows-first flat surface、既有行高和 logical DPI 坐标；没有新增图标依赖，窄面板通过既有滚动/裁剪保持可读。
- 4.30 的对象级 transport target 与本轮空间参数分离：transport 改变 session voice，空间字段改变 scene configuration 并由现有 editor property transaction 处理。

### 失败状态与回滚路径

- 没有 active/visible Camera 时使用稳定默认 Listener 并报告未绑定；Camera/Transform 数据异常时仍回退 forward/up 默认向量。
- 空间字段编辑失败不会修改 World；运行时非法值只在 AudioPlayParams 边界归一化，不创建额外资源。
- 回滚可移除新增字段、binding 比较与 listener sync，保留 4.30 transport、4.29 picker 和既有 AudioSource path/ID 契约。

### 未解决风险与下一轮

- 当前 Listener 选择是首个 active Camera，尚未有显式主 Listener/多相机优先级，也未计算 listener velocity；后续应先定义可序列化/运行时选择契约。
- 当前距离参数已进入 backend，但没有 Inspector 衰减曲线和空间调试 gizmo；模型深度/material/texture GPU 预览与资源 rename/import migration 仍是独立阶段。
- 下一轮进入 cursor/seek capability 与场景 voice 时间轴，随后推进 streaming 策略和真实设备/媒体平台 QA。

## 第 4.32 子阶段：AudioSource 场景 voice cursor/seek 时间轴

### 实现与范围

- AudioSceneSystem 增加 cursor capability、cursor readback 和有界 absolute seek；只操作其私有 scene voice，不触碰 Media Preview voice。
- EditorLayer 将 `audio-source:<ObjectId>:relative:<delta>` 与 Media Preview 的归一化 seek target 分流；Inspector Timeline 行使用 `-5 s` / `+5 s` 语义按钮，显示真实 cursor 秒数或明确的 Unavailable。
- Scene seek 的 delta 限制为 ±3600 秒，absolute cursor 限制为 7 天；AudioSystem/backend 不支持 cursor、voice 未绑定、voice 已停止或目标非法时只报告失败，不伪造 wall-clock 时间。
- Timeline 放在 AudioSource 属性组末尾，保留已有 clip/bus/spatial 字段的默认可见和命中位置；World/scene JSON 不新增运行时 cursor 或 voice 句柄。

### 契约与证据

- `shinkou_audio_scene_system_tests` fake backend 验证 Playing voice cursor 初值、absolute seek 到 12.5 秒、负数拒绝和现有 transport/资源回收契约。
- `shinkou_editor_interaction_tests` 验证 Inspector 真实点击 Play 后滚动到 Timeline，点击 `+5 s` 驱动 scene voice cursor 到 5 秒，再验证 Pause/Resume/Stop、空间字段、picker、状态和 Undo/Redo。
- 最终代码 focused CTest `2/2` passed、总计 `11.53 sec`；全量构建 `cmake --build out/build/mingw-debug -j 2` 通过；全量 CTest `55/55` passed、0 failures、总计 `36.88 sec`。
- 最终重链 sample `out/build/mingw-debug/shinkou_engine_sample.exe dx11 --frames 10` 退出 `0`，报告 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=10 passes=40 draws=30`；render graph 含 `editor_scene_present`，UI trace 为 `editor-ui-commands=184 editor-ui-text=41 editor-ui-assets=11 editor-ui-visible-assets=11 editor-ui-dpi=1.5`。

### 安全、性能与视觉审计

- seek 命令只接受严格数字 ObjectId、`relative:` 前缀和有限 delta；不将 UI 字符串交给 shell、文件系统或媒体 decoder。
- cursor 读取只调用 AudioSystem 已绑定 voice 的 capability/state/cursor API；没有每帧文件 IO、临时资源池、跨线程回调或 manifest 扫描。
- Timeline 复用 Inspector 行高、扁平按钮、focus/pressed/accent 状态和 logical DPI；新增行位于属性组末尾，避免破坏既有资源选择工作流。
- UI integration 仍沿 `Engine::tick → EditorLayer → retained UiRenderList → Renderer::submit → D3D11 editor UI` 路径，scene seek 不改变 backend presentation contract。

### 失败状态与回滚路径

- backend capability false、AudioSystem 未初始化、目标不存在、voice 未播放/暂停或 delta 越界均显示明确状态；Media Preview 不受影响。
- seek 没有持久化场景写入，不进入 Undo；spatial/listener 和 transport 仍由先前阶段保持。
- 回滚只需删除 cursor fields、Timeline renderer、scene seek dispatch 与 AudioScene cursor API，不影响 AudioSource clip identity、空间参数或 Media cursor/seek。

### 未解决风险与下一轮

- 当前场景 AudioSource 没有 duration contract，seek 不显示总时长，也没有到文件末尾的跨 backend 一致语义；后续需要把 duration/format/streaming capability 纳入 AudioAsset 描述。
- 目前只提供离散 ±5 秒定位，不是连续波形时间轴；连续播放/streaming prefetch 和性能测量属于下一阶段。
- 资源 rename/import migration、derived cache 和模型完整 PBR/动画仍未完成，不能宣称最终 Unity 级能力已完成。

## 第 4.33 子阶段：AudioAsset duration/capability contract 与场景时间轴边界

### 实现与范围

- `AudioAssetInfo` 将 `streaming`、`durationKnown`、`durationSeconds` 和 `seekable` 作为后端能力快照；`IAudioBackend::inspect_asset` 默认保守返回 unknown，`AudioSystem::AssetSlot` 缓存 metadata，并在 unload/shutdown 时清零。
- Miniaudio 对文件 decoder 查询 PCM format/length，并报告可证明的 duration 与 file-backed seek seam；Null/fake backend 不凭空声明时长。duration 查询失败不会阻止 asset 注册，后续 UI 仍明确显示 unavailable。
- `AudioSceneSystem::SourceBinding` 缓存 asset info；已知 duration 时 cursor/seek 的上限为文件末尾，未知 duration 时保留 7 天 transport 安全上限。stop、play failure 和 binding 重建会清掉缓存，避免 stale metadata。
- Inspector Timeline 仍是末尾的 retained row；cursor 可用且时长已知时显示 `current / duration s`，本轮仍只提供 `-5 s` / `+5 s` 离散控制，不宣称连续 waveform、streaming prefetch 或音视频同步。

### 契约与证据

- `shinkou_audio_tests` 增加 metadata cache 断言：30 秒、seekable 资源可读，streaming 意图保持 descriptor 语义。
- `shinkou_audio_scene_system_tests` 通过：30 秒 scene source 的 duration 可读，seek 到 60 秒被夹到 30 秒；现有 manifest identity、共享 clip、listener/spatial、transport 和资源回收契约保持通过。
- `shinkou_editor_interaction_tests` 通过：真实 Inspector Play 后点击 `+5 s`，scene cursor 到 5 秒，Timeline 文本显示 `5.00 / 30.00 s`，picker、空间字段、Pause/Resume/Stop 与 Undo/Redo 保持通过。
- 当前聚焦 CTest：`3/3` passed（AudioSystem、AudioSceneSystem、EditorInteraction），总计 `15.53 sec`；增量全目标构建通过。最终全量 CTest `55/55` passed、0 failures、总计 `41.15 sec`。
- 最终 D3D11 sample：`out/build/mingw-debug/shinkou_engine_sample.exe dx11 --frames 10` 退出码 `0`，报告 `device-ready=1 bindless=0 native-ui=1 viewport-scissor=1 frames=10 passes=40 draws=30`；UI trace 为 `editor-ui-commands=184 editor-ui-text=41 editor-ui-assets=11 editor-ui-visible-assets=11`，render graph 含 `editor_scene_present` 与 `ui_overlay_present`。

### 安全、性能与视觉审计

- metadata 只在唯一 asset load 时检查；paint、cursor readback 和 retained draw 不做文件 IO、网络、进程启动或 shell 解析。AudioSystem 的已有 path resolution 和 AssetSystem project-relative identity 校验仍是资源边界。
- duration 只有在 finite 且大于 0 时才标记 known；非法值归一化为 unknown。scene seek 仍拒绝非有限/负数输入，已知文件末尾优先于通用 7 天上限。
- Miniaudio 某些格式的 length 查询可能触发 decoder 扫描，当前为每个 unique asset 一次而非每帧；这是真实的性能风险，下一轮迁移到异步 derived metadata cache，并在 cache hit/miss 上做测量。
- 视觉继续沿用 Windows-first flat Inspector 行、语义文本和 DPI-safe logical coordinates；能力不足时显示 Unavailable，而不是绘制没有依据的进度比例。UI integration 仍沿 `Engine::tick → EditorLayer → retained UiRenderList → Renderer::submit → backend` 链路。

### 失败状态与回滚路径

- decoder 打开失败、duration unknown、backend 没有 inspect 或 seek seam 时，资源仍可注册/播放，但 Timeline 不提供虚假总时长，scene cursor 仅按 capability 和安全上限工作。
- scene voice 停止、播放失败、manifest revision invalidation 和 unload 都清除 binding metadata；不会把旧 duration 带到新 path/AssetId。
- 回滚可移除 `AudioAssetInfo`、backend inspect、scene duration clamp 和 Timeline value 扩展，回到 4.32 的 cursor-only presentation，不影响 AudioSource path/AssetId、空间 listener 或 Media Preview voice 分层。

### 未解决风险与下一轮

- Miniaudio metadata inspection 仍同步发生在 asset load 边界，超大 MP3/复杂格式可能造成加载尖峰；下一轮做异步 metadata/derived cache 与可观测耗时。
- 当前时间轴依旧是离散按钮，没有连续 slider、波形、播放头刷新策略或 streaming buffer 状态；这些需要先定义帧预算和缓存失效契约。
- rename/import migration、derived artifact identity、完整 PBR/material/texture/depth/animation model preview 仍未完成，项目尚不能宣称 Unity 级完整能力。

## 第 4.34 子阶段：AudioSource 连续 scrubber 与严格 absolute seek

### 实现与范围

- AudioSource Inspector Timeline 行增加 retained scrubber：duration 已知且 cursor 可用时绘制进度填充和 marker；能力不足时显示 `Duration unavailable` 或 `Timeline unavailable`，不伪造时间轴比例。
- scrubber 使用独立 `audio-timeline:<ObjectId>` region；点击位置生成 `audio-source:<ObjectId>:absolute:<seconds>`，与已有 `relative:<delta>` 共用 AudioSceneSystem，EditorLayer 对两种 target 做严格 finite/范围/对象校验。
- 时间轴行扩展为 76 logical px，track 与 `-5 s`/`+5 s` 辅助按钮分层绘制；该行位于 AudioSource 属性组末尾，既有 clip/bus/spatial/transport 字段命中位置不变。
- absolute seek 仍是 session voice 操作，不写 scene JSON、不进入 Undo；paint/input 不读取文件、不解码、不启动进程，Media Preview voice 继续与 Scene Audio 分离。

### 契约与证据

- `shinkou_editor_interaction_tests` 真实点击 scrubber 中点：fake scene voice 从 5 秒定位到 15 秒，并显示 `15.00 / 30.00 s`；既有 picker、空间参数、Pause/Resume/Stop、Undo/Redo 保持通过。
- `shinkou_audio_scene_system_tests` 与 `shinkou_ui_render_tests` 通过，duration clamp、scene transport 和 retained render contract 未回归；focused CTest 为 `3/3` passed、总计 `16.25 sec`。最终补充 `absolute:nan` 输入后，`shinkou_editor_interaction_tests` 单测仍通过（`13.37 sec`）。
- `cmake --build out/build/mingw-debug -j 2` 增量全目标构建通过。
- 最终全量 CTest：`55/55` passed、0 failures、总计 `19.36 sec`。
- 视觉捕获：`shinkou_ui_capture.exe shinkou_engine_sample.exe editor-audio-timeline.bmp 5000 dx11` 成功，报告 `captured=1`、`client=1280x690`、`dpi=144`、`mode=EngineGpuReadback`；截图检查确认 dark Windows shell、Hierarchy、Viewport、Asset Browser、Inspector 和底部状态栏正常呈现。样例没有选中 AudioSource，因此该截图是宿主 UI 基线，不冒充时间轴截图。

### 安全、性能与视觉审计

- `absolute:` payload 只在 EditorLayer 解析为 finite、非负秒数，并按已知 duration 或未知时长的 7 天上限夹紧；ObjectId 必须完整消费且非零，非法字符串不会进入 AudioSceneSystem。
- scrubber 的模型读取是常数 scalar 查询，进度 track/marker/button 提交不扫描文件、不触发 decoder、不新增 voice/asset；连续点击仍通过同一主线程 command boundary。
- retained region 与 row height 纳入现有 DPI logical layout；track、marker、accent 和 unavailable 文本沿用 Shinkou flat surface token。已有字段命中位置由交互测试覆盖，D3D11 capture 证明宿主 presentation 链路仍可见。

### 失败状态与回滚路径

- duration unknown、cursor unsupported、voice inactive 或 target 非法时不发送有效 absolute seek；±5 秒按钮仍按 4.33 capability 规则工作，UI 显示明确不可用状态。
- 如果点击落在无效区域，UiRuntime 不触发 command；如果 AudioScene 拒绝 seek，EditorLayer 只更新 bounded status，不修改场景配置。
- 回滚可移除 `audio-timeline` region、absolute parser 和额外行高，恢复 4.33 duration metadata 与离散 transport；不影响资源 identity、空间 listener 或 Media Preview。

### 未解决风险与下一轮

- 当前 scrubber 只有标尺/进度 marker，没有把异步 waveform snapshot 复用到场景 Inspector；下一轮需要定义 snapshot identity、source stamp、取消/替换和缓存预算后再连接波形。
- streaming buffer、loop/end-of-file、播放头按帧刷新和真实设备输出质量仍未完成；Miniaudio metadata 同步检查的加载尖峰也仍待 derived cache 化。
- 资源 rename/import migration 与完整模型 PBR/material/texture/depth/animation 预览仍是后续阶段，不能宣称最终 Unity 级能力已完成。

## 第 4.35 子阶段：异步 waveform snapshot 与场景 scrubber 复用

### 实现与范围

- EditorLayer 将已完成的 immutable `EditorAudioPreviewSnapshot` 传入 EditorUiModel 的场景 transport presentation，但只有 `selectedAsset_ == AudioSource.clipPath` 且 snapshot valid 时才复用；Model 只持有 shared pointer，不创建 decoder/voice。
- AudioSource scrubber 在 duration/cursor 可用时绘制 bounded peak columns：播放头前使用 accent，播放头后使用 muted token；snapshot 不可用时回退到 4.34 的 filled track/marker，并显示已有的 duration/capability 状态。
- Media Preview 和 Scene Audio 的 target、句柄、播放状态继续分离；共享边界仅为已完成的只读 waveform snapshot，snapshot pointer 变更由 Model revision 触发 retained repaint。
- 未新增 scene JSON 字段、Undo 记录、同步文件 IO 或自动扫描未选中的 AudioSource；streaming buffer/loop/end-of-file 仍未在本轮伪装完成。

### 契约与证据

- `shinkou_editor_audio_preview_tests`、`shinkou_ui_render_tests`、`shinkou_editor_interaction_tests` focused CTest `3/3` passed、总计 `15.35 sec`；覆盖 provider、retained renderer 和 scene scrubber/absolute seek/NaN guard 回归。
- `cmake --build out/build/mingw-debug -j 2` 全目标增量构建通过；新增 pointer 不改变 AudioSystem/AudioSceneSystem ABI 行为，已有 55 项测试集继续作为提交前门禁。
- 最终全量 CTest：`55/55` passed、0 failures、总计 `37.76 sec`；本轮 snapshot pointer 与 waveform draw 加入后重新验证完成。D3D11/capture 仍沿用 4.34 的宿主链路证据，真实 AudioSource waveform capture 留作下一轮专用 fixture。

### 安全、性能与视觉审计

- snapshot 只能来自已有异步 provider 的 generation/source-stamp 路径；路径不匹配、future 失败/取消、snapshot invalid 都不进入 waveform draw，避免 stale resource 内容显示在别的 AudioSource 上。
- peak columns 受 snapshot size 和 track width 双重上限约束，paint 只读取共享只读内存并提交有限 line commands；没有每帧 IO、decoder、临时 PCM 缓冲或额外 voice。
- scrubber waveform 使用既有 flat surface/accent/muted 语义色和 DPI-safe logical geometry；capture 仍只证明宿主壳层，真实 AudioSource waveform 需要专用选中对象 fixture 后再做截图验收。

### 失败状态与回滚路径

- snapshot 不可用时保留 4.34 progress marker，AudioScene transport 不受影响；对象/资源选择切换会通过 Model pointer 清除旧 waveform。
- 回滚只移除 `audioTransportPreview` pointer 和 waveform branch，保留 4.34 continuous scrubber、absolute parser、duration metadata 和相对控制。

### 未解决风险与下一轮

- 当前 EditorInteraction fixture 的 `preview.wav` 是不可解码占位文件，因此本轮没有真实 AudioSource waveform screenshot；下一轮建立可解码音频 fixture并做专用 capture。
- streaming buffer、loop/end-of-file、播放头刷新频率和真实设备输出仍未完成；metadata inspection 的同步尖峰仍待 derived cache 化。
- rename/import migration、完整模型 PBR/material/texture/depth/animation preview 仍未完成，项目尚不能宣称 Unity 级完整能力。

## 第 4.36 子阶段：AudioSource 组件绑定的真实 waveform 端到端证据

### 实现与范围

- EditorLayer 新增 `audioPreviewSourcePath_` identity，并把异步 audio preview 请求抽象为任意已索引的 project-relative audio path；资源浏览器选择仍可驱动 Media Preview，但 Inspector waveform 现在由选中 AudioSource 的 `clipPath` 驱动。
- AudioSource 选中时，EditorLayer 在 model sync 前按 clip path 查找 asset index 并启动/复用现有 worker；`poll_audio_preview` 校验 generation、result path、当前 source path 和 audio descriptor，旧 future 的取消结果不会进入新组件。
- EditorInteraction fixture 的 `preview.wav` 改为确定性 16-bit mono PCM WAV（256 frames、8 kHz），并在时间轴上等待至少 8 个 bounded vertical waveform line commands；fallback 的单个 playhead line 不足以通过该断言。
- 未改变 scene JSON、Undo 栈、AudioScene voice ownership 或 UI renderer/backend contract；没有新增同步 decoder、设备播放或 shell/file command。

### 契约与证据

- `cmake --build out/build/mingw-debug --target shinkou_editor_interaction_tests -j 2` 通过；该目标同时重链当前 `shinkou_physics` 静态库。
- 聚焦 CTest：`shinkou_editor_audio_preview_tests`、`shinkou_dock_layout_tests`、`shinkou_ui_render_tests`、`shinkou_editor_interaction_tests` 为 `4/4` passed，累计 `6.68 sec`。
- `shinkou_editor_interaction_tests` 单独重复执行 3 次均通过；覆盖真实 PCM fixture 解码、AudioSource Inspector waveform command、连续 scrub 到 15 秒、非法 `absolute:nan`、Pause/Resume/Stop、空间参数及 Undo/Redo。
- 全目标增量构建 `cmake --build out/build/mingw-debug -j 2` 在先产出当前工作区的 `shinkou_physics` 静态库后通过；早先并行构建时的 `cannot find shinkou_physics/libshinkou_physics.a` 是依赖产出顺序问题，已通过完整重跑复核。
- 最终全量 CTest：`56/56` passed、0 failures，总计 `25.57 sec`；Physics 的额外测试来自工作区中与本轮 UI 无关的并行改动，未被本轮提交范围吸收。

### 安全、性能与视觉审计

- preview provider 继续通过 `FileSystemService::resolve_existing` 的 project-relative boundary、256 MiB 文件上限、miniaudio decoder 和 cancellation token；UI path 不进入 shell/进程启动路径。
- source path 切换会增加 preview generation、取消旧 future，并要求 result path 与当前 path 一致；shared immutable snapshot 只在 render/model bridge 中传递，paint 不做文件 IO 或解码。
- Inspector waveform 列数由 `min(peaks.size(), track.width * 0.5)` 限制；本轮使用 retained command 证据验证真实 waveform，尚未宣称有 AudioSource 专用 D3D11 截图。宿主 UI capture 仍只作为 shell/backend visibility baseline。
- Windows-first flat Inspector、accent/muted waveform、76 logical px timeline row 和已有 logical DPI region 未改变；AudioSource clip/bus/spatial/transport 命中位置由交互测试保留覆盖。

### 失败状态与回滚路径

- decoder 不可用、fixture/资源损坏、source path 不在 index、future 取消或 stamp 失配时，Inspector 保留 4.34 marker/filled-track fallback；不会伪造波形或影响 scene transport。
- 回滚可删除 `audioPreviewSourcePath_`、组件路径触发器和 waveform line branch，保留 duration/capability、absolute seek 和既有 Media Preview provider。

### 未解决风险与下一轮

- 目前已有 retained waveform command 证据，但尚未有样例场景自动选中 AudioSource 的 D3D11 screenshot；下一轮建立专用 visual fixture 并覆盖 1280/1600/窄窗口与 DPI matrix。
- streaming buffer、loop/end-of-file、播放头刷新频率、异步 duration derived cache、rename/import migration 和完整 PBR/material/texture/depth/animation model preview 仍未完成；项目仍不能宣称 Unity 级完整能力。

## 第 4.37 子阶段：AudioSource D3D11 visual fixture 与窗口证据

### 实现与范围

- `EngineSample.cpp` 增加显式 `--audio-fixture` 模式：临时目录生成 1 秒 PCM WAV，启动后通过真实 `AssetSystem::scan_sources()` 获得 AssetId，创建/选中 `Audio Source Fixture`，绑定 `assets/preview.wav` 并保存临时 scene 清除 dirty close prompt。
- 新增 [AudioSourceVisualCapture.script](C:/Users/Lenovo/Desktop/Shinkou/Shinkou%20Engine/docs/AudioSourceVisualCapture.script)，通过 targeted Win32 message -> SDL/InputSystem -> EditorLayer -> retained UI 滚动 Inspector，然后触发 GPU capture；默认样例路径和默认项目根不受影响。
- fixture 使用 RAII 清理临时 project root；第一次 capture 发现相对路径传给子进程会导致 QA state/GPU output 无法定位，改用绝对 output/script 路径后复跑成功，未修改工具以掩盖该证据问题。

### 契约与证据

- `cmake --build out/build/mingw-debug --target shinkou_engine_sample -j 2` 通过；fixture 直接运行 `--audio-fixture --frames 10 dx11` 退出码 0，并输出 `editor-ui-assets=1`、`editor-ui-first-asset=assets/preview.wav`。
- 视觉 capture 命令要求 `--require-gpu --client-size 1280x720`，最终返回码 0：`mode=EngineGpuReadback`、`surface=1280x720`、`client=1280x720`、`dpi=144`、186 frames；sample 输出 `object=1 asset=7442611027655595716 path=assets/preview.wav`。
- 脚本 steps 全部成功：wait、hover Inspector、wheel、capture 共 `5/5 ok`；state snapshot 显示 `AssetSystem manifest ready: 1 resources`、`audio-timeline:1` region 和 Inspector scroll 后的可见轨道。
- 最终截图：[editor-audio-source-4-37.bmp](C:/Users/Lenovo/Desktop/Shinkou/Shinkou%20Engine/out/qa/editor-audio-source-4-37.bmp) 已检查：Hierarchy 选中 Audio Source Fixture、Asset Browser 显示 preview.wav、Inspector 显示 waveform 与 `0.00 / 1.00 s`。

### 安全、性能与视觉审计

- fixture 所有文件位于唯一临时目录，正常退出后 RAII 删除；capture 子进程只接收 `--audio-fixture`、`dx11` 和绝对 capture/script 路径，不访问用户项目文件。
- waveform 仍由 EditorLayer 异步 provider 生成，AudioScene 使用真实 AssetId/path/duration；sample 运行 186 frames 未在 paint 中增加 IO、decoder 或额外 voice。
- `EngineGpuReadback` 证明截图来自 D3D11 presented surface，而不是 GDI fallback；1280×720 client、DPI 144、logical scale 1.5 下 dark shell、Timeline、waveform、asset icon 和层级布局可读。
- capture 关闭时若 fixture 场景 dirty 会出现系统保存确认框；本轮通过保存临时 scene 消除了该交互阻塞，并在最终运行确认临时目录清理无新增残留。

### 失败状态与回滚路径

- fixture 创建/AssetSystem indexing 失败时 sample 返回非零；capture 要求 GPU readback，首次相对路径错误和脚本 timeout 均被记录为失败，不计入成功证据。
- decoder/preview 失败仍回退 4.34 marker/filled track；移除 sample flag、capture script 和 fixture-only save 即可回滚，不影响常规 EditorLayer 或 4.36 waveform contract。

### 未解决风险与下一轮

- 当前已完成 1280×720 dark/DPI 1.5 的真实窗口证据，尚未完成 1600、1024/窄窗口、light/high-contrast 和多 DPI matrix。
- streaming buffer、loop/end-of-file、播放头刷新预算、异步 duration derived cache、rename/import migration 与完整 PBR/material/texture/depth/animation model preview 仍未完成；不能宣称最终 Unity 级能力已完成。

## 第 4.38 子阶段：资源改名/删除与活动场景引用迁移

### 实现与范围

- `EditorLayer::remap_live_asset_references` 现在对文件和目录使用 normalized project-relative prefix，将活动 World 中 `AssetReferenceComponent` 的 path 迁移到新位置；`AudioSourceComponent` 同步迁移 `clipPath` 并清零旧 `assetId`。
- `poll_asset_manifest_scan` 发布新 immutable manifest 后调用 pending refresh：按 `FileSystemService::project_relative_existing` 得到的 canonical source path 查找新 entry，普通资源重新绑定 AssetId；AudioSource 仅接受 `key.type == "audio"`，否则保持 path + `assetId=0` 的 missing/unavailable 语义。
- Delete 路径按同一 prefix 使 live reference 的 AssetId 失效但保留 path，方便 Inspector/场景诊断；选中 asset 和当前 Asset Browser 目录在 rename/delete 时同步更新。
- 活动 World 发生外部文件迁移时清空旧 `EditorDocument` undo/redo snapshots 并标记 scene dirty，因为现有 history 只保存 World、不能原子地回滚 filesystem rename；这避免用户 Undo 恢复已经不存在的旧路径。
- 未加载 scene/prefab 文件、跨项目移动、依赖图重写和 AssetId 算法变更仍明确不在本轮范围。

### 契约与证据

- `shinkou_editor_interaction_tests` 通过真实 retained Project browser 路径：`Folder/Nested/needle.txt -> renamed.txt` 后断言文件存在、live reference path 更新、旧/新 manifest AssetId 不同且引用重新绑定新 ID；同一测试通过 Context menu Delete 后断言文件消失、live reference path 保留且 AssetId 为 0。
- 同一回归保留并通过 AudioSource picker/transport/waveform、模型 AssetId 场景实例、native drop、viewport boundary、scene save/open、Undo/Redo、build profile 和 compile_commands 集成检查；focused CTest 为 `1/1 passed`。
- 迁移后重新等待 manifest ready，再执行 viewport boundary 断言；这验证了 delete 引起的异步 AssetSystem refresh 不会被错误地当作坐标或拖放错误。
- 全量 `cmake --build out/build/mingw-debug -j 2` 通过；完整 CTest `56/56 passed`、0 failures、总计 `20.04 sec`；`shinkou_editor_interaction_tests` focused rerun `1/1 passed`、`19.09 sec`。`git diff --check` 无实际 whitespace error（仅报告仓库既有 LF→CRLF 提示）。

### 安全、性能与视觉审计

- Rename/Delete 的原始 IO 仍由 `FileSystemService` 执行，拒绝 project root、绝对路径、`..` 穿越和已存在目标；引用迁移只消费已校验的组件字符串和 manifest canonical path。
- rename 的 World 遍历是一次性 bounded component pass；ID rebinding 在后台 manifest worker 完成后、主线程发布快照的边界发生；paint 不 hash 文件、不启动进程、不访问 decoder 或音频设备。
- 本轮没有改变 retained geometry 或 renderer/backend presentation contract；现有 Windows-first Project browser、状态栏、console 和 Inspector 视觉基线保持不变，交互测试覆盖 context-menu delete 和资源树入口。

### 失败状态与回滚路径

- 文件 rename/delete 失败时不修改活动 World；manifest 未发现新 entry 时 path 保留、AssetId 为 0；AudioSource 类型变化也不会错误绑定为非音频资源。
- 旧 world-only undo snapshots 在迁移后被清空，这是明确的安全一致性策略；回滚可移除 live migration/pending refresh 并保留原始 FileSystemService 事务和 manifest refresh。

### 未解决风险与下一轮

- 当前迁移只覆盖活动 World，未加载的 scene/prefab 仍需要结构化引用 migration；文件操作本身还没有加入可回滚的统一历史栈。
- streaming buffer、loop/end-of-file、playback-head refresh budget、异步 duration derived cache、多 DPI/主题视觉矩阵和完整模型 PBR/material/texture/depth/animation 仍未完成。

## 第 4.39 子阶段：未加载 scene/prefab 的结构化引用迁移

### 实现与范围

- `EditorLayer::migrate_asset_documents` 在 Rename/Delete 文件事务成功后，以 32768 条递归项目文件上限筛选 `.scene`/`.prefab`，跳过当前 `scenePath_` 对应的活动场景，避免未保存 World 被直接落盘。
- 每个候选文档使用 `FileSystemService::read_text_limited` 的 16 MiB 上限和 `EditorDocument::from_json` 完整解析；只改写已知 `AssetReference.path`/`assetId` 与 `AudioSource.clipPath`/`assetId` 字段。Rename 写入新 normalized prefix 并令 ID 为 `0`，Delete 保留旧 path 供诊断并令 ID 为 `0`。
- 合法且有变化的文档通过 `EditorDocument::to_json` 和 `write_text_atomic` 更新；解析失败、大小超限、序列化失败、写入失败或扫描达到上限时跳过原文件，并保留首个审计错误。
- `EditorInteractionTests` 增加未打开 `assets/Scenes/Unloaded.prefab` fixture：Rename 后验证 `assets/Folder/Nested/renamed.txt`，Delete 后验证 `assets/Folder-extra.txt` 仍存在于文档且两者 AssetId 都为 `0`。

### 契约与证据

- `cmake --build out/build/mingw-debug -j 2` 通过，随后 `shinkou_editor_interaction_tests` 目标重编译通过。
- 聚焦 CTest：`shinkou_editor_interaction_tests` `1/1 passed`，总计 `22.80 sec`；测试通过真实 retained Project browser Rename 和 Context-menu Delete 触发文件事务，不直接调用迁移私有方法。
- 最终全量 CTest：`56/56 passed`、0 failures，总计 `31.60 sec`；结果包含当前工作区并行 Physics 测试，但本轮只提交 UI/文档相关文件。
- 断言覆盖：磁盘资源 rename/delete、活动 World path/AssetId 迁移、异步 manifest rebind、未加载 prefab 的结构化 path/ID 迁移、场景 Save/Open、拖放边界、AudioSource waveform/transport、模型 AssetId 场景实例、编译器/项目集成面板和 Undo/Redo 回归。
- 视觉/渲染链路未改变：本轮没有新增截图，沿用 4.37 的 D3D11 1280×720/DPI 144 baseline；本轮证明的是 Project browser -> FileSystemService -> EditorDocument structured write 的交互与持久化链路。

### 安全、性能与视觉审计

- 文件安全：候选路径来自受限 `FileSystemService::list`，读写仍做 canonical project-root boundary；不接受任意绝对路径、`..` 穿越或全文替换；未知/损坏文档保持原样。
- 数据安全：Rename 清零旧 AssetId，Delete 保留 missing-reference path 并清零 AssetId；活动场景继续清空无法覆盖 filesystem 的 world-only undo/redo，避免恢复旧路径。
- 性能：单次操作只做一轮 bounded document scan；每个合法文档最多一次 parse 和一次 atomic write；无 paint、渲染、decoder、AssetSystem manifest worker 或 audio voice 额外工作。
- 视觉：没有改变 retained draw list、输入桥、DPI logical geometry 或 backend presentation；状态栏/console 汇总跳过数量和首个错误，后续将通过 visual fixture 补持久化迁移状态的窗口证据。

### 失败状态与回滚路径

- 文件事务失败时不启动文档迁移；单个文档失败不覆盖它的原内容；扫描上限或解析错误只增加 skipped/audit 状态，不影响其他合法文档。
- 活动场景不被离线 writer 直接改写；用户仍通过 Save Scene 决定何时持久化内存迁移。回滚可移除 `migrate_asset_documents` 调用及 4.39 fixture，保留 4.38 live migration。

### 未解决风险与下一轮

- 迁移后的未加载文档会安全地保存正确 path，但其旧 ID 被置零；重新打开后基于 manifest canonical path 的自动 rebind 尚未完成，下一轮补齐并增加对应断言。
- 文件操作历史、失败重试/批量报告、streaming buffer/loop/end-of-file、播放头预算、多 DPI/主题矩阵和完整模型 PBR/material/texture/depth/animation 仍未完成；项目仍不能宣称最终 Unity 级能力已完成。

## 第 4.40 子阶段：文档打开与 manifest 的 AssetId 重新绑定

### 实现与范围

- 新增 `EditorLayer::rebind_asset_references_from_manifest`：manifest immutable snapshot 发布后扫描活动 World；Open Scene/Prefab 时若 snapshot 已就绪也立即执行，未就绪则等待下次发布。
- `AssetReferenceComponent` 按 `FileSystemService::project_relative_existing(entry.sourcePath)` 得到的 canonical project-relative path 绑定任意匹配 entry；`AudioSourceComponent` 只有 entry type 为 `audio` 才绑定，其他情况保持 path + `assetId=0`。
- 重新绑定成功会标记 scene dirty，要求用户显式 Save Scene；不存在资源不清除 path，只维持 0，保留 missing-reference 诊断。没有改变 AssetId 计算、manifest schema 或资源处理器。
- EditorInteraction 增加 path-only rebind、Refresh Assets、打开 `Unloaded.prefab`、重命名/删除资源分别恢复/保持 ID、再打开 `test.scene` 的真实流程断言。

### 契约与证据

- `cmake --build out/build/mingw-debug --target shinkou_editor_interaction_tests -j 2` 通过；构建期间并行 Physics 静态库正常重新链接，本轮没有暂存其改动。
- 聚焦 CTest：`shinkou_editor_interaction_tests` `1/1 passed`，总计 `30.90 sec`；覆盖 Refresh Assets -> manifest publish -> path-only rebind 和 OpenScene -> immediate rebind 两条边界。
- 最终全量构建 `cmake --build out/build/mingw-debug -j 2` 通过；全量 CTest `56/56 passed`、0 failures，总计 `32.41 sec`。
- EditorInteraction 断言迁移 prefab 中 renamed path 得到新 manifest ID，deleted path 保持旧诊断路径且 ID 为 0，并断言重新打开活动 scene 后 object count 为 5；同时保留资源浏览器、拖放 boundary、AudioSource、模型预览、编译器集成与 Undo/Redo。
- 视觉/渲染链路不变：本轮无新增 screenshot；4.37 的 D3D11 1280×720/DPI 144 capture 继续作为 UI/backend baseline，本轮新增证据是文档打开和 manifest identity 状态链路。

### 安全、性能与视觉审计

- rebind 不相信文档里的旧 ID，必须通过当前 manifest entry 和 canonical source path；AudioSource 还必须通过资源类型校验，避免把纹理/模型 ID 绑定成音频。
- manifest 不可用或 source 缺失时不做同步扫描、不生成猜测 ID；path 保留、ID 为 0，现有 Inspector/AudioScene missing/pending 语义继续生效。
- rebind 只在 manifest 发布和 OpenScene 边界执行一次有界 World component pass；不在 paint、renderer、decoder、音频设备或进程启动路径做工作。
- 没有改变 retained draw list、输入桥、DPI logical geometry 或 renderer presentation seam；成功/失败状态仍由已有状态栏、console 和 Inspector identity 文案承载。

### 失败状态与回滚路径

- manifest future 失败、路径越界、类型不匹配或资源删除时，rebind 返回 0/写入 0，不抛出异常、不覆盖文档路径；下一次合法 manifest 可再次尝试。
- 用户未保存的 scene 仍不会被自动写盘；rebind 只标 dirty。回滚可移除 rebind 方法与 OpenScene/manifest 调用，保留 4.39 的结构化迁移和旧 ID 清零策略。

### 未解决风险与下一轮

- 文件 rename/delete 尚未拥有跨 filesystem + World 的可恢复事务 history；批量操作报告、失败重试和用户确认语义仍需设计。
- streaming buffer/loop/end-of-file、播放头刷新预算、异步 duration cache、多 DPI/主题视觉矩阵和完整模型 PBR/material/texture/depth/animation 仍未完成，项目仍不能宣称最终 Unity 级能力已完成。

## 第 4.41 子阶段：可恢复的项目文件事务历史

### 实现与范围

- `EditorLayer` 增加 `EditHistoryKind::File` marker，与原有 World `EditorDocument` snapshot 共用 Undo/Redo 顺序；文件事务不会被误当成场景快照，也不会在回放时重复入栈。
- Rename 记录 source/destination；Delete 改为通过 `FileSystemService::rename` 移到 `.shinkou/recycle/<serial>/<filename>`，保留操作、选中资源和 Asset Browser 目录上下文。Undo 将其移回原路径，Redo 再次移入回收区。
- 文件回放重新执行 live reference invalidation/remap、未加载 scene/prefab migration、selection/asset-directory 更新和 AssetSystem manifest refresh；`replayingFileOperation_` 防止内部迁移清空当前 history。
- `AssetSystem::scan_sources` 遇到 `.shinkou` 目录时禁用递归，回收文件、缓存、manifest 和 build profile 不会成为 manifest 资源。
- EditorInteraction 增加回收区、manifest 隔离、文件 Undo 恢复并 rebind、文件 Redo 再删除的真实断言；测试中重新获取当前 World reference，避免场景快照 restore 后使用失效对象指针。

### 契约与证据

- `cmake --build out/build/mingw-debug --target shinkou_editor_interaction_tests -j 2` 通过。
- 聚焦 CTest：`shinkou_editor_interaction_tests` `1/1 passed`，总计 `42.90 sec`；覆盖资源浏览器 Delete、recycle entry、AssetSystem metadata isolation、Undo restore/rebind、Redo recycle/invalidate、未加载 prefab migration 和现有 UI/音频/模型/构建集成回归。
- 最终全量构建 `cmake --build out/build/mingw-debug -j 2` 通过；全量 CTest `57/57 passed`、0 failures，总计 `40.00 sec`。本轮工作区并行 Physics 增加了第 57 个测试，但未被本轮提交吸收。
- AssetSystem 直接重扫验证 `assets/Folder-extra.txt` 恢复后仍产生稳定原始 AssetId；EditorLayer immutable manifest snapshot 同样公开并匹配 canonical source path，排除了“清零后随机恢复”误判。
- 视觉/渲染链路没有改动；状态通过 retained Project Browser、状态栏和 Console 现有路径承载，D3D11 4.37 baseline 继续有效。

### 安全、性能与视觉审计

- 回收路径由编辑器生成在项目根内，仍经过 canonical boundary；用户输入只决定已验证 source path，不能把目标写到项目外或执行进程命令。
- Delete 变成可逆 rename，原始数据不在当前会话的历史窗口内被直接销毁；旧 file history 被新文件事务清空时才移除其回收项，Undo/Redo 分支失效语义明确。
- `.shinkou` 在 AssetSystem 源扫描中被隔离，防止回收/缓存污染资源类型和 AssetId；Editor asset browser 默认从 `assets` 目录扫描，布局与命中区域不变。
- 文件事务回放只在命令边界进行 IO、World pass 和 manifest refresh；paint、renderer、preview decoder、audio voice 不承担文件历史工作。

### 失败状态与回滚路径

- 回收移动、恢复或重做失败时 operation 留在原 history 栈，引用迁移不会继续伪装成功；资产文档写入失败保留原内容并记录审计错误。
- 新的场景编辑清理 file redo branch，避免用户在新编辑后恢复旧文件状态；`.shinkou/recycle` 中的孤立内容尚未做跨会话治理，但不会进入 AssetSystem manifest。
- 回滚可删除 File history/recycle 逻辑并回到物理删除，但这会重新引入不可恢复风险，因此只作为应急兼容方案，不是目标状态。

### 未解决风险与下一轮

- history 目前是进程内的线性栈，尚未跨会话持久化；回收项需要启动时治理、空间上限和用户可见的恢复/永久清理面板。
- 批量文件操作、并发外部修改冲突、streaming buffer/loop/end-of-file、播放头预算、多 DPI/主题矩阵和完整模型 PBR/material/texture/depth/animation 仍未完成。

## 第 4.42 子阶段：跨会话文件事务日志

### 实现与范围

- 新增 `engine/include/shinkou/editor/FileHistory.h` 与 `engine/src/editor/FileHistory.cpp`，通过 Shinkou reflection/JSON 序列化 `editor.FileHistoryDocument` 和文件操作条目；日志固定版本 1，单文件上限 1 MiB、条目上限 64。
- `EditorLayer` 在初始化、项目根切换、Reload Layout 和 Reset Layout 时重新载入 `.shinkou/file-history.json`；载入前清理旧根的进程内 history/recycle 分支，避免跨项目污染。
- 载入时逐项校验 Rename 的 source/destination 状态、Recycle Delete 的原路径/回收路径状态和 `.shinkou/recycle` 边界；无效条目跳过并写入 Console，不执行任意日志路径。
- 当前 fileUndo 分支在新文件事务、文件 Undo/Redo 后原子重写；Undo 后 journal 为空，Redo 后 journal 重新包含可恢复 delete。`.shinkou` 仍由 AssetSystem 排除在资源 manifest 之外。
- 非目标：本轮不实现回收区空间治理、跨进程锁、批量恢复 UI 或永久删除 UI；日志只描述编辑器已知的 Rename/Recycle Delete。

### 契约与证据

- `cmake --build out/build/mingw-debug --target shinkou_editor_interaction_tests -j 2` 通过；当前并行 Physics CMake 配置暂时过滤该编辑器源文件，因此本地验证通过 `target_sources` 显式补入 `FileHistory.cpp`，在基础配置下则由 engine source glob 自动收集。
- 聚焦 CTest：`shinkou_editor_interaction_tests` `1/1 passed`，总计 `39.80 sec`；覆盖真实 Project browser Delete、日志落盘、第二个 `EditorLayer` 跨实例加载、Undo 清空 journal、Redo 重写 journal。
- `EditorInteractionTests` 同时断言真实回收文件状态、AssetSystem manifest 不含 `.shinkou` 内容、Undo 后原始 AssetId 重新绑定、Redo 后引用重新失效；测试使用重建后的当前 World 引用，避免 scene snapshot restore 后使用失效指针。
- Reload Layout/Reset Layout 已与初始化和 `set_project_root` 使用相同的 history 切换钩子；完整构建通过，全量 CTest `57/57 passed`、0 failures，总计 `68.91 sec`，包含并行 Physics 测试。

### 安全、性能与视觉审计

- 文件安全：日志仅由反射 JSON 解析为结构化字段；所有 source/destination/recycle path 继续经过项目相对规范化，回收项必须位于 `.shinkou/recycle`，原子写入避免半截日志覆盖。
- 数据安全：日志只恢复磁盘状态明确匹配的操作；未知版本、缺失文件、目标冲突、越界回收路径和超限输入均跳过并报告，不会因启动自动改写项目资源。
- 性能：JSON IO 只发生在初始化、布局切换和文件事务/Undo/Redo 边界；没有新增 paint、retained draw list、renderer、preview decoder、audio voice 或 AssetSystem worker 工作。
- 视觉/交互：不改变 Project Browser 行高、快捷键、DPI logical geometry、Inspector 或 backend presentation；恢复/跳过状态沿用状态栏和 Console，可在后续 recovery panel 轮次补齐窗口级证据。

### 失败状态与回滚路径

- journal 读取失败时编辑器继续打开项目，Console 保留原因；单条冲突不会阻止其他有效条目载入。journal 写入失败不伪造成功状态，但当前进程内 history 仍可继续使用。
- 切换项目/布局前会移除旧项目的回收分支，避免误把旧 root 的可恢复操作带入新 root；新 root 的有效日志重新建立 file history marker。
- 回滚可移除 `FileHistoryDocument` 和 load/save hooks，回到 4.41 进程内 history；删除仍保留 4.41 的可恢复 recycle 语义。

### 未解决风险与下一轮

- 跨会话日志尚未治理孤立回收项、磁盘空间上限、外部进程并发修改和用户可见恢复/永久清理；当前只做安全跳过，不做自动破坏性清理。
- streaming buffer/loop/end-of-file、播放头刷新预算、多 DPI/主题矩阵以及模型 PBR/material/texture/depth/animation 仍未完成；项目仍不能宣称完整 Unity 级能力已完成。

## 第 4.43 子阶段：可见的文件恢复与回收区治理

### 实现与范围

- `EditorUiModel` 新增 `EditorFileRecoveryUiState`；`EditorLayer` 以有界 snapshot 发布 Undo/Redo 数量、当前操作、recycle 字节数、磁盘状态和 orphan 数量，保留 UI 与文件系统的生命周期边界。
- `EditorUi` 新增默认隐藏的 `File Recovery` dock panel、Window 菜单入口和 retained controls：`Undo latest`、`Redo latest`、`Prune orphans`。操作全部回到现有 `EditorCommand::Undo/Redo/PruneFileRecovery`，没有新增旁路文件变更。
- Recovery snapshot 递归扫描 `.shinkou/recycle` 最多 512 个条目，展示最多 64 行；当前历史和 redo history 的 operation root 建立保护集合，孤立 root 可由用户明确点击 `Prune orphans` 删除。总占用超过 512 MiB 或扫描受限时状态显示治理警告。
- 面板保持默认关闭，加入 layout reflection、dock default workspace、Reload/Reset Layout 和 ImGui Window 菜单同步；`.shinkou` 仍不进入 AssetSystem manifest。

### 契约与证据

- `EditorInteractionTests` 通过真实 retained Window/Recovery panel 打开路径，断言 Delete 后 `undoCount==1`、Recovery Undo 控件存在；点击 `recovery-undo` 后断言 `undoCount==0`、`redoCount==1` 和 Redo 控件存在，再点击 `recovery-redo` 断言计数恢复。
- 面板操作继续覆盖真实文件恢复、活动 World 引用迁移、manifest rebind、journal 清空/重写、未加载 prefab migration 和现有音频/图片/视频/模型/编译器回归。
- `cmake --build out/build/mingw-debug --target shinkou_editor_interaction_tests -j 2` 通过；聚焦 CTest 在一次既有 manifest-ready 异步时序波动后复跑稳定，最终 `1/1 passed`，总计 `53.02 sec`。最终全量构建通过，全量 CTest `57/57 passed`、0 failures，总计 `86.00 sec`。

### 安全、性能与视觉审计

- Prune 的目标来自 project-root bounded `FileSystemService::list`，且只删除不在 fileUndo/fileRedo 保护集合中的 recycle operation root；`.shinkou/file-history.json`、当前资源路径和外部路径不会进入删除集合。
- Recovery snapshot 在 `process_input`/事务边界更新，paint 只读取 `EditorFileRecoveryUiState`；列表、字节预算、条目数量都有上限，避免面板打开导致无界递归或文件 IO。
- 视觉沿用底部 tab stack、语义颜色和共享按钮/聚焦命中区，不改变中心 viewport、Inspector 行高或 renderer/backend seam；默认 hidden，用户从 Window 菜单显式打开。

### 失败状态与回滚路径

- 历史与磁盘冲突显示不可恢复状态，Undo/Redo 本身仍由既有 apply_file_operation 返回错误；Prune 单项失败进入 Console，并继续处理其他孤立根。
- 扫描达到 512 项或总占用超过 512 MiB 只产生面板警告，不静默永久删除；用户必须明确点击 `Prune orphans`，且保护项不会被清理。
- 回滚可移除 Recovery model/panel 和 `PruneFileRecovery` 命令，保留 4.42 持久化 journal 与 4.41 recycle/Undo 语义。

### 未解决风险与下一轮

- 当前仍没有跨进程锁、外部编辑器并发冲突的细粒度 diff、批量选择恢复和真正的 Asset dependency graph；下轮应将这些状态纳入批量事务报告。
- streaming buffer/loop/end-of-file、播放头预算、多 DPI/主题视觉矩阵以及模型 PBR/material/texture/depth/animation 仍未完成。

## 第 4.44 子阶段：音频预览 live-loop 与结束边界契约

### 实现与范围

- `IAudioBackend`/`AudioSystem` 增加可选 `set_loop(AudioVoiceId, bool)` transport seam；Miniaudio 将其映射到活动 `ma_sound` 的 looping 属性，旧/能力不足后端仍可安全采用默认 no-op。
- `EditorLayer` 的 retained Media 命令和旧 ImGui Media panel 在播放中切换 Loop 时都会更新当前 voice；场景 `audio-source:<id>` transport 不被该预览开关改写。
- 音频预览 voice 进入 `Finished`、`Stopped` 或 `Invalid` 时，若 asset 是路径型临时加载且仍由 AudioSystem 持有，则先卸载再清理预览 bookkeeping；自然结束仍将 currentTime 保持在 duration，显式 Stop 的清零行为不变。
- `EditorAudioPreviewTests` 的可控后端模拟 1ms duration、cursor、live loop 和 Finished；测试覆盖 loop 开关后的跨 EOF、关闭 loop 后 EOF、临时 asset 回收和再次播放。

### 契约与证据

- 专项构建：`cmake --build out/build/mingw-debug --target shinkou_editor_audio_preview_tests -j 2` 通过。
- 专项 CTest：`shinkou_editor_audio_preview_tests` `1/1 passed`，总计约 `3.00 sec`。
- 联合回归：`shinkou_editor_audio_preview_tests` 与 `shinkou_editor_interaction_tests` 均通过，`2/2 passed`，总计约 `41.42 sec`；Interaction 仍覆盖 retained Media/AudioSource transport、场景 voice 和编辑器 UI 命令总线。
- 最终全量构建：`cmake --build out/build/mingw-debug -j 2` 通过。
- 最终全量 CTest：`57/57 passed`、0 failures，总计 `72.90 sec`；覆盖音频系统/AudioScene、Editor Media/AudioSource、图片/视频/模型预览、编译器/项目集成、UI render 和工作区并行 Physics 测试。

### 安全、性能与视觉审计

- voice handle 只从 `AudioSystem` 已创建的预览绑定进入 `set_loop`；结束回收只卸载 `audioPreviewAssetId_ == 0` 的临时路径 asset，manifest-backed binding 保持缓存所有权，不新增任意路径或进程入口。
- loop/EOF/release 都在命令或 transport 状态边界执行；绘制只消费 `EditorMediaUiState` 和已有音频快照，没有把 filesystem、decoder、future 或回收扫描加入 paint。
- waveform/timeline/Loop button/状态文字继续使用现有 retained UI 和 logical DPI 几何；新增行为的窗口证据以交互断言为主，未把非专用截图误称为音频窗口证据。

### 失败状态与回滚路径

- 不支持 live loop 的 backend 不崩溃，UI loop 状态继续作用于下一次 voice；无效或已结束 voice 清为 stopped 并释放临时资源。
- 编译器/渲染/场景音频的并行改动不在本轮文件范围内，保持工作区未暂存；本轮只可独立回滚 AudioSystem seam、EditorLayer 两处接入和 preview fixture。
- 若全量回归暴露既有并行 Physics/CMake 变更影响，保留专项通过证据并暂停提交，不将失败归因于音频轮次。

### 未解决风险与下一轮

- 尚未完成真实多 backend 设备上的播放头刷新成本、streaming ring buffer/underflow、外部编辑器并发文件冲突和批量事务报告。
- 多 DPI/主题窗口 capture matrix 与模型 PBR/material/texture/depth/animation 预览仍未完成；项目继续不能宣称最终 Unity 级能力已完成。

## 第 4.45 子阶段：外部文件变化审计与事务边界

### 实现与范围

- `EditorLayer` 新增 scope-aware file scan baseline 和 bounded editor expectation：编辑器自己的文件写入、资源 rename/delete、recycle move、未加载文档迁移、build profile/compile export 与父目录元数据变化不会被报告为外部冲突。
- 未被 expectation 覆盖的 project-relative Added/Modified/Removed 变化进入 `EditorFileRecoveryUiState.externalChanges`，上限 64；Recovery retained panel 显示 External 数量、变化类型/路径和 `Dismiss changes`，清除命令立即同步 snapshot，不触碰磁盘。
- `FileSystemService::SnapshotValue` 从单一写入时间扩展为 size + writeStamp + directory 三字段比较；没有引入内容 hash、watcher 或 paint-time IO。`.shinkou` 变化继续不进入外部用户报告，AssetSystem manifest 仍隔离该目录。
- 资源事务 expectation 会补记 exact parent directory chain；subtree 操作仍只对源/目标根递归匹配，避免 Windows directory timestamp 更新污染真实冲突报告。

### 契约与证据

- `cmake --build out/build/mingw-debug --target shinkou_file_system_tests shinkou_editor_ui_model_tests shinkou_editor_audio_preview_tests shinkou_editor_interaction_tests -j 2` 通过。
- 聚焦 CTest：4/4 passed，总计 `46.53 sec`；覆盖外部 `assets/external-editor.txt` 写入、scan/manifest 发布、Recovery external row、Dismiss snapshot 清除、size-aware FileSystem fingerprint，以及既有音频/rename/delete/drag-drop/AudioSource/model/build/Undo/Redo 回归。
- 全量构建 `cmake --build out/build/mingw-debug -j 2` 通过；全量 CTest：57/57 passed，总计 `50.36 sec`，包含并行 Physics targets 与现有渲染/媒体/编辑器套件。
- 本轮验证未发现失败；4.45 可进入受控提交。GitHub 推送仍需用户确认具体 remote/branch。

### 安全、性能与视觉审计

- 外部报告只消费已完成异步 scan 的 project-relative metadata；`.shinkou` 路径被过滤，任何冲突不会自动 reload、overwrite、delete、shell 执行或网络访问。
- expectation 集合最多 128 个 marker，外部 rows 最多 64 个；scope 首次扫描只建立 baseline，之后每秒逻辑轮询触发 worker scan，Recovery paint 不访问 FileSystemService。
- 父目录链 exact 标记与 subtree 标记分离，避免把同目录 sibling 的外部编译器输出隐藏；External 状态复用 danger/accent/flat UI tokens 和现有 retained hit regions。

### 失败状态与回滚路径

- 早期验证发现 scope 切换会把 `assets` 初始 Added 当作外部变化，已改为 per-scope baseline；随后发现 Windows directory timestamp 会把编辑器 rename 误报，已通过 parent expectation 修正；两类回归均由专项 CTest 复核。
- Dismiss 只清理 UI audit state；扫描/manifest 失败继续由既有 status/console 报告，磁盘内容保持不变。
- 可回滚 external fields、scan signature 扩展和 expectation 过滤；原有 FileSystem boundary、Recovery Undo/Redo、journal 和 AssetSystem `.shinkou` 隔离不依赖自动冲突解决。

### 未解决风险与下一轮

- 同大小且写入时间未变化的外部内容替换仍无法由 metadata-only fingerprint 证明；需要显式 diff/hash budget 或 watcher 设计，不能在本轮宣称完整并发冲突解决。
- 尚未完成批量事务报告、三方 merge/resolve、多 DPI/主题 capture matrix、真实 backend 性能测量和模型动画/深度/offscreen 证据。

## 后续轮次模板

每轮复制以下条目并填写实际证据：

- 范围/非目标：
- 代码变更：
- 契约与单元测试：
- 集成测试：
- 构建配置与目标：
- 全量 CTest：
- 安全审计：路径、权限、进程、输入上限、第三方依赖：
- 性能审计：帧时间、分配、IO、缓存、峰值：
- 视觉/交互审计：窗口尺寸、DPI、主题、键鼠/拖放：
- 失败状态与回滚路径：
- 未解决风险：
- 下一轮入口：
