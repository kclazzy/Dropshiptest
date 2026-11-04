#pragma once

// Общие доменные типы. Одно решение — одно место: имена перечислений,
// которые хранятся в SQLite как TEXT, определены только здесь.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace dropship {

// Деньги всегда хранятся в минимальных неделимых единицах (центы, копейки).
using Cents = std::int64_t;
using UnixSeconds = std::int64_t;

namespace detail {

template <class E>
struct EnumName {
    E value;
    std::string_view name;
};

template <class E, std::size_t N>
constexpr std::optional<E> parseEnum(const std::array<EnumName<E>, N>& table,
                                     std::string_view text) noexcept {
    for (const auto& entry : table) {
        if (entry.name == text) return entry.value;
    }
    return std::nullopt;
}

template <class E, std::size_t N>
constexpr std::string_view nameOf(const std::array<EnumName<E>, N>& table, E value) noexcept {
    for (const auto& entry : table) {
        if (entry.value == value) return entry.name;
    }
    return {};
}

}  // namespace detail

// Площадка-витрина (stores.platform_type).
enum class Platform { Local, Shopify, WooCommerce, Ebay, Ozon, Avito, Wildberries, YandexMarket };

inline constexpr std::array<detail::EnumName<Platform>, 8> kPlatformNames{{
    {Platform::Local, "local"},  // каталог без публикации на площадку
    {Platform::Shopify, "shopify"},
    {Platform::WooCommerce, "woocommerce"},
    {Platform::Ebay, "ebay"},
    {Platform::Ozon, "ozon"},
    {Platform::Avito, "avito"},
    {Platform::Wildberries, "wildberries"},
    {Platform::YandexMarket, "yandex_market"},
}};

constexpr std::string_view toString(Platform p) noexcept {
    return detail::nameOf(kPlatformNames, p);
}
constexpr std::optional<Platform> parsePlatform(std::string_view s) noexcept {
    return detail::parseEnum(kPlatformNames, s);
}

// Тип задачи в task_queue (task_queue.task_type).
enum class TaskType { AiRewrite, StorePublish, PriceSync, AutoOrder, TrackingSync };

inline constexpr std::array<detail::EnumName<TaskType>, 5> kTaskTypeNames{{
    {TaskType::AiRewrite, "AI_REWRITE"},
    {TaskType::StorePublish, "STORE_PUBLISH"},
    {TaskType::PriceSync, "PRICE_SYNC"},
    {TaskType::AutoOrder, "AUTO_ORDER"},
    {TaskType::TrackingSync, "TRACKING_SYNC"},
}};

constexpr std::string_view toString(TaskType t) noexcept {
    return detail::nameOf(kTaskTypeNames, t);
}
constexpr std::optional<TaskType> parseTaskType(std::string_view s) noexcept {
    return detail::parseEnum(kTaskTypeNames, s);
}

}  // namespace dropship
