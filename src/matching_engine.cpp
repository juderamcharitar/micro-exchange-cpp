#include "micro_exchange/matching_engine.hpp"

#include <limits>
#include <utility>

namespace micro_exchange {

MatchingEngine::MatchingEngine(Quantity max_order_quantity, PriceTicks max_price_ticks)
    : max_order_quantity_(max_order_quantity), max_price_ticks_(max_price_ticks) {}

bool MatchingEngine::validate(const NewOrder& order, std::string& reason) const {
    if (order.id == 0) {
        reason = "order id must be positive";
    } else if (order.quantity == 0 || order.quantity > max_order_quantity_) {
        reason = "quantity outside permitted range";
    } else if (order.type == OrderType::Limit &&
               (order.limit_price_ticks <= 0 || order.limit_price_ticks > max_price_ticks_)) {
        reason = "limit price outside permitted tick range";
    } else if (order.type == OrderType::Market && order.limit_price_ticks != 0) {
        reason = "market order must have price 0";
    } else if (order.type == OrderType::Market && order.tif != TimeInForce::IOC) {
        reason = "market orders must use IOC";
    } else {
        return true;
    }
    return false;
}

SubmitResult MatchingEngine::submit(const NewOrder& order) {
    std::string reason;
    if (!validate(order, reason)) {
        ++stats_.rejected;
        return {false, std::move(reason), {}, 0, false};
    }
    if (used_ids_.find(order.id) != used_ids_.end()) {
        ++stats_.rejected;
        return {false, "order id already used (including filled/cancelled ids)", {}, 0, false};
    }
    used_ids_.insert(order.id);
    ++stats_.accepted_new;
    return submit_validated(order);
}

SubmitResult MatchingEngine::submit_validated(const NewOrder& order) {
    Quantity remaining = order.quantity;
    SubmitResult result;
    result.accepted = true;
    result.reason = "accepted";

    // Generic matching loop: opposing maps use their own best-price ordering.
    // Trades execute at the resting maker's price, not at taker's limit.
    const auto consume = [&](auto& opposing) {
        while (remaining > 0 && !opposing.empty()) {
            auto level_it = opposing.begin();
            const PriceTicks maker_price = level_it->first;
            const bool crosses = order.type == OrderType::Market ||
                (order.side == Side::Buy ? maker_price <= order.limit_price_ticks
                                         : maker_price >= order.limit_price_ticks);
            if (!crosses) break;

            auto& level = level_it->second;
            auto& maker = level.front();
            const Quantity filled = remaining < maker.leaves ? remaining : maker.leaves;
            result.executions.push_back({next_execution_sequence_++, maker.id,
                                         order.id, maker_price, filled});
            ++stats_.trades;
            stats_.executed_quantity += filled;
            remaining -= filled;
            maker.leaves -= filled;
            if (maker.leaves == 0) {
                live_.erase(maker.id);
                level.pop_front();
            }
            if (level.empty()) opposing.erase(level_it);
        }
    };

    if (order.side == Side::Buy) consume(asks_);
    else consume(bids_);

    if (remaining > 0 && order.type == OrderType::Limit && order.tif == TimeInForce::GTC) {
        if (order.side == Side::Buy) {
            auto& level = bids_[order.limit_price_ticks];
            auto pos = level.insert(level.end(), {order.id, remaining});
            live_.emplace(order.id, LiveOrder{order.side, order.limit_price_ticks, pos});
        } else {
            auto& level = asks_[order.limit_price_ticks];
            auto pos = level.insert(level.end(), {order.id, remaining});
            live_.emplace(order.id, LiveOrder{order.side, order.limit_price_ticks, pos});
        }
        result.resting = true;
    }
    result.remaining_quantity = remaining;
    return result;
}

void MatchingEngine::erase_live(OrderId id, const LiveOrder& location) {
    if (location.side == Side::Buy) {
        auto level = bids_.find(location.price_ticks);
        level->second.erase(location.position);
        if (level->second.empty()) bids_.erase(level);
    } else {
        auto level = asks_.find(location.price_ticks);
        level->second.erase(location.position);
        if (level->second.empty()) asks_.erase(level);
    }
    live_.erase(id);
}

CancelResult MatchingEngine::cancel(OrderId id) {
    auto it = live_.find(id);
    if (it == live_.end()) {
        ++stats_.rejected;
        return {false, "no resting order with that id", 0};
    }
    const LiveOrder location = it->second;
    const Quantity removed = location.position->leaves;
    erase_live(id, location);
    ++stats_.cancelled;
    return {true, "cancelled", removed};
}

SubmitResult MatchingEngine::replace(OrderId id, PriceTicks new_price_ticks,
                                      Quantity new_remaining_quantity) {
    auto it = live_.find(id);
    if (it == live_.end()) {
        ++stats_.rejected;
        return {false, "no resting order with that id", {}, 0, false};
    }
    const LiveOrder previous = it->second;
    const NewOrder replacement{id, previous.side, OrderType::Limit, new_price_ticks,
                               new_remaining_quantity, TimeInForce::GTC};
    std::string reason;
    if (!validate(replacement, reason)) {
        ++stats_.rejected;
        return {false, reason, {}, 0, false};
    }
    ++stats_.replaced;
    if (new_price_ticks == previous.price_ticks &&
        new_remaining_quantity <= previous.position->leaves) {
        previous.position->leaves = new_remaining_quantity;
        return {true, "replaced; priority retained", {}, new_remaining_quantity, true};
    }
    erase_live(id, previous);
    auto result = submit_validated(replacement);
    result.reason = "replaced; priority reset";
    return result;
}

bool MatchingEngine::would_self_trade(
    const NewOrder& incoming, std::uint64_t taker_owner,
    const std::function<std::uint64_t(OrderId)>& owner_for_id) const {
    if (taker_owner == 0 || incoming.quantity == 0) return false;
    Quantity remaining = incoming.quantity;
    const auto inspect = [&](const auto& opposing) -> bool {
        for (const auto& [price, level] : opposing) {
            const bool crossed = incoming.type == OrderType::Market ||
                (incoming.side == Side::Buy ? price <= incoming.limit_price_ticks
                                            : price >= incoming.limit_price_ticks);
            if (!crossed) break;
            for (const auto& maker : level) {
                if (owner_for_id(maker.id) == taker_owner) return true;
                remaining -= std::min(remaining, maker.leaves);
                if (remaining == 0) return false;
            }
        }
        return false;
    };
    return incoming.side == Side::Buy ? inspect(asks_) : inspect(bids_);
}

BookSnapshot MatchingEngine::snapshot(std::size_t depth) const {
    BookSnapshot out;
    const auto append = [depth](const auto& book, auto& target) {
        for (const auto& [price, orders] : book) {
            if (target.size() >= depth) break;
            Quantity qty = 0;
            for (const auto& order : orders) qty += order.leaves;
            target.push_back({price, qty, orders.size()});
        }
    };
    append(bids_, out.bids);
    append(asks_, out.asks);
    return out;
}

std::optional<RestingOrderView> MatchingEngine::find_resting(OrderId id) const {
    auto it = live_.find(id);
    if (it == live_.end()) return std::nullopt;
    return RestingOrderView{it->second.side, it->second.price_ticks,
                            it->second.position->leaves};
}

EngineStats MatchingEngine::stats() const {
    auto s = stats_;
    s.resting_orders = live_.size();
    return s;
}

bool MatchingEngine::check_invariants(std::string* error) const {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!bids_.empty() && !asks_.empty() && bids_.begin()->first >= asks_.begin()->first)
        return fail("crossed/locked book after matching");
    std::size_t count = 0;
    const auto check = [&](const auto& book, Side side) {
        for (const auto& [price, orders] : book) {
            if (price <= 0 || orders.empty()) return false;
            for (auto it = orders.begin(); it != orders.end(); ++it) {
                if (it->leaves == 0 || used_ids_.find(it->id) == used_ids_.end()) return false;
                auto ref = live_.find(it->id);
                if (ref == live_.end() || ref->second.side != side ||
                    ref->second.price_ticks != price || ref->second.position != it) return false;
                ++count;
            }
        }
        return true;
    };
    if (!check(bids_, Side::Buy) || !check(asks_, Side::Sell))
        return fail("corrupt level, resting quantity, or live-order index");
    if (count != live_.size()) return fail("live order index count mismatch");
    return true;
}

}  // namespace micro_exchange
