#include "shinkou/editor/FileHistory.h"
#include "shinkou/reflection/Reflection.h"
#include "shinkou/reflection/Serialization.h"

#include <utility>

namespace shinkou::reflection {
SHINKOU_REFLECT_TYPE(::shinkou::editor::FileHistoryEntry, "editor.FileHistoryEntry");
SHINKOU_REFLECT_TYPE(::shinkou::editor::FileHistoryDocument, "editor.FileHistoryDocument");
SHINKOU_REFLECT_TYPE(std::vector<::shinkou::editor::FileHistoryEntry>, "editor.FileHistoryEntries");
}

namespace shinkou::editor {
namespace {
void register_file_history(reflection::TypeRegistry& registry) {
    registry.register_type(reflection::TypeBuilder<FileHistoryEntry>("editor.FileHistoryEntry")
        .field("kind", &FileHistoryEntry::kind)
        .field("sourcePath", &FileHistoryEntry::sourcePath)
        .field("destinationPath", &FileHistoryEntry::destinationPath)
        .field("recyclePath", &FileHistoryEntry::recyclePath)
        .field("selectedAssetBefore", &FileHistoryEntry::selectedAssetBefore)
        .field("assetDirectoryBefore", &FileHistoryEntry::assetDirectoryBefore)
        .take());
    registry.register_type(reflection::make_vector_descriptor<FileHistoryEntry>("editor.FileHistoryEntries"));
    registry.register_type(reflection::TypeBuilder<FileHistoryDocument>("editor.FileHistoryDocument")
        .field("version", &FileHistoryDocument::version)
        .field("entries", &FileHistoryDocument::entries)
        .take());
}
}

bool FileHistoryDocument::to_json(std::string& json, std::string& error) const {
    reflection::TypeRegistry registry;
    register_file_history(registry);
    const auto result = reflection::serialize_json(
        registry, reflection::type_id<FileHistoryDocument>(), this, json, {true});
    error = result.message;
    return static_cast<bool>(result);
}

bool FileHistoryDocument::from_json(std::string_view json, FileHistoryDocument& document,
                                    std::string& error) {
    if (json.size() > 1u * 1024u * 1024u) {
        error = "file history exceeds 1 MiB";
        return false;
    }
    reflection::TypeRegistry registry;
    register_file_history(registry);
    FileHistoryDocument next;
    const auto result = reflection::deserialize_json(
        registry, reflection::type_id<FileHistoryDocument>(), json, &next);
    error = result.message;
    if (!result) return false;
    if (next.version != 1 || next.entries.size() > 64) {
        error = "unsupported or oversized file history";
        return false;
    }
    document = std::move(next);
    return true;
}
} // namespace shinkou::editor
