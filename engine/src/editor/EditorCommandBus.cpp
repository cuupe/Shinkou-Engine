#include "shinkou/editor/EditorCommandBus.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace shinkou::editor {

EditorCommandResult EditorCommandResult::success(bool changed, std::string message) {
    return {true, changed, std::move(message)};
}

EditorCommandResult EditorCommandResult::failure(std::string message) {
    return {false, false, std::move(message)};
}

bool EditorCommandBus::register_handler(EditorCommand command, Handler handler) {
    if (command == EditorCommand::None || !handler) return false;
    return handlers_.emplace(command, std::move(handler)).second;
}

bool EditorCommandBus::unregister_handler(EditorCommand command) {
    if (command == EditorCommand::None) return false;
    return handlers_.erase(command) != 0;
}

bool EditorCommandBus::has_handler(EditorCommand command) const noexcept {
    return command != EditorCommand::None && handlers_.find(command) != handlers_.end();
}

EditorCommandInvocation EditorCommandBus::make_invocation(const EditorCommandRequest& request,
                                                           std::uint64_t sequence,
                                                           EditorTransactionId transaction) const {
    return {request.command, request.target, request.sourceId, request.description,
            request.source, transaction, sequence};
}

EditorCommandDispatchResult EditorCommandBus::finish(EditorCommandDispatchResult result) {
    lastResult_ = result;
    return result;
}

void EditorCommandBus::trim_traces() noexcept {
    constexpr std::size_t traceLimit = 4096;
    if (traces_.size() <= traceLimit) return;
    const auto eraseCount = traces_.size() - traceLimit;
    traces_.erase(traces_.begin(), traces_.begin() + static_cast<std::ptrdiff_t>(eraseCount));
}

void EditorCommandBus::trim_history() noexcept {
    if (historyLimit_ == 0) {
        undoStack_.clear();
        redoStack_.clear();
        return;
    }
    if (undoStack_.size() > historyLimit_) {
        const auto eraseCount = undoStack_.size() - historyLimit_;
        undoStack_.erase(undoStack_.begin(), undoStack_.begin() + static_cast<std::ptrdiff_t>(eraseCount));
    }
    if (redoStack_.size() > historyLimit_) {
        const auto eraseCount = redoStack_.size() - historyLimit_;
        redoStack_.erase(redoStack_.begin(), redoStack_.begin() + static_cast<std::ptrdiff_t>(eraseCount));
    }
}

void EditorCommandBus::mark_trace(EditorCommandTraceState state, EditorTransactionId transaction,
                                   std::uint64_t sequence) noexcept {
    for (auto it = traces_.rbegin(); it != traces_.rend(); ++it) {
        if (it->invocation.sequence == sequence && it->invocation.transaction == transaction) {
            it->state = state;
            return;
        }
    }
}

EditorCommandDispatchResult EditorCommandBus::fail(EditorCommandInvocation invocation, std::string message) {
    const auto diagnostic = message.empty() ? std::string("Editor command failed") : std::move(message);
    traces_.push_back({std::move(invocation), EditorCommandTraceState::Failed, false});
    trim_traces();
    const auto& trace = traces_.back();
    if (failureCallback_) failureCallback_(trace.invocation, diagnostic);
    return finish({false, false, trace.invocation.transaction != 0, revision_, trace.invocation.sequence,
                   trace.invocation.transaction, diagnostic});
}

