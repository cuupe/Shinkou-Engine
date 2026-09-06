#include "shinkou/editor/EditorCommandBus.h"

#include <iostream>
#include <string>

namespace {
using namespace shinkou::editor;

bool equal_message(const std::string& actual, const char* expected) {
    return actual == expected;
}
}

int main() {
    EditorCommandBus bus;
    std::size_t failures = 0;
    EditorCommandInvocation failedInvocation;
    std::string failureMessage;
    bus.set_failure_callback([&](const EditorCommandInvocation& invocation, std::string_view message) {
        failedInvocation = invocation;
        failureMessage = std::string(message);
        ++failures;
    });

    const auto empty = bus.execute({EditorCommand::None, {}, "test.empty", {}, EditorCommandSource::Shortcut, 0});
    if (empty.accepted || bus.revision() != 0 || failures != 1 || failedInvocation.sourceId != "test.empty" ||
        !equal_message(failureMessage, "Cannot execute an empty editor command")) return 1;

    bus.register_handler(EditorCommand::SaveScene, [](const EditorCommandInvocation&) {
        return EditorCommandExecution{EditorCommandResult::failure("save rejected"), {}, {}};
    });
    const auto failed = bus.execute({EditorCommand::SaveScene, {}, "menu.file.save", {}, EditorCommandSource::Menu, 0});
    if (failed.accepted || bus.revision() != 0 || failures != 2 || failedInvocation.source != EditorCommandSource::Menu ||
        !equal_message(failureMessage, "save rejected")) return 2;

    int value = 0;
    int undoCalls = 0;
    int redoCalls = 0;
    if (!bus.register_handler(EditorCommand::CreateEmpty, [&](const EditorCommandInvocation&) {
            ++value;
            return EditorCommandExecution{
                EditorCommandResult::success(true, "created"),
                [&] { --value; ++undoCalls; return EditorCommandResult::success(true); },
                [&] { ++value; ++redoCalls; return EditorCommandResult::success(true); },
            };
        })) return 3;
    if (bus.register_handler(EditorCommand::CreateEmpty, {})) return 4;

    const auto created = bus.execute({EditorCommand::CreateEmpty, "root", "toolbar.create", "Create root", EditorCommandSource::Toolbar, 0});
    if (!created.accepted || !created.changed || value != 1 || bus.revision() != 1 || bus.traces().back().state != EditorCommandTraceState::Applied ||
        bus.traces().back().invocation.source != EditorCommandSource::Toolbar) return 5;
    if (!bus.can_undo() || bus.can_redo()) return 6;

    const auto undone = bus.undo(EditorCommandSource::Shortcut, "ctrl-z");
    if (!undone.accepted || value != 0 || undoCalls != 1 || bus.revision() != 2 || bus.can_undo() || !bus.can_redo()) return 7;
    const auto redone = bus.redo(EditorCommandSource::Shortcut, "ctrl-y");
    if (!redone.accepted || value != 1 || redoCalls != 1 || bus.revision() != 3 || !bus.can_undo() || bus.can_redo()) return 8;

    const auto transaction = bus.begin_transaction("Create pair", EditorCommandSource::Viewport, "gizmo.drag");
    if (transaction == 0 || !bus.active_transaction() || bus.begin_transaction("nested", EditorCommandSource::Panel) != 0) return 9;
    const auto first = bus.execute({EditorCommand::CreateEmpty, "a", "gizmo.drag", {}, EditorCommandSource::Viewport, transaction});
    const auto second = bus.execute({EditorCommand::CreateEmpty, "b", "gizmo.drag", {}, EditorCommandSource::Viewport, 0});
    if (!first.accepted || !second.accepted || !first.transactional || bus.active_transaction()->commandCount != 2 || value != 3) return 10;
    const auto committed = bus.commit_transaction(transaction);
    if (!committed.accepted || !committed.changed || bus.has_active_transaction() || bus.traces().back().state != EditorCommandTraceState::Applied) return 11;
    const auto transactionTraceCount = bus.traces().size();
    const auto transactionUndo = bus.undo(EditorCommandSource::Shortcut, "ctrl-z");
    if (!transactionUndo.accepted || value != 1 || undoCalls != 3 || bus.revision() != 6 || bus.traces().size() != transactionTraceCount) return 12;

    const auto cancelId = bus.begin_transaction("Cancelled pair", EditorCommandSource::Panel, "details");
    if (cancelId == 0) return 13;
    if (!bus.execute({EditorCommand::CreateEmpty, "cancel", "details", {}, EditorCommandSource::Panel, 0}).accepted) return 14;
    const auto cancelled = bus.cancel_transaction(cancelId);
    if (!cancelled.accepted || value != 1 || undoCalls != 4 || bus.revision() != 8 || bus.traces().back().state != EditorCommandTraceState::Cancelled) return 15;

    if (bus.execute({EditorCommand::SaveScene, {}, "wrong-transaction", {}, EditorCommandSource::Menu, cancelId}).accepted) return 16;
    if (failures != 3) return 17;

    bus.clear_history();
    if (bus.can_undo() || bus.can_redo() || bus.revision() != 8) return 18;
    std::cout << "EditorCommandBus source tracing, transactional undo/redo, failure callbacks, and revision passed\n";
    return 0;
}
