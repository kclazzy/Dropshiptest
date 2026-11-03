#include "Harness.hpp"
#include "core/Domain.hpp"

using namespace dropship;

// Эти строки хранятся в БД: переименование сломает существующие данные.
TEST(domain_platform_names_are_stable) {
    CHECK_EQ(toString(Platform::Ozon), std::string_view{"ozon"});
    CHECK_EQ(toString(Platform::Avito), std::string_view{"avito"});
    CHECK_EQ(toString(Platform::YandexMarket), std::string_view{"yandex_market"});
    for (const auto& entry : kPlatformNames) {
        const auto parsed = parsePlatform(entry.name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == entry.value);
    }
    CHECK(!parsePlatform("amazon").has_value());
    CHECK(!parsePlatform("").has_value());
}

TEST(domain_task_type_names_are_stable) {
    CHECK_EQ(toString(TaskType::AiRewrite), std::string_view{"AI_REWRITE"});
    CHECK_EQ(toString(TaskType::AutoOrder), std::string_view{"AUTO_ORDER"});
    for (const auto& entry : kTaskTypeNames) {
        const auto parsed = parseTaskType(entry.name);
        REQUIRE(parsed.has_value());
        CHECK(*parsed == entry.value);
    }
    CHECK(!parseTaskType("ai_rewrite").has_value());
}
