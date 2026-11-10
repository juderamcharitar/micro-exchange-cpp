#include "micro_exchange/ledger.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace micro_exchange {

void ExecutionLedger::apply_side(AccountState& a, bool buy, PriceTicks price, Quantity qty) {
    const std::int64_t signed_qty = buy ? static_cast<std::int64_t>(qty)
                                        : -static_cast<std::int64_t>(qty);
    const std::int64_t old = a.position;
    const std::int64_t next = old + signed_qty;
    const long double px = static_cast<long double>(price);
    a.cash_flow_tick_units += buy ? -px * qty : px * qty;

    if (old == 0 || (old > 0) == (signed_qty > 0)) {
        const auto held = static_cast<long double>(std::abs(old));
        a.average_entry_ticks = (held * a.average_entry_ticks +
                                  static_cast<long double>(qty) * px) /
                                 (held + static_cast<long double>(qty));
    } else {
        const auto closed = static_cast<std::uint64_t>(std::min(std::abs(old), std::abs(signed_qty)));
        const long double direction = old > 0 ? 1.0L : -1.0L;
        a.realized_pnl_tick_units += static_cast<long double>(closed) *
                                      (px - a.average_entry_ticks) * direction;
        if (next == 0) a.average_entry_ticks = 0;
        else if ((next > 0) != (old > 0)) a.average_entry_ticks = px;
    }
    a.position = next;
    ++a.fill_sides;
}

bool ExecutionLedger::apply_trade(std::uint64_t sequence, std::uint64_t buyer_owner,
                                  std::uint64_t seller_owner, PriceTicks price, Quantity qty,
                                  std::string* error) {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    if (sequence != trade_count_ + 1 || buyer_owner == 0 || seller_owner == 0 ||
        price <= 0 || qty == 0 || qty > static_cast<Quantity>(std::numeric_limits<std::int64_t>::max()))
        return fail("invalid trade input or sequence");
    // Prevent overflowing position; ensure both legs are posted atomically.
    const auto buyer = account(buyer_owner).position;
    const auto seller = account(seller_owner).position;
    const auto q = static_cast<std::int64_t>(qty);
    if (buyer_owner == seller_owner) {
        // Self-cross: signed cash and position net to zero, but two fill sides.
        // Self-trade does not alter the net account position, cash, or P&L.
        // It still creates two execution sides for reconciliation.
        accounts_[buyer_owner].fill_sides += 2;
    } else {
        if (buyer > std::numeric_limits<std::int64_t>::max() - q ||
            seller < std::numeric_limits<std::int64_t>::min() + q)
            return fail("account position overflow");
        apply_side(accounts_[buyer_owner], true, price, qty);
        apply_side(accounts_[seller_owner], false, price, qty);
    }
    ++trade_count_;
    return true;
}

AccountState ExecutionLedger::account(std::uint64_t owner_id) const {
    auto it = accounts_.find(owner_id);
    return it == accounts_.end() ? AccountState{} : it->second;
}

bool ExecutionLedger::check(std::string* error) const {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    long double cash = 0;
    long double positions = 0;
    std::uint64_t sides = 0;
    for (const auto& [id, account] : accounts_) {
        if (id == 0 || !std::isfinite(account.cash_flow_tick_units) ||
            !std::isfinite(account.average_entry_ticks) ||
            !std::isfinite(account.realized_pnl_tick_units)) return fail("nonfinite account state");
        if (account.position == 0 && account.average_entry_ticks != 0)
            return fail("flat account has open cost basis");
        if (account.position != 0 && account.average_entry_ticks <= 0)
            return fail("open account has invalid cost basis");
        cash += account.cash_flow_tick_units;
        positions += static_cast<long double>(account.position);
        sides += account.fill_sides;
    }
    if (positions != 0 || std::fabs(cash) > 0.001L)
        return fail("double-entry positions/cash do not reconcile");
    if (sides != trade_count_ * 2) return fail("fill-side count differs from trade count");
    return true;
}

} // namespace micro_exchange
