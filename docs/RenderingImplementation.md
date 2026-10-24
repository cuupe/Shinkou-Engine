# 渲染系统整改与验收

目标：让渲染功能从参数、规划、资源、Shader 到最终图像形成可验证的执行路径，
同时保持现有编辑器提交边界和公共 API 的兼容。

## 实施顺序

1. 功能图：区分描述规划和执行构建；缺少实现必须失败；检查阶段依赖；事务式构建。
2. 公共管线：通用顶点布局、多顶点流、实例绘制、深度/模板、独立混合和光栅化状态。
3. 场景数据：统一灯光分簇；绑定阴影级联与对象矩阵；保留视锥外的阴影投射物。
4. 真实着色：共享 PBR Shader 与材质参数；提供可执行后处理；历史资源跨帧管理。
5. 模块边界：按原生后端分编译单元，分离通用状态与平台转换，保持 UI 合成顺序。
6. 帧资源：明确并行录制与资源回收边界，减少重复创建，按后端逐项验证多帧提交。
7. 验收：除描述合约测试外，增加原生像素回读、多帧、异常路径与缓存隔离验证。

## 构建环境

- 当前有效构建目录：`out/build/mingw-debug`。
- `engine_build` MCP 当前配置指向不存在的 `out/build/editor`，不能作为成功依据。
- MinGW 运行时目录：`D:/paths/GNU/mingw64/bin`，运行测试时置于当前进程 PATH 前部。
- UI 运行时为 `shinkou::ui`，由 `Renderer::set_editor_ui_render_list()` /
  `IRenderBackend::render_editor_ui()` 提交。技能文档中的独立 `ui/`、UiKitPanelHost
  不存在于此工作区；不据此创建第二套 UI。

## 完成标准

每项记录实际实现和执行过的验证。仅通过计划、节点数量、描述校验的功能不能标为
图像能力已完成。没有原生实现的硬件功能不得因为设备 Ready 而宣称可用。

## 当前进度

基础执行链已落地。以下严格区分已实现和后续工作。

| 原问题 | 实现结果 | 入口 |
|---|---|---|
| 后端集中在巨型单文件 | 工厂与 DX11、DX12、Vulkan、Null 分离，共享验证/状态转换/纹理复用独立 | `engine/src/render/backends/` |
| 功能图节点为空 | `plan()` 仅描述；`build()` 要求实际执行器，验证依赖和参数，事务提交 | `RenderFeatureGraphBuilder` |
| 管线固定 | 类型化顶点布局、多流、实例范围、深度模板、逐附件混合和光栅状态；全部进入缓存键 | `PipelineState` |
| PBR 只有描述 | GGX/Smith/Schlick、金属粗糙度、法线/AO/自发光、方向/点/聚光灯、外部 IBL 图和 LUT | `PbrMaterialLibrary`、`PbrForward.hlsl` |
| 阴影缺少闭环 | 独立级联/对象常量、视锥外投射物、图集、级联选择及 3×3 PCF | `ForwardRenderer::build_shadows` |
| 后处理没有实现 | 原生 SSAO、SSR、TAA、Bloom、曝光、色调映射和 Present Shader/材质 | `BuiltinRenderFeatures` |
| 多层 API 分离 | 统一管理 HDR、深度、阴影、历史帧、标准材质和提交反馈 | `StandardRenderPipeline` |
| 重复分簇算法 | 共用 light binning、64 字节灯光 ABI、相交分区覆盖、内存预算和输出容量检查 | `LightingPipeline` |
| 调度器未接入 | 显式录制回调生成不可变值命令，验证资源声明后按图顺序回放；内置图像阶段实际使用 | `RenderCommandList`、`RenderGraphScheduler` |
| 每帧重复创建和写入竞态 | 有界原生纹理复用、CPU 可见缓冲区写入前等待旧读者、历史资源交换 | 后端、`TemporalRenderHistory` |

