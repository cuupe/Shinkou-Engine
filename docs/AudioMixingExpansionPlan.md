# Shinkou 音频混音与效果系统扩展计划

本文档用于给后续智能体提供统一的上下文、边界和实施顺序。目标是把当前的
`AudioSystem + miniaudio` 实时混音基础扩展成可用于编辑器、运行时和音频测试工具的
专业级实时音频框架，同时保持 Win32 测试工具不依赖任何渲染 API。

## 1. 当前基线

当前已经具备以下能力：

- `AudioSystem` 管理资源、Voice、Track、Bus 和 Master 输出。
- miniaudio backend 使用实时节点图连接音频路径。
- 支持 Track、Bus、Master 级别的效果链入口。
- `AudioEffectChain` 已支持 Gain、Pan、Low-pass、High-pass、Compressor、Limiter、Delay、Reverb。
- Win32 原生控件测试程序位于 `Tools/AudioMixerTest/AudioMixerTest.cpp`。
- 测试程序可以切换 Master、Music、SFX、Voice、Ambient、UI 总线，并实时调整部分效果参数。
- 构建目标为 `shinkou_audio_mixer_test`。

### 当前并发播放能力

- `AudioConfig::maxVoices` 默认值为 `128`。
- Win32 测试工具当前显式配置为 `32` 个 Voice。
- miniaudio backend 按 `maxVoices` 预留 Voice 槽位；同一个资源可以创建多个播放实例。
- Voice 池耗尽时，新的 `play()` 返回无效句柄 `0`，当前不会自动抢占旧声音。
- 结束的 Voice 需要通过持续调用 `AudioSystem::update()` 才会被回收并重新进入空闲池。
- `streaming=false` 走 Decode 路径，适合短音效；长音频和音乐可以使用 `streaming=true`。
- 当前还没有 Voice 优先级、Voice Stealing、Virtual Voice、距离裁剪和同类实例数限制。

因此，当前实现适合几十到一百多个并发音效实例，但不能把句柄容量上限直接等同于可稳定运行的实时 Voice 数量。
实际并发上限还取决于采样率、声道数、音频线程缓冲、解码方式、效果链数量和目标硬件性能。

相关核心文件：

- `engine/include/shinkou/audio/AudioDSP.h`
- `engine/src/audio/AudioDSP.cpp`
- `engine/include/shinkou/audio/AudioSystem.h`
- `engine/src/audio/AudioSystem.cpp`
- `Tools/AudioMixerTest/AudioMixerTest.cpp`
- `Tools/AudioMixerTest/README.md`

## 2. 目标架构

推荐保持以下信号路径，不要把效果直接烘焙进源文件：

```text
Audio Asset / Stream
        |
      Voice
        |
   Track Insert Chain
        |
      Track
        |
    Bus Insert Chain
        |
       Bus Group
        |
 Master Insert Chain
        |
   Output / Device
```

每一层都应该支持：

- 插入、删除、重排效果。
- 旁路（Bypass）。
- 湿声/干声比例（Wet/Dry）。
- 参数实时更新。
- 预设保存与恢复。
- 状态查询和错误报告。

## 3. 必须遵守的工程约束

### 3.1 音频线程约束

音频回调中禁止：

- 内存分配、释放或扩容 `std::vector`。
- 文件、控制台、日志和网络 I/O。
- 等待互斥锁、条件变量或长时间系统调用。
- 调用可能触发资源加载的接口。
- 抛出异常。

当前效果节点为了保护链状态使用了互斥锁，这是后续 P0 优先级的技术债务。专业化扩展前，
应改成“控制线程准备完整快照，音频线程无锁切换快照”或使用预分配命令队列。

### 3.2 图结构约束

- Voice 只能连接到一个 Track、Bus 或 Master 入口。
- Track、Bus、Master 的效果节点必须可独立清理，不得依赖析构顺序碰运气。
- 修改路由时不能让音频线程看到半连接状态。
- 所有效果都必须能在无输入时正确处理尾音，尤其是 Delay、Reverb、Chorus 和 Flanger。
- 通道数和采样率必须由 `AudioFormat` 明确管理，不能在效果内部假设固定为立体声。

### 3.3 API 约束

新增公共 API 时优先保持旧接口兼容。后端不支持某能力时：

- 使用明确的默认行为或返回值。
- 不要让 `NullAudioBackend`、测试 Fake Backend 因为新增纯可选能力而无法编译。
- 在 `IAudioBackend` 上优先提供默认空实现，再由 miniaudio backend 覆盖。

## 4. 分阶段实施计划

### P0：实时安全与可观测性

目标：先让当前效果链具备可长期扩展的运行时基础。

任务：

