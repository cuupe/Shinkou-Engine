# Shinkou Editor UI 验收档案

这份档案是 UI 子任务合并前的硬门槛。它把“模型/命令生成”和“窗口像素”分开验收，
也把 Windows/D3D11 已实现能力与其他后端的能力边界分开记录。

## 1. 构建与 headless 合约

在仓库根目录执行：

```powershell
cmake --build out/build/ui-current -j 2
ctest --test-dir out/build/ui-current --output-on-failure -j 2
```

必须满足：构建成功，CTest 全部通过；至少包含 `shinkou_ui_runtime_tests`、
`shinkou_ui_render_tests`、`shinkou_dock_layout_tests`、`shinkou_render_view_tests`、
`shinkou_file_system_tests`。这些测试分别证明 retained layout/input、draw-list/virtualization、
停靠几何、viewport 坐标/网格和项目根目录文件安全，不互相替代。

## 2. 实际窗口像素

构建 `shinkou_ui_capture` 后，使用捕获工具启动编辑器：

```powershell
out/build/ui-current/shinkou_ui_capture.exe `
  out/build/ui-current/shinkou_engine_sample.exe `
  out/build/ui-current/editor-acceptance.bmp 2500 `
  --editor --project "C:/Users/Lenovo/Desktop/Shinkou/Shinkou Engine"
```

验收输出必须包含窗口标题、client/surface 尺寸、DPI、采样方式、`captured=1`，并且
截图用 `view_image` 检查以下区域：

- Windows 标题栏/原生菜单只有一套；工具栏、左侧层级、中央视口、右侧检查器、底部资源/控制台和状态栏有明确边界；
- 世界内容只出现在 central viewport 的 client rect 内，编辑器 UI 不被场景清屏覆盖；
- 资源项通过稳定的矢量图标区分类型；文字类型标记仅可用于明确报告的资源缺失回退，不能算作 SVG 验收证据；
- 图标来自 `engine/resources/editor/icons/*.svg` 的路径资源管线；捕获图中不得出现由文字拼成的类型标识或空白占位图标；
- Small、Large、Tree 三种资源视图都只显示图标和名称，不得出现文件大小列；Tree 必须有可见的折叠箭头、缩进和层级导线；Large 必须为网格布局，Small 必须为紧凑列表；
- 搜索、Small/Large/Tree、刷新、双击目录、右键菜单、重命名、新建文件夹、删除确认可操作；
- 文本没有裁切、重叠或漂移，hover/pressed/focus/selected 状态可辨认。

## 3. 尺寸、DPI 和主题矩阵

至少记录以下场景；若当前捕获工具不能设置尺寸或主题，必须在报告中明确标为“缺证据”，
不得用 1280x720 的一次截图代替：

| 场景 | 必查项 |
| --- | --- |
| 1280x720，100% DPI | 默认 shell、视口占比、文字和资源行 |
| 1600x900，125% DPI | 缩放、输入命中、字体、停靠拖动 |
| 约 1024x768 | 最小宽度、收缩顺序、无重叠和无黑块 |
| light / dark / high-contrast | token 对比度、焦点和危险操作反馈 |

## 4. 性能门槛

分别测量 idle、hover、对象选择、过滤输入、长资源列表滚动、面板切换和 resize。报告需
包含模型同步、layout、input hit-test、paint command、renderer submit、文本/媒体的分段
数据；不能只报告进程启动时间。

正常 Windows 编辑器的目标是稳定 60 FPS（约 16.67 ms/frame），交互不能引入连续多帧
秒级停顿。若设备或后端不满足，必须保留实测数值、后端、尺寸/DPI 和明确的失败原因。

交互验收还需确认：纯 hover/pressed/focus 变化只提交旧、新 region 并集的 dirty rect；后端
不得在这种变化上重新扫描文件系统、重新解析 SVG、重建全部路径几何或上传整张 UI surface。
内容/布局变化可以触发 full repaint，但必须能从 trace 中解释原因。

## 5. 后端能力矩阵

| 后端 | 命令生成 | 真实像素 | 结论规则 |
| --- | --- | --- | --- |
| Windows/D3D11 | 必须非空 | 当前支持，需 GPU capture；应报告 full/dirty upload 路径 | 通过才可宣称编辑器可见 |
| D3D12/Vulkan | 可测试 | 未实现 adapter 时不得宣称可见 | 必须显式报告 capability/error |
| Null/headless | 可测试 | 不产生窗口像素 | 只用于合约/命令测试 |

## 6. 不合规处理

任何子任务出现以下情况均退回：把旧独立 UI 库重新接入、把 scene render target 扩成
全窗口、用清空 command list “优化”卡顿、只给单元测试没有截图、只给截图没有 trace、
对未支持后端静默返回空白，或在 UI paint 中执行同步文件扫描/设备等待。
