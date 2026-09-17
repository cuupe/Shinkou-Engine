#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shinkou::editor {

struct FileHistoryEntry {
    std::string kind;
    std::string sourcePath;
    std::string destinationPath;
    std::string recyclePath;
    std::string selectedAssetBefore;
    std::string assetDirectoryBefore;
};

struct FileHistoryDocument {
    std::uint32_t version{1};
    std::vector<FileHistoryEntry> entries;

    bool to_json(std::string& json, std::string& error) const;
    static bool from_json(std::string_view json, FileHistoryDocument& document, std::string& error);
};

} // namespace shinkou::editor
