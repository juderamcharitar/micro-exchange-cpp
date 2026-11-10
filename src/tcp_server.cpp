// Micro Exchange v0.8: localhost-only, single-threaded TCP order gateway.
// Research/demo only. Deliberately no remote access, broker connection or auth.
#include "micro_exchange/matching_engine.hpp"
#include "micro_exchange/journal.hpp"
#include "micro_exchange/ledger.hpp"
#include "micro_exchange/sha256.hpp"

#include <algorithm>
#include <exception>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <cmath>
#include <stdexcept>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket bad_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket bad_socket = -1;
#endif

using namespace micro_exchange;

namespace {
constexpr std::size_t max_clients = 32;
constexpr std::size_t max_line = 512;
constexpr std::size_t max_input = 8192;
constexpr std::size_t max_output = 65536;
// Static, deliberately conservative lab risk policy; no monetary currencies.
constexpr Quantity risk_max_new_qty = 10000;
constexpr Quantity risk_max_market_qty = 500;
constexpr Quantity risk_max_open_qty = 100000;
constexpr std::int64_t risk_max_absolute_position = 100000;
constexpr std::size_t risk_max_open_orders = 1000;
constexpr PriceTicks risk_max_price_ticks = 1000000;
constexpr std::uint64_t risk_max_notional_tick_units = 500000000;


void close_socket(Socket fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

bool would_block() {
#ifdef _WIN32
    const int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}

bool make_nonblocking(Socket fd) {
#ifdef _WIN32
    u_long enable = 1;
    return ioctlsocket(fd, FIONBIO, &enable) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

struct SocketRuntime {
    bool valid{true};
    SocketRuntime() {
#ifdef _WIN32
        WSADATA data{};
        valid = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }
    ~SocketRuntime() {
#ifdef _WIN32
        if (valid) WSACleanup();
#endif
    }
};

template <typename T> bool decimal(const std::string& s, T& value) {
    if (s.empty()) return false;
    T parsed{};
    const char* end = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(s.data(), end, parsed);
    if (ec != std::errc{} || ptr != end) return false;
    value = parsed;
    return true;
}

std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> result;
    std::istringstream ss(line);
    for (std::string token; ss >> token;) {
        result.push_back(std::move(token));
        if (result.size() > 10) break;
    }
    return result;
}

bool valid_alias(const std::string& alias) {
    if (alias.empty() || alias.size() > 24) return false;
    for (unsigned char c : alias) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

bool valid_secret(const std::string& token) {
    if (token.size() != 64) return false;
    for (unsigned char c : token) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool digest_equal(const std::string& a, const std::string& b) {
    if (a.size() != 64 || b.size() != 64) return false;
    unsigned char mismatch = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        mismatch = static_cast<unsigned char>(mismatch | (a[i] ^ b[i]));
    return mismatch == 0;
}

struct Client {
    std::uint64_t client_id{};
    std::string input;
    std::string output;
    bool closing{false};
    bool identity_locked{false};  // REGISTER/AUTH must precede all trading commands.
};

class Server {
public:
    Server(unsigned short port, const std::string& journal_path, bool tcp_nodelay,
           bool group_commit, FaultStage fault_stage, std::uint64_t fault_at)
        : port_(port), tcp_nodelay_(tcp_nodelay), group_commit_(group_commit),
          journal_(journal_path) {
        journal_.set_fault(fault_stage, fault_at);
    }

    bool recover(std::string& error) {
        replaying_ = true;
        const bool ok = journal_.recover(
            [this](std::uint64_t seq, std::uint64_t owner_id, const std::string& raw) {
                if (seq == 0 || owner_id == 0 || seq <= next_request_seq_) return false;
                next_client_id_ = std::max(next_client_id_, owner_id);
                next_request_seq_ = seq - 1;
                command(bad_socket, owner_id, raw);
                std::string why;
                return !persistence_failed_ && next_request_seq_ == seq && engine_.check_invariants(&why) &&
                       ledger_.check(&why) && ledger_.trade_count() == engine_.stats().trades;
            }, error);
        replaying_ = false;
        return ok && journal_.open_append(error);
    }

    ~Server() {
        for (auto& [fd, client] : clients_) { (void)client; close_socket(fd); }
        if (listen_ != bad_socket) close_socket(listen_);
    }

    bool start() {
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == bad_socket) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 ONLY
        int reuse = 1;
#ifdef _WIN32
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_, static_cast<int>(max_clients)) != 0 ||
            !make_nonblocking(listen_)) return false;
        sockaddr_in actual{};
#ifdef _WIN32
        int len = sizeof(actual);
#else
        socklen_t len = sizeof(actual);
#endif
        if (getsockname(listen_, reinterpret_cast<sockaddr*>(&actual), &len) != 0) return false;
        std::cout << "READY host=127.0.0.1 port=" << ntohs(actual.sin_port)
                  << " version=1.0.0" << std::endl;
        return true;
    }

    int loop() {
        for (;;) {
            fd_set reads, writes;
            FD_ZERO(&reads); FD_ZERO(&writes);
            FD_SET(listen_, &reads);
            Socket highest = listen_;
            for (const auto& [fd, client] : clients_) {
                if (!client.closing) FD_SET(fd, &reads);
                if (!client.output.empty()) FD_SET(fd, &writes);
                highest = std::max(highest, fd);
            }
#ifdef _WIN32
            // Winsock ignores nfds; FD_SETSIZE=64 exceeds max_clients+listener.
            int ready = select(0, &reads, &writes, nullptr, nullptr);
#else
            int ready = select(highest + 1, &reads, &writes, nullptr, nullptr);
#endif
            if (ready < 0) {
                if (would_block()) continue;
                std::cerr << "select failed\n";
                return 3;
            }
            if (FD_ISSET(listen_, &reads)) accept_client();
            // Read *all* ready clients before synchronizing this event-loop
            // batch. Crucially, no socket output may be flushed until the
            // journal reports successful fsync/_commit for the whole batch.
            for (auto& [fd, client] : clients_) {
                if (!client.closing && FD_ISSET(fd, &reads)) receive(fd, client);
                if (persistence_failed_) {
                    // Never expose a success response from a failed or ambiguous
                    // append/commit. Do not flush stdio buffers on normal exit.
                    std::cerr << "fatal journal mutation failure (unacknowledged)\n";
                    std::cerr.flush();
                    std::_Exit(4);
                }
            }
            if (group_commit_ && journal_.pending()) {
                std::string error;
                if (!journal_.sync(error)) {
                    // In-memory matching already took place but NOT ONE success
                    // response from this event-loop group may leave the process.
                    // Fail-stop; the next boot strictly replays whatever reached
                    // stable storage. The client must treat disconnect as unknown.
                    std::cerr << "fatal journal group commit: " << error << '\n';
                    std::cerr.flush();
                    std::_Exit(4);
                }
            }
            std::vector<Socket> to_close;
            for (auto& [fd, client] : clients_) {
                if (FD_ISSET(fd, &writes) && !client.output.empty()) flush(fd, client);
                if (client.closing && client.output.empty()) to_close.push_back(fd);
            }
            for (Socket fd : to_close) {
                client_sockets_.erase(clients_.at(fd).client_id);
                close_socket(fd); clients_.erase(fd);
            }
        }
    }

private:
    void accept_client() {
        sockaddr_in peer{};
#ifdef _WIN32
        int len = sizeof(peer);
#else
        socklen_t len = sizeof(peer);
#endif
        Socket fd = ::accept(listen_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd == bad_socket) return;
        if (clients_.size() >= max_clients || !make_nonblocking(fd)) {
            close_socket(fd);
            return;
        }
        if (tcp_nodelay_) {
            int on = 1;
#ifdef _WIN32
            if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
#else
            if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
#endif
                close_socket(fd);
                return;
            }
        }
#ifndef _WIN32
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#endif
        Client client;
        client.client_id = ++next_client_id_;
        client_sockets_[client.client_id] = fd;
        clients_.emplace(fd, std::move(client));
        queue(fd, "HELLO client=" + std::to_string(next_client_id_) + " version=1.0.0\n");
    }

