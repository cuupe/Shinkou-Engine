#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shinkou::uikit {

inline constexpr std::uint32_t kAssetBrowserStateVersion = 1;
inline constexpr std::string_view kAssetBrowserStateSchema = "shinkou.uikit.asset-browser";

enum class AssetFileType : std::uint8_t {
    Unknown,
    Directory,
    Image,
    Video,
    Audio,
    Font,
    Text,
    Model,
    Material,
    Shader,
    Scene,
    Script,
    Other,
};

const char* asset_file_type_name(AssetFileType type) noexcept;
bool parse_asset_file_type(std::string_view value, AssetFileType& type) noexcept;
AssetFileType identify_asset_file_type(const std::filesystem::path& path, bool isDirectory = false) noexcept;

struct AssetFileRecord {
    std::filesystem::path path{};
    bool isDirectory{false};
    std::uint64_t size{0};
    // Unix epoch seconds. A platform adapter can fill a higher precision value
    // internally, while this portable model remains stable for XML and diffs.
    std::int64_t modifiedTime{0};
};

// Placeholder metadata intentionally contains no decoder-owned objects. A
// future image/video/audio adapter may populate these fields asynchronously.
struct AssetMetadata {
    std::string mimeType{};
    std::uint32_t width{0};
    std::uint32_t height{0};
    double duration{0.0};
    bool hasDimensions{false};
    bool hasDuration{false};
};

enum class AssetThumbnailState : std::uint8_t { NotRequested, Pending, Ready, Failed };

struct AssetThumbnail {
    std::string cacheKey{};
    std::string sourceUri{};
    AssetThumbnailState state{AssetThumbnailState::NotRequested};
};

struct AssetEntry {
    std::filesystem::path path{};
    std::string name{};
    std::string relativePath{};
    AssetFileType type{AssetFileType::Unknown};
    bool isDirectory{false};
    std::uint64_t size{0};
    std::int64_t modifiedTime{0};
    AssetMetadata metadata{};
    AssetThumbnail thumbnail{};
};

enum class AssetChangeType : std::uint8_t { Added, Removed, Modified };

struct AssetChange {
    AssetChangeType type{AssetChangeType::Modified};
    AssetEntry entry{};
};

// This is the only filesystem contract the browser needs. Implementations may
// use Win32, std::filesystem, a project VFS, or a remote asset service.
class IAssetFileSystem {
public:
    virtual ~IAssetFileSystem() = default;

    virtual bool list_directory(const std::filesystem::path& directory,
                                std::vector<AssetFileRecord>& records,
                                std::string* error = nullptr) const = 0;
    virtual bool read_file(const std::filesystem::path& path,
                           std::vector<std::uint8_t>& bytes,
                           std::string* error = nullptr) const = 0;
    virtual bool write_file(const std::filesystem::path& path,
                            const std::vector<std::uint8_t>& bytes,
                            std::string* error = nullptr) = 0;
    virtual bool rename_file(const std::filesystem::path& from,
                             const std::filesystem::path& to,
                             std::string* error = nullptr) = 0;
    virtual bool create_directory(const std::filesystem::path& path,
                                  std::string* error = nullptr) = 0;
};

// Windows-ready default adapter. It uses the C++17 filesystem API so the
// browser remains buildable without linking to a decoding or UI library.
class NativeAssetFileSystem final : public IAssetFileSystem {
public:
    bool list_directory(const std::filesystem::path& directory,
                        std::vector<AssetFileRecord>& records,
                        std::string* error = nullptr) const override;
    bool read_file(const std::filesystem::path& path,
                   std::vector<std::uint8_t>& bytes,
                   std::string* error = nullptr) const override;
    bool write_file(const std::filesystem::path& path,
                    const std::vector<std::uint8_t>& bytes,
                    std::string* error = nullptr) override;
    bool rename_file(const std::filesystem::path& from,
                     const std::filesystem::path& to,
                     std::string* error = nullptr) override;
    bool create_directory(const std::filesystem::path& path,
                          std::string* error = nullptr) override;
};

enum class AssetBrowserView : std::uint8_t { Grid, List, Columns };

struct AssetBrowserState {
    std::filesystem::path rootDirectory{};
    std::filesystem::path currentDirectory{};
    std::string searchQuery{};
    std::vector<AssetFileType> typeFilter{};
    std::vector<std::filesystem::path> selectedPaths{};
    AssetBrowserView view{AssetBrowserView::Grid};
    float thumbnailSize{96.0f};
    bool showHiddenFiles{false};
    bool includeDirectories{true};
    bool recursiveScan{false};

    bool valid(std::string* error = nullptr) const;
};

const char* asset_browser_view_name(AssetBrowserView view) noexcept;
bool parse_asset_browser_view(std::string_view value, AssetBrowserView& view) noexcept;

std::string serialize_xml(const AssetBrowserState& state, bool pretty = true);
bool deserialize_xml(std::string_view xml, AssetBrowserState& state, std::string* error = nullptr);

class AssetBrowser final {
    IAssetFileSystem* fileSystem_{nullptr};
    AssetBrowserState state_{};
    std::vector<AssetEntry> entries_{};
    std::string lastError_{};

    bool resolve_inside_root(const std::filesystem::path& path,
                             std::filesystem::path& resolved,
                             std::string* error = nullptr) const;
    AssetEntry make_entry(const AssetFileRecord& record) const;
    bool refresh_records(const std::vector<AssetFileRecord>& records,
                         std::vector<AssetChange>* changes = nullptr);

public:
    explicit AssetBrowser(IAssetFileSystem& fileSystem) : fileSystem_(&fileSystem) {}

    IAssetFileSystem& file_system() noexcept { return *fileSystem_; }
    const IAssetFileSystem& file_system() const noexcept { return *fileSystem_; }
    AssetBrowserState& state() noexcept { return state_; }
    const AssetBrowserState& state() const noexcept { return state_; }
    const std::vector<AssetEntry>& entries() const noexcept { return entries_; }
    const std::string& last_error() const noexcept { return lastError_; }

    bool set_root_directory(const std::filesystem::path& root, std::string* error = nullptr);
    bool set_current_directory(const std::filesystem::path& directory, std::string* error = nullptr);
    bool scan(std::string* error = nullptr);
    std::vector<const AssetEntry*> visible_entries() const;

    void set_search_query(std::string query) { state_.searchQuery = std::move(query); }
    void set_type_filter(std::vector<AssetFileType> types) { state_.typeFilter = std::move(types); }
    void clear_type_filter() { state_.typeFilter.clear(); }
    bool matches_filter(const AssetEntry& entry) const;

    bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
                   std::string* error = nullptr) const;
    bool write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
                    std::string* error = nullptr);
    bool rename_file(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::string* error = nullptr);
    bool create_directory(const std::filesystem::path& path, std::string* error = nullptr);

    // Re-scans the current directory and returns the delta since the previous
    // scan. The returned entries remain owned by this browser until the next poll.
    std::vector<AssetChange> poll_changes(std::string* error = nullptr);

    std::string to_xml(bool pretty = true) const { return serialize_xml(state_, pretty); }
    bool from_xml(std::string_view xml, std::string* error = nullptr);
};

} // namespace shinkou::uikit
