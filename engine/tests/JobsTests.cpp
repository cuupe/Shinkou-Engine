// Behavioural tests for shinkou::jobs::JobSystem.
//
// The suite is deliberately free of assert(): it must stay strict in Release
// builds too, where NDEBUG would turn every check into a no-op.

#include "shinkou/jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using shinkou::jobs::Counter;
using shinkou::jobs::JobSystem;

int g_failures = 0;
int g_checks = 0;

void report(bool condition, const char* expression, const char* file, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "  FAILED: %s  (%s:%d)\n", expression, file, line);
    ++g_failures;
}

#define CHECK(...) report((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

// A structural check: reports every index that was executed zero times or twice.
void check_exactly_once(const std::vector<std::atomic<int>>& hits, const char* label) {
    std::size_t missing = 0;
    std::size_t duplicated = 0;
    std::size_t first_missing = hits.size();
    std::size_t first_duplicated = hits.size();
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const int value = hits[i].load(std::memory_order_relaxed);
        if (value == 0) {
            if (first_missing == hits.size()) first_missing = i;
            ++missing;
        } else if (value > 1) {
            if (first_duplicated == hits.size()) first_duplicated = i;
            ++duplicated;
        }
    }
    if (missing != 0 || duplicated != 0) {
        std::fprintf(stderr, "  %s: %zu missing (first %zu), %zu duplicated (first %zu)\n",
                     label, missing, first_missing, duplicated, first_duplicated);
    }
    CHECK(missing == 0);
    CHECK(duplicated == 0);
}

void test_worker_count() {
    JobSystem three(3);
    CHECK(three.worker_count() == 3);
    CHECK(three.worker_count() <= JobSystem::kMaxWorkers);

    JobSystem automatic(0);
    CHECK(automatic.worker_count() >= 1);
    CHECK(automatic.worker_count() == JobSystem::hardware_worker_count());

    JobSystem clamped(1000);
    CHECK(clamped.worker_count() == JobSystem::kMaxWorkers);
}

void test_submit_and_wait() {
    JobSystem system(4);
    std::atomic<int> executed{0};
    Counter counter;
    constexpr int kJobs = 2000;
    for (int i = 0; i < kJobs; ++i) {
        system.submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); }, &counter);
    }
    system.wait(counter);
    CHECK(executed.load() == kJobs);
    CHECK(counter.remaining() == 0);
    CHECK(counter.done());
}

void test_counter_manual_completion() {
    Counter counter;
    counter.add(2);
    CHECK(counter.remaining() == 2);
    CHECK(!counter.done());
    CHECK(!counter.complete()); // one completion left
    CHECK(counter.remaining() == 1);
    CHECK(counter.complete()); // this call reached zero
    CHECK(counter.done());
    CHECK(!counter.complete()); // no underflow, no duplicate completion
    CHECK(counter.remaining() == 0);
}

void test_nested_submit() {
    JobSystem system(4);
    std::atomic<int> leaf{0};
    Counter outer;
    system.submit([&system, &leaf, &outer] {
        Counter inner;
        for (int i = 0; i < 5; ++i) {
            system.submit([&leaf] { leaf.fetch_add(1, std::memory_order_relaxed); }, &inner);
        }
        system.wait(inner);
    }, &outer);
    system.wait(outer);
    CHECK(leaf.load() == 5);
}

void test_parallel_for() {
    JobSystem system(4);
    constexpr std::size_t kCount = 200000;
    std::vector<std::atomic<int>> hits(kCount);
    for (auto& slot : hits) slot.store(0, std::memory_order_relaxed);

    system.parallel_for(kCount, 64, [&hits](std::size_t index) {
        hits[index].fetch_add(1, std::memory_order_relaxed);
    });

    check_exactly_once(hits, "parallel_for default grain");
}

