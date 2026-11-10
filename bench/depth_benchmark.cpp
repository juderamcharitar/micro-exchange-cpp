// Deterministic synthetic market-workload microbenchmark, not real order-flow data.
#include "micro_exchange/matching_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace micro_exchange;
namespace {
using Clock = std::chrono::steady_clock;
using Ns = std::uint64_t;

std::size_t positive(const std::string& s, std::size_t max) {
    if (s.empty() || s[0] == '-') throw std::invalid_argument("expected positive integer");
    std::size_t pos = 0;
    unsigned long long x = std::stoull(s, &pos);
    if (pos != s.size() || x == 0 || x > max) throw std::invalid_argument("integer out of range");
    return static_cast<std::size_t>(x);
}

std::vector<std::size_t> parse_levels(const std::string& raw) {
    std::vector<std::size_t> values;
    std::size_t start = 0;
    while (start < raw.size()) {
        auto stop = raw.find(',', start);
        values.push_back(positive(raw.substr(start, stop == std::string::npos ? stop : stop-start), 10000));
        if (stop == std::string::npos) break;
        start = stop + 1;
    }
    if (values.empty() || values.size() > 12) throw std::invalid_argument("expected 1..12 depth values");
    return values;
}

Ns measure(const std::function<void()>& operation) {
    auto begin = Clock::now();
    operation();
    return static_cast<Ns>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count());
}

Ns quantile(const std::vector<Ns>& sorted, int percent) {
    if (sorted.empty()) throw std::logic_error("no operation samples");
    return sorted[(sorted.size()-1) * static_cast<std::size_t>(percent) / 100];
}

struct Trial {
    double duration_seconds{};
    double throughput{};
    std::vector<Ns> latency;
    std::uint64_t trades{};
};

Trial run(std::size_t levels, std::size_t cycles) {
    MatchingEngine book;
    OrderId id = 1;
    for (std::size_t level=0; level<levels; ++level) {
        const auto bid = book.submit({id++, Side::Buy, OrderType::Limit,
                                      10000-static_cast<PriceTicks>(level), 1, TimeInForce::GTC});
        const auto ask = book.submit({id++, Side::Sell, OrderType::Limit,
                                      20000+static_cast<PriceTicks>(level), 1, TimeInForce::GTC});
        if (!bid.accepted || !bid.resting || !ask.accepted || !ask.resting)
            throw std::runtime_error("failed to preseed stable noncrossing book");
    }

    Trial trial;
    trial.latency.reserve(cycles*6);
    // Six repeatable operations: add maker, two partial fills, add passive bid,
    // decrease quantity without losing priority, then cancel. Best-price liquidity
    // is replenished inside each cycle. The 2*levels baseline resting orders remain.
    const auto begin = Clock::now();
    for (std::size_t cycle=0; cycle<cycles; ++cycle) {
        const OrderId maker_id=id++;
        SubmitResult maker;
        trial.latency.push_back(measure([&] {
            maker=book.submit({maker_id, Side::Sell, OrderType::Limit, 15000, 2, TimeInForce::GTC});
        }));
        if (!maker.accepted || !maker.resting || maker.remaining_quantity!=2)
            throw std::runtime_error("maker placement failure");

        for (int part=0; part<2; ++part) {
            const OrderId taker_id=id++;
            SubmitResult taker;
            trial.latency.push_back(measure([&] {
                taker=book.submit({taker_id, Side::Buy, OrderType::Market, 0, 1, TimeInForce::IOC});
            }));
            if (!taker.accepted || taker.executions.size()!=1 ||
                taker.executions[0].maker_id!=maker_id ||
                taker.executions[0].quantity!=1 || taker.executions[0].price_ticks!=15000)
                throw std::runtime_error("partial/full execution mismatch");
        }

        const OrderId bid_id=id++;
        SubmitResult bid;
        trial.latency.push_back(measure([&] {
            bid=book.submit({bid_id, Side::Buy, OrderType::Limit, 9500, 2, TimeInForce::GTC});
        }));
        if (!bid.accepted || !bid.resting) throw std::runtime_error("passive bid failure");

        SubmitResult replacement;
        trial.latency.push_back(measure([&] { replacement=book.replace(bid_id, 9500, 1); }));
        if (!replacement.accepted || replacement.reason!="replaced; priority retained")
            throw std::runtime_error("same-price replacement failure");

        CancelResult cancellation;
        trial.latency.push_back(measure([&] { cancellation=book.cancel(bid_id); }));
        if (!cancellation.cancelled || cancellation.removed_quantity!=1)
            throw std::runtime_error("cancel failure");
    }
    const auto end = Clock::now();
    trial.duration_seconds=std::chrono::duration<double>(end-begin).count();
    trial.trades=book.stats().trades;
    std::string reason;
    if (!book.check_invariants(&reason) || book.stats().resting_orders!=2*levels ||
        book.stats().trades != 2*cycles) {
        throw std::runtime_error("final book invariant/trade count failed: " + reason);
    }
    trial.throughput=static_cast<double>(cycles*6)/trial.duration_seconds;
    return trial;
}
}

