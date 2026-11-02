# ECS 运行时优化（2026-11-02）

本轮优化覆盖结构变更、命令记录与系统调度；保留现有 GameObject 与
`EcsSystem` 生命周期。所有性能数字都是特定基准的结果，不代表整帧或 Unity DOTS
对比。场景 codec、Baking、C# Dictionary 存储不在本轮改动范围。

## 结构变更与命令内存

- Archetype 缓存 add/remove 的双向迁移边。第一次建立目标 archetype 后，同一路径
  直接查边，不再为每个实体分配、排序、哈希组件类型集合。边随 store 一起销毁。
- Chunk 移除使用 `indexInArchetype` 直接定位并 swap-back，复杂度由 O(chunk 数)
  变为 O(1)。删除尾 chunk 时不再访问已经越过 vector 末尾的索引。
- 非平凡组件 swap-back 后析构原末尾元素，避免已退出 live range 的对象泄漏。
  类型描述同时记录平凡析构特征，普通数据组件跳过无效析构调用。
- CommandBuffer 用保留的 16 KiB payload 页代替每条命令 `new/delete`，支持 payload
  对齐与非平凡析构；playback/clear 重置页游标并复用容量。`reserve()` 预留命令数组。
  峰值容量保留到 buffer 销毁；需要归还峰值内存时重建 buffer。

## 显式读写系统管线

```cpp
struct Position { float x, y, z; };
struct Velocity { float x, y, z; };

auto& store = world.ecs().store();
auto& query = store.query<Position, Velocity>();
world.chunk_pipeline().add<Position, const Velocity>(query,
    [](shinkou::ecs::Chunk& chunk, Position* p, const Velocity* v) {
        for (std::uint32_t i = 0; i < chunk.count; ++i) {
            p[i].x += v[i].x * (1.0f / 60.0f);
            p[i].y += v[i].y * (1.0f / 60.0f);
            p[i].z += v[i].z * (1.0f / 60.0f);
        }
    }, 8); // grain 的单位是 chunk
world.set_job_system(&jobs);
```

`const T` 声明只读，`T` 声明写入。调度器在注册变化后构建依赖图：相同组件类型
有任意一方写入时，后注册系统依赖先注册系统；只读/只读、不同类型可以重叠。
这里采用组件类型级别的保守依赖，不尝试证明两个查询的实体集合是否相交。

每帧先在调用线程准备所有 query 缓存，再发布根任务。最后一个前驱完成后发布后继；
在整个 pipeline 末尾 join，不设置全局逐波屏障。每个 kernel 也可通过现有 JobSystem
按 chunk 并行。纯依赖链直接依次运行 kernel，省去系统级任务提交；未注入 JobSystem
时同样串行运行。图、任务回调和命令页在稳定运行期间复用。

`World::update_ecs_pipeline()` 顺序为 authoring 组件、旧 `EcsSystem`、新 chunk
pipeline；之后才进入 `World::add_system` 回调。现有有副作用的系统不会自动转为
并行。新增 kernel 需要显式注册到 `chunk_pipeline()`。

约束：

1. Kernel 只访问声明的数组和不可变外部输入。相同 kernel 的不同 chunk 回调也可能
   并发执行；不要修改捕获的共享计数器或 authoring 对象。
2. 不允许 kernel 内结构变更。query 的 required 类型必须包含所有声明访问的类型。
   Tag 作为 query 过滤条件，不作为数组参数。
3. Query/store 生命周期覆盖 pipeline；不能在 run 期间修改或销毁管线。
4. 异常时等待已发布任务结束后向调用者抛出；后继任务仍完成依赖记账。已有组件写入
   不回滚，失败帧不能视作事务。后续可以再次 run。
5. grain 需要按负载测量；很小的 kernel 不保证并行收益。此 API 没有自动跟踪外部
   数据读写，也没有运行时拦截裸指针越权访问。

## 并行记录结构变更

```cpp
shinkou::ecs::CommandBufferGroup commands(store); // 长期保留以复用页
shinkou::ecs::parallel_chunks_recorded<Position>(jobs, store.query<Position>(), 8,
    commands, [](auto& chunk, auto& buffer, Position* p) {
        for (std::uint32_t i = 0; i < chunk.count; ++i)
            if (p[i].y < -100.0f) buffer.destroy_entity(chunk.entities()[i]);
    });
commands.playback(); // 工作线程已经 join，当前处于可结构变更的同步点
```

每个连续 chunk 分区独占一个 CommandBuffer，work stealing 不改变其所有权；回放按
分区索引顺序执行，不依赖线程完成顺序。占位 Entity 只在所属 buffer 内有意义，不能
跨分区传递，也不会自动重映射组件字段中的 Entity。单个 CommandBuffer 仍不可被多个
线程同时写入。录制异常时先 join 再丢弃本次命令，组件数组中的直接写入不回滚。

对绑定 authoring GameObject 的实体，销毁仍使用 Registry/World 的生命周期接口；
这些 buffer 的 playback 操作的是原始 EntityStore，不会触发 Registry 的 observer。

## 验证与复现

`EcsRuntimeTests` 覆盖非平凡组件迁移与回收、命令页复用与析构、payload 对齐、
单/多 worker 依赖图、读者并行、异常后复用、query 更新、分区占位句柄与回放顺序。
`WorldTransformTests` 另外验证管线在 World 中执行，并在旧回调前完成。

