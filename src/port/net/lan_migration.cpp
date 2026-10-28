#include "lan_migration.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace omega::lan {
namespace {

bool valid_endpoint(const LanAddress& endpoint) {
    return endpoint.ip != 0 && endpoint.ip != kLanBroadcastIp && (endpoint.ip & 0xffu) != 0xffu && endpoint.port != 0;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
}

template <typename T>
void hash_word(std::uint64_t& hash, T value) {
    for (std::size_t i = 0; i < sizeof(T); ++i) hash_byte(hash, static_cast<std::uint8_t>(value >> (i * 8)));
}

LanMigrationAuth auth_for(const LanMigrationView& view, std::uint16_t client) {
    const auto& member = *view.members[client];
    return {view.session_id, view.epoch, client, member.capability, member.endpoint};
}

bool proposal_matches(const LanMigrationProposal& proposal, const LanMigrationVote& vote) {
    return vote.next_epoch == proposal.next_epoch && vote.generation == proposal.generation &&
           vote.view_digest == proposal.view_digest && vote.candidate == proposal.candidate &&
           vote.live_mask == proposal.live_mask;
}

}  // namespace

bool LanMigrationView::valid() const {
    if (!session_id || !epoch || !generation || host >= kLanMaxClients || !(active_mask & lan_member_bit(host)))
        return false;
    std::array<std::uint32_t, kLanMaxClients> orders{};
    std::size_t order_count = 0;
    for (std::uint16_t client = 0; client < kLanMaxClients; ++client) {
        const bool active = (active_mask & lan_member_bit(client)) != 0;
        if (!members[client]) {
            if (active) return false;
            continue;
        }
        const auto& member = *members[client];
        if (member.client != client || !member.join_order || !member.capability || !valid_endpoint(member.endpoint))
            return false;
        if (active) orders[order_count++] = member.join_order;
    }
    std::sort(orders.begin(), orders.begin() + static_cast<std::ptrdiff_t>(order_count));
    return std::adjacent_find(orders.begin(), orders.begin() + static_cast<std::ptrdiff_t>(order_count)) ==
           orders.begin() + static_cast<std::ptrdiff_t>(order_count);
}

std::uint32_t LanMigrationView::quorum() const {
    return static_cast<std::uint32_t>(std::popcount(active_mask)) / 2 + 1;
}

std::uint64_t LanMigrationView::digest() const {
    std::uint64_t hash = 1469598103934665603ull;
    hash_word(hash, session_id);
    hash_word(hash, epoch);
    hash_word(hash, generation);
    hash_word(hash, host);
    hash_word(hash, active_mask);
    for (std::uint16_t client = 0; client < kLanMaxClients; ++client) {
        hash_byte(hash, members[client] ? 1 : 0);
        if (!members[client]) continue;
        const auto& member = *members[client];
        hash_word(hash, member.client);
        hash_word(hash, member.join_order);
        hash_word(hash, member.endpoint.ip);
        hash_word(hash, member.endpoint.port);
        hash_word(hash, member.capability);
    }
    return hash;
}

std::optional<std::uint16_t> LanMigrationView::successor(std::uint8_t live_mask) const {
    live_mask = static_cast<std::uint8_t>(live_mask & active_mask & ~lan_member_bit(host));
    std::optional<std::uint16_t> best;
    for (std::uint16_t client = 0; client < kLanMaxClients; ++client) {
        if (!(live_mask & lan_member_bit(client)) || !members[client]) continue;
        if (!best || members[client]->join_order < members[*best]->join_order ||
            (members[client]->join_order == members[*best]->join_order && client < *best))
            best = client;
    }
    return best;
}

LanMigrationPeer::LanMigrationPeer(LanMigrationView view, std::uint16_t local)
    : view_(std::move(view)), local_(local) {
    if (local_ < kLanMaxClients && (view_.active_mask & lan_member_bit(local_))) reachable_mask_ = lan_member_bit(local_);
}

void LanMigrationPeer::set_reachable(std::uint8_t mask) {
    reachable_mask_ = static_cast<std::uint8_t>(mask & view_.active_mask);
    if (local_ < kLanMaxClients && (view_.active_mask & lan_member_bit(local_)))
        reachable_mask_ = static_cast<std::uint8_t>(reachable_mask_ | lan_member_bit(local_));
}

bool LanMigrationPeer::has_quorum() const {
    return view_.valid() &&
           static_cast<std::uint32_t>(std::popcount(static_cast<unsigned>(reachable_mask_ & view_.active_mask))) >=
               view_.quorum();
}

