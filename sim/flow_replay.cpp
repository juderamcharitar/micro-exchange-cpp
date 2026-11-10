#include "micro_exchange/matching_engine.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using micro_exchange::BookSnapshot;
using micro_exchange::MatchingEngine;
using micro_exchange::NewOrder;
using micro_exchange::OrderId;
using micro_exchange::OrderType;
using micro_exchange::PriceTicks;
using micro_exchange::Quantity;
using micro_exchange::Side;
using micro_exchange::TimeInForce;

namespace {

// This is a transparent SYNTHETIC order-event generator and deterministic
// in-memory trace replayer. It is not an exchange message-data reconstruction.
// SplitMix64 provides a stable sequence; floating point math is not promised
// to produce byte-for-byte identical traces across different C++ runtimes.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        std::uint64_t x = (state_ += 0x9e3779b97f4a7c15ULL);
        x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31U);
    }
    std::uint64_t bound(std::uint64_t n) {
        if (!n) throw std::invalid_argument("random bound is zero");
        const std::uint64_t threshold = (std::uint64_t{0} - n) % n;
        for (;;) {
            const auto x = next();
            if (x >= threshold) return x % n;
        }
    }
    bool percent(unsigned pct) { return bound(100) < pct; }
    std::uint64_t gap_microseconds(unsigned events_per_second) {
        // Exponential interarrival model; timestamps quantized to whole us.
        const long double u = (static_cast<long double>(next()) + 0.5L) /
                              18446744073709551616.0L;
        const long double dt = -std::log(u) * 1000000.0L / events_per_second;
        return std::max<std::uint64_t>(1, static_cast<std::uint64_t>(dt + 0.5L));
    }
private:
    std::uint64_t state_;
};

struct Regime {
    std::string name;
    unsigned arrivals_per_second;
    unsigned passive_pct;
    unsigned cancel_pct;
    unsigned replace_pct;
    unsigned spread_ticks;
    unsigned price_jitter_ticks;
    unsigned max_qty;
};

// Scenario knobs are intentionally bounded and explicit. They do not represent
// statistically estimated order-arrival or cancellation probabilities.
struct Tuning {
    unsigned arrival_scale_percent{100};
    unsigned size_scale_percent{100};
    unsigned aggression_shift_points{0}; // transfers passive NEW probability to aggressive NEW
    unsigned cancel_shift_points{0};     // transfers passive NEW probability to cancellation
    bool defaults() const {
        return arrival_scale_percent == 100 && size_scale_percent == 100 &&
            aggression_shift_points == 0 && cancel_shift_points == 0;
    }
};

void validate_tuning(const Tuning& t) {
    if (t.arrival_scale_percent < 50 || t.arrival_scale_percent > 200 ||
        t.size_scale_percent < 50 || t.size_scale_percent > 200 ||
        t.aggression_shift_points > 20 || t.cancel_shift_points > 20 ||
        t.aggression_shift_points + t.cancel_shift_points >= 43)
        throw std::runtime_error("invalid scenario tuning (scales 50..200, shifts 0..20; preserve passive orders)");
}

Regime tuned_regime(const Regime& base, const Tuning& t) {
    Regime r = base;
    const unsigned shift = t.aggression_shift_points + t.cancel_shift_points;
    if (shift >= r.passive_pct) throw std::runtime_error("scenario removes all passive orders");
    r.passive_pct -= shift;
    r.cancel_pct += t.cancel_shift_points;
    r.arrivals_per_second = (r.arrivals_per_second * t.arrival_scale_percent + 50) / 100;
    r.max_qty = std::max(1U, (r.max_qty * t.size_scale_percent + 50) / 100);
    return r;
}

const Regime CALM{"calm", 2200, 61, 22, 7, 4, 3, 9};
const Regime VOLATILE{"volatile", 5400, 54, 20, 8, 9, 11, 22};
const Regime STRESS{"stress", 12000, 43, 17, 7, 17, 30, 45};

const Regime& choose_regime(const std::string& profile, std::size_t index, std::size_t count) {
    if (profile == "calm") return CALM;
    if (profile == "volatile") return VOLATILE;
    if (profile == "stress") return STRESS;
    if (profile == "mixed") {
        // Exact three contiguous phases, even when count is not divisible by 3.
        if (index < count / 3) return CALM;
        if (index < (count * 2) / 3) return VOLATILE;
        return STRESS;
    }
    throw std::invalid_argument("unknown profile: " + profile);
}

