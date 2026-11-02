# 协程运行时

`shinkou::coroutines` 提供 C++20 无栈协程、主循环调度与 JobSystem 后台工作桥接。
`Engine::coroutines()` 返回随引擎运行和关闭的调度器，也可以单独构造 `Scheduler`。
现有引擎和 `Scheduler.h` 保持 C++17；编写 `co_await` 的翻译单元使用 C++20。

## 设计参考

- [Unity 协程](https://docs.unity.com/en-us/engine/6000.0/manual/scripting/coroutines-section/coroutines)：跨帧恢复，以及协程与线程的职责区分。
- [UniTask](https://github.com/Cysharp/UniTask)：与 PlayerLoop 阶段对齐的恢复、取消和组合接口。
- [Unreal Tasks System](https://dev.epicgames.com/documentation/unreal-engine/tasks-systems-in-unreal-engine)：使用依赖和完成通知组织工作，避免阻塞等待。
- [Godot GDScript await](https://docs.godotengine.org/en/4.1/tutorials/scripting/gdscript/gdscript_basics.html#awaiting-for-signals-or-coroutines)：事件/信号驱动的挂起与恢复。
- [cppcoro](https://github.com/lewissbaker/cppcoro)：惰性任务、值和异常传播、任务组合。

本实现使用标准 `<coroutine>`，没有引入上述项目的源码或运行时依赖。协程本体始终
在调度器的创建线程运行；CPU 密集工作显式交给已有 JobSystem，没有新建线程池。

## 接入

```cmake
target_link_libraries(game PRIVATE ShinkouEngine::Coroutines)
```

这个目标为调用方启用 C++20，仍链接同一个引擎。只使用普通引擎接口的目标可以
继续链接 `ShinkouEngine::Engine` 并使用 C++17。单独集成运行时时，可以链接
`shinkou_coroutine_runtime` 并自行设置 `cxx_std_20`；使用 `background` 的程序还需
链接 JobSystem 实现。

```cpp
#include "shinkou/Engine.h"
#include "shinkou/coroutines/Coroutine.h"

namespace co = shinkou::coroutines;

co::Task<int> prepare(shinkou::jobs::JobSystem& jobs) {
    co_await co::next_frame();
    int answer = co_await co::background(jobs, [] { return 6 * 7; });
    co_return answer;
}

co::Task<void> update_value(shinkou::jobs::JobSystem& jobs, int& output) {
    int value = co_await prepare(jobs);
    co_await co::yield(co::Phase::LateUpdate);
    output = value; // 已回到引擎主线程。
}

// 在创建 Engine 的同一线程调用；jobs 和 output 需要满足下文的生命周期约定。
// auto handle = engine.coroutines().spawn(update_value(jobs, output));
```

任务是惰性且只能移动的。`spawn(Task<void>)` 接管根任务，在下次 Update 执行；
`co_await child()` 接管子任务，继承根调度器并通过对称转移运行。`Task<T>` 支持
移动结果，包括 `std::unique_ptr<T>`，不支持引用结果。丢弃尚未启动的 Task 会
释放其帧；丢弃 TaskHandle 不会取消已启动的根任务。

## 等待与组合

| 接口 | 行为 |
| --- | --- |
| `next_frame(n, phase)` | 至少等待一帧，默认 Update；按调度器帧号等待 n 帧 |
| `yield(phase)` | 较晚阶段可在本帧恢复；相同/较早阶段延至下一帧 |
| `sleep_for(seconds, clock, phase)` | 指定时钟上的相对时间；零秒也至少挂起一帧 |
| `wait_until(predicate, phase)` | 每帧检查一次条件；适合需要轮询的状态 |
| `co_await Event` | 多等待者、手动重置事件；`set/reset/is_set` 可以跨线程调用 |
| `co_await TaskHandle` | 等待根任务；转发失败和取消 |
| `wait_for(handle, seconds, clock)` | 返回是否完成；默认未缩放时间，超时不取消目标 |
| `when_all(handles)` | 等待已启动的全部根任务；全部结束后才转发首个异常 |
| `when_any(handles)` | 返回完成任务的索引；其他任务继续运行 |
| `background(jobs, callable)` | 在 JobSystem 执行，恢复时返回值或重新抛出异常 |

`when_any` 在一次恢复前若已有多个任务完成，返回最小索引；调用方通过对应
TaskHandle 检查失败。这是明确的选择规则，不声称能重建跨线程的精确完成先后。
所有 `handles` 都是观察已有任务，组合器不会隐式取得它们的取消所有权。

事件提前发出不会丢失。对已经登记的等待者，`set()` 后马上 `reset()` 仍保留这次
唤醒；reset 影响后续等待。取消帧时注销订阅，不会在从未触发的事件里留下等待者。
完成通知与超时同时到达只恢复一次；`wait_for` 恢复时若目标已经完成，以完成为准。

## 引擎阶段与时间

Engine 每帧在退出检查之后调用 `begin_frame`。Update 在 World/物理更新前；
LateUpdate 在 World、音频同步之后及编辑器 transform 刷新前；EndOfFrame 在
renderer submit 之后。EndOfFrame 表示 CPU 提交完成，不是 GPU fence 完成。

缩放时间采用引擎限幅后的模拟 delta；编辑器暂停时为零，单步时推进一次。
未缩放时间采用传入 tick 的实际 delta。暂停不会停止 next_frame 或未缩放等待。
独立 Scheduler 的 `begin_frame(scaledDelta, unscaledDelta)` 可接入自己的时间缩放。
时间必须有限且非负；无效参数、时钟溢出、阶段倒序、非 owner 操作和递归 pump
会报告异常。`tick(dt)` 是三个阶段的便捷驱动。

## 取消、异常与生命周期

`TaskHandle::cancel()` 可跨线程调用，它请求在 owner 下一次 `run` 时取消。取消在
owner 上销毁根帧和正在等待的子任务帧，执行 RAII 析构；不会在被取消任务内部
注入可捕获的异常。等待其 TaskHandle 的其他任务会收到 `Cancelled`。
开始前取消不会执行协程体。一个正在运行的协程必须返回或到达挂起点，调度器
才能处理请求；这不是抢占调度。

`TaskGroup` 保存它启动的根句柄，析构请求全部取消。它的析构不阻塞，也不保证
请求已经被 pump。若组内工作引用即将释放的数据，先取消并驱动 scheduler 至组
结束，或显式等待 `when_all`，再释放数据。组长期使用时可重建分批作用域，避免
持续保存所有已完成句柄。

Engine::shutdown 在关闭资产、音频和窗口等系统之前同步 `cancel_all()`。Scheduler
析构还会关闭跨线程收件箱并释放剩余帧；重复 shutdown 安全。后台任务仅持有自己的
结果状态及弱收件箱引用，关闭后到达的通知会被丢弃，不会恢复已销毁帧。

后台 callable 已经提交后不能被强制中断；取消其协程不会等待 callable。callable
必须拥有它使用的数据，或由调用方保证引用持续有效直至 JobSystem 完成。传入的
JobSystem 必须覆盖提交调用的生命周期，并遵循其自身的销毁约定。繁重、阻塞 I/O
任务仍会占用 JobSystem worker；此桥接没有实现平台异步 I/O reactor。

协程的引用参数和捕获闭包遵循 C++ 的普通生命周期规则。优先使用按值参数的
命名协程函数；不要启动一个马上销毁捕获闭包的协程 lambda。只支持本模块提供的
调度型 awaitable；外部原始句柄不得自行 resume/destroy。Scheduler 的创建、pump、
spawn、清空、销毁在同一线程，不可在任务内部递归驱动或关闭它。协程帧的析构函数
不得在清空阶段启动新任务，也不得抛异常。任务依赖必须无环，自等待会立即失败。

嵌套 Task 的异常在 await 处重新抛出。根异常保存在句柄中；Scheduler 还保存首个
未取出的根异常供 `take_exception()` 获取。Engine 每帧读取并写入日志。所有根失败
均计入 stats；多个根同时失败时，可以通过各自句柄取得其独立异常。

## 成本与可观测性

根任务使用可复用的索引槽；句柄包含独立完成状态。内部通知携带 slot、generation
和 suspension epoch，不携带协程帧指针。这同时防止槽重用和同次等待的重复唤醒。
就绪队列按阶段 FIFO，向量保留容量，消费后复用存储。常见的下一帧等待使用双缓冲
向量，插入与转移均为 O(1) 摊销成本；更远的帧等待及 scaled、unscaled 时间各自
使用最小堆，插入和弹出为 O(log N)。相同截止帧的远期请求先于新近的一帧请求转移，
保留请求先后顺序。没有全量 active-task 逐帧遍历，完全休眠帧只检查队列和堆顶。
取消/超时造成的失效堆项采用阈值压缩，空闲时清空，避免长期挂起时间造成无限积累。

帧等待和阶段切换不创建独立等待对象。根的共享完成事件延迟到首次 join 时创建，
无需 join 的根只分配句柄控制块。事件订阅使用稳定地址的链表节点，注销 O(1)，
广播时相邻的同一收件箱共享一次加锁，不创建临时哈希表。协程帧、根控制块、后台
结果、实际需要的完成事件及订阅节点仍需要分配，不能把整个系统称为零分配。
队列和堆保留容量以复用峰值存储。`stats()` 返回
active/peakActive、ready/timers、spawned/completed/cancelled/failed、resumes、budgetExhaustions；
ready/timers 包含尚未收集的失效票据，是物理队列占用而非精确有效等待数。

`EngineConfig::coroutines.maxResumesPerPhase` 默认 4096，限制每阶段处理的就绪票据数；
超过预算的工作保留 FIFO 次序并延迟。恢复过程中 spawn 到相同阶段的根不会在此次
run 中执行。这个预算不限制一个协程内的执行时间、同步子任务链长度、到期堆批量
转移或取消清理成本；长计算应分片或 background，不能依靠预算打断无限循环。

## 验证与测量

```powershell
cmake --build out/build/runtime-release --target shinkou_coroutine_tests shinkou_coroutine_benchmarks --parallel 4
ctest --test-dir out/build/runtime-release -R '^shinkou_coroutine_tests$' --output-on-failure
./out/build/runtime-release/shinkou_coroutine_benchmarks.exe
```

本机 MinGW GCC 15.2.0，Release `-O3`，2026-11-02 首轮测量：

| 工作负载 | 测量 |
| --- | --- |
| 10000 根任务创建并完成 | 4.177 ms，约 418 ns/根 |
| 990000 次跨帧恢复 | 83.999 ms，约 85 ns/次 |
| 100000 休眠根任务，10000 次空闲 tick | 0.709 ms，约 71 ns/tick |
| 100000 同步子任务 await | 3.668 ms，约 37 ns/次 |

这是单机微基准，不是跨机器保证或与其他引擎的横向性能比较。基准检查最终工作量，
跨帧测量在预热等待堆之后计时，休眠测量在所有任务已经挂起之后计时。当前实现没有
专门的 coroutine frame allocator；是否需要帧池应由实际工作负载分配数据决定。

契约测试覆盖阶段/帧数、暂停时钟、零等待、返回值、移动结果、异常、嵌套取消清理、
事件提前触发/重置/广播、槽重用、join/超时、when_all/when_any、条件等待、预算、
2000 子根扩容、时间堆压缩、100 轮共 6400 个等待者的并发完成/取消竞争、单 worker
后台工作、跨线程取消、关闭后迟到通知、错误线程与递归驱动。Debug 和 Release 都运行。

`shinkou_engine_coroutine_tests` 另外使用隐藏窗口和 Null 渲染后端验证真实 Engine
阶段顺序（协程 Update → World → 协程 LateUpdate → render callback → 协程
EndOfFrame），以及 shutdown 同步清理和未初始化时的取消。它通过
`ShinkouEngine::Coroutines` 目标编译，同时检查调用方的 C++20 构建接入。

后续优化加入了下一帧双缓冲、可复用就绪队列、延迟完成事件和链表订阅。
新增测试覆盖远期/近期等待同帧到期的 FIFO、预算延迟、失效票据与槽复用，以及
交错订阅同一事件的多个调度器。五轮交替测量的最新数据及代价见
[协程与日志优化报告](RuntimeOptimization.md)。
