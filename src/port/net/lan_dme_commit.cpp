#include "lan_dme_commit.hpp"

#include <algorithm>
#include <limits>

namespace omega::lan {
namespace {

void hash_byte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
}

template <typename T>
void hash_word(std::uint64_t& hash, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) hash_byte(hash, static_cast<std::uint8_t>(value >> (i * 8)));
}

std::size_t event_bytes(const LanDmeEvent& event) { return event.payload.size(); }

}  // namespace

std::uint64_t lan_dme_payload_digest(std::span<const std::uint8_t> payload) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const auto byte : payload) hash_byte(hash, byte);
    return hash;
}

std::uint64_t lan_dme_checkpoint_digest(const LanDmeCheckpoint& checkpoint) {
    std::uint64_t hash = 1469598103934665603ull;
    hash_word(hash, checkpoint.session_id);
    hash_word(hash, checkpoint.epoch);
    hash_word(hash, checkpoint.host);
    hash_word(hash, checkpoint.sequence);
    for (const auto boundary : checkpoint.applied_through) hash_word(hash, boundary);
    hash_word(hash, checkpoint.event_digest);
    hash_word(hash, checkpoint.state_digest);
    return hash;
}

bool lan_dme_checkpoint_valid(const LanDmeCheckpoint& checkpoint) {
    return checkpoint.session_id != 0 && checkpoint.epoch != 0 && checkpoint.host < kLanMaxClients &&
           checkpoint.sequence != 0 && checkpoint.digest == lan_dme_checkpoint_digest(checkpoint);
}

LanDmeCommitLog::LanDmeCommitLog(std::uint64_t session_id, std::uint16_t local, std::uint32_t epoch,
                                 std::uint16_t host, LanClock clock, LanDmeCommitConfig config)
    : session_id_(session_id), local_(local), epoch_(epoch), host_(host), clock_(std::move(clock)),
      config_(config) {
    next_sequence_.fill(1);
    if (!session_id_ || local_ >= kLanMaxClients || !epoch_ || host_ >= kLanMaxClients || !clock_ ||
        !config_.max_retained_events || !config_.max_retained_bytes || !config_.max_pending_events ||
        !config_.max_pending_bytes || !config_.receipt_timeout_ms)
        error_ = LanDmeCommitError::InvalidCheckpoint;
}

std::size_t LanDmeCommitLog::retained_events() const {
    std::size_t count = 0;
    for (const auto& events : retained_) count += events.size();
    return count;
}

std::size_t LanDmeCommitLog::pending_events() const {
    std::size_t count = staged_.size();
    for (const auto& events : incoming_) count += events.size();
    return count;
}

std::uint64_t LanDmeCommitLog::committed_through(std::uint16_t origin) const {
    return origin < kLanMaxClients ? applied_through_[origin] : 0;
}

void LanDmeCommitLog::service_timeout() {
    if (error_ != LanDmeCommitError::None) return;
    const auto now = clock_();
    for (const auto& events : retained_) {
        if (!events.empty() && now >= events.front().sent_at &&
            now - events.front().sent_at >= config_.receipt_timeout_ms) {
            error_ = LanDmeCommitError::ReceiptTimeout;
            return;
        }
    }
}

LanDmeRecordResult LanDmeCommitLog::record_host_event(std::span<const std::uint8_t> payload) {
    service_timeout();
    if (error_ != LanDmeCommitError::None) return {error_, std::nullopt};
    if (payload.empty() || payload.size() > config_.max_retained_bytes)
        return {LanDmeCommitError::InvalidEvent, std::nullopt};
    if (retained_events() >= config_.max_retained_events ||
        retained_bytes_ + payload.size() > config_.max_retained_bytes)
        return {LanDmeCommitError::Backpressure, std::nullopt};
    if (next_sequence_[local_] == std::numeric_limits<std::uint64_t>::max()) {
        error_ = LanDmeCommitError::SequenceGap;
        return {error_, std::nullopt};
    }
    LanDmeEvent event;
    event.session_id = session_id_;
    event.created_epoch = epoch_;
    event.origin = local_;
    event.sequence = next_sequence_[local_]++;
    event.payload.assign(payload.begin(), payload.end());
    event.payload_digest = lan_dme_payload_digest(event.payload);
    retained_[local_].push_back({event, clock_()});
    retained_bytes_ += event.payload.size();
    return {LanDmeCommitError::None, event};
}

