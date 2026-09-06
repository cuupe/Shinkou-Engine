#pragma once

#include "shinkou/Math.h"
#include "shinkou/MathSimd.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace shinkou::math {

struct ParallelConfig {
    // Zero selects a conservative hardware-based worker count.
    std::size_t workerCount{0};
    // Work smaller than this threshold stays on the caller thread.
    std::size_t grainSize{256};
    bool enabled{true};
};

// Thread-safe for concurrent and nested for_each calls. The executor object
// must outlive every call using it; shutdown is explicit and idempotent.
class ParallelExecutor {
    using TaskFunction = void (*)(void*, std::size_t, std::size_t);

    struct Job {
        TaskFunction function{nullptr};
        void* context{nullptr};
        std::size_t count{0};
        std::size_t grain{1};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> remainingWorkers{0};
        std::atomic<bool> failed{false};
        std::exception_ptr exception{};

        Job(TaskFunction function_, void* context_, std::size_t count_, std::size_t grain_, std::size_t workerCount)
            : function(function_), context(context_), count(count_), grain(grain_), remainingWorkers(workerCount) {}
    };

    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable workComplete_;
    std::vector<Job*> pendingJobs_;
    std::size_t defaultGrain_{256};
    bool stopping_{false};

    void worker_loop(std::size_t workerIndex);
    void execute_job(Job& job) noexcept;
    void run_erased(std::size_t count, std::size_t grain, TaskFunction function, void* context);

public:
    explicit ParallelExecutor(ParallelConfig config = {});
    ~ParallelExecutor();
    ParallelExecutor(const ParallelExecutor&) = delete;
    ParallelExecutor& operator=(const ParallelExecutor&) = delete;

    std::size_t worker_count() const noexcept { return workers_.size(); }
    bool enabled() const noexcept { return !workers_.empty(); }
    bool is_shutdown() const noexcept;
    // Must be called only after submitted work has completed.
    void shutdown() noexcept;

    template<class Function>
    void for_each(std::size_t count, Function&& function, std::size_t grainSize = 0) {
        using Callable = std::remove_reference_t<Function>;
        Callable* callable = &function;
        const auto invoke = [](void* context, std::size_t begin, std::size_t end) {
            auto& fn = *static_cast<Callable*>(context);
            for (std::size_t index = begin; index < end; ++index) fn(index);
        };
        run_erased(count, grainSize, invoke, callable);
    }
};

template<class Function>
inline void ParallelFor(ParallelExecutor& executor, std::size_t count,
                        Function&& function, std::size_t grainSize = 0) {
    executor.for_each(count, std::forward<Function>(function), grainSize);
}

inline void ParallelTransformPoints(ParallelExecutor& executor, const Mat4& matrix,
                                     ConstArrayView<Vec3> input, ArrayView<Vec3> output,
                                     std::size_t grainSize = 0) {
    if (input.size() != output.size()) return;
    const std::size_t blockCount = input.size() / 4 + (input.size() % 4 != 0 ? 1 : 0);
    const std::size_t blockGrain = grainSize == 0 ? 0 : std::max<std::size_t>(1, grainSize / 4);
    executor.for_each(blockCount, [&](std::size_t block) {
        const std::size_t begin = block * 4;
        const std::size_t count = std::min<std::size_t>(4, input.size() - begin);
        TransformPointsSimd(matrix, {input.data() + begin, count}, {output.data() + begin, count});
    }, blockGrain);
}

inline void ParallelTransformPointsScalar(ParallelExecutor& executor, const Mat4& matrix,
                                          ConstArrayView<Vec3> input, ArrayView<Vec3> output,
                                          std::size_t grainSize = 0) {
    if (input.size() != output.size()) return;
    executor.for_each(input.size(), [&](std::size_t index) { output[index] = TransformPoint(matrix, input[index]); }, grainSize);
}

