#pragma once
// PORT-ONLY: safety protocol groundwork for LAN session-host migration. This does not switch LanTransport's
// live star route: it defines and tests the committed membership, deterministic election, quorum/epoch rules,
// authenticated endpoint updates, and logical reliable-event replay boundary required before that switch is safe.

#include "lan_transport.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace omega::lan {

constexpr std::uint8_t lan_member_bit(std::uint16_t client) {
    return client < kLanMaxClients ? static_cast<std::uint8_t>(1u << client) : 0;
}

struct LanMigrationMember {
    std::uint16_t client = 0;
    std::uint32_t join_order = 0;
    LanAddress endpoint;
    std::uint64_t capability = 0;
    friend bool operator==(const LanMigrationMember&, const LanMigrationMember&) = default;
};

struct LanMigrationView {
    std::uint64_t session_id = 0;
    std::uint32_t epoch = 1;
    std::uint32_t generation = 1;
    std::uint16_t host = 0;
    std::uint8_t active_mask = 0;
    std::array<std::optional<LanMigrationMember>, kLanMaxClients> members;

    bool valid() const;
    std::uint32_t quorum() const;
    std::uint64_t digest() const;
    std::optional<std::uint16_t> successor(std::uint8_t live_mask) const;
};

struct LanMigrationAuth {
    std::uint64_t session_id = 0;
    std::uint32_t epoch = 0;
    std::uint16_t sender = 0;
    std::uint64_t capability = 0;
    LanAddress source;
};

enum class LanMigrationError {
    None,
    InvalidView,
    WrongSession,
    StaleEpoch,
    FutureEpoch,
    UnknownMember,
    BadCapability,
    BadEndpoint,
    NotHost,
    NoQuorum,
    HostStillReachable,
    WrongCandidate,
    MembershipMismatch,
    ConflictingVote,
    IncompleteVotes,
    PendingJoinChanged,
    SequenceGap,
    Duplicate,
};

struct LanMigrationProposal {
    LanMigrationAuth auth;
    std::uint32_t next_epoch = 0;
    std::uint32_t generation = 0;
    std::uint64_t view_digest = 0;
    std::uint16_t candidate = 0;
    std::uint8_t live_mask = 0;
};

struct LanMigrationVote {
    LanMigrationAuth auth;
    std::uint32_t next_epoch = 0;
    std::uint32_t generation = 0;
    std::uint64_t view_digest = 0;
    std::uint16_t candidate = 0;
    std::uint8_t live_mask = 0;
};

struct LanMigrationCommit {
    LanMigrationProposal proposal;
    std::array<std::optional<LanMigrationVote>, kLanMaxClients> votes;
};

class LanMigrationPeer {
public:
    LanMigrationPeer(LanMigrationView view, std::uint16_t local);

    const LanMigrationView& view() const { return view_; }
    std::uint16_t local() const { return local_; }
    void set_reachable(std::uint8_t mask);
    std::uint8_t reachable() const { return reachable_mask_; }
    bool has_quorum() const;
    bool can_serve() const;

    LanMigrationError authenticate(const LanMigrationAuth& auth) const;
    std::optional<LanMigrationProposal> propose_host_loss() const;
    std::pair<LanMigrationError, std::optional<LanMigrationVote>> vote(const LanMigrationProposal& proposal);
    LanMigrationError apply(const LanMigrationCommit& commit);

private:
    LanMigrationError validate_proposal(const LanMigrationProposal& proposal) const;
    LanMigrationError validate_vote(const LanMigrationProposal& proposal, const LanMigrationVote& vote) const;

    LanMigrationView view_;
    std::uint16_t local_ = 0;
    std::uint8_t reachable_mask_ = 0;
    std::uint32_t voted_epoch_ = 0;
    std::uint16_t voted_candidate_ = 0;
    std::uint64_t voted_digest_ = 0;
};

// A join is not part of election/quorum membership until every committed peer and the joiner acknowledged the
// host-authenticated descriptor. If the epoch/view changes first, commit() fails closed.
class LanPendingJoin {
public:
    static std::pair<LanMigrationError, std::optional<LanPendingJoin>> begin(
        const LanMigrationView& view, const LanMigrationAuth& host_auth, const LanMigrationMember& member);

    LanMigrationError acknowledge(const LanMigrationView& view, const LanMigrationAuth& auth);
    LanMigrationError acknowledge_joiner(std::uint64_t session_id, std::uint32_t epoch, std::uint64_t capability,
                                         LanAddress source);
    bool ready(const LanMigrationView& view) const;
    std::pair<LanMigrationError, std::optional<LanMigrationView>> commit(const LanMigrationView& view) const;

private:
    LanMigrationMember member_;
    std::uint64_t base_session_id_ = 0;
    std::uint32_t base_epoch_ = 0;
    std::uint32_t base_generation_ = 0;
    std::uint64_t base_digest_ = 0;
    std::uint8_t acknowledgements_ = 0;
};

struct LanReconnectRequest {
    std::uint64_t session_id = 0;
    std::uint32_t known_epoch = 0;
    std::uint16_t client = 0;
    std::uint64_t capability = 0;
    LanAddress previous_endpoint;
    LanAddress new_endpoint;
};

// Validation only. Admission still needs a quorum membership change; returning None is not a success response.
LanMigrationError lan_validate_reconnect(const LanMigrationPeer& host, const LanReconnectRequest& request);

struct LanReliableEvent {
    std::uint64_t session_id = 0;
    std::uint32_t created_epoch = 0;
    std::uint16_t origin = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> bytes;
    friend bool operator==(const LanReliableEvent&, const LanReliableEvent&) = default;
};

enum class LanReplayResult { Applied, Duplicate, WrongSession, FutureEvent, SequenceGap };

// Logical reliable ids survive a transport-stream reset. Senders retain host-recipient events until an
// application-level quorum commit boundary says they may be retired. Receivers suppress a replay by origin/id.
class LanReplayLedger {
public:
    LanReplayLedger(std::uint64_t session_id, std::uint32_t epoch);

    std::pair<LanMigrationError, std::optional<LanReliableEvent>> record(
        std::uint16_t origin, std::span<const std::uint8_t> bytes);
    LanMigrationError mark_committed(std::uint16_t origin, std::uint64_t through);
    std::vector<LanReliableEvent> retained(std::uint16_t origin) const;

    bool advance_epoch(std::uint32_t epoch);
    LanReplayResult apply(const LanReliableEvent& event, std::uint32_t delivery_epoch);
    std::uint64_t applied_through(std::uint16_t origin) const;
    std::uint64_t committed_through(std::uint16_t origin) const;

private:
    std::uint64_t session_id_ = 0;
    std::uint32_t epoch_ = 0;
    std::array<std::uint64_t, kLanMaxClients> next_{};
    std::array<std::uint64_t, kLanMaxClients> committed_{};
    std::array<std::uint64_t, kLanMaxClients> applied_{};
    std::array<std::deque<LanReliableEvent>, kLanMaxClients> retained_;
};

}  // namespace omega::lan
