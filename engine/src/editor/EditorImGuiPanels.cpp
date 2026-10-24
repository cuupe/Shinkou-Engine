#include "EditorImGuiView.h"
#include "EditorGlyphs.h"
#include "shinkou/editor/EditorLayer.h"
#if defined(SHINKOU_WITH_IMGUI)
#include "shinkou/World.h"
#include "shinkou/GameObject.h"
#include "shinkou/render/RenderScene.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <sstream>

namespace shinkou::editor {
void EditorImGuiView::object_node(EditorLayer& e, const EditorObjectTreeNode& node) {
    ImGui::PushID(std::to_string(node.id).c_str());
    auto flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (node.selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (!node.children.empty()) ImGui::SetNextItemOpen(true,ImGuiCond_Once);
    if (!node.active) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,0.5f);
    const bool open = ImGui::TreeNodeEx("object", flags, "%s  %s",iconsReady_?(node.children.empty()?glyph::Object:glyph::Folder):"",node.name.c_str());
    region(e,"hierarchy:" + std::to_string(node.id));
    alias_region("hierarchy-name:"+node.name,"hierarchy:"+std::to_string(node.id));
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        e.finish_inspector_edit();
        e.layout_.selectedObject = node.id;
        e.uiModel_.select_object(node.id);
        e.set_selected_asset({});
    }
    if(ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))ImGui::SetTooltip("%s",node.name.c_str());
    if(ImGui::BeginPopupContextItem("object-actions")) {
        e.finish_inspector_edit();e.layout_.selectedObject=node.id;e.uiModel_.select_object(node.id);e.set_selected_asset({});
        if(ImGui::MenuItem(label(e,"聚焦对象","Frame selection"),"F"))e.dispatch_command(EditorCommand::FrameSelection,{},*e.activeWorld_);
        if(ImGui::MenuItem(label(e,"创建子对象","Create child")))e.dispatch_command(EditorCommand::CreateChild,{},*e.activeWorld_);
        ImGui::Separator();
        if(ImGui::MenuItem(label(e,"删除对象","Delete object"),"Delete"))e.dispatch_command(EditorCommand::DeleteObject,{},*e.activeWorld_);
        ImGui::EndPopup();
    }
    if (!node.active) ImGui::PopStyleVar();
    if (open) { for (const auto& child : node.children) object_node(e,child); ImGui::TreePop(); }
    ImGui::PopID();
}
void EditorImGuiView::hierarchy(EditorLayer& e, World& world) {
    if (!begin_panel(e,"hierarchy")) return;
    ImGui::SetNextItemWidth(std::max(40*scale_,ImGui::GetContentRegionAvail().x-ImGui::GetFrameHeight()-ImGui::GetStyle().ItemSpacing.x));
    if (ImGui::InputTextWithHint("##filter",label(e,"搜索场景对象","Search objects"),objectFilter_.data(),objectFilter_.size()))
        e.uiModel_.set_object_filter(objectFilter_.data());
    region(e,"hierarchy.filter");
    ImGui::SameLine();
    if (button(e,"+ 创建对象","+ Create object","command:create-empty"))
        e.dispatch_command(EditorCommand::CreateEmpty,{},world);
    ImGui::Separator();
    for (const auto& node : e.uiModel_.object_roots()) object_node(e,node);
    if (ImGui::BeginPopupContextWindow("objects-context",ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem(label(e,"创建空对象","Create empty"))) e.dispatch_command(EditorCommand::CreateEmpty,{},world);
        if (ImGui::MenuItem(label(e,"创建 3D 对象","Create 3D object"))) e.dispatch_command(EditorCommand::Create3DObject,{},world);
        ImGui::EndPopup();
    }
    ImGui::End();
}
void EditorImGuiView::assets(EditorLayer& e) {
    if (!begin_panel(e,"assets")) return;
    if (button(e,"上一级","Up","asset-parent")) e.set_asset_directory(e.assetDirectory_.parent_path());
    ImGui::SameLine();
    if (button(e,"刷新","Refresh","asset-refresh")) e.handle_asset_action(EditorAssetAction::Refresh,{},{});
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(100.0f,ImGui::GetContentRegionAvail().x));
    const bool filterChanged = ImGui::InputTextWithHint("##filter",label(e,"搜索资源（包含子目录）","Search assets (including subfolders)"),assetFilter_.data(),assetFilter_.size());
    region(e,"assets.filter");
    ImGui::TextDisabled("%s",e.assetDirectory_.empty()?"/":e.assetDirectory_.generic_string().c_str());
    const std::string scope=e.assetDirectory_.generic_string()+"|"+assetFilter_.data();
    if (fileRevision_!=e.projectFilesRevision_ || fileScope_!=scope || filterChanged) {
        fileRevision_=e.projectFilesRevision_; fileScope_=scope; files_.clear();
        for (std::size_t i=0;i<e.projectFiles_.size();++i) {
            const auto& entry=e.projectFiles_[i];
            const auto path=entry.relativePath.generic_string();
            if (assetFilter_[0]) { if (path.find(assetFilter_.data())!=std::string::npos) files_.push_back(i); }
            else if (entry.relativePath.parent_path().lexically_normal()==e.assetDirectory_.lexically_normal()) files_.push_back(i);
        }
        std::stable_sort(files_.begin(),files_.end(),[&](auto a,auto b) {
            const auto& l=e.projectFiles_[a];const auto& r=e.projectFiles_[b];
            return l.directory!=r.directory ? l.directory : l.name<r.name;
        });
    }
    visibleAssets_=files_.size();
    if (ImGui::BeginTable("asset-list",2,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY|ImGuiTableFlags_Resizable|ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(label(e,"名称","Name"),ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(label(e,"大小","Size"),ImGuiTableColumnFlags_WidthFixed,85*scale_);
        ImGui::TableSetupScrollFreeze(0,1); ImGui::TableHeadersRow();
        ImGuiListClipper clipper; clipper.Begin(static_cast<int>(files_.size()));
        while (clipper.Step()) for (int i=clipper.DisplayStart;i<clipper.DisplayEnd;++i) {
            const auto& entry=e.projectFiles_[files_[i]];
            const auto path=entry.relativePath.generic_string();
            ImGui::PushID(path.c_str()); ImGui::TableNextRow(); ImGui::TableNextColumn();
            const auto name=std::string(entry.directory?"▸  ":"    ")+entry.name;
            if (ImGui::Selectable(name.c_str(),e.selectedAsset_==path,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowDoubleClick)) {
                e.set_selected_asset(path);
                if (!entry.directory) {e.set_panel_visible("inspector",true);focusPanel_="inspector";}
                if (ImGui::IsMouseDoubleClicked(0)) e.handle_asset_action(entry.directory?EditorAssetAction::Navigate:EditorAssetAction::Open,path,{});
            }
            region(e,"asset:"+path);
            if (!entry.directory && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("SHINKOU_ASSET",path.c_str(),path.size()+1); ImGui::TextUnformatted(entry.name.c_str()); ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("asset-context")) {
                if (ImGui::MenuItem(label(e,"打开","Open"))) e.handle_asset_action(entry.directory?EditorAssetAction::Navigate:EditorAssetAction::Open,path,{});
                if (ImGui::MenuItem(label(e,"重命名…","Rename…"))) { actionPath_=path; actionKind_="rename"; std::strncpy(rename_.data(),entry.name.c_str(),rename_.size()-1); }
                if (ImGui::MenuItem(label(e,"移到回收区…","Move to recovery…"))) { actionPath_=path; actionKind_="delete"; }
                if (entry.directory && ImGui::MenuItem(label(e,"新建文件夹…","New folder…"))) { actionPath_=path; actionKind_="folder"; rename_.fill(0); }
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            if (entry.directory) ImGui::TextDisabled("—"); else ImGui::TextDisabled("%.1f KB",entry.size/1024.0);
            ImGui::PopID();
        }
        assetScroll_=ImGui::GetScrollY()/std::max(scale_,0.5f);
        ImGui::EndTable();
    }
    if (ImGui::BeginPopupContextWindow("asset-background",ImGuiPopupFlags_MouseButtonRight|ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::MenuItem(label(e,"新建文件夹…","New folder…"))) { actionPath_=e.assetDirectory_.generic_string(); actionKind_="folder"; rename_.fill(0); }
        ImGui::EndPopup();
    }
    if (!actionKind_.empty()) ImGui::OpenPopup("asset-operation");
    if (ImGui::BeginPopupModal("asset-operation",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s",actionPath_.c_str());
        const bool deleting=actionKind_=="delete";
        if (deleting) ImGui::TextWrapped("%s",label(e,"资源将移入项目回收区，可通过撤销恢复。","The asset will move to project recovery and can be undone."));
        else { ImGui::SetNextItemWidth(280*scale_); ImGui::InputText(label(e,"名称","Name"),rename_.data(),rename_.size()); }
        if (button(e,"确认","Confirm","asset-delete-confirm")) {
            e.handle_asset_action(deleting?EditorAssetAction::Delete:actionKind_=="rename"?EditorAssetAction::Rename:EditorAssetAction::NewFolder,actionPath_,rename_.data());
            actionKind_.clear(); ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(label(e,"取消","Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) { actionKind_.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::End();
}
void EditorImGuiView::viewport(EditorLayer& e, render::Renderer& renderer, World& world, bool game) {
    if (!begin_panel(e,game?"game":"viewport",ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse)) return;
    if (!renderer.capabilities().supportsEditorViewportScissor) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::BeginChild("viewport-unavailable",{},ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s",label(e,"当前后端尚未接通编辑器场景视口。请使用 D3D11 编辑器。","This backend does not support the editor scene viewport yet. Use the D3D11 editor."));
        ImGui::EndChild(); ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    if (!game) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
        ImGui::BeginChild("viewport-tools",{0,0},ImGuiChildFlags_Borders|ImGuiChildFlags_AutoResizeY,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetNextItemWidth(80*scale_);
        const char* viewLabel=e.renderViewMode_=="Perspective"?label(e,"透视","Perspective"):e.renderViewMode_.c_str();
        if (ImGui::BeginCombo("##view",viewLabel)) {
            for (const auto* mode : {"Perspective","Front","Back","Left","Right","Top","Bottom"}) {
                if (ImGui::Selectable(mode,e.renderViewMode_==mode)) { e.renderViewMode_=mode; e.apply_render_view_mode(world,mode); }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const char* ops[] = {"W", "E", "R"};
        const char* icons[]={glyph::Move,glyph::Rotate,glyph::Scale};
        for (int i=0;i<3;++i) {
            ImGui::PushID(i);
            const bool selected=operation_==i;
            if (selected) ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(iconsReady_?icons[i]:ops[i])) operation_=i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",i==0?label(e,"移动 (W)","Move (W)"):i==1?label(e,"旋转 (E)","Rotate (E)"):label(e,"缩放 (R)","Scale (R)"));
            if (selected) ImGui::PopStyleColor();
            ImGui::PopID(); ImGui::SameLine();
        }
        ImGui::Checkbox(ImGui::GetContentRegionAvail().x>80*scale_?label(e,"网格","Grid"):"##grid",&grid_);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",label(e,"显示网格","Show grid"));
        ImGui::EndChild(); ImGui::PopStyleColor();
    }
    const auto origin=ImGui::GetCursorScreenPos(), available=ImGui::GetContentRegionAvail();
    const ImVec2 pos{std::round(origin.x),std::round(origin.y)},size{std::floor(available.x),std::floor(available.y)};
    if (size.x<1 || size.y<1) { ImGui::End(); return; }
    const auto hostOrigin=ImGui::GetMainViewport()->Pos;
    if (!game || !viewportVisible_) viewport_={pos.x-hostOrigin.x,pos.y-hostOrigin.y,size.x,size.y};
    viewportVisible_=true;
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("scene-surface",size,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight|ImGuiButtonFlags_MouseButtonMiddle);
    region(e,game ? "game.surface" : "viewport.surface");
    const bool hovered=ImGui::IsItemHovered(); const bool active=ImGui::IsItemActive();
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload=ImGui::AcceptDragDropPayload("SHINKOU_ASSET")) {
            const float s=std::max(0.25f,e.displayDpiScale_*e.layout_.uiScale);
            e.drop_asset_to_viewport(static_cast<const char*>(payload->Data),{(ImGui::GetIO().MousePos.x-hostOrigin.x)/s,(ImGui::GetIO().MousePos.y-hostOrigin.y)/s});
        }
        ImGui::EndDragDropTarget();
    }
    std::vector<EditorModelSceneInstance> instances;
    world.each_game_object([&](const GameObject& object) {
        const auto* reference=object.get_component<AssetReferenceComponent>();
        const auto* transform=object.get_component<components::TransformComponent>();
        if (!reference || !transform || !object.active_in_hierarchy()) return;
        const auto found=e.modelSceneAssets_.find(reference->asset_id());
        if (found!=e.modelSceneAssets_.end() && found->second.snapshot && found->second.snapshot->valid())
            instances.push_back({reference->asset_id(),object.id(),found->second.snapshot,transform->world_matrix()});
    });
    auto& worldView=worldViews_[game ? 1 : 0];
    if (worldView.render(renderer,world,static_cast<std::uint32_t>(std::round(size.x)),
        static_cast<std::uint32_t>(std::round(size.y)),instances,!game && grid_)) {
        const auto texture=renderer.imgui_texture(worldView.texture());
        if (texture) ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(texture),pos,{pos.x+size.x,pos.y+size.y});
        e.modelSceneRenderState_=worldView.model_state();
    }
    ImGui::GetWindowDrawList()->PushClipRect(pos,{pos.x+size.x,pos.y+size.y},true);
    if (!game) {
        render::RenderScene scene; scene.extract(world,renderer,size.x/size.y);
        const auto& view=scene.view();
        // Stable projected hit regions let native QA click actual world pixels.
        // They are observations only; picking still travels through Win32 input.
        world.each_game_object([&](const GameObject& object) {
            if (!object.active_in_hierarchy() || (!object.has_ecs_entity() && !object.get_component<AssetReferenceComponent>())) return;
            const auto* transform=object.get_component<components::TransformComponent>();
            if (!transform) return;
            const auto center=math::TransformPoint(transform->world_matrix(),{0,0,0});
            const auto clip=math::TransformVector4(view.viewProjection,{center.x,center.y,center.z,1});
            if (clip.w<=0 || clip.z<0 || clip.z>clip.w || std::abs(clip.x)>=clip.w || std::abs(clip.y)>=clip.w) return;
            const float x=pos.x+(clip.x/clip.w+1)*size.x*0.5f,y=pos.y+(1-clip.y/clip.w)*size.y*0.5f;
            region_rect(e,"world-object:"+std::string(object.name()),{x-3,y-3,6,6});
        });
        ImGuizmo::SetOrthographic(view.projection.orthographic);
        ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList()); ImGuizmo::SetRect(pos.x,pos.y,size.x,size.y);
        if (auto* object=world.find_object(e.layout_.selectedObject)) {
            if (auto* transform=object->get_component<components::TransformComponent>()) {
                auto matrix=transform->world_matrix();
                const ImGuizmo::OPERATION operations[]={ImGuizmo::TRANSLATE,ImGuizmo::ROTATE,ImGuizmo::SCALE};
                const bool changed=ImGuizmo::Manipulate(view.viewMatrix.m,view.projectionMatrix.m,operations[operation_],localGizmo_?ImGuizmo::LOCAL:ImGuizmo::WORLD,matrix.m);
                const bool usingGizmo=ImGuizmo::IsUsing();
                const auto field=std::to_string(transform->id())+":__local";
                if (usingGizmo && !gizmoEditing_) gizmoEditing_=e.begin_inspector_edit(field);
                if (changed && gizmoEditing_) {
                    if (object->parent()) if (auto* parent=object->parent()->get_component<components::TransformComponent>()) matrix=math::InverseOrIdentity(parent->world_matrix())*matrix;
                    math::Transform next;
                    if (math::Decompose(matrix,next)) e.edit_inspector_value(field,next);
                }
                if (!usingGizmo && gizmoEditing_) {e.finish_inspector_edit();gizmoEditing_=false;}
            }
        }
        if (!ImGuizmo::IsUsing()) {
            const auto& io=ImGui::GetIO();
            if (hovered && io.MouseWheel!=0) e.navigate_viewport(ViewportNavigation::Zoom,{0,io.MouseWheel});
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) e.navigate_viewport(ViewportNavigation::Orbit,{io.MouseDelta.x,io.MouseDelta.y});
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) e.navigate_viewport(ViewportNavigation::Pan,{io.MouseDelta.x,io.MouseDelta.y});
            if (hovered && !ImGuizmo::IsOver() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < io.MouseDragThreshold*io.MouseDragThreshold) {
                const auto id=worldView.pick(world,renderer,instances,{io.MousePos.x-pos.x,io.MousePos.y-pos.y});
                e.layout_.selectedObject=id;
                e.uiModel_.select_object(id);
                e.set_selected_asset({});
            }
        }
    }
    ImGui::GetWindowDrawList()->PopClipRect();
    if (!game && size.x>440*scale_) {
        const auto* hint=label(e,"右键 转向   中键 平移   左键 选择","RMB Look   MMB Pan   LMB Select");
        const auto extent=ImGui::CalcTextSize(hint);
        const ImVec2 text{pos.x+10*scale_,pos.y+size.y-extent.y-8*scale_};
        ImGui::GetWindowDrawList()->AddRectFilled({text.x-4*scale_,text.y-3*scale_},
            {text.x+extent.x+4*scale_,text.y+extent.y+3*scale_},IM_COL32(20,23,28,180),3*scale_);
        ImGui::GetWindowDrawList()->AddText(text,IM_COL32(220,225,235,255),hint);
    }
    ImGui::End();
}
}
#endif
