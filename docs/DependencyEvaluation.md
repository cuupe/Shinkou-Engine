# 依赖评估：LZ4/Zstd、Abseil、Boost

评估范围：根构建图（`engine/`、`assets/`、`render/`）及 `third_party/`、`cmake/` 的现有依赖方式。
`Tools/EngineDevMCP` 是 Node 侧（stdio MCP + 进程管理），不在本评估内。

## 结论

| 候选 | 现在引入 | 核心依据 | 重新评估的触发条件 |
| --- | --- | --- | --- |
| Zstd | 否 | 没有会用到压缩的数据形态：没有资产包、没有二进制存档、网络模块未启用 | cook 出资产包、资源下载/更新、存档或遥测二进制膨胀 |
| LZ4 | 否 | 没有需要"压缩+解压都极快"的热路径 | 远程视口/帧流、网络快照、大块内存实时传输 |
| Abseil | 否 | C++17 常规设施已够用；默认 MinGW 不在其官方支持矩阵内；构建面大 | profiler 证明容器/格式化是热点，且先完成 MinGW 验证 |
| Boost | 不建议 | 无独占能力，且与"vendored 单文件依赖"模型冲突 | 需要 asio 级网络时优先 standalone asio，而不是整个 Boost |

一句话：当前缺的不是压缩算法，而是**会用压缩的数据形态**；Abseil/Boost 解决的是通用设施问题，
而本项目的通用设施问题（重复的 JSON 解析器）用 Boost 解决反而是最重的方案。

## 现状事实（可复查）

- 构建：CMake 3.25+、C++17。默认预设 `mingw-debug`，本机为 MinGW GCC 15.2.0
  （`D:/paths/GNU/mingw64/bin/c++.exe`）；`windows-vs` 预设尚未生成过构建目录（`out/build/vs` 不存在）。
- 规模：`engine/`、`assets/`、`render/`、`Tools/` 共 332 个 C/C++ 源文件、约 86,000 行；仓库 44 次提交。
- 依赖模型：单文件/源码子集 vendored（ufbx、miniz、pugixml、imgui、implot、ImGuizmo、hello_imgui、
  codicons、entt、miniaudio），SDL3 走 FetchContent，spdlog 可选探测，PhysX 为 Git 子模块。
  固定版本与用途记录在 `third_party/ASSET_READER_LIBRARIES.md`，许可证随源码入库。
- 压缩现状：全仓唯一的压缩调用点是编辑器文本预览读取 DOCX/ZIP
  （`engine/src/editor/EditorTextPreview.cpp:71-100`，miniz 的 `mz_zip_reader_*`），测试用它造 fixture。
  此外没有 zlib/zstd/lz4 的任何调用。
- 资源读取是直接读源文件：`AssetSystem::load` 从源路径读取字节后跑 processor/loader；磁盘缓存把
  artifact payload 原样写入（`assets/src/AssetSystem.cpp:110-123`），没有压缩、没有打包容器。
  本地 `.shinkou/cache` 目前没有任何缓存文件，仓库里也几乎没有可压缩的内容资源。
- 场景、文档、主题、布局都是 JSON 文本（例如 `EditorDocument::from_json` 限 16 MiB），可读性是刻意保留的。
- 网络模块虽然被 `Engine` 持有并在启动时初始化（`engine/src/core/Engine.cpp:48`），但
  `NetworkConfig` 的 HTTP/TCP/UDP 默认全关，示例和编辑器从未打开，只有 `engine/tests/NetworkSystemTests.cpp`
  真正驱动它——目前是待接入的脚手架，不构成压缩消费方。
- 渲染后端的编辑器路径是 D3D11，图像解码走 WIC，纹理以 CPU 像素数据上传；没有 shader/PSO 二进制缓存目录。

## LZ4 / Zstd

### 为什么现在都不需要

1. **主要用途缺席。** 这两个库的价值在打包资产、下载更新、网络传输、大存档；上述路径目前都不存在。
   现有资源系统是"源文件直读 + 本地缓存"，不是"发布包"。
2. **miniz 不能被替换。** DOCX/ZIP 是 DEFLATE 格式契约，zstd/lz4 读不了；引入后是并存，不是替代，
   依赖面只会变大。miniz 只服务于受限的预览读取，且已按安全边界封装（1 MiB/64 MiB/8 MiB 预算）。
3. **缓存是本地开发产物。** 压缩 `.shinkou/cache` 是用解压延迟换磁盘占用；在 NVMe 本地盘上收益不明，
   而且当前缓存目录是空的，没有可测样本。
4. **网络与存档都不成立。** 网络模块未启用；场景是刻意可读、可 diff 的 JSON，压缩会破坏这个属性。
5. **没有性能压力证据。** 仓库没有能证明"IO 或带宽是瓶颈"的 profile 记录；先引入库属于凭直觉优化。

### 触发条件与选型

| 场景 | 选择 | 用法 |
| --- | --- | --- |
| cook 后的资产包、DLC/补丁下载、超大存档 | Zstd | cook 时 level 3–6；运行时可用解压-only 库（`zstddeclib`），体积和攻击面都更小 |
| 实时视口/帧流、网络快照、编辑器大块内存传输 | LZ4 | 块级（64–256 KB）+ 每块独立压缩，单块损坏不影响整体 |
| 需要按 chunk 随机访问的包 | Zstd 或 LZ4 | 包格式先定"每 chunk 一条压缩记录"，压缩算法作为字段而非格式前提 |