    struct Receipt {
        std::string raw;
        std::string reply;
    };
    struct RequestCapture {
        Server& server;
        Socket fd{bad_socket};
        std::uint64_t account{};
        std::uint64_t request_id{};
        std::string raw;
        std::string reply;
        bool active{false};
        bool journaled{false};
        explicit RequestCapture(Server& server) : server(server) {}
        ~RequestCapture() {
            if (active) {
                server.active_capture_ = nullptr;
                if (journaled && !server.persistence_failed_) {
                    server.receipts_[account][request_id] = Receipt{raw, reply};
                }
            }
        }
    };

    void queue(Socket fd, std::string message) {
        // Capture the exact original ACK/FILL once for RID requests, including
        // during deterministic WAL replay (where there is no connected socket).
        if (active_capture_ && active_capture_->fd == fd)
            active_capture_->reply += message;
        auto it = clients_.find(fd);
        if (it == clients_.end() || it->second.closing) return;
        Client& client = it->second;
        if (message.size() > max_output - client.output.size()) {
            // Slow readers cannot consume unbounded memory. Close rather than block
            // the single-threaded matching engine on network backpressure.
            client.output.clear();
            client.closing = true;
            return;
        }
        client.output += message;
    }

    void flush(Socket fd, Client& client) {
        if (client.output.empty()) return;
        const std::size_t len = std::min<std::size_t>(client.output.size(), 16'384);
#ifdef _WIN32
        int sent = ::send(fd, client.output.data(), static_cast<int>(len), 0);
#else
#ifdef MSG_NOSIGNAL
        int sent = static_cast<int>(::send(fd, client.output.data(), len, MSG_NOSIGNAL));
#else
        int sent = static_cast<int>(::send(fd, client.output.data(), len, 0));
#endif
#endif
        if (sent > 0) client.output.erase(0, static_cast<std::size_t>(sent));
        else if (sent == 0 || !would_block()) {
            client.output.clear();
            client.closing = true;
        }
    }

    void receive(Socket fd, Client& client) {
        char buffer[4096];
        int bytes = ::recv(fd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            if (bytes == 0 || !would_block()) {
                client.output.clear();
                client.closing = true;
            }
            return;
        }
        client.input.append(buffer, static_cast<std::size_t>(bytes));
        if (client.input.size() > max_input) {
            client.output.clear();
            client.closing = true;
            return;
        }
        for (;;) {
            std::size_t nl = client.input.find('\n');
            if (nl == std::string::npos) break;
            if (nl > max_line) {
                client.output.clear(); client.closing = true; return;
            }
            std::string line = client.input.substr(0, nl);
            client.input.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.find('\0') != std::string::npos) {
                client.output.clear(); client.closing = true; return;
            }
            command(fd, client.client_id, line);
            if (client.closing) break;
        }
        if (client.input.size() > max_line) {
            client.output.clear(); client.closing = true;
        }
    }

