#include "app/Cli.hpp"

#include <algorithm>
#include <iostream>
#include <iterator>
#include <optional>

#include "core/Json.hpp"
#include "orders/AutoOrder.hpp"
#include "orders/Orders.hpp"
#include "orders/Tracking.hpp"

namespace dropship::cli {

namespace {

constexpr const char* kCommands[] = {"store-add", "store-list", "variants", "link", "order-add", "orders", "order-done", "order-track", "order-retry", "help"};

std::optional<std::string> option(const std::vector<std::string>& args, const std::string& name) {
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == name) return args[i + 1];
    }
    return std::nullopt;
}

std::optional<std::int64_t> parseId(const std::optional<std::string>& text) {
    if (!text || text->empty() || text->size() > 18) return std::nullopt;
    if (!std::all_of(text->begin(), text->end(), [](unsigned char c) { return std::isdigit(c); })) return std::nullopt;
    const auto value = std::stoll(*text);
    return value > 0 ? std::optional<std::int64_t>(value) : std::nullopt;
}

bool nonEmptyString(const Json& o, const char* key) { return o.contains(key) && o[key].is_string() && !o[key].get<std::string>().empty(); }

// Пустая строка — всё в порядке, иначе описание проблемы.
std::string validateCredentials(const std::string& platform, const Json& creds) {
    if (!creds.is_object()) return "credentials must be a JSON object";
    if (platform == "ozon") {
        if (!nonEmptyString(creds, "client_id") || !nonEmptyString(creds, "api_key")) return "ozon needs client_id and api_key (strings)";
        if (!creds.contains("warehouse_id") || !creds["warehouse_id"].is_number_integer() || creds["warehouse_id"].get<std::int64_t>() <= 0) {
            return "ozon needs warehouse_id (positive integer): the FBS warehouse for stock";
        }
    } else if (platform == "avito") {
        if (!nonEmptyString(creds, "client_id") || !nonEmptyString(creds, "client_secret")) return "avito needs client_id and client_secret (strings)";
    }
    return {};
}

void printHelp(std::ostream& out) {
    out << "Commands:\n"
           "  store-add --name NAME --platform ozon|avito|local   (credentials JSON on stdin)\n"
           "  store-list\n"
           "  variants [--product ID]\n"
           "  link --variant ID --external OFFER_ID_OR_ITEM_ID\n"
           "  order-add --store ID --id ORDER_ID --offer OFFER_ID --qty N --total 1449.90   (address JSON on stdin)\n"
           "  orders [--status NEW|QUEUED_FOR_EXT|ORDERED|SHIPPED|MANUAL_REVIEW|ERROR]\n"
           "  order-done --order ID --supplier-order NUMBER\n"
           "  order-track --order ID --number TRACK [--carrier NAME]\n"
           "  order-retry --order ID\n";
}

int storeAdd(const std::vector<std::string>& args, db::DatabaseManager& database, const store::CredentialVault& vault,
             std::istream& in, std::ostream& out, std::ostream& err, UnixSeconds now) {
    const auto name = option(args, "--name");
    const auto platform = option(args, "--platform");
    if (!name || name->empty() || !platform) {
        err << "usage: store-add --name NAME --platform ozon|avito|local  (credentials JSON on stdin)\n";
        return 2;
    }
    if (!parsePlatform(*platform)) {
        err << "unknown platform: " << *platform << "\n";
        return 2;
    }
    std::vector<std::uint8_t> blob;
    if (*platform != "local") {
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const Json creds = parseJson(text);
        if (creds.is_discarded()) {
            err << "credentials on stdin are not valid JSON\n";
            return 2;
        }
        if (*platform != "ozon" && *platform != "avito") {
            err << "no adapter for platform '" << *platform << "' yet (supported: ozon, avito, local)\n";
            return 2;
        }
        if (const auto problem = validateCredentials(*platform, creds); !problem.empty()) {
            err << problem << "\n";
            return 2;
        }
        auto encrypted = vault.encrypt(dumpJson(creds));
        if (!encrypted) {
            err << "cannot encrypt credentials: " << encrypted.error() << "\n";
            return 1;
        }
        blob = std::move(*encrypted);
    }
    const auto inserted = database.execute(
        "INSERT INTO stores(name, platform_type, api_url, encrypted_credentials, created_at) VALUES(?, ?, '', ?, ?)",
        {*name, *platform, db::Blob(blob.begin(), blob.end()), now});
    if (!inserted) {
        err << "database error: " << inserted.error().message << "\n";
        return 1;
    }
    out << "Store added, id " << inserted->lastInsertRowId << "\n";
    return 0;
}

