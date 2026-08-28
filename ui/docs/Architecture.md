# ShinkouUI 架构

```text
XML Theme / StyleSheet
          |
      UiContext ---- AnimationTimeline
          |
       Widget Tree ---- Win32 IPlatformAdapter
          |
       RenderList
          |
      IRenderBackend (D2D/D3D/engine renderer)
```

核心层不拥有窗口、不调用 GPU、不引入 ImGui。布局只在 dirty 状态或视口/DPI 变化时重算；绘制阶段生成线性命令列表，后端可以按材质、纹理和裁剪批处理。虚拟列表可以使用 `visible_range` 只创建可视区附近的项目，避免低配置机器被大量控件拖慢。

视觉策略是“扁平优先、层级清晰”：面板和标题栏默认不使用圆角，按钮/输入框只使用 3–4px 圆角；阴影和动画不是强制开销，`reduce-motion` 可在主题中关闭动画。

