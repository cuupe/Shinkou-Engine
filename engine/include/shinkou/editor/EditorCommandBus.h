#pragma once

#include "shinkou/editor/EditorUiModel.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace shinkou::editor {

// The source is part of the command contract. UI adapters, shortcuts, and
// viewport tools can therefore share one execution path without losing the
// origin of a mutation.
enum class EditorCommandSource : std::uint8_t {
    Unknown,
    Menu,
    Shortcut,
    Toolbar,
    Viewport,
    Panel,
    Script,
    System,
};

using EditorTransactionId = std::uint64_t;

struct EditorCommandRequest {
    EditorCommand command{EditorCommand::None};
    std::string target;
    std::string sourceId;
    std::string description;
    EditorCommandSource source{EditorCommandSource::Unknown};
    EditorTransactionId transaction{0};
};

struct EditorCommandInvocation {
    EditorCommand command{EditorCommand::None};
    std::string target;
    std::string sourceId;
    std::string description;
    EditorCommandSource source{EditorCommandSource::Unknown};
    EditorTransactionId transaction{0};
    std::uint64_t sequence{0};
};

struct EditorCommandResult {
    bool succeeded{false};
    bool changed{false};
    std::string message;

    static EditorCommandResult success(bool changed = false, std::string message = {});
    static EditorCommandResult failure(std::string message);

    explicit operator bool() const noexcept { return succeeded; }
};

// A changed command must provide both inverse operations. This makes the
// undo boundary explicit instead of silently putting non-undoable mutations
// onto the editor history.
struct EditorCommandExecution {
    EditorCommandResult result;
    std::function<EditorCommandResult()> undo;
    std::function<EditorCommandResult()> redo;
};

struct EditorCommandDispatchResult {
    bool accepted{false};
    bool changed{false};
    bool transactional{false};
    std::uint64_t revision{0};
    std::uint64_t sequence{0};
    EditorTransactionId transaction{0};
    std::string message;

    explicit operator bool() const noexcept { return accepted; }
};

enum class EditorCommandTraceState : std::uint8_t {
    Failed,
    Pending,
    Applied,
    Undone,
    Cancelled,
    RollbackFailed,
};

struct EditorCommandTrace {
    EditorCommandInvocation invocation;
    EditorCommandTraceState state{EditorCommandTraceState::Failed};
    bool changed{false};
};

struct EditorTransactionInfo {
    EditorTransactionId id{0};
    std::string label;
    std::string sourceId;
    EditorCommandSource source{EditorCommandSource::Unknown};
    std::size_t commandCount{0};
};

class EditorCommandBus {
public:
    using Handler = std::function<EditorCommandExecution(const EditorCommandInvocation&)>;
    using FailureCallback = std::function<void(const EditorCommandInvocation&, std::string_view)>;

    bool register_handler(EditorCommand command, Handler handler);
    bool unregister_handler(EditorCommand command);
    bool has_handler(EditorCommand command) const noexcept;

    void set_failure_callback(FailureCallback callback) { failureCallback_ = std::move(callback); }

    EditorCommandDispatchResult execute(EditorCommandRequest request = {});

    EditorTransactionId begin_transaction(std::string label, EditorCommandSource source,
                                          std::string sourceId = {});
    EditorCommandDispatchResult commit_transaction(EditorTransactionId transaction);
    EditorCommandDispatchResult cancel_transaction(EditorTransactionId transaction);
    bool has_active_transaction() const noexcept { return transaction_.has_value(); }
    std::optional<EditorTransactionInfo> active_transaction() const;

    EditorCommandDispatchResult undo(EditorCommandSource source = EditorCommandSource::System,
                                     std::string sourceId = {});
    EditorCommandDispatchResult redo(EditorCommandSource source = EditorCommandSource::System,
                                     std::string sourceId = {});
    bool can_undo() const noexcept { return !undoStack_.empty() && !transaction_.has_value(); }
    bool can_redo() const noexcept { return !redoStack_.empty() && !transaction_.has_value(); }

    void clear_history() noexcept;
    void set_history_limit(std::size_t limit) noexcept;
    std::size_t history_limit() const noexcept { return historyLimit_; }

    std::uint64_t revision() const noexcept { return revision_; }
    std::uint64_t sequence() const noexcept { return sequence_; }
    const std::vector<EditorCommandTrace>& traces() const noexcept { return traces_; }
    const EditorCommandDispatchResult& last_result() const noexcept { return lastResult_; }

private:
    struct HistoryEntry {
        EditorCommandInvocation invocation;
        bool changed{false};
        std::function<EditorCommandResult()> undo;
        std::function<EditorCommandResult()> redo;
    };

    struct HistoryGroup {
        EditorTransactionId transaction{0};
        std::string label;
        std::vector<HistoryEntry> entries;
    };

    struct ActiveTransaction {
        EditorTransactionInfo info;
        std::vector<HistoryEntry> entries;
    };

    std::unordered_map<EditorCommand, Handler> handlers_;
    FailureCallback failureCallback_;
    std::optional<ActiveTransaction> transaction_;
    std::vector<HistoryGroup> undoStack_;
    std::vector<HistoryGroup> redoStack_;
    std::vector<EditorCommandTrace> traces_;
    EditorCommandDispatchResult lastResult_{};
    std::uint64_t revision_{0};
    std::uint64_t sequence_{0};
    EditorTransactionId nextTransaction_{1};
    std::size_t historyLimit_{256};

    EditorCommandInvocation make_invocation(const EditorCommandRequest& request,
                                             std::uint64_t sequence,
                                             EditorTransactionId transaction) const;
    EditorCommandDispatchResult fail(EditorCommandInvocation invocation, std::string message);
    void trim_history() noexcept;
    void trim_traces() noexcept;
    void mark_trace(EditorCommandTraceState state, EditorTransactionId transaction,
                    std::uint64_t sequence) noexcept;
    EditorCommandDispatchResult finish(EditorCommandDispatchResult result);
    EditorCommandDispatchResult apply_history_group(HistoryGroup& group, bool undo,
                                                     EditorCommandSource source, std::string sourceId);
};

} // namespace shinkou::editor
