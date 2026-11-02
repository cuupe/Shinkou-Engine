# ECS chunk 存储

本文记录运行时对象存储的重构：EnTT 稀疏集被替换为 DOTS 风格的
**archetype + 16 KiB chunk + 8 字节实体句柄**，作为运行时唯一数据真源；
`GameObject` 保留为 authoring/编辑器门面。

迁移边缓存、命令页复用、并行命令分区和组件读写依赖调度见
[ECS 运行时优化](EcsRuntimeOptimization.md)。

## 目标与边界

- 运行时热数据按 **类型连续（SoA）** 存放，批量遍历没有逐实体间接寻址。
- 实体句柄固定 8 字节，解析是"一次数组索引"，不是哈希。
- 查询结果按 archetype 缓存，迭代单位是 chunk。
- 结构性变更（增删组件）通过 archetype 迁移完成，语义与 DOTS 一致。
- 保留编辑器所需的一切：`GameObject` 树、`Component` 生命周期、属性反射、
  `.scene` 序列化（chunk 数据不参与序列化）。

不在本次范围内：`ISharedComponentData`、enableable 位图、按类型 change filter、
buffer/blob 组件、SubScene/Baking、Burst 等价编译器。这些是后续增量。

层级元数据仍由 `GameObject` 树保存；**世界矩阵已经数据化**（见下文
"世界矩阵管线"），渲染提取不再逐对象访问 authoring `Transform`。

## 内存结构

```text
EntityStore
├── EntityInChunk table[]      // index -> {Chunk*, slot}，一次索引定位
├── versions[]                 // index -> generation
├── freeIndices[]              // LIFO 回收
└── Archetype[]                // 按组件类型集合唯一
    ├── types[]                // 升序 TypeId，签名
    ├── offsetsById[]          // 每类型在 chunk 内的字节偏移（0 = 不存在）
    └── Chunk*[]               // 16 KiB 块

Chunk (16 KiB, 64-byte aligned)
┌────────────────────────────────────────────────────┐
│ header: Archetype* count capacity index ver        │
│ Entity   entities[capacity]        ← 8B/实体       │
│ Position positions[capacity]       ← 连续           │
│ Velocity velocities[capacity]      ← 连续           │
└────────────────────────────────────────────────────┘
```

同一个 chunk 内所有数组共享一个下标空间：第 i 个实体的 `Position` 在
`positions[i]`，`Velocity` 在 `velocities[i]`。因此遍历一个 chunk 只需要
一次指针解引用和 N 次连续读写；不同类型的数组虽然没有交错，但**处在同一个
16 KiB 块内**，缓存与 TLB 局部性明显优于"每类型一个全局大数组"。

容量计算（`Archetype::compute_layout`）从
`(16384 - header) / entityStride` 开始向下收缩，直到含对齐填充的精确布局不超过
16 KiB。例：`Position(12B) + Velocity(12B)` → 511 实体/chunk、stride 32B。

## 类型系统

`component_type_info<T>()` 在首次使用时注册 `TypeId`、名字、大小、对齐、是否
tag，以及 `construct/destruct/copy` 三个函数指针：

- tag 组件（`std::is_empty_v<T>`）不占 chunk 空间，只参与签名；
- 平凡拷贝类型走 `memcpy`，其他类型走逐元素拷贝构造；
- `type_id<T>()` 内部先注册再返回 id，因此不会出现"有 id 没描述"的窗口。

`TypeId` 是进程内自增，archetype 签名按 id 升序保存；跨进程不保证一致（这与
DOTS 的 TypeManager 不同，但没有持久化需求）。

## 实体与句柄

`shinkou::Entity { uint32 id; uint32 generation; }`，`sizeof == 8`。

- `create()` 复用 LIFO 空闲下标，否则追加；`versions[id]` 保持当前代际。
- `destroy()` 先析构组件、swap-back 释放槽位，然后 `++versions[id]`。
- `valid(entity)` 同时检查 `id != 0`、槽位在用、代际匹配，因此旧句柄不会误判。
- `clear()` 销毁全部实体（每个都递增代际）后重置下标游标，清空前的句柄全部失效。

`EntityInChunk` 表按下标直接寻址：`locate(entity)` 是一次边界检查 + 一次数组读。
`World` 另有一张 `entity id -> GameObject*` 扁平表，所以 `World::find_object(Entity)`
也不再哈希。

