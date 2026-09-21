#include "shinkou/editor/EditorLocalization.h"

#include <iostream>

int main() {
    using shinkou::editor::EditorLocalization;

    EditorLocalization localization;
    if (localization.language() != "zh-CN" || !localization.is_chinese()) return 1;
    if (localization.text("Scene Editor") != "场景编辑器") return 2;
    if (localization.text("An asset name") != "An asset name") return 3;

    localization.set_language("en-US");
    if (localization.language() != "en-US" || localization.is_chinese() ||
        localization.text("Scene Editor") != "Scene Editor") return 4;

    localization.set_language("unsupported");
    if (localization.language() != "zh-CN" || localization.text("Play") != "播放") return 5;
    if (!EditorLocalization::supported("zh") || !EditorLocalization::supported("en") ||
        EditorLocalization::supported("ja-JP")) return 6;

    std::cout << "EditorLocalization language table passed\n";
    return 0;
}
