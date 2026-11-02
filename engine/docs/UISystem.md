# UI 模块与编辑器的边界

主编辑器使用 Dear ImGui Docking，通过官方 Win32 输入和 DX11、DX12、Vulkan GPU backend 展示。生命周期、文件事务、资源加载、命令和撤销由 EditorLayer 管理；控件由 EditorImGuiView 及分拆文件实现。完整开发入口见 [UIWorkflow](../../docs/UIWorkflow.md)。

engine/include/shinkou/ui 保留通用的独立 UI API：

- Theme、Style：语义颜色、尺寸、字体及 JSON 配置。
- Media：引擎音频/视频传输接口。
- Render：不可变图像、模型、文本等快照及通用命令数据。
- Ui、Components、InputBridge、ImGuiAdapter：供独立使用者选择的 retained runtime、组件和适配器，保留各自测试，不构成编辑器的回退路径。

编辑器不拥有 UiRuntime 或 ComponentDocument，不生成 retained UI 绘制列表，渲染 backend 不再提供 render_editor_ui。增加编辑器功能时复用 EditorImGuiWidgets、EditorAssetBrowser 和控制器 API，避免另建控件树、命中系统或平行状态。

EditorUiModel 是反射属性、层级、构建和媒体的展示快照。DockWorkspace 是版本化工作区数据；实际编辑器停靠布局使用 ImGui 的 .imgui.ini。二者都不是 renderer。
