#include "micro_exchange/journal.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#endif

namespace micro_exchange {
namespace {
std::uint64_t checksum(const std::string& input) {
    std::uint64_t result = 14695981039346656037ULL;
    for (unsigned char c : input) { result ^= c; result *= 1099511628211ULL; }
    return result;
}
std::string hex_encode(const std::string& raw) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        encoded.push_back(digits[c >> 4]);
        encoded.push_back(digits[c & 15]);
    }
    return encoded;
}
int unhex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}
bool hex_decode(const std::string& text, std::string& output) {
    if (text.size() % 2 != 0 || text.empty() || text.size() > 1024) return false;
    output.clear();
    for (std::size_t i = 0; i < text.size(); i += 2) {
        auto hi = unhex(text[i]), lo = unhex(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        output.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}
bool uint(const std::string& s, std::uint64_t& out) {
    if (s.empty()) return false;
    auto [end, err] = std::from_chars(s.data(), s.data() + s.size(), out);
    return err == std::errc{} && end == s.data() + s.size();
}
std::string make_payload(std::uint64_t seq, std::uint64_t client,
                         const std::string& command) {
    return "MEX1|" + std::to_string(seq) + "|" + std::to_string(client) +
           "|" + hex_encode(command);
}
std::string line_for(std::uint64_t seq, std::uint64_t client,
                     const std::string& command) {
    auto payload = make_payload(seq, client, command);
    std::ostringstream output;
    output << payload << '|' << std::hex << std::setfill('0') << std::setw(16)
           << checksum(payload) << '\n';
    return output.str();
}
}  // namespace

CommandJournal::CommandJournal(std::string path) : path_(std::move(path)) {}
CommandJournal::~CommandJournal() { if (file_) std::fclose(file_); }

bool CommandJournal::recover(const std::function<bool(std::uint64_t, std::uint64_t,
                                         const std::string&)>& callback,
                             std::string& error) {
    if (!enabled()) return true;
    // Acquire an OS-owned exclusive *writer* lock before examining the journal.
    // The descriptor remains locked until the exchange exits (also on crashes).
    auto parent = std::filesystem::path(path_).parent_path();
    std::error_code dir_ec;
    if (!parent.empty()) std::filesystem::create_directories(parent, dir_ec);
    if (dir_ec) { error = "journal_directory_error"; return false; }
#ifdef _WIN32
    int fd = -1;
    if (_sopen_s(&fd, path_.c_str(), _O_CREAT | _O_RDWR | _O_APPEND | _O_BINARY,
                 _SH_DENYWR, _S_IREAD | _S_IWRITE) != 0 || fd < 0) {
        error = "journal_already_locked_or_unavailable"; return false;
    }
    file_ = _fdopen(fd, "ab+");
    if (!file_) { _close(fd); error = "journal_fdopen_failed"; return false; }
#else
    int fd = ::open(path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0600);
    if (fd < 0) { error = "journal_open_failed"; return false; }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd); error = "journal_already_locked"; return false;
    }
    file_ = fdopen(fd, "ab+");
    if (!file_) { ::close(fd); error = "journal_fdopen_failed"; return false; }
