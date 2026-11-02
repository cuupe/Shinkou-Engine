#include "shinkou/ecs/CommandBuffer.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace shinkou::ecs {

CommandBuffer::CommandBuffer(EntityStore &store) : store_(&store) {
}

CommandBuffer::~CommandBuffer() {
    clear();
}

void *CommandBuffer::allocate_payload(std::size_t size, std::size_t alignment) {
    for (;;) {
        if (currentBlock_ == payloadBlocks_.size()) {
            const auto capacity = std::max<std::size_t>(16u * 1024u, size + alignment - 1u);
            payloadBlocks_.push_back({std::make_unique<std::uint8_t[]>(capacity), capacity, 0});
        }
        auto &block = payloadBlocks_[currentBlock_];
        void *pointer = block.data.get() + block.used;
        auto space = block.capacity - block.used;
        if (std::align(alignment, size, pointer, space)) {
            block.used = static_cast<std::uint8_t *>(pointer) - block.data.get() + size;
            return pointer;
        }
        ++currentBlock_;
    }
}

std::size_t CommandBuffer::payload_capacity() const noexcept {
    std::size_t capacity = 0;
    for (const auto &block : payloadBlocks_)
        capacity += block.capacity;
    return capacity;
}

Entity CommandBuffer::create_entity() {
    const Entity entity = placeholder();
    commands_.push_back(Command{Kind::Create, entity, nullptr, nullptr, nullptr});
    return entity;
}

void CommandBuffer::destroy_entity(Entity entity) {
    commands_.push_back(Command{Kind::Destroy, entity, nullptr, nullptr, nullptr});
}

Entity CommandBuffer::resolve(Entity entity) const noexcept {
    if (entity.id < kPlaceholderBase)
        return entity;
    const auto index = entity.id - kPlaceholderBase;
    if (index >= created_.size())
        return Entity{};
    return created_[index];
}

void CommandBuffer::playback() {
    for (const auto &command : commands_) {
        switch (command.kind) {
        case Kind::Create:
            created_.push_back(store_->create());
            break;
        case Kind::Destroy: {
            const auto target = resolve(command.entity);
            if (store_->valid(target))
                store_->destroy(target);
            break;
        }
        case Kind::Add:
        case Kind::Remove: {
            const auto target = resolve(command.entity);
            if (store_->valid(target) && command.apply)
                command.apply(*store_, target, command.payload);
            break;
        }
        }
    }
    clear();
}

void CommandBuffer::clear() noexcept {
    for (auto &command : commands_) {
        if (command.release && command.payload)
            command.release(command.payload);
        command.payload = nullptr;
    }
    commands_.clear();
    created_.clear();
    nextPlaceholder_ = 0;
    for (auto &block : payloadBlocks_)
        block.used = 0;
    currentBlock_ = 0;
}

void CommandBufferGroup::prepare(std::size_t partitions) {
    for (const auto &buffer : buffers_)
        if (!buffer->empty())
            throw std::logic_error(
                "play back or clear recorded commands before reusing partitions");
    while (buffers_.size() < partitions)
        buffers_.push_back(std::make_unique<CommandBuffer>(*store_));
    active_ = partitions;
}

void CommandBufferGroup::playback() {
    for (std::size_t i = 0; i < active_; ++i)
        buffers_[i]->playback();
}

void CommandBufferGroup::clear() noexcept {
    for (auto &buffer : buffers_)
        buffer->clear();
}

} // namespace shinkou::ecs
