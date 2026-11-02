#include "shinkou/ecs/ComponentType.h"

#include <mutex>
#include <vector>

namespace shinkou::ecs::detail {
namespace {

std::mutex &type_table_mutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

std::vector<const ComponentTypeInfo *> &type_table() noexcept {
    // Index 0 is the invalid id and must never be registered; ids start at 1.
    static std::vector<const ComponentTypeInfo *> table{nullptr, nullptr};
    return table;
}

} // namespace

std::atomic<std::uint32_t> &next_type_id() noexcept {
    static std::atomic<std::uint32_t> next{1u};
    return next;
}

void register_type_info(const ComponentTypeInfo *info) noexcept {
    if (!info || info->id == kInvalidTypeId)
        return;
    const std::lock_guard<std::mutex> lock(type_table_mutex());
    auto &table = type_table();
    if (table.size() <= info->id)
        table.resize(static_cast<std::size_t>(info->id) + 1u, nullptr);
    table[info->id] = info;
}

const ComponentTypeInfo *find_type_info(TypeId id) noexcept {
    if (id == kInvalidTypeId)
        return nullptr;
    const std::lock_guard<std::mutex> lock(type_table_mutex());
    const auto &table = type_table();
    return id < table.size() ? table[id] : nullptr;
}

} // namespace shinkou::ecs::detail
