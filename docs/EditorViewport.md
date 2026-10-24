# 编辑器高 DPI 与世界视口

原生编辑器使用 Dear ImGui Docking、Win32 和 D3D11。窗口以 Per-Monitor V2
模式接收物理客户区像素。每帧读取窗口 DPI，因此跨显示器移动且客户区尺寸不变时，
字体也能更新。DPI、用户缩放或字体设置变化时，在下一帧开始前释放字体 GPU 资源、
重新烘焙字体图集并上传；样式从原始主题重新缩放，避免累计缩放和放大旧字体。
设置面板提供界面缩放与字体大小。编辑器默认最大化到当前显示器。

字体重建遵循仓库固定版本 ImGui 1.91 的图集生命周期；上游说明见
[对应版本 DPI 文档](https://github.com/ocornut/imgui/blob/v1.91.9b-docking/docs/FAQ.md#q-how-should-i-handle-dpi-in-my-application)。

`EditorWorldViewport` 为 Scene 和 Game 分别维护与可见区域物理像素等大的颜色、
深度目标和帧常量。世界几何通过 RenderGraph / ForwardRenderer 绘制，资源模型
使用同一深度目标。天空和带深度测试的 GPU 地面网格也在图中提交，网格通过像素
导数抗锯齿并随距离淡出，模型会遮挡后方网格。最后将颜色纹理作为 ImGui 图像提交。调整窗口、停靠和用户缩放
后重新匹配目标尺寸，重复尺寸复用纹理。停靠树保持用户的分割比例，避免缩小时侧栏
挤占场景区。场景不再依赖透明 UI 背后的背板画面，也不
使用固定 1280×720 的示例贴图坐标。Game 使用同一 World 的活动相机，拥有独立目标。

世界提取遍历全部 GameObject，包括子对象；关联 ECS 网格使用 GameObject 的世界
变换。暂停编辑仍会刷新变换；父对象变换变化也会使子对象世界矩阵更新。

## 操作

- 右键拖动：透视模式绕相机当前位置转向，限制俯仰，避免翻转；正交模式平移。
- 中键拖动：沿相机右、上方向移动，始终位于以前方为法线的平面内。
- 滚轮：透视前后移动；正交调整显示范围。
- 左键点击：选取射线命中的最近可见三角形；点击空白取消选择。
- W/E/R：移动、旋转、缩放操作器；F：聚焦所选对象。

拖动必须从视口内开始，捕获后可继续越过视口边缘；操作器拖动期间不执行相机操作
和拾取。UI 使用逻辑尺寸，画面目标、鼠标射线和 GPU 视口统一使用物理像素。
导入模型先进行包围盒剔除，再做双面三角形相交；ECS 网格读取保留的 CPU 顶点和
32 位索引，支持 binding 0 的 Float3 POSITION、索引范围及顶点偏移。没有保留 CPU
几何、GPU 程序生成或蒙皮变形后的几何，目前需要后续 GPU ID/深度拾取扩展。

## 验证

```powershell
cmake --preset mingw-debug
cmake --build out/build/mingw-debug --parallel 6
ctest --test-dir out/build/mingw-debug -R shinkou_editor_world_viewport_tests --output-on-failure
Tools/UiCapture/RunViewportInteraction.ps1 -Theme dark
Tools/UiCapture/RunViewportInteraction.ps1 -Theme light
Tools/UiCapture/RunViewportInteraction.ps1 -Theme high-contrast
```

原生脚本通过指定子窗口的 Win32 输入，断言相机移动/转向的几何约束、真实对象选中、
1440×900 / 3840×2160 / 1024×768 / 2560×1440 调整后的目标像素尺寸，并保存 GPU
截图。用户缩放测试能验证字体重建，但不等同于更换物理显示器 DPI。

D3D11 是本轮完整世界视口的验证后端。DX12/Vulkan 的原生 UI 后端已接入，其世界
视口适配仍保留明确的不支持提示；Null 测试只验证数学、资源和渲染图契约。

### 2026-10-24 验证记录

- MinGW Debug 完整构建成功，完整 CTest 68/68 通过；随后视口、对象、模型预览、
  渲染管线执行和编辑器交互 5/5 复验通过，最终网格透明度调整后再次通过视口测试。
- D3D11 原生深色、浅色、高对比度各 52/52 项操作通过，输出为 GPU 客户区读回。
- 实际显示器 DPI 为 144（150%）；用户缩放时字体重新烘焙到约 42–43 物理像素，
  恢复默认后回到 25.5 像素。未将用户缩放当作切换物理显示器的证据。
- 3840×2160 客户区的世界目标为 2116×1217；1024×768 时为 531×258，
  与对应可见区域逐像素匹配。UI、世界、操作器分别保持自己的尺寸和提交顺序。
- 原生证据位于 `out/qa/viewport-accepted-{dark,light,high-contrast}-20261024/`；
  方便查看的 PNG 位于 `out/deliverables/viewport-hidpi-20261024/`。
