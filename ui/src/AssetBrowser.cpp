#include "shinkou/uikit/AssetBrowser.h"

#include "shinkou/uikit/Xml.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace shinkou::uikit {
namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::string path_string(const std::filesystem::path& path) {
    return path.generic_string();
}

std::string lower_ascii(std::string value) {
    for (char& character : value) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

bool finite(float value) noexcept { return std::isfinite(value); }

std::int64_t modified_time(const std::filesystem::file_time_type& value) noexcept {
    const auto fileNow = std::filesystem::file_time_type::clock::now();
    const auto systemNow = std::chrono::system_clock::now();
    const auto systemTime = systemNow + (value - fileNow);
    return std::chrono::duration_cast<std::chrono::seconds>(systemTime.time_since_epoch()).count();
}

std::string filesystem_error_message(std::string_view operation, const std::filesystem::path& path,
                                     const std::error_code& code) {
    std::string result(operation);
    result += " '" + path_string(path) + "'";
    if (code) result += ": " + code.message();
    return result;
}

std::string extension_of(const std::filesystem::path& path) {
    return lower_ascii(path.extension().string());
}

std::string mime_type_for(AssetFileType type, std::string extension) {
    switch (type) {
    case AssetFileType::Image:
        if (extension == ".png") return "image/png";
        if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
        if (extension == ".webp") return "image/webp";
        if (extension == ".svg") return "image/svg+xml";
        return "image/*";
    case AssetFileType::Video: return "video/*";
    case AssetFileType::Audio: return "audio/*";
    case AssetFileType::Font: return "font/*";
    case AssetFileType::Text:
    case AssetFileType::Shader:
    case AssetFileType::Script: return "text/plain";
    case AssetFileType::Model: return "model/*";
    default: return {};
    }
}

bool hidden_name(const std::string& name) noexcept {
    return !name.empty() && name.front() == '.' && name != "." && name != "..";
}

bool path_is_inside(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const auto relative = candidate.lexically_relative(root);
    if (relative.empty()) return root == candidate;
    for (const auto& part : relative) if (part == "..") return false;
    return true;
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return (error ? path : absolute).lexically_normal();
}

template <typename T>
bool parse_number(const std::string& value, T& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0' || !std::isfinite(parsed)) return false;
    if (parsed < static_cast<double>(std::numeric_limits<T>::lowest()) || parsed > static_cast<double>(std::numeric_limits<T>::max())) return false;
    output = static_cast<T>(parsed);
    return true;
}

bool parse_bool(const std::string& value, bool& output) {
    if (value == "true") { output = true; return true; }
    if (value == "false") { output = false; return true; }
    return false;
}

bool parse_state_node(const XmlNode& root, AssetBrowserState& state, std::string* error) {
    if (root.name != "asset-browser-state" || root.attribute("schema") != kAssetBrowserStateSchema) {
        set_error(error, "unsupported asset browser XML schema");
        return false;
    }
    std::uint32_t version = 0;
    if (!parse_number(root.attribute("version"), version) || version != kAssetBrowserStateVersion) {
        set_error(error, "unsupported asset browser XML version");
        return false;
    }

    AssetBrowserState parsed;
    parsed.rootDirectory = root.attribute("root");
    parsed.currentDirectory = root.attribute("current");
    parsed.searchQuery = root.attribute("search");
    if (!parse_asset_browser_view(root.attribute("view"), parsed.view) ||
        !parse_number(root.attribute("thumbnailSize"), parsed.thumbnailSize) ||
        !parse_bool(root.attribute("showHidden"), parsed.showHiddenFiles) ||
        !parse_bool(root.attribute("includeDirectories"), parsed.includeDirectories) ||
        !parse_bool(root.attribute("recursive"), parsed.recursiveScan)) {
        set_error(error, "invalid asset browser XML state value");
        return false;
    }

    if (const auto* filter = root.child("type-filter")) {
        for (const auto* child : filter->children_named("type")) {
            AssetFileType type = AssetFileType::Unknown;
            if (!parse_asset_file_type(child->attribute("value"), type)) {
                set_error(error, "invalid asset browser type filter");
                return false;
            }
            if (std::find(parsed.typeFilter.begin(), parsed.typeFilter.end(), type) == parsed.typeFilter.end()) {
                parsed.typeFilter.push_back(type);
            }
        }
    }
    if (const auto* selection = root.child("selection")) {
        for (const auto* child : selection->children_named("path")) parsed.selectedPaths.emplace_back(child->attribute("value"));
    }
    if (!parsed.valid(error)) return false;
    state = std::move(parsed);
    return true;
}

void collect_records(const IAssetFileSystem& fileSystem, const std::filesystem::path& directory,
                     bool recursive, std::vector<AssetFileRecord>& output, std::string* error) {
    std::vector<AssetFileRecord> pending;
    if (!fileSystem.list_directory(directory, pending, error)) return;
    std::vector<std::filesystem::path> directories;
    for (auto& record : pending) {
        output.push_back(std::move(record));
        if (recursive && output.back().isDirectory) directories.push_back(output.back().path);
    }
    if (!recursive) return;

    std::unordered_set<std::string> visited;
    visited.insert(normalized_path(directory).generic_string());
    for (std::size_t index = 0; index < directories.size(); ++index) {
        const auto directoryPath = normalized_path(directories[index]);
        if (!visited.insert(directoryPath.generic_string()).second) continue;
        pending.clear();
        if (!fileSystem.list_directory(directoryPath, pending, error)) return;
        for (auto& record : pending) {
            output.push_back(std::move(record));
            if (output.back().isDirectory) directories.push_back(output.back().path);
        }
    }
}

} // namespace

