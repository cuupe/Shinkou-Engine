# 第三方依赖

本目录是第三方依赖的统一入口：

- `dependencies.json`：普通库/字体的固定版本、官方归档 URL、SHA-256、许可证路径。
- `patches/`：本项目确实需要的上游集成补丁。目前仅 ImGui 的 DX12/Vulkan 后端补丁。
- `PhysX/`：大型 SDK 的 Git 子模块，固定提交由主仓库 gitlink 记录。

`external/` 已合并到这里。两个旧目录存放的库不同，没有重复保存 PhysX；
此前分别使用源码副本和子模块，职责与命名不统一。

## 获取和缓存

CMake 默认下载固定归档并校验 SHA-256，再将源码展开至构建目录的 `_deps/`。
源码和下载缓存不提交 Git。项目自行选择所需源文件，普通依赖不会启动上游的
示例、测试或递归下载。只有显式启用 SDL3 输入时才配置 SDL3 自身的构建。

```powershell
cmake -S . -B out/build/mingw-debug -DSHINKOU_FETCH_DEPENDENCIES=ON
cmake --build out/build/mingw-debug --parallel 4
```

首次配置需要联网。依赖缓存齐全后可关闭下载：

```powershell
cmake -S . -B out/build/mingw-debug -DSHINKOU_FETCH_DEPENDENCIES=OFF
```

缺少缓存时配置会明确失败。切换启用的功能后，新依赖也需要先下载。
`SHINKOU_FETCH_DEPENDENCIES` 对 Core、Assets 独立构建同样生效。

CMake 标准选项 `FETCHCONTENT_BASE_DIR` 可以指定一个共享缓存目录；
`FETCHCONTENT_SOURCE_DIR_SHINKOU_<NAME>` 可以显式指定已有源码，例如
`FETCHCONTENT_SOURCE_DIR_SHINKOU_JSON`。该选项用于本地开发或已准备的离线源码，
调用方负责提供与清单一致的版本；本地源码覆盖不经过归档下载和哈希校验。
SDL3 也保留 `SHINKOU_INPUT_SDL3_SOURCE_DIR` 和 `SHINKOU_INPUT_FETCH_SDL3` 开关。

## 依赖范围

| 依赖 | 版本 | 获取时机 |
| --- | --- | --- |
| spdlog | 1.16.0 | 根工程日志 |
| nlohmann/json | 3.12.0 | Core，包括独立 Assets |
| ufbx | 0.20.0 | 资产预览读取器 |
| miniz | 3.1.1 | DOCX/ZIP 预览 |
| pugixml | 1.15 | OOXML 预览 |
| miniaudio | 0.11.21 | 启用 miniaudio |
| Dear ImGui | 1.91.9b-docking | 启用编辑器 UI |
| HelloImGui | 1.6.3 | 启用编辑器 UI，仅主题模块 |
| ImPlot | 0.16 | 启用编辑器 UI |
| ImGuizmo | 105e4fde45e4e74aae8973d98750820ef7aca7e2 | 启用编辑器 UI |
| Codicons | 0.0.36 | 启用编辑器 UI，仅字体 |
| EnTT | 3.16.0 | 启用对比基准；不传播至引擎公共头文件 |
| SDL3 | 3.2.0 | 启用输入 DLL |

清单中的完整 revision 与 SHA-256 是实际构建依据。系统 SDK（Vulkan 等）和可选
语言运行时继续由本机工具链提供，不作为源码复制进仓库。

## PhysX

PhysX 固定到 `da950a3537927784951853c66618036f332ca0ce`，使用：

```powershell
git submodule update --init --recursive third_party/PhysX
```

主仓库只记录子模块 URL 和提交号，不包含它的整套源码。PhysX 不随普通 CMake
配置自动下载或构建；它需要与本机编译器/配置一致的 SDK 产物。配置方法见
[PhysX 集成](../docs/PhysicsIntegration.md)。

## 补丁与许可证

ImGui 补丁在已校验的源文件上应用；再次配置可识别已应用状态，无法匹配时配置
直接失败。下载后的 4 个后端文件与迁移前版本一致；其他依赖文件保持上游原样。

各归档中的上游许可证随安装复制至 `share/shinkou/licenses/<name>/`，Codicons
字体另安装至 `share/shinkou/editor/fonts/`。修改依赖版本时同时更新归档哈希、
许可证路径及受影响补丁，并验证相关功能构建和测试。

## 迁移验证（2026-11-02）

- 原有 161 个依赖文件与固定归档/应用补丁后的内容对比一致（忽略 CRLF/LF 差异）。
- Windows x64 / MinGW Release，ImGui、Vulkan、miniaudio 启用，PhysX 后端关闭：
  全量构建成功，CTest 83/83 通过。测试使用工作区内可写的 `TEMP`/`TMP`。
- 独立 Core 测试 1/1、独立 Assets 测试 2/2 通过，均可从共享缓存配置和构建。
- 独立输入 DLL 通过固定 SDL3 3.2.0 源码完成静态 SDL + DLL 构建。
- 根工程及独立输入工程在关闭下载后均重新配置成功，ImGui 补丁可重复识别。
  全新构建目录在关闭下载且无缓存时按预期失败。
- 安装验证覆盖当前启用的 12 个依赖许可证、Codicons 字体和 spdlog 公开头文件。
- PhysX 子模块路径迁移前后均为同一提交，本轮没有构建其 SDK。
