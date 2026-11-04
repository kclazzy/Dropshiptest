#include "FakeTransport.hpp"
#include "Harness.hpp"
#include "update/Updater.hpp"

using namespace dropship;
using namespace dropship::update;

TEST(updater_parses_versions_and_rejects_garbage) {
    CHECK(parseVersion("1.2.3") == (Version{1, 2, 3}));
    CHECK(parseVersion("v0.10.0") == (Version{0, 10, 0}));
    CHECK(parseVersion("2.1") == (Version{2, 1, 0}));
    CHECK(parseVersion("1.2.3-beta.1") == (Version{1, 2, 3}));
    for (const char* bad : {"", "v", "a.b.c", "1.2.3.4", "1..2", "1.2.", "-1.0.0", "1.2.x", "9999999.0.0"}) {
        CHECK(!parseVersion(bad).has_value());
    }
}

TEST(updater_orders_versions_numerically_not_textually) {
    CHECK(*parseVersion("1.10.0") > *parseVersion("1.9.9"));
    CHECK(*parseVersion("2.0.0") > *parseVersion("1.99.99"));
    CHECK(*parseVersion("1.0.0") == *parseVersion("v1"));
}

TEST(updater_detects_a_newer_release) {
    const auto info = evaluateRelease(Json{{"tag_name", "v0.2.0"}, {"html_url", "https://github.com/o/r/releases/v0.2.0"},
                                           {"body", "Что нового"}},
                                      "0.1.0");
    REQUIRE(info.has_value());
    CHECK(info->updateAvailable);
    CHECK_EQ(info->latest, std::string("0.2.0"));
    CHECK_EQ(info->current, std::string("0.1.0"));
    CHECK_EQ(info->notes, std::string("Что нового"));
}

TEST(updater_ignores_same_older_draft_and_prerelease) {
    CHECK(!evaluateRelease(Json{{"tag_name", "v0.1.0"}}, "0.1.0")->updateAvailable);
    CHECK(!evaluateRelease(Json{{"tag_name", "v0.0.9"}}, "0.1.0")->updateAvailable);
    CHECK(!evaluateRelease(Json{{"tag_name", "v9.0.0"}, {"draft", true}}, "0.1.0")->updateAvailable);
    CHECK(!evaluateRelease(Json{{"tag_name", "v9.0.0"}, {"prerelease", true}}, "0.1.0")->updateAvailable);
    CHECK(!evaluateRelease(Json{{"tag_name", "nightly"}}, "0.1.0")->updateAvailable);
}

TEST(updater_queries_github_for_the_latest_release) {
    testfakes::FakeTransport transport;
    transport.reply(200, R"({"tag_name":"v1.0.0","html_url":"https://x"})");
    const auto info = checkForUpdate(transport, "kclazzy/Dropshiptest", "0.1.0");
    REQUIRE(info.has_value());
    CHECK(info->updateAvailable);
    REQUIRE(transport.requests.size() == 1);
    CHECK_EQ(transport.requests[0].url, std::string("https://api.github.com/repos/kclazzy/Dropshiptest/releases/latest"));
}

TEST(updater_treats_missing_releases_as_up_to_date_and_reports_real_failures) {
    testfakes::FakeTransport transport;
    transport.reply(404, "{}");
    CHECK(!checkForUpdate(transport, "o/r", "0.1.0")->updateAvailable);
    transport.reply(500, "oops");
    CHECK(!checkForUpdate(transport, "o/r", "0.1.0").has_value());
    transport.reply(200, "not json");
    CHECK(!checkForUpdate(transport, "o/r", "0.1.0").has_value());
    transport.fail("offline");
    CHECK(!checkForUpdate(transport, "o/r", "0.1.0").has_value());
}

TEST(updater_refuses_suspicious_repository_names) {
    testfakes::FakeTransport transport;
    for (const char* bad : {"", "norepo", "a/b/c", "a/../b", "o/r?x=1", "o/r#", "o r/x", "/r", "o/"}) {
        CHECK(!checkForUpdate(transport, bad, "0.1.0").has_value());
    }
    CHECK(transport.requests.empty());
}
