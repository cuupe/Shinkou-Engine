# ShinkouRender

这是从引擎渲染系统中先抽出的独立“显示内核”项目，当前不接入现有引擎。它把显示相关的稳定边界单独定义出来，避免 UI、窗口和具体 GPU 后端互相耦合：

- `DisplayList`：线性的显示命令列表，可缓存、统计和批量提交；
- `IDisplayBackend`：未来接入 D3D11/D3D12/Vulkan/Metal 的后端接口；
- `DisplayRenderer`：负责帧生命周期、尺寸和后端切换；
- `SoftwareDisplayBackend`：无窗口、无 GPU 依赖的 CPU 显示后端，用于显示正确性测试和低配置兼容验证；
- `shinkou_render_display_tests`：只测试显示像素、清屏、矩形、边框、线段、缩放和帧统计。

现有 `engine/src/render` 仍保持原样。本阶段先稳定独立库的 API，下一阶段再做引擎侧适配层和逐模块迁移，避免一次性搬动渲染图、资源系统和平台后端造成大范围回归。

## 构建和测试

```powershell
cmake -S render -B out/build/shinkou-render -G "MinGW Makefiles" -DSHINKOU_RENDER_BUILD_TESTS=ON -DSHINKOU_RENDER_BUILD_EXAMPLE=ON
cmake --build out/build/shinkou-render -j 6
ctest --test-dir out/build/shinkou-render --output-on-failure
out/build/shinkou-render/shinkou_render_preview.exe
```