1. 将效果参数更新改为无锁发布。
   - 控制线程构造完整的 `EffectChainSnapshot`。
   - 音频线程在 block 边界切换快照。
   - 旧快照延迟回收，不能在音频线程直接释放。
2. 为每条 Track、Bus、Master 增加效果链状态查询。
3. 增加 Bypass、Wet/Dry 和效果实例名称。
4. 增加每个节点的 CPU 时间、输入峰值、输出峰值、RMS 和削波次数统计。
5. 增加音频线程 underrun、路由失败和效果初始化失败的诊断信息。
6. 为 Delay/Reverb 尾音写自动化测试。
7. 增加 Voice 优先级、抢占策略和虚拟化策略的设计文档，避免 Voice 池耗尽时静默失败。

验收标准：

- 效果参数连续拖动 60 秒不崩溃、不出现明显爆音。
- 音频回调不发生动态分配和阻塞等待。
- 可以查询任意 Track/Bus/Master 的效果链和电平数据。
- 单元测试覆盖空链、旁路链、尾音、重建链和重复启停。

### P1：核心效果包

目标：覆盖常见游戏音频和基础后期制作需求。

建议新增效果：

- 3-band 或 parametric EQ。
- Noise Gate。
- Saturation / Soft Clip / Distortion。
- Chorus。
- Flanger。
- Tremolo。
- Stereo Width。
- Auto Gain 或 Make-up Gain。

实现要求：

- 每种效果独立的 `AudioEffectType` 和参数结构。
- 参数范围、默认值和单位写入注释或元数据表。
- 对异常参数进行 clamp，不允许 NaN、Inf 或负采样率进入 DSP。
- 所有延迟类效果预分配最大缓冲区，参数变化时避免实时线程重新分配。
- 每种效果至少提供离线 `AudioPcmBuffer` 测试和实时 `AudioRealtimeBuffer` 测试。

推荐的参数元数据：

```cpp
struct AudioEffectParameterInfo {
    const char* name;
    float minimum;
    float maximum;
    float defaultValue;
    float step;
    const char* unit;
};
```

后续 UI 不应把参数范围硬编码在窗口代码中，应该从效果元数据生成控件，或至少共享同一份范围定义。

### P2：专业混音工作流

目标：从“能插入效果”升级为“能管理工程和混音状态”。

任务：

1. 效果预设：JSON 或引擎现有序列化格式、版本号、向后兼容。
2. 效果链编辑：插入、删除、移动、复制、粘贴、整条链保存和加载。
3. 参数自动化：时间轴曲线、平滑插值、block 边界处理，避免 zipper noise。
4. Sidechain：Compressor、Gate 支持另一条 Track/Bus 的控制信号，并防止循环依赖。
5. Sends/Returns：支持 Pre-Fader 和 Post-Fader send。
6. Snapshot：保存整套 Bus 音量、Mute、Solo、效果链和参数，并支持平滑切换。

### P3：Win32 专业测试与编辑界面

建议拆分为以下区域：

- 左侧：Track/Bus 树和路由关系。
- 中间：当前选中通道的插入效果链。
- 右侧：效果参数、预设和旁路。
- 底部：峰值表、RMS、LUFS、underrun 和 CPU 统计。

必须继续满足：

- 使用 `HWND`、Common Controls、Dialog、Trackbar、ListView、TreeView 等原生控件。
- 不引入 DX11、DX12、Vulkan、OpenGL 或 ImGui 绘制测试界面。
- 如果需要波形或频谱，优先使用 Win32 控件可承载的离线位图或简单 GDI 绘制，并与音频后端解耦。
- UI 线程只提交命令，不直接触碰音频节点内部状态。

## 5. 推荐的 API 演进

当前的 `set_bus_effects()` 和 `set_track_effects()` 适合第一版批量替换。后续可增加不破坏旧接口的实例化 API：

```cpp
using AudioEffectInstanceId = std::uint64_t;

AudioEffectInstanceId insert_bus_effect(
    AudioBus bus,
    AudioEffectDesc desc,
    std::uint32_t insertIndex = UINT32_MAX);

bool remove_bus_effect(AudioBus bus, AudioEffectInstanceId effect);
bool move_bus_effect(AudioBus bus, AudioEffectInstanceId effect, std::uint32_t newIndex);
bool set_effect_parameter(
    AudioEffectInstanceId effect,
    std::string_view parameter,
    float value);
void set_effect_bypass(AudioEffectInstanceId effect, bool bypass);
```

实现这些 API 时要区分：

- 编辑器/控制线程的对象 ID。
- 音频线程使用的不可变运行时快照。
- 延迟释放的 DSP 状态和缓冲区。

