# ShinkouUI Studio

这是一个 Windows-only 的样式工作台，用于在接入引擎前审查和调整 ShinkouUI 的视觉规则。

当前功能：

- Windows 11 浅色/深色主题切换；
- 中央实时预览：面板、普通按钮、主要按钮、文本输入框和滑块；
- 强调色 HEX 编辑；
- 控件圆角 0–16px 调整；
- 字体大小 10–24px 调整，默认 Microsoft YaHei；
- XML 样式保存/加载；
- 启动时 DPI 感知，窗口最大化后按实际客户区重新布局。

它使用 `ShinkouUI` 的 `StyleSheet` 和 `RenderList`，GDI 只作为预览后端，不会成为 UI 核心依赖。

