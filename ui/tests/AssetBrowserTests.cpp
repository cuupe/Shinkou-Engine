#include "shinkou/uikit/AssetBrowser.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace shinkou::uikit;

int main() {
    const auto root = std::filesystem::temp_directory_path() / "shinkou_asset_browser_tests";
    std::error_code cleanupError;
    std::filesystem::remove_all(root, cleanupError);
    std::filesystem::create_directories(root / "Textures");
    {
        std::ofstream(root / "Textures" / "hero.PNG") << "image";
        std::ofstream(root / "notes.txt") << "hello";
        std::ofstream(root / ".hidden.asset") << "hidden";
    }

    NativeAssetFileSystem native;
    AssetBrowser browser(native);
    std::string error;
    assert(browser.set_root_directory(root, &error));
    assert(browser.entries().size() == 3);
    assert(identify_asset_file_type("hero.PNG") == AssetFileType::Image);
    assert(identify_asset_file_type("clip.MP4") == AssetFileType::Video);
    assert(identify_asset_file_type("sound.wav") == AssetFileType::Audio);

    browser.set_search_query("note");
    assert(browser.visible_entries().size() == 1);
    browser.set_search_query("");
    browser.set_type_filter({AssetFileType::Image});
    assert(browser.visible_entries().size() == 1); // directory navigation remains visible
    browser.state().includeDirectories = false;
    assert(browser.visible_entries().size() == 0);
    browser.state().includeDirectories = true;
    browser.state().showHiddenFiles = true;
    browser.clear_type_filter();
    assert(browser.visible_entries().size() == 3);

    assert(browser.set_current_directory("Textures", &error));
    std::vector<std::uint8_t> bytes;
    assert(browser.read_file("hero.PNG", bytes, &error));
    assert(bytes.size() == 5);
    assert(browser.write_file("new.txt", {1, 2, 3}, &error));
    assert(browser.rename_file("new.txt", "renamed.txt", &error));
    assert(browser.create_directory("Nested", &error));
    assert(std::filesystem::exists(root / "Textures" / "Nested"));

    const auto stateBefore = browser.state();
    browser.state().searchQuery = "renamed";
    browser.state().view = AssetBrowserView::Columns;
    browser.state().typeFilter = {AssetFileType::Text};
    browser.state().selectedPaths = {"renamed.txt"};
    const auto xml = browser.to_xml();
    AssetBrowser restored(native);
    assert(restored.from_xml(xml, &error));
    assert(restored.state().view == AssetBrowserView::Columns);
    assert(restored.state().searchQuery == "renamed");
    assert(restored.state().selectedPaths.size() == 1);
    assert(restored.state().typeFilter.front() == AssetFileType::Text);

    assert(browser.write_file("changed.txt", {9}, &error));
    const auto added = browser.poll_changes(&error); // write_file already refreshed; no duplicate event expected
    assert(added.empty());
    std::filesystem::remove(root / "Textures" / "changed.txt");
    const auto removed = browser.poll_changes(&error);
    assert(removed.size() == 1 && removed.front().type == AssetChangeType::Removed);

    const auto beforeBad = restored.to_xml();
    assert(!restored.from_xml("<asset-browser-state schema=\"wrong\" version=\"1\"/>", &error));
    assert(restored.to_xml() == beforeBad);

    std::filesystem::remove_all(root, cleanupError);
    std::cout << "Asset browser scanning, filters, native file operations, changes, and XML passed\n";
    return 0;
}