同时修复：初始化常量缓冲区错误别名；DX12 别名丢失 SampleDesc；DX12 PSO
缓存初始化字节提前销毁造成崩溃；DX11 无窗口 scissor 与离屏视口恢复；深度标志
归一化；颜色/深度独立清除；无错误输出指针时的失败传播；SPIR-V 独立图像、
只读结构缓冲区和存储图像反射；DXBC/DXIL 存储图像分类；DX12 缩放时交换链格式变化。

## 使用与生命周期

独立样例使用 `StandardRenderPipeline`。运行：

```powershell
$env:PATH = 'D:\paths\GNU\mingw64\bin;' + $env:PATH
out/build/mingw-debug/shinkou_engine_sample.exe --sample dx11 --frames 120
```

后端参数可替换为 `dx12` 或 `vulkan`。初始化后用 `create_material()` 创建
`PbrMeshVertex` 布局的材质。每帧顺序为 `Renderer::begin_graph()`、提取场景、
`pipeline.build(renderer, scene, target, deltaSeconds)`、提交、`pipeline.complete_frame(success)`。
`target` 可为空以呈现到窗口；曝光适应使用实际秒数。
相机切换调用 `invalidate_history()`。有 pending 帧时禁止再次构建和缩放。
`build()` 可传离屏显示目标，尺寸与格式必须匹配配置；退出前调用 `release(renderer)`。

完全定制时组合底层材质库、ForwardRenderer、图像库和功能图构建器。工作线程
只能向 `RenderCommandList` 追加命令，原生设备操作仍在提交线程执行。
编辑器保持原有 viewport 与 UI overlay 边界，预览共享 BRDF。

Vulkan 内置 HLSL 在构建时通过 `glslc` 编译，顶点输出翻转 Y；缺少产物会明确
初始化失败。DX11/DX12 内置源路径目前使用 Shader Model 5。

## 渲染顺序和阶段扩展

`StandardRenderPipelineDesc::stages` 是实际执行的命名序列，空列表使用默认顺序。
可在初始化时设置，也可在没有 pending 帧时通过 `set_stages(stages, executors)` 替换。
后者会重置运动/TAA/曝光历史。每个名字唯一，原有 SSAO/SSR/TAA/Bloom/曝光/阴影开关
仍然过滤对应阶段；motionVectors、motionBlur 决定默认序列的插入，显式序列则使用节点的 enabled。
天空由 `settings.sky.enabled` 控制。以下序列需开启天空，例如先做阴影，再做深度，或将 SSR 放在 TAA 前后：

```cpp
using Stage = shinkou::render::RenderFeatureGraphStage;
settings.stages = {
    {"sun", Stage::ShadowCascades}, {"depth_pass", Stage::DepthPrepass},
    {"scene", Stage::Opaque}, {"background", Stage::Sky},
    {"velocity", Stage::MotionVectors},
    {"reflections", Stage::SSR}, {"resolve", Stage::TAA},
    {"blur", Stage::MotionBlur}, {"glow", Stage::Bloom},
    {"adapt", Stage::Exposure}, {"display", Stage::ToneMap},
    {"screen", Stage::Present}
};
```

`$color` 跟随前一个替换颜色流的阶段；`source` 也可显式引用前面阶段的名字。
Bloom、曝光和 SSAO 默认输出辅助纹理，不替换颜色流，由 ToneMap 组合。
`inputs` 绑定额外命名资源，`dependsOn` 表达无共享纹理时也必须遵守的前置依赖。
顺序约束同时进入图编译、工作线程录制依赖和跨队列等待，不依赖插入顺序的偶然性。
Present 必须最后；HDR 效果不能读取已转为显示颜色的输出；读取未来/缺失资源会失败。
构建在候选图中完成，准备失败或依赖无效不会破坏调用方原来的图。

