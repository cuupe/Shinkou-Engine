#pragma once

#include "shinkou/editor/EditorUiModel.h"
#include "shinkou/editor/FileSystem.h"
#include "shinkou/input/InputSystem.h"
#include "shinkou/render/Renderer.h"
#include "shinkou/ui/Media.h"
#include "shinkou/uikit/Render.h"
#include "shinkou/uikit/Ui.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou::editor {

struct EditorLayoutState;

class UiKitPanelHost final {
public:
    using CommandHandler = std::function<void(EditorCommand, std::string_view)>;
    using ObjectSelectionHandler = std::function<void(ObjectId)>;
    using ObjectNameHandler = std::function<void(ObjectId, std::string)>;
    using ObjectActiveHandler = std::function<void(ObjectId, bool)>;
    using MediaCommandHandler = std::function<void(const ui::MediaCommand&)>;
    using FilterHandler = std::function<void(std::string)>;

    UiKitPanelHost();

    void set_command_handler(CommandHandler handler) { commandHandler_ = std::move(handler); }
    void set_object_selection_handler(ObjectSelectionHandler handler) { objectSelectionHandler_ = std::move(handler); }
    void set_object_name_handler(ObjectNameHandler handler) { objectNameHandler_ = std::move(handler); }
    void set_object_active_handler(ObjectActiveHandler handler) { objectActiveHandler_ = std::move(handler); }
    void set_media_command_handler(MediaCommandHandler handler) { mediaCommandHandler_ = std::move(handler); }
    void set_object_filter_handler(FilterHandler handler) { objectFilterHandler_ = std::move(handler); }
    void set_asset_selection_handler(FilterHandler handler) { assetSelectionHandler_ = std::move(handler); }

    void set_viewport(float width, float height, float dpiScale = 1.0f);
    void rebuild(const EditorUiModel& model, const std::vector<FileEntry>& files,
                 const render::Renderer& renderer, const ui::MediaPanel& mediaPanel, const EditorLayoutState& layout,
                 const std::vector<std::string>& consoleEntries, std::string_view status,
                 float deltaSeconds);
    void process_input(const input::InputSystem& input);
    void paint();

    const uikit::UiContext& runtime() const noexcept { return runtime_; }
    const uikit::RenderList& render_list() const noexcept { return renderList_; }
    std::size_t input_submitted() const noexcept { return inputSubmitted_; }
    std::size_t input_handled() const noexcept { return inputHandled_; }

#if defined(SHINKOU_WITH_IMGUI)
    void draw_imgui() const;
#endif

private:
    uikit::UiContext runtime_;
    uikit::RenderList renderList_;
    CommandHandler commandHandler_;
    ObjectSelectionHandler objectSelectionHandler_;
    ObjectNameHandler objectNameHandler_;
    ObjectActiveHandler objectActiveHandler_;
    MediaCommandHandler mediaCommandHandler_;
    FilterHandler objectFilterHandler_;
    FilterHandler assetSelectionHandler_;
    float width_{1280.0f};
    float height_{720.0f};
    float dpiScale_{1.0f};
    std::size_t inputSubmitted_{0};
    std::size_t inputHandled_{0};
    std::size_t buildSignature_{0};
    std::string objectFilter_;
    std::string assetFilter_;
    std::string selectedAsset_;
    std::unordered_map<ObjectId, std::string> objectNameOverrides_;

    void apply_style(const EditorLayoutState& layout);
    void build_tree(const EditorUiModel& model, const std::vector<FileEntry>& files,
                    const render::Renderer& renderer, const ui::MediaPanel& mediaPanel, const EditorLayoutState& layout,
                    const std::vector<std::string>& consoleEntries, std::string_view status);
    void add_hierarchy_node(uikit::Panel& parent, const EditorObjectTreeNode& node, int depth);
    static uikit::Color color(std::string_view value, uikit::Color fallback);
    static std::size_t hash_combine(std::size_t seed, std::string_view value);
    static std::size_t hash_combine(std::size_t seed, std::size_t value);
    void command(EditorCommand value, std::string_view target = {});
};

} // namespace shinkou::editor
