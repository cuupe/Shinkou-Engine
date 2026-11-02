#pragma once

// The engine-facing API remains C++17. Include Coroutine.h from C++20 code
// to author tasks; all task bodies and their continuations run on this owner.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace shinkou::coroutines {
template <class T = void> class Task;
class Scheduler;
namespace detail {
struct SchedulerImpl;
struct Control;
struct PromiseBase;
struct Access;
struct WaitState;
} // namespace detail

enum class Phase : std::uint8_t { Update, LateUpdate, EndOfFrame };
enum class Clock : std::uint8_t { Scaled, Unscaled };
enum class Status : std::uint8_t { Pending, Completed, Cancelled, Failed };

class Cancelled final : public std::runtime_error {
  public:
    Cancelled() : std::runtime_error("coroutine cancelled") {
    }
};

// Copies observe the same completion. cancel() is safe from any thread and is
// processed by the owner at its next run(). Destroying a handle does not cancel.
class TaskHandle final {
  public:
    TaskHandle() = default;
    bool valid() const noexcept {
        return static_cast<bool>(control_);
    }
    bool done() const noexcept;
    Status status() const noexcept;
    void cancel() const;
    void rethrow_if_failed() const;

  private:
    explicit TaskHandle(std::shared_ptr<detail::Control> control) : control_(std::move(control)) {
    }
    std::shared_ptr<detail::Control> control_;
    friend class Scheduler;
    friend struct detail::Access;
};

struct SchedulerConfig {
    std::size_t initialCapacity{256};
    std::size_t maxResumesPerPhase{4096};
};

struct SchedulerStats {
    std::size_t active{0};
    std::size_t peakActive{0};
    std::size_t ready{0};
    std::size_t timers{0};
    std::uint64_t spawned{0};
    std::uint64_t completed{0};
    std::uint64_t cancelled{0};
    std::uint64_t failed{0};
    std::uint64_t resumes{0};
    std::uint64_t budgetExhaustions{0};
};

class Scheduler final {
  public:
    explicit Scheduler(const SchedulerConfig &config = {});
    ~Scheduler();
    Scheduler(const Scheduler &) = delete;
    Scheduler &operator=(const Scheduler &) = delete;

    // Lazy tasks start on the next run(Update), never inline in spawn().
    TaskHandle spawn(Task<void> task);
    // Advance explicit clocks once per frame. Deltas must be finite and >= 0.
    void begin_frame(double scaledDelta, double unscaledDelta);
    std::size_t run(Phase phase, std::size_t budget = 0);
    void tick(double delta);
    void tick(double scaledDelta, double unscaledDelta);
    // Destroys all suspended frames on the owner before dependent systems die.
    // Background callables already submitted to JobSystem finish independently.
    void cancel_all();
    void shutdown();
    bool closed() const;
    std::uint64_t frame() const;
    double time(Clock clock = Clock::Scaled) const;
    SchedulerStats stats() const;
    // Unobserved root failures remain visible even if the handle was discarded.
    std::exception_ptr take_exception();

  private:
    std::unique_ptr<detail::SchedulerImpl> impl_;
    friend struct detail::Access;
};

// Scope-owned roots. Destruction requests cancellation without blocking a frame.
// Pump until the handles complete before releasing data referenced by frames.
// Submitted background callables need their own independent data lifetime.
// The scheduler must outlive calls to spawn().
class TaskGroup final {
  public:
    explicit TaskGroup(Scheduler &scheduler);
    ~TaskGroup();
    TaskGroup(const TaskGroup &) = delete;
    TaskGroup &operator=(const TaskGroup &) = delete;
    TaskHandle spawn(Task<void> task);
    void cancel();
    bool done() const;
    const std::vector<TaskHandle> &handles() const noexcept {
        return handles_;
    }

  private:
    Scheduler *scheduler_;
    std::vector<TaskHandle> handles_;
};
} // namespace shinkou::coroutines