const char* asset_file_type_name(AssetFileType type) noexcept {
    switch (type) {
    case AssetFileType::Directory: return "directory";
    case AssetFileType::Image: return "image";
    case AssetFileType::Video: return "video";
    case AssetFileType::Audio: return "audio";
    case AssetFileType::Font: return "font";
    case AssetFileType::Text: return "text";
    case AssetFileType::Model: return "model";
    case AssetFileType::Material: return "material";
    case AssetFileType::Shader: return "shader";
    case AssetFileType::Scene: return "scene";
    case AssetFileType::Script: return "script";
    case AssetFileType::Other: return "other";
    default: return "unknown";
    }
}

bool parse_asset_file_type(std::string_view value, AssetFileType& type) noexcept {
    constexpr AssetFileType values[] = {AssetFileType::Unknown, AssetFileType::Directory, AssetFileType::Image,
        AssetFileType::Video, AssetFileType::Audio, AssetFileType::Font, AssetFileType::Text, AssetFileType::Model,
        AssetFileType::Material, AssetFileType::Shader, AssetFileType::Scene, AssetFileType::Script, AssetFileType::Other};
    for (const auto candidate : values) {
        if (value == asset_file_type_name(candidate)) { type = candidate; return true; }
    }
    return false;
}

AssetFileType identify_asset_file_type(const std::filesystem::path& path, bool isDirectory) noexcept {
    if (isDirectory) return AssetFileType::Directory;
    const auto extension = extension_of(path);
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" || extension == ".gif" || extension == ".tga" || extension == ".webp" || extension == ".svg") return AssetFileType::Image;
    if (extension == ".mp4" || extension == ".mov" || extension == ".avi" || extension == ".mkv" || extension == ".webm") return AssetFileType::Video;
    if (extension == ".wav" || extension == ".mp3" || extension == ".ogg" || extension == ".flac" || extension == ".aac") return AssetFileType::Audio;
    if (extension == ".ttf" || extension == ".otf" || extension == ".woff" || extension == ".woff2") return AssetFileType::Font;
    if (extension == ".txt" || extension == ".md" || extension == ".json" || extension == ".xml" || extension == ".yaml" || extension == ".yml" || extension == ".ini" || extension == ".csv") return AssetFileType::Text;
    if (extension == ".fbx" || extension == ".obj" || extension == ".gltf" || extension == ".glb") return AssetFileType::Model;
    if (extension == ".mat" || extension == ".material") return AssetFileType::Material;
    if (extension == ".hlsl" || extension == ".glsl" || extension == ".shader") return AssetFileType::Shader;
    if (extension == ".scene" || extension == ".level") return AssetFileType::Scene;
    if (extension == ".lua" || extension == ".py" || extension == ".cs" || extension == ".cpp" || extension == ".h") return AssetFileType::Script;
    return extension.empty() ? AssetFileType::Unknown : AssetFileType::Other;
}

