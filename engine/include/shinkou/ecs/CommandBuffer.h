#pragma once

#include "shinkou/Types.h"
#include "shinkou/ecs/EntityStore.h"

#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace shinkou::ecs {

// Deferred structural changes, DOTS style.
//
// Systems that run on worker threads must not migrate archetypes or allocate
// chunks; they record commands here instead.  Playback happens on the owning
// thread at a sync point.  Entities created inside the buffer can be referenced
// by later commands through placeholder handles.
class CommandBuffer {
  public:
    explicit CommandBuffer(EntityStore &store);
    ~CommandBuffer();
    CommandBuffer(const CommandBuffer &) = delete;
    CommandBuffer &operator=(const CommandBuffer &) = delete;

    Entity create_entity();
    void destroy_entity(Entity entity);

    template <class T, class... Args> void add_component(Entity entity, Args &&...args) {
        auto *payload =
            ::new (allocate_payload(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
        try {
            commands_.push_back(Command{Kind::Add, entity, payload,
                                        [](EntityStore &store, Entity target, const void *data) {
                                            store.emplace<T>(target, *static_cast<const T *>(data));
                                        },
                                        payload_destructor<T>()});
        } catch (...) {
            payload->~T();
            throw;
        }
    }

    template <class T> void set_component(Entity entity, const T &value) {
        add_component<T>(entity, value);
    }

    template <class T> void remove_component(Entity entity) {
        commands_.push_back(Command{
            Kind::Remove, entity, nullptr,
            [](EntityStore &store, Entity target, const void *) { store.remove<T>(target); },
            nullptr});
    }

    void playback();
    void clear() noexcept;
    std::size_t command_count() const noexcept {
        return commands_.size();
    }
    bool empty() const noexcept {
        return commands_.empty();
    }
    EntityStore &store() noexcept {
        return *store_;
    }
    void reserve(std::size_t commands) {
        commands_.reserve(commands);
    }
    // Retained pages are reused by clear/playback. Useful for memory budgeting
    // and proving the warmed recording path does not allocate per payload.
    std::size_t payload_capacity() const noexcept;

  private:
    enum class Kind : std::uint8_t { Create, Destroy, Add, Remove };

    struct Command {
        Kind kind;
        Entity entity{};
        const void *payload{nullptr};
        void (*apply)(EntityStore &, Entity, const void *){nullptr};
        void (*release)(const void *){nullptr};
    };

    static constexpr std::uint32_t kPlaceholderBase = 0x40000000u;

    Entity placeholder() noexcept {
        return Entity{kPlaceholderBase + nextPlaceholder_++, 0u};
    }
    Entity resolve(Entity entity) const noexcept;
    void *allocate_payload(std::size_t size, std::size_t alignment);
    template <class T>
    static constexpr auto payload_destructor() noexcept -> void (*)(const void *) {
        if constexpr (std::is_trivially_destructible_v<T>)
            return nullptr;
        else
            return [](const void *data) { static_cast<const T *>(data)->~T(); };
    }

    struct PayloadBlock {
        std::unique_ptr<std::uint8_t[]> data;
        std::size_t capacity{0};
        std::size_t used{0};
    };

    EntityStore *store_{nullptr};
    std::vector<Command> commands_;
    std::vector<Entity> created_;
    std::vector<PayloadBlock> payloadBlocks_;
    std::size_t currentBlock_{0};
    std::uint32_t nextPlaceholder_{0};
};

// One buffer per logical partition, not per OS thread: work stealing never
// makes two jobs append to the same vector. Playback follows partition order.
class CommandBufferGroup {
  public:
    explicit CommandBufferGroup(EntityStore &store) : store_(&store) {
    }
    void prepare(std::size_t partitions);
    CommandBuffer &operator[](std::size_t partition) noexcept {
        return *buffers_[partition];
    }
    void playback();
    void clear() noexcept;
    std::size_t size() const noexcept {
        return active_;
    }

  private:
    EntityStore *store_;
    std::vector<std::unique_ptr<CommandBuffer>> buffers_;
    std::size_t active_{0};
};

} // namespace shinkou::ecs