EditorCommandDispatchResult EditorCommandBus::execute(EditorCommandRequest request) {
    const auto sequence = ++sequence_;
    const auto activeId = transaction_ ? transaction_->info.id : 0;
    if (request.transaction != 0 && request.transaction != activeId) {
        return fail(make_invocation(request, sequence, request.transaction),
                    "Command transaction does not match the active transaction");
    }
    const auto transaction = request.transaction != 0 ? request.transaction : activeId;
    auto invocation = make_invocation(request, sequence, transaction);

    if (invocation.command == EditorCommand::None) return fail(std::move(invocation), "Cannot execute an empty editor command");
    const auto handler = handlers_.find(invocation.command);
    if (handler == handlers_.end()) return fail(std::move(invocation), "No handler is registered for the editor command");

    EditorCommandExecution execution;
    try {
        execution = handler->second(invocation);
    } catch (const std::exception& error) {
        return fail(std::move(invocation), std::string("Editor command threw: ") + error.what());
    } catch (...) {
        return fail(std::move(invocation), "Editor command threw an unknown exception");
    }

    if (!execution.result.succeeded) {
        return fail(std::move(invocation), execution.result.message.empty() ?
            "Editor command handler rejected the command" : execution.result.message);
    }
    if (execution.result.changed && (!execution.undo || !execution.redo)) {
        return fail(std::move(invocation), "Changed editor commands must provide undo and redo callbacks");
    }

    const bool changed = execution.result.changed;
    traces_.push_back({invocation, transaction == 0 ? EditorCommandTraceState::Applied : EditorCommandTraceState::Pending, changed});
    trim_traces();
    if (changed) {
        ++revision_;
        HistoryEntry entry{std::move(invocation), true, std::move(execution.undo), std::move(execution.redo)};
        if (transaction_) transaction_->entries.push_back(std::move(entry));
        else {
            undoStack_.push_back({0, {}, {std::move(entry)}});
            redoStack_.clear();
            trim_history();
        }
    }
    return finish({true, changed, transaction != 0, revision_, sequence, transaction, execution.result.message});
}

EditorTransactionId EditorCommandBus::begin_transaction(std::string label, EditorCommandSource source,
                                                         std::string sourceId) {
    if (transaction_) return 0;
    auto id = nextTransaction_++;
    if (id == 0) id = nextTransaction_++;
    transaction_ = ActiveTransaction{{id, std::move(label), std::move(sourceId), source, 0}, {}};
    return id;
}

std::optional<EditorTransactionInfo> EditorCommandBus::active_transaction() const {
    if (!transaction_) return std::nullopt;
    auto result = transaction_->info;
    result.commandCount = transaction_->entries.size();
    return result;
}

EditorCommandDispatchResult EditorCommandBus::commit_transaction(EditorTransactionId transaction) {
    if (!transaction_ || transaction_->info.id != transaction) {
        EditorCommandRequest request;
        request.source = EditorCommandSource::System;
        request.transaction = transaction;
        const auto sequence = ++sequence_;
        return fail(make_invocation(request, sequence, transaction), "Cannot commit an inactive editor transaction");
    }
    const auto commandCount = transaction_->entries.size();
    if (commandCount != 0) {
        for (const auto& entry : transaction_->entries) mark_trace(EditorCommandTraceState::Applied,
                                                                     transaction, entry.invocation.sequence);
        undoStack_.push_back({transaction, transaction_->info.label, std::move(transaction_->entries)});
        redoStack_.clear();
        trim_history();
    }
    transaction_.reset();
    return finish({true, commandCount != 0, true, revision_, 0, transaction, "Transaction committed"});
}

EditorCommandDispatchResult EditorCommandBus::cancel_transaction(EditorTransactionId transaction) {
    if (!transaction_ || transaction_->info.id != transaction) {
        EditorCommandRequest request;
        request.source = EditorCommandSource::System;
        request.transaction = transaction;
        const auto sequence = ++sequence_;
        return fail(make_invocation(request, sequence, transaction), "Cannot cancel an inactive editor transaction");
    }
    bool rollbackSucceeded = true;
    std::string diagnostic = "Transaction cancelled";
    for (auto it = transaction_->entries.rbegin(); it != transaction_->entries.rend(); ++it) {
        EditorCommandResult result;
        try {
            result = it->undo();
        } catch (const std::exception& error) {
            result = EditorCommandResult::failure(std::string("Undo threw: ") + error.what());
        } catch (...) {
            result = EditorCommandResult::failure("Undo threw an unknown exception");
        }
        if (!result.succeeded) {
            rollbackSucceeded = false;
            diagnostic = result.message.empty() ? "Transaction rollback failed" : result.message;
            mark_trace(EditorCommandTraceState::RollbackFailed, transaction, it->invocation.sequence);
            if (failureCallback_) failureCallback_(it->invocation, diagnostic);
        } else {
            ++revision_;
            mark_trace(EditorCommandTraceState::Cancelled, transaction, it->invocation.sequence);
        }
    }
    const auto changed = !transaction_->entries.empty();
    transaction_.reset();
    return finish({rollbackSucceeded, changed, true, revision_, 0, transaction,
                   rollbackSucceeded ? diagnostic : diagnostic});
}

