#include "shinkou/editor/UiKitPanelHost.h"

#include "shinkou/editor/EditorLayer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <utility>

#if defined(SHINKOU_WITH_IMGUI)
#include <imgui.h>
#endif

namespace shinkou::editor {
namespace {

class FixedPanel final : public uikit::Panel {
public:
    FixedPanel(float width, float height) : width_(width), height_(height) {}

    uikit::Size measure(uikit::Size) const override {
        return {
            width_ > 0.0f ? width_ : 0.0f,
            height_ > 0.0f ? height_ : 0.0f
        };
    }

private:
    float width_{0.0f};
    float height_{0.0f};
};

uikit::PointerButton pointer_button(std::string_view control) {
    const auto separator = control.find(':');
    const auto name = separator == std::string_view::npos ? control : control.substr(separator + 1);
    if (name == "left" || name == "1") return uikit::PointerButton::Left;
    if (name == "middle" || name == "2") return uikit::PointerButton::Middle;
    if (name == "right" || name == "3") return uikit::PointerButton::Right;
    return uikit::PointerButton::None;
}

int key_code(std::string_view control) {
    const auto separator = control.find(':');
    const auto name = separator == std::string_view::npos ? control : control.substr(separator + 1);
    if (name == "Backspace") return 8;
    if (name == "Left") return 37;
    if (name == "Right") return 39;
    if (name == "Up") return 38;
    if (name == "Down") return 40;
    if (name == "Enter" || name == "Return") return 13;
    if (name == "Escape") return 27;
    return name.size() == 1 ? static_cast<unsigned char>(name.front()) : 0;
}

uikit::Panel& panel(uikit::Panel& parent, std::string title, float width, float height) {
    auto& result = parent.emplace<FixedPanel>(width, height);
    result.name = std::move(title);
    result.styleClass = "panel";
    result.padding = uikit::Insets(12.0f);
    result.gap = 7.0f;
    return result;
}

uikit::Label& label(uikit::Panel& parent, std::string value, bool secondary = false) {
    auto& result = parent.emplace<uikit::Label>(std::move(value));
    result.secondary = secondary;
    result.styleClass = "label";
    return result;
}

uikit::Button& button(uikit::Panel& parent, std::string value, std::string variant = {}) {
    auto& result = parent.emplace<uikit::Button>(std::move(value));
    result.styleClass = "button";
    result.styleVariant = std::move(variant);
    return result;
}

void surface(uikit::Panel& target, uikit::Color value) {
    target.backgroundOverride = value;
    target.hasBackgroundOverride = true;
}

const EditorObjectTreeNode* find_object_node(const std::vector<EditorObjectTreeNode>& nodes, ObjectId id) {
    for (const auto& node : nodes) {
        if (node.id == id) return &node;
        if (const auto* child = find_object_node(node.children, id)) return child;
    }
    return nullptr;
}

} // namespace

UiKitPanelHost::UiKitPanelHost() {
    runtime_.style() = uikit::StyleSheet::make_windows11_dark();
    runtime_.root().name = "EditorRoot";
}

void UiKitPanelHost::set_viewport(float width, float height, float dpiScale) {
    width_ = std::max(1.0f, width);
    height_ = std::max(1.0f, height);
    dpiScale_ = std::max(0.25f, dpiScale);
    runtime_.set_viewport({width_, height_}, dpiScale_);
}

uikit::Color UiKitPanelHost::color(std::string_view value, uikit::Color fallback) {
    if (value.empty()) return fallback;
    const auto parsed = uikit::Color::from_hex(std::string(value));
    return parsed.a == 0.0f && value != "#00000000" ? fallback : parsed;
}

std::size_t UiKitPanelHost::hash_combine(std::size_t seed, std::string_view value) {
    for (const unsigned char character : value) {
        seed ^= static_cast<std::size_t>(character);
        seed *= static_cast<std::size_t>(1099511628211ull);
    }
    return seed;
}

std::size_t UiKitPanelHost::hash_combine(std::size_t seed, std::size_t value) {
    return hash_combine(seed, std::to_string(value));
}

void UiKitPanelHost::apply_style(const EditorLayoutState& layout) {
    const bool dark = layout.theme != "light";
    runtime_.style() = dark ? uikit::StyleSheet::make_windows11_dark() : uikit::StyleSheet::make_windows11_light();
    if (layout.theme == "high-contrast") {
        runtime_.style().palette["window"] = uikit::Color::from_hex("#000000");
        runtime_.style().palette["surface"] = uikit::Color::from_hex("#111111");
        runtime_.style().palette["text"] = uikit::Color::from_hex("#FFFFFF");
        runtime_.style().palette["text-secondary"] = uikit::Color::from_hex("#E6E6E6");
        runtime_.style().palette["accent"] = uikit::Color::from_hex("#00E5FF");
        runtime_.style().windowBackground = runtime_.style().palette["window"];
        runtime_.style().surface = runtime_.style().palette["surface"];
        runtime_.style().controls["panel"].background = runtime_.style().surface;
        runtime_.style().controls["label"].text = runtime_.style().palette["text"];
        runtime_.style().controls["label"].textSecondary = runtime_.style().palette["text-secondary"];
    }
    runtime_.style().font.family = "Microsoft YaHei";
    runtime_.style().font.path = layout.fontPath;
    runtime_.style().font.size = std::clamp(layout.fontSize, 8.0f, 64.0f);
    runtime_.style().uiScale = std::clamp(layout.uiScale, 0.5f, 3.0f);
    runtime_.style().reduceMotion = layout.reduceMotion;
    const uikit::Color background = color(layout.backgroundColor,
        dark ? uikit::Color::from_hex("#202020") : uikit::Color::from_hex("#F3F4F6"));
    runtime_.style().windowBackground = background;
    runtime_.style().palette["window"] = background;
}

void UiKitPanelHost::command(EditorCommand value, std::string_view target) {
    if (commandHandler_) commandHandler_(value, target);
}

void UiKitPanelHost::add_hierarchy_node(uikit::Panel& parent, const EditorObjectTreeNode& node, int depth) {
    if (node.name.empty()) return;
    const auto overrideName = objectNameOverrides_.find(node.id);
    const std::string displayName = overrideName == objectNameOverrides_.end() ? node.name : overrideName->second;
    const std::string caption = std::string(static_cast<std::size_t>(depth) * 2u, ' ') +
        (node.children.empty() ? "• " : "▸ ") + displayName;
    auto& item = button(parent, caption, node.selected ? "primary" : "subtle");
    item.clicked = [this, id = node.id] {
        if (objectSelectionHandler_) objectSelectionHandler_(id);
    };
    item.horizontalAlign = uikit::Align::Stretch;
    for (const auto& child : node.children) add_hierarchy_node(parent, child, depth + 1);
}

void UiKitPanelHost::build_tree(const EditorUiModel& model, const std::vector<FileEntry>& files,
                                const render::Renderer& renderer, const ui::MediaPanel& mediaPanel, const EditorLayoutState& layout,
                                const std::vector<std::string>& consoleEntries, std::string_view status) {
    const bool dark = layout.theme != "light";
    runtime_.root().clear();
    auto& root = runtime_.root();
    root.layout = uikit::LayoutMode::Vertical;
    root.padding = {};
    root.gap = 0.0f;
    root.styleClass = "panel";
    surface(root, runtime_.style().windowBackground);

    auto& toolbar = root.emplace<FixedPanel>(0.0f, 46.0f);
    toolbar.layout = uikit::LayoutMode::Horizontal;
    toolbar.padding = uikit::Insets(12.0f, 7.0f);
    toolbar.gap = 7.0f;
    surface(toolbar, runtime_.style().surfaceElevated);
    toolbar.visible = layout.showToolbar;
    auto& brand = label(toolbar, "SHINKOU  /  Editor");
    brand.styleVariant = "primary";
    auto& run = button(toolbar, "运行", "primary");
    run.clicked = [this] { command(EditorCommand::Play); };
    auto& pause = button(toolbar, "暂停", "subtle");
    pause.clicked = [this] { command(EditorCommand::Pause); };
    auto& step = button(toolbar, "单步", "subtle");
    step.clicked = [this] { command(EditorCommand::Step); };
    auto& save = button(toolbar, "保存布局", "subtle");
    save.clicked = [this] { command(EditorCommand::SaveLayout); };
    auto& reset = button(toolbar, "重置布局", "subtle");
    reset.clicked = [this] { command(EditorCommand::ResetLayout); };
    auto& create = button(toolbar, "+ 对象", "subtle");
    create.clicked = [this] { command(EditorCommand::CreateEmpty); };
    auto& createUi = button(toolbar, "+ UI", "subtle");
    createUi.clicked = [this] { command(EditorCommand::CreateUiObject); };
    auto& refresh = button(toolbar, "刷新资源", "subtle");
    refresh.clicked = [this] { command(EditorCommand::RefreshAssets); };
    auto& toolbarStatus = label(toolbar, layout.workspace + "  ·  " + std::string(status), true);
    toolbarStatus.flex = 1.0f;
    toolbarStatus.horizontalAlign = uikit::Align::End;

    auto& body = root.emplace<FixedPanel>(0.0f, 0.0f);
    body.layout = uikit::LayoutMode::Horizontal;
    body.flex = 1.0f;
    body.gap = 1.0f;

    auto& hierarchy = panel(body, "hierarchy", 250.0f, 0.0f);
    hierarchy.visible = layout.showHierarchy;
    surface(hierarchy, runtime_.style().surface);
    label(hierarchy, "层级", false);
    auto& hierarchySearch = hierarchy.emplace<uikit::TextBox>(objectFilter_);
    hierarchySearch.styleClass = "input";
    hierarchySearch.changed = [this](const std::string& value) {
        objectFilter_ = value;
        if (objectFilterHandler_) objectFilterHandler_(value);
    };
    auto& hierarchyActions = hierarchy.emplace<FixedPanel>(0.0f, 32.0f);
    hierarchyActions.layout = uikit::LayoutMode::Horizontal;
    hierarchyActions.gap = 6.0f;
    auto& addObject = button(hierarchyActions, "+ 新建", "subtle");
    addObject.clicked = [this] { command(EditorCommand::CreateEmpty); };
    auto& addChild = button(hierarchyActions, "+ 子级", "subtle");
    addChild.clicked = [this] { command(EditorCommand::CreateChild); };
    auto& hierarchyList = hierarchy.emplace<uikit::ScrollView>();
    hierarchyList.layout = uikit::LayoutMode::Vertical;
    hierarchyList.flex = 1.0f;
    hierarchyList.gap = 3.0f;
    hierarchyList.clipChildren = true;
    for (const auto& node : model.object_roots()) add_hierarchy_node(hierarchyList, node, 0);

    auto& workspace = panel(body, "workspace", 0.0f, 0.0f);
    workspace.flex = 1.0f;
    workspace.padding = {};
    workspace.gap = 1.0f;
    surface(workspace, runtime_.style().windowBackground);
    auto& workspaceHeader = workspace.emplace<FixedPanel>(0.0f, 38.0f);
    workspaceHeader.layout = uikit::LayoutMode::Horizontal;
    workspaceHeader.padding = uikit::Insets(12.0f, 5.0f);
    workspaceHeader.gap = 6.0f;
    surface(workspaceHeader, runtime_.style().surface);
    workspaceHeader.visible = layout.showViewport || layout.showGame;
    auto& tabs = workspaceHeader.emplace<uikit::TabView>();
    tabs.tabs = {"场景", "运行", "资源"};
    tabs.activeTab = layout.showGame ? 1u : 0u;
    tabs.tabChanged = [this](std::size_t index) {
        command(EditorCommand::TogglePage, index == 1 ? "game" : index == 2 ? "project" : "scene");
    };
    auto& viewport = workspace.emplace<FixedPanel>(0.0f, 0.0f);
    viewport.flex = 1.0f;
    viewport.visible = layout.showViewport || layout.showGame;
    viewport.padding = uikit::Insets(16.0f);
    viewport.layout = uikit::LayoutMode::Vertical;
    viewport.gap = 8.0f;
    surface(viewport, uikit::Color::from_hex(dark ? "#10151D" : "#E8EDF3"));
    label(viewport, layout.showGame ? "Game View" : "Scene View", false);
    auto& viewportSurface = viewport.emplace<FixedPanel>(0.0f, 0.0f);
    viewportSurface.flex = 1.0f;
    viewportSurface.padding = uikit::Insets(18.0f);
    viewportSurface.layout = uikit::LayoutMode::Vertical;
    viewportSurface.gap = 8.0f;
    surface(viewportSurface, uikit::Color::from_hex(dark ? "#161D27" : "#F7F9FB"));
    label(viewportSurface, "渲染视口", false);
    label(viewportSurface, layout.showGame ? "运行时画面输出 / 输入已连接" : "场景编辑画布 / 渲染管线输出", true);
    auto& viewportHint = label(viewportSurface,
        "Backend: " + std::string(renderer.capabilities().api == render::BackendApi::Null ? "Null" : "GPU") +
        "  ·  " + std::to_string(static_cast<unsigned>(width_)) + " × " +
        std::to_string(static_cast<unsigned>(height_)), true);
    viewportHint.flex = 1.0f;
    viewportHint.verticalAlign = uikit::Align::Center;
    viewportHint.horizontalAlign = uikit::Align::Center;

    auto& bottom = workspace.emplace<FixedPanel>(0.0f, 154.0f);
    bottom.layout = uikit::LayoutMode::Horizontal;
    bottom.gap = 1.0f;
    auto& assets = panel(bottom, "assets", 0.0f, 0.0f);
    assets.flex = 1.0f;
    assets.visible = layout.showAssets;
    surface(assets, runtime_.style().surface);
    auto& assetsHeader = assets.emplace<FixedPanel>(0.0f, 28.0f);
    assetsHeader.layout = uikit::LayoutMode::Horizontal;
    label(assetsHeader, "项目资源", false);
    auto& assetSearch = assetsHeader.emplace<uikit::TextBox>(assetFilter_);
    assetSearch.styleClass = "input";
    assetSearch.flex = 1.0f;
    assetSearch.changed = [this](const std::string& value) { assetFilter_ = value; };
    auto& assetRefresh = button(assetsHeader, "刷新", "subtle");
    assetRefresh.clicked = [this] { command(EditorCommand::RefreshAssets); };
    auto& assetScroll = assets.emplace<uikit::ScrollView>();
    assetScroll.layout = uikit::LayoutMode::Vertical;
    assetScroll.flex = 1.0f;
    assetScroll.gap = 2.0f;
    for (const auto& entry : files) {
        const auto path = entry.relativePath.generic_string();
        if (!assetFilter_.empty() && path.find(assetFilter_) == std::string::npos) continue;
        auto& item = button(assetScroll, (entry.directory ? "▸  " : "    ") + path, path == selectedAsset_ ? "primary" : "subtle");
        item.clicked = [this, path] {
            selectedAsset_ = path;
            if (assetSelectionHandler_) assetSelectionHandler_(path);
        };
    }

    auto& console = panel(bottom, "console", 0.0f, 0.0f);
    console.flex = 1.0f;
    console.visible = layout.showConsole;
    surface(console, runtime_.style().surface);
    label(console, "控制台", false);
    auto& consoleScroll = console.emplace<uikit::ScrollView>();
    consoleScroll.layout = uikit::LayoutMode::Vertical;
    consoleScroll.flex = 1.0f;
    consoleScroll.gap = 1.0f;
    const std::size_t firstConsole = consoleEntries.size() > 10 ? consoleEntries.size() - 10 : 0;
    for (std::size_t index = firstConsole; index < consoleEntries.size(); ++index) label(consoleScroll, consoleEntries[index], true);
    label(console, renderer.last_error().empty() ? "就绪" : renderer.last_error(), !renderer.last_error().empty());
    bottom.visible = layout.showAssets || layout.showConsole;

    if (layout.showProfiler || layout.showRenderGraph || layout.showSettings || layout.showMedia) {
        auto& auxiliary = workspace.emplace<FixedPanel>(0.0f, 174.0f);
        auxiliary.layout = uikit::LayoutMode::Horizontal;
        auxiliary.gap = 1.0f;

        if (layout.showProfiler) {
            auto& profiler = panel(auxiliary, "profiler", 0.0f, 0.0f);
            profiler.flex = 1.0f;
            surface(profiler, runtime_.style().surface);
            const auto stats = renderer.stats();
            label(profiler, "性能分析", false);
            label(profiler, "帧数     " + std::to_string(stats.frames), true);
            label(profiler, "绘制调用 " + std::to_string(stats.drawCalls), true);
            label(profiler, "渲染通道 " + std::to_string(stats.passes), true);
            label(profiler, "资源创建 " + std::to_string(stats.resourceCreates) +
                "  /  销毁 " + std::to_string(stats.resourceDestroys), true);
        }

        if (layout.showRenderGraph) {
            auto& graphPanel = panel(auxiliary, "render-graph", 0.0f, 0.0f);
            graphPanel.flex = 1.0f;
            surface(graphPanel, runtime_.style().surface);
            const auto& graph = renderer.graph();
            label(graphPanel, "渲染图", false);
            label(graphPanel, "Passes " + std::to_string(graph.passes().size()) +
                "  ·  Resources " + std::to_string(graph.resources().size()) +
                "  ·  Transitions " + std::to_string(graph.transitions().size()), true);
            const std::size_t passLimit = std::min<std::size_t>(graph.passes().size(), 4);
            for (std::size_t index = 0; index < passLimit; ++index)
                label(graphPanel, "• " + graph.passes()[index].name, true);
            if (graph.diagnostics().executionFailed)
                label(graphPanel, graph.diagnostics().lastError, true);
        }

        if (layout.showSettings) {
            auto& settings = panel(auxiliary, "settings", 240.0f, 0.0f);
            surface(settings, runtime_.style().surface);
            label(settings, "编辑器设置", false);
            label(settings, "主题 / 字体 / 缩放", true);
            auto& darkTheme = button(settings, "深色", layout.theme == "dark" ? "primary" : "subtle");
            darkTheme.clicked = [this] { command(EditorCommand::SetDarkTheme); };
            auto& lightTheme = button(settings, "浅色", layout.theme == "light" ? "primary" : "subtle");
            lightTheme.clicked = [this] { command(EditorCommand::SetLightTheme); };
            auto& contrastTheme = button(settings, "高对比度", layout.theme == "high-contrast" ? "primary" : "subtle");
            contrastTheme.clicked = [this] { command(EditorCommand::SetHighContrastTheme); };
            label(settings, "Microsoft YaHei  ·  " + std::to_string(static_cast<int>(layout.fontSize)) + " px  ·  " +
                std::to_string(static_cast<int>(layout.uiScale * 100.0f)) + "%", true);
        }

        if (layout.showMedia) {
            auto& media = panel(auxiliary, "media", 0.0f, 0.0f);
            media.flex = 1.0f;
            surface(media, runtime_.style().surface);
            const auto& description = mediaPanel.description();
            const auto& playback = mediaPanel.playback();
            label(media, "媒体预览", false);
            label(media, description.resource.uri.empty() ? "未选择资源" : description.resource.uri, true);
            label(media, std::string("类型  ") + ui::media_kind_name(description.kind) +
                "  ·  状态  " + ui::media_playback_state_name(playback.state), true);
            if (description.kind != ui::MediaKind::Image && description.showPlaybackControls) {
                auto& play = button(media, playback.state == ui::MediaPlaybackState::Playing ? "暂停" : "播放", "primary");
                play.clicked = [this, state = playback.state] {
                    if (mediaCommandHandler_) mediaCommandHandler_(state == ui::MediaPlaybackState::Playing ? ui::MediaCommand::pause() : ui::MediaCommand::play());
                };
                auto& stop = button(media, "停止", "subtle");
                stop.clicked = [this] { if (mediaCommandHandler_) mediaCommandHandler_(ui::MediaCommand::stop()); };
            }
        }
    }

    auto& inspector = panel(body, "inspector", 300.0f, 0.0f);
    inspector.visible = layout.showInspector;
    surface(inspector, runtime_.style().surface);
    label(inspector, "检查器", false);
    if (model.selected_object() == 0) {
        label(inspector, "未选择对象", true);
        label(inspector, "从左侧层级中选择对象以编辑属性。", true).wrap = true;
    } else {
        const auto* selectedNode = find_object_node(model.object_roots(), model.selected_object());
        const auto nameOverride = objectNameOverrides_.find(model.selected_object());
        const std::string selectedName = nameOverride == objectNameOverrides_.end()
            ? (selectedNode ? selectedNode->name : "选中对象") : nameOverride->second;
        label(inspector, "GameObject", true);
        label(inspector, "Object ID  " + std::to_string(model.selected_object()), false);
        auto& name = inspector.emplace<uikit::TextBox>(selectedName);
        name.styleClass = "input";
        name.changed = [this, id = model.selected_object()](const std::string& value) {
            objectNameOverrides_[id] = value;
            if (objectNameHandler_) objectNameHandler_(id, value);
        };
        auto& active = inspector.emplace<uikit::CheckBox>("启用");
        active.checked = selectedNode ? selectedNode->active : true;
        active.clicked = [this, id = model.selected_object(), &active] {
            if (objectActiveHandler_) objectActiveHandler_(id, active.checked);
        };
        auto& transform = panel(inspector, "transform", 0.0f, 112.0f);
        transform.padding = uikit::Insets(8.0f);
        surface(transform, runtime_.style().windowBackground);
        label(transform, "Transform", false);
        label(transform, "Position   0.00   0.00   0.00", true);
        label(transform, "Rotation   0.00   0.00   0.00", true);
        label(transform, "Scale      1.00   1.00   1.00", true);
        auto& addComponent = button(inspector, "+ 添加组件", "primary");
        addComponent.clicked = [this] { command(EditorCommand::AddComponent); };
        auto& duplicate = button(inspector, "复制对象", "subtle");
        duplicate.clicked = [this] { command(EditorCommand::CreateChild); };
    }

    auto& statusBar = root.emplace<FixedPanel>(0.0f, 28.0f);
    statusBar.layout = uikit::LayoutMode::Horizontal;
    statusBar.padding = uikit::Insets(12.0f, 4.0f);
    surface(statusBar, runtime_.style().surfaceElevated);
    statusBar.visible = layout.showStatusBar;
    label(statusBar, "●", false).styleVariant = "primary";
    auto& statusLabel = label(statusBar, std::string(status), true);
    statusLabel.flex = 1.0f;
    auto& stats = label(statusBar,
        "UI commands  " + std::to_string(renderList_.stats().commandCount) + "  ·  input " +
        std::to_string(inputHandled_) + "/" + std::to_string(inputSubmitted_), true);
    stats.horizontalAlign = uikit::Align::End;
}

void UiKitPanelHost::rebuild(const EditorUiModel& model, const std::vector<FileEntry>& files,
                             const render::Renderer& renderer, const ui::MediaPanel& mediaPanel, const EditorLayoutState& layout,
                             const std::vector<std::string>& consoleEntries, std::string_view status,
                             float deltaSeconds) {
    set_viewport(width_, height_, dpiScale_);
    std::size_t signature = 1469598103934665603ull;
    signature = hash_combine(signature, layout.theme);
    signature = hash_combine(signature, layout.projectRoot);
    signature = hash_combine(signature, layout.backgroundColor);
    signature = hash_combine(signature, layout.fontPath);
    signature = hash_combine(signature, std::to_string(layout.uiScale));
    signature = hash_combine(signature, std::to_string(layout.fontSize));
    signature = hash_combine(signature, assetFilter_);
    signature = hash_combine(signature, selectedAsset_);
    signature = hash_combine(signature, layout.showToolbar ? 1u : 0u);
    signature = hash_combine(signature, layout.showStatusBar ? 1u : 0u);
    signature = hash_combine(signature, layout.showHierarchy ? 1u : 0u);
    signature = hash_combine(signature, layout.showInspector ? 1u : 0u);
    signature = hash_combine(signature, layout.showViewport ? 1u : 0u);
    signature = hash_combine(signature, layout.showAssets ? 1u : 0u);
    signature = hash_combine(signature, layout.showConsole ? 1u : 0u);
    signature = hash_combine(signature, layout.showProfiler ? 1u : 0u);
    signature = hash_combine(signature, layout.showRenderGraph ? 1u : 0u);
    signature = hash_combine(signature, layout.showMedia ? 1u : 0u);
    signature = hash_combine(signature, mediaPanel.description().resource.uri);
    signature = hash_combine(signature, static_cast<std::size_t>(mediaPanel.description().kind));
    signature = hash_combine(signature, static_cast<std::size_t>(mediaPanel.playback().state));
    signature = hash_combine(signature, std::to_string(model.selected_object()));
    signature = hash_combine(signature, model.object_filter());
    for (const auto& node : model.object_roots()) {
        signature = hash_combine(signature, std::to_string(node.id));
    }
    for (const auto& file : files) {
        signature = hash_combine(signature, file.relativePath.generic_string());
        signature = hash_combine(signature, file.directory ? 1u : 0u);
        signature = hash_combine(signature, static_cast<std::size_t>(file.size));
    }
    for (const auto& entry : consoleEntries) signature = hash_combine(signature, entry);
    signature = hash_combine(signature, status);
    signature = hash_combine(signature, layout.showGame ? "game" : "scene");
    signature = hash_combine(signature, layout.showSettings ? "settings" : "normal");
    if (signature != buildSignature_) {
        apply_style(layout);
        build_tree(model, files, renderer, mediaPanel, layout, consoleEntries, status);
        buildSignature_ = signature;
    }
    runtime_.tick(std::max(0.0f, deltaSeconds));
    paint();
}

void UiKitPanelHost::process_input(const input::InputSystem& input) {
    if (runtime_.root().children().empty()) return;
    for (const auto& source : input.events()) {
        uikit::UiEvent event;
        event.position = {source.position.x, source.position.y};
        if (event.position.x == 0.0f && event.position.y == 0.0f) event.position = {input.mouse().position.x, input.mouse().position.y};
        event.delta = {source.delta.x, source.delta.y};
        event.text = source.text;
        event.button = pointer_button(source.control);
        switch (source.type) {
        case input::InputEventType::MouseMove: event.type = uikit::UiEventType::PointerMove; break;
        case input::InputEventType::MouseButtonDown: event.type = uikit::UiEventType::PointerDown; break;
        case input::InputEventType::MouseButtonUp: event.type = uikit::UiEventType::PointerUp; break;
        case input::InputEventType::MouseWheel:
            event.type = uikit::UiEventType::Wheel;
            event.wheelDelta = source.value != 0.0f ? source.value : source.delta.y;
            break;
        case input::InputEventType::KeyDown:
            event.type = uikit::UiEventType::KeyDown;
            event.key = key_code(source.control);
            break;
        case input::InputEventType::KeyUp:
            event.type = uikit::UiEventType::KeyUp;
            event.key = key_code(source.control);
            break;
        case input::InputEventType::TextInput: event.type = uikit::UiEventType::TextInput; break;
        default: continue;
        }
        ++inputSubmitted_;
        if (runtime_.dispatch(event)) ++inputHandled_;
    }
}

void UiKitPanelHost::paint() {
    renderList_.clear();
    runtime_.paint(renderList_);
}

#if defined(SHINKOU_WITH_IMGUI)
void UiKitPanelHost::draw_imgui() const {
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    const auto to_u8 = [](float value) -> int {
        return static_cast<int>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    const auto color32 = [&to_u8](const uikit::Color& value) {
        return IM_COL32(to_u8(value.r), to_u8(value.g), to_u8(value.b), to_u8(value.a));
    };
    for (const auto& command : renderList_.commands()) {
        switch (command.type) {
        case uikit::DrawCommandType::BeginClip:
            drawList->PushClipRect({command.rect.x, command.rect.y},
                                   {command.rect.x + command.rect.width, command.rect.y + command.rect.height}, true);
            break;
        case uikit::DrawCommandType::EndClip:
            drawList->PopClipRect();
            break;
        case uikit::DrawCommandType::Rect:
            drawList->AddRectFilled({command.rect.x, command.rect.y},
                                    {command.rect.x + command.rect.width, command.rect.y + command.rect.height},
                                    color32(command.color), std::max({command.radii.left, command.radii.top, command.radii.right, command.radii.bottom}));
            break;
        case uikit::DrawCommandType::Border:
            drawList->AddRect({command.rect.x, command.rect.y},
                              {command.rect.x + command.rect.width, command.rect.y + command.rect.height},
                              color32(command.color), std::max({command.radii.left, command.radii.top, command.radii.right, command.radii.bottom}),
                              0, std::max(1.0f, command.thickness));
            break;
        case uikit::DrawCommandType::Line:
            drawList->AddLine({command.from.x, command.from.y}, {command.to.x, command.to.y},
                              color32(command.color), std::max(1.0f, command.thickness));
            break;
        case uikit::DrawCommandType::Text: {
            const float fontSize = std::max(9.0f, command.fontSize);
            drawList->AddText(nullptr, fontSize,
                              {command.rect.x + 4.0f, command.rect.y + (command.rect.height - fontSize) * 0.5f},
                              color32(command.color), command.text.c_str());
            break;
        }
        case uikit::DrawCommandType::Image:
            drawList->AddRectFilled({command.rect.x, command.rect.y},
                                    {command.rect.x + command.rect.width, command.rect.y + command.rect.height},
                                    IM_COL32(90, 100, 115, 80), 4.0f);
            drawList->AddText({command.rect.x + 8.0f, command.rect.y + 8.0f},
                              IM_COL32(210, 220, 235, 220), command.resource.c_str());
            break;
        case uikit::DrawCommandType::Gradient:
            drawList->AddRectFilledMultiColor({command.rect.x, command.rect.y},
                                               {command.rect.x + command.rect.width, command.rect.y + command.rect.height},
                                               color32(command.color), color32(command.color),
                                               color32(command.secondaryColor), color32(command.secondaryColor));
            break;
        }
    }
}
#endif

} // namespace shinkou::editor
