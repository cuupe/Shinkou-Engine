# 资源预览与专属配置

在项目面板选择资源，检查器会显示“预览”和“专属配置”。配置实时影响当前预览，点击“保存配置”后写入项目目录；重新选择资源会恢复已保存值。脚本预览不会执行脚本。

| 类型 | 当前功能 | 专属配置 / 操作 |
|---|---|---|
| TXT、Markdown、JSON、CSV、常见脚本、Shader、场景/材质文本 | UTF-8 / 带 BOM 的 UTF-16、完整滚动、内容搜索、行号、复制 | 换行、文字缩放、行号；脚本可在已配置 IDE 打开 |
| DOCX | 中文正文、标题级别和表格文字，内存读取 ZIP/OOXML | 标题样式、换行、文字缩放、复制；不包含 Word 分页、嵌入图片和复杂排版 |
| 图像 | 现有图像解码器与 GPU 预览 | 透明棋盘背景、预览缩放 |
| WAV、MP3 | 波形、采样率/声道/时长、播放/暂停/停止、定位 | 总线、音量、音高/速率、声像、循环、流式读取、淡入、空间位置/距离衰减；独立试听轨道上的低通与混响 |
| MP4 等系统可解码视频 | 实际视频帧、播放/暂停/停止、定位、前后逐帧 | 循环、预览速率；当前是画面预览，不同步播放视频音轨 |
| FBX | 独立 3D 预览，节点变换、单位/坐标转换、法线、UV、材质颜色 | 旋转/缩放/重置相机，自动旋转、GPU 线框显示；FBX 纹理与动画播放尚未接入 |
| OBJ、glTF、GLB | 复用现有独立模型预览、材质/纹理与 glTF 动画能力 | 自动旋转、GPU 线框显示与既有材质、纹理、动画选择 |
| ZIP | 目录与未压缩大小列表，不解压到磁盘 | 换行、行号、文字缩放 |

音频“创建场景音源”创建可撤销的 AudioSource，并复制片段路径、总线、音量、音高、循环、流式读取、空间与距离参数。声像、淡入和效果链是当前试听参数，不会假装已经序列化为场景组件功能。视频支持依赖 Windows Media Foundation 安装的编解码器；音频其他扩展名依赖实际解码器。未支持的格式明确显示不可用状态，不会把其他模型格式误当 OBJ 解析。

目前完整的编辑器 3D 场景和模型 GPU 预览使用 D3D11；D3D12/Vulkan 仍受现有编辑器视口能力限制。文档与配置数据层不依赖 GPU，保留旧 UI 路径用于 headless 验证。

## 配置与生命周期

- 配置位置：`Saved/Editor/AssetSettings/<项目相对资源路径>/settings.json`，不修改源文件。
- 资源或目录重命名与撤销/重做时，配置子目录一起迁移。删除资源保留配置，使回收恢复后可继续使用。
- 未保存的当前配置会跟随选中资源重命名；切换资源时恢复目标资源的已保存配置。
- 保存使用 FileSystemService 原子写入；读取先验证版本、有限浮点数与范围，损坏配置不会覆盖调用方的有效值。
- AudioSystem 缓存区分流式和解码资源。同一个路径切换加载方式不会错误复用另一种描述符。试听效果使用独立轨道，停止、切换资源或关闭编辑器时释放。

## 实现与限制

独立模块：`EditorTextPreview`（只读文本/DOCX/ZIP）、`EditorFbxPreview`、`EditorAssetSettings`（配置序列化）、`EditorAssetInspection`（编辑器与音频桥接）、`EditorImGuiAssetOptions`（上游 ImGui 控件）。解码在异步任务中执行，UI 消费不可变快照。完整文本由快照共享，旧 retained 检查器只保留 24 行，避免每帧复制整篇文档。

文本读取最多 1 MiB / 10,000 行。DOCX/ZIP 压缩包最多 64 MiB，Word XML 解压后最多 8 MiB，显示文字有额外预算。FBX 源最多 128 MiB，解析分配器和三角形/顶点数量有限额；支持取消加载。读取不跟随 Word 外部关系、宏或 FBX 外部文件路径。

第三方库与固定提交见 [资源读取器清单](../third_party/ASSET_READER_LIBRARIES.md)：ufbx、miniz、pugixml。源文件和许可证随仓库离线提供。

## 重复验证

构建 Debug 后运行 CTest。Windows 测试需要可写的 TEMP/TMP；可将它们指向工作区 `out/test-tmp`。原生预览回归：

```powershell
Tools/UiCapture/RunAssetInspection.ps1
Tools/UiCapture/RunAssetInspection.ps1 -Scenario narrow -Theme high-contrast
```

脚本需要 Python 和 ffmpeg（仅生成 QA 文件），创建独立项目并验证源代码、DOCX、WAV/MP3、MP4 逐帧、FBX 和配置保存。截图由引擎 GPU 回读产生，旁边保存窗口状态与逐步操作结果。输入方式是目标编辑器窗口的 Win32 消息与共享键盘队列。该模式通过仅在 QA 环境启用的持续定向悬停，避免后台窗口的系统 MouseLeave 清掉合成鼠标位置；不代表物理鼠标或键盘测试。

2026-10-23 验证：Debug 全量构建、67/67 CTest 通过（`out/asset-inspection-complete-tests.log`）；深色与浅色各 84 步原生交互通过，高对比度下 29 步二进制 FBX、1024×768 缩放、配置保存和 DOCX 交互通过。实际 Windows DPI 为 144（150%），没有将窗口缩放当作其他系统 DPI 的验收。证据目录为 `out/qa/asset-inspection-verified-{dark,light,narrow}-20261023/`。

最终模型预览使用独立 512×512 目标及相同宽高比的投影，避免受主场景停靠尺寸影响。该修正后又通过 4 项受影响的解析、视频、原生模型渲染与编辑器交互回归（`out/asset-inspection-final-validation.log`），并在浅色主题重跑 29 步二进制 FBX / 窄窗口流程（`out/qa/asset-inspection-final-aspect-20261023/`）。