bool NativeAssetFileSystem::list_directory(const std::filesystem::path& directory,
                                           std::vector<AssetFileRecord>& records, std::string* error) const {
    records.clear();
    std::error_code statusError;
    if (!std::filesystem::is_directory(directory, statusError)) {
        set_error(error, filesystem_error_message("cannot list directory", directory, statusError));
        return false;
    }
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator(directory, iteratorError);
    const std::filesystem::directory_iterator end;
    if (iteratorError) {
        set_error(error, filesystem_error_message("cannot enumerate directory", directory, iteratorError));
        return false;
    }
    while (iterator != end) {
        const auto& entry = *iterator;
        std::error_code typeError;
        const bool directoryEntry = entry.is_directory(typeError);
        if (typeError) {
            set_error(error, filesystem_error_message("cannot inspect entry", entry.path(), typeError));
            return false;
        }
        AssetFileRecord record;
        record.path = entry.path();
        record.isDirectory = directoryEntry;
        if (!directoryEntry) {
            std::error_code sizeError;
            record.size = entry.is_regular_file(sizeError) ? entry.file_size(sizeError) : 0;
            if (sizeError) record.size = 0;
        }
        std::error_code timeError;
        const auto time = entry.last_write_time(timeError);
        record.modifiedTime = timeError ? 0 : modified_time(time);
        records.push_back(std::move(record));
        iterator.increment(iteratorError);
        if (iteratorError) {
            set_error(error, filesystem_error_message("cannot enumerate directory", directory, iteratorError));
            return false;
        }
    }
    return true;
}

bool NativeAssetFileSystem::read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
                                      std::string* error) const {
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(path, sizeError);
    if (sizeError || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        set_error(error, filesystem_error_message("cannot read file", path, sizeError));
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { set_error(error, "cannot open file for reading '" + path_string(path) + "'"); return false; }
    bytes.resize(static_cast<std::size_t>(size));
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !input.eof()) { set_error(error, "cannot read file '" + path_string(path) + "'"); return false; }
    return true;
}

bool NativeAssetFileSystem::write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                                       std::string* error) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { set_error(error, "cannot open file for writing '" + path_string(path) + "'"); return false; }
    if (!bytes.empty()) output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) { set_error(error, "cannot write file '" + path_string(path) + "'"); return false; }
    return true;
}

bool NativeAssetFileSystem::rename_file(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error) {
    std::error_code filesystemError;
    std::filesystem::rename(from, to, filesystemError);
    if (filesystemError) { set_error(error, filesystem_error_message("cannot rename", from, filesystemError)); return false; }
    return true;
}

bool NativeAssetFileSystem::create_directory(const std::filesystem::path& path, std::string* error) {
    std::error_code filesystemError;
    if (!std::filesystem::create_directories(path, filesystemError) && filesystemError) {
        set_error(error, filesystem_error_message("cannot create directory", path, filesystemError));
        return false;
    }
    return true;
}