EditorCommandDispatchResult EditorCommandBus::apply_history_group(HistoryGroup& group, bool undo,
                                                                   EditorCommandSource source, std::string sourceId) {
    if (group.entries.empty()) {
        return finish({true, false, group.transaction != 0, revision_, 0, group.transaction, "History group is empty"});
    }
    auto invoke = [&](HistoryEntry& entry) {
        EditorCommandResult result;
        try {
            result = undo ? entry.undo() : entry.redo();
        } catch (const std::exception& error) {
            result = EditorCommandResult::failure(std::string(undo ? "Undo threw: " : "Redo threw: ") + error.what());
        } catch (...) {
            result = EditorCommandResult::failure(undo ? "Undo threw an unknown exception" : "Redo threw an unknown exception");
        }
        if (!result.succeeded && failureCallback_) {
            auto invocation = entry.invocation;
            invocation.source = source;
            invocation.sourceId = sourceId;
            failureCallback_(invocation, result.message.empty() ? (undo ? "Undo failed" : "Redo failed") : result.message);
        }
        return result;
    };

    if (undo) {
        for (auto it = group.entries.rbegin(); it != group.entries.rend(); ++it) {
            const auto result = invoke(*it);
            if (!result.succeeded) {
                return finish({false, false, group.transaction != 0, revision_, 0, group.transaction,
                                result.message.empty() ? "Undo failed" : result.message});
            }
        }
        for (const auto& entry : group.entries) mark_trace(EditorCommandTraceState::Undone,
                                                            group.transaction, entry.invocation.sequence);
    } else {
        for (auto& entry : group.entries) {
            const auto result = invoke(entry);
            if (!result.succeeded) {
                return finish({false, false, group.transaction != 0, revision_, 0, group.transaction,
                                result.message.empty() ? "Redo failed" : result.message});
            }
        }
        for (const auto& entry : group.entries) mark_trace(EditorCommandTraceState::Applied,
                                                            group.transaction, entry.invocation.sequence);
    }
    ++revision_;
    return finish({true, true, group.transaction != 0, revision_, 0, group.transaction,
                   undo ? "Undo applied" : "Redo applied"});
}

EditorCommandDispatchResult EditorCommandBus::undo(EditorCommandSource source, std::string sourceId) {
    if (transaction_) {
        EditorCommandRequest request;
        request.source = source;
        request.sourceId = std::move(sourceId);
        const auto sequence = ++sequence_;
        return fail(make_invocation(request, sequence, transaction_->info.id),
                    "Cannot undo while an editor transaction is active");
    }
    if (undoStack_.empty()) {
        EditorCommandRequest request;
        request.source = source;
        request.sourceId = std::move(sourceId);
        const auto sequence = ++sequence_;
        return fail(make_invocation(request, sequence, 0), "Undo history is empty");
    }
    auto group = std::move(undoStack_.back());
    undoStack_.pop_back();
    const auto result = apply_history_group(group, true, source, std::move(sourceId));
    if (result.accepted) redoStack_.push_back(std::move(group));
    else undoStack_.push_back(std::move(group));
    trim_history();
    return result;
}

EditorCommandDispatchResult EditorCommandBus::redo(EditorCommandSource source, std::string sourceId) {
    if (transaction_) {
        EditorCommandRequest request;
        request.source = source;
        request.sourceId = std::move(sourceId);
        const auto sequence = ++sequence_;
        return fail(make_invocation(request, sequence, transaction_->info.id),
                    "Cannot redo while an editor transaction is active");
    }
    if (redoStack_.empty()) {
        EditorCommandRequest request;
        request.source = source;
        request.sourceId = std::move(sourceId);
        const auto sequence = ++sequence_;
        return fail(make_invocation(request, sequence, 0), "Redo history is empty");
    }
    auto group = std::move(redoStack_.back());
    redoStack_.pop_back();
    const auto result = apply_history_group(group, false, source, std::move(sourceId));
    if (result.accepted) undoStack_.push_back(std::move(group));
    else redoStack_.push_back(std::move(group));
    trim_history();
    return result;
}

void EditorCommandBus::clear_history() noexcept {
    if (transaction_) return;
    undoStack_.clear();
    redoStack_.clear();
}

void EditorCommandBus::set_history_limit(std::size_t limit) noexcept {
    historyLimit_ = limit;
    trim_history();
}

} // namespace shinkou::editor