bool valid_regime(const std::string& name) {
    return name == "seed" || name == "calm" || name == "volatile" || name == "stress" || name == "external";
}

struct Event {
    std::uint64_t ts_us{};
    std::string regime;
    std::string op;
    OrderId order_id{};
    std::string side;
    std::string type;
    PriceTicks price_ticks{};
    Quantity qty{};
    std::string tif;
};

std::string event_csv(const Event& e) {
    std::ostringstream out;
    out << e.ts_us << ',' << e.regime << ',' << e.op << ',' << e.order_id << ','
        << e.side << ',' << e.type << ',';
    if (e.op != "CANCEL") out << e.price_ticks;
    out << ',';
    if (e.op != "CANCEL") out << e.qty;
    out << ',' << e.tif;
    return out.str();
}

std::vector<std::string> csv_fields(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const auto pos = line.find(',', start);
        if (pos == std::string::npos) { parts.push_back(line.substr(start)); break; }
        parts.push_back(line.substr(start, pos-start));
        start = pos + 1;
    }
    return parts;
}

template <typename T>
T integer(std::string_view value, const char* name) {
    T out{};
    if (value.empty() || value.front() == '+' || value.front() == ' ' ||
        value.back() == ' ') throw std::runtime_error(std::string("invalid ") + name);
    const auto result = std::from_chars(value.data(), value.data()+value.size(), out);
    if (result.ec != std::errc{} || result.ptr != value.data()+value.size())
        throw std::runtime_error(std::string("invalid ") + name);
    return out;
}

Event parse_event(const std::string& line) {
    const auto f = csv_fields(line);
    if (f.size() != 9) throw std::runtime_error("expected 9 comma-separated event fields");
    Event e;
    e.ts_us = integer<std::uint64_t>(f[0], "timestamp_us");
    e.regime = f[1];
    e.op = f[2];
    e.order_id = integer<OrderId>(f[3], "order_id");
    e.side = f[4];
    e.type = f[5];
    e.price_ticks = f[6].empty() ? 0 : integer<PriceTicks>(f[6], "price_ticks");
    e.qty = f[7].empty() ? 0 : integer<Quantity>(f[7], "quantity");
    e.tif = f[8];
    if (!valid_regime(e.regime) || e.order_id == 0)
        throw std::runtime_error("invalid regime or zero order_id");
    if (e.op == "NEW") {
        if ((e.side != "BUY" && e.side != "SELL") ||
            (e.type != "LIMIT" && e.type != "MARKET") ||
            (e.tif != "GTC" && e.tif != "IOC") || e.qty == 0 ||
            (e.type == "MARKET" && (e.price_ticks != 0 || e.tif != "IOC")) ||
            (e.type == "LIMIT" && e.price_ticks <= 0))
            throw std::runtime_error("invalid NEW fields");
    } else if (e.op == "CANCEL") {
        if (!e.side.empty() || !e.type.empty() || !f[6].empty() ||
            !f[7].empty() || !e.tif.empty()) throw std::runtime_error("invalid CANCEL fields");
    } else if (e.op == "REPLACE") {
        if (!e.side.empty() || !e.type.empty() || !e.tif.empty() ||
            e.price_ticks <= 0 || e.qty == 0) throw std::runtime_error("invalid REPLACE fields");
    } else {
        throw std::runtime_error("invalid operation: " + e.op);
    }
    return e;
}

class LiveOrders {
public:
    void add(OrderId id) {
        if (indexes_.count(id)) throw std::logic_error("live order ID added twice");
        indexes_[id] = ids_.size();
        ids_.push_back(id);
    }
    void remove(OrderId id) {
        const auto it = indexes_.find(id);
        if (it == indexes_.end()) return;
        const auto index = it->second;
        const OrderId last = ids_.back();
        ids_[index] = last;
        indexes_[last] = index;
        ids_.pop_back();
        indexes_.erase(it);
    }
    bool empty() const { return ids_.empty(); }
    OrderId sample(Random& rng) const { return ids_.at(static_cast<std::size_t>(rng.bound(ids_.size()))); }
private:
    std::vector<OrderId> ids_;
    std::unordered_map<OrderId, std::size_t> indexes_;
};

