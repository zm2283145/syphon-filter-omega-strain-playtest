#pragma once
// PORT-ONLY: application commit/checkpoint protocol for reliable events addressed to the LAN session host.
// Transport ACK is intentionally insufficient: an event is retired only after the application pump applies a
// deterministic batch and publishes a checkpoint/receipt. The live LanDme framing does not use this yet; LanDme
// owns one log so the remaining play-state checkpoint hook can wire it without changing these safety semantics.

#include "lan_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace omega::lan {

struct LanDmeCommitConfig {
    std::size_t max_retained_events = 256;
    std::size_t max_retained_bytes = 256 * 1024;
    std::size_t max_pending_events = 256;
    std::size_t max_pending_bytes = 256 * 1024;
    std::uint64_t receipt_timeout_ms = 5000;
};

struct LanDmeEvent {
    std::uint64_t session_id = 0;
    std::uint32_t created_epoch = 0;
    std::uint16_t origin = 0;
    std::uint64_t sequence = 0;
    std::uint64_t payload_digest = 0;
    std::vector<std::uint8_t> payload;
    friend bool operator==(const LanDmeEvent&, const LanDmeEvent&) = default;
};

struct LanDmeCheckpoint {
    std::uint64_t session_id = 0;
    std::uint32_t epoch = 0;
    std::uint16_t host = 0;
    std::uint64_t sequence = 0;
    std::array<std::uint64_t, kLanMaxClients> applied_through{};
    std::uint64_t event_digest = 0;
    std::uint64_t state_digest = 0;
    std::uint64_t digest = 0;
    friend bool operator==(const LanDmeCheckpoint&, const LanDmeCheckpoint&) = default;
};

struct LanDmeReceipt {
    std::uint64_t session_id = 0;
    std::uint32_t epoch = 0;
    std::uint16_t host = 0;
    std::uint16_t origin = 0;
    std::uint64_t through = 0;
    LanDmeCheckpoint checkpoint;
    friend bool operator==(const LanDmeReceipt&, const LanDmeReceipt&) = default;
};

enum class LanDmeCommitError {
    None,
    WrongSession,
    StaleEpoch,
    FutureEpoch,
    WrongHost,
    WrongOrigin,
    InvalidEvent,
    InvalidCheckpoint,
    IncompatibleState,
    SequenceGap,
    Backpressure,
    ReceiptTimeout,
    ApplicationFailed,
    BatchOpen,
    NoBatch,
    NotMigrationReady,
};

enum class LanDmeReceiveResult { Buffered, DuplicatePending, DuplicateCommitted };

struct LanDmeRecordResult {
    LanDmeCommitError error = LanDmeCommitError::None;
    std::optional<LanDmeEvent> event;
};

struct LanDmeReceiveOutcome {
    LanDmeCommitError error = LanDmeCommitError::None;
    std::optional<LanDmeReceiveResult> result;
};

struct LanDmeCommitOutcome {
    LanDmeCommitError error = LanDmeCommitError::None;
    std::optional<LanDmeCheckpoint> checkpoint;
    std::vector<LanDmeReceipt> receipts;
};

std::uint64_t lan_dme_payload_digest(std::span<const std::uint8_t> payload);
std::uint64_t lan_dme_checkpoint_digest(const LanDmeCheckpoint& checkpoint);
bool lan_dme_checkpoint_valid(const LanDmeCheckpoint& checkpoint);

class LanDmeCommitLog {
public:
    LanDmeCommitLog(std::uint64_t session_id, std::uint16_t local, std::uint32_t epoch, std::uint16_t host,
                    LanClock clock, LanDmeCommitConfig config = {});

    std::uint64_t session_id() const { return session_id_; }
    std::uint32_t epoch() const { return epoch_; }
    std::uint16_t host() const { return host_; }
    LanDmeCommitError error() const { return error_; }
    bool migration_ready() const { return migration_ready_; }
    std::size_t retained_events() const;
    std::size_t retained_bytes() const { return retained_bytes_; }
    std::size_t pending_events() const;
    std::size_t pending_bytes() const { return pending_bytes_; }
    std::uint64_t committed_through(std::uint16_t origin) const;
    const std::optional<LanDmeCheckpoint>& checkpoint() const { return checkpoint_; }

    // Sender side: record before handing the event to the transport. Backpressure and a receipt timeout return an
    // explicit error and never fabricate a sent/success result.
    LanDmeRecordResult record_host_event(std::span<const std::uint8_t> payload);
    LanDmeCommitError accept_receipt(const LanDmeReceipt& receipt);
    std::vector<LanDmeEvent> replay_suffix() const;
    void service_timeout();

    // Host side: receive may arrive reordered. application_batch() releases only each sender's contiguous prefix,
    // sorted by {sequence, origin}; the caller applies all of it, computes its state digest, then commits.
    LanDmeReceiveOutcome receive_host_event(const LanDmeEvent& event, std::uint32_t delivery_epoch);
    std::pair<LanDmeCommitError, std::vector<LanDmeEvent>> application_batch();
    LanDmeCommitOutcome commit_application(std::uint64_t state_digest);
    void fail_application();

    // Checkpoints make a joined peer/elected successor eligible only when its independently computed state digest
    // matches. An epoch transition is exact-next and starts from the last committed checkpoint.
    LanDmeCommitError accept_checkpoint(const LanDmeCheckpoint& checkpoint, std::uint64_t local_state_digest);
    LanDmeCommitError begin_epoch(std::uint32_t next_epoch, std::uint16_t new_host,
                                  const LanDmeCheckpoint& base, std::uint64_t local_state_digest);

private:
    struct Retained {
        LanDmeEvent event;
        std::uint64_t sent_at = 0;
    };

    LanDmeCommitError validate_event(const LanDmeEvent& event, std::uint32_t delivery_epoch) const;
    void remember_checkpoint(const LanDmeCheckpoint& checkpoint);
    std::uint64_t next_event_digest(std::uint64_t digest, const LanDmeEvent& event) const;
    void clear_incoming();

    std::uint64_t session_id_ = 0;
    std::uint16_t local_ = 0;
    std::uint32_t epoch_ = 0;
    std::uint16_t host_ = 0;
    LanClock clock_;
    LanDmeCommitConfig config_;
    LanDmeCommitError error_ = LanDmeCommitError::None;

    std::array<std::uint64_t, kLanMaxClients> next_sequence_{};
    std::array<std::uint64_t, kLanMaxClients> applied_through_{};
    std::array<std::deque<Retained>, kLanMaxClients> retained_;
    std::array<std::map<std::uint64_t, LanDmeEvent>, kLanMaxClients> incoming_;
    std::vector<LanDmeEvent> staged_;
    std::size_t retained_bytes_ = 0;
    std::size_t pending_bytes_ = 0;
    bool batch_open_ = false;
    bool migration_ready_ = false;
    std::uint64_t checkpoint_sequence_ = 0;
    std::uint64_t event_digest_ = 1469598103934665603ull;
    std::optional<LanDmeCheckpoint> checkpoint_;
};

}  // namespace omega::lan
