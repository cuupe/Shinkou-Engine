# GameObject / ECS 对象系统

Shinkou 的场景对象系统提供两条共存路径：

- `ObjectStorage::Regular`：默认路径。对象拥有稳定的 C++ 包装和常规组件容器，适合编辑器、层级树和低数量 gameplay 对象。
- `ObjectStorage::Ecs`：创建时直接分配 EnTT entity。该对象仍保留 `GameObject` 层级和生命周期接口，但它的 `Component` 实例直接存放在 Registry 的类型池中，适合大批量、按类型批处理的对象。

```cpp
auto& player = world.create_object("Player"); // 默认 Regular
auto& particles = world.create_object("Particles", shinkou::ObjectStorage::Ecs);
auto& child = player.create_child("Weapon", shinkou::ObjectStorage::Ecs);
```

## 通用组件契约

继承 `shinkou::Component` 的组件在两种存储路径上使用同一套 API 和语义：

```cpp
auto* regular = player.add_component<MyComponent>();
auto* ecs = particles.add_component<MyComponent>();

regular->set_enabled(false);
ecs->set_enabled(false);
world.update(deltaSeconds);
```

`add_component<T>()`、`get_component<T>()`、属性反射、启用/禁用、更新、销毁回调都保持一致。对 ECS 对象，`add_ecs_component<T>()` 在 `T` 是 `Component` 时会进入这条通用生命周期；对普通数据类型仍使用 Registry 的高性能组件池：

```cpp
particles.add_ecs_component<Position>(position);
particles.add_ecs_component<MyComponent>(); // 等价于通用 Component attach
```

每个对象只允许一个同类型通用组件。ECS-backed 通用组件由 Registry 所有，GameObject 仅维护一个非 owning 指针索引，因此不会再为同一组件复制一份对象。

高频数据逻辑可以直接使用零分配的原生 view：

```cpp
world.ecs().view<Position, Velocity>().each(
    [](auto entity, Position& position, Velocity& velocity) {
        position.value += velocity.value;
    });
```

`Registry::view()` 保持 EnTT 原生实体参数，避免每个元素经过代际句柄哈希转换；需要 `shinkou::Entity` 的安全包装时使用已有的 `Registry::each<T...>()`。

## 生命周期顺序

对象创建：

1. 分配 ObjectId；ECS 对象同时创建 Entity。
2. 自动挂载 `TransformComponent`。
3. 组件 `on_attach` -> `on_create` -> 当前层级激活时 `on_enable`。
4. GameObject `created` 回调。

更新：

1. 先按层级同步所有对象的 Transform 快照。
2. 常规对象进入独立的常规对象管线；ECS 对象进入独立的 ECS 对象管线。
3. ECS 管线继续按 `EcsSystem::order()` 执行原生 ECS 批处理系统。
4. 执行旧版 `World::add_system` 回调。
5. 刷新 Registry 延迟销毁请求。
6. 统一收集对象树销毁。

两条对象管线在同一个 `World::update()` 帧边界内同步完成，但不会互相遍历：常规管线只访问 `regularObjectOrder_`，ECS 管线只访问 `ecsObjectOrder_`。需要极致吞吐的纯数据逻辑应放入 `EcsSystem`，直接使用 Registry view；兼容 `GameObject` 生命周期的 `Component` 则走 ECS 对象管线。

`destroy()` 只发出请求；正在更新时始终延迟到本帧末尾，避免迭代器和组件指针失效。父对象销毁会递归请求所有子对象。组件销毁顺序是禁用 -> `on_detach` -> `on_destroy`，ECS-backed 组件在回调完成后才从 Registry pool 移除。

## 性能约束

- GameObject 更新不创建每帧临时快照；组件和子对象采用索引遍历，允许回调中请求增删并保持延迟销毁安全。
- `World::each_game_object` 使用维护好的扁平指针索引，适用于不要求父子顺序的批处理。
- `World::reserve()`、`World::optimize()` 和默认开启的自动容量优化用于把 map/vector 扩容移出稳定帧热路径。
- `Registry::reserve()` 预留 Entity、回收列表和延迟销毁队列；`destroy_deferred()` + `flush_destroyed()` 支持系统内安全销毁。
- `World::statistics()` 提供对象、组件、ECS 实体和更新计数，供 profiler/自动策略使用。
- `engine/benchmarks/ObjectBenchmarks.cpp` 对比常规对象逐组件更新与 ECS 对象数据组件的批量 `view<Position, Velocity>().each(...)` 更新；创建、预热和校验均不计入帧时间。

Registry 的 `Entity` 是代际句柄。`clear()` 会递增已有槽位代际且 `valid()` 同时检查 native entity，因此 clear 前的句柄不会在后续创建中重新变成有效句柄。

## 选择建议

默认使用 Regular，不需要为了少量对象支付 ECS 查询和池管理成本；当对象数量大、组件访问按类型成批、且不依赖层级回调顺序时，在创建时选择 Ecs。两种对象可以任意混合于同一个 World，并共享 Transform、Tag、Lifetime 及用户 Component 的效果。
