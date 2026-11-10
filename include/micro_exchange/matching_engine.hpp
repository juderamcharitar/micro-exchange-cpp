#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace micro_exchange {

using OrderId = std::uint64_t;
using Quantity = std::uint64_t;
using PriceTicks = std::int64_t;

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };
enum class TimeInForce { GTC, IOC };

struct NewOrder {
    OrderId id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    PriceTicks limit_price_ticks{};  // 0 for MARKET; integer ticks, NOT dollars
    Quantity quantity{};
    TimeInForce tif{TimeInForce::GTC};
};

struct Execution {
    std::uint64_t sequence{};
    OrderId maker_id{};
    OrderId taker_id{};
    PriceTicks price_ticks{};  // trade at passive/resting order price
    Quantity quantity{};
};

struct SubmitResult {
    bool accepted{};
    std::string reason;
    std::vector<Execution> executions;
    Quantity remaining_quantity{};  // cancelled remainder for IOC; resting for GTC limit
    bool resting{};
};

struct CancelResult {
    bool cancelled{};
    std::string reason;
    Quantity removed_quantity{};
};

struct LevelView {
    PriceTicks price_ticks{};
    Quantity aggregate_quantity{};
    std::size_t order_count{};
};

struct BookSnapshot {
    std::vector<LevelView> bids;  // best to worst, descending
    std::vector<LevelView> asks;  // best to worst, ascending
};

struct RestingOrderView {
    Side side;
    PriceTicks price_ticks;
    Quantity leaves;
};

struct EngineStats {
    std::uint64_t accepted_new{};
    std::uint64_t cancelled{};
    std::uint64_t replaced{};
    std::uint64_t rejected{};
    std::uint64_t trades{};
    Quantity executed_quantity{};
    std::size_t resting_orders{};
};

class MatchingEngine {
public:
    explicit MatchingEngine(Quantity max_order_quantity = 1'000'000'000,
                            PriceTicks max_price_ticks = 1'000'000'000'000);

    // Strict single-instrument, in-memory, deterministic simulation.
    SubmitResult submit(const NewOrder& order);
    CancelResult cancel(OrderId id);

    // New quantity means NEW REMAINING quantity, not original order quantity.
    // Same price + quantity decrease/equality RETAINS queue priority.
    // Price change or quantity increase LOSES queue priority, re-enters as taker.
    SubmitResult replace(OrderId id, PriceTicks new_price_ticks,
                         Quantity new_remaining_quantity);

    // Preview potential self-matching without changing book, FIFO, or counters.
    // owner_for_id returns 0 for unknown orders.
    bool would_self_trade(const NewOrder& incoming, std::uint64_t taker_owner,
                          const std::function<std::uint64_t(OrderId)>& owner_for_id) const;

    BookSnapshot snapshot(std::size_t depth = 5) const;
    EngineStats stats() const;
    std::optional<RestingOrderView> find_resting(OrderId id) const;
    bool check_invariants(std::string* error = nullptr) const;

private:
    struct RestingOrder {
        OrderId id;
        Quantity leaves;
    };
    using Level = std::list<RestingOrder>;
    using Bids = std::map<PriceTicks, Level, std::greater<PriceTicks>>;
    using Asks = std::map<PriceTicks, Level, std::less<PriceTicks>>;
    struct LiveOrder {
        Side side;
        PriceTicks price_ticks;
        Level::iterator position;
    };

    bool validate(const NewOrder& order, std::string& reason) const;
    SubmitResult submit_validated(const NewOrder& order);
    void erase_live(OrderId id, const LiveOrder& location);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, LiveOrder> live_;
    std::unordered_set<OrderId> used_ids_;
    Quantity max_order_quantity_;
    PriceTicks max_price_ticks_;
    std::uint64_t next_execution_sequence_{1};
    EngineStats stats_;
};

}  // namespace micro_exchange
