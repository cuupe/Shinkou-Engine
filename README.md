# Shinkou Engine

C++17 游戏引擎与 Windows 原生编辑器，使用 CMake 构建。主引擎包含渲染、场景、
输入、音频、脚本、物理、资源和 retained UI；资源库也可以独立构建。

## 目录

| 路径 | 内容 |
| --- | --- |
| `core/` | 独立公共基础库：错误/结果、值树与 JSON、句柄、哈希和 UTF-8 路径 |
| `engine/include/shinkou/` | 引擎公开头文件 |
| `engine/src/` | 各子系统实现 |
| `engine/tests/` | 引擎单元与合约测试 |
| `engine/samples/` | 示例入口及 shader 源码 |
| `engine/resources/` | 编辑器图标等随引擎提供的资源 |
| `engine/input/` | SDL3 输入 DLL，SDL3 静态链接到 DLL 内部 |
| `engine/physics/` | 独立物理构建入口与示例 |
| `engine/scripting/` | C# 脚本运行时与编译工具 |
| `assets/` | 独立 `shinkou_assets` 资源库、测试和文档；不是示例素材目录 |
| `render/` | 独立显示内核实验项目，尚未接入根构建图 |
| `third_party/`、`external/` | 随仓库提供的依赖及 PhysX 子模块 |
| `cmake/` | 可选依赖探测 |
| `Tools/` | 开发 MCP、音频工具与编辑器交互记录 |
| `docs/` | 架构、工作流和验收文档 |
| `out/`、`Saved/`、`.shinkou/cache/` | 本地构建、运行日志及资源缓存，不作为源码提交 |

## 构建与测试

需要 CMake 3.25 或更新版本及支持 C++20 的编译器（协程运行库使用 C++20，
现有引擎调用接口保留 C++17）。Windows 上已有 MinGW 和
Visual Studio 预设；`windows-vs` 使用 Visual Studio 18 2026 生成器。

在仓库根目录运行：

```powershell
cmake --preset mingw-debug
cmake --build --preset mingw-debug --parallel 4
ctest --test-dir out/build/mingw-debug --output-on-failure
```

MinGW 的编译器、make 及运行时 DLL 所在目录需要位于 `PATH` 前部，优先于其他
工具附带的旧 GCC 运行时。构建后的编辑器示例入口是
`out/build/mingw-debug/shinkou_engine_sample.exe`。

若测试在启动时以 `0xc0000139` 退出，先检查是否从 Anaconda 等目录加载了
不匹配的 `libstdc++-6.dll`。只为当前 PowerShell 会话设置编译器目录即可：

```powershell
# 将路径替换为本机 CMAKE_CXX_COMPILER 所在目录。
$env:PATH = 'D:\paths\GNU\mingw64\bin;' + $env:PATH
```

默认启用 SDL3 输入，并从源码拉取 SDL3。离线构建时，可以通过
`SHINKOU_INPUT_SDL3_SOURCE_DIR` 指定已有 SDL3 源码，或关闭
`SHINKOU_ENABLE_SDL3_INPUT`。`SHINKOU_FETCH_DEPENDENCIES=OFF` 不会关闭输入模块
单独控制的 SDL3 下载。PhysX、Vulkan、Lua、Python、.NET 等依赖的探测与配置见
`cmake/ShinkouDependencies.cmake`；PhysX 的接入要求见下方文档。

日志默认启用。仓库内提供固定版本的 spdlog，关闭依赖下载也能离线编译日志；
运行时默认异步输出到控制台和工作目录下的 `Saved/Logs/Engine.log`。
日志级别、文件轮转等配置见 [日志文档](engine/docs/Logging.md)。

仅检查文件系统相关改动：

```powershell
cmake --build out/build/mingw-debug --target shinkou_file_system_tests --parallel 4
ctest --test-dir out/build/mingw-debug -R '^shinkou_file_system_tests$' --output-on-failure --timeout 60
```

仅构建资源库，无需窗口、渲染器或 SDL3：

```powershell
cmake -S assets -B out/build/assets -G "MinGW Makefiles" -DSHINKOU_BUILD_TESTS=ON
cmake --build out/build/assets --target shinkou_assets_tests --parallel 4
ctest --test-dir out/build/assets --output-on-failure
```

## 编辑器

主编辑器现使用 Dear ImGui Docking、HelloImGui 主题、ImPlot 图表和 ImGuizmo。
启动入口为 `shinkou_engine_sample.exe --editor dx11 --project <项目绝对路径>`。
依赖固定版本、界面分层和真实窗口验证方式见
[编辑器界面接入](docs/EditorImGuiMigration.md)。

资源检查器支持文档/脚本、音频、视频与模型的预览和专属配置，详见
[资源预览与配置](docs/EditorAssetInspection.md)。

## 文件与资源访问

编辑器文件操作由 `shinkou::editor::FileSystemService` 管理，接受项目相对路径。
`list_detailed()` 和 `scan()` 会报告是否完整、是否达到数量上限及错误；不完整
扫描不发布变更，也不覆盖上一次完整快照。删除或重命名链接时操作链接条目本身。

运行时资源由独立 `AssetSystem` 通过 `project://...` 等挂载 URI 加载，提供异步
请求、缓存、依赖传播与热重载。两者的职责和后续缺口记录在
[文件系统审计](docs/FileSystemAudit.md)。

## 平台与操作系统接口

`shinkou::platform::ISystem` 统一提供时钟、虚拟内存、线程/同步/TLS/Fiber、
文件与目录监听、子进程、动态模块、栈回溯和控制台访问。后端分三类：

- **Windows Native**：`Window` 之外的内核对象层（`VirtualAlloc`、SRWLOCK、
  Fiber、`CreateProcessW` 等）；
- **POSIX Native**：Linux/macOS/BSD 的 `mmap`、pthread、ucontext、fork/exec、
  `dl_iterate_phdr` 与 `sigaction`；
- **Portable**：纯 C++17 标准库回退（Android/Emscripten/未知平台）。

入口、能力矩阵与各平台构建命令见
[平台与操作系统接口](engine/docs/Platform.md)。该层为增量引入，现有
`Window.h` 与编辑器/渲染/网络调用点保持不变；窗口、Vulkan surface 与编辑器
仍是 Windows 优先，跨平台运行时尚未接入。

## 文档入口

- [公共基础与统一数据契约](docs/CoreArchitecture.md)
- [C++20 协程：主循环调度、取消、事件和 JobSystem 桥接](engine/docs/Coroutines.md)
- [平台与操作系统接口（Native / Portable 后端）](engine/docs/Platform.md)
- [资源工作流](docs/AssetWorkflow.md)与[资源库说明](assets/README.md)
- [ECS chunk 存储（archetype / 16 KiB chunk）](docs/EcsChunkStorage.md)
- [ECS 运行时优化与读写依赖管线](docs/EcsRuntimeOptimization.md)
- [文件系统审计与本轮完善](docs/FileSystemAudit.md)
- [渲染架构](docs/RenderingArchitecture.md)
- [物理接入](docs/PhysicsIntegration.md)
- [编辑器 UI 工作流](docs/UIWorkflow.md)
- [独立显示内核](render/README.md)
- [音频混音测试工具](Tools/AudioMixerTest/README.md)
