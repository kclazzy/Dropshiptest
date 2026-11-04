#include "Harness.hpp"
#include "ai/ContentSanitizer.hpp"

using dropship::ai::sanitizeHtml;

namespace {
std::string s(const char* text) { return sanitizeHtml(text); }
}  // namespace

TEST(sanitize_keeps_allowed_formatting) {
    CHECK_EQ(s("<p>Hello <strong>world</strong></p><ul><li>a</li></ul>"),
             std::string("<p>Hello <strong>world</strong></p><ul><li>a</li></ul>"));
}

TEST(sanitize_normalizes_case_and_strips_attributes) {
    CHECK_EQ(s("<P CLASS=\"x\" onclick=\"evil()\">Hi</P>"), std::string("<p>Hi</p>"));
    CHECK_EQ(s("</p onclick=x>"), std::string("</p>"));
}

TEST(sanitize_drops_script_and_style_with_their_content) {
    CHECK_EQ(s("<p>a</p><script>alert(1)</script><style>p{}</style><p>b</p>"), std::string("<p>a</p><p>b</p>"));
    CHECK_EQ(s("<p>a</p><SCRIPT type=x>alert(1)</ScRiPt>b"), std::string("<p>a</p>b"));
    CHECK_EQ(s("<p>a</p><script>alert(1)"), std::string("<p>a</p>"));  // не закрыт — остаток отбрасывается
    CHECK_EQ(s("<iframe src=x>fallback</iframe>ok"), std::string("ok"));
}

TEST(sanitize_unwraps_disallowed_tags_but_keeps_text) {
    CHECK_EQ(s("<div><span>text</span> <a href=\"javascript:alert(1)\">link</a><img src=x onerror=alert(1)></div>"),
             std::string("text link"));
}

TEST(sanitize_respects_quotes_inside_tags) {
    CHECK_EQ(s("<a title=\"a>b\">x</a>"), std::string("x"));
}

TEST(sanitize_removes_comments_and_doctype) {
    CHECK_EQ(s("a<!-- <b>x</b> -->b"), std::string("ab"));
    CHECK_EQ(s("<!DOCTYPE html><p>x</p>"), std::string("<p>x</p>"));
    CHECK_EQ(s("a<!-- never closed <b>"), std::string("a"));
}

TEST(sanitize_escapes_stray_angle_brackets_and_ampersands) {
    CHECK_EQ(s("5 < 6 & 7 > 3"), std::string("5 &lt; 6 &amp; 7 &gt; 3"));
    CHECK_EQ(s("<p>ok</p><b"), std::string("<p>ok</p>&lt;b"));
}

TEST(sanitize_keeps_existing_entities) {
    CHECK_EQ(s("Tom &amp; Jerry &#39;x&#39; &#x41; &nbsp;"), std::string("Tom &amp; Jerry &#39;x&#39; &#x41; &nbsp;"));
    CHECK_EQ(s("AT&T"), std::string("AT&amp;T"));
}

TEST(sanitize_normalizes_br) {
    CHECK_EQ(s("a<br/>b<BR />c</br>d"), std::string("a<br>b<br>cd"));
}

TEST(sanitize_handles_empty_and_plain_text_and_utf8) {
    CHECK_EQ(s(""), std::string());
    CHECK_EQ(s("Беспроводные наушники"), std::string("Беспроводные наушники"));
}