不要让 UI 直接持有 `AudioEffectChain::EffectState*`，也不要把 miniaudio 的 `ma_node*` 暴露到公共引擎 API。

## 6. 智能体任务拆分

后续可以把任务分配给不同智能体，但每个智能体必须先阅读本文档和相关源文件。

### Agent A：DSP 效果

负责 `AudioDSP.h/.cpp`、新效果算法、参数校验、状态准备和离线测试。

不负责 Win32 布局、miniaudio graph 路由或渲染系统。交付物必须包含效果实现、参数说明和边界/尾音测试。

### Agent B：AudioSystem 后端

负责 `AudioSystem.h/.cpp`、Track/Bus/Master 路由、快照切换、命令队列和效果实例生命周期。

不负责具体 DSP 算法或 UI 控件样式。交付物必须包含后端接口、路由生命周期测试和线程安全说明。

### Agent C：Win32 测试工具

负责 `Tools/AudioMixerTest/AudioMixerTest.cpp`、原生控件、参数编辑、预设按钮和诊断显示。

不负责在 UI 中实现 DSP，不得直接访问 miniaudio 对象，不得引入渲染 API。交付物必须包含 UI 到 AudioSystem API 的映射和自动化烟测步骤。

### Agent D：性能与测试

负责音频线程实时性、效果链压力测试、统计数据和基准。

重点指标：最大回调耗时、P95/P99 回调耗时、最大同时 Voice 数、Voice 池耗尽行为、最大效果实例数、内存分配次数、underrun 数量。

## 7. 测试矩阵

每次修改音频系统后，至少执行：

```powershell
cmake --build out/build/mingw-debug --target shinkou_audio_tests -j 4
ctest --test-dir out/build/mingw-debug -R '^shinkou_audio_tests$' --output-on-failure
cmake --build out/build/mingw-debug --target shinkou_audio_mixer_test -j 4
```

手工或自动化烟测至少覆盖：

1. 空效果链播放。
2. 单个 Bus 启用 Reverb。
3. Master 启用 Limiter。
4. Delay/Reverb 播放结束后的尾音。
5. 播放过程中替换整条效果链。
6. 创建、销毁、重建 Track。
7. 同时播放最大数量 Voice。
8. 反复启动和关闭音频设备。
9. 音频设备初始化失败或无默认设备。
10. Null backend 下旧测试仍能编译运行。

如果修改了 Win32 控件，额外确认：

- 900px 最小宽度下控件不重叠。
- 125%、150%、200% DPI 下标签和滑块仍可用。
- 关闭窗口后没有音频线程或测试进程残留。
- 不显示任何渲染窗口或图形 API 窗口。

## 8. 提交前检查清单

- [ ] 是否先阅读了本文档和现有 AudioSystem/DSP 代码？
- [ ] 是否只修改了负责范围内的文件？
- [ ] 是否避免音频回调分配、锁等待和 I/O？
- [ ] 是否为新效果增加了离线与实时测试？
- [ ] 是否验证了空链、旁路、尾音和重建链？
- [ ] 是否保持 Null backend 和 Fake backend 兼容？
- [ ] 是否更新了 Win32 测试工具 README？
- [ ] 是否执行 `git diff --check`？
- [ ] 是否记录了未完成事项和潜在回归？

## 9. 给后续智能体的任务模板

```text
任务：在 Shinkou 音频系统中实现 <功能>。

先阅读：
- docs/AudioMixingExpansionPlan.md
- engine/include/shinkou/audio/AudioDSP.h
- engine/src/audio/AudioDSP.cpp
- engine/include/shinkou/audio/AudioSystem.h
- engine/src/audio/AudioSystem.cpp

约束：
- 不使用渲染 API 实现音频测试 UI。
- 音频回调中不得分配、阻塞或做 I/O。
- 保持 Null/Fake backend 可编译。
- 不覆盖其他智能体未完成的改动。

交付：
- 实现代码。
- 单元测试和实时烟测。
- README 或本计划中对应章节的更新。
- 说明修改文件、验证命令、已知限制。
```

## 10. 当前明确的下一步

推荐下一位智能体按以下顺序开始：

1. 去掉实时回调中的互斥锁，完成效果链快照/命令队列。
2. 增加 Bypass、Wet/Dry、RMS/Peak Meter 和状态查询。
3. 增加 EQ、Noise Gate、Saturation 三个常用效果。
4. 将 Win32 UI 从硬编码参数范围迁移到效果参数元数据。
5. 增加效果链保存/加载和预设。
6. 最后再实现 Sidechain、Sends/Returns 和参数自动化。

这样可以先解决实时可靠性，再扩大效果数量，避免在锁和生命周期问题尚未解决时继续堆叠 UI 和 DSP 功能。