#endif
    std::ifstream input(path_, std::ios::binary);
    if (!input) { error = "could_not_read_journal"; return false; }
    std::string line;
    std::uint64_t number = 0;
    while (std::getline(input, line)) {
        ++number;
        if (line.size() > 1200) { error = "journal_line_too_long_at_" + std::to_string(number); return false; }
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true) {
            auto i = line.find('|', start);
            if (i == std::string::npos) { fields.push_back(line.substr(start)); break; }
            fields.push_back(line.substr(start, i - start)); start = i + 1;
        }
        if (fields.size() != 5 || fields[0] != "MEX1" || fields[4].size() != 16) {
            error = "bad_journal_record_at_" + std::to_string(number); return false;
        }
        std::uint64_t seq{}, client{}, sum{};
        auto [end, ec] = std::from_chars(fields[4].data(),
                                         fields[4].data() + fields[4].size(), sum, 16);
        std::string command;
        if (!uint(fields[1], seq) || !uint(fields[2], client) ||
            seq <= last_seq_ || client == 0 ||
            ec != std::errc{} || end != fields[4].data() + fields[4].size() ||
            !hex_decode(fields[3], command) ||
            checksum(make_payload(seq, client, command)) != sum ||
            !callback(seq, client, command)) {
            error = "invalid_or_corrupt_journal_entry_at_" + std::to_string(number);
            return false;
        }
        last_seq_ = seq;
    }
    if (input.bad()) { error = "journal_read_error"; return false; }
    // A partial final record cannot be safely replayed: reject until repaired.
    // Preserve the original file on failure; do not silently truncate evidence.
    if (number && std::filesystem::file_size(path_) > 0) {
        std::ifstream binary(path_, std::ios::binary | std::ios::ate);
        auto endpos = binary.tellg();
        binary.seekg(endpos - std::streamoff(1));
        if (binary.get() != '\n') { error = "truncated_journal_tail"; return false; }
    }
    return true;
}

bool CommandJournal::open_append(std::string& error) {
    if (!enabled()) return true;
    if (!file_) { error = "journal_was_not_locked"; return false; }
    return true;
}

bool CommandJournal::append_pending(std::uint64_t sequence, std::uint64_t client_id,
                                    const std::string& command, std::string& error) {
    if (!enabled()) return true;
    if (!file_ || sequence <= last_seq_ || command.empty() || command.size() > 512) {
        error = "journal_sequence_or_size"; return false;
    }
    if (appended_count_ + 1 == fault_at_) {
        if (fault_stage_ == FaultStage::BeforeWrite)
            std::_Exit(81);  // no implicit fclose flush; client gets no ACK
        if (fault_stage_ == FaultStage::WriteError) {
            error = "injected_journal_write_failure";
            return false;
        }
    }
    // The append descriptor was opened with O_APPEND/_O_APPEND and the stream
    // is held under an OS lock for its lifetime. Flush / fsync occurs at the
    // commit boundary, not at the record boundary.
    if (std::fseek(file_, 0, SEEK_END) != 0) {
        error = "journal_seek_end_failed"; return false;
    }
    const auto record = line_for(sequence, client_id, command);
    if (std::fwrite(record.data(), 1, record.size(), file_) != record.size()) {
        error = "journal_write_failed"; return false;
    }
    last_seq_ = sequence;
    ++pending_count_;
    ++appended_count_;
    return true;
}

bool CommandJournal::sync(std::string& error) {
    if (!enabled() || pending_count_ == 0) return true;
    const bool trip = fault_at_ != 0 && appended_count_ >= fault_at_;
    if (trip && fault_stage_ == FaultStage::BeforeSync)
        std::_Exit(82);  // potentially ambiguous: buffered/kernel bytes vary
    if (trip && fault_stage_ == FaultStage::SyncError) {
        error = "injected_journal_sync_failure";
        return false;
    }
    if (!file_ || std::fflush(file_) != 0) {
        error = "journal_flush_failed"; return false;
    }
#ifdef _WIN32
    if (_commit(_fileno(file_)) != 0) { error = "journal_sync_failed"; return false; }
#else
    if (fsync(fileno(file_)) != 0) { error = "journal_sync_failed"; return false; }
#endif
    ++sync_count_;
    last_group_size_ = pending_count_;
    max_group_size_ = std::max(max_group_size_, pending_count_);
    pending_count_ = 0;
    if (trip && fault_stage_ == FaultStage::AfterSync)
        std::_Exit(83);  // stable journal, but no success response sent
    return true;
}

bool CommandJournal::append(std::uint64_t sequence, std::uint64_t client_id,
                            const std::string& command, std::string& error) {
    return append_pending(sequence, client_id, command, error) && sync(error);
}
}  // namespace micro_exchange