    void reports(Socket fd, std::uint64_t seq, const SubmitResult& r, OrderId id,
                 const std::string& action) {
        if (!r.accepted) {
            queue(fd, "ACK seq=" + std::to_string(seq) + " action=" + action +
                  " status=rejected order_id=" + std::to_string(id) + "\n");
            return;
        }
        queue(fd, "ACK seq=" + std::to_string(seq) + " action=" + action +
              " status=accepted order_id=" + std::to_string(id) +
              " leaves=" + std::to_string(r.remaining_quantity) +
              " resting=" + (r.resting ? "1" : "0") + "\n");
        for (const auto& e : r.executions) {
            const auto maker_owner = owners_.find(e.maker_id);
            const auto taker_owner = owners_.find(e.taker_id);
            const auto taker_side = order_sides_.find(e.taker_id);
            if (maker_owner == owners_.end() || taker_owner == owners_.end() ||
                taker_side == order_sides_.end()) {
                throw std::runtime_error("ledger: missing owner or side for an execution");
            }
            const bool taker_buy = taker_side->second == Side::Buy;
            std::string ledger_error;
            if (!ledger_.apply_trade(e.sequence,
                                     taker_buy ? taker_owner->second : maker_owner->second,
                                     taker_buy ? maker_owner->second : taker_owner->second,
                                     e.price_ticks, e.quantity, &ledger_error)) {
                throw std::runtime_error("ledger: failed to post execution: " + ledger_error);
            }
            last_trade_ticks_ = e.price_ticks;
            auto event = "FILL req_seq=" + std::to_string(seq) +
                         " trade_seq=" + std::to_string(e.sequence) +
                         " maker=" + std::to_string(e.maker_id) +
                         " taker=" + std::to_string(e.taker_id) +
                         " price_ticks=" + std::to_string(e.price_ticks) +
                         " qty=" + std::to_string(e.quantity) + "\n";
            queue(fd, event);  // taker gets the fill report
            auto owner = owners_.find(e.maker_id);
            const auto taker = clients_.find(fd);
            if (owner != owners_.end() && taker != clients_.end() &&
                owner->second != taker->second.client_id) {
                auto maker_socket = client_sockets_.find(owner->second);
                if (maker_socket != client_sockets_.end())
                    queue(maker_socket->second, event);  // connected maker receives fill
            }
        }
    }