自定义节点使用 `Stage::Custom`，通过 `RenderFeatureGraphExecutor::name` 精确匹配实例；
空执行器名字是该类型的默认实现。可自定义输出尺寸、格式、颜色域、队列，以及
`replacesColor`、`producesImage`；仅有副作用的阶段可令 `source` 为空并关闭图像输出。
额外外部输入通过 `RenderFeatureGraphDesc::resources` 注册。图像 HDR 当前采用 RGBA16F。
`prepare()` 可以注册完整子图，或注册中间 pass 后返回最终录制函数；返回计划中的
`passIndices` 暴露该阶段实际包含的全部 pass。内置 Bloom 即使用这一机制。
缺少执行器不会生成占位效果，`plan()` 与可执行的 `build()` 仍然明确区分。

## 时域画质实现

- 刚体运动矢量：RGBA16F 存储前帧 UV 减当前 UV、前帧 NDC 深度和有效位。
  相机、对象模型矩阵和八样本 Halton 投影抖动使用一致的前后帧变换。
  新对象、重新出现的对象和历史失效帧不接受旧历史。当前仅支持标准 PBR 顶点布局。
- TAA：运动矢量重投影、历史 alpha 深度检查、屏外/无效速度剔除、邻域颜色限制，
  可调反馈和锐化。没有运动输入时支持相机矩阵重投影。
- Bloom：半分辨率预过滤、逐级降采样、逐级上采样重建和全分辨率输出。
  层级、软阈值、强度、scatter 可配置；降至 1×1 后停止分配。
- 曝光：16×16 采样的几何平均亮度、middle gray、EV 补偿、曝光上下限，以及
  `1-exp(-rate*deltaSeconds)` 时间适应。可独立设置场景变暗时的速率；1×1 历史双缓冲。
  `adaptationRate=0` 为即时曝光；标准管线默认启用时间适应。
- 运动模糊：按 UV 速度、快门比例和像素半径采样，使用深度限制跨物体混色，最多 32 样本。
  默认关闭，可加入命名序列；底层构建器也接受命名外部速度输入。

`complete_frame(true)` 才提交前帧对象/相机和两组历史；失败、缩放、相机切换或修改
阶段序列必须重置历史。标准入口拥有一份 TAA 和一份曝光历史；多实例应通过底层
构建器分别提供历史纹理和按实例命名的参数。天空背景已支持无穷远方向重投影；骨骼与顶点形变运动尚未实现。

## 环境天空与直方图曝光

`StandardRenderPipelineDesc::sky.enabled=true` 在默认链的不透明阶段之后插入 Sky；显式
序列通过 `RenderFeatureGraphStage::Sky` 定位。天空在已有 HDR 附件上绘制远平面，保留
几何颜色与深度，可以在其他 HDR 阶段之后绘制，但不能放在场景附件生产之前或 ToneMap 之后。
`SkySettings` 支持线性 HDR 渐变，以及调用方持有的二维经纬环境图、强度和绕 Y 轴旋转；
sRGB 环境纹理先转为线性颜色。正交相机使用平行射线。环境纹理不因天空模块释放而销毁。
`set_sky()` 在没有 pending 帧时修改设置并重置时域历史。

MotionVectors 现在先绘制背景速度，再保留该附件并绘制几何速度。背景通过无穷远方向
重投影去除相机平移，保留透视相机旋转和 jitter；只填充深度为远平面的像素。历史无效
时有效位为零，供 TAA 拒绝旧内容。正交天空方向在整个屏幕恒定，使用零 UV 速度。

曝光可选择 `ExposureMetering::LogAverage` 或 `Histogram`。前者保留兼容的 16×16 采样；
后者读取全部输入像素，在 GPU 上清空 256 桶缓冲、以 16×16 工作组统计，然后进行百分位
归约和已有的时间适应。bin 0 为近黑像素（亮度不大于 1e-6），不参与有效像素百分位。
可调对数亮度范围、低/高百分位；全黑返回最大曝光。默认范围 -12 到 16 EV，裁剪 2%/98%。

```cpp
settings.sky.enabled = true;
settings.sky.environment = hdrEquirectangularTexture; // 可为空以使用渐变
settings.sky.intensity = 1.0f;
settings.exposureParameters.metering = ExposureMetering::Histogram;
settings.exposureParameters.lowPercentile = 0.02f;
settings.exposureParameters.highPercentile = 0.98f;
```

