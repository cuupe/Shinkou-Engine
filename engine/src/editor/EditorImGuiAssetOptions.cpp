#include "EditorImGuiView.h"
#include "EditorImGuiWidgets.h"
#include "shinkou/editor/EditorLayer.h"
#if defined(SHINKOU_WITH_IMGUI)
#include <algorithm>
#include <imgui.h>

namespace shinkou::editor {
void EditorImGuiView::asset_options(EditorLayer &e) {
    const auto *entry = e.assetIndex_ ? e.assetIndex_->find(e.assetBrowser_.selection) : nullptr;
    if (!entry)
        return;
    auto settings = e.assetSettings_;
    bool changed = false;
    const auto kind = entry->descriptor.kind;
    ImGui::TextDisabled("%s | %llu bytes", entry->descriptor.extension.c_str(),
                        static_cast<unsigned long long>(entry->descriptor.size));
    if (button(e, "保存配置", "Save settings", "asset-option.save"))
        e.save_asset_settings();
    ImGui::SameLine();
    if (button(e, "重新加载", "Reload", "asset-option.reload")) {
        const auto previous = e.assetSettings_;
        e.assetSettingsPath_.clear();
        e.load_asset_settings();
        const auto loaded = e.assetSettings_;
        const auto status = e.assetSettingsStatus_;
        if (!e.assetSettingsLoadFailed_) {
            e.assetSettings_ = previous;
            e.set_asset_settings(loaded);
            e.assetSettingsStatus_ = status;
        } else
            e.assetSettings_ = previous;
        settings = e.assetSettings_;
    }
    if (button(e, "恢复默认值", "Defaults", "asset-option.defaults")) {
        e.set_asset_settings({});
        settings = e.assetSettings_;
    }
    ImGui::TextWrapped("%s", e.assetSettingsStatus_.c_str());
    ImGui::Separator();
    if (!ImGui::BeginChild("asset-options-scroll", {0, 0})) {
        ImGui::EndChild();
        return;
    }
    const auto check = [&](const char *zh, const char *en, bool &value, const char *id) {
        changed = widgets::checkbox(label(e, zh, en), value) || changed;
        region(e, id);
    };
    const auto slider = [&](const char *zh, const char *en, float &value, float lo, float hi,
                            const char *id, const char *format = "%.2f") {
        changed = widgets::slider(label(e, zh, en), value, lo, hi, format) || changed;
        region(e, id);
    };
    const bool text = e.assetPreviewState_.textPreview || kind == AssetPreviewKind::Text ||
                      kind == AssetPreviewKind::Document || kind == AssetPreviewKind::Shader;
    if (text) {
        check("自动换行", "Word wrap", settings.wordWrap, "asset-option.wrap");
        check("显示行号", "Line numbers", settings.lineNumbers, "asset-option.line-numbers");
        slider("文字缩放", "Text scale", settings.textScale, 0.75f, 2, "asset-option.text-scale");
        if (kind == AssetPreviewKind::Document) {
            check("显示文档标题样式", "Heading styles", settings.documentStyles,
                  "asset-option.document-styles");
            ImGui::TextWrapped(
                "%s",
                label(e, "预览包含正文、标题和表格文字。分页、图片及复杂排版请使用文档编辑器。",
                      "Preview includes paragraphs, headings and table text. Page layout and "
                      "embedded images require a document editor."));
        } else if (kind == AssetPreviewKind::Text || kind == AssetPreviewKind::Shader) {
            if (button(e, "在 IDE 中打开", "Open in IDE", "asset-option.open-ide"))
                e.open_diagnostic_in_ide(e.assetBrowser_.selection + ":1:1");
        }
    }
    if (kind == AssetPreviewKind::Audio) {
        ImGui::TextUnformatted(label(e, "引擎音频试听参数", "Engine audio audition"));
        int bus = static_cast<int>(settings.bus);
        if (ImGui::Combo(label(e, "输出总线", "Output bus"), &bus,
                         "Master\0Music\0Sfx\0Voice\0Ambient\0UI\0")) {
            settings.bus = static_cast<unsigned>(bus);
            changed = true;
        }
        region(e, "asset-option.audio-bus");
        check("循环", "Loop", settings.loop, "asset-option.loop");
        check("流式读取", "Streaming", settings.streaming, "asset-option.streaming");
        slider("音量", "Volume", settings.volume, 0, 1, "asset-option.volume");
        slider("音高 / 播放速率", "Pitch / playback rate", settings.pitch, 0.25f, 4,
               "asset-option.pitch");
        slider("声像", "Pan", settings.pan, -1, 1, "asset-option.pan");
        slider("淡入（下次播放）", "Fade in (next play)", settings.fadeIn, 0, 10,
               "asset-option.fade-in", "%.2f s");
        check("空间音频", "Spatial audio", settings.spatialized, "asset-option.spatial");
        if (settings.spatialized) {
            slider("最小距离", "Min distance", settings.minDistance, 0.01f, 100,
                   "asset-option.min-distance");
            settings.maxDistance = std::max(settings.maxDistance, settings.minDistance);
            slider("最大距离", "Max distance", settings.maxDistance, settings.minDistance, 1000,
                   "asset-option.max-distance");
            slider("距离衰减", "Rolloff", settings.rolloff, 0, 10, "asset-option.rolloff");
            float p[3]{settings.positionX, settings.positionY, settings.positionZ};
            ImGui::TextUnformatted(label(e, "声源位置", "Source position"));
            widgets::VectorInput position;
            position.scale = scale_;
            position.speed = 0.1f;
            position.minimum = -100000;
            position.maximum = 100000;
            position.resetLabel = label(e, "重置", "Reset");
            position.onAxis = [&](int axis, const widgets::EditEvent &event) {
                if (qaEnabled_) {
                    const char *names[]{"X", "Y", "Z"};
                    region(e, std::string("asset-option.audio-position:") + names[axis]);
                }
                if (event.changed || event.reset) {
                    settings.positionX = p[0];
                    settings.positionY = p[1];
                    settings.positionZ = p[2];
                    changed = true;
                }
            };
            widgets::vector_input(p, 3, position);
        }
        if (ImGui::CollapsingHeader(label(e, "试听效果链", "Audition effects"))) {
            check("低通滤波", "Low pass", settings.lowPass, "asset-option.low-pass");
            if (settings.lowPass)
                slider("截止频率", "Cutoff", settings.cutoffHz, 20, 20000, "asset-option.cutoff",
                       "%.0f Hz");
            check("混响", "Reverb", settings.reverb, "asset-option.reverb");
            if (settings.reverb)
                slider("混响湿声", "Reverb wet", settings.reverbWet, 0, 1,
                       "asset-option.reverb-wet");
        }
    }
    if (kind == AssetPreviewKind::Video) {
        check("循环播放", "Loop", settings.loop, "asset-option.loop");
        slider("预览速率", "Preview rate", settings.videoRate, 0.1f, 4, "asset-option.video-rate");
        ImGui::TextWrapped(
            "%s", label(e, "媒体面板支持播放、暂停、定位和逐帧查看。视频解码取决于系统编解码器。",
                        "Use the Media panel for playback, seeking and frame stepping. Codec "
                        "support depends on the system."));
    }
    if (kind == AssetPreviewKind::Image) {
        check("透明棋盘背景", "Transparency checkerboard", settings.imageCheckerboard,
              "asset-option.checkerboard");
        slider("预览缩放", "Preview zoom", settings.imageZoom, 0.25f, 4, "asset-option.image-zoom");
    }
    if (kind == AssetPreviewKind::Model) {
        check("自动旋转", "Auto rotate", settings.modelAutoRotate, "asset-option.model-rotate");
        check("线框显示", "Wireframe", settings.modelWireframe, "asset-option.model-wire");
        if (const auto &model = e.assetPreviewState_.modelPreview) {
            ImGui::Text("%zu vertices | %zu triangles", model->vertexCount, model->triangleCount);
            ImGui::Text("%zu meshes | %zu materials", model->meshCount, model->materialCount);
            ImGui::Text("Bounds (%.3f, %.3f, %.3f) - (%.3f, %.3f, %.3f)", model->minX, model->minY,
                        model->minZ, model->maxX, model->maxY, model->maxZ);
            if (model->sourceFormat == "fbx")
                ImGui::TextWrapped(
                    "%s", label(e, "FBX 支持几何、法线、UV 和材质颜色。贴图与动画播放尚未接入。",
                                "FBX supports geometry, normals, UVs and material colors. Texture "
                                "loading and animation playback are not connected yet."));
        }
    }
    if (changed)
        e.set_asset_settings(settings);
    if (kind == AssetPreviewKind::Audio && e.activeWorld_) {
        if (button(e, "创建场景音源", "Create scene AudioSource",
                   "asset-option.create-audio-source"))
            e.create_audio_source_from_asset(*e.activeWorld_);
        ImGui::TextWrapped(
            "%s",
            label(e, "创建音源会复制音量、音高、总线、循环、流式读取及空间参数；效果链用于试听。",
                  "Creates a source with volume, pitch, bus, loop, streaming and spatial settings. "
                  "The effect chain is for auditioning."));
    }
    ImGui::EndChild();
}
} // namespace shinkou::editor
#endif
