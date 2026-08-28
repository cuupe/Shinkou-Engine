# ShinkouUI

`ShinkouUI` 是一个独立的、Windows-first 的游戏编辑器/工具 UI 库。它不依赖 ImGui、SDL 或具体 GPU API，核心只负责：

- XML 驱动的主题、字体、度量和控件状态配置；
- 可组合 Widget 树、Overlay/Vertical/Horizontal 布局、flex 和 DPI 缩放；
- Button、Slider、TextBox、RichTextBox、Panel、Image、Video、Audio 基础组件；
- 后端无关的 `RenderList`，可以接 Direct2D、D3D、OpenGL 或引擎自己的渲染管线；
- 动画时间线、缓动、减弱动态效果和输入事件；
- `IPlatformAdapter` 平台抽象，当前包含 Win32 DPI/字体目录接口，为 macOS/Linux 留出扩展点。
- Windows-only `ShinkouUI Studio` 可视化样式工作台，支持实时主题预览、强调色、圆角、字体大小、XML 保存和加载。

## 构建

```powershell
cmake -S ui -B out/build/shinkou-ui -G "MinGW Makefiles" -DSHINKOU_UI_BUILD_TESTS=ON -DSHINKOU_UI_BUILD_EXAMPLE=ON
cmake --build out/build/shinkou-ui -j 6
ctest --test-dir out/build/shinkou-ui --output-on-failure
```

Windows 下可直接启动：

```powershell
out/build/shinkou-ui/shinkou_ui_studio.exe
```

Studio 是独立工具，不属于引擎编辑器。右侧属性区可以修改强调色、控件圆角和字体大小，`保存 XML`/`加载 XML` 对应 `StyleSheet::to_xml_string()` 和 `StyleSheet::from_xml()`。

现有引擎和 ImGui 调试界面不会被这个独立项目修改。编辑器接入阶段只需要实现 `IRenderBackend`、将窗口输入转换成 `UiEvent`，并把 `RenderList` 交给渲染后端。
