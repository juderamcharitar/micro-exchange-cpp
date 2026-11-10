#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace micro_exchange {

// Checksummed, strictly replayed single-writer WAL; optionally batch fsyncs.
// Exactly one journal writer per exchange process; not a database or HA log.
// Deterministic process-stop fault points for laboratory tests only. These
// mimic uncertain network outcomes; they are NOT real disk hardware failures.
enum class FaultStage { None, BeforeWrite, BeforeSync, AfterSync, WriteError, SyncError };

class CommandJournal {
public:
    explicit CommandJournal(std::string path);
    ~CommandJournal();
    CommandJournal(const CommandJournal&) = delete;
    CommandJournal& operator=(const CommandJournal&) = delete;

    // Replays all prior validated entries before the journal can be reopened for appends.
    // Return false on any truncated/corrupt/non-monotonic entry (fail closed).
    bool recover(const std::function<bool(std::uint64_t, std::uint64_t,
                                          const std::string&)>& callback,
                 std::string& error);
    bool open_append(std::string& error);
    // Appends AND flushes to operating-system storage before returning true.
    bool append(std::uint64_t sequence, std::uint64_t client_id,
                const std::string& command, std::string& error);
    // Group mode: queue whole records in the stdio stream, then synchronize the
    // entire group ONCE. No response may be sent before sync() succeeds.
    bool append_pending(std::uint64_t sequence, std::uint64_t client_id,
                        const std::string& command, std::string& error);
    bool sync(std::string& error);
    bool pending() const { return pending_count_ != 0; }
    std::uint64_t appended_count() const { return appended_count_; }
    std::uint64_t sync_count() const { return sync_count_; }
    std::uint64_t max_group_size() const { return max_group_size_; }
    std::uint64_t last_group_size() const { return last_group_size_; }
    bool enabled() const { return !path_.empty(); }
    void set_fault(FaultStage stage, std::uint64_t at_mutation) {
        fault_stage_ = stage;
        fault_at_ = at_mutation;
    }

private:
    std::string path_;
    std::FILE* file_{nullptr};
    std::uint64_t last_seq_{0};
    std::uint64_t pending_count_{0};
    std::uint64_t appended_count_{0};
    std::uint64_t sync_count_{0};
    std::uint64_t last_group_size_{0};
    std::uint64_t max_group_size_{0};
    FaultStage fault_stage_{FaultStage::None};
    std::uint64_t fault_at_{0};
};

}  // namespace micro_exchange