void test_parallel_for_uneven_work() {
    JobSystem system(4);
    constexpr std::size_t kCount = 4000;
    std::vector<std::atomic<int>> hits(kCount);
    for (auto& slot : hits) slot.store(0, std::memory_order_relaxed);

    // Varying cost per index: the dynamic index counter has to spread it out.
    system.parallel_for(kCount, 1, [&hits](std::size_t index) {
        const unsigned spins = static_cast<unsigned>(index % 7) * 500u;
        for (unsigned i = 0; i < spins; ++i) std::this_thread::yield();
        hits[index].fetch_add(1, std::memory_order_relaxed);
    });

    check_exactly_once(hits, "parallel_for uneven work");
}

void test_parallel_for_serial_fallback() {
    JobSystem system(4);
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<bool> ran_on_caller{true};
    std::atomic<int> executed{0};
    // grain >= count collapses to the caller thread, which is the documented
    // deterministic mode.
    system.parallel_for(16, 16, [&](std::size_t) {
        if (std::this_thread::get_id() != caller) ran_on_caller.store(false);
        executed.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(ran_on_caller.load());
    CHECK(executed.load() == 16);
}

void test_parallel_for_nested() {
    JobSystem system(4);
    std::atomic<int> total{0};
    system.parallel_for(8, 1, [&system, &total](std::size_t) {
        system.parallel_for(1000, 32, [&total](std::size_t) {
            total.fetch_add(1, std::memory_order_relaxed);
        });
    });
    CHECK(total.load() == 8000);
}

void test_exception_propagation() {
    JobSystem system(3);
    bool caught = false;
    try {
        system.parallel_for(10000, 32, [](std::size_t index) {
            if (index == 5000) throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);

    // The pool must stay usable and must not keep rethrowing the stale error.
    std::atomic<int> executed{0};
    system.parallel_for(1000, 64, [&executed](std::size_t) {
        executed.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(executed.load() == 1000);
    CHECK(system.take_exception() == nullptr);
}

void test_exception_from_submit_is_captured() {
    JobSystem system(2);
    system.submit([] { throw std::runtime_error("submit boom"); });
    system.wait_idle();
    const std::exception_ptr error = system.take_exception();
    CHECK(error != nullptr);
    CHECK(system.take_exception() == nullptr);
}

void test_parallel_for_slow_body_coverage() {
    // A body that is definitely slower than the inline escalation budget: this
    // exercises the fan-out path (start inline, escalate, join) rather than the
    // finished-inline fast path, and still demands exact coverage.
    JobSystem system(3);
    constexpr std::size_t kCount = 256;
    std::vector<std::atomic<int>> hits(kCount);
    for (auto& slot : hits) slot.store(0, std::memory_order_relaxed);
    system.parallel_for(kCount, 4, [&hits](std::size_t index) {
        for (int spin = 0; spin < 200; ++spin) std::this_thread::yield();
        hits[index].fetch_add(1, std::memory_order_relaxed);
    });
    check_exactly_once(hits, "slow body");
}

void test_exception_during_inline_participation() {
    // Regression: a throw raised by the *calling* thread used to unwind out of
    // parallel_for before waiting for the helper tasks, which then wrote through
    // the dead stack frame (corrupting the next call's counter and callables).
    JobSystem system(3);
    int caught = 0;
    for (int round = 0; round < 12; ++round) {
        try {
            system.parallel_for(64, 1, [](std::size_t) { throw std::runtime_error("inline boom"); });
        } catch (const std::runtime_error&) {
            ++caught;
        }
        std::atomic<int> executed{0};
        system.parallel_for(2000, 64, [&executed](std::size_t) {
            executed.fetch_add(1, std::memory_order_relaxed);
        });
        CHECK(executed.load() == 2000);
    }
    CHECK(caught == 12);
}

void test_cancel() {
    JobSystem system(2);
    std::atomic<bool> ran{false};
    Counter counter;
    system.submit([&ran] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        ran.store(true);
    }, &counter);
    counter.cancel();
    system.wait(counter);
    // The body may already have started; what matters is that the counter always
    // completes, whichever side of the fence the job was on.
    CHECK(counter.done());

    // A cancelled counter must skip bodies that never started.
    Counter second;
    std::atomic<bool> secondRan{false};
    second.cancel();
    system.submit([&secondRan] { secondRan.store(true); }, &second);
    system.wait(second);
    CHECK(second.done());
    CHECK(!secondRan.load());
}

void test_submit_batch() {
    JobSystem system(3);
    std::atomic<int> executed{0};
    Counter counter;
    constexpr std::size_t kBatch = 100;
    JobSystem::Function bodies[kBatch];
    for (auto& body : bodies) {
        body = [&executed] { executed.fetch_add(1, std::memory_order_relaxed); };
    }
    for (int round = 0; round < 10; ++round) system.submit_batch(bodies, kBatch, &counter);
    system.wait(counter);
    CHECK(executed.load() == 1000);
    CHECK(counter.done());

    // Empty batches must be a no-op, including a null range.
    system.submit_batch(nullptr, 0, &counter);
    system.submit_batch(bodies, 0, &counter);
    CHECK(counter.remaining() == 0);
}

void test_submit_batch_repeated() {
    // Same callable queued `count` times: this is what parallel_for's helper
    // fan-out uses, so it must count exactly once per copy.
    JobSystem system(3);
    std::atomic<int> executed{0};
    Counter counter;
    const JobSystem::Function body = [&executed] {
        executed.fetch_add(1, std::memory_order_relaxed);
    };
    system.submit_batch(body, 1500, &counter);
    system.wait(counter);
    CHECK(executed.load() == 1500);
    CHECK(counter.done());

    // Empty repeat is a no-op.
    system.submit_batch(body, 0, &counter);
    CHECK(counter.remaining() == 0);
}

void test_submit_batch_larger_than_a_deque() {
    // A single call bigger than one worker's ring must spill into the overflow
    // queue and still complete exactly once per job.
    JobSystem system(2);
    constexpr std::size_t kCount = 5000;
    std::atomic<int> executed{0};
    Counter counter;
    std::vector<JobSystem::Function> bodies(kCount, JobSystem::Function([&executed] {
        executed.fetch_add(1, std::memory_order_relaxed);
    }));
    system.submit_batch(bodies.data(), bodies.size(), &counter);
    system.wait(counter);
    CHECK(executed.load() == static_cast<int>(kCount));
    CHECK(counter.done());
}

void test_submit_batch_from_a_job() {
    JobSystem system(2);
    std::atomic<int> inner{0};
    Counter outer;
    system.submit([&system, &inner, &outer] {
        Counter local;
        std::vector<JobSystem::Function> bodies(64, JobSystem::Function([&inner] {
            inner.fetch_add(1, std::memory_order_relaxed);
        }));
        system.submit_batch(bodies.data(), bodies.size(), &local);
        system.wait(local);
    }, &outer);
    system.wait(outer);
    CHECK(inner.load() == 64);
}

void test_submit_batch_exception_is_captured() {
    JobSystem system(2);
    std::vector<JobSystem::Function> bodies;
    bodies.emplace_back([] { throw std::runtime_error("batch boom"); });
    bodies.emplace_back([] {});
    Counter counter;
    system.submit_batch(bodies.data(), bodies.size(), &counter);
    system.wait(counter);
    CHECK(system.take_exception() != nullptr);
    CHECK(system.take_exception() == nullptr);
}

void test_parallel_for_completes_before_return() {
    // Regression: an early return here means a task was consumed twice (a counter
    // that completes while work is still queued), which used to corrupt the next
    // parallel_for through the reused stack counter.
    JobSystem system(3);
    for (int round = 0; round < 25; ++round) {
        std::atomic<int> executed{0};
        system.parallel_for(20000, 32, [&executed](std::size_t) {
            executed.fetch_add(1, std::memory_order_relaxed);
        });
        const int immediately = executed.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(immediately == 20000);
        CHECK(executed.load() == immediately);
    }
}

void test_single_worker_is_deterministic() {
    JobSystem system(1);
    constexpr std::size_t kCount = 20000;
    auto run = [&system](std::vector<int>& out) {
        out.assign(kCount, -1);
        system.parallel_for(kCount, 1, [&out](std::size_t index) {
            out[index] = static_cast<int>(index) * 3 + 1;
        });
    };
    std::vector<int> first;
    std::vector<int> second;
    run(first);
    run(second);
    CHECK(first == second);
    bool complete = true;
    for (std::size_t i = 0; i < kCount; ++i) {
        if (first[i] != static_cast<int>(i) * 3 + 1) complete = false;
    }
    CHECK(complete);
}

void test_multiple_producers() {
    JobSystem system(4);
    Counter counter;
    std::atomic<int> executed{0};
    constexpr int kProducers = 8;
    constexpr int kPerProducer = 5000;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&system, &counter, &executed] {
            for (int i = 0; i < kPerProducer; ++i) {
                system.submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); },
                              &counter);
            }
        });
    }
    for (auto& producer : producers) producer.join();
    system.wait(counter);
    CHECK(executed.load() == kProducers * kPerProducer);
}

