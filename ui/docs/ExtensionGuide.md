# 扩展与使用约定

## 注册自定义控件

```cpp
WidgetRegistry registry;
registry.register_type("asset-picker", [](const XmlNode& node) {
    auto widget = std::make_unique<Panel>();
    widget->styleClass = node.attribute("style", "panel");
    return widget;
});
```

控件工厂只负责创建对象，通用属性（`style`、`visible`、`enabled`、`flex`、`padding`、`margin`、`layout`）由库统一加载。自定义控件可覆写 `type_name()` 和 `serialize()`，从而参与 XML 保存。

## 扩展渲染后端

UI 只生成 `RenderList`，渲染库只消费自己的 `DisplayList`。两者之间预留明确适配层，不让 UI 组件直接持有窗口句柄、纹理对象或 GPU 上下文。这样可以为编辑器、运行时 UI 和测试分别使用不同后端。

## 性能约定

- 布局只在 dirty、视口或 DPI 改变时重算；
- 绘制命令使用连续 vector 存储，后端可按类型/材质批处理；
- 大列表用 `visible_range()`，只生成可视区和 overscan 范围；
- 动画支持 `reduce-motion`，低配置设备或无障碍设置可关闭过渡；
- 样式对象集中管理颜色、字体和度量，控件不重复分配主题数据。

