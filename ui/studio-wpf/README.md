# ShinkouUI Studio (WPF)

这是新的 Windows 样式编辑器。WPF 负责窗口、属性面板、数据绑定和 XML 文件操作；中央预览不是 WPF 自己画的，而是调用 `ShinkouRender` 的 `shinkou_render_native.dll` 生成 RGBA 显示缓冲，再转换为 `WriteableBitmap`。

## 构建顺序

先构建渲染共享库：

```powershell
cmake -S render -B out/build/shinkou-render -G "MinGW Makefiles" -DSHINKOU_RENDER_BUILD_TESTS=ON -DSHINKOU_RENDER_BUILD_NATIVE_API=ON
cmake --build out/build/shinkou-render -j 6
```

再构建 WPF 工具：

```powershell
dotnet build ui/studio-wpf/ShinkouUI.Studio.Wpf.csproj --configuration Debug
ui/studio-wpf/bin/Debug/net8.0-windows/ShinkouUI.Studio.Wpf.exe
```

项目会自动将 `shinkou_render_native.dll` 复制到 WPF 输出目录。WPF 工具不依赖现有引擎，也不修改 ImGui 编辑器。