void test_task_storage_is_released() {
    JobSystem system(2);
    const std::shared_ptr<int> token = std::make_shared<int>(7);
    CHECK(token.use_count() == 1);
    {
        std::atomic<int> executed{0};
        Counter counter;
        for (int i = 0; i < 4096; ++i) {
            system.submit([token, &executed] { executed.fetch_add(1, std::memory_order_relaxed); },
                          &counter);
        }
        system.wait(counter);
        CHECK(executed.load() == 4096);
    }
    // Every captured copy must be gone: recycled jobs keep their std::function
    // cleared, so nothing pins the payload alive.
    CHECK(token.use_count() == 1);

    system.parallel_for(4096, 16, [token](std::size_t) { (void)token; });
    CHECK(token.use_count() == 1);
}

void test_system_restart_reuses_no_state() {
    // Regression: per-thread task magazines were keyed by the JobSystem address,
    // and the allocator happily reuses that block for the next system. The new
    // system then inherited dangling task pointers from the destroyed pool.
    std::atomic<int> executed{0};
    constexpr int kRounds = 40;
    constexpr int kJobs = 128;
    for (int round = 0; round < kRounds; ++round) {
        JobSystem system(2);
        Counter counter;
        for (int i = 0; i < kJobs; ++i) {
            system.submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); },
                          &counter);
        }
        system.wait(counter);
    }
    CHECK(executed.load() == kRounds * kJobs);
}