int main(int argc, char** argv) {
    std::vector<std::size_t> levels{10,100,1000};
    std::size_t cycles=3000, repetitions=3;
    std::string csv_path;
    try {
        for (int i=1; i<argc; ++i) {
            std::string arg=argv[i];
            if (arg=="--levels" && i+1<argc) levels=parse_levels(argv[++i]);
            else if (arg=="--cycles" && i+1<argc) cycles=positive(argv[++i],100000);
            else if (arg=="--repetitions" && i+1<argc) repetitions=positive(argv[++i],12);
            else if (arg=="--csv" && i+1<argc) csv_path=argv[++i];
            else throw std::invalid_argument("Usage: exchange_depth_bench [--levels 10,100,1000] "
                                             "[--cycles 3000] [--repetitions 3] [--csv file]");
        }
        std::ofstream csv;
        if (!csv_path.empty()) {
            const auto dir=std::filesystem::path(csv_path).parent_path();
            if (!dir.empty()) std::filesystem::create_directories(dir);
            csv.open(csv_path, std::ios::trunc);
            if (!csv) throw std::runtime_error("cannot open CSV output");
            csv << "levels,preloaded_orders,cycles,repetitions,operations,trades_per_repetition,"
                   "median_throughput_ops_per_sec,p50_op_ns,p99_op_ns\n";
        }
        std::cout << "workload=synthetic stable-depth six-operation cycle: "
                     "maker, two partial fills, passive bid, reduce, cancel\n"
                     "measurement=single-threaded engine API; startup/preload excluded; "
                     "operation samples include clock overhead\n"
                  << "levels,preloaded_orders,cycles,repetitions,operations,trades_per_repetition,"
                     "median_throughput_ops_per_sec,p50_op_ns,p99_op_ns\n";
        for (std::size_t depth : levels) {
            std::vector<double> throughput;
            std::vector<Ns> latency;
            latency.reserve(cycles*repetitions*6);
            for (std::size_t r=0; r<repetitions; ++r) {
                auto result=run(depth,cycles);
                throughput.push_back(result.throughput);
                latency.insert(latency.end(),result.latency.begin(),result.latency.end());
            }
            std::sort(throughput.begin(),throughput.end());
            std::sort(latency.begin(),latency.end());
            const auto n=cycles*6*repetitions;
            std::ostringstream row;
            row << depth << ',' << 2*depth << ',' << cycles << ',' << repetitions << ','
                << n << ',' << cycles*2 << ',' << std::fixed << std::setprecision(2)
                << throughput[(throughput.size()-1)/2] << ',' << quantile(latency,50)
                << ',' << quantile(latency,99) << '\n';
            std::cout << row.str();
            if (csv.is_open()) csv << row.str();
        }
        std::cout << "NOTE: Not exchange throughput or an empirical order-flow replay; "
                     "fixed scripted operations and chrono overhead affect results.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "depth benchmark failed: " << e.what() << '\n';
        return 2;
    }
}