LanDmeCommitError LanDmeCommitLog::validate_event(const LanDmeEvent& event,
                                                  std::uint32_t delivery_epoch) const {
    if (error_ != LanDmeCommitError::None) return error_;
    if (event.session_id != session_id_) return LanDmeCommitError::WrongSession;
    if (delivery_epoch < epoch_) return LanDmeCommitError::StaleEpoch;
    if (delivery_epoch > epoch_ || event.created_epoch > epoch_) return LanDmeCommitError::FutureEpoch;
    if (event.origin >= kLanMaxClients || event.sequence == std::numeric_limits<std::uint64_t>::max() ||
        (event.origin == host_ && event.created_epoch == delivery_epoch) || !event.sequence || event.payload.empty() ||
        event.payload_digest != lan_dme_payload_digest(event.payload))
        return LanDmeCommitError::InvalidEvent;
    return LanDmeCommitError::None;
}

LanDmeReceiveOutcome LanDmeCommitLog::receive_host_event(const LanDmeEvent& event,
                                                         std::uint32_t delivery_epoch) {
    if (const auto error = validate_event(event, delivery_epoch); error != LanDmeCommitError::None)
        return {error, std::nullopt};
    if (event.sequence <= applied_through_[event.origin])
        return {LanDmeCommitError::None, LanDmeReceiveResult::DuplicateCommitted};
    auto& events = incoming_[event.origin];
    if (events.count(event.sequence))
        return {LanDmeCommitError::None, LanDmeReceiveResult::DuplicatePending};
    if (pending_events() >= config_.max_pending_events ||
        pending_bytes_ + event.payload.size() > config_.max_pending_bytes) {
        error_ = LanDmeCommitError::Backpressure;
        return {error_, std::nullopt};
    }
    pending_bytes_ += event.payload.size();
    events.emplace(event.sequence, event);
    return {LanDmeCommitError::None, LanDmeReceiveResult::Buffered};
}

std::pair<LanDmeCommitError, std::vector<LanDmeEvent>> LanDmeCommitLog::application_batch() {
    if (error_ != LanDmeCommitError::None) return {error_, {}};
    if (batch_open_) return {LanDmeCommitError::BatchOpen, {}};
    staged_.clear();
    for (std::uint16_t origin = 0; origin < kLanMaxClients; ++origin) {
        auto& events = incoming_[origin];
        auto next = applied_through_[origin] + 1;
        for (auto it = events.find(next); it != events.end(); it = events.find(++next)) {
            staged_.push_back(std::move(it->second));
            events.erase(it);
        }
    }
    std::sort(staged_.begin(), staged_.end(), [](const LanDmeEvent& a, const LanDmeEvent& b) {
        return a.sequence != b.sequence ? a.sequence < b.sequence : a.origin < b.origin;
    });
    batch_open_ = true;
    return {LanDmeCommitError::None, staged_};
}

std::uint64_t LanDmeCommitLog::next_event_digest(std::uint64_t digest, const LanDmeEvent& event) const {
    hash_word(digest, event.created_epoch);
    hash_word(digest, event.origin);
    hash_word(digest, event.sequence);
    hash_word(digest, event.payload_digest);
    return digest;
}

