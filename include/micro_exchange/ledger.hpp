#pragma once

#include "micro_exchange/matching_engine.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace micro_exchange {

// Single-instrument, zero-fee, synthetic *trade cash-flow* accounting.
// No collateral, deposits, margin, borrow/financing, or monetary conversion.
struct AccountState {
    std::int64_t position{};   // signed: positive long, negative short
    long double cash_flow_tick_units{};
    long double average_entry_ticks{};
    long double realized_pnl_tick_units{};
    std::uint64_t fill_sides{};
};

class ExecutionLedger {
public:
    // Each price/quantity match is posted once for buyer and once for seller.
    // The owner IDs must correspond to the clients as recorded by the WAL.
    bool apply_trade(std::uint64_t sequence, std::uint64_t buyer_owner,
                     std::uint64_t seller_owner, PriceTicks price, Quantity quantity,
                     std::string* error = nullptr);

    AccountState account(std::uint64_t owner_id) const;
    bool check(std::string* error = nullptr) const;
    std::uint64_t trade_count() const { return trade_count_; }
    std::size_t account_count() const { return accounts_.size(); }

private:
    static void apply_side(AccountState& account, bool buy, PriceTicks price, Quantity qty);
    std::unordered_map<std::uint64_t, AccountState> accounts_;
    std::uint64_t trade_count_{};
};

} // namespace micro_exchange
