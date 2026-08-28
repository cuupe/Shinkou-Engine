# 渲染库抽取边界

当前仓库的 `engine/src/render` 是完整引擎渲染系统，包含资源管理、RenderGraph、着色器编译、材质、队列同步和 D3D/Vulkan 后端。一次性搬迁会同时改变大量现有依赖，因此本阶段先把最稳定的显示边界独立出来：

| 层级 | 当前独立库 | 后续迁移方向 |
|---|---|---|
| 显示命令 | `DisplayList` | 由 UI 适配层转换或直接生成 |
| 帧生命周期 | `DisplayRenderer` | 对接引擎窗口和 swapchain |
| 后端接口 | `IDisplayBackend` | D3D11/D3D12/Vulkan/Metal |
| 测试后端 | `SoftwareDisplayBackend` | 保留作为回归测试和无 GPU CI 后端 |
| 资源/管线 | 暂不迁移 | 从 `engine/src/render` 按依赖分组迁移 |
| RenderGraph | 暂不迁移 | 待显示接口稳定后再抽出 |

`render/` 是单独可配置、单独可编译、单独可测试的项目，不加入根 CMake，也不改变现有引擎目标。后续迁移按“公共类型 → 资源生命周期 → RenderGraph → 平台后端”的顺序进行，每一步都保留引擎旧路径作为回滚边界。