struct Metrics {
    std::uint64_t events{}, new_orders{}, cancellations{}, replacements{};
    std::uint64_t market_ioc{}, limit_ioc{}, limit_gtc{};
    std::uint64_t first_ts{}, last_ts{};
    std::uint64_t trades{}, quantity{}, quoted{}, spread_total{};
    std::uint64_t min_spread{std::numeric_limits<std::uint64_t>::max()}, max_spread{};
    std::uint64_t bid_depth_total{}, ask_depth_total{};
    long double imbalance_total{};
    long double best_imbalance_total{};
    std::uint64_t best_imbalance_samples{};
    long double mid_abs_change_bps_total{};
    std::uint64_t mid_change_samples{};
    long double previous_midpoint{};
    bool has_previous_midpoint{};
    void quote(const BookSnapshot& b) {
        std::uint64_t bqty=0, aqty=0;
        for (const auto& level : b.bids) bqty += level.aggregate_quantity;
        for (const auto& level : b.asks) aqty += level.aggregate_quantity;
        bid_depth_total += bqty;
        ask_depth_total += aqty;
        if (bqty+aqty) imbalance_total += (static_cast<long double>(bqty)-static_cast<long double>(aqty)) /
            static_cast<long double>(bqty+aqty);
        if (!b.bids.empty() && !b.asks.empty()) {
            const auto spread = b.asks.front().price_ticks - b.bids.front().price_ticks;
            if (spread <= 0) throw std::runtime_error("crossed or locked book");
            const auto s = static_cast<std::uint64_t>(spread);
            ++quoted;
            spread_total += s;
            min_spread = std::min(min_spread, s);
            max_spread = std::max(max_spread, s);
            const auto best_bid_qty = b.bids.front().aggregate_quantity;
            const auto best_ask_qty = b.asks.front().aggregate_quantity;
            const auto best_total = best_bid_qty + best_ask_qty;
            if (best_total) {
                best_imbalance_total += (static_cast<long double>(best_bid_qty) -
                    static_cast<long double>(best_ask_qty)) / best_total;
                ++best_imbalance_samples;
            }
            const long double mid = (static_cast<long double>(b.bids.front().price_ticks) +
                static_cast<long double>(b.asks.front().price_ticks)) / 2.0L;
            if (has_previous_midpoint && previous_midpoint > 0) {
                mid_abs_change_bps_total += std::abs(mid - previous_midpoint) /
                    previous_midpoint * 10000.0L;
                ++mid_change_samples;
            }
            previous_midpoint = mid;
            has_previous_midpoint = true;
        } else {
            has_previous_midpoint = false; // no reference midpoint during one-sided books
        }
    }
};

std::uint64_t fnv_bytes(std::uint64_t hash, std::string_view data) {
    for (const unsigned char c : data) { hash ^= c; hash *= 1099511628211ULL; }
    return hash;
}

struct SourceInfo {
    std::uint64_t seed{};
    std::string profile;
    std::uint64_t synthetic_events{};
    std::uint64_t seed_events{};
    Tuning tuning{};
    bool extended_metadata{false};
};

class Runner {
public:
    explicit Runner(SourceInfo info, const std::string& trades_path)
        : info_(std::move(info)) {
        if (!trades_path.empty()) {
            const auto parent = std::filesystem::path(trades_path).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            tape_.open(trades_path, std::ios::trunc);
            if (!tape_) throw std::runtime_error("cannot write trades tape: " + trades_path);
            tape_ << "event_index,ts_us,regime,sequence,maker_id,taker_id,price_ticks,quantity\n";
        }
    }

    const MatchingEngine& engine() const { return book_; }
    const LiveOrders& live() const { return live_; }