    void reject(Socket fd, std::uint64_t seq, const std::string& action,
                const std::string& error) {
        queue(fd, "ACK seq=" + std::to_string(seq) + " action=" + action +
              " status=rejected reason=" + error + "\n");
    }

    std::string levels(const std::vector<LevelView>& levels) {
        std::string s;
        for (const auto& x : levels) {
            if (!s.empty()) s += ',';
            s += std::to_string(x.price_ticks) + ':' + std::to_string(x.aggregate_quantity)
                 + ':' + std::to_string(x.order_count);
        }
        return s.empty() ? "none" : s;
    }

    // Per-client exposure is derived from actual live orders, so partial fills
    // and replacements never leave a stale counter. No market data required.
    bool risk_ok(std::uint64_t client_id, OrderId ignore_id,
                 Side side, PriceTicks price, Quantity qty, bool market,
                 bool may_rest, std::string& reason) const {
        if (qty > risk_max_new_qty) { reason = "risk_order_quantity"; return false; }
        if (market && qty > risk_max_market_qty) {
            reason = "risk_market_quantity"; return false;
        }
        if (!market && price > 0) {
            if (price > risk_max_price_ticks ||
                qty > risk_max_notional_tick_units / static_cast<std::uint64_t>(price)) {
                reason = "risk_order_notional"; return false;
            }
        }
        Quantity outstanding = 0;
        Quantity buy_exposure = 0;
        Quantity sell_exposure = 0;
        std::size_t count = 0;
        for (const auto& [id, owner] : owners_) {
            if (owner != client_id || id == ignore_id) continue;
            auto live = engine_.find_resting(id);
            if (!live) continue;
            if (outstanding > risk_max_open_qty - live->leaves) {
                reason = "risk_existing_exposure_over_limit"; return false;
            }
            outstanding += live->leaves;
            if (live->side == Side::Buy) buy_exposure += live->leaves;
            else sell_exposure += live->leaves;
            ++count;
        }
        if (may_rest) {
            if (qty > risk_max_open_qty - outstanding) {
                reason = "risk_open_quantity"; return false;
            }
            if (count >= risk_max_open_orders) {
                reason = "risk_open_orders"; return false;
            }
        }
        // Conservative bound that remains valid if any subset of outstanding
        // buys/sells subsequently fills. It accounts for pending resting orders
        // and the *entire* proposed order, even when aggressive/IOC.
        const long double position = static_cast<long double>(ledger_.account(client_id).position);
        const long double max_long = position + static_cast<long double>(buy_exposure) +
            (side == Side::Buy ? static_cast<long double>(qty) : 0.0L);
        const long double min_short = position - static_cast<long double>(sell_exposure) -
            (side == Side::Sell ? static_cast<long double>(qty) : 0.0L);
        if (max_long > risk_max_absolute_position) {
            reason = "risk_position_long"; return false;
        }
        if (min_short < -risk_max_absolute_position) {
            reason = "risk_position_short"; return false;
        }
        return true;
    }

    bool journal_before_mutation(Socket fd, std::uint64_t seq,
                                 std::uint64_t client_id,
                                 const std::string& line, const std::string& action) {
        (void)fd;
        (void)action;
        if (replaying_) {
            if (active_capture_) active_capture_->journaled = true;
            return true;
        }
        std::string error;
        if ((group_commit_ ? journal_.append_pending(seq, client_id, line, error)
                           : journal_.append(seq, client_id, line, error))) {
            if (active_capture_) active_capture_->journaled = true;
            return true;
        }
        // Never emit even a rejection on an ambiguous storage failure.
        // Connection closes when event loop fail-stops.
        // Never continue processing once persistence is uncertain: otherwise
        // subsequent commands could commit past an ambiguous failed write.
        persistence_failed_ = true;
        return false;
    }

    std::uint64_t owner_for(OrderId id) const {
        auto it = owners_.find(id);
        return it == owners_.end() ? 0 : it->second;
    }

