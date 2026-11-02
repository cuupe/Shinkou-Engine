# 原生编辑器界面接入

主编辑器采用 Dear ImGui Docking。HelloImGui 提供完整主题，ImPlot 提供性能曲线，
ImGuizmo 提供场景网格和变换操作器。依赖源码与许可证已纳入 `third_party/`，
版本、来源及用途见 [依赖清单](../third_party/EDITOR_UI_LIBRARIES.md)。

## 构建和启动

```powershell
cmake --preset mingw-debug
cmake --build out/build/mingw-debug --parallel 4
out/build/mingw-debug/shinkou_engine_sample.exe --editor dx11 --project <项目绝对路径>
```

`SHINKOU_ENABLE_IMGUI` 默认开启，使用仓库内固定版本；该部分构建无需下载。
关闭后仅保留控制器和运行时库；编辑器展示初始化会明确失败。默认编辑器后端为 D3D11。

## 代码边界

- `EditorImGuiView.cpp`：上下文、字体、上游主题、工作区停靠、菜单、工具栏与帧提交。
- `EditorImGuiPanels.cpp`：场景层级、属性、资源列表、视口与 ImGuizmo。
- `EditorImGuiTools.cpp`：预览、媒体控制、构建、恢复、日志、ImPlot 与渲染图表格。
- `EditorLayer`：继续管理文档、撤销/重做、异步资源、构建进程与命令校验。

控件通过控制器修改数据；属性编辑、资源拖放及变换操作器复用现有事务。
每次变换拖动只创建一次撤销检查点。业务逻辑不再混入另一套失效的 ImGui 面板实现。
通用 retained UI 模块作为独立库 API 保留，编辑器不创建或绘制它们。

属性控件的身份包含所属对象，切换选择时同步属性快照，防止未提交的文本写入另一对象。
资源刷新和重复选择保持已解码模型、材质与纹理的缓存状态，避免检查器退回仅元数据。

Win32 窗口使用 Unicode 消息，官方 Win32 backend 接收输入。每帧先执行官方 renderer
和 platform `NewFrame`，随后生成控件并提交 `ImDrawData`。原生编辑器不再叠加旧 UI
绘制列表。关闭时先等待 GPU 并释放 renderer backend，再销毁上下文。

## 工作区和视口

默认布局包含左侧层级、中间场景、右侧检查器及下方项目/控制台标签。菜单或工具栏
“面板”可打开性能、构建、媒体、设置、恢复与渲染图窗口。支持上游标签拖动、分割、
浮动与布局持久化；设置中的“重置停靠布局”恢复默认拓扑。

停靠信息保存在项目布局文件旁的 `.imgui.ini` 中。语言切换使用稳定的 `###panel-id`，
避免译名改变窗口身份。主题和尺寸在帧边界应用，避免累计 DPI 缩放。

场景使用真实 GPU 输出。ImGui 视口矩形采用物理客户区像素，控制器入口与 QA 记录
显式换算为逻辑像素。新工作区允许渲染图写入明确的离屏目标，再合成进场景区域；
D3D11 不再将这些写入重定向到窗口后读取空纹理。模型检查器直接采样独立预览纹理，
不覆盖主场景。

Scene/Game 现各自持有匹配物理像素的颜色与深度纹理，并通过 ImGui 图像显示 World。
高 DPI 字体重建、右键转向、中键相机平面平移和左键三角形拾取的实现边界与验证
方式见 [世界视口说明](EditorViewport.md)。

DX11、DX12、Vulkan 均已适配场景、模型和独立窗口。现有契约与原生测试入口见 [后端说明](EditorRendererBackends.md)。渲染阶段顺序通过渲染管线 API 配置。

## 验证

```powershell
Tools/UiCapture/RunImGuiInteraction.ps1 -Theme dark
Tools/UiCapture/RunImGuiInteraction.ps1 -Theme light
Tools/UiCapture/RunImGuiInteraction.ps1 -Theme high-contrast
```

每轮使用独立项目，保存完整 GPU 客户区截图、控件矩形与逐步断言。
检查精确中文改名、Ctrl+A/Z/Y、运行/暂停/单步、资源搜索、面板切换、性能曲线，
以及 1440×900 → 1024×768 → 1440×900 的布局。键盘采用连接子编辑器输入队列的
合成事件；不将这些记录描述为物理键鼠测试。

本机真实窗口 DPI 为 144（150%）。改变客户区大小或界面缩放不能作为其他系统 DPI
已经验收的证据。测试输出目录为 `out/qa/`，不将本机截图或项目缓存作为引擎源码。

### 2026-10-23 验证记录

- Debug 全量构建通过：`out/editor-cached-model-build.log`。
- 65 项回归通过：`out/editor-regression-final.log`，包含模型缓存重选回归。
  新增模型缓存用例在修复前确定性失败，记录在 `out/editor-model-regression-before.log`。
- 深色、浅色、高对比度各 59 步窗口交互通过，含中文精确改名、撤销/重做、
  未提交属性切换保护与窗口缩放。证据位于 `out/qa/imgui-selection-*-20261023/`。
- OBJ 模型检查器与主场景同时显示的 GPU 截图：
  `out/qa/imgui-model-fixed-20261023/model.bmp`。
- 当时全量 66 项中的 `shinkou_file_system_tests` 失败，断言位于
  `FileSystemTests.cpp:154`（悬空目录链接创建）。该用例已单独复核失败，
  记录为 `out/editor-filesystem-recheck.log`，不计入上述 65 项通过结果。

资源检查器后续改进已补齐 DOCX、音频高级参数、真实 MP4 帧与 FBX 预览，见
[资源预览与配置](EditorAssetInspection.md)。悬空链接测试改为先创建目标再删除目标，
保留悬空状态验证；当前全量 67 项已通过，记录为 `out/asset-inspection-complete-tests.log`。

默认系统临时目录不可写时，应将测试进程的 `TEMP` / `TMP` 设置为工作区中独立的
临时子目录。当前环境中沙盒内 Windows DLL 初始化也可能返回 `0xc0000142`；
本轮窗口和回归证据来自正常 Windows 桌面环境。

## 2026-10-25 管线收敛

旧 EditorUi、DX11 专用 UI compositor 和旧 SVG 解析器已删除。共享浏览模型与控件、控制器模式及测试边界见 [UIWorkflow](UIWorkflow.md)。
