# 编辑器功能实现与验证记录

日期：2026-09-05 至 2026-09-06。输入：UIHandoff、UIWorkflow、UIAcceptance、当前 retained UI 与未提交工作区。

实现契约：保留原生窗口/菜单、顶部命令栏、左侧层级、中央 RenderView、右侧检查器、底部资源/日志和状态栏。沿用语义主题与扁平分隔线；输入包含 hover/pressed/focus/selected/disabled，窄资源面板将搜索与完整文字按钮分行。禁止 paint 中 IO、第二套 UI runtime 和静默后端回退。

写集：engine 的 editor/UI 输入、场景与生命周期接缝、D3D11 UI 计量、对应测试、Tools/UiCapture、CMake 和本次验收文档。保留既有数学/资产/旧 UI 退役改动。

输出与验收：

1. 资源浏览的祖先过滤、键盘导航、真实文件操作、滚动条与可见区域命中一致。
2. 对象/组件检查器读写真实属性；创建、删除、撤销/重做、场景读写和播放控制产生真实状态变化。
3. 日志、设置和诊断面板消费真实数据，未实现能力明确显示。
4. 输入/layout/index/paint/raster/upload/composite/present 可测量，交互脚本在临时项目执行。
5. 完整构建和 CTest；D3D11 GPU 像素检查；H/G/O/D 分开记录。真实系统 DPI 无法覆盖时保留 inconclusive，不能以 UI scale 冒充。

## 实际实现

- 输入：启用 SDL 文本输入；鼠标按钮、滚轮使用事件自己的客户区坐标；失焦释放拖动与修饰键状态。面板裁切和命中范围一致，按实际绘制顺序组织 retained 区域，拖出按钮后松开不触发点击。
- 资源浏览：Small / Large / Tree 保留选中路径；搜索补齐深层祖先并穿透折叠目录；方向键、Home/End、Page、Enter、F2、Delete、Escape、返回目录、可拖滚动条可用。窄面板保留完整按钮文字并给资源行预留高度。真实文件操作经过项目根目录约束，删除需确认，验收只操作新建临时项目。
- 检查器：从真实 GameObject / Component 属性生成输入；名称、激活、组件启用、反射属性写回对象，拒绝非法类型、范围和非有限数字；外部名称、激活和层级变化会更新视图。组件列表添加真正注册的类型。
- 编辑文档：创建空对象/子对象、删除、最多 64 步撤销重做、新建/打开/保存 `.scene`。版本化 JSON 保留 OOP 父子关系、序列化属性、组件启用及 Tag 数据；替换前先验证，失败保留原场景。Windows 覆盖保存使用原子替换，避免删除旧文件后提交失败。文本读入上限 16 MiB。
- 运行与视口：Play/Pause/Step 控制 World 和 Physics 的实际更新；编辑状态不推进模拟。中键环绕、Shift+中键平移、滚轮缩放作用于活动相机；方向视图和 Frame Selected 可用。去掉未实现的鼠标拾取提示。停止模拟保留运行时变化。
- 设置与反馈：原生菜单接入场景、布局、编辑器设置及窗口命令；设置面板激活后可滚动、切换主题/比例并保存。Console 可清空，Profiler 每 250 ms 更新真实 CPU 指标，能力面板显示真实后端状态。重置布局保留项目专属存储路径。
- 呈现：修正 D3D11 feature-level 数组长度；清屏后恢复世界 viewport/scissor，UI 合成显式恢复全客户区状态，消除世界越过底部页签的像素泄漏。保留 D2D/DirectWrite/SVG 缓存与 dirty upload。
- 验收工具：新增可复跑的临时项目脚本、结构化操作日志、状态快照、相机/滚动断言和 GPU 多阶段捕获。状态文件用结束标记确认写完，位图校验完整像素长度后才复制。

## 证据边界

**H：** `out/qa/ui-functional/build.log`、`ctest.log`。全部 40 项测试通过，包含文件覆盖/提交失败、默认 Lifetime 往返、损坏文档回滚、属性校验、真实文件操作、焦点丢失、设置滚动/保存、撤销重做、相机控制及模拟门控。

**G / O：** `Tools/UiCapture/RunEditorInteraction.ps1` 每次新建独立项目。完整脚本 122 步，包括三种视图、祖先搜索、F2 重命名、新建目录、取消/确认删除、双击目录、返回、键盘选中揭示、滚动条/滚轮、输入、撤销重做、保存、resize、缩放和环绕相机。每步失败会使工具退出非零。操作经过 Win32 → SDL → InputSystem → EditorUi，不直接调用编辑命令；QA 消息仅导出状态或请求 GPU 捕获。该证据属于自动化原生窗口消息，不是物理硬件输入或前台 SendInput 验收。

