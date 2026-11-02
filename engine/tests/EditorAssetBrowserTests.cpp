#include "shinkou/editor/EditorAssetBrowser.h"
#include <cassert>
#include <iostream>
using namespace shinkou::editor;
int main() {
    std::vector<FileEntry> entries{{"assets/Z.txt","Z.txt"},{"assets/Folder","Folder",true},
        {"assets/Folder/note.TXT","note.TXT"},{"assets/Folder-extra.txt","Folder-extra.txt"},
        {"assets-other/note.txt","note.txt"},{"assets/a.txt","a.txt"},
        {"assets/Folder/Sub","Sub",true},{"assets/Folder/Sub/中文.txt",u8"中文.txt"}};
    EditorAssetBrowser model;model.directory="assets";model.refresh(entries,1);
    assert(model.rows().size()==4 && model.rows()[0].sourceIndex==1 && model.rows()[1].sourceIndex==5);
    const auto* cached=model.rows().data();model.refresh(entries,1);assert(cached==model.rows().data());
    model.toggle_directory("assets/Folder");model.refresh(entries,1);
    assert(model.rows().size()==6 && model.rows()[1].sourceIndex==6 && model.rows()[1].depth==1);
    model.toggle_directory("assets/Folder/Sub");model.refresh(entries,1);
    assert(model.rows().size()==7 && model.rows()[2].depth==2);
    model.view=EditorAssetView::SmallList;model.refresh(entries,1);assert(model.rows().size()==4);
    model.view=EditorAssetView::LargeIcons;model.refresh(entries,1);assert(model.rows().size()==4);
    model.filter="NOTE.txt";model.refresh(entries,1);
    assert(model.rows().size()==1 && model.rows()[0].sourceIndex==2); // Scope boundary excludes assets-other.
    model.filter=u8"中文";model.refresh(entries,1);assert(model.rows().size()==1 && model.rows()[0].sourceIndex==7);
    model.directory="assets/Folder";model.filter.clear();model.refresh(entries,1);assert(model.rows().size()==2);
    entries.push_back({"assets/Folder/new.lua","new.lua"});model.refresh(entries,2);assert(model.rows().size()==3);
    model.reset();model.refresh(entries,3);assert(model.rows().empty());
    std::cout<<"Shared asset browser scope, modes, expansion, filtering and cache passed\n";
}