    void apply(const Event& e) {
        if (!valid_regime(e.regime)) throw std::runtime_error("invalid regime");
        if (stats_.events && e.ts_us < last_ts_) throw std::runtime_error("non-monotonic timestamp");
        last_ts_ = e.ts_us;
        Metrics& m = by_regime_[e.regime];
        if (m.events == 0) m.first_ts = e.ts_us;
        m.last_ts = e.ts_us;
        ++m.events;
        ++stats_.events;
        digest_ = fnv_bytes(digest_, event_csv(e));
        digest_ = fnv_bytes(digest_, "\n");
        std::vector<micro_exchange::Execution> executions;
        if (e.op == "NEW") {
            ++m.new_orders;
            ++stats_.new_orders;
            if (e.type == "MARKET") { ++m.market_ioc; ++stats_.market_ioc; }
            else if (e.tif == "IOC") { ++m.limit_ioc; ++stats_.limit_ioc; }
            else { ++m.limit_gtc; ++stats_.limit_gtc; }
            const NewOrder order{e.order_id, e.side == "BUY" ? Side::Buy : Side::Sell,
                e.type == "LIMIT" ? OrderType::Limit : OrderType::Market,
                e.price_ticks, e.qty, e.tif == "GTC" ? TimeInForce::GTC : TimeInForce::IOC};
            auto result = book_.submit(order);
            if (!result.accepted) throw std::runtime_error("NEW rejected: " + result.reason);
            executions = std::move(result.executions);
            if (result.resting) live_.add(e.order_id);
        } else if (e.op == "CANCEL") {
            ++m.cancellations;
            ++stats_.cancellations;
            const auto result = book_.cancel(e.order_id);
            if (!result.cancelled) throw std::runtime_error("CANCEL rejected: " + result.reason);
            live_.remove(e.order_id);
        } else if (e.op == "REPLACE") {
            ++m.replacements;
            ++stats_.replacements;
            const auto result = book_.replace(e.order_id, e.price_ticks, e.qty);
            if (!result.accepted) throw std::runtime_error("REPLACE rejected: " + result.reason);
            executions = result.executions;
            if (!result.resting) live_.remove(e.order_id);
        } else throw std::runtime_error("invalid operation: " + e.op);

        for (const auto& trade : executions) {
            ++stats_.trades;
            ++m.trades;
            stats_.quantity += trade.quantity;
            m.quantity += trade.quantity;
            std::ostringstream row;
            row << stats_.events << ',' << e.ts_us << ',' << e.regime << ','
                << trade.sequence << ',' << trade.maker_id << ',' << trade.taker_id
                << ',' << trade.price_ticks << ',' << trade.quantity << '\n';
            const auto str = row.str();
            digest_ = fnv_bytes(digest_, str);
            if (tape_) tape_ << str;
            if (!book_.find_resting(trade.maker_id)) live_.remove(trade.maker_id);
        }
        const auto snap = book_.snapshot(5);
        stats_.quote(snap);
        m.quote(snap);
        if (stats_.events % 257 == 0) {
            std::string problem;
            if (!book_.check_invariants(&problem)) throw std::runtime_error("engine invariants: " + problem);
        }
    }

    void finish() const {
        std::string problem;
        if (!book_.check_invariants(&problem)) throw std::runtime_error("engine invariants: " + problem);
        if (stats_.events != info_.seed_events + info_.synthetic_events)
            throw std::runtime_error("event count does not match trace metadata");
        const auto seed_it = by_regime_.find("seed");
        const auto seen_seed = seed_it == by_regime_.end() ? 0ULL : seed_it->second.events;
        if (seen_seed != info_.seed_events)
            throw std::runtime_error("seed event count mismatch");
        if (book_.stats().trades != stats_.trades)
            throw std::runtime_error("trade count discrepancy");
    }

