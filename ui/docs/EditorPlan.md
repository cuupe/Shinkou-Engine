# ShinkouUI 编辑器完整实现计划

## 目标

编辑器不是静态样式预览，而是一套可操作的 UI 设计工作区：

- 组件可以添加、删除、复制、选中、重排和嵌套。
- 每个组件有独立属性、状态、事件和可序列化实例数据。
- 样式提供接近 CSS 的选择器、状态、继承、任意属性和动画参数，但配置语言使用 XML。
- WPF 只负责编辑器壳和工具面板，C++ shinkou_ui 负责运行时控件树、布局、命中测试、事件和渲染提交。
- Windows 底层能力通过平台适配器隔离，未来增加 macOS/Linux 时不改变控件层 API。

## 当前已完成

1. C++ 控件树与基础布局：Panel、Button、ToggleButton、CheckBox、Slider、TextBox、ComboBox、ProgressBar、Separator、ScrollView、ListView、TabView、RichTextBox、Image、Video、Audio。
2. 事件路由：命中测试、pointer capture、capture/target/bubble 阶段、焦点切换、控件状态反馈。
3. CSS-like 样式扩展：StyleRule 支持 selector、state 和任意 property；已有控件样式仍保持快速结构化路径。
4. 编辑文档：添加、复制、删除、选择、属性修改、撤销/重做、XML 序列化。
5. 原生桥：shinkou_ui_native.dll 提供 C ABI，WPF 的组件操作同步到 C++ EditorDocument。
6. Windows 平台能力：DPI、文件打开/保存对话框、Unicode 剪贴板抽象。
7. WPF Studio：组件库、组件树、实例属性检查器、可操作预览、XML 导入/导出。

## 后续分工与里程碑

### M1：编辑器可用性

- 组件树拖拽重排、父子嵌套和多选。
- 对齐线、吸附、缩放、平移、画布网格和安全区。
- 属性检查器按类型生成，支持颜色拾取器、四角圆角、四边 padding/margin、布局约束。
- 完善快捷键：复制、删除、撤销/重做、保存、聚焦画布。

### M2：样式系统

- selector 解析：type、.class、#name、后代、直接子级和状态伪类。
- 样式继承、变量、主题覆盖和组件局部 override。
- 变换、透明度、阴影、渐变、描边、图片填充、字体 fallback。
- XML schema 校验和错误定位，保证非法样式不会让编辑器崩溃。

### M3：反馈与动画

- hover/pressed/focus/disabled/checked/selected 的状态面板。
- 时间轴、关键帧、easing、reduce-motion。
- 编辑器内录制交互状态，预览支持暂停、逐帧和性能统计。

### M4：资源和系统交互

- 文件浏览器、资源拖入、路径校验、图片缩略图和缓存。
- 文本编辑器、剪贴板、外部文件变更监视和冲突提示。
- 音频/视频预览适配器，解码与 UI 主线程隔离。

### M5：性能与稳定性

- 脏区域重绘、布局脏标记、样式缓存、资源缓存。
- 大列表虚拟化、命令池、避免每帧分配。
- 低端电脑档位：禁用阴影/动画/媒体预览，保持 60 FPS 目标。
- Fuzz XML、事件路由、序列化 round-trip 和 ABI 兼容性测试。

## 模块边界

| 模块 | 责任 |
|---|---|
| ui/include/shinkou/uikit | 稳定的 C++ 控件、样式、事件和编辑文档 API |
| ui/src | 控件实现、XML 工厂、Windows 平台适配 |
| ui/src/CAPI.cpp | WPF/其他工具使用的 C ABI |
| render/ | 与 UI 无关的显示列表和渲染后端 |
| ui/studio-wpf | 样式编辑器工作区、属性面板和可视化操作 |

## 验收标准

- 新控件只需要添加 C++ 类、工厂注册、绘制/事件/序列化测试和编辑器描述，即可出现在组件库。
- 同一份 XML 能在编辑器加载、C++ 运行时加载、保存后 round-trip。
- 不使用 WPF 默认控件外观作为运行时 UI；WPF 仅作为编辑器工具壳。
- 运行时 UI 不依赖 WPF、ImGui 或具体操作系统窗口类。
