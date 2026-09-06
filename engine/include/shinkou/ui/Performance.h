#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace shinkou::ui {
enum class UiStage : std::size_t { Input, Model, Index, Layout, Paint, Raster, Upload, Composite, Present, FullBytes, DirtyBytes, CacheHit, Count };
enum class UiWorkload : std::size_t { Idle, Hover, Input, Filter, Scroll, Resize, Count };
struct UiPercentiles { std::size_t samples{0}; double p50{0}, p95{0}, maximum{0}; };
class UiPerformance {
    static constexpr std::size_t capacity = 2048;
    struct Series { std::array<double,capacity> values{}; std::size_t count{0}, next{0}; };
    std::array<std::array<Series,static_cast<std::size_t>(UiStage::Count)>,static_cast<std::size_t>(UiWorkload::Count)> history_{};
    std::array<double,static_cast<std::size_t>(UiStage::Count)> frame_{};
    UiWorkload workload_{UiWorkload::Idle};
public:
    void begin_frame() noexcept { frame_.fill(0); workload_ = UiWorkload::Idle; }
    void workload(UiWorkload workload) noexcept { if (workload > workload_) workload_ = workload; }
    void add(UiStage stage, double value) noexcept { frame_[static_cast<std::size_t>(stage)] += value; }
    void end_frame() noexcept;
    UiPercentiles summary(UiWorkload workload, UiStage stage) const;
    bool write_csv(const std::string& path) const;
};
UiPerformance& ui_performance() noexcept;
class UiTimer {
    UiStage stage_;
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
public:
    explicit UiTimer(UiStage stage) : stage_(stage) {}
    ~UiTimer() { ui_performance().add(stage_, std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start_).count()); }
};
} // namespace shinkou::ui
