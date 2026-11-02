# 编辑器界面

编辑器采用 Dear ImGui Docking；HelloImGui 提供主题，ImPlot 提供曲线，ImGuizmo 提供变换操作器，Codicons 提供字体图标。仅保留这一套编辑器展示路径。依赖版本和许可证见 [依赖清单](../../third_party/EDITOR_UI_LIBRARIES.md)。

## 控制器和展示

EditorLayer 管理场景文档、类型化属性、撤销、资源扫描与导入、文件恢复、音视频和构建命令。EditorImGuiView 负责控件与布局，各 GPU backend 只提交 ImDrawData。旧 EditorUi、DX11 Direct2D UI compositor、旧 SVG 解析器及未使用的工具栏组件已移除。

initialize(false) 为纯控制器模式，不创建可视界面。initialize(true) 创建 ImGui 展示；无窗口时可用 Null 和引擎事件运行相同控件用于测试。关闭 SHINKOU_ENABLE_IMGUI 后控制器可用，编辑器展示初始化明确失败。

## 共享功能

- 资源浏览模型统一目录、筛选、选择、列表/图标/目录树和展开缓存，筛选限制在当前目录子树内。
- 字符串、复选框、滑块及 XYZ/XYZW 共用控件。Transform 位置、欧拉角旋转、缩放逐轴编辑，复用撤销事务；音频空间位置复用相同向量控件。
- 检查器和媒体窗口共享播放/暂停/停止与波形实现。
- 层级选择和视口拾取共用对象选择入口；Scene/Game 每帧共享模型实例提取结果。
- 自定义面板经 register_panel 接入相同的停靠窗口策略。

层级使用可见行虚拟化和版本缓存，QA 记账默认关闭。搜索栏固定、行高按物理像素测量，水平内容范围按层级/字体变化缓存。性能基准、开关及 C++/配置布局边界见 [UI 性能说明](../../docs/EditorUiPerformance.md)。

## 工作区、DPI 与世界画面

默认左侧层级、中间 Scene/Game、右侧检查器、下方资源/工具标签。支持分割、标签拖动、浮动、独立原生窗口及布局保存。停靠布局保存在编辑器布局文件旁的 .imgui.ini；DockWorkspace 保留公开数据格式，不承担另一套窗口绘制。

字体按物理像素重建，主题尺寸从基础样式重新缩放，避免累乘。QA 区域显式从物理像素换算为逻辑像素。改变用户缩放不是实际更改显示器 DPI 的证据。

Scene/Game 使用独立颜色与深度目标显示 World。右键转向，中键沿相机平面平移，左键拾取；坐标统一为左手、+Z 前方。DX11/DX12/Vulkan 均有场景、模型和独立窗口适配，契约与原生验收入口见 [后端说明](../../docs/EditorRendererBackends.md)。

资源能力见 [资源预览与配置](../../docs/EditorAssetInspection.md)，属性架构见 [检查器](../../docs/EditorInspectorArchitecture.md)。

## 验证

测试分为控制器业务、实际 ImGui 无窗口控件、原生 GPU 窗口三个层次。无窗口命令数量或交互区域不能替代真实窗口截图。开发边界与用例见 [UIWorkflow](../../docs/UIWorkflow.md)。

```powershell
cmake --build out/build/mingw-debug --parallel 5
ctest --test-dir out/build/mingw-debug --output-on-failure
Tools/UiCapture/RunInspectorInteraction.ps1 -Backend dx11
```
