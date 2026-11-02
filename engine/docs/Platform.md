# 平台与操作系统接口

本文描述 `shinkou::platform` 的统一系统层：它补齐了原先只有
`platform/Window.h` 的操作系统能力，并把 Windows 内核对象、POSIX 内核服务与
C++17 标准库回退后端放在同一个 `ISystem` 接口后面。本轮是**增量**改动：
`Window.h`、`editor/FileSystemService`、渲染/编辑器/网络等既有调用点都没有
替换，后续可以按模块分批迁移。窗口创建与消息循环仍由旧的 Windows
`Window` 实现负责（窗口 ≠ Windows 平台；跨平台窗口属于后续阶段）。

## 目录

| 路径 | 内容 |
| --- | --- |
| `engine/include/shinkou/platform/Platform.h` | OS/架构/编译器/端序检测、`Nanoseconds`、对齐与 `cpu_relax` 等编译期基础设施 |
| `engine/include/shinkou/platform/Error.h` | `ErrorCode`、`SystemError`、`Result<T>` / `Status` |
| `engine/include/shinkou/platform/SystemInfo.h` | 主机信息、`CalendarTime`、`MemoryStatus`、`Capability` |
| `engine/include/shinkou/platform/Sync.h` | 互斥量、条件变量、信号量、事件、TLS、Fiber、`SpinLock`、`OnceFlag` |
| `engine/include/shinkou/platform/Thread.h` | `IThread` 与线程优先级/亲和性描述 |
| `engine/include/shinkou/platform/Memory.h` | 虚拟地址预留/提交/保护、文件映射与共享内存 |
| `engine/include/shinkou/platform/File.h` | 原生文件流、目录枚举、目录变更监听 |
| `engine/include/shinkou/platform/Process.h` | 子进程、标准流捕获、环境变量 |
| `engine/include/shinkou/platform/Library.h` | 动态模块与已加载模块枚举 |
| `engine/include/shinkou/platform/Diagnostics.h` | 栈回溯、未处理异常、控制台 |
| `engine/include/shinkou/platform/System.h` | 统一接口 `ISystem`、`make_system()`、进程级 `System` |
| `engine/src/platform/system/windows/` | Windows 原生后端（内核对象） |
| `engine/src/platform/system/posix/` | Linux/macOS/BSD 原生后端（mmap、pthread、ucontext、fork/exec、dl_iterate_phdr、sigaction） |
| `engine/src/platform/system/portable/` | 标准库回退后端（无内核对象时使用） |

`engine/src/platform/system/*.cpp` 由根 `CMakeLists.txt` 递归收集，并从引擎源
集合中排除后单独构建为 `shinkou_platform_system` 静态库
（`ShinkouEngine::PlatformSystem`），再以 PUBLIC 依赖链接进 `shinkou_engine`。
新增文件无需手工登记。该库不依赖渲染器/编辑器，因此平台测试可以在不完整
构建整棵引擎的情况下运行。

## 统一接口

```cpp
#include "shinkou/platform/System.h"

using namespace shinkou::platform;

auto system = make_system(BackendKind::Auto); // Windows 上为 Native，其余为 Portable
SystemInfo info = system->info();

// 内核虚拟内存
auto region = system->reserve_memory(1 << 20);
region.value()->commit(0, 1 << 20);
region.value()->protect(0, 1 << 20, MemoryProtection::ReadWrite);

// 线程与同步
auto event = system->create_event(EventOptions{});
std::atomic<bool> done{false};
auto thread = system->create_thread(ThreadDesc{}, [&] { done = true; });
event.value()->set();
thread.value()->join();

// 原生文件
auto file = system->open_file("Saved/report.txt",
                              OpenOptions{OpenMode::ReadWrite, ShareMode::ReadWrite,
                                          true, true});
file.value()->write_all("hello", 5);

// 子进程与管道
ProcessOptions options;
options.executable = "cmd.exe";
options.arguments = {"/c", "echo hello"};
options.capture_stdout = true;
auto process = system->spawn_process(options);
```

进程级入口 `System::instance()` 供暂时无法接收 `ISystem&` 的旧代码使用；它
在首次访问时按 `default_backend()` 惰性创建。`System::set_instance()` 只用于
启动阶段或测试，调用后旧引用失效，不允许与其他线程并发替换。

失败路径一律返回 `Result<T>` 或 `Status`，带可移植 `ErrorCode`、原始
`native_code` 与可读消息：

```cpp
auto mapped = system->map_file(path, 0, 0, MemoryProtection::Read);
if (!mapped.ok())
    log::error("map failed: {}", mapped.error().describe());
```

## 后端与能力

`ISystem::supports(Capability)` 是调用前的能力检查。三个后端：

- **Windows Native**：`Win32`/内核对象全量实现。
- **POSIX Native**（Linux、macOS、BSD）：在 Portable 基线之上叠加内核服务。
- **Portable**：纯 C++17 标准库，作为 Android、Emscripten 和未知平台的回退。

