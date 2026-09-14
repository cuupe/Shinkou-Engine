#include "shinkou/editor/EditorAssetIndex.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace shinkou::editor;
    std::vector<FileEntry> entries{
        {"assets/main.cpp", "main.cpp", false, 42, 7},
        {"assets/Textures", "Textures", true, 0, 8},
        {"assets/music.wav", "music.wav", false, 128, 9},
    };
    const auto snapshot = EditorAssetIndex::build(entries, 17);
    assert(snapshot && snapshot->revision() == 17 && snapshot->size() == 3);
    assert(snapshot->find("assets/main.cpp") != nullptr);
    assert(snapshot->find("assets/main.cpp")->descriptor.kind == AssetPreviewKind::Text);
    assert(snapshot->find("assets/Textures")->descriptor.kind == AssetPreviewKind::Folder);
    assert(snapshot->find("assets/music.wav")->descriptor.kind == AssetPreviewKind::Audio);
    assert(snapshot->find("assets/missing.txt") == nullptr);

    entries[0].name = "mutated-after-build";
    assert(snapshot->find("assets/main.cpp")->descriptor.displayName == "main.cpp");
    std::cout << "Immutable editor asset index snapshot passed\n";
    return 0;
}
