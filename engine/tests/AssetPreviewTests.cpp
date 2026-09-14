#include "shinkou/editor/AssetPreview.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace shinkou::editor;
    AssetPreviewCatalog catalog;

    const auto image = catalog.describe(FileEntry{"Textures/Hero.PNG", "Hero.PNG", false, 1024, 0});
    assert(image.kind == AssetPreviewKind::Image);
    assert(image.extension == ".png");
    assert(image.mimeType == "image/png");
    assert(image.supports(AssetPreviewCapability::Metadata));
    assert(!image.supports(AssetPreviewCapability::Thumbnail));
    assert(!image.supports(AssetPreviewCapability::Interactive));
    assert(image.statusMessage.find("provider") != std::string::npos);

    const auto model = catalog.describe(FileEntry{"Models/Ship.GLB", "Ship.GLB", false, 2048, 0});
    assert(model.kind == AssetPreviewKind::Model);
    assert(model.mimeType == "model/gltf-binary");
    assert(AssetPreviewCatalog::kind_name(model.kind) == "Mesh");

    const auto shader = catalog.describe(FileEntry{"Shaders/main.HLSL", "main.HLSL", false, 512, 0});
    assert(shader.kind == AssetPreviewKind::Shader);
    assert(shader.previewTitle == "Shader Resource");

    const auto folder = catalog.describe(FileEntry{"Textures", "Textures", true, 0, 0});
    assert(folder.kind == AssetPreviewKind::Folder);
    assert(folder.supports(AssetPreviewCapability::Metadata));
    assert(folder.supports(AssetPreviewCapability::Interactive));
    assert(!folder.supports(AssetPreviewCapability::Thumbnail));
    assert(folder.mimeType == "inode/directory");

    const auto unknown = catalog.describe(FileEntry{"cache/blob", "blob", false, 7, 0});
    assert(unknown.kind == AssetPreviewKind::Unknown);
    assert(unknown.mimeType == "application/octet-stream");

    const auto binary = catalog.describe(FileEntry{"cache/blob.dat", "blob.dat", false, 7, 0});
    assert(binary.kind == AssetPreviewKind::Binary);
    assert(binary.previewTitle == "File Details");

    const auto archive = catalog.describe(FileEntry{"Packages/game.7Z", "game.7Z", false, 4096, 0});
    assert(archive.kind == AssetPreviewKind::Archive);
    assert(archive.mimeType == "application/x-7z-compressed");

    std::cout << "Asset preview classification and capability contract passed\n";
    return 0;
}