bool LanMigrationPeer::can_serve() const { return local_ == view_.host && has_quorum(); }

LanMigrationError LanMigrationPeer::authenticate(const LanMigrationAuth& auth) const {
    if (!view_.valid()) return LanMigrationError::InvalidView;
    if (auth.session_id != view_.session_id) return LanMigrationError::WrongSession;
    if (auth.epoch < view_.epoch) return LanMigrationError::StaleEpoch;
    if (auth.epoch > view_.epoch) return LanMigrationError::FutureEpoch;
    if (auth.sender >= kLanMaxClients || !(view_.active_mask & lan_member_bit(auth.sender)) ||
        !view_.members[auth.sender])
        return LanMigrationError::UnknownMember;
    const auto& member = *view_.members[auth.sender];
    if (auth.capability != member.capability) return LanMigrationError::BadCapability;
    if (auth.source != member.endpoint) return LanMigrationError::BadEndpoint;
    return LanMigrationError::None;
}

std::optional<LanMigrationProposal> LanMigrationPeer::propose_host_loss() const {
    if (!view_.valid() || local_ >= kLanMaxClients || (reachable_mask_ & lan_member_bit(view_.host)) || !has_quorum())
        return std::nullopt;
    const auto candidate = view_.successor(reachable_mask_);
    if (!candidate || *candidate != local_ || view_.epoch == std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    LanMigrationProposal proposal;
    proposal.auth = auth_for(view_, local_);
    proposal.next_epoch = view_.epoch + 1;
    proposal.generation = view_.generation;
    proposal.view_digest = view_.digest();
    proposal.candidate = local_;
    proposal.live_mask = reachable_mask_;
    return proposal;
}

LanMigrationError LanMigrationPeer::validate_proposal(const LanMigrationProposal& proposal) const {
    if (const auto error = authenticate(proposal.auth); error != LanMigrationError::None) return error;
    if (reachable_mask_ & lan_member_bit(view_.host)) return LanMigrationError::HostStillReachable;
    if (proposal.next_epoch != view_.epoch + 1)
        return proposal.next_epoch <= view_.epoch ? LanMigrationError::StaleEpoch : LanMigrationError::FutureEpoch;
    if (proposal.generation != view_.generation || proposal.view_digest != view_.digest())
        return LanMigrationError::MembershipMismatch;
    if ((proposal.live_mask & ~view_.active_mask) || (proposal.live_mask & lan_member_bit(view_.host)) ||
        !(proposal.live_mask & lan_member_bit(local_)) || !(proposal.live_mask & lan_member_bit(proposal.candidate)) ||
        (proposal.live_mask & ~reachable_mask_))
        return LanMigrationError::MembershipMismatch;
    if (static_cast<std::uint32_t>(std::popcount(static_cast<unsigned>(proposal.live_mask))) < view_.quorum())
        return LanMigrationError::NoQuorum;
    const auto candidate = view_.successor(proposal.live_mask);
    if (!candidate || *candidate != proposal.candidate || proposal.auth.sender != proposal.candidate)
        return LanMigrationError::WrongCandidate;
    return LanMigrationError::None;
}

std::pair<LanMigrationError, std::optional<LanMigrationVote>> LanMigrationPeer::vote(
    const LanMigrationProposal& proposal) {
    if (const auto error = validate_proposal(proposal); error != LanMigrationError::None) return {error, std::nullopt};
    if (voted_epoch_ == proposal.next_epoch &&
        (voted_candidate_ != proposal.candidate || voted_digest_ != proposal.view_digest))
        return {LanMigrationError::ConflictingVote, std::nullopt};
    voted_epoch_ = proposal.next_epoch;
    voted_candidate_ = proposal.candidate;
    voted_digest_ = proposal.view_digest;
    LanMigrationVote vote;
    vote.auth = auth_for(view_, local_);
    vote.next_epoch = proposal.next_epoch;
    vote.generation = proposal.generation;
    vote.view_digest = proposal.view_digest;
    vote.candidate = proposal.candidate;
    vote.live_mask = proposal.live_mask;
    return {LanMigrationError::None, vote};
}

LanMigrationError LanMigrationPeer::validate_vote(const LanMigrationProposal& proposal,
                                                   const LanMigrationVote& vote) const {
    if (const auto error = authenticate(vote.auth); error != LanMigrationError::None) return error;
    if (!proposal_matches(proposal, vote) || !(proposal.live_mask & lan_member_bit(vote.auth.sender)))
        return LanMigrationError::MembershipMismatch;
    return LanMigrationError::None;
}

LanMigrationError LanMigrationPeer::apply(const LanMigrationCommit& commit) {
    if (const auto error = validate_proposal(commit.proposal); error != LanMigrationError::None) return error;
    std::uint8_t voters = 0;
    for (const auto& vote : commit.votes) {
        if (!vote) continue;
        if (const auto error = validate_vote(commit.proposal, *vote); error != LanMigrationError::None) return error;
        voters = static_cast<std::uint8_t>(voters | lan_member_bit(vote->auth.sender));
    }
    if (!(voters & lan_member_bit(commit.proposal.candidate)) ||
        static_cast<std::uint32_t>(std::popcount(static_cast<unsigned>(voters))) < view_.quorum())
        return LanMigrationError::IncompleteVotes;
    view_.epoch = commit.proposal.next_epoch;
    view_.generation += 1;
    view_.host = commit.proposal.candidate;
    view_.active_mask = commit.proposal.live_mask;
    reachable_mask_ = static_cast<std::uint8_t>(reachable_mask_ & view_.active_mask);
    if (view_.active_mask & lan_member_bit(local_))
        reachable_mask_ = static_cast<std::uint8_t>(reachable_mask_ | lan_member_bit(local_));
    voted_epoch_ = 0;
    voted_candidate_ = 0;
    voted_digest_ = 0;
    return LanMigrationError::None;
}

std::pair<LanMigrationError, std::optional<LanPendingJoin>> LanPendingJoin::begin(
    const LanMigrationView& view, const LanMigrationAuth& host_auth, const LanMigrationMember& member) {
    LanMigrationPeer host(view, view.host);
    host.set_reachable(view.active_mask);
    if (!view.valid()) return {LanMigrationError::InvalidView, std::nullopt};
    if (const auto error = host.authenticate(host_auth); error != LanMigrationError::None) return {error, std::nullopt};
    if (host_auth.sender != view.host) return {LanMigrationError::NotHost, std::nullopt};
    if (member.client >= kLanMaxClients || view.members[member.client] ||
        (view.active_mask & lan_member_bit(member.client)))
        return {LanMigrationError::UnknownMember, std::nullopt};
    if (!member.join_order || !member.capability || !valid_endpoint(member.endpoint))
        return {LanMigrationError::BadEndpoint, std::nullopt};
    LanPendingJoin pending;
    pending.member_ = member;
    pending.base_session_id_ = view.session_id;
    pending.base_epoch_ = view.epoch;
    pending.base_generation_ = view.generation;
    pending.base_digest_ = view.digest();
    pending.acknowledgements_ = lan_member_bit(view.host);
    return {LanMigrationError::None, pending};
}

LanMigrationError LanPendingJoin::acknowledge(const LanMigrationView& view, const LanMigrationAuth& auth) {
    if (view.epoch != base_epoch_ || view.generation != base_generation_ || view.digest() != base_digest_)
        return LanMigrationError::PendingJoinChanged;
    LanMigrationPeer peer(view, auth.sender);
    if (const auto error = peer.authenticate(auth); error != LanMigrationError::None) return error;
    acknowledgements_ = static_cast<std::uint8_t>(acknowledgements_ | lan_member_bit(auth.sender));
    return LanMigrationError::None;
}

LanMigrationError LanPendingJoin::acknowledge_joiner(std::uint64_t session_id, std::uint32_t epoch,
                                                     std::uint64_t capability, LanAddress source) {
    if (session_id != base_session_id_) return LanMigrationError::WrongSession;
    if (epoch < base_epoch_) return LanMigrationError::StaleEpoch;
    if (epoch > base_epoch_) return LanMigrationError::FutureEpoch;
    if (capability != member_.capability) return LanMigrationError::BadCapability;
    if (source != member_.endpoint) return LanMigrationError::BadEndpoint;
    acknowledgements_ = static_cast<std::uint8_t>(acknowledgements_ | lan_member_bit(member_.client));
    return LanMigrationError::None;
}

bool LanPendingJoin::ready(const LanMigrationView& view) const {
    if (view.epoch != base_epoch_ || view.generation != base_generation_ || view.digest() != base_digest_) return false;
    const std::uint8_t required = static_cast<std::uint8_t>(view.active_mask | lan_member_bit(member_.client));
    return (acknowledgements_ & required) == required;
}

std::pair<LanMigrationError, std::optional<LanMigrationView>> LanPendingJoin::commit(
    const LanMigrationView& view) const {
    if (view.epoch != base_epoch_ || view.generation != base_generation_ || view.digest() != base_digest_)
        return {LanMigrationError::PendingJoinChanged, std::nullopt};
    if (!ready(view)) return {LanMigrationError::IncompleteVotes, std::nullopt};
    LanMigrationView next = view;
    next.members[member_.client] = member_;
    next.active_mask = static_cast<std::uint8_t>(next.active_mask | lan_member_bit(member_.client));
    ++next.generation;
    return next.valid() ? std::pair{LanMigrationError::None, std::optional<LanMigrationView>{std::move(next)}}
                        : std::pair{LanMigrationError::InvalidView, std::optional<LanMigrationView>{}};
}

LanMigrationError lan_validate_reconnect(const LanMigrationPeer& host, const LanReconnectRequest& request) {
    const auto& view = host.view();
    if (!view.valid()) return LanMigrationError::InvalidView;
    if (host.local() != view.host) return LanMigrationError::NotHost;
    if (!host.can_serve()) return LanMigrationError::NoQuorum;
    if (request.session_id != view.session_id) return LanMigrationError::WrongSession;
    if (request.known_epoch > view.epoch) return LanMigrationError::FutureEpoch;
    if (request.client >= kLanMaxClients || !view.members[request.client]) return LanMigrationError::UnknownMember;
    const auto& member = *view.members[request.client];
    if (request.capability != member.capability) return LanMigrationError::BadCapability;
    if (request.previous_endpoint != member.endpoint || !valid_endpoint(request.new_endpoint))
        return LanMigrationError::BadEndpoint;
    return LanMigrationError::None;
}

LanReplayLedger::LanReplayLedger(std::uint64_t session_id, std::uint32_t epoch)
    : session_id_(session_id), epoch_(epoch) {
    next_.fill(1);
}

std::pair<LanMigrationError, std::optional<LanReliableEvent>> LanReplayLedger::record(
    std::uint16_t origin, std::span<const std::uint8_t> bytes) {
    if (!session_id_ || !epoch_ || origin >= kLanMaxClients) return {LanMigrationError::InvalidView, std::nullopt};
    LanReliableEvent event{session_id_, epoch_, origin, next_[origin]++,
                           std::vector<std::uint8_t>(bytes.begin(), bytes.end())};
    retained_[origin].push_back(event);
    return {LanMigrationError::None, event};
}

LanMigrationError LanReplayLedger::mark_committed(std::uint16_t origin, std::uint64_t through) {
    if (origin >= kLanMaxClients || through >= next_[origin]) return LanMigrationError::SequenceGap;
    if (through < committed_[origin]) return LanMigrationError::StaleEpoch;
    committed_[origin] = through;
    auto& events = retained_[origin];
    while (!events.empty() && events.front().sequence <= through) events.pop_front();
    return LanMigrationError::None;
}

std::vector<LanReliableEvent> LanReplayLedger::retained(std::uint16_t origin) const {
    if (origin >= kLanMaxClients) return {};
    return {retained_[origin].begin(), retained_[origin].end()};
}

bool LanReplayLedger::advance_epoch(std::uint32_t epoch) {
    if (epoch != epoch_ + 1) return false;
    epoch_ = epoch;
    return true;
}

LanReplayResult LanReplayLedger::apply(const LanReliableEvent& event, std::uint32_t delivery_epoch) {
    if (event.session_id != session_id_) return LanReplayResult::WrongSession;
    if (delivery_epoch != epoch_ || event.created_epoch > epoch_) return LanReplayResult::FutureEvent;
    if (event.origin >= kLanMaxClients || !event.sequence) return LanReplayResult::SequenceGap;
    if (event.sequence <= applied_[event.origin]) return LanReplayResult::Duplicate;
    if (event.sequence != applied_[event.origin] + 1) return LanReplayResult::SequenceGap;
    applied_[event.origin] = event.sequence;
    return LanReplayResult::Applied;
}

std::uint64_t LanReplayLedger::applied_through(std::uint16_t origin) const {
    return origin < kLanMaxClients ? applied_[origin] : 0;
}

std::uint64_t LanReplayLedger::committed_through(std::uint16_t origin) const {
    return origin < kLanMaxClients ? committed_[origin] : 0;
}

}  // namespace omega::lan
