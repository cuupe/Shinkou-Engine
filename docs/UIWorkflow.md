# 编辑器 UI 开发入口

编辑器只有一套展示管线：Dear ImGui Docking。原有 EditorUi 控件树、输入命中、SVG 解析器和 DX11 Direct2D/CPU 叠加绘制已移除，不保留隐藏兼容界面。

```text
Win32 消息 → 官方 ImGui Win32 backend
EditorLayer（命令、事务、异步资源、文档）
  → EditorImGuiView（布局和控件）
  → ImDrawData → Renderer → DX11 / DX12 / Vulkan
```

无窗口测试通过 `EditorLayer::initialize(true)` 生成同一套 ImGui 控件和绘制数据，输入由引擎事件适配到 ImGui IO；Null 不提交 GPU。`initialize(false)` 只初始化控制器与模型，不创建控件命中区或另一套绘制列表。关闭 SHINKOU_ENABLE_IMGUI 后可构建运行时和控制器测试，启动编辑器展示会明确失败。

QA 区域记录默认关闭，自动测试环境或 set_ui_qa_enabled(true) 显式开启。层级虚拟化、版本缓存、基准方法和 C++/配置布局边界见 [UI 性能说明](EditorUiPerformance.md)。

## 修改位置

| 内容 | 入口 |
| --- | --- |
| 生命周期、文档、文件事务、撤销、命令 | engine/src/editor/EditorLayer.cpp |
| 菜单、工作区、主题、字体、DPI、帧提交 | EditorImGuiView.cpp |
| 层级、资源列表/目录树/图标、Scene/Game | EditorImGuiPanels.cpp |
| 反射属性与事务 | EditorImGuiInspector.cpp |
| 资源专属配置 | EditorImGuiAssetOptions.cpp |
| 媒体、波形、构建、日志与工具窗口 | EditorImGuiTools.cpp |
| 字符串、复选框、滑块、XYZ/XYZW 控件 | EditorImGuiWidgets.* |
| 资源目录、筛选、选择、展开与可见行缓存 | EditorAssetBrowser.* |

新增面板使用 register_panel，由同一 presenter 执行窗口布局和回调。操作场景对象使用 select_object；资源选择、导航、文件操作分别使用 set_selected_asset、set_asset_directory、handle_asset_action。所有入口复用控制器，不在控件内执行文件 I/O 或另建状态。

Scene/Game 共享每帧提取的模型实例快照，各自持有颜色/深度目标和 GPU 资源。检查器与媒体窗口共享传输控件和波形；反射向量与音频空间位置共用逐轴输入。

## 保留的数据与库

EditorUiModel 是展示数据。DockWorkspace 保留版本化配置和公开数据 API；实际交互停靠由 ImGui 管理，保存在布局文件旁的 .imgui.ini。engine/ui 的主题、样式、媒体和不可变快照被编辑器复用；通用 retained runtime、组件和适配器是独立库 API，编辑器不会构造或提交它们。

## 旧接口迁移

本次主动移除旧编辑器接口，不保留空实现或兼容控件树：

| 旧接口 | 当前入口 |
| --- | --- |
| EditorUi、AssetIconLibrary | EditorImGuiView、Codicons 字体图标 |
| EditorLayer::editor_ui / ui_runtime / ui_input_stats | 控制器 API；QA 观察使用 ui_interaction_regions |
| render_editor_ui / set_editor_ui_render_list / supportsNativeUi | ImDrawData / set_imgui_draw_data / supportsImGui |
| EditorAssetPreviewUiState::textLines | textPreview 的不可变完整文本快照 |
| ui_text_command_count | ui_labeled_control_count：按钮和面板标题计数，不再冒充文本绘制次数 |

目录文件操作共用 FileSystemService；Windows 短暂占用最多重试 94 ms，仍不覆盖已存在的目标。失败的文件撤销/重做保留原历史项，允许解除占用或目标冲突后重试。

## 验证

- shinkou_editor_asset_browser_tests：范围边界、排序、展开、三种模式、筛选、版本失效。
- shinkou_editor_interaction_tests：控制器模式、类型化编辑、文件重命名/恢复、引用迁移、媒体和异步预览。
- shinkou_editor_imgui_presentation_tests：实际 ImGui 控件、逐轴布局、输入、共享状态、自定义面板与 DPI 变化。
- 原生 GPU 验收见 [渲染后端说明](EditorRendererBackends.md) 和 Tools/UiCapture/README.md。使用独立项目和 GPU 回读截图；无窗口绘制数据不能证明原生窗口或 GPU 像素正确。