void test_destructor_drains_pending_work() {
    std::atomic<int> executed{0};
    {
        JobSystem system(4);
        Counter counter;
        for (int i = 0; i < 500; ++i) {
            system.submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); },
                          &counter);
        }
        // No wait: the destructor is contractually required to run the backlog and
        // leave no counter stranded.
    }
    CHECK(executed.load() == 500);
}

void test_run_pending_and_idle() {
    JobSystem system(1);
    std::atomic<int> executed{0};
    Counter counter;
    for (int i = 0; i < 64; ++i) {
        system.submit([&executed] { executed.fetch_add(1, std::memory_order_relaxed); }, &counter);
    }
    const std::size_t first = system.run_pending(8);
    CHECK(first <= 8);
    system.wait_idle();
    CHECK(executed.load() == 64);
    system.wait(counter);
    CHECK(counter.done());
}

void test_wait_is_reentrant_from_a_job() {
    JobSystem system(2);
    std::atomic<int> depth{0};
    std::atomic<int> maxDepth{0};
    std::atomic<int> total{0};
    Counter outer;
    for (int i = 0; i < 4; ++i) {
        system.submit([&] {
            const int current = depth.fetch_add(1, std::memory_order_relaxed) + 1;
            int previous = maxDepth.load(std::memory_order_relaxed);
            while (current > previous &&
                   !maxDepth.compare_exchange_weak(previous, current, std::memory_order_relaxed)) {
            }
            Counter inner;
            for (int j = 0; j < 8; ++j) {
                system.submit([&total] { total.fetch_add(1, std::memory_order_relaxed); }, &inner);
            }
            system.wait(inner);
            depth.fetch_sub(1, std::memory_order_relaxed);
        }, &outer);
    }
    system.wait(outer);
    CHECK(total.load() == 32);
    CHECK(maxDepth.load() >= 1);
}

