#include "micro_exchange/matching_engine.hpp"

#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace micro_exchange;

namespace {

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void show_result(const SubmitResult& r) {
    if (!r.accepted) {
        std::cout << "REJECT " << r.reason << '\n';
        return;
    }
    for (const auto& e : r.executions) {
        std::cout << "TRADE seq=" << e.sequence << " maker=" << e.maker_id
                  << " taker=" << e.taker_id << " price_ticks=" << e.price_ticks
                  << " qty=" << e.quantity << '\n';
    }
    std::cout << "ACK " << r.reason << " leaves=" << r.remaining_quantity
              << " resting=" << (r.resting ? "yes" : "no") << '\n';
}

bool handle(const std::string& line, MatchingEngine& engine) {
    auto cleaned = line.substr(0, line.find('#'));
    std::istringstream in(cleaned);
    std::string action;
    if (!(in >> action)) return true;
    action = upper(action);
    if (action == "QUIT" || action == "EXIT") return false;
    if (action == "HELP") {
        std::cout << "NEW <id> <BUY|SELL> <LIMIT|MARKET> <price_ticks> <qty> <GTC|IOC>\n"
                     "CANCEL <id>\nREPLACE <id> <new_price_ticks> <new_remaining_qty>\n"
                     "BOOK [depth]\nSTATS\nCHECK\nQUIT\n";
        return true;
    }
    if (action == "NEW") {
        OrderId id{};
        Quantity qty{};
        PriceTicks price{};
        std::string side, type, tif, extra;
        if (!(in >> id >> side >> type >> price >> qty >> tif) || (in >> extra)) {
            std::cout << "ERROR syntax: NEW id BUY|SELL LIMIT|MARKET price_ticks qty GTC|IOC\n";
            return true;
        }
        side = upper(side); type = upper(type); tif = upper(tif);
        if ((side != "BUY" && side != "SELL") ||
            (type != "LIMIT" && type != "MARKET") ||
            (tif != "GTC" && tif != "IOC")) {
            std::cout << "ERROR invalid side, type or time-in-force\n";
            return true;
        }
        show_result(engine.submit({id, side == "BUY" ? Side::Buy : Side::Sell,
                                   type == "LIMIT" ? OrderType::Limit : OrderType::Market,
                                   price, qty, tif == "GTC" ? TimeInForce::GTC : TimeInForce::IOC}));
    } else if (action == "CANCEL") {
        OrderId id{};
        std::string extra;
        if (!(in >> id) || (in >> extra)) {
            std::cout << "ERROR syntax: CANCEL id\n"; return true;
        }
        const auto r = engine.cancel(id);
        if (r.cancelled) std::cout << "CANCELLED id=" << id << " qty=" << r.removed_quantity << '\n';
        else std::cout << "REJECT " << r.reason << '\n';
    } else if (action == "REPLACE") {
        OrderId id{};
        PriceTicks price{};
        Quantity qty{};
        std::string extra;
        if (!(in >> id >> price >> qty) || (in >> extra)) {
            std::cout << "ERROR syntax: REPLACE id new_price_ticks new_remaining_qty\n";
            return true;
        }
        show_result(engine.replace(id, price, qty));
    } else if (action == "BOOK") {
        std::size_t depth = 5;
        std::string extra;
        if (in >> depth) {
            if (in >> extra) { std::cout << "ERROR syntax: BOOK [depth]\n"; return true; }
        } else if (!in.eof()) {
            std::cout << "ERROR syntax: BOOK [depth]\n"; return true;
        }
        if (depth > 100) depth = 100;
        auto book = engine.snapshot(depth);
        std::cout << "BIDS (best first)\n";
        for (const auto& x : book.bids)
            std::cout << "  " << x.price_ticks << " qty=" << x.aggregate_quantity
                      << " orders=" << x.order_count << '\n';
        std::cout << "ASKS (best first)\n";
        for (const auto& x : book.asks)
            std::cout << "  " << x.price_ticks << " qty=" << x.aggregate_quantity
                      << " orders=" << x.order_count << '\n';
    } else if (action == "STATS") {
        const auto s = engine.stats();
        std::cout << "STATS accepted=" << s.accepted_new << " cancelled=" << s.cancelled
                  << " replaced=" << s.replaced << " rejected=" << s.rejected
                  << " trades=" << s.trades << " executed_qty=" << s.executed_quantity
                  << " resting=" << s.resting_orders << '\n';
    } else if (action == "CHECK") {
        std::string error;
        if (engine.check_invariants(&error)) std::cout << "CHECK OK\n";
        else std::cout << "CHECK FAIL " << error << '\n';
    } else {
        std::cout << "ERROR unknown command: " << action << '\n';
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::ifstream script;
    std::istream* source = &std::cin;
    if (argc == 3 && std::string(argv[1]) == "--script") {
        script.open(argv[2]);
        if (!script) {
            std::cerr << "Unable to read script: " << argv[2] << '\n';
            return 2;
        }
        source = &script;
    } else if (argc != 1) {
        std::cerr << "Usage: exchange_cli [--script path]\n";
        return 2;
    }
    MatchingEngine engine;
    if (source == &std::cin) std::cout << "micro-exchange v0.1 (HELP for commands)\n";
    std::string line;
    while (std::getline(*source, line)) {
        if (!handle(line, engine)) break;
    }
    std::string error;
    if (!engine.check_invariants(&error)) {
        std::cerr << "Invariant failure at shutdown: " << error << '\n';
        return 3;
    }
    return 0;
}
