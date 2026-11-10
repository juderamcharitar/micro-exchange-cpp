#include "micro_exchange/matching_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace micro_exchange;

namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b-a).count());
}
std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, unsigned hundredths) {
    if (sorted.empty()) return 0;
    return sorted[(sorted.size()-1)*hundredths/100];
}
}

int main(int argc, char** argv) {
    std::size_t iterations = 100'000;
    if (argc > 2) {
        std::cerr << "Usage: exchange_bench [iterations]\n"; return 2;
    }
    if (argc == 2) {
        try { iterations = static_cast<std::size_t>(std::stoull(argv[1])); }
        catch (...) { std::cerr << "invalid iterations\n"; return 2; }
        if (iterations == 0 || iterations > 5'000'000) {
            std::cerr << "iterations must be 1..5000000\n"; return 2;
        }
    }
    MatchingEngine engine;
    std::vector<std::uint64_t> samples;
    samples.reserve(iterations * 2);
    const auto start = Clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto maker_id = 2 * static_cast<OrderId>(i) + 1;
        const auto taker_id = maker_id + 1;
        auto a = Clock::now();
        const auto maker = engine.submit({maker_id, Side::Sell, OrderType::Limit,
                                         10'000, 1, TimeInForce::GTC});
        auto b = Clock::now();
        const auto taker = engine.submit({taker_id, Side::Buy, OrderType::Market,
                                         0, 1, TimeInForce::IOC});
        auto c = Clock::now();
        if (!maker.accepted || !maker.resting || taker.executions.size() != 1 ||
            taker.executions.front().maker_id != maker_id) {
            std::cerr << "benchmark logic failure\n"; return 3;
        }
        samples.push_back(ns_between(a,b));
        samples.push_back(ns_between(b,c));
    }
    const auto finish = Clock::now();
    std::string error;
    if (!engine.check_invariants(&error) || engine.stats().resting_orders != 0) {
        std::cerr << "book invariant failure: " << error << '\n'; return 4;
    }
    const double seconds = std::chrono::duration<double>(finish-start).count();
    std::sort(samples.begin(), samples.end());
    std::cout << "mode=single-threaded in-memory add+immediate-execute synthetic workload\n"
              << "messages=" << 2 * iterations << " trades=" << engine.stats().trades << '\n'
              << "elapsed_seconds=" << seconds << '\n'
              << "throughput_messages_per_second=" << (2.0 * iterations / seconds) << '\n'
              << "p50_operation_ns=" << percentile(samples,50) << '\n'
              << "p99_operation_ns=" << percentile(samples,99) << '\n'
              << "NOTE: timings include chrono per-op overhead; not wire-to-wire latency.\n";
    return 0;
}