    // Reject crossing orders that would consume any of the taker's own
    // resting liquidity. Pre-flight only: rejected orders don't enter the WAL.
    bool self_trade(const NewOrder& order, std::uint64_t owner) const {
        if (replaying_) return false;  // compatibility with historical v0.4 journals
        return engine_.would_self_trade(order, owner,
                        [this](OrderId id) { return owner_for(id); });
    }

    void command(Socket fd, std::uint64_t client_id, const std::string& raw) {
        const auto seq = ++next_request_seq_;
        RequestCapture capture(*this);  // finalizes on all return paths
        std::string line = raw;
        auto t = tokens(line);
        if (persistence_failed_) { reject(fd, seq, "ALL", "journal_unavailable"); return; }

        if (t.empty()) { reject(fd, seq, "EMPTY", "bad_syntax"); return; }
        if (t[0] == "RID") {
            std::uint64_t request_id{};
            if (t.size() < 4 || !decimal(t[1], request_id) || request_id == 0 ||
                (t[2] != "NEW" && t[2] != "CANCEL" && t[2] != "REPLACE")) {
                reject(fd, seq, "RID", "bad_syntax"); return;
            }
            if (!registered_owner_.count(client_id) || !journal_.enabled()) {
                reject(fd, seq, "RID", "registered_durable_account_required"); return;
            }
            // A retry with the same request ID and unchanged command returns
            // exactly the originally committed result, never executes twice.
            auto owner_it = receipts_.find(client_id);
            if (owner_it != receipts_.end()) {
                auto old = owner_it->second.find(request_id);
                if (old != owner_it->second.end()) {
                    if (old->second.raw != raw) {
                        if (replaying_) persistence_failed_ = true;
                        else reject(fd, seq, "RID", "idempotency_conflict");
                        return;
                    }
                    if (replaying_) { persistence_failed_ = true; return; }
                    const auto& reply = old->second.reply;
                    const auto newline = reply.find('\n');
                    if (newline == std::string::npos || reply.rfind("ACK ", 0) != 0) {
                        reject(fd, seq, "RID", "missing_stored_receipt"); return;
                    }
                    queue(fd, reply.substr(0, newline) + " duplicate=1\n" +
                              reply.substr(newline + 1));
                    return;
                }
            }
            // Translate a RID envelope into an ordinary internal command;
            // journal the entire RID line (not just its inner command).
            line.clear();
            for (std::size_t i = 2; i < t.size(); ++i) {
                if (i > 2) line += ' ';
                line += t[i];
            }
            t = tokens(line);
            capture.active = true;
            capture.fd = fd;
            capture.account = client_id;
            capture.request_id = request_id;
            capture.raw = raw;
            active_capture_ = &capture;
        }
        const std::string& a = t[0];
        // Registered account creation is persisted as ALIAS + SHA-256 digest;
        // cleartext capability is NEVER written to the WAL.
        if (replaying_ && a == "REGISTERED" && t.size() == 3) {
            if (!valid_alias(t[1]) || !valid_secret(t[2]) ||
                registered_.count(t[1]) || registered_owner_.count(client_id))
                { persistence_failed_ = true; return; }
            registered_.emplace(t[1], AccountIdentity{client_id, t[2]});
            registered_owner_.emplace(client_id, t[1]);
        } else if (!replaying_ && a == "REGISTER" && t.size() == 3) {
            auto client_it = clients_.find(fd);
            if (client_it == clients_.end() || client_it->second.identity_locked ||
                registered_owner_.count(client_id)) {
                reject(fd, seq, a, "identity_not_pristine"); return;
            }
            if (!valid_alias(t[1]) || !valid_secret(t[2])) {
                reject(fd, seq, a, "bad_identity_format"); return;
            }
            if (registered_.count(t[1])) {
                reject(fd, seq, a, "alias_taken"); return;
            }
            const auto hash = sha256_hex(t[2]);
            const std::string record = "REGISTERED " + t[1] + " " + hash;
            if (!journal_.enabled()) { reject(fd, seq, a, "journal_required"); return; }
            if (!journal_before_mutation(fd, seq, client_id, record, a)) return;
            registered_.emplace(t[1], AccountIdentity{client_id, hash});
            registered_owner_.emplace(client_id, t[1]);
            client_it->second.identity_locked = true;
            queue(fd, "REGISTER seq=" + std::to_string(seq) +
                      " status=accepted account=" + t[1] +
                      " owner=" + std::to_string(client_id) + "\n");
        } else if (!replaying_ && a == "AUTH" && t.size() == 3) {
            auto client_it = clients_.find(fd);
            if (client_it == clients_.end() || client_it->second.identity_locked) {
                reject(fd, seq, a, "identity_not_pristine"); return;
            }
            auto registration = registered_.find(t[1]);
            if (!valid_alias(t[1]) || !valid_secret(t[2]) ||
                registration == registered_.end() ||
                !digest_equal(sha256_hex(t[2]), registration->second.digest)) {
                reject(fd, seq, a, "invalid_credentials"); return;
            }
            const auto target = registration->second.owner;
            if (client_sockets_.count(target) && client_sockets_.at(target) != fd) {
                reject(fd, seq, a, "account_already_connected"); return;
            }
            client_sockets_.erase(client_id);
            client_it->second.client_id = target;
            client_it->second.identity_locked = true;
            client_sockets_[target] = fd;
            queue(fd, "AUTH seq=" + std::to_string(seq) + " status=accepted account=" +
                      t[1] + " owner=" + std::to_string(target) + "\n");
        } else if (a == "PING" && t.size() == 1) {
            queue(fd, "PONG seq=" + std::to_string(seq) + "\n");
        } else if (a == "NEW") {
            OrderId id{}; Quantity qty{}; PriceTicks price{};
            if (t.size() != 7 || !decimal(t[1], id) || !decimal(t[4], price) ||
                !decimal(t[5], qty) || (t[2] != "BUY" && t[2] != "SELL") ||
                (t[3] != "LIMIT" && t[3] != "MARKET") ||
                (t[6] != "GTC" && t[6] != "IOC")) {
                reject(fd, seq, a, "bad_syntax"); return;
            }
            std::string why;
            if (!risk_ok(client_id, 0, t[2] == "BUY" ? Side::Buy : Side::Sell,
                         price, qty, t[3] == "MARKET",
                         t[3] == "LIMIT" && t[6] == "GTC", why)) {
                reject(fd, seq, a, why); return;
            }
            const NewOrder incoming{id, t[2] == "BUY" ? Side::Buy : Side::Sell,
                                    t[3] == "LIMIT" ? OrderType::Limit : OrderType::Market,
                                    price, qty, t[6] == "GTC" ? TimeInForce::GTC : TimeInForce::IOC};
            if (self_trade(incoming, client_id)) {
                reject(fd, seq, a, "self_trade_prevention"); return;
            }
            if (!journal_before_mutation(fd, seq, client_id, raw, a)) return;
            if (!replaying_) clients_.at(fd).identity_locked = true;
            auto result = engine_.submit(incoming);
            if (result.accepted) {
                owners_[id] = client_id;
                order_sides_[id] = t[2] == "BUY" ? Side::Buy : Side::Sell;
            }
            reports(fd, seq, result, id, a);
        } else if (a == "CANCEL" || a == "REPLACE") {
            OrderId id{}; PriceTicks price{}; Quantity qty{};
            if ((a == "CANCEL" && t.size() != 2) ||
                (a == "REPLACE" && t.size() != 4) || !decimal(t[1], id) ||
                (a == "REPLACE" && (!decimal(t[2], price) || !decimal(t[3], qty)))) {
                reject(fd, seq, a, "bad_syntax"); return;
            }
            auto owner = owners_.find(id);
            if (owner == owners_.end() || owner->second != client_id) {
                reject(fd, seq, a, "not_order_owner"); return;
            }
            if (a == "REPLACE") {
                std::string why;
                if (!risk_ok(client_id, id, order_sides_.at(id),
                             price, qty, false, true, why)) {
                    reject(fd, seq, a, why); return;
                }
                const NewOrder incoming{id, order_sides_.at(id), OrderType::Limit,
                                        price, qty, TimeInForce::GTC};
                if (self_trade(incoming, client_id)) {
                    reject(fd, seq, a, "self_trade_prevention"); return;
                }
            }
            if (!journal_before_mutation(fd, seq, client_id, raw, a)) return;
            if (!replaying_) clients_.at(fd).identity_locked = true;
            if (a == "CANCEL") {
                auto result = engine_.cancel(id);
                if (result.cancelled) {
                    queue(fd, "ACK seq=" + std::to_string(seq) +
                              " action=CANCEL status=accepted order_id=" +
                              std::to_string(id) + " removed=" +
                              std::to_string(result.removed_quantity) + "\n");
                } else reject(fd, seq, a, "not_resting");
            } else {
                reports(fd, seq, engine_.replace(id, price, qty), id, a);
            }
        } else if (a == "ACCOUNT" && (t.size() == 1 || t.size() == 2)) {
            PriceTicks mark = last_trade_ticks_;
            if (t.size() == 2 && (!decimal(t[1], mark) || mark <= 0 || mark > 1000000000)) {
                reject(fd, seq, a, "bad_mark_price"); return;
            }
            const auto acct = ledger_.account(client_id);
            const long double unrealized = mark > 0 ?
                static_cast<long double>(acct.position) *
                    (static_cast<long double>(mark) - acct.average_entry_ticks) : 0.0L;
            const long double equity = acct.cash_flow_tick_units +
                static_cast<long double>(acct.position) * mark;
            auto format = [](long double value) {
                std::ostringstream os;
                os << std::fixed << std::setprecision(2) << value;
                return os.str();
            };
            queue(fd, "ACCOUNT seq=" + std::to_string(seq) +
                      " owner=" + std::to_string(client_id) +
                      " position=" + std::to_string(acct.position) +
                      " cash_flow_tick_units=" + format(acct.cash_flow_tick_units) +
                      " avg_entry_ticks=" + format(acct.average_entry_ticks) +
                      " realized_pnl_tick_units=" + format(acct.realized_pnl_tick_units) +
                      " mark_ticks=" + std::to_string(mark) +
                      " unrealized_pnl_tick_units=" + format(unrealized) +
                      " equity_tick_units=" + format(equity) +
                      " fill_sides=" + std::to_string(acct.fill_sides) + "\n");
        } else if (a == "LEDGER_CHECK" && t.size() == 1) {
            std::string error;
            const bool valid = ledger_.check(&error) &&
                               ledger_.trade_count() == engine_.stats().trades;
            queue(fd, "LEDGER_CHECK seq=" + std::to_string(seq) +
                      " status=" + (valid ? "ok" : "failed") +
                      " trades=" + std::to_string(ledger_.trade_count()) +
                      " accounts=" + std::to_string(ledger_.account_count()) + "\n");
        } else if (a == "BOOK") {
            std::uint64_t depth = 5;
            if (t.size() == 2 && !decimal(t[1], depth)) {
                reject(fd, seq, a, "bad_syntax"); return;
            }
            if ((t.size() != 1 && t.size() != 2) || depth > 100) {
                reject(fd, seq, a, "bad_depth"); return;
            }
            auto book = engine_.snapshot(static_cast<std::size_t>(depth));
            queue(fd, "BOOK seq=" + std::to_string(seq) +
                      " bids=" + levels(book.bids) + " asks=" + levels(book.asks) + "\n");
        } else if (a == "JOURNAL_STATS" && t.size() == 1) {
            queue(fd, "JOURNAL_STATS seq=" + std::to_string(seq) +
                      " mode=" + (group_commit_ ? "group" : "sync") +
                      " appended=" + std::to_string(journal_.appended_count()) +
                      " syncs=" + std::to_string(journal_.sync_count()) +
                      " max_group=" + std::to_string(journal_.max_group_size()) +
                      " last_group=" + std::to_string(journal_.last_group_size()) + "\n");
        } else if (a == "STATS" && t.size() == 1) {
            auto s = engine_.stats();
            queue(fd, "STATS seq=" + std::to_string(seq) +
                      " accepted=" + std::to_string(s.accepted_new) +
                      " cancelled=" + std::to_string(s.cancelled) +
                      " replaced=" + std::to_string(s.replaced) +
                      " rejected=" + std::to_string(s.rejected) +
                      " trades=" + std::to_string(s.trades) +
                      " resting=" + std::to_string(s.resting_orders) + "\n");
        } else if (a == "CHECK" && t.size() == 1) {
            std::string error;
            queue(fd, "CHECK seq=" + std::to_string(seq) +
                      " status=" + (engine_.check_invariants(&error) && ledger_.check(&error) &&
                                    ledger_.trade_count() == engine_.stats().trades ? "ok" : "failed") + "\n");
        } else if (a == "QUIT" && t.size() == 1) {
            queue(fd, "BYE seq=" + std::to_string(seq) + "\n");
            // Deliberately preserve resting orders on disconnect in this version.
            // Deliberate GTC semantics: orders persist; registered owners can reclaim after reconnect.
            clients_.at(fd).closing = true;
        } else {
            reject(fd, seq, a, "unknown_or_invalid_command");
        }
    }