**引入形态**（与现有约定一致）：官方单文件合并版本放入 `third_party/<lib>/`，作为独立静态库或在
`shinkou_asset_readers` 旁新增 target，`SYSTEM PUBLIC` include，并在 `third_party/ASSET_READER_LIBRARIES.md`
补固定版本、用途、许可证（Zstd BSD-3-Clause，LZ4 BSD-2-Clause）。

**不要在引入时顺手做的事**：把 ZIP/DOCX 读取换成 zstd；在热路径对小块数据做压缩；把压缩写进
`AssetSystem` 的缓存格式而不带版本号（缓存格式版本是 `CacheVersion`，改格式必须递增）。

## Abseil

### 为什么现在不需要

- **基础已具备**：C++17、`std::string_view`（约 485 处引用）、entt、`std::unordered_map`（约 41 处引用）。
  这个规模下，容器查找不是瓶颈；`absl::flat_hash_map` 的收益需要 real-world profile 才能兑现。
- **缺口可以更便宜地填**：`absl::Span` 对应 ~40 行的 `Span<T>`；`absl::StatusOr` 对应一个 `Result<T>`
  （项目里已经有 `SerializationResult`、`AssetLoadResult` 这类同构返回类型，统一它们比引入 Abseil 更划算）。
- **平台风险（关键）**：Abseil 的受支持平台遵循 Google Foundational C++ Support Policy，Windows 侧以
  MSVC 为准；MinGW 不在该矩阵内，属于未验证路径。本项目**默认预设就是 MinGW GCC 15.2**，
  引入后很可能只在 VS 预设下可靠，等于给默认构建加了一条隐性依赖。
- **架构一致性**：PhysX 自带 Foundation 的 allocator/容器/callback，std 是一套，Abseil 会是第三套工具层。

### 什么时候再考虑

- 有 profiler 数据表明容器查找或字符串格式化占据可观的帧时间（先测量，再引入）；
- 需要跨模块统一的错误链语义（payload + code + message 链），且自己写两版 `Result` 后仍不够；
- 决定把默认工具链迁到 MSVC/Clang 之后。

即便如此，也建议**只挑需要的子库**（通常只有 `absl::flat_hash_map` 与 `absl::StrFormat`），
而不是整套 Abseil。

## Boost

### 为什么不建议

- **没有独占能力需求**：文件系统用 `std::filesystem`，variant/optional 用 std，序列化有自建 reflection
  （`engine/src/reflection/Serialization.cpp`），正则/日期/命令行都没有需求。
- **与依赖模型冲突**：项目现有依赖是"vendored 单文件 + 固定 commit + 许可证入库"，Boost 需要整套构建
  （b2/CMake）与版本升级策略；MinGW 上构建全量 Boost 的成本和时间都不小。
- **如果真正缺的是 JSON**：见下节——用单头文件 JSON 库比 Boost.JSON 更契合本项目，
  后者还牵出 Boost.Container/System 等编译依赖。
- **如果真正缺的是网络**：选 standalone Asio（header-only、无 Boost 依赖），而不是整个 Boost。

## 比引入这些库更值得做的事

1. **收敛 JSON 解析器**：目前有 8 处独立的 `JsonParser` 实现——
   `engine/src/editor/DockLayout.cpp:359`、`engine/src/editor/EditorCompileCommands.cpp:28`、
   `engine/src/editor/EditorGltfPreview.cpp:46`、`engine/src/editor/EditorToolIntegration.cpp:43`、
   `engine/src/reflection/Serialization.cpp:34`、`engine/src/ui/Components.cpp:62`、
   `engine/src/ui/Media.cpp:31`、`engine/src/ui/Theme.cpp:52`。
   这是仓库里真实存在的重复，且每处都要各自维护"深度上限/体积上限/重复键/数字有限性"的规则。
   若要加依赖，这里才是有量化收益的位置（单头文件、可关异常、可设分配上限的实现优先）。
2. **明确依赖政策**：当前 vendored、FetchContent、子模块三种方式并存。建议对新增依赖固定要求：
   固定版本/commit、许可证入库、在 `third_party/*.md` 记录用途与"为什么不用 std/自研"、以及在
   `mingw-debug` 与 VS 两个预设上都构建通过。
3. **格式先行**：将来设计资产包时，先把"压缩算法 + 每 chunk 压缩后长度"写进格式头，避免后续改格式。

## 若将来决定引入，最小验证清单

- `mingw-debug` 与 VS 预设全量构建 + `ctest` 通过；记录构建时间与二进制定量变化。
- 用真实项目资源测：压缩率、压缩/解压吞吐、峰值内存、缓存命中路径的加载延迟。
- 许可证与 NOTICE 更新到 `third_party/`，并在 `third_party/ASSET_READER_LIBRARIES.md` 登记。
- 压缩路径必须有"未压缩回退"分支，避免损坏数据导致整包不可读。
