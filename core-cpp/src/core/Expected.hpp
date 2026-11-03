#pragma once

// Минимальный аналог std::expected для C++20 (std::expected требует C++23).
// Подмножество API, совместимое по именам: has_value / operator bool / value /
// operator* / operator-> / error, плюс специализация для void.

#include <concepts>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace dropship {

template <class E>
struct Unexpected {
    E error;
};

template <class E>
Unexpected<std::decay_t<E>> unexpected(E&& e) {
    return Unexpected<std::decay_t<E>>{std::forward<E>(e)};
}

template <class T, class E>
class Expected {
public:
    Expected(Unexpected<E> u) : data_(std::in_place_index<1>, std::move(u.error)) {}

    template <class U>
        requires(std::constructible_from<T, U &&> &&
                 !std::same_as<std::remove_cvref_t<U>, Expected> &&
                 !std::same_as<std::remove_cvref_t<U>, Unexpected<E>>)
    Expected(U&& value) : data_(std::in_place_index<0>, std::forward<U>(value)) {}

    bool has_value() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & { return std::get<0>(data_); }
    const T& value() const& { return std::get<0>(data_); }
    T&& value() && { return std::get<0>(std::move(data_)); }

    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(*this).value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    E& error() & { return std::get<1>(data_); }
    const E& error() const& { return std::get<1>(data_); }

private:
    std::variant<T, E> data_;
};

template <class E>
class Expected<void, E> {
public:
    Expected() = default;
    Expected(Unexpected<E> u) : error_(std::move(u.error)) {}

    bool has_value() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return has_value(); }

    E& error() & { return *error_; }
    const E& error() const& { return *error_; }

private:
    std::optional<E> error_;
};

}  // namespace dropship