LanDmeCommitOutcome LanDmeCommitLog::commit_application(std::uint64_t state_digest) {
    if (error_ != LanDmeCommitError::None) return {error_, std::nullopt, {}};
    if (!batch_open_) return {LanDmeCommitError::NoBatch, std::nullopt, {}};
    std::array<bool, kLanMaxClients> advanced{};
    for (const auto& event : staged_) {
        if (event.sequence != applied_through_[event.origin] + 1) {
            error_ = LanDmeCommitError::SequenceGap;
            return {error_, std::nullopt, {}};
        }
        applied_through_[event.origin] = event.sequence;
        event_digest_ = next_event_digest(event_digest_, event);
        advanced[event.origin] = true;
        pending_bytes_ -= event_bytes(event);
    }
    if (checkpoint_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        error_ = LanDmeCommitError::SequenceGap;
        return {error_, std::nullopt, {}};
    }
    LanDmeCheckpoint checkpoint;
    checkpoint.session_id = session_id_;
    checkpoint.epoch = epoch_;
    checkpoint.host = host_;
    checkpoint.sequence = ++checkpoint_sequence_;
    checkpoint.applied_through = applied_through_;
    checkpoint.event_digest = event_digest_;
    checkpoint.state_digest = state_digest;
    checkpoint.digest = lan_dme_checkpoint_digest(checkpoint);
    remember_checkpoint(checkpoint);

    std::vector<LanDmeReceipt> receipts;
    for (std::uint16_t origin = 0; origin < kLanMaxClients; ++origin)
        if (advanced[origin])
            receipts.push_back({session_id_, epoch_, host_, origin, applied_through_[origin], checkpoint});
    staged_.clear();
    batch_open_ = false;
    migration_ready_ = true;
    return {LanDmeCommitError::None, checkpoint, std::move(receipts)};
}

void LanDmeCommitLog::fail_application() {
    if (error_ == LanDmeCommitError::None) error_ = LanDmeCommitError::ApplicationFailed;
}

void LanDmeCommitLog::remember_checkpoint(const LanDmeCheckpoint& checkpoint) {
    checkpoint_ = checkpoint;
    checkpoint_sequence_ = checkpoint.sequence;
    applied_through_ = checkpoint.applied_through;
    event_digest_ = checkpoint.event_digest;
}

LanDmeCommitError LanDmeCommitLog::accept_receipt(const LanDmeReceipt& receipt) {
    service_timeout();
    if (error_ != LanDmeCommitError::None) return error_;
    if (receipt.session_id != session_id_ || receipt.checkpoint.session_id != session_id_)
        return LanDmeCommitError::WrongSession;
    if (receipt.epoch < epoch_ || receipt.checkpoint.epoch < epoch_) return LanDmeCommitError::StaleEpoch;
    if (receipt.epoch > epoch_ || receipt.checkpoint.epoch > epoch_) return LanDmeCommitError::FutureEpoch;
    if (receipt.host != host_ || receipt.checkpoint.host != host_) return LanDmeCommitError::WrongHost;
    if (receipt.origin != local_) return LanDmeCommitError::WrongOrigin;
    if (!lan_dme_checkpoint_valid(receipt.checkpoint) ||
        receipt.through != receipt.checkpoint.applied_through[local_])
        return LanDmeCommitError::InvalidCheckpoint;
    if (receipt.through >= next_sequence_[local_]) return LanDmeCommitError::SequenceGap;
    if (checkpoint_ && receipt.checkpoint.sequence < checkpoint_->sequence)
        return LanDmeCommitError::StaleEpoch;
    if (checkpoint_ && receipt.checkpoint.sequence == checkpoint_->sequence &&
        receipt.checkpoint.digest != checkpoint_->digest)
        return LanDmeCommitError::InvalidCheckpoint;
    for (std::uint16_t origin = 0; origin < kLanMaxClients; ++origin)
        if (receipt.checkpoint.applied_through[origin] < applied_through_[origin])
            return LanDmeCommitError::InvalidCheckpoint;
    auto& events = retained_[local_];
    while (!events.empty() && events.front().event.sequence <= receipt.through) {
        retained_bytes_ -= events.front().event.payload.size();
        events.pop_front();
    }
    remember_checkpoint(receipt.checkpoint);
    return LanDmeCommitError::None;
}