inline void ParallelTransformVectors(ParallelExecutor& executor, const Mat4& matrix,
                                     ConstArrayView<Vec3> input, ArrayView<Vec3> output,
                                     std::size_t grainSize = 0) {
    if (input.size() != output.size()) return;
    const std::size_t blockCount = input.size() / 4 + (input.size() % 4 != 0 ? 1 : 0);
    const std::size_t blockGrain = grainSize == 0 ? 0 : std::max<std::size_t>(1, grainSize / 4);
    executor.for_each(blockCount, [&](std::size_t block) {
        const std::size_t begin = block * 4;
        const std::size_t count = std::min<std::size_t>(4, input.size() - begin);
        TransformVectorsSimd(matrix, {input.data() + begin, count}, {output.data() + begin, count});
    }, blockGrain);
}

inline void ParallelTransformPointsSoa(ParallelExecutor& executor, const Mat4& matrix,
                                      Vec3SoaConstView input, Vec3SoaView output,
                                      std::size_t grainSize = 0) {
    if (!input.valid() || !output.valid() || input.count != output.count) return;
    const std::size_t blockCount = input.count / 8 + (input.count % 8 != 0 ? 1 : 0);
    const std::size_t blockGrain = grainSize == 0 ? 0 : std::max<std::size_t>(1, grainSize / 8);
    executor.for_each(blockCount, [&](std::size_t block) {
        const std::size_t begin = block * 8;
        const std::size_t count = std::min<std::size_t>(8, input.count - begin);
        TransformPointsSoaSimd(matrix,
            {input.x + begin, input.y + begin, input.z + begin, count},
            {output.x + begin, output.y + begin, output.z + begin, count});
    }, blockGrain);
}

inline void ParallelLerp(ParallelExecutor& executor, ConstArrayView<Vec3> a,
                         ConstArrayView<Vec3> b, ArrayView<Vec3> output, float weight,
                         std::size_t grainSize = 0) {
    if (a.size() != b.size() || a.size() != output.size()) return;
    executor.for_each(a.size(), [&](std::size_t index) { output[index] = Lerp(a[index], b[index], weight); }, grainSize);
}

inline void ParallelLerpSimd(ParallelExecutor& executor, ConstArrayView<Vec3> a,
                             ConstArrayView<Vec3> b, ArrayView<Vec3> output, float weight,
                             std::size_t grainSize = 0) {
    if (a.size() != b.size() || a.size() != output.size()) return;
    const std::size_t blockCount = a.size() / 4 + (a.size() % 4 != 0 ? 1 : 0);
    const std::size_t blockGrain = grainSize == 0 ? 0 : std::max<std::size_t>(1, grainSize / 4);
    executor.for_each(blockCount, [&](std::size_t block) {
        const std::size_t begin = block * 4;
        const std::size_t count = std::min<std::size_t>(4, a.size() - begin);
        LerpSimd({a.data() + begin, count}, {b.data() + begin, count}, {output.data() + begin, count}, weight);
    }, blockGrain);
}

inline void ParallelLerpSoa(ParallelExecutor& executor, Vec3SoaConstView a,
                            Vec3SoaConstView b, Vec3SoaView output, float weight,
                            std::size_t grainSize = 0) {
    if (!a.valid() || !b.valid() || !output.valid() || a.count != b.count || a.count != output.count) return;
    const std::size_t blockCount = a.count / 8 + (a.count % 8 != 0 ? 1 : 0);
    const std::size_t blockGrain = grainSize == 0 ? 0 : std::max<std::size_t>(1, grainSize / 8);
    executor.for_each(blockCount, [&](std::size_t block) {
        const std::size_t begin = block * 8;
        const std::size_t count = std::min<std::size_t>(8, a.count - begin);
        LerpSoaSimd({a.x + begin, a.y + begin, a.z + begin, count},
                    {b.x + begin, b.y + begin, b.z + begin, count},
                    {output.x + begin, output.y + begin, output.z + begin, count}, weight);
    }, blockGrain);
}

}
