#include "orders/OrderService.hpp"

#include <condition_variable>
#include <mutex>

#include "orders/Tracking.hpp"

namespace dropship::orders {

OrderService::OrderService(db::DatabaseManager& database, IExtensionLink& link, store::IAdapterProvider* provider,
                           std::function<UnixSeconds()> clock, std::function<void(const std::string&)> log,
                           std::chrono::seconds pollEvery) {
    thread_ = std::jthread([&database, &link, provider, clock = std::move(clock), log = std::move(log),
                            pollEvery](std::stop_token stop) {
        const auto say = [&](const std::string& text) { if (log) log(text); };
        std::mutex mutex;
        std::condition_variable_any wake;
        UnixSeconds nextPoll = 0;
        while (!stop.stop_requested()) {
            bool busy = false;
            if (provider && clock() >= nextPoll) {
                nextPoll = clock() + pollEvery.count();
                const auto polled = pollStoreOrders(database, *provider, clock());
                if (!polled) say("orders: poll error: " + polled.error().message);
                else if (polled->ingested > 0) say("orders: " + std::to_string(polled->ingested) + " new from stores");
            }
            const auto order = processNextOrderTask(database, link, clock());
            if (!order) {
                say("orders: database error: " + order.error().message);
            } else if (*order != OrderTaskOutcome::NoTask) {
                busy = true;
                switch (*order) {
                    case OrderTaskOutcome::Sent: say("orders: purchase command sent to the extension"); break;
                    case OrderTaskOutcome::Review: say("orders: an order needs manual review"); break;
                    case OrderTaskOutcome::Rejected: say("orders: extension refused, will retry"); break;
                    default: break;
                }
            }
            if (provider) {
                const auto tracking = processNextTrackingSync(database, *provider, clock());
                if (!tracking) say("orders: database error: " + tracking.error().message);
                else if (*tracking != TrackingOutcome::NoTask) {
                    busy = true;
                    if (*tracking == TrackingOutcome::Failed) say("orders: tracking was not sent (see the order note)");
                }
            }
            if (busy) continue;
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
        }
    });
}

OrderService::~OrderService() = default;

}  // namespace dropship::orders
