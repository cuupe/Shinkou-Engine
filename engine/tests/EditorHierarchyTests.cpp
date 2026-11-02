#include "shinkou/editor/EditorHierarchy.h"
#include "shinkou/World.h"
#include <cassert>
#include <optional>
#include <iostream>
using namespace shinkou;
using namespace shinkou::editor;
int main() {
    World world;
    auto& root=world.create_object("Root");auto& child=root.create_child("Child");
    auto& other=world.create_object("Other");
    EditorUiModel model;model.sync(world);
    const auto version=model.hierarchy_revision(),worldVersion=world.hierarchy_revision();
    for(int i=0;i<5;++i)model.sync(world);
    assert(model.hierarchy_revision()==version && world.hierarchy_revision()==worldVersion);
    model.select_object(child.id());model.sync(world);
    assert(model.hierarchy_revision()==version && model.object_roots()[0].children[0].selected);
    model.select_object(root.id());model.sync(world);
    assert(model.object_roots()[0].selected && !model.object_roots()[0].children[0].selected);
    root.set_name("Root");root.set_active(true);
    assert(world.hierarchy_revision()==worldVersion);
    child.set_name("Needle");model.sync(world);
    assert(model.hierarchy_revision()!=version && model.object_roots()[0].children[0].name=="Needle");
    EditorHierarchy rows;rows.refresh(model.object_roots(),model.hierarchy_revision(),false);
    assert(rows.rows().size()==3 && rows.rows()[1].depth==1);
    const auto rebuilds=rows.rebuilds();model.select_object(child.id());model.sync(world);
    rows.refresh(model.object_roots(),model.hierarchy_revision(),false);assert(rows.rebuilds()==rebuilds);
    rows.set_expanded(root.id(),false);rows.refresh(model.object_roots(),model.hierarchy_revision(),false);assert(rows.rows().size()==2);
    model.set_object_filter("needle");model.sync(world);rows.refresh(model.object_roots(),model.hierarchy_revision(),true);
    assert(rows.rows().size()==2 && rows.rows()[0].node->name=="Root");
    model.set_object_filter("");model.sync(world);rows.refresh(model.object_roots(),model.hierarchy_revision(),false);
    assert(rows.rows().size()==2); // Search preserves the user's collapsed state.
    const auto count=world.object_count();child.set_parent(&other);model.sync(world);
    assert(world.object_count()==count && model.object_roots()[1].children[0].id==child.id());
    other.set_active(false);model.sync(world);assert(!model.object_roots()[1].children[0].active);
    const auto types=world.component_types_revision();
    const auto typeCount=model.component_types().size();
    assert(world.register_component_type<components::TagComponent>("ExtraTag"));model.sync(world);
    assert(world.component_types_revision()>types && model.component_types().size()==typeCount+1);
    child.destroy();model.sync(world);assert(model.selected_object()==0);
    // Reusing a World's address must not reuse its cached hierarchy snapshot.
    std::optional<World> replacement;replacement.emplace();replacement->create_object("First world");
    model.sync(*replacement);replacement.reset();replacement.emplace();replacement->create_object("Second world");
    model.sync(*replacement);assert(model.object_roots().front().name=="Second world");
    std::cout<<"Hierarchy revisions, cached rows, selection, filter and World replacement passed\n";
}