    std::string summary() const {
        std::ostringstream out;
        out << std::fixed << std::setprecision(4);
        const auto snap = book_.snapshot(1);
        out << "{\n  \"schema\": \"micro_exchange_flow_v1\",\n"
            << "  \"data_kind\": \""
            << (info_.profile == "external" ? "unverified_external_order_event_trace" : "synthetic_order_event_trace")
            << "\",\n"
            << "  \"profile\": \"" << info_.profile << "\",\n"
            << "  \"seed\": " << info_.seed << ",\n"
            << "  \"assumed_regime_arrivals_per_second\": ";
        if (info_.profile == "external") out << "null";
        else out << "{\"calm\": " << tuned_regime(CALM, info_.tuning).arrivals_per_second
                 << ", \"volatile\": " << tuned_regime(VOLATILE, info_.tuning).arrivals_per_second
                 << ", \"stress\": " << tuned_regime(STRESS, info_.tuning).arrivals_per_second << '}';
        out << ",\n"
            << "  \"seed_events\": " << info_.seed_events << ",\n"
            << "  \"generated_events\": " << info_.synthetic_events << ",\n"
            << "  \"total_events\": " << stats_.events << ",\n"
            << "  \"event_counts\": {\"new\": " << stats_.new_orders
            << ", \"market_ioc\": " << stats_.market_ioc
            << ", \"limit_ioc\": " << stats_.limit_ioc
            << ", \"limit_gtc\": " << stats_.limit_gtc
            << ", \"cancel\": " << stats_.cancellations << ", \"replace\": "
            << stats_.replacements << "},\n"
            << "  \"executions\": {\"trades\": " << stats_.trades
            << ", \"quantity\": " << stats_.quantity << "},\n"
            << "  \"quote_metrics\": ";
        metrics_json(out, stats_, info_.extended_metadata);
        if (info_.extended_metadata) {
            const auto& t = info_.tuning;
            out << ",\n  \"synthetic_tuning\": {\"arrival_scale_percent\": "
                << t.arrival_scale_percent << ", \"size_scale_percent\": " << t.size_scale_percent
                << ", \"aggression_shift_points\": " << t.aggression_shift_points
                << ", \"cancel_shift_points\": " << t.cancel_shift_points << '}';
        }
        out << ",\n  \"regimes\": {\n";
        bool first = true;
        for (const auto& [name, s] : by_regime_) {
            if (!first) out << ",\n";
            first = false;
            out << "    \"" << name << "\": {\"events\": " << s.events
                << ", \"new\": " << s.new_orders << ", \"cancel\": " << s.cancellations
                << ", \"replace\": " << s.replacements
                << ", \"aggressive_new\": " << (s.market_ioc+s.limit_ioc)
                << ", \"span_us\": " << (s.last_ts-s.first_ts)
                << ", \"observed_arrivals_per_second\": "
                << (s.last_ts > s.first_ts ?
                    static_cast<long double>(s.events-1)*1000000.0L/(s.last_ts-s.first_ts) : 0.0L)
                << ", \"trades\": " << s.trades
                << ", \"quantity\": " << s.quantity << ", \"quotes\": ";
            metrics_json(out, s, info_.extended_metadata);
            out << '}';
        }
        out << "\n  },\n  \"final_book\": {\"best_bid_ticks\": ";
        if (snap.bids.empty()) out << "null";
        else out << snap.bids.front().price_ticks;
        out << ", \"best_ask_ticks\": ";
        if (snap.asks.empty()) out << "null";
        else out << snap.asks.front().price_ticks;
        out << ", \"resting_orders\": " << book_.stats().resting_orders << "},\n";
        std::ostringstream hex;
        hex << std::hex << std::setfill('0') << std::setw(16) << digest_;
        out << "  \"execution_fingerprint_fnv1a64\": \"0x" << hex.str() << "\"\n}\n";
        return out.str();
    }

private:
    static void metrics_json(std::ostringstream& out, const Metrics& s, bool extended) {
        out << "{\"two_sided_quote_samples\": " << s.quoted << ", \"mean_spread_ticks\": ";
        if (s.quoted) out << static_cast<long double>(s.spread_total)/s.quoted;
        else out << "null";
        out << ", \"min_spread_ticks\": ";
        if (s.quoted) out << s.min_spread;
        else out << "null";
        out << ", \"max_spread_ticks\": ";
        if (s.quoted) out << s.max_spread;
        else out << "null";
        out << ", \"mean_bid_depth5_qty\": "
            << (s.events ? static_cast<long double>(s.bid_depth_total)/s.events : 0.0L)
            << ", \"mean_ask_depth5_qty\": "
            << (s.events ? static_cast<long double>(s.ask_depth_total)/s.events : 0.0L)
            << ", \"mean_depth5_imbalance\": "
            << (s.events ? s.imbalance_total/s.events : 0.0L);
        if (extended) {
            out << ", \"mean_best_quote_imbalance\": ";
            if (s.best_imbalance_samples) out << s.best_imbalance_total/s.best_imbalance_samples;
            else out << "null";
            out << ", \"mean_absolute_midprice_change_bps_per_quote_observation\": ";
            if (s.mid_change_samples) out << s.mid_abs_change_bps_total/s.mid_change_samples;
            else out << "null";
        }
        out << '}';
    }

    SourceInfo info_;
    MatchingEngine book_;
    LiveOrders live_;
    Metrics stats_;
    std::map<std::string, Metrics> by_regime_;
    std::uint64_t last_ts_{};
    std::uint64_t digest_{14695981039346656037ULL};
    std::ofstream tape_;
};

void ensure_parent(const std::string& filename) {
    const auto parent = std::filesystem::path(filename).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
}