## 结构性变更

`emplace<T>` / `remove<T>` 是 archetype 迁移：

1. 计算目标 archetype（源类型集合 ∪/∖ {T}），必要时创建。
2. 在目标 chunk 分配槽位（`count++`）。
3. 在新槽位构造 `T`（`emplace` 的实参直接落位）。
4. `copy_shared`：把源实体与目标 archetype 共有的组件逐类型拷贝到新槽位。
5. 写入实体数组，更新 `table[id]`。
6. `destroy_slot`：析构源槽位的全部组件；`release_slot`：把源 chunk 最后一个
   实体 swap-back 到空洞处，`count--`；chunk 变空则归还分配器。

**失效规则**：迁移会改变实体所在地址，所以任何组件指针在下一次结构变更后都无效；
chunk 指针、实体句柄保持稳定。作者代码应持有 `Entity + 类型`，或在同一段无结构
变更的代码里使用指针。

`remove<T>` 走同一流程，跳过 `T` 的拷贝并在源侧析构它。删除最后一个组件会迁移到
空 archetype。

## 分配器与 chunk 生命周期

`ChunkAllocator` 以 16 KiB + 64B 的块为单位分配，块在 store 生命周期内不释放，
chunk 指针因此永远稳定（`EntityInChunk` 可以裸存 `Chunk*`）。释放的 chunk 进入
空闲链表，按 archetype 重新初始化 header 后复用。

archetype 维护 `firstPartialChunk` 提示：分配槽位从该位置向后扫描；`release_slot`
把提示回退到刚腾出的 chunk，避免每次从头扫描。空 chunk 立即回收（空 archetype 例外，
保留一块用于实体创建/销毁的抖动）。

## 世界矩阵管线

`ecs::LocalToWorld`（`math::Mat4`，64 B）是渲染读取的世界矩阵，存放在 chunk 里。
每个 `ObjectStorage::Ecs` 对象在创建时自动获得该组件；`LocalTransform` 暂未数据化，
authoring `TransformComponent::local` 仍是编辑面。

每帧分三步，静态场景在第 2 步前就返回：

1. **脏列表**：`TransformComponent` 的 setter 调用 `notify_transform_changed()`，
   把对象 id 推进 `pendingTransformUpdates_`。常规对象在世界上出现第一个 ECS
   对象之后也走这条路径，因此混合层级不会读到过期矩阵。
2. **按深度分配**：把每个脏对象的**整棵子树**按 `hierarchyDepth_` 推进
   `dirtyTransformLevels_[depth]`，同层对象互不依赖，可以并行。
3. **逐层计算**：从 depth 0 开始，每层调用
   `jobs::parallel_for(level.size(), 32, kernel)`：
   - ECS 对象：`LocalToWorld = parentWorld × TransformMatrix(local)`，并回写
     authoring `world_matrix_`，编辑器 gizmo / 拾取看到的矩阵与运行时一致；
   - 常规对象：走原来的 `update_world_transform()`（内部有 dirty 与父矩阵比较）。
   - 父矩阵来源：ECS 父 → chunk `LocalToWorld`；常规父 → authoring `world_matrix()`。

静态场景没有脏对象，变换阶段成本为 0；动画场景按层并行。`World::refresh_transforms()`
（编辑器暂停模式）走同一条管线。

## 查询

`Query` 持有 `all`/`none` 签名，按需重建匹配的 chunk 列表：

- 缓存键是 `store.structure_version()`；只有 archetype 创建或 chunk 增删会递增，
  实体增删（未改变 chunk 集合）不会让查询失效。
- `each<Ts...>(fn)`：chunk 主序，每个 chunk 内先取出各类型数组指针，再线性遍历。
- `each_chunk<Ts...>(fn)`：把 `(Chunk&, T*...)` 交给调用方，用于并行或自定义 kernel。
- `query<Ts...>()` 结果按查询签名缓存在 store 内，热路径零分配。

因为实体只属于一个 archetype，匹配结果天然不相交，`entity_count()` 就是各 chunk
`count` 之和，不需要去重。

## 并行分块

`shinkou/ecs/Parallel.h` 提供：

```cpp
ecs::parallel_chunks<Position, Velocity>(jobs, query, grain, [](Chunk& chunk, Position* p, Velocity* v) {
    for (uint32_t i = 0; i < chunk.count; ++i) p[i].x += v[i].x * dt;
});
```