能力不支持时返回 `NotSupported` 或报告相应 `Capability`，不会用语义不同的方式
伪装。

| 能力 | Windows | POSIX | Portable | 说明 |
| --- | --- | --- | --- | --- |
| `VirtualMemory` | 是 | 是 | 是 | Windows `VirtualAlloc`；POSIX `mmap`+`mprotect`；Portable 为页对齐堆内存 + 保护调用 |
| `LargePages` / `PageLock` | 是 / 是 | 否 / 是 | 否 / 是 | POSIX 后端未启用大页；页锁定为 `mlock` |
| `FileMapping` / `SharedMemory` | 是 / 是 | 是 / 是 | 否 / 否 | POSIX 为 `mmap(MAP_SHARED)` 与 `shm_open` |
| `FileWatch` | 是 | 是 | 是 | Windows 用 `ReadDirectoryChangesW`；POSIX/Portable 轮询元数据，可能漏掉等值重写 |
| `Fibers` | 是 | 是 | 否 | POSIX 使用 `ucontext`（macOS 上已废弃但可用） |
| `ThreadAffinity` / `ThreadNaming` | 是 | 仅 Linux | 否 | POSIX 通过 `pthread_setaffinity_np` / `pthread_setname_np` |
| `ThreadPriority` | 是 | 否 | 否 | POSIX 只接受 `Normal`（实时调度需要额外权限） |
| `ThreadLocalStorage` | 是 | 是 | 是 | `TlsAlloc` / `pthread_key` / `thread_local` 槽位表 |
| `NamedKernelObjects` | 是 | 否 | 否 | POSIX 后端暂不暴露跨进程命名互斥量/事件 |
| `ProcessSpawn` / `ProcessCapture` | 是 / 是 | 是 / 是 | 是 / 否 | POSIX 使用 `fork`+`execve` 与管道；Portable 通过 `system()` |
| `ProcessEnvironment` | 是 | 是 | 是 | 进程环境块读写 |
| `DynamicModules` / `ModuleEnumeration` | 是 / 是 | 是 / 仅 Linux | 是 / 否 | POSIX 枚举使用 `dl_iterate_phdr` |
| `StackTrace` / `CrashHandler` | 是 / 是 | 是 / 是 | Windows 上是 / 否 | POSIX 使用 `backtrace` 与 `sigaction`（`SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT`） |
| `ConsoleColors` | 是 | 否 | Windows 上是 | 启用 VT 序列 |
| `PhysicalCoreCount` / `CpuBrand` | 是 | 是 | 否 | POSIX 读取 `/proc/cpuinfo` 或 `sysctl` |
| `WallClockLocalTime` | 是 | 是 | 是 | 本地日历时间 |

## 内核相关操作

- **虚拟内存**：预留、按页提交/撤销、`MemoryProtection` 修改、页锁定/解锁、
  指令缓存刷新、大页尝试、文件映射与命名共享内存。Windows 使用
  `VirtualAlloc`/`VirtualFree`/`VirtualProtect`/`VirtualLock`/
  `FlushInstructionCache`/`CreateFileMappingW`/`MapViewOfFile`。
- **线程与调度**：`CreateThread` 以挂起状态创建，先应用优先级、亲和性和
  `SetThreadDescription` 名称再恢复执行；支持 `join`/`join_for`/`detach`。
- **同步对象**：递归互斥量使用 `CRITICAL_SECTION`，非递归使用 `SRWLOCK`，
  命名互斥量使用内核 `CreateMutexW`；条件变量为标准
  `CONDITION_VARIABLE`；信号量/事件为内核对象；TLS 使用 `TlsAlloc`。
- **Fiber**：线程先转换为 Fiber，`create_fiber()` 后在“父 Fiber-子 Fiber”之间
  切换；`yield_fiber()` 回到最近一次 `resume()` 的上下文。
- **进程**：`CreateProcessW` 直接传递参数数组（不经过 shell），支持环境块
  覆盖、工作目录、`STARTF_USESTDHANDLES` 管道捕获与 `CREATE_NO_WINDOW`。
- **模块**：`LoadLibraryW`/`GetProcAddress`，并用 Toolhelp32 枚举已加载模块
  与地址归属。
- **诊断**：`OutputDebugStringW`、`IsDebuggerPresent`、`DebugBreak`、
  `CaptureStackBackTrace`、`SetUnhandledExceptionFilter`。

POSIX 原生后端在 Portable 基线之上提供：

- **虚拟内存**：`mmap(PROT_NONE)` 预留、`mprotect` 提交/保护、`madvise` 撤销、
  `munmap` 释放、`mlock`/`munlock`、`__builtin___clear_cache`；文件映射使用
  `mmap(MAP_SHARED)`，共享内存使用 `shm_open`+`ftruncate`+`mmap`，`msync` 回写。