std::string metadata_line(const SourceInfo& info) {
    std::ostringstream out;
    out << "# " << (info.extended_metadata ? "MEXFLOW2" : "MEXFLOW1")
        << " seed=" << info.seed << " profile=" << info.profile
        << " synthetic_events=" << info.synthetic_events << " seed_events=" << info.seed_events;
    if (info.extended_metadata) {
        out << " arrival_scale_percent=" << info.tuning.arrival_scale_percent
            << " size_scale_percent=" << info.tuning.size_scale_percent
            << " aggression_shift_points=" << info.tuning.aggression_shift_points
            << " cancel_shift_points=" << info.tuning.cancel_shift_points;
    }
    return out.str();
}

SourceInfo parse_metadata(const std::string& line) {
    std::istringstream in(line);
    std::vector<std::string> tokens;
    for (std::string tok; in >> tok;) tokens.push_back(tok);
    if ((tokens.size() != 6 && tokens.size() != 10) || tokens[0] != "#" ||
        (tokens[1] != "MEXFLOW1" && tokens[1] != "MEXFLOW2"))
        throw std::runtime_error("missing or invalid MEXFLOW header");
    const bool extended = tokens[1] == "MEXFLOW2";
    if (extended != (tokens.size() == 10))
        throw std::runtime_error("MEXFLOW format and tuning fields disagree");
    auto extract = [](const std::string& token, std::string_view prefix) -> std::string {
        if (token.rfind(prefix, 0) != 0) throw std::runtime_error("invalid metadata token: " + token);
        return token.substr(prefix.size());
    };
    SourceInfo meta;
    meta.seed = integer<std::uint64_t>(extract(tokens[2], "seed="), "metadata seed");
    meta.profile = extract(tokens[3], "profile=");
    meta.synthetic_events = integer<std::uint64_t>(extract(tokens[4], "synthetic_events="), "metadata events");
    meta.seed_events = integer<std::uint64_t>(extract(tokens[5], "seed_events="), "metadata preload");
    meta.extended_metadata = extended;
    if (extended) {
        meta.tuning.arrival_scale_percent = integer<unsigned>(
            extract(tokens[6], "arrival_scale_percent="), "arrival scale");
        meta.tuning.size_scale_percent = integer<unsigned>(
            extract(tokens[7], "size_scale_percent="), "size scale");
        meta.tuning.aggression_shift_points = integer<unsigned>(
            extract(tokens[8], "aggression_shift_points="), "aggression shift");
        meta.tuning.cancel_shift_points = integer<unsigned>(
            extract(tokens[9], "cancel_shift_points="), "cancel shift");
        validate_tuning(meta.tuning);
    }
    if (meta.profile != "mixed" && meta.profile != "calm" && meta.profile != "volatile" &&
        meta.profile != "stress" && meta.profile != "external")
        throw std::runtime_error("unsupported trace profile");
    if (meta.synthetic_events > 1000000 || meta.seed_events > 1000000)
        throw std::runtime_error("trace event count exceeds bounded safety limit");
    return meta;
}

PriceTicks passive_price(const BookSnapshot& b, Side side, const Regime& regime, Random& rng) {
    constexpr PriceTicks anchor = 10000;
    const PriceTicks midpoint = (!b.bids.empty() && !b.asks.empty())
        ? (b.bids.front().price_ticks + b.asks.front().price_ticks) / 2 : anchor;
    const PriceTicks shift = static_cast<PriceTicks>(rng.bound(2*regime.price_jitter_ticks+1))
        - static_cast<PriceTicks>(regime.price_jitter_ticks);
    const PriceTicks halfspread = static_cast<PriceTicks>(regime.spread_ticks+1)/2;
    if (side == Side::Buy) {
        PriceTicks price = std::max<PriceTicks>(1, midpoint-halfspread+shift);
        if (!b.asks.empty()) price = std::min(price, b.asks.front().price_ticks-1);
        return std::max<PriceTicks>(1, price);
    }
    PriceTicks price = std::max<PriceTicks>(1, midpoint+halfspread+shift);
    if (!b.bids.empty()) price = std::max(price, b.bids.front().price_ticks+1);
    return price;
}

