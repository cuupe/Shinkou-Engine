#include "EditorImGuiView.h"
#include "shinkou/editor/EditorLayer.h"
#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#include <implot.h>
#include <algorithm>
#include <cmath>

namespace shinkou::editor {
void EditorImGuiView::image(EditorLayer& e, render::Renderer& renderer, const std::string& key,
                            const std::shared_ptr<const ui::UiImageSnapshot>& snapshot, float height) {
    if (!snapshot || !snapshot->valid()) return;
    auto& cached=images_[key];
    if (cached.revision!=snapshot->revision) {
        render::TextureDesc desc; desc.width=snapshot->width; desc.height=snapshot->height; desc.format="rgba8";
        desc.initialData.resize(snapshot->bgraPremultiplied.size());
        for (std::size_t i=0;i<desc.initialData.size();i+=4) {
            const auto* p=snapshot->bgraPremultiplied.data()+i; auto* q=desc.initialData.data()+i;
            const unsigned a=p[3];
            q[0]=a?static_cast<std::uint8_t>(std::min(255u,p[2]*255u/a)):0;
            q[1]=a?static_cast<std::uint8_t>(std::min(255u,p[1]*255u/a)):0;
            q[2]=a?static_cast<std::uint8_t>(std::min(255u,p[0]*255u/a)):0; q[3]=p[3];
        }
        const auto texture=renderer.create_texture(desc);
        if (texture) { renderer.destroy_resource(cached.texture); cached.texture=texture; cached.revision=snapshot->revision; }
    }
    if (const auto id=renderer.imgui_texture(cached.texture)) {
        float fit=std::min(ImGui::GetContentRegionAvail().x/snapshot->width,height/snapshot->height);
        if (key=="asset-image-preview") fit*=e.assetSettings_.imageZoom;
        if (key=="asset-image-preview" && e.assetSettings_.imageCheckerboard) {
            const auto p=ImGui::GetCursorScreenPos();auto* draw=ImGui::GetWindowDrawList();
            const float w=snapshot->width*fit,h=snapshot->height*fit,cell=16*scale_;
            for(float y=0;y<h;y+=cell) for(float x=0;x<w;x+=cell)
                draw->AddRectFilled({p.x+x,p.y+y},{p.x+std::min(x+cell,w),p.y+std::min(y+cell,h)},
                    (static_cast<int>(x/cell)+static_cast<int>(y/cell))%2?IM_COL32(110,110,110,255):IM_COL32(160,160,160,255));
        }
        ImGui::Image(static_cast<ImTextureID>(id),{snapshot->width*fit,snapshot->height*fit}); region(e,key);
    }
}
void EditorImGuiView::preview(EditorLayer& e, render::Renderer& renderer) {
    const auto state=e.uiModel_.asset_preview();
    ImGui::PushID(state.path.c_str());
    ImGui::TextWrapped("%s",state.path.c_str());
    ImGui::TextDisabled("%s",state.kind.c_str());
    if (!ImGui::BeginTabBar("asset-inspection")) {ImGui::PopID();return;}
    const bool previewOpen=ImGui::BeginTabItem(label(e,"预览","Preview"));region(e,"asset-preview-tab");
    if (previewOpen) ImGui::EndTabItem();
    const bool settingsOpen=ImGui::BeginTabItem(label(e,"专属配置","Settings"));region(e,"asset-settings-tab");
    if (settingsOpen) ImGui::EndTabItem();
    ImGui::EndTabBar();
    assetTab_=settingsOpen?"settings":"preview";
    // Keep the selectors above the potentially tall/scrolling preview content.
    if (settingsOpen) asset_options(e);
    if (previewOpen) {
    ImGui::TextWrapped("%s",state.status.c_str());
    if (state.imageSnapshot) {
        ImGui::Text("%u × %u",state.imageWidth,state.imageHeight);
        if (ImGui::BeginChild("image-scroll",{0,240*scale_},ImGuiChildFlags_Borders,ImGuiWindowFlags_HorizontalScrollbar))
            image(e,renderer,"asset-image-preview",state.imageSnapshot,220*scale_);
        ImGui::EndChild();
    }
    const auto& mediaState=e.uiModel_.media_state();
    if (mediaState.path==state.path) {
        ImGui::TextWrapped("%s",mediaState.previewStatus.c_str());
        if (e.activeWorld_ && (mediaState.kind=="Audio" || mediaState.kind=="Video")) {
            ImGui::BeginDisabled(!mediaState.available);
            if(button(e,"播放","Play","asset-media.play")) e.dispatch_command(EditorCommand::MediaPlay,{},*e.activeWorld_);
            ImGui::SameLine();
            if(button(e,"暂停","Pause","asset-media.pause")) e.dispatch_command(EditorCommand::MediaPause,{},*e.activeWorld_);
            ImGui::SameLine();
            if(button(e,"停止","Stop","asset-media.stop")) e.dispatch_command(EditorCommand::MediaStop,{},*e.activeWorld_);
            ImGui::EndDisabled();
            if(button(e,"打开媒体面板","Open Media panel","asset-media.open")) {e.set_panel_visible("media",true);focusPanel_="media";}
        }
        if (mediaState.videoPreview) {
            const auto& v=*mediaState.videoPreview;
            ImGui::Text("%u × %u | %.2f fps | %.2f s",v.width,v.height,v.frameRate,v.duration);
            image(e,renderer,"asset-video-preview",v.firstFrame,220*scale_);
            if (v.audioStreamCount) ImGui::TextWrapped("%s",label(e,"视频画面预览；音轨暂未同步播放。","Video frame preview; audio tracks are not played."));
        }
        if (mediaState.audioPreview) {
            const auto& a=*mediaState.audioPreview;
            ImGui::Text("%u Hz | %u ch | %.2f s",a.sampleRate,a.channels,a.duration);
            if(!a.peaks.empty() && ImPlot::BeginPlot("##inspector-waveform",{ImGui::GetContentRegionAvail().x,100*scale_},ImPlotFlags_NoLegend|ImPlotFlags_NoMenus)) {
                ImPlot::SetupAxes("Seconds","Peak",ImPlotAxisFlags_AutoFit,ImPlotAxisFlags_AutoFit);
                ImPlot::PlotLine("Waveform",a.peaks.data(),static_cast<int>(a.peaks.size()),a.duration/std::max<std::size_t>(1,a.peaks.size()-1));
                ImPlot::EndPlot();
            }
        }
    }
    if (state.modelPreview) {
        if (const auto texture=renderer.imgui_texture(e.modelPreviewRenderer_.offscreen_color())) {
            const float width=std::max(1.0f,ImGui::GetContentRegionAvail().x);
            // A fixed neutral backdrop keeps lit geometry and wireframe
            // readable when the surrounding editor switches to a light theme.
            const auto origin=ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(origin,{origin.x+width,origin.y+width},IM_COL32(38,41,47,255));
            ImGui::Image(static_cast<ImTextureID>(texture),{width,width}); region(e,"asset-model-preview");
            const auto& io=ImGui::GetIO();
            if (ImGui::IsItemHovered() && io.MouseWheel) e.navigate_model_preview(ViewportNavigation::Zoom,{0,io.MouseWheel});
            if (ImGui::IsItemHovered() && ImGui::IsMouseDragging(0)) e.navigate_model_preview(ViewportNavigation::Orbit,{io.MouseDelta.x,io.MouseDelta.y});
        } else ImGui::TextWrapped("%s",state.modelGpuPreviewStatus.c_str());
        if (button(e,"重置视角","Reset view","asset-model-reset")) e.reset_model_preview();
        ImGui::TextWrapped("%s",state.modelMaterialLabel.c_str());
        if (button(e,"< 材质","< Material","asset-model-material-prev")) e.select_model_material(-1);
        ImGui::SameLine();
        if (button(e,"材质 >","Material >","asset-model-material-next")) e.select_model_material(1);
        ImGui::TextWrapped("%s · %s",state.modelTextureLabel.c_str(),state.modelTextureRole.c_str());
        if (button(e,"< 贴图","< Texture","asset-model-texture-prev")) e.select_model_texture(-1);
        ImGui::SameLine();
        if (button(e,"贴图 >","Texture >","asset-model-texture-next")) e.select_model_texture(1);
        image(e,renderer,"model-texture-preview",state.modelTextureSnapshot,180*scale_);
        if (button(e,"播放 / 暂停动画","Play / pause animation","asset-model-animation-toggle")) e.toggle_model_animation();
        if (button(e,"< 动画","< Animation","asset-model-animation-prev")) e.select_model_animation(-1);
        ImGui::SameLine();
        if (button(e,"动画 >","Animation >","asset-model-animation-next")) e.select_model_animation(1);
        if (state.modelPreviewScene && state.modelPreviewScene->animationPlayable) {
            const auto& animation=*state.modelPreviewScene;
            float time=animation.animationTime;
            if (ImGui::SliderFloat(label(e,"动画位置","Animation position"),&time,0,animation.animationDuration,"%.2f s") && animation.animationDuration>0)
                e.seek_model_animation(time/animation.animationDuration);
            region(e,"asset-model-animation-timeline");
        }
    }
    if (state.textPreview) {
        const auto& lines=state.textPreview->lines;
        ImGui::TextDisabled("%s | %zu lines",state.textPreview->encoding.c_str(),lines.size());
        ImGui::InputTextWithHint("##text-search",label(e,"搜索内容","Find text"),textFilter_.data(),textFilter_.size());region(e,"asset-text-search");
        if(button(e,"复制全文","Copy text","asset-text-copy")) {
            std::string text;for(const auto& line:lines) {text+=line;text+='\n';}ImGui::SetClipboardText(text.c_str());
        }
        if (ImGui::BeginChild("text-preview",{0,320*scale_},ImGuiChildFlags_Borders,e.assetSettings_.wordWrap?0:ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::SetWindowFontScale(e.assetSettings_.textScale);
            const auto drawLine=[&](int i) {
                const auto& line=lines[static_cast<std::size_t>(i)];
                if(textFilter_[0] && line.find(textFilter_.data())==std::string::npos) return;
                const bool heading=e.assetSettings_.documentStyles && static_cast<std::size_t>(i)<state.textPreview->styles.size() && state.textPreview->styles[i]>0 && state.textPreview->styles[i]<10;
                if(heading) ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextLink));
                if(e.assetSettings_.lineNumbers) {ImGui::TextDisabled("%4d",i+1);ImGui::SameLine();}
                if(e.assetSettings_.wordWrap) ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(line.empty()?" ":line.c_str());
                if(e.assetSettings_.wordWrap) ImGui::PopTextWrapPos();
                if(heading) ImGui::PopStyleColor();
            };
            if(!e.assetSettings_.wordWrap && !textFilter_[0]) {
                ImGuiListClipper clipper;clipper.Begin(static_cast<int>(lines.size()));
                while(clipper.Step()) for(int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) drawLine(i);
            } else for(int i=0;i<static_cast<int>(lines.size());++i) drawLine(i);
        }
        ImGui::EndChild();region(e,"asset-text-preview");
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s",state.assetSystemStatus.c_str());
    }
    ImGui::PopID();
}
void EditorImGuiView::console(EditorLayer& e) {
    if (!begin_panel(e,"console")) return;
    if (button(e,"清空","Clear","console.clear")) e.consoleEntries_.clear();
    ImGui::SameLine(); ImGui::TextDisabled("%zu",e.consoleEntries_.size());
    ImGui::Separator();
    ImGui::BeginChild("messages");
    ImGuiListClipper clipper; clipper.Begin(static_cast<int>(e.consoleEntries_.size()));
    while (clipper.Step()) for (int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) ImGui::TextUnformatted(e.consoleEntries_[i].c_str());
    ImGui::EndChild(); ImGui::End();
}
void EditorImGuiView::profiler(EditorLayer& e, const render::Renderer& renderer) {
    if (!begin_panel(e,"profiler")) return;
    if (ImPlot::BeginPlot(label(e,"帧耗时 (ms)","Frame time (ms)"),{-1,180*scale_})) {
        ImPlot::SetupAxes(label(e,"帧","Frame"),"ms",ImPlotAxisFlags_AutoFit,ImPlotAxisFlags_AutoFit);
        if (!e.frameTimes_.empty()) ImPlot::PlotLine("CPU",e.frameTimes_.data(),static_cast<int>(e.frameTimes_.size()));
        ImPlot::EndPlot();
    }
    const auto stats=renderer.stats();
    ImGui::Text("Draws %llu | Dispatch %llu | Barriers %llu",static_cast<unsigned long long>(stats.drawCalls),static_cast<unsigned long long>(stats.dispatchCalls),static_cast<unsigned long long>(stats.barriers));
    ImGui::Text("Resources +%llu / -%llu",static_cast<unsigned long long>(stats.resourceCreates),static_cast<unsigned long long>(stats.resourceDestroys));
    ImGui::End();
}
void EditorImGuiView::render_graph(EditorLayer& e, render::Renderer& renderer) {
    if (!begin_panel(e,"render-graph")) return;
    const auto& diagnostics=renderer.graph().diagnostics();
    ImGui::Text("%zu passes | %zu resources",diagnostics.passCount,diagnostics.resourceCount);
    ImGui::TextDisabled("%s",label(e,"执行顺序来自编译后的渲染图","Execution order from the compiled render graph"));
    if (ImGui::BeginTable("passes",4,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Pass"); ImGui::TableSetupColumn("CPU ms"); ImGui::TableSetupColumn("GPU ms"); ImGui::TableSetupColumn("Draws"); ImGui::TableHeadersRow();
        for (const auto& pass:renderer.graph().pass_timings()) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(pass.name.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.3f",pass.cpuNanoseconds/1e6);
            ImGui::TableNextColumn(); if (pass.gpuNanoseconds) ImGui::Text("%.3f",pass.gpuNanoseconds/1e6); else ImGui::TextDisabled("—");
            ImGui::TableNextColumn(); ImGui::Text("%llu",static_cast<unsigned long long>(pass.drawCalls));
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
void EditorImGuiView::settings(EditorLayer& e) {
    if (!begin_panel(e,"settings")) return;
    ImGui::SeparatorText(label(e,"外观","Appearance"));
    if (ImGui::BeginCombo(label(e,"主题","Theme"),e.layout_.theme.c_str())) {
        for (const auto* theme:{"dark","light","high-contrast"}) if (ImGui::Selectable(theme,e.layout_.theme==theme)) e.set_theme(theme);
        ImGui::EndCombo();
    }
    if (ImGui::BeginCombo(label(e,"语言","Language"),e.layout_.language.c_str())) {
        for (const auto* language:{"zh-CN","en-US"}) if (ImGui::Selectable(language,e.layout_.language==language)) e.set_language(language);
        ImGui::EndCombo();
    }
    if (!ImGui::IsAnyItemActive()) {scaleEdit_=e.layout_.uiScale;fontSizeEdit_=e.layout_.fontSize;}
    ImGui::SliderFloat(label(e,"界面缩放","UI scale"),&scaleEdit_,0.75f,2.0f,"%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {e.layout_.uiScale=scaleEdit_;e.sync_layout_to_style();}
    region(e,"settings.ui-scale");
    ImGui::SliderFloat(label(e,"字体大小","Font size"),&fontSizeEdit_,12.0f,32.0f,"%.0f px");
    if (ImGui::IsItemDeactivatedAfterEdit()) {e.layout_.fontSize=fontSizeEdit_;e.sync_layout_to_style();}
    region(e,"settings.font-size");
    if (button(e,"恢复默认缩放与字号","Reset scale and font","settings.reset-scale")) {
        e.layout_.uiScale=1;e.layout_.fontSize=EditorLayoutState{}.fontSize;e.sync_layout_to_style();
    }
    ImGui::SeparatorText(label(e,"工作区","Workspace"));
    if (button(e,"重置停靠布局","Reset docking layout","settings.reset-layout")) resetDock_=true;
    if (button(e,"保存设置","Save preferences","settings.save")) e.save_layout();
    ImGui::End();
}
void EditorImGuiView::build(EditorLayer& e, World& world) {
    if (!begin_panel(e,"build")) return;
    const auto state=e.uiModel_.build_state();
    ImGui::TextWrapped("%s",state.status.c_str());
    if (ImGui::BeginCombo(label(e,"配置","Profile"),state.selectedProfileId.c_str())) {
        for (const auto& id:state.profileIds) if (ImGui::Selectable(id.c_str(),id==state.selectedProfileId)) e.select_build_profile(id);
        ImGui::EndCombo();
    }
    if (ImGui::BeginTable("build-fields",2,ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& f:state.profileFields) field(e,f);
        ImGui::EndTable();
    }
    if (button(e,state.running?"取消构建":"构建项目",state.running?"Cancel build":"Build project","build.run"))
        e.dispatch_command(state.running?EditorCommand::CancelBuild:EditorCommand::BuildProject,{},world);
    ImGui::SameLine();
    if (button(e,"刷新工具链","Refresh tools","build.tools")) e.dispatch_command(EditorCommand::RefreshBuildTools,{},world);
    if (ImGui::CollapsingHeader(label(e,"项目与 IDE","Project and IDE"))) {
        ImGui::TextWrapped("%s",state.projectStatus.c_str());
        for (const auto& ide:state.ides) { ImGui::Text("%s: %s",ide.name.c_str(),ide.available?ide.executable.c_str():"Unavailable"); }
        for (const auto* id:{"vscode","visual-studio","rider"}) {
            if (ImGui::Button(id)) e.dispatch_command(EditorCommand::OpenProjectInIde,id,world);
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }
    if (ImGui::BeginTabBar("build-output")) {
        if (ImGui::BeginTabItem(label(e,"输出","Output"))) {
            ImGui::BeginChild("output");
            ImGuiListClipper clipper; clipper.Begin(static_cast<int>(state.outputLines.size()));
            while (clipper.Step()) for (int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) ImGui::TextUnformatted(state.outputLines[i].c_str());
            ImGui::EndChild(); ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(label(e,"诊断","Diagnostics"))) {
            for (std::size_t i=0;i<state.diagnostics.size();++i) {
                const auto& d=state.diagnostics[i]; ImGui::PushID(static_cast<int>(i));
                const auto text=d.severity+": "+d.file+":"+std::to_string(d.line)+"  "+d.message;
                if (ImGui::Selectable(text.c_str(),i==state.selectedDiagnostic)) e.select_build_diagnostic(std::to_string(i));
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) e.open_diagnostic_in_ide(d.file+":"+std::to_string(d.line)+":"+std::to_string(d.column));
                ImGui::PopID();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
void EditorImGuiView::media(EditorLayer& e, render::Renderer& renderer, World& world) {
    if (!begin_panel(e,"media")) return;
    const auto state=e.uiModel_.media_state();
    ImGui::TextWrapped("%s",state.path.c_str()); ImGui::TextWrapped("%s",state.status.c_str());
    ImGui::BeginDisabled(!state.available);
    if (button(e,"播放","Play","media.play")) e.dispatch_command(EditorCommand::MediaPlay,{},world);
    ImGui::SameLine();
    if (button(e,"暂停","Pause","media.pause")) e.dispatch_command(EditorCommand::MediaPause,{},world);
    ImGui::SameLine();
    if (button(e,"停止","Stop","media.stop")) e.dispatch_command(EditorCommand::MediaStop,{},world);
    bool loop=state.loop;
    if (ImGui::Checkbox(label(e,"循环","Loop"),&loop)) e.dispatch_command(EditorCommand::MediaToggleLoop,{},world);
    float time=static_cast<float>(state.currentTime),volume=static_cast<float>(state.volume);
    if (state.duration>0 && ImGui::SliderFloat(label(e,"位置","Position"),&time,0,static_cast<float>(state.duration),"%.2f s")) e.dispatch_command(EditorCommand::MediaSeek,std::to_string(time/state.duration),world);
    region(e,"media.seek");
    if (state.kind=="Audio" && ImGui::SliderFloat(label(e,"音量","Volume"),&volume,0,1)) e.dispatch_command(EditorCommand::MediaSetVolume,std::to_string(volume),world);
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s",state.previewStatus.c_str());
    if(state.videoPreview) {
        image(e,renderer,"media-video",state.videoPreview->firstFrame,240*scale_);
        if(state.videoPreview->audioStreamCount) ImGui::TextDisabled("Video only / %u audio stream(s)",state.videoPreview->audioStreamCount);
        const double step=state.videoPreview->frameRate>0?1.0/state.videoPreview->frameRate:1.0/30.0;
        ImGui::BeginDisabled(!state.available || state.duration<=0);
        if(button(e,"上一帧","Previous frame","media.prev-frame")) {
            e.dispatch_command(EditorCommand::MediaPause,{},world);
            e.dispatch_command(EditorCommand::MediaSeek,std::to_string(std::clamp((state.currentTime-step)/state.duration,0.0,1.0)),world);
        }
        ImGui::SameLine();
        if(button(e,"下一帧","Next frame","media.next-frame")) {
            e.dispatch_command(EditorCommand::MediaPause,{},world);
            e.dispatch_command(EditorCommand::MediaSeek,std::to_string(std::clamp((state.currentTime+step)/state.duration,0.0,1.0)),world);
        }
        ImGui::EndDisabled();
    }
    if(state.audioPreview && !state.audioPreview->peaks.empty()) {
        const auto& a=*state.audioPreview;
        if(ImPlot::BeginPlot("##audio-waveform",{ImGui::GetContentRegionAvail().x,110*scale_},ImPlotFlags_NoLegend|ImPlotFlags_NoMenus)) {
            ImPlot::SetupAxes("Seconds","Peak",ImPlotAxisFlags_AutoFit,ImPlotAxisFlags_AutoFit);
            const auto dx=a.duration/std::max<std::size_t>(1,a.peaks.size()-1);
            ImPlot::PlotLine("Waveform",a.peaks.data(),static_cast<int>(a.peaks.size()),dx);
            ImPlot::EndPlot();
        }
        region(e,"media.waveform");
    }
    ImGui::End();
}
void EditorImGuiView::recovery(EditorLayer& e, World& world) {
    if (!begin_panel(e,"recovery")) return;
    const auto state=e.uiModel_.file_recovery_state();
    ImGui::TextWrapped("%s",state.status.c_str());
    ImGui::Text("Undo %zu | Redo %zu | Orphans %zu",state.undoCount,state.redoCount,state.orphanCount);
    if (button(e,"撤销","Undo","recovery.undo")) e.dispatch_command(EditorCommand::Undo,{},world);
    ImGui::SameLine();
    if (button(e,"重做","Redo","recovery.redo")) e.dispatch_command(EditorCommand::Redo,{},world);
    for (const auto& entry:state.entries) { ImGui::BulletText("%s",entry.sourcePath.c_str()); ImGui::TextDisabled("%s",entry.recyclePath.c_str()); }
    ImGui::End();
}
}
#endif