bool AssetBrowserState::valid(std::string* error) const {
    if (rootDirectory.empty() != currentDirectory.empty()) {
        set_error(error, "asset browser root and current directory must be set together"); return false;
    }
    if (!finite(thumbnailSize) || thumbnailSize < 16.0f || thumbnailSize > 1024.0f) {
        set_error(error, "asset browser thumbnail size is invalid"); return false;
    }
    for (const auto type : typeFilter) if (type == AssetFileType::Unknown) {
        set_error(error, "asset browser cannot filter by unknown type"); return false;
    }
    return true;
}

const char* asset_browser_view_name(AssetBrowserView view) noexcept {
    switch (view) {
    case AssetBrowserView::List: return "list";
    case AssetBrowserView::Columns: return "columns";
    default: return "grid";
    }
}

bool parse_asset_browser_view(std::string_view value, AssetBrowserView& view) noexcept {
    if (value == "grid") { view = AssetBrowserView::Grid; return true; }
    if (value == "list") { view = AssetBrowserView::List; return true; }
    if (value == "columns") { view = AssetBrowserView::Columns; return true; }
    return false;
}

std::string serialize_xml(const AssetBrowserState& state, bool) {
    XmlNode root;
    root.name = "asset-browser-state";
    root.attributes["schema"] = std::string(kAssetBrowserStateSchema);
    root.attributes["version"] = std::to_string(kAssetBrowserStateVersion);
    root.attributes["root"] = path_string(state.rootDirectory);
    root.attributes["current"] = path_string(state.currentDirectory);
    root.attributes["search"] = state.searchQuery;
    root.attributes["view"] = asset_browser_view_name(state.view);
    root.attributes["thumbnailSize"] = std::to_string(state.thumbnailSize);
    root.attributes["showHidden"] = state.showHiddenFiles ? "true" : "false";
    root.attributes["includeDirectories"] = state.includeDirectories ? "true" : "false";
    root.attributes["recursive"] = state.recursiveScan ? "true" : "false";
    XmlNode filter;
    filter.name = "type-filter";
    for (const auto type : state.typeFilter) {
        XmlNode child;
        child.name = "type";
        child.attributes["value"] = asset_file_type_name(type);
        filter.children.push_back(std::move(child));
    }
    root.children.push_back(std::move(filter));
    XmlNode selection;
    selection.name = "selection";
    for (const auto& path : state.selectedPaths) {
        XmlNode child;
        child.name = "path";
        child.attributes["value"] = path_string(path);
        selection.children.push_back(std::move(child));
    }
    root.children.push_back(std::move(selection));
    XmlDocument document;
    document.set_root(std::move(root));
    return document.serialize();
}

bool deserialize_xml(std::string_view xml, AssetBrowserState& state, std::string* error) {
    if (xml.size() > 4u * 1024u * 1024u) { set_error(error, "asset browser XML exceeds 4 MiB"); return false; }
    XmlDocument document;
    std::string source(xml);
    if (!document.parse(source, error)) return false;
    const auto* root = document.root();
    if (!root) { set_error(error, "asset browser XML has no root"); return false; }
    return parse_state_node(*root, state, error);
}

AssetEntry AssetBrowser::make_entry(const AssetFileRecord& record) const {
    AssetEntry entry;
    entry.path = normalized_path(record.path);
    entry.name = entry.path.filename().string();
    entry.relativePath = path_string(entry.path.lexically_relative(normalized_path(state_.rootDirectory)));
    entry.isDirectory = record.isDirectory;
    entry.type = identify_asset_file_type(entry.path, record.isDirectory);
    entry.size = record.size;
    entry.modifiedTime = record.modifiedTime;
    entry.metadata.mimeType = mime_type_for(entry.type, extension_of(entry.path));
    entry.thumbnail.sourceUri = std::string("file://") + path_string(entry.path);
    std::ostringstream cache;
    cache << entry.relativePath << ':' << entry.size << ':' << entry.modifiedTime;
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char character : cache.str()) { hash ^= character; hash *= 1099511628211ull; }
    entry.thumbnail.cacheKey = std::to_string(hash);
    return entry;
}

