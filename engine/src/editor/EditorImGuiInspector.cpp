#include "EditorImGuiView.h"
#include "EditorGlyphs.h"
#include "shinkou/editor/EditorLayer.h"
#include "shinkou/World.h"
#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cstring>

namespace shinkou::editor {
void EditorImGuiView::field(EditorLayer& e,const EditorInspectorField& f) {
    ImGui::PushID(f.id.c_str());
    ImGui::TableNextRow();ImGui::TableNextColumn();ImGui::AlignTextToFramePadding();
    const char* name=f.label.c_str();
    if(f.id=="name")name=label(e,"名称","Name");
    if(f.id=="active")name=label(e,"启用","Active");
    if(f.componentName=="Transform") {
        if(f.id.find(":position")!=std::string::npos)name=label(e,"位置","Position");
        if(f.id.find(":rotation")!=std::string::npos)name=label(e,"旋转 °","Rotation °");
        if(f.id.find(":scale")!=std::string::npos)name=label(e,"缩放","Scale");
    }
    ImGui::TextUnformatted(name);ImGui::TableNextColumn();
    // Build settings share the row presentation, but have a separate document.
    if(f.id.rfind("build-profile.",0)==0) {
        auto& buffer=editBuffers_[f.id];
        if(activeProperty_!=f.id){std::strncpy(buffer.data(),f.value.c_str(),buffer.size()-1);buffer.back()=0;}
        ImGui::SetNextItemWidth(-1);
        const bool enter=ImGui::InputText("##value",buffer.data(),buffer.size(),ImGuiInputTextFlags_EnterReturnsTrue);
        if(ImGui::IsItemActivated())activeProperty_=f.id;
        if(enter || ImGui::IsItemDeactivatedAfterEdit())e.edit_build_profile_field(f.id,buffer.data());
        if(ImGui::IsItemDeactivated())activeProperty_.clear();
        region(e,"field:"+f.id);ImGui::PopID();return;
    }
    auto [it,inserted]=propertyDrafts_.try_emplace(f.id,f.data);
    if(inserted || activeProperty_!=f.id)it->second=f.data;
    auto& draft=it->second;
    ImGui::BeginDisabled(!f.editable);
    const auto lifecycle=[&](bool changed,const PropertyValue& value) {
        if(ImGui::IsItemActivated()) {e.begin_inspector_edit(f.id);activeProperty_=f.id;}
        if(changed) e.edit_inspector_value(f.id,value);
        if(ImGui::IsItemDeactivated()) {
            e.finish_inspector_edit(ImGui::IsKeyPressed(ImGuiKey_Escape));
            if(activeProperty_==f.id)activeProperty_.clear();
        }
    };
    const auto vector=[&](float* values,int count,const auto& pack,const char* group="") {
        ImGui::PushID(group);
        const float available=ImGui::GetContentRegionAvail().x;
        const bool stacked=available<count*48*scale_;
        const float gap=3*scale_,width=stacked?available:(available-gap*(count-1))/count;
        const char* axes[]={"X","Y","Z","W"};
        const ImVec4 colors[]={{0.74f,0.27f,0.28f,1},{0.24f,0.60f,0.38f,1},{0.25f,0.48f,0.78f,1},{0.55f,0.48f,0.68f,1}};
        ImGui::BeginGroup();
        for(int axis=0;axis<count;++axis) {
            if(axis && !stacked)ImGui::SameLine(0,gap);
            ImGui::PushID(axis);ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Button,colors[axis]);ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,1,1,1));
            const bool reset=ImGui::Button(axes[axis],{17*scale_,ImGui::GetFrameHeight()});
            if(ImGui::IsItemHovered())ImGui::SetTooltip("%s %s",label(e,"重置","Reset"),axes[axis]);
            ImGui::PopStyleColor(2);
            if(reset) {e.finish_inspector_edit();values[axis]=(f.id.find(":scale")!=std::string::npos || std::string_view(group)=="scale" || (count==4 && axis==3)) ? 1.0f:0.0f;e.edit_inspector_value(f.id,pack(values));}
            ImGui::SameLine(0,0);ImGui::SetNextItemWidth(std::max(18*scale_,width-17*scale_));
            const float speed=f.presentation==PropertyPresentation::EulerDegrees ? 0.25f : f.step>0 ? static_cast<float>(f.step):0.025f;
            const bool changed=ImGui::DragFloat("##axis",&values[axis],speed,0,0,"%.3f",ImGuiSliderFlags_NoRoundToFormat);
            const auto value=pack(values);if(changed)draft=value;
            const auto key="field:"+f.id+(*group?":"+std::string(group):"")+":"+axes[axis];
            region(e,key);
            if(f.componentName=="Transform") {
                const auto colon=f.id.find(':');alias_region("transform:"+f.id.substr(colon+1)+":"+axes[axis],key);
            }
            lifecycle(changed,value);ImGui::EndGroup();ImGui::PopID();
        }
        ImGui::EndGroup();
        ImGui::PopID();
    };
    ImGui::SetNextItemWidth(-1);
    if(auto* value=std::get_if<math::Transform>(&draft)) {
        auto pose=*value;
        ImGui::TextDisabled("%s",label(e,"位置","Position"));
        float position[]={pose.position.x,pose.position.y,pose.position.z};
        vector(position,3,[&](const float* v)->PropertyValue{pose.position={v[0],v[1],v[2]};return pose;},"position");
        ImGui::TextDisabled("%s",label(e,"旋转 °","Rotation °"));
        const auto angles=math::ToEulerXYZ(pose.rotation)*math::Degrees(1);
        float rotation[]={angles.x,angles.y,angles.z};
        vector(rotation,3,[&](const float* v)->PropertyValue{pose.rotation=math::FromEulerXYZ(math::Vec3{v[0],v[1],v[2]}*math::Radians(1));return pose;},"rotation");
        ImGui::TextDisabled("%s",label(e,"缩放","Scale"));
        float scale[]={pose.scale.x,pose.scale.y,pose.scale.z};
        vector(scale,3,[&](const float* v)->PropertyValue{pose.scale={v[0],v[1],v[2]};return pose;},"scale");
    } else if(auto* value=std::get_if<math::Vec3>(&draft)) {
        float axes[]={value->x,value->y,value->z};vector(axes,3,[](const float* v)->PropertyValue{return math::Vec3{v[0],v[1],v[2]};});
    } else if(auto* value=std::get_if<math::Vec2>(&draft)) {
        float axes[]={value->x,value->y};vector(axes,2,[](const float* v)->PropertyValue{return math::Vec2{v[0],v[1]};});
    } else if(auto* value=std::get_if<math::Quat>(&draft)) {
        float axes[]={value->x,value->y,value->z,value->w};vector(axes,4,[](const float* v)->PropertyValue{return math::Quat{v[0],v[1],v[2],v[3]};});
    } else if(auto* value=std::get_if<bool>(&draft)) {
        const bool changed=ImGui::Checkbox("##value",value);lifecycle(changed,draft);
    } else if(f.choices && !f.choices->empty()) {
        const char* selected=f.value.c_str();for(const auto& choice:*f.choices)if(choice.value==f.value)selected=choice.label.c_str();
        if(ImGui::BeginCombo("##value",selected)) {
            for(const auto& choice:*f.choices)if(ImGui::Selectable(choice.label.c_str(),choice.value==f.value)) {
                PropertyValue typed;
                if(parse_property_text(f.type,choice.value,typed))e.edit_inspector_value(f.id,typed);
            }
            ImGui::EndCombo();
        }
    } else if(auto* value=std::get_if<double>(&draft)) {
        const double minimum=f.minimum,maximum=f.maximum;
        const bool changed=ImGui::DragScalar("##value",ImGuiDataType_Double,value,static_cast<float>(f.step>0?f.step:0.05),
            maximum>minimum?&minimum:nullptr,maximum>minimum?&maximum:nullptr,"%.3f",ImGuiSliderFlags_AlwaysClamp);
        lifecycle(changed,draft);
    } else if(auto* value=std::get_if<std::int64_t>(&draft)) {
        const bool changed=ImGui::DragScalar("##value",ImGuiDataType_S64,value,1);lifecycle(changed,draft);
    } else if(auto* value=std::get_if<std::uint64_t>(&draft)) {
        const bool changed=ImGui::DragScalar("##value",ImGuiDataType_U64,value,1);lifecycle(changed,draft);
    } else if(std::holds_alternative<std::string>(draft)) {
        auto& buffer=editBuffers_[f.id];
        if(activeProperty_!=f.id){std::strncpy(buffer.data(),std::get<std::string>(draft).c_str(),buffer.size()-1);buffer.back()=0;}
        const bool enter=ImGui::InputText("##value",buffer.data(),buffer.size(),ImGuiInputTextFlags_EnterReturnsTrue);
        lifecycle(enter || ImGui::IsItemEdited() || ImGui::IsItemDeactivatedAfterEdit(),std::string(buffer.data()));
    } else ImGui::TextWrapped("%s",f.value.c_str());
    region(e,"field:"+f.id);
    ImGui::EndDisabled();ImGui::PopID();
}

