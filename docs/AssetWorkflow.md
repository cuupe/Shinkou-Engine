# Shinkou 资源管理工作流

资源核心位于独立的 `shinkou_assets` 静态库中，目录为 `assets/`。主引擎只通过 target 和公开头文件消费它，资源库本身不依赖 renderer、window、UI、ECS 或音频后端，可单独构建和测试。

Shinkou 的资源系统以 `AssetKey { uri, type }` 作为唯一身份。模块不再直接持有相对文件路径，而是通过 `AssetSystem` 请求虚拟 URI；例如 `project://textures/player.png`。URI 由 mount 解析到物理路径，禁止通过 `..` 越过 mount 根目录。

## 运行时流程

```text
URI + type
   │ canonicalize
   ▼
Record（稳定 AssetId、当前 generation、状态）
   │ request / request_async
   ▼
读取源文件 → processor 导入 → loader 构造 AssetData
   │                         │
   │                         └─ 记录硬/软依赖并更新反向依赖图
   ▼
发布不可变 shared AssetData → 事件 → 使用者取得版本句柄
```

同一个 key 的并发请求共享同一个 future，避免重复 IO 和重复导入。processor 负责把源文件转换为平台无关 artifact，loader 负责把 artifact 变成运行时数据；GPU、音频、脚本等模块应分别注册自己的 processor/loader，而不是各自维护第二套文件缓存。

## 版本、热重载与失败策略

- `AssetId` 对 key 稳定；每次成功发布都会递增 `AssetGeneration`。
- `AssetHandle` 同时包含 id、generation 和 key。重新加载成功后旧句柄失效，使用者可用 `AssetLoadResult::handle` 获取新句柄。
- `poll()` 使用源文件时间戳和大小检测变更。直接调用 `invalidate()` 可立即触发变更传播。
- 依赖图沿硬/软依赖向上传播：源资源失效时，所有依赖它的已加载资源进入 `Stale`。
- 热重载期间保留上一版 `AssetData`；新版本导入失败时记录 `lastError`，状态回退为 `Ready`，避免渲染和音频在编辑器中出现空洞。
- `reload_async()` 会重载目标及其依赖者；`reload_stale()` 可在工具或场景切换时批量重新排队所有 stale 记录。

## 缓存与内存

磁盘缓存位于 `.shinkou/cache`。缓存键包含源 hash、processor 版本、loader 版本以及每个依赖的源 hash；因此更换导入器、运行时构造器或依赖内容后不会错误复用旧 artifact。缓存格式版本变更会被安全地忽略。

内存预算只统计当前由系统持有的已发布数据。`pin()` 用于场景、关卡或编辑器面板声明长期使用；`trim()` 采用最近最少使用策略，只回收未 pin 的数据，保留记录和依赖图以便再次请求。

## 接入约定

1. 启动阶段创建并初始化 `AssetSystem`，先添加 mount，再注册 processor、loader 和扩展名映射。
2. 业务代码只保存 `AssetHandle` 或 `AssetKey`，不保存拼接出的物理路径。
3. processor 在 `AssetArtifact::dependencies` 中声明依赖。必需依赖标记 `hard=true`，可选依赖标记 `hard=false`。
4. 资源发布事件回调只做轻量的失效通知；GPU 对象创建、UI 缩略图和其他主线程工作由消费方在自己的安全点执行。
5. 编辑器扫描使用 `scan_sources()` / `write_manifest()`，运行时加载使用 `request()` / `load()`；不要在 paint 或渲染提交热路径扫描文件。

## 当前边界

系统已经提供统一身份、mount、异步加载、导入/构造分层、依赖传播、热重载、版本句柄、缓存、内存回收、manifest 和事件总线。具体 PNG、gltf、材质、场景或音频格式的解码器属于插件/模块，只需注册对应 type，不需要修改资源核心。