bool AssetBrowser::refresh_records(const std::vector<AssetFileRecord>& records, std::vector<AssetChange>* changes) {
    std::vector<AssetEntry> refreshed;
    refreshed.reserve(records.size());
    for (const auto& record : records) refreshed.push_back(make_entry(record));
    std::sort(refreshed.begin(), refreshed.end(), [](const AssetEntry& left, const AssetEntry& right) {
        const auto leftName = lower_ascii(left.relativePath);
        const auto rightName = lower_ascii(right.relativePath);
        return leftName == rightName ? left.relativePath < right.relativePath : leftName < rightName;
    });
    if (changes) {
        std::unordered_map<std::string, const AssetEntry*> previous;
        for (const auto& entry : entries_) previous.emplace(entry.relativePath, &entry);
        std::unordered_set<std::string> seen;
        for (auto& entry : refreshed) {
            seen.insert(entry.relativePath);
            const auto found = previous.find(entry.relativePath);
            if (found == previous.end()) changes->push_back({AssetChangeType::Added, entry});
            else if (found->second->size != entry.size || found->second->modifiedTime != entry.modifiedTime || found->second->type != entry.type || found->second->isDirectory != entry.isDirectory) {
                entry.thumbnail = found->second->thumbnail;
                changes->push_back({AssetChangeType::Modified, entry});
            } else {
                entry.thumbnail = found->second->thumbnail;
                entry.metadata = found->second->metadata;
            }
        }
        for (const auto& oldEntry : entries_) if (!seen.count(oldEntry.relativePath)) changes->push_back({AssetChangeType::Removed, oldEntry});
    }
    entries_ = std::move(refreshed);
    return true;
}

bool AssetBrowser::resolve_inside_root(const std::filesystem::path& path, std::filesystem::path& resolved, std::string* error) const {
    if (state_.rootDirectory.empty()) { set_error(error, "asset browser root directory is not set"); return false; }
    const auto base = path.is_absolute() ? path : (state_.currentDirectory.empty() ? state_.rootDirectory : state_.currentDirectory) / path;
    resolved = normalized_path(base);
    const auto root = normalized_path(state_.rootDirectory);
    if (!path_is_inside(root, resolved)) { set_error(error, "asset path escapes the browser root"); return false; }
    return true;
}

bool AssetBrowser::set_root_directory(const std::filesystem::path& root, std::string* error) {
    const auto normalized = normalized_path(root);
    std::vector<AssetFileRecord> records;
    std::string localError;
    if (!fileSystem_->list_directory(normalized, records, &localError)) { set_error(error, localError); lastError_ = localError; return false; }
    if (state_.recursiveScan) {
        records.clear();
        collect_records(*fileSystem_, normalized, true, records, &localError);
        if (!localError.empty()) { set_error(error, localError); lastError_ = localError; return false; }
    }
    state_.rootDirectory = normalized;
    state_.currentDirectory = normalized;
    refresh_records(records);
    lastError_.clear();
    return true;
}

bool AssetBrowser::set_current_directory(const std::filesystem::path& directory, std::string* error) {
    std::filesystem::path resolved;
    if (!resolve_inside_root(directory, resolved, error)) return false;
    std::vector<AssetFileRecord> records;
    std::string localError;
    if (!fileSystem_->list_directory(resolved, records, &localError)) { set_error(error, localError); lastError_ = localError; return false; }
    if (state_.recursiveScan) {
        records.clear();
        collect_records(*fileSystem_, resolved, true, records, &localError);
        if (!localError.empty()) { set_error(error, localError); lastError_ = localError; return false; }
    }
    state_.currentDirectory = resolved;
    refresh_records(records);
    lastError_.clear();
    return true;
}

