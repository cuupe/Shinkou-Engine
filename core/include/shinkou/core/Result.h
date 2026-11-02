#pragma once
#include "shinkou/core/Error.h"
#include <utility>
#include <variant>

namespace shinkou::core {
template<class T> class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {
        if (std::get<1>(storage_).ok()) std::get<1>(storage_).code = ErrorCode::Unknown;
    }
    bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }
    T& value() & { return std::get<0>(storage_); }
    const T& value() const & { return std::get<0>(storage_); }
    T&& value() && { return std::get<0>(std::move(storage_)); }
    Error& error() & { return std::get<1>(storage_); }
    const Error& error() const & { return std::get<1>(storage_); }
    T& operator*() & { return value(); }
    const T& operator*() const & { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
private:
    std::variant<T, Error> storage_;
};
template<> class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}
    bool ok() const noexcept { return error_.ok(); }
    explicit operator bool() const noexcept { return ok(); }
    Error& error() & { return error_; }
    const Error& error() const & { return error_; }
private:
    Error error_;
};
using Status = Result<void>;
}