最终主题运行与截图路径见下方结果表；旧 `ui-functional` 与首次 light 运行保留调试记录，不作为最终主题通过证据。PNG 仅由捕获的 BMP 无损转换，没有重绘或修改内容。

| 场景 | 最终证据目录 | 结果 |
| --- | --- | --- |
| dark | `out/qa/ui-release-dark-0906` | 122 步通过；Tree / Small / Large / Inspector / 窄窗口 / 相机截图 |
| light | `out/qa/ui-release-light-0906` | 122 步通过；修正文件图标纸面与白色背景的对比度 |
| high-contrast | `out/qa/ui-release-contrast-0906` | 122 步通过；按钮、输入和面板边界可见，按下反色反馈 |
| 中央水平分隔条 | `out/qa/ui-final-splitter-0906` | 原生拖动完成；`assertions.json` 验证 viewport 高度实际变化，保留前后 GPU 截图 |

最终代码重新构建、40 项 CTest 通过后运行以上三套脚本，共 366 个成功步骤；额外的分隔条脚本通过。以上最终窗口测试均使用 fixture 项目。复跑入口为 `Tools/UiCapture/README.md`。

**D：** 枚举当前显示器得到唯一显示器 `dpi=144,144`。实测客户区 1920×1080、1600×900、1280×720，均为 150% 系统 DPI；当前 client 与 GPU surface 尺寸一致。100%/125% 矩阵为 **inconclusive**，没有修改系统缩放，也没有用 UI scale 代替系统 DPI。D3D12/Vulkan/Null 的 retained UI capability 为 false；只有 D3D11 有本轮 GPU 像素证据。

11 个内置 SVG 与 `out/install/ui-current/share/shinkou/editor/icons` 的 11 个安装资源仍一致，图标合约测试通过。解析器仍是内置路径所需的 M/L/H/V/Z 子集。

## 性能

每种 workload 保存最多 2048 帧；CSV 包含 input、model、index、layout、paint、D2D raster、upload、composite CPU、Present 阻塞、full/dirty bytes 和逐帧 cache-hit 指标的 p50/p95/max。未执行的阶段记为该帧 0；0 个样本不能视为通过。各阶段百分位不能相加当作总帧时间，composite CPU 不能冒充 GPU 时间。

`out/qa/ui-release-dark-0906/interactive.bmp.timings.csv`：

| 负载 | 帧数 | input p95 ms | paint p95 ms | raster p95 ms | Present p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| idle | 1417 | 0.0019 | 0 | 0 | 0.2848 |
| hover | 93 | 0.0196 | 0.3437 | 4.5124 | 0.2728 |
| input | 131 | 0.2522 | 0.2836 | 6.3975 | 0.3416 |
| filter | 7 | 0.0077 | 0.1726 | 5.7686 | 0.245 |
| scroll | 52 | 0.0309 | 0.3492 | 6.1058 | 0.3747 |
| resize | 4 | 0.0017 | 0.2719 | 395.5113 | 1.043 |

hover 的 full-upload p95 为 0，dirty-upload p95 为 208656 bytes；内容变化允许整面上传。scroll/drag 与普通 hover 分开归类。filter/resize 样本较少；resize 包含初次布局/冷启动，存在 395.5113 ms 栅格峰值，**不能据此宣称 resize 已稳定 60 FPS**。GPU readback、驱动、字体冷缓存也会影响这类验收负载。完整数据保留在 CSV，未删去慢帧。

## 仍未实现或未验收的能力

这是一套已接通的基础编辑器工作流，不等于完整生产级引擎编辑器平台已经验收完毕。

1. `.scene` 当前保存 OOP 对象树，不是任意 ECS/物理世界快照。带有 ECS ownership 的对象因缺少 codec 会明确拒绝保存/撤销快照；独立 ECS 实体不会被文档新建/恢复清除。恢复会重新创建 ObjectId / ComponentId，外部旧句柄失效。示例的原始 ECS 绘制实体尚未与 OOP 检查器绑定，因此不能把对象属性编辑当作所有渲染实体的可视化编辑证据。
2. 任意资源导入、网格/材质/媒体预览 provider、3D/2D/UI 对象工厂、独立 Game render target、鼠标拾取/变换 gizmo、投影世界网格和自定义 retained 面板内容 API 尚未完成。相应界面明确报告缺少能力，工厂菜单禁用，不再把空 Transform 冒充完整对象。当前视口格线是屏幕辅助线。
3. 文本编辑支持整值替换、UTF-8 退格、Ctrl+A、提交和取消；完整光标/选择区编辑、系统剪贴板和 IME 组合态仍未验收。
4. 未覆盖真实 100%/125% DPI、其他操作系统、D3D12/Vulkan 原生呈现，以及连续 resize 的充分性能样本。按 UIHandoff 的整阶段判定，当前不能标记“全面验收通过”。
