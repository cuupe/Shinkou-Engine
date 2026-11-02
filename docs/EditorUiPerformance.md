# 编辑器 UI 性能与布局

主编辑器仍使用同一套 Dear ImGui 展示。每帧构建可见控件，不意味着必须每帧遍历整个项目、复制全部模型或生成测试辅助数据。

## 本轮优化

- 层级先按展开状态缓存为扁平行，由 ImGuiListClipper 提交可见行和测量所需的常数行。滚动、悬停和选择不重建列表；筛选时保留匹配对象的祖先链，退出筛选后恢复折叠状态。
- 搜索栏固定在滚动区外；水平内容宽度按数据/字体变更缓存，避免长名称进入可视区才出现横向滚动条并遮住末行。首行对齐物理像素，行高由实际绘制步长测量；QA 区域裁剪到可见矩形。Home/End 的列表滚动独立处理。

水平范围取整个层级的缓存宽度，在筛选和折叠时保持稳定，因此不会每输入一个字符就重新测量全部标签；该策略也避免滚动条引起的视口高度跳变。
- World 为创建、删除、重命名、重设父级和启用状态维护 hierarchy_revision。EditorUiModel 检查版本，替代逐帧全树哈希；组件类型列表也按注册版本缓存。World 实例身份防止同一地址上的新 World 误用旧缓存。
- 选中状态通过节点索引更新，属性变化不再让无关层级失效。属性值仍按帧同步，以保留脚本或运行时从编辑器外修改属性的可见性。
- QA 记账默认关闭，生产帧不生成区域映射、动态 QA 标识或世界对象投影。开启后只记录已提交的控件。Delete 等功能使用真实 ImGui 焦点，不依赖 QA 字符串。
- 视口操作器复用 EditorWorldViewport 已提取的 RenderView，避免重复提取 RenderScene。
- 增加 Editor.UI.ModelSync、Editor.UI.Hierarchy、Editor.UI.Build、Editor.UI.RenderData profiler 区间。性能面板显示缓存行数、实际提交行数、重建次数及 QA 状态。

## QA 开关

默认关闭。自动窗口测试已有 SHINKOU_UI_QA_SNAPSHOT 时会自动开启；也可设置环境变量 SHINKOU_EDITOR_UI_QA=1，或调用 EditorLayer::set_ui_qa_enabled(true)。关闭时清空历史区域，测试消费者应在开启后等待一次 draw。

正常输入不读取这些记录。ui_interaction_regions 是测试观察接口，不是生产命中系统。

## 基准方法与结果

基准：MinGW Debug、Null renderer、20,001 个对象、1600×1000 物理像素。每档先预热 6 帧，再记录 30 帧 prepare/draw 的 CPU 时间；输入注入在计时外。100%/125% 是基准的 UI 缩放，不是更改 Windows 显示器 DPI。该结果不包含真实 GPU、呈现等待、完整 World 更新或字体冷启动，不能换算为整机 FPS。

| 负载 | 优化前 | 优化后复测 |
| --- | --- | --- |
| 空闲 p50，100% | 38.99 ms | 0.87 ms |
| 空闲 p50，125% | 38.90 ms | 1.06 ms |
| QA 区域数 | 每帧 40,025 | 生产模式 0；开启 QA 时约 100 条 |
| 层级控件提交 | 整棵展开树 | 100% 43 行；125% 35 行 |

优化后同时记录 hover、scroll、selection、typing、resize。筛选输入的每个新字符仍有 O(N) 匹配/重建成本，最终复测的输入 p50 为 9.19/9.68 ms，最大值 11.37/11.29 ms；首轮记录中观测过 23.28 ms 尖峰，完整 CSV 保留该轮数据。11 个输入事件的样本量不代表稳定 p95。

原始记录：out/ui-perf-baseline.csv、out/ui-perf-after-qa-off.csv、out/ui-perf-after-qa-on.csv、out/ui-perf-workloads-qa-off.csv、out/ui-perf-workloads-qa-off-repeat.csv、out/ui-perf-workloads-qa-on.csv。

最终版本记录为 out/ui-perf-delivery-qa-off.csv、out/ui-perf-delivery-qa-on.csv；全量 74 项通过见 out/ui-perf-delivery-tests.log。三个后端的大层级原生验收各 18 步，既有场景/属性/独立窗口验收 88 步，均采用 GPU 回读。证据索引位于 out/deliverables/ui-performance-20261025/verification.json。

```powershell
cmake -S . -B out/build/mingw-debug -DSHINKOU_BUILD_BENCHMARKS=ON
cmake --build out/build/mingw-debug --target shinkou_editor_ui_benchmarks
out/build/mingw-debug/shinkou_editor_ui_benchmarks.exe 20000
Tools/UiCapture/RunHierarchyPerformance.ps1 -Backend dx11
```

基准也可作为真实窗口压力工具：shinkou_editor_ui_benchmarks --native dx11 <项目路径> 20000 dark。支持 dx12、vulkan 和 light/high-contrast。

## 布局由什么定义

| 层级 | 当前来源 |
| --- | --- |
| 控件结构、行为、工具栏、属性行 | EditorImGui*.cpp 中的 C++ |
| 首次默认停靠拓扑和比例 | EditorImGuiView::workspace 的 C++ DockBuilder |
| 用户拖动、分割、浮动与独立窗口的位置 | 布局文件旁的 .imgui.ini |
| 主题、语言、字号、缩放、窗口可见性 | 编辑器布局/样式 JSON |

当前没有 XML 控件布局解释器。已有 DockWorkspace JSON 是公开工作区数据格式，当前 ImGui presenter 的默认拓扑仍由 DockBuilder 创建，不能把它当成任意控件布局文件。

如果后续把默认布局移到 JSON/XML，可以在载入或热重载时解析为缓存描述，帧内仅执行缓存布局；文件格式本身不会替代虚拟化或减少 GPU 重绘。当前视口仍按帧渲染 World，静态视口的脏更新及空闲降频属于后续独立优化。
