#include "shinkou/render/AdvancedRenderFeatures.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

namespace shinkou::render {
namespace {
bool supported(RenderFeature feature, const RenderCapabilities& caps) {
    switch (feature) {
    case RenderFeature::DepthPrepass:
    case RenderFeature::CascadedShadows:
    case RenderFeature::ScreenSpaceAmbientOcclusion:
    case RenderFeature::TemporalAntiAliasing:
    case RenderFeature::Bloom:
    case RenderFeature::Exposure:
    case RenderFeature::ScreenSpaceReflections:
    case RenderFeature::OcclusionCulling:
    case RenderFeature::LodSelection:
        return caps.deviceReady;
    case RenderFeature::MeshletRasterization:
        return caps.supportsMeshShaders;
    case RenderFeature::RayTracing:
        return caps.supportsRayTracing;
    case RenderFeature::VariableRateShading:
        return caps.supportsVariableRateShading;
    }
    return false;
}

const RenderFeature* dependencies(RenderFeature feature, std::size_t& count) {
    static const RenderFeature depth[] = {RenderFeature::DepthPrepass};
    static const RenderFeature taa[] = {RenderFeature::DepthPrepass};
    static const RenderFeature ssr[] = {RenderFeature::DepthPrepass, RenderFeature::ScreenSpaceAmbientOcclusion};
    switch (feature) {
    case RenderFeature::ScreenSpaceAmbientOcclusion:
    case RenderFeature::OcclusionCulling:
    case RenderFeature::CascadedShadows:
        count = 1; return depth;
    case RenderFeature::TemporalAntiAliasing:
        count = 1; return taa;
    case RenderFeature::ScreenSpaceReflections:
        count = 2; return ssr;
    default:
        count = 0; return nullptr;
    }
}
}

const char* to_string(RenderFeature feature) noexcept {
    switch (feature) {
    case RenderFeature::DepthPrepass: return "depth_prepass";
    case RenderFeature::CascadedShadows: return "cascaded_shadows";
    case RenderFeature::ScreenSpaceAmbientOcclusion: return "ssao";
    case RenderFeature::TemporalAntiAliasing: return "taa";
    case RenderFeature::Bloom: return "bloom";
    case RenderFeature::Exposure: return "exposure";
    case RenderFeature::ScreenSpaceReflections: return "ssr";
    case RenderFeature::OcclusionCulling: return "occlusion_culling";
    case RenderFeature::LodSelection: return "lod_selection";
    case RenderFeature::MeshletRasterization: return "meshlet_rasterization";
    case RenderFeature::RayTracing: return "ray_tracing";
    case RenderFeature::VariableRateShading: return "vrs";
    }
    return "unknown";
}

const RenderFeatureDecision* RenderFeaturePlan::decision(RenderFeature feature) const noexcept {
    const auto it = std::find_if(decisions.begin(), decisions.end(),
        [feature](const auto& value) { return value.feature == feature; });
    return it == decisions.end() ? nullptr : &*it;
}

RenderFeaturePlan build_render_feature_plan(const RenderCapabilities& capabilities,
                                            const std::vector<RenderFeatureRequest>& requests) {
    RenderFeaturePlan plan;
    constexpr std::size_t FeatureCount = static_cast<std::size_t>(RenderFeature::VariableRateShading) + 1u;
    std::vector<std::uint8_t> visitState(FeatureCount, 0u);

    for (const auto& request : requests) {
        if (plan.decision(request.feature)) {
            plan.error = "duplicate render feature request";
            return plan;
        }
        const bool isSupported = supported(request.feature, capabilities);
        RenderFeatureDecision decision{request.feature, request.enabled && isSupported,
                                       isSupported, request.enabled && !isSupported && !request.required,
                                       isSupported ? "supported" : "backend capability is unavailable"};
        if (request.enabled && request.required && !isSupported) {
            plan.error = std::string("required render feature is unsupported: ") + to_string(request.feature);
            return plan;
        }
        plan.decisions.push_back(std::move(decision));
    }

    const auto index_of = [](RenderFeature feature) {
        return static_cast<std::size_t>(feature);
    };
    const auto request_for = [&requests](RenderFeature feature) -> const RenderFeatureRequest* {
        const auto found = std::find_if(requests.begin(), requests.end(),
            [feature](const RenderFeatureRequest& request) { return request.feature == feature; });
        return found == requests.end() ? nullptr : &*found;
    };
    const auto add_auto_dependency = [&plan, &capabilities](RenderFeature feature) {
        const bool isSupported = supported(feature, capabilities);
        plan.decisions.push_back({feature, isSupported, isSupported, !isSupported,
            isSupported ? "auto-enabled dependency" : "dependency backend capability is unavailable"});
    };

    std::function<bool(RenderFeature, bool)> resolve = [&](RenderFeature feature, bool requiredByParent) {
        const auto featureIndex = index_of(feature);
        if (featureIndex >= visitState.size()) {
            plan.error = "render feature dependency references an invalid feature";
            return false;
        }
        if (visitState[featureIndex] == 1u) {
            plan.error = std::string("render feature dependency cycle detected at: ") + to_string(feature);
            return false;
        }

        auto find_decision = [&plan](RenderFeature candidate) -> RenderFeatureDecision* {
            const auto found = std::find_if(plan.decisions.begin(), plan.decisions.end(),
                [candidate](const RenderFeatureDecision& value) { return value.feature == candidate; });
            return found == plan.decisions.end() ? nullptr : &*found;
        };
        auto* decision = find_decision(feature);
        if (!decision) {
            add_auto_dependency(feature);
            decision = find_decision(feature);
        }
        if (!decision || !decision->enabled) {
            if (requiredByParent) {
                plan.error = std::string("render feature dependency is unavailable: ") + to_string(feature);
                return false;
            }
            return false;
        }

        visitState[featureIndex] = 1u;
        std::size_t dependencyCount = 0;
        const auto* required = dependencies(feature, dependencyCount);
        for (std::size_t i = 0; i < dependencyCount; ++i) {
            const auto dependency = required[i];
            const auto* explicitRequest = request_for(dependency);
            const bool dependencyRequired = requiredByParent ||
                (explicitRequest && explicitRequest->required);
            if (!resolve(dependency, dependencyRequired)) {
                decision = find_decision(feature);
                if (decision) {
                    decision->enabled = false;
                    decision->fallback = !requiredByParent;
                    decision->reason = std::string("dependency unavailable: ") + to_string(dependency);
                }
                visitState[featureIndex] = 2u;
                if (requiredByParent) {
                    plan.error = std::string("required render feature dependency is unavailable: ") + to_string(dependency);
                }
                return false;
            }
        }
        visitState[featureIndex] = 2u;
        return true;
    };

    for (const auto& request : requests) {
        if (!request.enabled) continue;
        if (!resolve(request.feature, request.required) && !plan.error.empty()) return plan;
    }

    std::fill(visitState.begin(), visitState.end(), 0u);
    std::function<void(RenderFeature)> append_order = [&](RenderFeature feature) {
        const auto featureIndex = index_of(feature);
        if (featureIndex >= visitState.size() || visitState[featureIndex] == 2u) return;
        if (visitState[featureIndex] == 1u) return;
        const auto* decision = plan.decision(feature);
        if (!decision || !decision->enabled) return;
        visitState[featureIndex] = 1u;
        std::size_t dependencyCount = 0;
        const auto* required = dependencies(feature, dependencyCount);
        for (std::size_t i = 0; i < dependencyCount; ++i) append_order(required[i]);
        visitState[featureIndex] = 2u;
        plan.executionOrder.push_back(feature);
    };
    for (const auto& decision : plan.decisions) append_order(decision.feature);
    plan.valid = true;
    return plan;
}
} // namespace shinkou::render