int storeList(db::DatabaseManager& database, std::ostream& out, std::ostream& err) {
    const auto rows = database.query("SELECT id, name, platform_type, is_active, last_error FROM stores ORDER BY id");
    if (!rows) {
        err << "database error: " << rows.error().message << "\n";
        return 1;
    }
    for (std::size_t i = 0; i < rows->rows.size(); ++i) {
        out << rows->intAt(i, "id") << "\t" << rows->textAt(i, "name") << "\t" << rows->textAt(i, "platform_type") << "\t"
            << (rows->intAt(i, "is_active") ? "active" : "disabled");
        if (!rows->isNull(i, "last_error")) out << "\tERROR: " << rows->textAt(i, "last_error");
        out << "\n";
    }
    return 0;
}

int variants(const std::vector<std::string>& args, db::DatabaseManager& database, std::ostream& out, std::ostream& err) {
    const auto product = option(args, "--product");
    if (product && !parseId(product)) {
        err << "--product must be a positive number\n";
        return 2;
    }
    const auto rows = database.query(
        "SELECT v.id, v.product_id, v.supplier_sku, COALESCE(v.external_variant_id, '') AS ext, v.supplier_price_cents, "
        "v.retail_price_cents, v.supplier_stock, COALESCE(p.title_override, p.ai_seo_title, p.raw_title) AS title "
        "FROM product_variants v JOIN products p ON p.id = v.product_id WHERE (? = 0 OR v.product_id = ?) ORDER BY v.id",
        {parseId(product).value_or(0), parseId(product).value_or(0)});
    if (!rows) {
        err << "database error: " << rows.error().message << "\n";
        return 1;
    }
    out << "variant\tproduct\tsku\tlinked_to\tsupplier_price\tretail_price\tstock\ttitle\n";
    for (std::size_t i = 0; i < rows->rows.size(); ++i) {
        out << rows->intAt(i, "id") << "\t" << rows->intAt(i, "product_id") << "\t" << rows->textAt(i, "supplier_sku") << "\t"
            << (rows->textAt(i, "ext").empty() ? "-" : rows->textAt(i, "ext")) << "\t" << rows->intAt(i, "supplier_price_cents") / 100.0
            << "\t" << rows->intAt(i, "retail_price_cents") / 100.0 << "\t" << rows->intAt(i, "supplier_stock") << "\t"
            << rows->textAt(i, "title") << "\n";
    }
    return 0;
}

int link(const std::vector<std::string>& args, db::DatabaseManager& database, std::ostream& out, std::ostream& err) {
    const auto variant = parseId(option(args, "--variant"));
    const auto external = option(args, "--external");
    if (!variant || !external) {
        err << "usage: link --variant ID --external OFFER_ID  (use '-' to unlink)\n";
        return 2;
    }
    if (external->size() > 200 || external->find_first_of("\r\n\t") != std::string::npos) {
        err << "invalid external id\n";
        return 2;
    }
    const bool unlink = *external == "-";
    const auto updated = database.execute("UPDATE product_variants SET external_variant_id = ? WHERE id = ?",
                                          {unlink ? db::Value{std::monostate{}} : db::Value{*external}, *variant});
    if (!updated) {
        err << "database error: " << updated.error().message << "\n";
        return 1;
    }
    if (updated->changes == 0) {
        err << "variant " << *variant << " not found\n";
        return 1;
    }
    out << (unlink ? "Unlinked" : "Linked") << " variant " << *variant << "\n";
    return 0;
}

// "1449.90" -> 144990; не больше двух знаков после точки.
std::optional<Cents> parseMoney(const std::string& text) {
    if (text.empty() || text.size() > 15) return std::nullopt;
    const auto dot = text.find('.');
    const std::string whole = text.substr(0, dot);
    const std::string frac = dot == std::string::npos ? "" : text.substr(dot + 1);
    const auto digits = [](const std::string& t) { return std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); }); };
    if (whole.empty() || !digits(whole) || !digits(frac) || frac.size() > 2) return std::nullopt;
    return std::stoll(whole) * 100 + (frac.empty() ? 0 : std::stoll(frac + std::string(2 - frac.size(), '0')));
}

