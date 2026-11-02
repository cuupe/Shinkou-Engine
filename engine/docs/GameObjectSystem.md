# GameObject / ECS 对象系统

Shinkou 的场景对象系统把**编辑器/编剧数据**和**运行时数据**分开存放：

- `ObjectStorage::Regular`（默认）：对象的一切数据都是 `GameObject` 持有的 `Component` 对象，适合编辑器、层级树和少量 gameplay 对象。
- `ObjectStorage::Ecs`：`GameObject` 仍然持有 authoring `Component`（Transform、Lifetime、Camera…），而**运行时 POD 数据**存放在 16 KiB archetype/chunk 中，通过 8 字节 `Entity` 句柄访问。

```cpp
auto& player = world.create_object("Player"); // 默认 Regular
auto& particles = world.create_object("Particles", shinkou::ObjectStorage::Ecs);
auto& child = player.create_child("Weapon", shinkou::ObjectStorage::Ecs);
```

## Authoring 与运行时数据的边界

继承 `shinkou::Component` 的类型是 **authoring 对象**：带虚函数、生命周期回调、`PropertyBuilder` 反射和 `std::string`，永远由 `GameObject::components_` 持有，**不会**进入 chunk。

chunk 里只放**可平凡拷贝**的 POD 组件，用 `add_ecs_component<T>()` 写入：

```cpp
struct Position { float x, y, z; };
struct Velocity { float x, y, z; };

auto& particles = world.create_object("Particles", shinkou::ObjectStorage::Ecs);
particles.add_ecs_component<Position>(Position{0.0f, 0.0f, 0.0f});
particles.add_ecs_component<Velocity>(Velocity{1.0f, 0.0f, 0.0f});
```

两条访问路径的语义如下：

| 目标 | API | 存储 |
| --- | --- | --- |
| authoring 组件 | `add_component<T>()` / `get_component<T>()` | `GameObject::components_` |
| 运行时 POD | `add_ecs_component<T>()` / `get_ecs_component<T>()` | archetype chunk |
| 世界矩阵 | 自动随 `ObjectStorage::Ecs` 创建 | chunk `ecs::LocalToWorld` |
| 批量遍历 | `world.ecs().each<T...>()` / `view<T...>().each()` | archetype chunk |
| 并行遍历 | `ecs::parallel_chunks<T...>(jobs, query, grain, fn)` | archetype chunk |

`add_ecs_component<T>()` 对 `Component` 派生类型编译期报错，对非平凡拷贝类型也会报错：chunk 迁移用 `memcpy` 语义，不允许带堆所有权的成员。

```cpp
// authoring：生命周期与反射可用
auto* camera = player.add_component<components::CameraComponent>();

// 运行时：零虚调用、零逐实体分配、按 chunk 连续
particles.add_ecs_component<Position>({});
world.ecs().view<Position, Velocity>().each(
    [](shinkou::Entity entity, Position& position, const Velocity& velocity) {
        position.x += velocity.x;
    });
```

`view()`/`each()` 现在直接迭代 chunk：每个匹配 archetype 的每条组件数组都是连续内存，回调里的 `Entity` 就是存储句柄本身（没有 EnTT 原生实体→代际句柄的哈希转换）。

## 生命周期顺序

对象创建：

1. 分配 ObjectId；`ObjectStorage::Ecs` 同时创建 Entity 并绑定 `entity id -> GameObject` 的扁平表。
2. 自动挂载 `TransformComponent`（authoring）。
3. 组件 `on_attach` -> `on_create` -> 当前层级激活时 `on_enable`。
4. GameObject `created` 回调。

更新：

1. 同步 Transform：脏对象按层级深度并行刷新，世界矩阵写入 chunk 的 `LocalToWorld` 并回写 authoring 视图（静态场景无脏对象时零成本）。
2. 常规对象进入常规管线（`regularObjectOrder_`）。
3. ECS 对象进入 ECS 管线：先更新它们仍持有的 authoring 组件，再按 `EcsSystem::order()` 执行 chunk 批处理系统。
4. 执行旧版 `World::add_system` 回调。
5. 刷新 Registry 延迟销毁请求。
6. 统一收集对象树销毁。

`destroy()` 只发出请求；更新中始终延迟到帧末，避免迭代器和 chunk 指针失效。父对象销毁会递归请求所有子对象。组件销毁顺序是禁用 -> `on_detach` -> `on_destroy`。

## 关键不变量

- **chunk 指针稳定，组件指针不稳定**：增删组件会让实体在 archetype 之间迁移（`memcpy` + 源槽 swap-back），因此任何 `T*` 只能在"下一次结构性变更之前"使用。跨帧缓存请存 `Entity` + 类型，而不是裸指针。
- **渲染读 chunk 世界矩阵**：ECS 对象自动拥有 `ecs::LocalToWorld`；`TransformComponent::set_*` 会把对象标记为脏，下一帧按层级深度并行刷新，并回写 authoring `world_matrix()`。静态场景（无脏对象）变换阶段成本为零。
- **authoring 组件指针稳定**：它们由 `GameObject` 独占持有，`World::find_component(ComponentId)` 依旧有效。
- **Entity 是 8 字节代际句柄**：`{index, generation}`。`destroy()` 递增 generation；`clear()` 递增全部已有代际，因此 clear 前的句柄不会重新变成有效句柄。
- **`has_ecs_entity()` 只表示"该对象有运行时存储"**，不表示它没有 authoring 组件。
- **`.scene` 仍只序列化 OOP 树**：chunk 数据没有属性反射，带 entity 的对象在文档保存时明确拒绝（无有损保存）。

## 性能约束

- 对象更新不创建每帧临时快照；组件和子对象采用索引遍历，允许回调中请求增删并保持延迟销毁安全。
- `World::each_game_object` 使用维护好的扁平指针索引；`World::find_object(Entity)` 是 entity id 直接索引，不再哈希。
- `World::reserve()`、`World::optimize()`、自动容量优化把 map/vector 扩容移出稳定帧热路径。
- `Registry::reserve()` 预留 Entity 表、回收列表和延迟销毁队列；`destroy_deferred()` + `flush_destroyed()` 支持系统内安全销毁。
- `World::statistics()` 提供对象、组件、ECS 实体和更新计数。
- `engine/benchmarks/EcsChunkBenchmarks.cpp` 对比常规对象、EnTT 稀疏集、chunk 批量创建/更新/销毁与 chunk 并行；`ObjectBenchmarks.cpp` 保留常规 vs ECS 的对象管线对比。
- chunk 存储的完整设计与实测数据见 [ECS chunk 存储](EcsChunkStorage.md)。

## 选择建议

默认 Regular。对象数量大、数据按类型成批、更新不需要 authoring 生命周期时用 Ecs，并且把热数据做成 POD 放进 chunk；authoring 组件只留真正需要编辑器编辑的部分。两种对象可以任意混用，共享同一套 Transform、Tag、Lifetime 语义。
