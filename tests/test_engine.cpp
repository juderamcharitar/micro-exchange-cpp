#include "micro_exchange/matching_engine.hpp"
#include "micro_exchange/sha256.hpp"

#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace micro_exchange;

namespace {

void require(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define CHECK(expr) require(static_cast<bool>(expr), #expr, __LINE__)

NewOrder buy(OrderId id, PriceTicks price, Quantity qty, TimeInForce tif = TimeInForce::GTC) {
    return {id, Side::Buy, OrderType::Limit, price, qty, tif};
}
NewOrder sell(OrderId id, PriceTicks price, Quantity qty, TimeInForce tif = TimeInForce::GTC) {
    return {id, Side::Sell, OrderType::Limit, price, qty, tif};
}
NewOrder market(OrderId id, Side side, Quantity qty) {
    return {id, side, OrderType::Market, 0, qty, TimeInForce::IOC};
}
void invariants(const MatchingEngine& e) {
    std::string why;
    CHECK(e.check_invariants(&why));
}

void test_sha256_reference_vectors() {
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

void test_fifo_same_price() {
    MatchingEngine e;
    CHECK(e.submit(sell(1, 101, 5)).resting);
    CHECK(e.submit(sell(2, 101, 7)).resting);
    const auto r = e.submit(buy(3, 101, 9));
    CHECK(r.accepted && !r.resting && r.remaining_quantity == 0);
    CHECK(r.executions.size() == 2);
    CHECK(r.executions[0].maker_id == 1 && r.executions[0].quantity == 5);
    CHECK(r.executions[1].maker_id == 2 && r.executions[1].quantity == 4);
    CHECK(r.executions[0].sequence == 1 && r.executions[1].sequence == 2);
    auto s = e.snapshot();
    CHECK(s.asks.size() == 1 && s.asks[0].aggregate_quantity == 3);
    invariants(e);
}

void test_best_price_before_time() {
    MatchingEngine e;
    e.submit(sell(1, 103, 2));
    e.submit(sell(2, 102, 2));
    e.submit(sell(3, 101, 2));
    auto r = e.submit(buy(4, 104, 5));
    CHECK(r.executions.size() == 3);
    CHECK(r.executions[0].maker_id == 3 && r.executions[0].price_ticks == 101);
    CHECK(r.executions[1].maker_id == 2 && r.executions[1].price_ticks == 102);
    CHECK(r.executions[2].maker_id == 1 && r.executions[2].price_ticks == 103);
    CHECK(r.executions[2].quantity == 1);
    invariants(e);
}

void test_bid_priority_and_passive_price() {
    MatchingEngine e;
    e.submit(buy(1, 101, 2));
    e.submit(buy(2, 104, 3));
    auto r = e.submit(sell(3, 100, 4));
    CHECK(r.executions.size() == 2);
    CHECK(r.executions[0].maker_id == 2 && r.executions[0].price_ticks == 104);
    CHECK(r.executions[1].maker_id == 1 && r.executions[1].price_ticks == 101);
    invariants(e);
}

void test_non_crossing_and_snapshot() {
    MatchingEngine e;
    e.submit(buy(1, 99, 5));
    e.submit(buy(2, 100, 3));
    e.submit(sell(3, 101, 2));
    auto b = e.snapshot(1);
    CHECK(b.bids.size() == 1 && b.bids[0].price_ticks == 100);
    CHECK(b.asks.size() == 1 && b.asks[0].price_ticks == 101);
    auto r = e.submit(buy(4, 100, 1));
    CHECK(r.executions.empty() && r.resting);
    b = e.snapshot();
    CHECK(b.bids[0].aggregate_quantity == 4 && b.bids[0].order_count == 2);
    invariants(e);
}

void test_ioc_partial_expires() {
    MatchingEngine e;
    e.submit(sell(1, 110, 2));
    auto r = e.submit(buy(2, 120, 5, TimeInForce::IOC));
    CHECK(r.executions.size() == 1 && r.remaining_quantity == 3 && !r.resting);
    CHECK(e.stats().resting_orders == 0);
    CHECK(!e.cancel(2).cancelled);
    invariants(e);
}

void test_market_never_rests() {
    MatchingEngine e;
    auto r = e.submit(market(1, Side::Buy, 3));
    CHECK(r.accepted && r.executions.empty() && r.remaining_quantity == 3 && !r.resting);
    e.submit(buy(2, 500, 2));
    r = e.submit(market(3, Side::Sell, 4));
    CHECK(r.executions.size() == 1 && r.executions[0].price_ticks == 500);
    CHECK(r.remaining_quantity == 2 && !r.resting);
    invariants(e);
}

void test_cancel_removes_exact_order() {
    MatchingEngine e;
    e.submit(buy(1, 100, 3));
    e.submit(buy(2, 100, 4));
    auto c = e.cancel(1);
    CHECK(c.cancelled && c.removed_quantity == 3);
    CHECK(!e.cancel(1).cancelled);
    auto r = e.submit(sell(3, 100, 3));
    CHECK(r.executions.size() == 1 && r.executions[0].maker_id == 2);
    invariants(e);
}

void test_duplicate_ids_including_cancelled_and_filled() {
    MatchingEngine e;
    e.submit(sell(1, 100, 2));
    CHECK(!e.submit(sell(1, 100, 2)).accepted);
    e.submit(buy(2, 100, 2));
    CHECK(!e.submit(buy(1, 100, 1)).accepted);
    e.submit(buy(3, 99, 3));
    CHECK(e.cancel(3).cancelled);
    CHECK(!e.submit(sell(3, 102, 1)).accepted);
    invariants(e);
}

void test_replace_decrease_preserves_fifo() {
    MatchingEngine e;
    e.submit(buy(1, 100, 5));
    e.submit(buy(2, 100, 5));
    const auto updated = e.replace(1, 100, 2);
    CHECK(updated.accepted && updated.reason == "replaced; priority retained");
    auto r = e.submit(sell(3, 100, 4));
    CHECK(r.executions.size() == 2);
    CHECK(r.executions[0].maker_id == 1 && r.executions[0].quantity == 2);
    CHECK(r.executions[1].maker_id == 2 && r.executions[1].quantity == 2);
    invariants(e);
}

void test_replace_increase_loses_fifo() {
    MatchingEngine e;
    e.submit(buy(1, 100, 5));
    e.submit(buy(2, 100, 5));
    const auto updated = e.replace(1, 100, 6);
    CHECK(updated.accepted && updated.reason == "replaced; priority reset");
    auto r = e.submit(sell(3, 100, 6));
    CHECK(r.executions.size() == 2);
    CHECK(r.executions[0].maker_id == 2 && r.executions[0].quantity == 5);
    CHECK(r.executions[1].maker_id == 1 && r.executions[1].quantity == 1);
    invariants(e);
}

void test_replace_price_change_reenters_matching() {
    MatchingEngine e;
    e.submit(buy(1, 90, 6));
    e.submit(sell(2, 100, 4));
    auto r = e.replace(1, 101, 6);
    CHECK(r.accepted && r.executions.size() == 1);
    CHECK(r.executions[0].maker_id == 2 && r.executions[0].taker_id == 1);
    CHECK(r.executions[0].price_ticks == 100 && r.executions[0].quantity == 4);
    CHECK(r.resting && r.remaining_quantity == 2);
    CHECK(e.snapshot().bids[0].price_ticks == 101);
    invariants(e);
}

void test_replace_bad_request_atomic() {
    MatchingEngine e;
    e.submit(buy(1, 99, 6));
    CHECK(!e.replace(1, -10, 4).accepted);
    CHECK(!e.replace(1, 100, 0).accepted);
    const auto snap = e.snapshot();
    CHECK(snap.bids.size() == 1 && snap.bids[0].price_ticks == 99);
    CHECK(snap.bids[0].aggregate_quantity == 6);
    invariants(e);
}

void test_rejection_cases() {
    MatchingEngine e(10, 1000);
    CHECK(!e.submit(buy(0, 99, 1)).accepted);
    CHECK(!e.submit(buy(1, 0, 1)).accepted);
    CHECK(!e.submit(buy(1, 1001, 1)).accepted);
    CHECK(!e.submit(buy(1, 99, 0)).accepted);
    CHECK(!e.submit(buy(1, 99, 11)).accepted);
    CHECK(!e.submit({1, Side::Buy, OrderType::Market, 0, 3, TimeInForce::GTC}).accepted);
    CHECK(!e.submit({1, Side::Sell, OrderType::Market, 1, 3, TimeInForce::IOC}).accepted);
    CHECK(e.stats().rejected == 7);
    CHECK(e.submit(buy(1, 99, 1)).accepted); // rejected IDs are not reserved
    invariants(e);
}

void test_full_fill_and_stats() {
    MatchingEngine e;
    e.submit(sell(1, 101, 5));
    auto r = e.submit(buy(2, 102, 3));
    CHECK(r.executions.size() == 1 && r.executions[0].quantity == 3);
    CHECK(!r.resting && r.remaining_quantity == 0);
    CHECK(e.stats().trades == 1 && e.stats().executed_quantity == 3);
    CHECK(e.snapshot().asks[0].aggregate_quantity == 2);
    invariants(e);
}

void test_multiple_price_level_depletion() {
    MatchingEngine e;
    e.submit(sell(1, 100, 1));
    e.submit(sell(2, 101, 1));
    e.submit(sell(3, 102, 1));
    auto r = e.submit(buy(4, 102, 3));
    CHECK(r.executions.size() == 3 && e.snapshot().asks.empty());
    invariants(e);
}

void test_replacing_unknown_is_not_new() {
    MatchingEngine e;
    CHECK(!e.replace(42, 100, 2).accepted);
    CHECK(e.submit(buy(42, 100, 2)).accepted);
    invariants(e);
}

void test_deterministic_randomized_sequences_and_invariants() {
    MatchingEngine a, b;
    std::mt19937_64 rng(20261109);
    std::vector<OrderId> prior;
    OrderId next_id = 1;
    for (int i = 0; i < 15'000; ++i) {
        const auto op = rng() % 10;
        if (op < 7 || prior.empty()) {
            const auto id = next_id++;
            const auto side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
            const auto type = (rng() % 5 == 0) ? OrderType::Market : OrderType::Limit;
            const PriceTicks price = type == OrderType::Market ? 0 : 90 + static_cast<PriceTicks>(rng() % 21);
            const Quantity qty = 1 + rng() % 20;
            const TimeInForce tif = type == OrderType::Market || rng() % 3 == 0
                ? TimeInForce::IOC : TimeInForce::GTC;
            NewOrder o{id, side, type, price, qty, tif};
            auto x = a.submit(o), y = b.submit(o);
            CHECK(x.accepted == y.accepted && x.resting == y.resting);
            CHECK(x.executions.size() == y.executions.size());
            for (std::size_t j = 0; j < x.executions.size(); ++j) {
                CHECK(x.executions[j].sequence == y.executions[j].sequence);
                CHECK(x.executions[j].maker_id == y.executions[j].maker_id);
                CHECK(x.executions[j].taker_id == y.executions[j].taker_id);
                CHECK(x.executions[j].price_ticks == y.executions[j].price_ticks);
                CHECK(x.executions[j].quantity == y.executions[j].quantity);
            }
            if (type == OrderType::Limit && tif == TimeInForce::GTC) prior.push_back(id);
        } else if (op < 9) {
            const OrderId id = prior[rng() % prior.size()];
            const auto x = a.cancel(id), y = b.cancel(id);
            CHECK(x.cancelled == y.cancelled && x.removed_quantity == y.removed_quantity);
        } else {
            const OrderId id = prior[rng() % prior.size()];
            const auto price = 90 + static_cast<PriceTicks>(rng() % 21);
            const auto qty = 1 + rng() % 20;
            auto x = a.replace(id, price, qty), y = b.replace(id, price, qty);
            CHECK(x.accepted == y.accepted && x.executions.size() == y.executions.size());
        }
        invariants(a);
        invariants(b);
    }
    auto x = a.snapshot(100), y = b.snapshot(100);
    CHECK(x.bids.size() == y.bids.size() && x.asks.size() == y.asks.size());
    for (std::size_t i = 0; i < x.bids.size(); ++i) {
        CHECK(x.bids[i].price_ticks == y.bids[i].price_ticks);
        CHECK(x.bids[i].aggregate_quantity == y.bids[i].aggregate_quantity);
    }
    for (std::size_t i = 0; i < x.asks.size(); ++i) {
        CHECK(x.asks[i].price_ticks == y.asks[i].price_ticks);
        CHECK(x.asks[i].aggregate_quantity == y.asks[i].aggregate_quantity);
    }
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> cases = {
        {"SHA-256 reference vectors", test_sha256_reference_vectors},
        {"FIFO within same price", test_fifo_same_price},
        {"best price before time", test_best_price_before_time},
        {"bid priority and passive trade price", test_bid_priority_and_passive_price},
        {"non-crossing orders and snapshot", test_non_crossing_and_snapshot},
        {"IOC remainder expires", test_ioc_partial_expires},
        {"market orders never rest", test_market_never_rests},
        {"cancel removes exact order", test_cancel_removes_exact_order},
        {"duplicate identifiers", test_duplicate_ids_including_cancelled_and_filled},
        {"replace decrease preserves priority", test_replace_decrease_preserves_fifo},
        {"replace increase loses priority", test_replace_increase_loses_fifo},
        {"replace price change crosses", test_replace_price_change_reenters_matching},
        {"invalid replacement leaves original untouched", test_replace_bad_request_atomic},
        {"order validation", test_rejection_cases},
        {"fills and accounting counters", test_full_fill_and_stats},
        {"price level depletion", test_multiple_price_level_depletion},
        {"replace unknown order", test_replacing_unknown_is_not_new},
        {"15k randomized deterministic operations", test_deterministic_randomized_sequences_and_invariants},
    };
    int failures = 0;
    for (const auto& [name, test] : cases) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& ex) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
        }
    }
    std::cout << "RESULT " << (cases.size() - failures) << '/' << cases.size() << " passed\n";
    return failures ? 1 : 0;
}