int orderAdd(const std::vector<std::string>& args, db::DatabaseManager& database, std::istream& in, std::ostream& out,
             std::ostream& err, UnixSeconds now) {
    const auto store = parseId(option(args, "--store"));
    const auto id = option(args, "--id");
    const auto offer = option(args, "--offer");
    const auto qty = parseId(option(args, "--qty"));
    const auto total = option(args, "--total") ? parseMoney(*option(args, "--total")) : std::nullopt;
    if (!store || !id || !offer || !qty || !total) {
        err << "usage: order-add --store ID --id ORDER_ID --offer OFFER_ID --qty N --total 1449.90  (address JSON on stdin)\n";
        return 2;
    }
    std::string address((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto result = orders::ingestOrder(database, {*store, *id, *offer, *qty, *total, address}, now);
    if (!result) {
        err << "error: " << result.error().message << "\n";
        return result.error().sqliteCode == 0 ? 2 : 1;
    }
    switch (result->status) {
        case orders::IngestStatus::Queued: out << "Order " << result->orderId << " accepted and queued for purchase\n"; break;
        case orders::IngestStatus::NeedsReview: out << "Order " << result->orderId << " needs manual review: " << result->reason << "\n"; break;
        case orders::IngestStatus::Duplicate: out << "Order " << result->orderId << " already exists, nothing changed\n"; break;
    }
    return 0;
}

int ordersList(const std::vector<std::string>& args, db::DatabaseManager& database, std::ostream& out, std::ostream& err) {
    const std::string status = option(args, "--status").value_or("");
    const auto rows = database.query(
        "SELECT id, external_order_id, status, quantity, retail_total_cents, COALESCE(net_profit_cents, 0) AS profit, "
        "COALESCE(supplier_order_id, '') AS sup, COALESCE(tracking_number, '') AS trk, COALESCE(error_message, '') AS err "
        "FROM orders WHERE (? = '' OR status = ?) ORDER BY id DESC LIMIT 200",
        {status, status});
    if (!rows) {
        err << "database error: " << rows.error().message << "\n";
        return 1;
    }
    out << "id\torder\tstatus\tqty\ttotal\tprofit\tsupplier_order\ttracking\tnote\n";
    for (std::size_t i = 0; i < rows->rows.size(); ++i) {
        out << rows->intAt(i, "id") << "\t" << rows->textAt(i, "external_order_id") << "\t" << rows->textAt(i, "status") << "\t"
            << rows->intAt(i, "quantity") << "\t" << rows->intAt(i, "retail_total_cents") / 100.0 << "\t" << rows->intAt(i, "profit") / 100.0
            << "\t" << rows->textAt(i, "sup") << "\t" << rows->textAt(i, "trk") << "\t" << rows->textAt(i, "err") << "\n";
    }
    return 0;
}

int orderDone(const std::vector<std::string>& args, db::DatabaseManager& database, std::ostream& out, std::ostream& err,
              UnixSeconds now) {
    const auto order = parseId(option(args, "--order"));
    const auto supplier = option(args, "--supplier-order");
    if (!order || !supplier) {
        err << "usage: order-done --order ID --supplier-order NUMBER\n";
        return 2;
    }
    const auto r = orders::applyFulfillment(database, {*order, orders::FulfillmentStatus::Ordered, *supplier, ""}, now);
    if (!r) {
        err << "error: " << r.error().message << "\n";
        return 2;
    }
    out << (*r ? "Order marked as purchased\n" : "Nothing changed (already recorded or not allowed from this status)\n");
    return 0;
}

int orderTrack(const std::vector<std::string>& args, db::DatabaseManager& database, std::ostream& out, std::ostream& err,
               UnixSeconds now) {
    const auto order = parseId(option(args, "--order"));
    const auto number = option(args, "--number");
    if (!order || !number) {
        err << "usage: order-track --order ID --number TRACK [--carrier NAME]\n";
        return 2;
    }
    const auto r = orders::applyTracking(database, *order, *number, option(args, "--carrier").value_or(""), now);
    if (!r) {
        err << "error: " << r.error().message << "\n";
        return 2;
    }
    out << (*r ? "Tracking saved, it will be sent to the store\n" : "Nothing changed (same tracking number)\n");
    return 0;
}

int orderRetry(const std::vector<std::string>& args, db::DatabaseManager& database, std::ostream& out, std::ostream& err,
               UnixSeconds now) {
    const auto order = parseId(option(args, "--order"));
    if (!order) {
        err << "usage: order-retry --order ID\n";
        return 2;
    }
    const auto r = orders::retryOrder(database, *order, now);
    if (!r) {
        err << "database error: " << r.error().message << "\n";
        return 1;
    }
    out << (*r ? "Order queued again\n" : "Order was not queued (wrong status, or the checks still fail: see `orders`)\n");
    return 0;
}

}  // namespace

bool isCommand(const std::string& arg) {
    return std::any_of(std::begin(kCommands), std::end(kCommands), [&](const char* c) { return arg == c; });
}

int run(const std::vector<std::string>& args, db::DatabaseManager& database, const store::CredentialVault& vault,
        std::istream& in, std::ostream& out, std::ostream& err, UnixSeconds now) {
    if (args.empty() || args[0] == "help") {
        printHelp(out);
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "store-add") return storeAdd(args, database, vault, in, out, err, now);
    if (args[0] == "store-list") return storeList(database, out, err);
    if (args[0] == "variants") return variants(args, database, out, err);
    if (args[0] == "link") return link(args, database, out, err);
    if (args[0] == "order-add") return orderAdd(args, database, in, out, err, now);
    if (args[0] == "orders") return ordersList(args, database, out, err);
    if (args[0] == "order-done") return orderDone(args, database, out, err, now);
    if (args[0] == "order-track") return orderTrack(args, database, out, err, now);
    if (args[0] == "order-retry") return orderRetry(args, database, out, err, now);
    printHelp(err);
    return 2;
}

}  // namespace dropship::cli