Event generate_action(std::uint64_t ts, const Regime& regime, Random& rng,
                      const Runner& runner, OrderId& next_id) {
    const auto choice = static_cast<unsigned>(rng.bound(100));
    std::string op;
    if (runner.live().empty() || choice < regime.passive_pct) op = "NEW_PASSIVE";
    else if (choice < regime.passive_pct+regime.cancel_pct) op = "CANCEL";
    else if (choice < regime.passive_pct+regime.cancel_pct+regime.replace_pct) op = "REPLACE";
    else op = "NEW_AGGRESSIVE";

    Event e;
    e.ts_us = ts;
    e.regime = regime.name;
    if (op == "CANCEL") {
        e.op = "CANCEL";
        e.order_id = runner.live().sample(rng);
        return e;
    }
    if (op == "REPLACE") {
        e.op = "REPLACE";
        e.order_id = runner.live().sample(rng);
        const auto resting = runner.engine().find_resting(e.order_id);
        if (!resting) throw std::logic_error("stale live order in replacement sampler");
        if (rng.percent(65)) {
            e.price_ticks = resting->price_ticks;
            e.qty = 1 + rng.bound(resting->leaves);
        } else {
            e.price_ticks = passive_price(runner.engine().snapshot(1), resting->side, regime, rng);
            e.qty = 1 + rng.bound(regime.max_qty);
        }
        return e;
    }
    e.op = "NEW";
    e.order_id = next_id++;
    const Side side = rng.percent(50) ? Side::Buy : Side::Sell;
    e.side = side == Side::Buy ? "BUY" : "SELL";
    e.qty = 1 + rng.bound(regime.max_qty);
    if (op == "NEW_PASSIVE") {
        e.type = "LIMIT";
        e.tif = "GTC";
        e.price_ticks = passive_price(runner.engine().snapshot(1), side, regime, rng);
    } else {
        e.tif = "IOC";
        if (rng.percent(70)) { e.type = "MARKET"; e.price_ticks = 0; }
        else {
            e.type = "LIMIT";
            const auto b = runner.engine().snapshot(1);
            if (side == Side::Buy)
                e.price_ticks = b.asks.empty() ? 10000 :
                    b.asks.front().price_ticks + static_cast<PriceTicks>(rng.bound(regime.spread_ticks+1));
            else
                e.price_ticks = b.bids.empty() ? 10000 :
                    std::max<PriceTicks>(1, b.bids.front().price_ticks -
                        static_cast<PriceTicks>(rng.bound(regime.spread_ticks+1)));
        }
    }
    return e;
}

std::string generate(const std::string& path, const std::string& trades, std::uint64_t seed,
                     std::size_t count, const std::string& profile, const Tuning& tuning) {
    if (count == 0 || count > 250000) throw std::runtime_error("events must be 1..250000");
    (void)choose_regime(profile, 0, count);
    validate_tuning(tuning);
    SourceInfo meta{seed, profile, count, 16, tuning, true};
    ensure_parent(path);
    std::ofstream trace(path, std::ios::trunc);
    if (!trace) throw std::runtime_error("cannot write event trace: " + path);
    trace << metadata_line(meta) << '\n'
          << "ts_us,regime,op,order_id,side,type,price_ticks,quantity,tif\n";
    Runner runner(meta, trades);
    Random rng(seed);
    OrderId next_id = 1;
    for (int level=1; level<=8; ++level) {
        for (Side side : {Side::Buy, Side::Sell}) {
            Event e;
            e.ts_us = 0;
            e.regime = "seed";
            e.op = "NEW";
            e.order_id = next_id++;
            e.side = side == Side::Buy ? "BUY" : "SELL";
            e.type = "LIMIT";
            e.tif = "GTC";
            e.price_ticks = 10000+(side == Side::Buy ? -level : level);
            e.qty = 12;
            trace << event_csv(e) << '\n';
            runner.apply(e);
        }
    }
    std::uint64_t ts = 0;
    for (std::size_t i=0; i<count; ++i) {
        const auto regime = tuned_regime(choose_regime(profile, i, count), tuning);
        const auto gap = rng.gap_microseconds(regime.arrivals_per_second);
        if (std::numeric_limits<std::uint64_t>::max()-ts < gap)
            throw std::runtime_error("timestamp overflow");
        ts += gap;
        const auto event = generate_action(ts, regime, rng, runner, next_id);
        trace << event_csv(event) << '\n';
        runner.apply(event);
    }
    trace.close();
    if (!trace) throw std::runtime_error("failed writing trace");
    runner.finish();
    return runner.summary();
}

