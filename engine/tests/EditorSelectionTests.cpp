#include "shinkou/editor/EditorSelection.h"

#include <iostream>
#include <vector>

int main() {
    using namespace shinkou::editor;
    EditorSelection selection;
    std::size_t changes = 0;
    EditorSelectionChange lastChange;
    selection.set_change_callback([&](const EditorSelectionChange& change) {
        ++changes;
        lastChange = change;
    });

    if (!selection.empty() || selection.primary() != 0 || selection.revision() != 0 || selection.clear()) return 1;
    if (!selection.select(42) || selection.size() != 1 || selection.primary() != 42 || selection.revision() != 1) return 2;
    const auto stableRevision = selection.revision();
    if (selection.select(42) || selection.revision() != stableRevision || changes != 1) return 3;

    if (!selection.select(std::vector<shinkou::ObjectId>{7, 42, 7, 0}, EditorSelectionMode::Add) ||
        selection.size() != 2 || selection.primary() != 7 || !selection.contains(42) || !selection.contains(7) || selection.revision() != 2) return 4;
    if (selection.select(42, EditorSelectionMode::Add) || selection.revision() != 2) return 5;
    if (!selection.set_primary(42) || selection.primary() != 42 || selection.revision() != 3 ||
        lastChange.kind != EditorSelectionChangeKind::SetPrimary) return 6;
    if (selection.set_primary(999) || selection.revision() != 3) return 7;

    if (!selection.select(7, EditorSelectionMode::Toggle) || selection.contains(7) || selection.primary() != 42 || selection.revision() != 4) return 8;
    if (!selection.select(7, EditorSelectionMode::Toggle) || !selection.contains(7) || selection.primary() != 7 || selection.revision() != 5) return 9;
    if (selection.remove(999) || selection.remove(999)) return 10;
    if (!selection.clear() || !selection.empty() || selection.primary() != 0 || selection.revision() != 6) return 11;
    if (selection.clear() || selection.revision() != 6 || changes != 6) return 12;

    if (!selection.set({11, 11, 0, 12}, 11) || selection.ids() != std::vector<shinkou::ObjectId>{11, 12} ||
        selection.primary() != 11 || selection.revision() != 7) return 13;
    if (!selection.set({12, 11}, 999) || selection.primary() != 11 || selection.revision() != 8) return 14;
    if (selection.select(std::vector<shinkou::ObjectId>{12, 11}, EditorSelectionMode::Replace) ||
        selection.ids() != std::vector<shinkou::ObjectId>{12, 11} || selection.primary() != 11 || selection.revision() != 8) return 15;

    std::cout << "EditorSelection single/multi selection, primary selection, deduplication, clear, callback, and revision passed\n";
    return 0;
}