库的默认测光模式仍是 LogAverage；独立样例明确启用天空和 Histogram。Histogram 要求计算
能力，缺失时构建失败并保留原图，不悄悄替换算法。展开后的清空/统计/归约 pass 和跨队列
依赖都可从管线计划与 RenderGraph 检查。

这条真实计算路径同时修复了公共后端问题：连续 StorageWrite 的内存屏障、DX11 dispatch
原生 Shader 检查、DX12 缓冲区显式 UAV 元素布局，以及 Vulkan 离屏计算管线对图形 render
pass 的错误依赖。StorageWrite 屏障包含读写访问，适用于原子读改写。

## 验证证据（2026-10-21）

- 全量构建成功：`cmake --build out/build/mingw-debug --parallel 4`，日志
  `out/render-environment-full-build.log`。
- 全量回归 **65/66 通过**，渲染相关用例全部通过。日志
  `out/render-environment-full-tests.log`；未把本次回归报告为全绿。
- 编辑器交互测试仍报 `GLTF material selection did not follow its base-color texture reference`。
  上轮同一用例也出现资源重绑定错误。这是 Null renderer 下的资源引用/选择路径，
  根因尚未确定，本轮未修改相应编辑器逻辑。
- 新增三种原生后端的 EnvironmentExposureTests：天空阶段实际绘制、保留几何/深度、
  HDR 环境旋转、背景方向重投影、平移无视差、切换历史；直方图非整组尺寸、极亮离群点、
  全黑、近黑排除、百分位归约、时间适应与历史重置。DX11 像素/速度/曝光与 CPU 数值参考比较。
  DX12/Vulkan 执行同一测试路径，但仍无纹理回读，不据此宣称跨后端像素一致。
- 定制/验证回归：天空位置非法、直方图范围非法、无计算能力时明确失败；保持原图事务性。
  直方图实际展开为三个 pass、提交两次 dispatch，并保留写后写内存依赖。
- 原有测试继续通过：命名阶段/几何顺序/子图边界/跨队列等待、自定义阶段交换改变图像、
  物体与相机运动、jitter、TAA 拒绝历史、曝光/Bloom/运动模糊、PBR/阴影/顶点流和资源复用。
- 启用天空、背景运动和直方图曝光的窗口样例：DX11、DX12、Vulkan 各 **120 帧**正常退出。
  每个后端累计 3,360 pass、3,120 draw、240 dispatch；末帧 28 pass，22 个录制 pass /
  45 条命令；累计 1,666 次纹理复用。日志 `out/render-environment-stress-*.log`。
  这些运行确认执行和生命周期，不是画质评测。

## 仍需实现，不能计为完成

1. 多帧在途、原生命令并行与多队列重叠：当前为写入安全而同步 GPU；CPU 命令包
   并行不等于原生命令缓冲区并行。需要帧环、缓冲区版本和逐帧描述符/allocator 所有权。
2. 统一 DXC/SM6 源编译、Vulkan 运行时源编译、热重载到活跃材质和 PSO 的事务替换。
3. 任意 mip/layer/cube/3D 原生视图、绑定和子资源范围屏障。
4. SSAO/SSR 为深度重建基础实现；TAA 尚无骨骼/顶点形变运动和时域超分辨率。
   曝光尚无局部曝光/区域权重；运动模糊尚无速度 tile dilation。
5. 标准 PBR 仍遍历灯光；GPU clustered compute 需要调用方提供 Shader/执行器。
   IBL 使用外部二维经纬图，未提供烘焙。阴影限首个 castsShadow 方向灯，未实现点光/
   聚光阴影、alpha-mask 投射物裁剪。
6. 后端内部仍可按资源、描述符、PSO、帧生命周期进一步拆分。编辑器场景预览仍按
   首材质近似，未建立完整多 primitive 材质范围；标准 HDR 入口目前接入独立样例。

现在具备可复用、可执行和可测试的前向渲染基础，不能称为高级能力全部完备的生产级渲染器。