void EditorImGuiView::inspector(EditorLayer& e,render::Renderer& renderer,World& world) {
    if(!begin_panel(e,"inspector"))return;
    const auto selected=e.uiModel_.selected_object();
    if(inspectedObject_!=selected) {
        e.finish_inspector_edit();activeProperty_.clear();propertyDrafts_.clear();editBuffers_.clear();
        e.uiModel_.sync(world);e.set_selected_asset({});inspectedObject_=selected;
    }
    ImGui::PushID(std::to_string(selected).c_str());
    if(!e.selectedAsset_.empty())preview(e,renderer);
    else if(auto* object=world.find_object(selected)) {
        const auto fields=e.uiModel_.inspector_fields();
        ImGui::TextDisabled("%s",glyph::Object);ImGui::SameLine();
        ImGui::TextUnformatted(std::string(object->name()).c_str());
        if(ImGui::BeginTable("object-properties",2,ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Property",ImGuiTableColumnFlags_WidthFixed,60*scale_);ImGui::TableSetupColumn("Value");
            for(const auto& f:fields)if(f.componentId==0)field(e,f);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        for(std::size_t i=0;i<fields.size();) {
            if(!fields[i].componentId){++i;continue;}
            const auto id=fields[i].componentId;const auto name=fields[i].componentName;
            auto end=i+1;while(end<fields.size()&&fields[end].componentId==id)++end;
            ImGui::PushID(std::to_string(id).c_str());
            const std::string heading=std::string(glyph::Component)+"  "+name;
            const bool expanded=ImGui::CollapsingHeader(heading.c_str(),ImGuiTreeNodeFlags_DefaultOpen|ImGuiTreeNodeFlags_AllowOverlap);
            region(e,"component:"+name);
            if(ImGui::BeginPopupContextItem("component-menu")) {
                if(name=="Transform" && ImGui::MenuItem(label(e,"重置变换","Reset transform"))) {
                    if(auto* transform=object->get_component<components::TransformComponent>())
                        e.edit_inspector_value(std::to_string(transform->id())+":__local",math::Transform{});
                }
                ImGui::EndPopup();
            }
            if(name!="Transform") {
                ImGui::SameLine(ImGui::GetWindowContentRegionMax().x-ImGui::GetFrameHeight());
                bool enabled=std::get<bool>(fields[i].data);
                if(ImGui::Checkbox("##enabled",&enabled))e.edit_inspector_value(fields[i].id,enabled);
                region(e,"field:"+fields[i].id);
            }
            if(expanded && ImGui::BeginTable("component-properties",2,ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Property",ImGuiTableColumnFlags_WidthFixed,58*scale_);ImGui::TableSetupColumn("Value");
                for(auto row=i+1;row<end;++row)field(e,fields[row]);
                ImGui::EndTable();
            }
            ImGui::PopID();i=end;ImGui::Spacing();
        }
        if(button(e,"+ 添加组件","+ Add component","inspector.add-component"))ImGui::OpenPopup("components");
        if(ImGui::BeginPopup("components")) {
            for(const auto& name:e.uiModel_.component_types())if(ImGui::Selectable(name.c_str()))e.dispatch_command(EditorCommand::AddComponent,name,world);
            ImGui::EndPopup();
        }
    } else ImGui::TextDisabled("%s",label(e,"选择对象以查看属性","Select an object to inspect"));
    ImGui::PopID();ImGui::End();
}
}
#endif
