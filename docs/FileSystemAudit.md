# 文件系统与目录结构审计

审计日期：2026-10-19。范围为第一方源码、目录组织、构建入口、编辑器
`FileSystemService`、资源库边界及相关测试；不修改第三方源码。

## 现状

项目已有明确的模块目录、CMake 预设和 CTest 测试。文件访问分为两层：

| 层 | 入口 | 已有职责 |
| --- | --- | --- |
| 编辑器项目文件 | `engine/include/shinkou/editor/FileSystem.h` | 项目根边界、文本读取、原子替换、创建目录、枚举、删除、重命名、轮询快照 |
| 运行时资源 | `assets/include/shinkou/assets/AssetSystem.h` | URI 挂载、异步加载、processor/loader、缓存、依赖图、版本、失效与热重载 |

`assets/` 是库源码，不是用户资源目录。`render/` 为独立实验项目，根
`CMakeLists.txt` 尚未加入它。构建输出放在 `out/`，运行数据位于 `Saved/`
和 `.shinkou/cache/`。

## 已确认并修复

| 缺口 | 原有行为 | 本轮处理 |
| --- | --- | --- |
| 扫描完整性不可见 | 达到数量上限或中途出错都返回一个普通列表 | 新增 `FileListResult`、`list_detailed()`；列表和扫描结果暴露 `complete`、`truncated`、`error` |
| 不完整扫描污染变更基线 | 未枚举到的文件被报告为 `Removed`，恢复完整扫描后又报告为 `Added` | 不完整扫描不发布 changes、不更新快照；完整扫描恢复后再比较真实变更 |
| 递归模式未纳入基线 | 从递归扫描改为直接子项扫描会误报子目录内容被删除 | 目录和递归模式共同界定基线 |
| Windows junction 未可靠解析 | 本机 MinGW 15.2 的 `canonical()` 和 `is_symlink()` 将 junction 当作普通目录 | Windows 使用 `CreateFileW` 和 `GetFinalPathNameByHandleW` 解析实际目标，用原生 reparse 属性判断链接 |
| 列表路径失去条目身份 | `filesystem::relative()` 会按链接目标重写名称 | 发布条目自身的项目相对名称；过滤外部/悬空链接，递归枚举显式跳过目录链接 |
| 删除和重命名跟随最后一级链接 | 操作可能落在目标条目而非链接自身 | 只解析父目录边界；最后一级链接按条目操作，递归删除也不进入 junction 目标 |
| 读取失败残留旧内容 | 调用者可能继续看到上一次成功读取的数据 | 开始读取和读取失败时清空输出；成功调用清除旧错误 |
| 根路径与写入边界 | 字符串拼接的根前缀检查不兼容盘符根；空路径写入可能尝试在根旁创建临时文件 | 使用路径组件判断边界；拒绝写入项目根和 Windows 盘符相对路径；提交前检查 close 结果 |
| 编辑器消费不完整结果 | 扫描结果直接替换可见资源索引和变更基线 | 同一范围已有完整索引时保留索引；首次截断列表可显示部分内容，但不建立完整基线，状态栏报告原因 |
| 缺少项目入口说明和本地产物规则 | 根 README 只有两行，缓存和 node_modules 无忽略规则 | README 补齐目录、构建、依赖和文档入口；忽略 node_modules、资源缓存、用户预设和编译数据库 |

## 扫描约定

- `complete=true` 表示已枚举完该范围内可发布的条目；恰好达到上限且没有更多
  条目仍属于完整扫描。
- `truncated=true` 表示还有可发布条目超出预算，同时 `complete=false`。
- 目录权限、条目元数据读取或迭代失败会令 `complete=false`，并设置 `error`。
- 不存在的扫描目录返回完整空列表，让之前实际存在的内容可以报告为删除。
- 外部链接和悬空链接不出现在列表中；内部目录链接保留自己的名称，但不递归。
- `list()` 保持原有便利接口；需要判断可靠性的调用方使用 `list_detailed()`。
- 一个服务实例只维护一个扫描基线；切换目录或递归模式会开始新基线。

## 验证

`engine/tests/FileSystemTests.cpp` 增加了扫描预算、零预算、恢复完整扫描、无效
范围、递归切换、扫描目录被删除、读取输出、空文件、中文路径、盘符根、链接
边界及链接操作测试。Windows 测试原生创建 junction，无需管理员权限或
Developer Mode；不支持目录链接的文件系统会明确输出跳过原因。

先用新增回归在原实现上复现 `bounded.changes.empty()` 断言失败，再验证修复。
独立静态链接的文件系统测试通过，包括实际创建的 junction；未跳过链接用例。
根构建的 `shinkou_engine` 及 6 个相关测试目标编译成功。6 项相关 CTest
（文件系统、资源预览、资源索引、构建系统、编译数据库、项目集成）全部通过。

本机默认 PATH 优先加载 Anaconda 自带的旧 MinGW DLL，最初 6 项测试均在
启动阶段报 `0xc0000139`。将 `D:\paths\GNU\mingw64\bin` 提前后，6/6
通过；本轮没有修改系统 PATH。测试命令：

```powershell
$env:PATH = 'D:\paths\GNU\mingw64\bin;' + $env:PATH
ctest --test-dir out/build/mingw-debug -R 'shinkou_(file_system|editor_asset_index|editor_project_integration|editor_compile_commands|editor_build_system|asset_preview)_tests' --output-on-failure --timeout 30 --parallel 4
```

EngineDev MCP 的默认构建目录 `out/build/editor` 不存在；实际使用已有的
`out/build/mingw-debug` 完成 CMake 构建。若使用该 MCP，需配置
`ENGINE_BUILD_DIR`。本轮未进行编辑器窗口交互或像素验收，也未声称全量测试通过。

Windows 实现依据：
[GetFinalPathNameByHandleW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfinalpathnamebyhandlew)、
[RemoveDirectoryW 的 junction 语义](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-removedirectoryw)。

## 尚未覆盖的能力与仓库问题

- 文件监控仍为按时间戳和大小轮询，不能保证捕获保持这两个值不变的内容替换；
  可按资源加载或文本冲突检测的实际需求增加内容 hash 或原生监控。
- 路径检查与实际 IO 之间仍可能发生外部并发替换。本服务不是用于隔离恶意进程
  的操作系统沙箱；若要提供此保证，需要用目录/文件句柄贯穿整个操作。
- 不完整目录扫描目前没有分页/续扫接口。超过编辑器的 32768 条预算时会给出
  状态提示；后续可在此基础上实现增量遍历。
- 资源库自身的挂载与 `scan_sources()` 有独立实现；本轮修复只覆盖编辑器文件
  服务，不代表资源库及其他直接使用 `std::filesystem` 的模块都已采用相同策略。
- 仓库历史已经跟踪 `Tools/node_modules`；新增忽略规则只阻止新文件加入，不会
  移除已跟踪依赖。清理历史依赖应单独处理，并保留 `package-lock.json`。
- 根目录没有项目许可证和 CI 配置。许可证需由项目所有者选择；CI 应按实际目标
  平台和依赖获取方式落地，不能用空文件或未经确认的许可证替代。