    unsigned short port_;
    bool tcp_nodelay_{false};
    bool group_commit_{false};
    Socket listen_{bad_socket};
    MatchingEngine engine_;
    ExecutionLedger ledger_;
    PriceTicks last_trade_ticks_{};
    CommandJournal journal_;
    bool replaying_{false};
    bool persistence_failed_{false};
    std::map<Socket, Client> clients_;
    struct AccountIdentity { std::uint64_t owner; std::string digest; };
    std::unordered_map<std::string, AccountIdentity> registered_;
    std::unordered_map<std::uint64_t, std::string> registered_owner_;
    std::unordered_map<OrderId, std::uint64_t> owners_;
    std::unordered_map<OrderId, Side> order_sides_;
    std::unordered_map<std::uint64_t, Socket> client_sockets_;
    std::unordered_map<std::uint64_t, std::unordered_map<std::uint64_t, Receipt>> receipts_;
    RequestCapture* active_capture_{nullptr};
    std::uint64_t next_client_id_{0};
    std::uint64_t next_request_seq_{0};
};
}  // namespace

int main(int argc, char** argv) {
    unsigned short port = 8765;
    std::string journal_path;
    bool tcp_nodelay = false;
    bool group_commit = false;
    FaultStage fault_stage = FaultStage::None;
    std::uint64_t fault_at = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            std::uint32_t parsed{};
            if (!decimal(argv[++i], parsed) || parsed > 65535) {
                std::cerr << "bad --port value\n"; return 2;
            }
            port = static_cast<unsigned short>(parsed);
        } else if (arg == "--journal" && i + 1 < argc) {
            journal_path = argv[++i];
            if (journal_path.empty()) { std::cerr << "empty journal path\n"; return 2; }
        } else if (arg == "--tcp-nodelay") {
            tcp_nodelay = true;
        } else if (arg == "--fault-stage" && i + 1 < argc) {
            const std::string stage = argv[++i];
            if (stage == "before-write") fault_stage = FaultStage::BeforeWrite;
            else if (stage == "before-sync") fault_stage = FaultStage::BeforeSync;
            else if (stage == "after-sync") fault_stage = FaultStage::AfterSync;
            else if (stage == "write-error") fault_stage = FaultStage::WriteError;
            else if (stage == "sync-error") fault_stage = FaultStage::SyncError;
            else { std::cerr << "invalid fault stage\n"; return 2; }
        } else if (arg == "--fault-at" && i + 1 < argc) {
            if (!decimal(argv[++i], fault_at) || fault_at == 0) {
                std::cerr << "invalid fault count\n"; return 2;
            }
        } else if (arg == "--journal-mode" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode != "sync" && mode != "group") {
                std::cerr << "journal mode must be sync or group\n"; return 2;
            }
            group_commit = (mode == "group");
        } else {
            std::cerr << "Usage: exchange_server [--port 0..65535] [--journal path] [--journal-mode sync|group] [--tcp-nodelay] [--fault-stage before-write|before-sync|after-sync|write-error|sync-error --fault-at N]\n";
            return 2;
        }
    }
    // Fault injection is explicit and limited to localhost test servers.
    if ((fault_stage == FaultStage::None) != (fault_at == 0) ||
        (fault_stage != FaultStage::None && journal_path.empty())) {
        std::cerr << "fault injection requires --fault-stage, --fault-at and --journal\n";
        return 2;
    }
    SocketRuntime runtime;
    if (!runtime.valid) { std::cerr << "Socket initialization failed\n"; return 2; }
    Server server(port, journal_path, tcp_nodelay, group_commit, fault_stage, fault_at);
    std::string why;
    if (!server.recover(why)) {
        std::cerr << "Journal recovery failed (refusing to start): " << why << '\n';
        return 2;
    }
    if (!server.start()) { std::cerr << "Unable to bind localhost TCP listener\n"; return 2; }
    return server.loop();
}
