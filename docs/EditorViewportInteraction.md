# 编辑器视口交互与性能

## 操作

- 右键拖动：在眼睛位置转向；中键拖动：沿相机右轴/上轴平移；滚轮：缩放视距。
- 左键点击物体：选择。W/E/R 切换移动、旋转、缩放操作器，拖动操作器修改 Transform。
- 拖动过程中 Esc 取消，完整拖动只记录一次撤销。Ctrl+Z / Ctrl+Y 撤销、重做。
- 游戏对象菜单支持 Cube / Sphere / Plane。物体创建在当前视线前方，自动选中；层级面板右键菜单也可创建。
- Ctrl+Shift+N 或层级面板 + 创建空对象。

## 修复的原因

1. 相机从前向量提取偏航、俯仰，却用 Rx·Ry 重新组合，顺序不互逆，按住右键会逐帧漂移。现在使用左手坐标系下的 Ry·Rx，零输入保持原旋转；测试比较单次与多帧分段输入。
2. 全视口的 InvisibleButton 抢占了 ImGui 的 hovered/active item，导致 ImGuizmo 的 CanActivate 永远失败。现在使用无交互 ID 的布局占位，让操作器优先处理左键；相机手势有独立所有权，释放、Esc、失焦或隐藏面板时结束。
3. 演示网格直接持有 ECS/GPU 资源，场景快照拒绝有损保存，创建操作在记录撤销时就被中断。PrimitiveMesh 组件只保存形状、颜色和启用状态，几何共享、GPU 资源由视口渲染器持有；创建、文档保存恢复和结构撤销走同一路径。
4. 原先先记录世界渲染，再处理鼠标修改，额外延迟一帧。现在先应用交互，再提取当前世界并记录场景，使用绘制通道保证世界纹理仍在操作器下方。

## 热路径

- 不变的几何快照只验证、上传一次；按形状共享基础物体几何。
- 材质和对象常量缓冲持久缓存。只更新发生变化的相机/对象常量，空闲时不重传。
- 外部缓冲的帧图声明只复制元数据，创建数据仍由 Renderer 保存，供设备恢复和 CPU 拾取使用。原始资源创建、瞬态缓冲上传不受影响。
- 编辑器入口使用交换链垂直同步控制节奏，去除叠加的软件 sleep_for 限帧。
- QA 命中区域依然默认关闭；新增观察区域只在显式 QA 模式产生。

## 可重复测量

`Tools/UiCapture/RunViewportInteraction.ps1 -Backend dx11 -Performance -OutputDirectory <新的工作区目录>`

该脚本在 3840×2160、150% DPI 下重复实际操作器拖动、右键转向和中键平移。CSV 为输出目录内的 `interactive.bmp.timings.csv`。统计为 Debug 构建、三个基础物体的原生窗口脚本输入样本；不代表复杂场景或 GPU 时间。

| 输入样本（2048 帧） | 优化前 p50 / p95 | 优化后 p50 / p95 |
| --- | --- | --- |
| 帧间隔 | 16.45 / 25.95 ms | 16.67 / 17.56 ms |
| CPU 场景构建 | 0.119 / 0.186 ms | 0.107 / 0.157 ms |

帧间隔 p95 改善约 32%。交换链等待时间在去除软件休眠后增加是正常的：等待集中到 Present，不能把它解释为 GPU 计算变慢。测试包含窗口缩放、GPU 截图和系统输入队列操作，仍存在少量长帧；这些数据不表示复杂场景的卡顿已经全部消除。

基线：`out/qa/viewport-perf-before-final-20261025`；结果：`out/qa/viewport-perf-after-20261025`。稳定帧的几何上传字节和材质创建数均为 0。

功能复现：`RunViewportInteraction.ps1 -Backend <dx11|dx12|vulkan> -Editing`。该模式检查操作器修改真实 Transform、撤销/重做、Esc 取消、创建、相机静止保持及平移平面。它与 Inspector 数字输入的 axis-drag 测试不同，实际命中了 ImGuizmo。

## 验收记录（2026-10-25）

- 完整构建与测试：`out/viewport-accepted-build.log`、`out/viewport-accepted-tests.log`，74/74 通过。
- 无 ImGui 构建：`out/viewport-no-imgui-tests.log`，控制器/创建与文档/视口数学 3/3 通过。
- DX11：`out/qa/viewport-dx11-accepted-20261025`，81 步通过。
- DX12：`out/qa/viewport-final-dx12-20261025`，80 步通过，GPU 日志确认场景物体和网格实际绘制。
- Vulkan：`out/qa/viewport-vulkan-rendered-20261025`，81 步通过；`out/qa/viewport-detached-vulkan-20261025`，独立窗口操作与重新停靠 36 步通过。
- Vulkan 菜单关闭残影复核：`out/qa/viewport-popup-vulkan-20261025`，7 步通过，已检查 GPU 截图。

`viewport-final-vulkan-20261025` 是被拒绝的早期记录：输入步骤通过但临时目录不可写，场景着色器没有生成。验收工具现在把临时文件放在工作区输出目录，要求场景就绪，并拒绝着色器/渲染错误日志；不能把只有交互步骤成功解释成世界渲染成功。

正式窗口已用 DX11 启动并关闭 QA。Computer Use 未获得该程序的控制授权，未执行正式窗口中的自动鼠标复查；原生脚本验证与正常模式手动测试须分开表述。