std::vector<LanDmeEvent> LanDmeCommitLog::replay_suffix() const {
    std::vector<LanDmeEvent> out;
    const auto boundary = applied_through_[local_];
    for (const auto& retained : retained_[local_])
        if (retained.event.sequence > boundary) out.push_back(retained.event);
    return out;
}

LanDmeCommitError LanDmeCommitLog::accept_checkpoint(const LanDmeCheckpoint& checkpoint,
                                                     std::uint64_t local_state_digest) {
    if (error_ != LanDmeCommitError::None) return error_;
    if (!lan_dme_checkpoint_valid(checkpoint)) return LanDmeCommitError::InvalidCheckpoint;
    if (checkpoint.session_id != session_id_) return LanDmeCommitError::WrongSession;
    if (checkpoint.epoch < epoch_) return LanDmeCommitError::StaleEpoch;
    if (checkpoint.epoch > epoch_) return LanDmeCommitError::FutureEpoch;
    if (checkpoint.host != host_) return LanDmeCommitError::WrongHost;
    if (checkpoint.state_digest != local_state_digest) return LanDmeCommitError::IncompatibleState;
    if (checkpoint_ && checkpoint.sequence < checkpoint_->sequence) return LanDmeCommitError::StaleEpoch;
    if (checkpoint_ && checkpoint.sequence == checkpoint_->sequence && checkpoint.digest != checkpoint_->digest)
        return LanDmeCommitError::InvalidCheckpoint;
    for (std::uint16_t origin = 0; origin < kLanMaxClients; ++origin)
        if (checkpoint.applied_through[origin] < applied_through_[origin])
            return LanDmeCommitError::InvalidCheckpoint;
    remember_checkpoint(checkpoint);
    auto& retained = retained_[local_];
    while (!retained.empty() && retained.front().event.sequence <= applied_through_[local_]) {
        retained_bytes_ -= retained.front().event.payload.size();
        retained.pop_front();
    }
    if (applied_through_[local_] < std::numeric_limits<std::uint64_t>::max())
        next_sequence_[local_] = std::max(next_sequence_[local_], applied_through_[local_] + 1);
    migration_ready_ = true;
    return LanDmeCommitError::None;
}

void LanDmeCommitLog::clear_incoming() {
    for (auto& events : incoming_) events.clear();
    staged_.clear();
    pending_bytes_ = 0;
    batch_open_ = false;
}

LanDmeCommitError LanDmeCommitLog::begin_epoch(std::uint32_t next_epoch, std::uint16_t new_host,
                                               const LanDmeCheckpoint& base,
                                               std::uint64_t local_state_digest) {
    if (error_ != LanDmeCommitError::None && error_ != LanDmeCommitError::ReceiptTimeout)
        return error_;
    if (!lan_dme_checkpoint_valid(base)) return LanDmeCommitError::InvalidCheckpoint;
    if (base.session_id != session_id_) return LanDmeCommitError::WrongSession;
    if (base.epoch != epoch_) return base.epoch < epoch_ ? LanDmeCommitError::StaleEpoch
                                                        : LanDmeCommitError::FutureEpoch;
    if (base.host != host_) return LanDmeCommitError::WrongHost;
    if (epoch_ == std::numeric_limits<std::uint32_t>::max() || !next_epoch ||
        next_epoch != epoch_ + 1 || new_host >= kLanMaxClients)
        return LanDmeCommitError::FutureEpoch;
    if (!checkpoint_ || checkpoint_->digest != base.digest) return LanDmeCommitError::NotMigrationReady;
    if (base.state_digest != local_state_digest) return LanDmeCommitError::IncompatibleState;
    epoch_ = next_epoch;
    host_ = new_host;
    error_ = LanDmeCommitError::None;
    clear_incoming();
    checkpoint_.reset();
    checkpoint_sequence_ = 0;
    applied_through_ = base.applied_through;
    event_digest_ = base.event_digest;
    migration_ready_ = true;
    return LanDmeCommitError::None;
}

}  // namespace omega::lan
