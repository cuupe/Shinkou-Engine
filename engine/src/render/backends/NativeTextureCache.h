#pragma once
#include "BackendCommon.h"
#include <list>

namespace shinkou::render {
// Only GPU-only textures are reused. Upload buffers, immutable initial data
// and frame parameters keep their existing retirement path. Reuse does not
// reset native layout/state: transitions continue from the last submitted use.
template <class Record> class NativeTextureCache {
    struct Key {
        std::string value;
        std::size_t bytes{0};
    };
    struct Entry {
        Key key;
        Record record;
    };
    std::list<Entry> available_;
    std::unordered_map<std::uint32_t, Key> live_;
    std::size_t bytes_{0};
    static Key key(const ResourceDesc &description) {
        const auto *texture = std::get_if<TextureDesc>(&description);
        if (!texture || !texture->initialData.empty() || !texture->initialSubresources.empty() ||
            texture->generateMips)
            return {};
        const auto format = texture->formatKind == TextureFormat::Unknown
                                ? texture_format_from_name(texture->format)
                                : texture->formatKind;
        const long double estimate = static_cast<long double>(texture->width) * texture->height *
                                     texture->layers * std::max(1u, texture->depth) *
                                     texture->sampleCount *
                                     (format == TextureFormat::RGBA16Float ? 8 : 4) * 2;
        if (estimate > 64u * 1024u * 1024u || estimate <= 0)
            return {};
        std::string value = texture->format + ":" + texture->colorSpace;
        const auto append = [&](auto field) {
            value += ':' + std::to_string(static_cast<unsigned long long>(field));
        };
        append(texture->width);
        append(texture->height);
        append(texture->layers);
        append(texture->mipLevels);
        append(texture->formatKind);
        append(texture->dimension);
        append(texture->depth);
        append(texture->sampleCount);
        append(texture->renderTarget);
        append(texture->depthStencil);
        append(texture->storage);
        append(texture->hdr);
        append(texture->resolve);
        append(texture->resolveMode);
        return {std::move(value), static_cast<std::size_t>(estimate)};
    }

  public:
    void forget(ResourceHandle handle) {
        live_.erase(handle.id);
    }
    std::optional<Record> take(ResourceHandle handle, const ResourceDesc &description) {
        const auto requested = key(description);
        if (requested.value.empty())
            return std::nullopt;
        const auto found =
            std::find_if(available_.begin(), available_.end(), [&](const auto &entry) {
                return entry.key.value == requested.value && entry.record.kind == handle.kind;
            });
        if (found == available_.end())
            return std::nullopt;
        bytes_ -= found->key.bytes;
        std::optional<Record> record{std::in_place, std::move(found->record)};
        available_.erase(found);
        live_[handle.id] = requested;
        return record;
    }
    void track(ResourceHandle handle, const ResourceDesc &description) {
        auto requested = key(description);
        if (!requested.value.empty())
            live_[handle.id] = std::move(requested);
    }
    bool recycle(ResourceHandle handle, Record &record) {
        const auto found = live_.find(handle.id);
        if (found == live_.end())
            return false;
        auto requested = std::move(found->second);
        live_.erase(found);
        if (available_.size() >= 64 || requested.bytes > 256u * 1024u * 1024u - bytes_)
            return false;
        bytes_ += requested.bytes;
        available_.push_back({std::move(requested), std::move(record)});
        return true;
    }
    template <class Release> void clear(Release release) {
        for (auto &entry : available_)
            release(entry.record);
        available_.clear();
        live_.clear();
        bytes_ = 0;
    }
};
} // namespace shinkou::render