bool AssetBrowser::scan(std::string* error) {
    if (state_.currentDirectory.empty()) { set_error(error, "asset browser current directory is not set"); return false; }
    std::vector<AssetFileRecord> records;
    std::string localError;
    if (state_.recursiveScan) collect_records(*fileSystem_, state_.currentDirectory, true, records, &localError);
    else if (!fileSystem_->list_directory(state_.currentDirectory, records, &localError)) { set_error(error, localError); lastError_ = localError; return false; }
    if (!localError.empty()) { set_error(error, localError); lastError_ = localError; return false; }
    refresh_records(records);
    lastError_.clear();
    return true;
}

bool AssetBrowser::matches_filter(const AssetEntry& entry) const {
    if (!state_.showHiddenFiles && hidden_name(entry.name)) return false;
    if (entry.isDirectory && !state_.includeDirectories) return false;
    if (!state_.searchQuery.empty()) {
        const auto query = lower_ascii(state_.searchQuery);
        if (lower_ascii(entry.name).find(query) == std::string::npos && lower_ascii(entry.relativePath).find(query) == std::string::npos) return false;
    }
    if (entry.isDirectory || state_.typeFilter.empty()) return true;
    return std::find(state_.typeFilter.begin(), state_.typeFilter.end(), entry.type) != state_.typeFilter.end();
}

std::vector<const AssetEntry*> AssetBrowser::visible_entries() const {
    std::vector<const AssetEntry*> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) if (matches_filter(entry)) result.push_back(&entry);
    return result;
}

bool AssetBrowser::read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes, std::string* error) const {
    std::filesystem::path resolved;
    if (!resolve_inside_root(path, resolved, error)) return false;
    return fileSystem_->read_file(resolved, bytes, error);
}

bool AssetBrowser::write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string* error) {
    std::filesystem::path resolved;
    if (!resolve_inside_root(path, resolved, error)) return false;
    if (!fileSystem_->write_file(resolved, bytes, error)) return false;
    scan();
    return true;
}

bool AssetBrowser::rename_file(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error) {
    std::filesystem::path resolvedFrom;
    std::filesystem::path resolvedTo;
    if (!resolve_inside_root(from, resolvedFrom, error) || !resolve_inside_root(to, resolvedTo, error)) return false;
    if (resolvedFrom == normalized_path(state_.rootDirectory)) { set_error(error, "cannot rename the browser root"); return false; }
    if (!fileSystem_->rename_file(resolvedFrom, resolvedTo, error)) return false;
    scan();
    return true;
}

bool AssetBrowser::create_directory(const std::filesystem::path& path, std::string* error) {
    std::filesystem::path resolved;
    if (!resolve_inside_root(path, resolved, error)) return false;
    if (!fileSystem_->create_directory(resolved, error)) return false;
    scan();
    return true;
}

std::vector<AssetChange> AssetBrowser::poll_changes(std::string* error) {
    std::vector<AssetChange> changes;
    if (state_.currentDirectory.empty()) { set_error(error, "asset browser current directory is not set"); return changes; }
    std::vector<AssetFileRecord> records;
    std::string localError;
    if (state_.recursiveScan) collect_records(*fileSystem_, state_.currentDirectory, true, records, &localError);
    else if (!fileSystem_->list_directory(state_.currentDirectory, records, &localError)) { set_error(error, localError); lastError_ = localError; return changes; }
    if (!localError.empty()) { set_error(error, localError); lastError_ = localError; return changes; }
    refresh_records(records, &changes);
    lastError_.clear();
    return changes;
}

bool AssetBrowser::from_xml(std::string_view xml, std::string* error) {
    AssetBrowserState parsed;
    if (!deserialize_xml(xml, parsed, error)) return false;
    if (parsed.rootDirectory.empty()) { state_ = std::move(parsed); entries_.clear(); return true; }
    const auto oldState = state_;
    const auto oldEntries = entries_;
    state_ = std::move(parsed);
    if (!scan(error)) { state_ = oldState; entries_ = oldEntries; return false; }
    return true;
}

} // namespace shinkou::uikit