- **线程**：`pthread_create`/`pthread_join`，完成状态用条件变量支持
  `join_for`；Linux 上 `pthread_setaffinity_np`/`pthread_getaffinity_np` 与
  `pthread_setname_np`。
- **Fiber**：`getcontext`/`makecontext`/`swapcontext`，每个 Fiber 保存调用方
  上下文，支持 `resume()`/`yield_fiber()`。
- **进程**：`fork` 前构建 argv/envp（子进程只做 async-signal-safe 调用），
  `dup2` 接管道，`execve`，`waitpid(WNOHANG)` 支持超时等待与 `SIGTERM`/`SIGKILL`。
- **模块**：Linux 上用 `dl_iterate_phdr` 计算基址/大小并归属地址。
- **诊断**：`backtrace` 抓栈；`sigaction` 注册崩溃处理器后转发
  `CrashReport`，随后恢复默认动作终止进程。

## 契约与限制

- 组合式等待（条件变量 + 互斥量）要求两个对象来自同一后端且使用匿名互斥量；
  命名内核互斥量不能传给条件变量。
- `IThread` 析构等价于 `detach()`，不会隐式阻塞等待；需要结果时显式
  `join()`/`join_for()`。
- 自动复位事件的 `is_signalled()` 使用本地逻辑状态跟踪，因为零超时等待本身
  会消费信号；跨进程命名自动复位事件的信号要等本进程实际等待后才同步。
- 崩溃处理器在故障线程上运行：写文件、写标准流是允许的，但应简短、不能抛出；
  返回后按平台默认行为终止进程。
- 路径一律使用 UTF-8；Windows 后端在边界转换为 UTF-16，临时文件、环境块与
  模块名同样保持 UTF-8。
- Portable 后端的 `reserve_memory` 实际是已提交的页对齐内存（`commit` 只做
  记账），用于让上层代码在非 Windows 平台上跑通，不代表真实的按需提交语义。
- POSIX 后端：共享内存名必须以 `/` 开头且不能包含第二个 `/`；线程优先级只
  接受 `Normal`；命名内核对象、大页与控制台颜色能力未提供；崩溃回调在信号
  处理器内执行，必须 async-signal-safe；macOS 的 `ucontext` API 已废弃但仍可用，
  可能产生编译警告。
- Android 与 Emscripten 不启用 POSIX 原生后端，使用 Portable 回退；Windows 与
  POSIX 原生后端之外的目标按 `make_system(BackendKind::Auto)` 自动选择。
- 本层不做权限沙箱、不检查项目根目录；路径边界仍由编辑器
  `FileSystemService` 负责。

## 测试与验证

Windows：

```powershell
cmake --build out/build/mingw-debug --target shinkou_platform_system_tests --parallel 4
ctest --test-dir out/build/mingw-debug -R '^shinkou_platform_system_tests$' --output-on-failure --timeout 120
```

Linux / macOS（平台层预设关闭了渲染器、编辑器、SDL、PhysX 等依赖）：

```bash
cmake --preset linux-debug          # macOS 使用 macos-debug
cmake --build --preset linux-debug --target shinkou_platform_system_tests --parallel 4
ctest --test-dir out/build/linux-debug -R '^shinkou_platform_system_tests$' --output-on-failure --timeout 120
```

`engine/tests/PlatformSystemTests.cpp` 对**三个后端**执行同一套契约检查：
主机信息、时钟、线程/TLS、互斥量/条件变量/信号量/事件、Fiber、虚拟内存、
文件映射/共享内存、文件与目录、目录监听、子进程与管道捕获、环境变量、动态
模块、栈回溯、控制台，以及通过子进程验证崩溃处理器。能力不支持的分支按
`supports()` 跳过，不作为失败；测试目标只链接 `shinkou_platform_system`。

验证状态：Windows 原生与 Portable 后端在本机通过（直接运行与 CTest，重复
稳定）。POSIX 原生后端的源码已加入构建图，但当前开发机没有 Linux/macOS
工具链（WSL 被拒、Docker 守护进程未启动、外网不可用），**尚未在本机编译
或运行**；首次验证请在 Linux/macOS 上执行上述命令，并据结果修复编译/行为
差异。

## 后续迁移

本轮不替换旧代码。建议的新代码入口是 `make_system()`/`System::instance()`；
后续迁移顺序建议：

1. `platform/Window.h`（窗口创建、文件拖放、DPI）保留为窗口适配层，内部改用
   `ISystem` 的线程/诊断能力。
2. `editor/FileSystemService` 下沉到 `ISystem` 文件接口，保留项目根边界检查。
3. `EditorBuildSystem` / `EditorToolIntegration` 的进程与模块调用改用
   `Process`/`Library`。
4. `NetworkSystem`、`AudioSystem` 等模块逐步移除各自的 `#if defined(_WIN32)`。