工作项是 chunk（或每 `grain` 个 chunk 一个），与 DOTS 的 `IJobChunk` 分区方式一致。
回调运行在工作线程，**禁止结构性变更**；需要创建/销毁实体时记录到 `CommandBuffer`。

`World::set_job_system()` 允许宿主注入线程池；设置后根子树数量足够多时，
Transform 快照阶段会按根子树并行（根之间没有共享状态）。

## 命令缓冲

`CommandBuffer` 记录 create/destroy/add/remove/set，`playback()` 在同步点顺序执行。
buffer 内创建的实体返回占位句柄，后续命令在 playback 时解析成真实实体，因此系统可以
"创建实体并立刻给它加组件"而不触发迁移。

## 实测

环境：Windows / MinGW GCC 15.2 / 32 逻辑核，`RelWithDebInfo` + 基准目标 `-O3`，
10 万实体、240 帧、2 个 3-float 组件；数字取空闲机器上 3 次运行的区间。

| 路径 | 构建 10 万实体 | 每帧更新 |
| --- | --- | --- |
| 常规 OOP（`World::update` 全帧，含 Transform 树与销毁扫描） | 50–52 ms | 9.3–9.7 ms |
| EnTT 稀疏集（`view<Position,Velocity>().each`） | 4.4–5.1 ms | 0.29–0.33 ms |
| chunk：`create()` + 两次迁移 | 30–33 ms | 0.052–0.055 ms |
| chunk：`create(archetype)` 一次性落位 | 4.9–5.9 ms | 0.050–0.056 ms |
| chunk：销毁 10 万实体 | — | 2.0–2.1 ms（整体） |
| chunk 并行（grain 1 / 8 / 32） | — | 0.064–0.103 ms |

结论：

- 稳态更新比 EnTT 稀疏集快 **5–6×**，比常规对象全帧快约两个数量级（后者包含
  Transform 层级与对象树扫描，不是同一口径）。
- 一次性按 archetype 创建比"创建后逐组件迁移"快约 **6×**；批量生成应优先
  `create(archetype)` 或预先确定 archetype。
- 裸双数组 SoA（两个相距 1.2 MB 的 `std::vector`）实测 0.164–0.172 ms，反而慢于
  chunk 的 0.050–0.056 ms：同 archetype 组件共处一个 16 KiB 块带来的局部性是主要差异。
- 当前规模下并行**没有收益**（196 个 chunk，工作项太细，调度与内存带宽主导）。
  下一步应把 chunk 合并成更大的批、或让系统在更高层并行，而不是逐 chunk 提交。
- `ObjectBenchmarks`（5 万对象走完整 `World::update`）：常规对象 4.5 ms/帧，
  ECS 对象 **0.75 ms/帧（约 6×）** —— 组件循环快速路径与 chunk 世界矩阵共同作用。
- 世界矩阵管线（10 万 ECS 对象）：静态场景每帧 **~0.01 µs**（脏列表为空，直接返回）；
  全对象每帧动画时串行 30–37 ms → 按层并行 **11–16 ms（2.3–2.6×）**。
- 同样的 10 万对象走完整 `World::update`：ECS 对象 **2.8–3.5 ms/帧**，常规对象
  11–12.5 ms/帧（约 3.5–4.5×）。

复现：

```powershell
cmake -S . -B out/build/dots -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DSHINKOU_BUILD_TESTS=ON -DSHINKOU_BUILD_BENCHMARKS=ON -DSHINKOU_ENABLE_SDL3_INPUT=OFF
cmake --build out/build/dots --target shinkou_ecs_chunk_benchmarks -j 16
out/build/dots/shinkou_ecs_chunk_benchmarks.exe
```

## 使用规则

1. chunk 里只放可平凡拷贝的 POD；authoring 状态留在 `Component`。
2. 跨帧不要保存组件指针；保存 `Entity` 或 `(Entity, 类型)`。
3. 系统内的结构性变更走 `CommandBuffer`，在同步点统一 playback。
4. 需要 entity→GameObject 时用 `World::find_object(Entity)`（O(1)），不要自建 map。
5. 批量生成使用 `EntityStore::create(archetype)`；`Registry::create()` + 多次
   `emplace` 会支付迁移成本。
6. 并行回调只读写传入的数组；不要在里面创建/销毁实体或增删组件。
