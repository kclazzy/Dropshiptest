#include "Harness.hpp"
#include "monitor/PageParser.hpp"

using namespace dropship;
using namespace dropship::monitor;

namespace {
std::string page(const std::string& head) { return "<html><head>" + head + "</head><body>x</body></html>"; }
std::string ld(const std::string& json) { return R"(<script type="application/ld+json">)" + json + "</script>"; }
}  // namespace

TEST(parser_prices_in_many_formats) {
    CHECK_EQ(*parsePriceToCents("1 234,56 ₽"), Cents{123456});
    CHECK_EQ(*parsePriceToCents("1,234.56"), Cents{123456});
    CHECK_EQ(*parsePriceToCents("1.234,5"), Cents{123450});
    CHECK_EQ(*parsePriceToCents("1 200"), Cents{120000});
    CHECK_EQ(*parsePriceToCents("0,99"), Cents{99});
    CHECK(!parsePriceToCents("abc").has_value());
    CHECK(!parsePriceToCents("").has_value());
    CHECK(!parsePriceToCents("99999999999999999999").has_value());
}

TEST(parser_reads_offers_as_variants_with_stock) {
    const auto html = page(ld(R"({"@type":"Product","name":"X","offers":[
        {"sku":"B","price":"1999.50","availability":"https://schema.org/InStock"},
        {"sku":"W","price":2100,"availability":"https://schema.org/OutOfStock"},
        {"sku":"R","price":"2200","inventoryLevel":{"value":3}}]})"));
    const auto r = parseProductPage(html);
    REQUIRE(r.has_value());
    REQUIRE(r->size() == 3);
    CHECK_EQ((*r)[0].sku, std::string("B"));
    CHECK_EQ((*r)[0].priceCents, Cents{199950});
    CHECK_EQ((*r)[0].stock, kDefaultInStockQuantity);
    CHECK_EQ((*r)[1].stock, std::int64_t{0});
    CHECK_EQ((*r)[2].stock, std::int64_t{3});
}

TEST(parser_finds_product_inside_graph_and_skips_broken_blocks) {
    const auto html = page(R"(<script type="application/ld+json">{oops</script>)" +
                           ld(R"({"@graph":[{"@type":"WebSite"},{"@type":["Thing","Product"],"offers":{"price":"5"}}]})"));
    const auto r = parseProductPage(html);
    REQUIRE(r.has_value());
    CHECK_EQ((*r)[0].priceCents, Cents{500});
}

TEST(parser_falls_back_to_meta_price) {
    const auto r = parseProductPage(page(R"(<meta property="product:price:amount" content="99,90">)"));
    REQUIRE(r.has_value());
    CHECK_EQ((*r)[0].priceCents, Cents{9990});
    CHECK_EQ((*r)[0].stock, kDefaultInStockQuantity);
}

TEST(parser_meta_availability_out_of_stock_gives_zero) {
    const auto r = parseProductPage(page(R"(<meta property="product:price:amount" content="10">)"
                                         R"(<meta property="product:availability" content="out of stock">)"));
    REQUIRE(r.has_value());
    CHECK_EQ((*r)[0].stock, std::int64_t{0});
}

TEST(parser_reports_pages_without_price) {
    CHECK(!parseProductPage("<html></html>").has_value());
    CHECK(!parseProductPage(page(ld(R"({"@type":"Product","name":"No price"})"))).has_value());
    CHECK(!parseProductPage("").has_value());
}

TEST(parser_is_robust_to_unterminated_and_hostile_input) {
    CHECK(!parseProductPage(R"(<script type="application/ld+json">{"@type":"Product")").has_value());
    CHECK(!parseProductPage(std::string(100000, '<')).has_value());
    std::string deep(100000, '[');
    CHECK(!parseProductPage(page(ld(deep))).has_value());  // глубокая вложенность не роняет разбор
}