std::string replay(const std::string& path, const std::string& trades) {
    std::ifstream stream(path);
    if (!stream) throw std::runtime_error("cannot open trace: " + path);
    std::string line;
    if (!std::getline(stream, line)) throw std::runtime_error("empty trace");
    const auto meta = parse_metadata(line);
    if (!std::getline(stream, line) || line !=
        "ts_us,regime,op,order_id,side,type,price_ticks,quantity,tif")
        throw std::runtime_error("unrecognized event CSV schema");
    Runner runner(meta, trades);
    std::uint64_t index = 0;
    while (std::getline(stream, line)) {
        if (line.empty()) throw std::runtime_error("empty CSV record at line " + std::to_string(index+3));
        if (++index > meta.synthetic_events+meta.seed_events)
            throw std::runtime_error("too many events in trace");
        try { runner.apply(parse_event(line)); }
        catch (const std::exception& e) {
            throw std::runtime_error("CSV line " + std::to_string(index+2) + ": " + e.what());
        }
    }
    if (!stream.eof()) throw std::runtime_error("trace read failed");
    runner.finish();
    return runner.summary();
}

void write_summary(const std::string& path, const std::string& json) {
    if (!path.empty()) {
        ensure_parent(path);
        std::ofstream stream(path, std::ios::trunc);
        if (!stream) throw std::runtime_error("cannot write summary: " + path);
        stream << json;
        if (!stream) throw std::runtime_error("summary write failed");
    }
}

void usage() {
    std::cerr << "Usage:\n"
       "  exchange_flow_sim --generate events.csv [--seed 42] [--events 12000]"
       " [--profile calm|volatile|stress|mixed] [--summary results.json] [--trades fills.csv]\n"
       "     [--arrival-scale-percent 50..200] [--size-scale-percent 50..200]\n"
       "     [--aggression-shift-points 0..20] [--cancel-shift-points 0..20]\n"
       "  exchange_flow_sim --replay events.csv [--summary results.json] [--trades fills.csv]\n"
       "Synthetic research only; this does not replay or reconstruct real L2 order IDs.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        std::string gen, rep, summary_path, tape_path, expected_path, profile="mixed";
        std::uint64_t seed=42;
        std::size_t events=12000;
        Tuning tuning;
        for (int i=1; i<argc; ++i) {
            const std::string s=argv[i];
            if (i+1 >= argc) throw std::runtime_error("missing value for " + s);
            const std::string value=argv[++i];
            if (s=="--generate") gen=value;
            else if (s=="--replay") rep=value;
            else if (s=="--summary") summary_path=value;
            else if (s=="--trades") tape_path=value;
            else if (s=="--expect-summary") expected_path=value;
            else if (s=="--profile") profile=value;
            else if (s=="--seed") seed=integer<std::uint64_t>(value,"seed");
            else if (s=="--events") events=integer<std::size_t>(value,"events");
            else if (s=="--arrival-scale-percent") tuning.arrival_scale_percent=integer<unsigned>(value,"arrival-scale-percent");
            else if (s=="--size-scale-percent") tuning.size_scale_percent=integer<unsigned>(value,"size-scale-percent");
            else if (s=="--aggression-shift-points") tuning.aggression_shift_points=integer<unsigned>(value,"aggression-shift-points");
            else if (s=="--cancel-shift-points") tuning.cancel_shift_points=integer<unsigned>(value,"cancel-shift-points");
            else throw std::runtime_error("unknown argument " + s);
        }
        if (gen.empty() == rep.empty()) { usage(); return 2; }
        if (!rep.empty() && !tuning.defaults()) throw std::runtime_error("replay tuning is read from trace metadata");
        const auto output = gen.empty() ? replay(rep,tape_path) :
            generate(gen,tape_path,seed,events,profile,tuning);
        if (!expected_path.empty()) {
            std::ifstream expected_file(expected_path);
            if (!expected_file) throw std::runtime_error("cannot open expected summary: " + expected_path);
            const std::string expected{std::istreambuf_iterator<char>(expected_file),
                                       std::istreambuf_iterator<char>()};
            if (output != expected) throw std::runtime_error("summary mismatch: trace or execution differs from expectation");
        }
        write_summary(summary_path, output);
        std::cout << output;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "flow simulator failed: " << e.what() << '\n';
        usage();
        return 2;
    }
}