void test_throughput_informational() {
    JobSystem system(4);
    constexpr int kJobs = 200000;
    Counter counter;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kJobs; ++i) system.submit([] {}, &counter);
    system.wait(counter);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double nanoseconds =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    std::printf("  info: empty job round-trip %.1f ns/job over %d jobs (%zu workers)\n",
                nanoseconds / kJobs, kJobs, system.worker_count());
}

struct TestCase {
    const char* name;
    void (*run)();
};

// Optional long-running mode (`--stress [rounds]`). Mixed adversarial traffic:
// several producers, nested fan-out, a throwing round every third iteration and
// a cancelled batch, all verified against exact expected counts. It stays in this
// binary on purpose: the job system must be provable without the engine.
bool run_stress_round(int round) {
    const bool throwThisRound = (round % 3) == 2;
    JobSystem system(0); // hardware concurrency
    const std::size_t workers = system.worker_count();

    // Phase 1: coverage under dynamic partitioning.
    constexpr std::size_t kCoverage = 200000;
    std::vector<std::atomic<int>> hits(kCoverage);
    for (auto& slot : hits) slot.store(0, std::memory_order_relaxed);
    if (throwThisRound) {
        try {
            system.parallel_for(kCoverage, 32, [&hits](std::size_t index) {
                hits[index].fetch_add(1, std::memory_order_relaxed);
                if (index == 91000) throw std::runtime_error("stress");
            });
            report(false, "stress: parallel_for should have thrown", __FILE__, __LINE__);
            return false;
        } catch (const std::runtime_error&) {
        }
        for (auto& slot : hits) slot.store(0, std::memory_order_relaxed);
    }
    system.parallel_for(kCoverage, 32, [&hits](std::size_t index) {
        hits[index].fetch_add(1, std::memory_order_relaxed);
    });
    for (std::size_t i = 0; i < kCoverage; ++i) {
        if (hits[i].load(std::memory_order_relaxed) != 1) {
            report(false, "stress: index not executed exactly once", __FILE__, __LINE__);
            return false;
        }
    }

    // Phase 2: several producers sharing one counter.
    Counter counter;
    std::atomic<long long> submitted{0};
    const int producers = 4;
    const int perProducer = 20000;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(producers));
    for (int p = 0; p < producers; ++p) {
        threads.emplace_back([&system, &counter, &submitted] {
            for (int i = 0; i < perProducer; ++i) {
                system.submit([&submitted] {
                    submitted.fetch_add(1, std::memory_order_relaxed);
                }, &counter);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    system.wait(counter);
    if (submitted.load() != static_cast<long long>(producers) * perProducer) {
        report(false, "stress: producer count mismatch", __FILE__, __LINE__);
        return false;
    }
    if (!counter.done()) {
        report(false, "stress: counter did not complete", __FILE__, __LINE__);
        return false;
    }

    // Phase 3: nested fan-out from every chunk.
    std::atomic<long long> nested{0};
    system.parallel_for(static_cast<std::size_t>(producers) * 8, 1, [&system, &nested](std::size_t) {
        system.parallel_for(500, 50, [&nested](std::size_t) {
            nested.fetch_add(1, std::memory_order_relaxed);
        });
    });
    if (nested.load() != static_cast<long long>(producers) * 8 * 500) {
        report(false, "stress: nested fan-out incomplete", __FILE__, __LINE__);
        return false;
    }

    // Phase 4: a cancelled batch must still complete its counter.
    Counter cancelled;
    std::atomic<int> ranAfterCancel{0};
    cancelled.cancel();
    for (int i = 0; i < 500; ++i) {
        system.submit([&ranAfterCancel] { ranAfterCancel.fetch_add(1, std::memory_order_relaxed); },
                      &cancelled);
    }
    system.wait(cancelled);
    if (!cancelled.done() || ranAfterCancel.load() != 0) {
        report(false, "stress: cancellation contract broken", __FILE__, __LINE__);
        return false;
    }

    std::printf("  round %2d: %zu workers, %zu coverage, %d producer jobs, %lld nested, ok\n",
                round, workers, kCoverage, producers * perProducer, nested.load());
    return true;
}

void run_stress(int rounds) {
    const auto start = std::chrono::steady_clock::now();
    int failed = 0;
    for (int round = 0; round < rounds; ++round) {
        if (!run_stress_round(round)) ++failed;
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::printf("\nstress: %d rounds, %d failed, %.2f s\n", rounds, failed, seconds);
    g_failures += failed;
}

} // namespace

int main(int argc, char** argv) {
    const TestCase tests[] = {
        {"worker_count", test_worker_count},
        {"submit_and_wait", test_submit_and_wait},
        {"counter_manual_completion", test_counter_manual_completion},
        {"nested_submit", test_nested_submit},
        {"parallel_for", test_parallel_for},
        {"parallel_for_uneven_work", test_parallel_for_uneven_work},
        {"parallel_for_serial_fallback", test_parallel_for_serial_fallback},
        {"parallel_for_nested", test_parallel_for_nested},
        {"exception_propagation", test_exception_propagation},
        {"exception_from_submit_is_captured", test_exception_from_submit_is_captured},
        {"submit_batch", test_submit_batch},
        {"submit_batch_repeated", test_submit_batch_repeated},
        {"submit_batch_larger_than_a_deque", test_submit_batch_larger_than_a_deque},
        {"submit_batch_from_a_job", test_submit_batch_from_a_job},
        {"submit_batch_exception_is_captured", test_submit_batch_exception_is_captured},
        {"parallel_for_slow_body_coverage", test_parallel_for_slow_body_coverage},
        {"parallel_for_completes_before_return", test_parallel_for_completes_before_return},
        {"exception_during_inline_participation", test_exception_during_inline_participation},
        {"cancel", test_cancel},
        {"single_worker_is_deterministic", test_single_worker_is_deterministic},
        {"multiple_producers", test_multiple_producers},
        {"task_storage_is_released", test_task_storage_is_released},
        {"system_restart_reuses_no_state", test_system_restart_reuses_no_state},
        {"destructor_drains_pending_work", test_destructor_drains_pending_work},
        {"run_pending_and_idle", test_run_pending_and_idle},
        {"wait_is_reentrant_from_a_job", test_wait_is_reentrant_from_a_job},
        {"throughput_informational", test_throughput_informational},
    };

    for (const TestCase& test : tests) {
        std::printf("[ RUN      ] %s\n", test.name);
        const int before = g_failures;
        test.run();
        std::printf("[ %s ] %s\n", g_failures == before ? "    OK  " : "  FAILED", test.name);
        std::fflush(stdout);
    }

    // --stress [rounds]: optional long run, same binary, still engine-free.
    int stressRounds = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stress") != 0) continue;
        stressRounds = 20;
        if (i + 1 < argc) {
            const int parsed = std::atoi(argv[i + 1]);
            if (parsed > 0) stressRounds = parsed;
        }
    }
    if (stressRounds > 0) {
        std::printf("\n[ RUN      ] stress (%d rounds)\n", stressRounds);
        std::fflush(stdout);
        run_stress(stressRounds);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