```powershell
cmake --build out/build/runtime-release --target shinkou_ecs_runtime_tests shinkou_ecs_runtime_benchmarks --parallel 8
ctest --test-dir out/build/runtime-release -R '^shinkou_(ecs_store|ecs_runtime|game_object|world_transform|jobs|components|render_view|physics_world|audio_scene_system)_tests$' --output-on-failure --timeout 60
out/build/runtime-release/shinkou_ecs_runtime_benchmarks.exe
```

基准使用 10 万实体，结构变更和命令各执行 20 轮；预热之后测量。原生算术热循环
额外测量 1000 帧并输出 checksum。系统级并行基准使用两个独立三角函数计算 kernel
和一个依赖两者的结果 kernel，4 worker，预热 4 帧、测量 10 帧；串并行校验值必须
完全一致。

2026-11-02，本机 Windows / MinGW GCC，`-std=c++17 -O3 -DNDEBUG`，同一基准在修改前
和修改后各编译一个程序；正反交替执行 5 次，以下取每组中位数：

| 工作负载 | 修改前 | 修改后 | 加速 |
| --- | ---: | ---: | ---: |
| 10 万实体各添加/移除一次 Health | 18.054 ms | 6.324 ms | 2.85× |
| 记录 10 万条 Position 设置命令 | 3.484 ms | 0.874 ms | 3.99× |
| 回放上述命令 | 2.248 ms | 0.773 ms | 2.91× |
| 销毁 10 万实体 | 1.412 ms | 1.192 ms | 1.18× |
| 原有 Position/Velocity 算术热循环 | 54.811 µs | 56.337 µs | 无收益；约 2.8% 波动 |

算术热循环没有改变存储布局或运算实现，不将其计为优化收益。两边 checksum 都为
`2900022.888`。系统级并行基准另测 3 次，逐次加速为 1.87×、1.85×、1.56×；
所有串并行 checksum 都为 `111754.548550`。这些数字仅说明该负载下的调度收益，
不外推为实际项目帧率。

本机原始输出位于 `out/build/ecs-runtime/storage-measurements.txt` 和
`measurements.txt`；二进制与构建目录不提交。固定基准可用 `--storage-only` 跳过
三角函数系统调度基准。验证配置关闭 SDL3、PhysX、ImGui；未验证图形编辑器交互，
9 项运行时回归全部通过，独立 Debug 构建启用 `_GLIBCXX_ASSERTIONS` 并通过。
测试的 TEMP/TMP 指向工作区内 `out/build/ecs-runtime/tmp`，以满足受限环境的写入权限。

## 第二轮：批量创建、清空与增量查询匹配

第一轮之后进一步减少了不必要的数据移动和类型签名扫描：

- `EntityStore::clear()` 按 chunk 的组件列整体析构，递增所有活实体代际后回收 chunk。
  整体死亡的数据不再逐实体 swap-back；原生组件析构按列进行，不承诺逐实体析构顺序。
  Registry 的销毁观察者仍在 store 清空之前逐实体通知，GameObject 生命周期不变。
- `Query` 保存已匹配的 archetype，并只匹配后来新增的 archetype。chunk 集合变化时，
  仍会更新匹配 archetype 的 chunk 列表，但不再重复扫描无关类型组合。每个 query
  额外保留 O(匹配 archetype 数量) 的指针；依赖 archetype 在 store 生命周期内不删除。
  无 required 类型的查询也正确包含空 archetype 的实体。
- `create_batch(archetype, output, count)` 按可用 chunk 范围初始化连续组件列，一次
  扩展句柄表，然后填充实体索引。支持已有半满 chunk、回收索引、非平凡默认构造和
  零数量调用；默认值和单个 `create(archetype)` 一致。调用方需提供足够的句柄数组，
  并满足 32 位实体索引容量限制。与原有 create 一样，分配/构造失败不提供恢复保证。

```cpp
auto& registry = world.ecs();
auto& shape = registry.store().archetype<Position, Velocity>();
std::vector<shinkou::Entity> spawned(100000);
registry.create_batch(shape, spawned.data(), spawned.size());
```

批量创建接口面向原生实体，不自动生成 GameObject 或 authoring 组件。清空与查询优化
对原调用路径生效；创建收益需要批量生成代码使用新接口。

`EcsBulkBenchmarks` 在同一机器、同一编译选项下，于本轮修改前后正反交替运行各 5 次，
取中位数（10 万实体，预热 1 轮、测量 20 轮）：

| 工作负载 | 本轮修改前 | 本轮修改后 | 加速 |
| --- | ---: | ---: | ---: |
| 清空 10 万实体 | 0.899 ms | 0.120 ms | 7.49× |
| 128 个无关 archetype，1 万次创建/销毁并刷新查询 | 4.060 ms | 0.206 ms | 19.71× |

同一修改后程序中，循环创建为 1.067 ms，`create_batch` 为 0.267 ms，约 4.00×。
所有对应校验值一致。查询结果只说明异构世界中 chunk 频繁变动时的刷新成本，不代表
稳定场景的 query 遍历能加速 19 倍；上述结果也不代表整帧收益。

```powershell
cmake --build out/build/runtime-release --target shinkou_ecs_bulk_benchmarks --parallel 8
out/build/runtime-release/shinkou_ecs_bulk_benchmarks.exe
```

新增测试覆盖批量默认值、半满 chunk 与回收索引、非平凡组件析构、观察者通知、
clear 前句柄失效、重复清空、空 archetype，以及清空后新增 archetype 的查询更新。
本机原始测量记录为 `out/build/ecs-bulk/measurements.txt`。
