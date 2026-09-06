#include "shinkou/ui/Performance.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>

namespace shinkou::ui {
UiPerformance& ui_performance() noexcept { static UiPerformance instance; return instance; }
void UiPerformance::end_frame() noexcept {
    auto& stages = history_[static_cast<std::size_t>(workload_)];
    for (std::size_t i=0; i<stages.size(); ++i) {
        auto& s = stages[i]; s.values[s.next] = frame_[i]; s.next = (s.next+1)%capacity; s.count = std::min(capacity,s.count+1);
    }
}
UiPercentiles UiPerformance::summary(UiWorkload workload, UiStage stage) const {
    const auto& s = history_[static_cast<std::size_t>(workload)][static_cast<std::size_t>(stage)];
    if (!s.count) return {};
    std::vector<double> values(s.values.begin(),s.values.begin()+s.count);
    std::sort(values.begin(),values.end());
    const auto percentile = [&](double p) { return values[static_cast<std::size_t>(std::ceil(p*values.size()))-1]; };
    return {s.count,percentile(0.5),percentile(0.95),values.back()};
}
bool UiPerformance::write_csv(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    const char* workloads[]{"idle","hover","input","filter","scroll","resize"};
    const char* stages[]{"input","model","index","layout","paint","d2d_raster","upload","composite_cpu","present_block","full_upload_bytes","dirty_upload_bytes","surface_cache_hit"};
    out << "workload,stage,unit,samples,p50,p95,max\n" << std::setprecision(9);
    for (std::size_t w=0;w<static_cast<std::size_t>(UiWorkload::Count);++w)
        for (std::size_t s=0;s<static_cast<std::size_t>(UiStage::Count);++s) {
            const auto p = summary(static_cast<UiWorkload>(w),static_cast<UiStage>(s));
            out << workloads[w] << ',' << stages[s] << ',' << (s<9 ? "ms" : s<11 ? "bytes" : "ratio") << ',' << p.samples << ',' << p.p50 << ',' << p.p95 << ',' << p.maximum << '\n';
        }
    return static_cast<bool>(out);
}
} // namespace shinkou::ui
