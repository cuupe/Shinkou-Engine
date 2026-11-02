# 协程与日志优化验证（2026-11-02）

本轮先保存优化前的基准可执行文件和协程运行库，再修改实现，使用相同的基准源码、
同机 MinGW GCC 15.2.0、Release `-O3`，交替运行 before/after 各五轮。下表取每个
工作负载的中位数；基准结束时核对执行量。数字是单机微基准，不是生产帧耗时保证。

| 工作负载 | 优化前 | 优化后 | 解释 |
| --- | ---: | ---: | --- |
| 根任务创建并完成 | 297.1 ns/根 | 219.0 ns/根 | 延迟创建完成事件，约减少 26% |
| 跨帧恢复 | 53.1 ns/次 | 19.4 ns/次 | 下一帧双缓冲与复用就绪向量，约减少 63% |
| 10000 等待者广播并完成 | 222.4 ns/等待者 | 182.6 ns/等待者 | 链表订阅与批量加锁，约减少 18% |
| 100000 休眠任务空闲 tick | 44.5 ns/tick | 47.1 ns/tick | 基本不变；未逐帧扫描休眠任务 |
| 同步子任务 await | 24.4 ns/次 | 24.1 ns/次 | 未改变执行模型，基本不变 |
| 禁用级别日志，无昂贵参数 | 0.73 ns/次 | 0.22 ns/次 | 级别缓存早判断 |
| 禁用级别日志，参数含 100 轮整数计算 | 70.84 ns/次 | 0.18 ns/次 | 参数不再求值 |
| 已启用同步日志，空 sink | 47.06 ns/条 | 68.08 ns/条 | 增加约 21 ns 的生命周期保护成本 |
| 已启用异步日志，Block，含排空 | 371.11 ns/条 | 424.74 ns/条 | 增加约 14%，包含安全保护和独立会话排空 |
| 四生产者异步日志，Block，含排空 | 349.62 ns/条 | 429.12 ns/条 | 增加约 23%，不能声称所有日志路径都更快 |

协程基准使用 10000 个根任务、990000 次跨帧恢复、100000 个休眠根任务和
100000 次同步子任务。日志基准每项 200000 条，禁用昂贵参数项原本会计算参数但
不输出。同步/异步项的空 sink 排除磁盘和控制台影响，仍执行真实消息格式化和
调度；异步 Block 不丢弃记录，计时包括线程池排空。没有用丢消息的吞吐量当作
有效记录吞吐量。

日志的可靠性收益是明确的：原实现直接解引用原子加载出的裸指针，没有保护其
使用期间的对象生命周期；关闭/重建可能与生产者访问重叠。现在每个生产者使用独立
保护槽，发布后再次验证指针。关闭先撤下发布指针，再等待已经成功保护的读者。
嵌套日志共享保护，线程退出回收保护槽。关闭线程不会持有注册互斥锁等待读者。
算法依据见 [WG21 hazard pointer 保护原则](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2530r3.pdf)。

每个日志会话独立拥有线程池与定时刷新器，不修改第三方 spdlog 的全局配置。
关闭时停止周期请求、排空池、直接刷新 sink，避免在满队列里插入刷新消息挤掉
最后一条记录。默认重新打开日志时追加；文件大小达到阈值才轮转。错误和溢出计数
保留供关闭后的诊断读取。`flush()` 仍是异步请求，详细约定见 [Logging.md](Logging.md)。

验证包括 Debug/Release 的协程及日志契约测试、隐藏窗口和 Null 后端的 Engine
阶段/关闭测试，以及 JobSystem、网络回归。新增的日志测试用阻塞 sink 确定性制造
256 格队列的 744 次覆盖，并确认释放后收到 257 条记录且最后一条为预期记录，
关闭没有引入额外覆盖；还验证 sink 抛异常、8000 条并发写入全部保留、40 次写入/
级别修改/刷新与关闭重建竞争，以及短寿命线程复用保护槽。

```powershell
cmake --build out/build/runtime-release --target shinkou_coroutine_tests shinkou_logging_tests shinkou_engine_coroutine_tests shinkou_coroutine_benchmarks shinkou_logging_benchmarks --parallel 4
ctest --test-dir out/build/runtime-release -R '^shinkou_(coroutine|logging|engine_coroutine|jobs|network)_tests$' --output-on-failure
./out/build/runtime-release/shinkou_coroutine_benchmarks.exe
./out/build/runtime-release/shinkou_logging_benchmarks.exe
```

当前机器的 before 二进制、原始五轮输出及中位数摘要保存在被 Git 忽略的
`out/optimization-baseline/`（`results-final.json`、`summary-final.json`）。
Windows 原生 Engine 测试需要正常桌面 DLL 初始化权限；受限沙箱可能在进入 main
前因 Windows loader 初始化失败而超时。协程/日志独立测试不需要创建窗口。
