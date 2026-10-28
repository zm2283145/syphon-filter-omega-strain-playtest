#include "lan_dme.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega::lan {
namespace {
constexpr std::uint8_t kChannelControl = 0x01, kChannelGame = 0x02, kChannelBackpack = 0x03;
constexpr std::uint8_t kStartReady = 1, kStartGo = 2, kClientInfo = 3, kClientRecord = 4, kJoinOrder = 5;
// The DME message registry (003057f8 / 00305998, src/main/DME/dme_library.cpp).
constexpr std::uint32_t kRegistryUp = 0x4912e8, kRegCallbacks = 0x4912f0, kRegCounts = 0x4912f4, kRegBuffer = 0x4912ec;
constexpr std::uint32_t kRegSpare = 0x4912f8;
constexpr std::uint32_t kRegMax = 0x529808, kRegCategories = 0x52980c, kRegBufferSize = 0x529810;

[[noreturn]] void unhosted(std::uint32_t address, const char* what) {
    char text[64];
    std::snprintf(text, sizeof text, "unhosted(0x%08x): ", address);
    throw std::runtime_error(std::string(text) + what + " (the DME API in LAN mode, LanDme)");
}
}  // namespace

LanDme::LanDme(LanTransport& transport, LanClock clock, Invoke invoke, Library library, std::uint32_t scratch)
    : transport_(transport), clock_(clock),
      application_commits_(transport.session_id(), transport.local_index(), 1, 0, std::move(clock)),
      invoke_(std::move(invoke)), library_(std::move(library)), scratch_(scratch) {}

std::uint32_t LanDme::local_clock() const { return static_cast<std::uint32_t>(clock_()); }

std::uint32_t LanDme::session_clock() const {
    const auto local = static_cast<std::int64_t>(clock_());
    return static_cast<std::uint32_t>(transport_.is_host() ? local : local + transport_.host_clock_offset_ms());
}

// The API gate every DME entry opens with: 0x11 before the library init (0x0048c4d7), 0x2b while it is not
// running (0x0048c4dc).
static std::uint32_t library_gate(const LevelLoadMemory& m) {
    if (m.read(0x48c4d7, 1) != 1) return 0x11;
    if (m.read(0x48c4dc, 1) != 1) return 0x2b;
    return 0;
}

std::vector<std::uint16_t> LanDme::connected() const {
    auto out = transport_.clients();
    out.push_back(transport_.local_index());
    std::sort(out.begin(), out.end());
    return out;
}

// START_READY's words are u16, START_GO's u32 / u16 / u16 (the header).
void LanDme::send_control(std::uint16_t to, std::uint8_t kind, std::initializer_list<std::uint32_t> words) {
    std::vector<std::uint8_t> frame{kChannelControl, kind, static_cast<std::uint8_t>(transport_.local_index())};
    std::size_t k = 0;
    for (const auto w : words) {
        const int size = (kind == kStartGo && k == 0) ? 4 : 2;
        for (int b = 0; b < size; ++b) frame.push_back(static_cast<std::uint8_t>(w >> (8 * b)));
        ++k;
    }
    if (transport_.send(to, frame, kLanReliableOrdered) != LanSendResult::Sent)
        throw std::runtime_error("LanDme: a control frame could not be sent to client " + std::to_string(to));
}

void LanDme::send_backpack(std::uint8_t type, std::uint8_t owner, std::uint32_t backpack, std::uint32_t target,
                           std::uint16_t recipient) {
    std::vector<std::uint8_t> frame{kChannelBackpack, static_cast<std::uint8_t>(transport_.local_index()), type, owner};
    for (const auto word : {backpack, target})
        for (int b = 0; b < 4; ++b) frame.push_back(static_cast<std::uint8_t>(word >> (8 * b)));
    if (transport_.send(recipient, frame, kLanReliableOrdered) != LanSendResult::Sent)
        throw std::runtime_error("LanDme: a backpack frame could not be sent");
}

std::vector<LanDme::BackpackMessage> LanDme::take_backpacks() {
    std::vector<BackpackMessage> out;
    out.swap(backpacks_);
    return out;
}

std::uint32_t LanDme::record(LevelLoadMemory& m, std::uint16_t client) {
    const auto out = scratch_ + 0x44;
    if (library_(m, 0x2db188, {out, 0, client})) unhosted(0x2db188, "the client record table is missing");
    return m.read(out);
}

// 002e15e0's world and record steps (the header); PORT-ONLY: world 0 itself (the lobby's join is not ported).
void LanDme::start_records(LevelLoadMemory& m, std::uint32_t p) {
    const auto me = transport_.local_index();
    const auto out = scratch_ + 0x40;
    if (library_(m, 0x2e51f0, {0, out}) || !m.read(out)) unhosted(0x2e51f0, "world 0 could not be created");
    const auto w = m.read(out);
    m.write(w + 0x400, 4, me);
    // 132fx5: +0x404 is the world's session host (002e5550; 002ddb30 reads it, 002e8c50 compares it with +0x400).
    // The world join sets it to 0x100 (002e0210 / 002e0760: 002e5550(world, 0x100)); the host's start sets it to
    // itself (002e8730, or 002e15e0 alone; set_session_host below); a joiner keeps 0x100.
    m.write(w + 0x404, 4, 0x100);
    m.write(w + 0x438, 4, 1);
    m.write(w + 0x444, 4, kLanMaxClients);
    m.write(0x58df88, 4, m.read(p + 0x20));
    m.write(0x48c478, 4, m.read(p + 0x24));
    m.write(0x58df8c, 4, m.read(p + 0x28));
    m.write(0x48c47c, 4, m.read(p + 0x2c));
    m.write(0x48c488, 4, m.read(p + 0x30));
    m.write(0x48c464, 4, 0);
    m.write(0x48c48c, 4, m.read(p + 0x34));
    m.write(0x48c450, 4, m.read(p + 4));
    m.write(0x48c45c, 4, 0);
    m.write(0x58df90, 4, m.read(p + 0x1c));
    const auto rec = library_(m, 0x2db270, {m.read(p + 4), me});
    if (!rec) unhosted(0x2db270, "the local client record was not created");
    m.write(rec + 8, 2, m.read(p + 0x14) & 0xffff);
    m.write(rec + 6, 2, 0x1000);
    m.write(rec + 0xa, 2, 0x800);
    m.write(rec + 0xc, 2, m.read(p + 0x18) & 0xffff);
    library_(m, 0x129a70, {rec + 0xe, p + 8, 0xb});
    m.write(rec + 0x19, 1, 0);
}

// 002db720's steps before the callback (132fx5: the record steps added): the joined client's record (002db188;
// 002db270 where there is none, such as the host's on a joiner), its +0 = the session clock word 0x0058806c, the
// client's info +0x29 = 1, newly joined (002e4550 announces the local client's objects to it, then clears the
// mark), and 002db470(record). PORT-ONLY: 002e5360 makes the info where 002db720's 002e52d8 finds the world join's
// member (002e0118's member list, not ported). Not done: 002db550 after the callback (the local record to the
// joined client, message *0x00598410, received by 002e6c30 -> 002db8e8).
void LanDme::newly_joined(LevelLoadMemory& m, std::uint16_t client) {
    const auto world = m.read(0x48c450);
    if (world >= 4) return;
    auto rec = record(m, client);
    if (!rec) rec = library_(m, 0x2db270, {world, client});
    if (!rec) unhosted(0x2db720, "the joined client's record could not be created");
    const auto out = scratch_ + 0x4c;
    if (library_(m, 0x2e5360, {world, client, out}) || !m.read(out)) return;
    m.write(rec, 4, m.read(0x58806c));
    m.write(m.read(out) + 0x29, 1, 1);
    library_(m, 0x2db470, {rec});
}

// 002e5550(world, client): world +0x404, the session host (002e8730 and 002e15e0 alone: the local client).
void LanDme::set_session_host(LevelLoadMemory& m, std::uint32_t client) {
    const auto out = scratch_ + 0x58;
    const auto world = m.read(0x48c450);
    if (world >= 4 || library_(m, 0x2e51b8, {world, out})) return;
    if (const auto w = m.read(out)) m.write(w + 0x404, 4, client);
}

// The local client's start (002e1918 alone, 002e8730 the host, 002e6f40 a client): its handle bases, 0x0048c454,
// then 002db688's record steps.
void LanDme::activate(LevelLoadMemory& m, std::uint32_t base, std::uint32_t base2) {
    const auto rec = record(m, transport_.local_index());
    if (!rec) unhosted(0x2db688, "the local client record is missing at the start");
    m.write(rec + 0xa, 2, base2);
    m.write(rec + 6, 2, base);
    m.write(0x48c454, 1, 1);
    m.write(rec + 4, 2, 0x1000);
    library_(m, 0x2db470, {rec});
}

// 002e8c50 on the host: the client's record and handle bases, then START_GO.
void LanDme::start_go(LevelLoadMemory& m, std::uint16_t client) {
    if (record(m, client)) return;  // 002e8c50 returns when the host has the record already
    const auto rec = library_(m, 0x2db270, {0, client});
    if (!rec) return;
    if (counts_[client]) m.write(rec + 6, 2, library_(m, 0x2e68c0, {counts_[client]}) & 0xffff);
    if (counts2_[client]) m.write(rec + 0xa, 2, library_(m, 0x2e69b8, {counts2_[client]}) & 0xffff);
    const auto order = next_join_order(m);
    if (const auto p = member(m, client)) m.write(p + 0x24, 4, order);
    send_control(client, kStartGo, {m.read(0x58806c), m.read(rec + 6, 2), m.read(rec + 0xa, 2)});
    for (std::uint16_t c = 0; c < kLanMaxClients; ++c)
        if (const auto p = member(m, c); p && m.read(p + 0x24)) send_join_order(m, client, c, m.read(p + 0x24));
    send_join_order(m, kLanAll, client, order);
}

std::uint32_t LanDme::member(LevelLoadMemory& m, std::uint16_t client) {
    const auto out = scratch_ + 0x5c;
    if (library_(m, 0x2e5360, {m.read(0x48c450), client, out})) return 0;
    return m.read(out);
}

std::uint32_t LanDme::next_join_order(LevelLoadMemory& m) {
    std::uint32_t order = 0;
    for (std::uint16_t c = 0; c < 0x100; ++c)
        if (const auto p = member(m, c)) order = std::max(order, m.read(p + 0x24));
    ++order;
    if (!order || order == 0x0fffffffU) order = 1;
    return order;
}

void LanDme::send_join_order(LevelLoadMemory&, std::uint16_t to, std::uint16_t client, std::uint32_t order) {
    std::vector<std::uint8_t> frame{kChannelControl, kJoinOrder, static_cast<std::uint8_t>(transport_.local_index()),
                                    static_cast<std::uint8_t>(client), static_cast<std::uint8_t>(client >> 8)};
    for (int b = 0; b < 4; ++b) frame.push_back(static_cast<std::uint8_t>(order >> (8 * b)));
    if (transport_.send(to, frame, kLanReliableOrdered) != LanSendResult::Sent)
        throw std::runtime_error("LanDme: a join-order frame could not be sent");
}

void LanDme::receive_join_order(LevelLoadMemory& m, const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 9) throw std::runtime_error("LanDme: a malformed join-order frame");
    const auto client = static_cast<std::uint16_t>(bytes[3] | bytes[4] << 8);
    const auto order = static_cast<std::uint32_t>(bytes[5] | bytes[6] << 8 | bytes[7] << 16 | bytes[8] << 24);
    if (client >= 0x100 || !order) throw std::runtime_error("LanDme: an invalid join-order frame");
    if (const auto p = member(m, client)) m.write(p + 0x24, 4, order);
}

void LanDme::send_client_record(LevelLoadMemory& m, std::uint16_t to) {
    const auto me = transport_.local_index();
    const auto rec = record(m, me);
    if (!rec) return;
    std::vector<std::uint8_t> frame(3 + 0x24);
    frame[0] = kChannelControl;
    frame[1] = kClientRecord;
    frame[2] = static_cast<std::uint8_t>(me);
    frame[3] = static_cast<std::uint8_t>(me);
    for (std::uint32_t k = 0; k < 5; ++k) {
        const auto v = m.read(rec + 4 + k * 2, 2);
        frame[3 + 8 + k * 2] = static_cast<std::uint8_t>(v);
        frame[3 + 9 + k * 2] = static_cast<std::uint8_t>(v >> 8);
    }
    // 002db550 sends 0x24 bytes although the stack declaration's trailing name array is larger; only these
    // fourteen bytes are on the wire (the source record name was strncpy-capped to eleven).
    for (std::uint32_t k = 0; k < 0x0e; ++k) frame[3 + 0x16 + k] = static_cast<std::uint8_t>(m.read(rec + 0xe + k, 1));
    if (transport_.send(to, frame, kLanReliableOrdered) != LanSendResult::Sent)
        throw std::runtime_error("LanDme: a client-record frame could not be sent");
}

void LanDme::receive_client_record(LevelLoadMemory& m, std::uint16_t origin, const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 3 + 0x24 || bytes[3] != origin)
        throw std::runtime_error("LanDme: a malformed client-record frame from client " + std::to_string(origin));
    auto rec = record(m, origin);
    if (!rec) rec = library_(m, 0x2db270, {m.read(0x48c450), origin});
    if (!rec) unhosted(0x2e6c78, "the client record could not be created");
    for (std::uint32_t k = 0; k < 5; ++k)
        m.write(rec + 4 + k * 2, 2, static_cast<std::uint32_t>(bytes[3 + 8 + k * 2] | bytes[3 + 9 + k * 2] << 8));
    for (std::uint32_t k = 0; k < 0x0e; ++k) m.write(rec + 0xe + k, 1, bytes[3 + 0x16 + k]);
}

void LanDme::sync_callback(LevelLoadMemory& memory, std::uint32_t status) {
    if (!sync_cb_) return;
    const auto function = sync_cb_;
    memory.write(scratch_, 4, status);
    memory.write(scratch_ + 4, 4, sync_user_);
    sync_cb_ = 0;  // DME clears the pointer after the call (0x0058df88 = 0)
    sync_user_ = 0;
    invoke_(memory, function, scratch_);
    // 132fx5: and its words (002e5d68 / 002e8730: D_0058DF88 = 0, D_0048C478 = 0 after the call).
    memory.write(0x58df88, 4, 0);
    memory.write(0x48c478, 4, 0);
}

void LanDme::joined(LevelLoadMemory& memory, std::uint16_t client) {
    if (joined_[client]) return;
    joined_[client] = true;
    ++joined_calls_;
    newly_joined(memory, client);  // 002db720's info step (132ln5h)
    if (joined_cb_) {
        memory.write(scratch_ + 0x10, 4, 0);
        memory.write(scratch_ + 0x14, 4, 0);  // the world
        memory.write(scratch_ + 0x18, 4, client);
        memory.write(scratch_ + 0x1c, 4, joined_user_);
        invoke_(memory, joined_cb_, scratch_ + 0x10);
    }
    send_client_record(memory, client);  // 002db550 follows the callback in 002db720
}

void LanDme::client_left(LevelLoadMemory& memory, std::uint16_t client) {
    joined_[client] = false;
    info_pending_[client] = false;
    if (!left_cb_) return;
    ++left_calls_;
    memory.write(scratch_ + 0x20, 4, 0);
    memory.write(scratch_ + 0x24, 4, client);
    invoke_(memory, left_cb_, scratch_ + 0x20);
}

void LanDme::control(LevelLoadMemory& memory, std::uint16_t origin, const std::vector<std::uint8_t>& bytes) {
    const std::size_t sizes[] = {0, 7, 11, 3, 3 + 0x24, 9};
    if (bytes.size() < 3 || bytes[0] != kChannelControl || bytes[2] != origin || origin >= kLanMaxClients ||
        bytes[1] == 0 || bytes[1] > kJoinOrder || bytes.size() != sizes[bytes[1]])
        throw std::runtime_error("LanDme: a malformed DME control frame from client " + std::to_string(origin));
    const auto u16 = [&](std::size_t at) { return static_cast<std::uint32_t>(bytes[at] | bytes[at + 1] << 8); };
    switch (bytes[1]) {
    case kStartReady:
        state_[origin] = 3;  // 002e8228: the member's +0x14 = the message's byte 0 (3), whatever it was (132fx5)
        counts_[origin] = static_cast<std::uint16_t>(u16(3));
        counts2_[origin] = static_cast<std::uint16_t>(u16(5));
        // 002e8228 / 002e8338 (the start message's receive): a started host (local record state 5) answers a client
        // in state 3 with 002e8c50 (its record, the handle bases and START_GO) at once: the join in progress
        // (checkpoint 132ln5f). PORT-ONLY: then the host's own state, which a late joiner of the original has from
        // the world join's member list (002e0118, not ported).
        if (started_ && (host_flag_ == 1 || host_flag_ == 2) && state_[transport_.local_index()] == 5 &&
            state_[origin] == 3) {
            start_go(memory, origin);
            send_control(origin, kClientInfo);
        } else if (started_ && state_[transport_.local_index()] == 4 && state_[origin] == 3) {
            // PORT-ONLY (132ln5h): a started client announces itself to a late joiner too (the original's joiner has
            // it from the world join's member list, 002e0118, not ported): the joiner's all-ready check (002e5d68)
            // wants every member's state, and its joined callback for this client (003faab0 clears 0x00493a10[c])
            // lets its 003fac40 send this client its character (003fa940).
            send_control(origin, kClientInfo);
        }
        break;
    case kStartGo: {
        if (started_) break;  // 002e6f40 returns 8 once started
        if (host_flag_ == 1) {  // a start message reaching the host: the start fails with status 7
            sync_callback(memory, 7);
            break;
        }
        state_[transport_.local_index()] = 4;
        started_ = true;
        send_control(kLanAll, kClientInfo);
        const auto clock = u16(3) | u16(5) << 16;
        memory.write(0x58806c, 4, clock);  // 002dd2d8
        const auto rec = record(memory, transport_.local_index());
        if (!rec) unhosted(0x2e6f40, "START_GO without the local client record");
        memory.write(rec, 4, clock);
        activate(memory, u16(7), u16(9));
        break;
    }
    case kClientInfo:
        if (state_[origin] < 4) state_[origin] = 4;
        info_pending_[origin] = true;
        break;
    case kClientRecord:
        receive_client_record(memory, origin, bytes);
        break;
    case kJoinOrder:
        receive_join_order(memory, bytes);
        break;
    default:
        throw std::runtime_error("LanDme: unknown control kind " + std::to_string(bytes[1]));
    }
}

// 002de398's checks (category < 6, id < 0x100, size > 0, data), then one LAN frame (see the header).
std::uint32_t LanDme::send_message(LevelLoadMemory& m, std::uint32_t kind, std::uint32_t world, std::uint32_t to,
                                   std::uint32_t category, std::uint32_t id, std::uint32_t size, std::uint32_t data) {
    if (category >= 6 || id >= 0x100 || static_cast<std::int32_t>(size) <= 0 || !data) return 0x17;
    if (world != 0) return 0xf;  // LAN has world 0 only (002e51b8, then +0x44c)
    if (size + 4 > kLanMaxPayload) unhosted(0x2de398, "a game message larger than one LAN frame");
    std::uint16_t recipient = kLanAll;
    if (to != 0xffffffffU && to != 0xffffU) {
        if (to >= kLanMaxClients || to == transport_.local_index()) return 0x17;
        recipient = static_cast<std::uint16_t>(to);
    }
    const auto mode = static_cast<std::uint8_t>(kind);
    const auto flags = (mode & 0x40) ? kLanReliableOrdered
                       : (mode & 0x10) ? kLanSequenced
                       : (mode & 0x80) ? kLanUnreliable
                                       : kLanReliableOrdered;
    std::vector<std::uint8_t> frame{kChannelGame, static_cast<std::uint8_t>(transport_.local_index()),
                                    static_cast<std::uint8_t>(category), static_cast<std::uint8_t>(id)};
    for (std::uint32_t k = 0; k < size; ++k) frame.push_back(static_cast<std::uint8_t>(m.read(data + k, 1)));
    switch (transport_.send(recipient, frame, flags)) {
    case LanSendResult::Sent: ++messages_sent_; return 0;
    case LanSendResult::NotConnected: return 8;
    default: return 0x17;
    }
}

// 002e64e0 -> 002def08 -> 00305a48(0, world +0x43c, origin, data, size, 0): per message, the category and id bytes,
// the registry's callback (00305c40), its counter, the rest copied to the registry's buffer, then the callback
// {0, world +0x43c, origin, buffer}, which returns the bytes it consumed. DME's error 7 cases throw here, except a
// callback's refusal, which DME drops (below).
void LanDme::game_message(LevelLoadMemory& m, std::uint16_t origin, const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 5 || bytes[1] != origin || origin >= kLanMaxClients)
        throw std::runtime_error("LanDme: a malformed game message frame from client " + std::to_string(origin));
    if (m.read(kRegistryUp, 1) != 1 || !m.read(kRegCallbacks) || !m.read(kRegCounts) || !m.read(kRegBuffer) ||
        !m.read(kRegSpare))
        unhosted(0x305a48, "a game message before the message registry is up");
    const auto size = static_cast<std::uint32_t>(bytes.size() - 2);
    if (size < 2 || size > m.read(kRegBufferSize)) unhosted(0x305a48, "a game message of a size the registry refuses");
    const auto world_out = scratch_ + 0x48;
    const auto world = library_(m, 0x2e51b8, {m.read(0x48c450), world_out}) ? 0U : m.read(world_out);
    const auto world_id = world ? m.read(world + 0x43c) : 0xffffffffU;  // 002dd280
    std::uint32_t pos = 0;
    while (pos < size) {
        const std::uint32_t category = bytes[2 + pos], id = bytes[3 + pos];
        pos += 2;
        const auto max = m.read(kRegMax);
        if (category >= m.read(kRegCategories) || id >= max)
            unhosted(0x305a48, std::string("a game message outside the registry (category " + std::to_string(category) + ", id " +
                                   std::to_string(id) + ")").c_str());
        const auto index = (category * max + id) * 4;
        const auto callback = m.read(m.read(kRegCallbacks) + index);
        if (!callback)
            unhosted(0x305a48, std::string("a game message without a callback (category " + std::to_string(category) + ", id " +
                                   std::to_string(id) + ")").c_str());
        const auto counter = m.read(kRegSpare) + index;
        m.write(counter, 4, m.read(counter) + 1);
        const auto buffer = m.read(kRegBuffer);
        for (std::uint32_t k = pos; k < size; ++k) m.write(buffer + (k - pos), 1, bytes[2 + k]);
        ++messages_received_;
        const auto consumed = library_(m, callback, {0, world_id, origin, buffer});
        if (static_cast<std::int32_t>(consumed) < 0) {
            // 00305a48 returns 7 for a refused message and the rest of it is dropped; 002def08 keeps that in
            // 0x005227e0[0], which 002e47b0 clears before it reads (world 0 never reports it). Such as an
            // announcement (002e76d0) reaching a client whose world is not started yet.
            m.write(0x5227e0, 4, 7);
            ++refused_;
            last_refused_ = callback | category << 24;
            last_refused_origin_ = origin;
            return;
        }
        pos += consumed;
        if (pos > size) unhosted(callback, "a game message callback consumed more than the message");
    }
}

std::uint32_t LanDme::update(LevelLoadMemory& memory) {
    std::vector<std::pair<std::uint16_t, std::vector<std::uint8_t>>> frames;
    LanTransportHandlers handlers;
    std::vector<std::uint16_t> left;
    const bool hosting = transport_.state() == LanTransportState::Hosting;  // the transport resets once the host is lost
    handlers.on_recv = [&](std::uint16_t origin, std::span<const std::uint8_t> bytes) {
        if (bytes.empty() || (bytes[0] != kChannelControl && bytes[0] != kChannelGame && bytes[0] != kChannelBackpack))
            throw std::runtime_error("LanDme: a frame on an unknown channel from client " + std::to_string(origin));
        frames.emplace_back(origin, std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
    };
    handlers.on_join = [](std::uint16_t) {};
    handlers.on_leave = [&](std::uint16_t client, LanLeaveReason) {
        if (client < kLanMaxClients) {
            state_[client] = 0;
            left.push_back(client);
        }
        // A client's link is to the session host only (client 0): losing it is world 0's stream error in 002e47b0
        // (00314cb0 / 003144c8); another client leaving is not a link error (132ln5h).
        if (client == 0 && !hosting) link_status_ = kLinkLost;
    };
    if (barrier_) memory.write(0x58806c, 4, session_clock());  // 002e4a30 keeps the session clock there
    transport_.poll(handlers);
    for (const auto& [origin, bytes] : frames) {  // in their arrival order
        if (bytes[0] == kChannelGame) game_message(memory, origin, bytes);
        else if (bytes[0] == kChannelControl) control(memory, origin, bytes);
        else {
            if (bytes.size() != 12 || bytes[1] != origin)
                throw std::runtime_error("LanDme: a malformed backpack frame from client " + std::to_string(origin));
            const auto word = [&](std::size_t at) {
                return static_cast<std::uint32_t>(bytes[at] | bytes[at + 1] << 8 | bytes[at + 2] << 16 | bytes[at + 3] << 24);
            };
            backpacks_.push_back({origin, bytes[2], bytes[3], word(4), word(8)});
        }
    }
    for (const auto client : left) client_left(memory, client);
    // The object replication (132ln5h): in a started session (the world +0x438 1, its local client < +0x444,
    // 0x0048c454 1) the local client's objects announced and flushed (002e4550), then the second handle space's
    // streams (002e4228).
    if (const auto out = scratch_ + 0x50; memory.read(0x48c450) < 4 && !library_(memory, 0x2e51b8, {memory.read(0x48c450), out})) {
        const auto w = memory.read(out);
        if (w && memory.read(w + 0x438) == 1 && memory.read(w + 0x400) < memory.read(w + 0x444) && (memory.read(0x48c454) & 0xff) == 1) {
            library_(memory, 0x2e4550, {});
            library_(memory, 0x2e4228, {});
        }
    }

    const auto me = transport_.local_index();
    const auto clients = connected();
    // The host's start (002e8730).
    if (barrier_ && !started_ && (host_flag_ == 1 || host_flag_ == 2)) {
        bool blocked = false;
        std::uint16_t first_ready = 0x100;
        for (const auto c : clients) {
            const auto s = state_[c];
            if (s == 0 || s == 4 || s == 5) blocked = true;
            if (s == 3 && first_ready == 0x100) first_ready = c;
        }
        if (!blocked && first_ready == me) {
            state_[me] = 5;
            started_ = true;
            set_session_host(memory, me);  // 002e5550(world, host) (132fx5)
            if (const auto p = member(memory, me)) memory.write(p + 0x24, 4, 1);  // 002e8730
            send_control(kLanAll, kClientInfo);
            activate(memory, 0, 0);
            sync_callback(memory, 0);
            for (const auto c : clients)
                if (c != me && state_[c] == 3) start_go(memory, c);
        }
    }
    // The all-ready check (002e5d68).
    bool called = false;
    if (barrier_ && !all_ready_ && (state_[me] == 4 || state_[me] == 5)) {
        const bool ready = std::none_of(clients.begin(), clients.end(), [&](std::uint16_t c) { return state_[c] == 0; });
        if (ready) {
            all_ready_ = true;
            memory.write(0x48c440, 1, 1);  // 002e5d68 (NetCompleteJoin): D_0048C440 = 1 before the callback (132fx5)
            sync_callback(memory, 0);
            called = true;
        }
    }
    // The joined-the-game callbacks (002e6c78 -> 002db720) once the local client has started. PORT-ONLY pacing
    // (132ln5h): one callback per update (the start callback or one joined callback), as separate messages arrive.
    // A joiner's 00429c00 and 003faab0 each toggle the barrier state (8 -> 9 -> 0xa) and 0042a1b0 reads it after
    // each update: a late joiner's third toggle (its start callback and two members' joined callbacks) in the
    // update that reached 0xa would leave it at 9.
    if (!called && (state_[me] == 4 || state_[me] == 5)) {
        for (const auto c : clients)
            if (c != me && info_pending_[c]) {
                info_pending_[c] = false;
                joined(memory, c);
                break;
            }
    }
    return link_status_;  // 002e4a30 returns 002e47b0's last nonzero world status (00429d30 then reads 002ec580)
}

std::uint32_t LanDme::call(LevelLoadMemory& m, std::uint32_t address, std::initializer_list<std::uint32_t> list) {
    const std::vector<std::uint32_t> a(list);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : 0U; };
    // 132fx5: every API wrapper opens with the gate and the lock (002ea508 .. 002ecf08 in the decomp's
    // src/lib/medius/medius_2EA508.c): 0x11 before the library init, 0x2b while it is not running.
    switch (address) {
    case 0x2ebaf8: case 0x2ebba0: case 0x2ebc48: case 0x2ebce0: case 0x2eabf0: case 0x2eac88: case 0x2ec4f8:
    case 0x2ec580: case 0x2eb9a8:
        if (const auto gate = library_gate(m)) return gate;
        break;
    default:
        break;
    }
    switch (address) {
    case 0x2ebaf8: {  // NetGetMyClientIndex: the gate, then 002dd178(out, world) (132fx5: as the decomp's C)
        if (!arg(0)) return 0x17;
        m.write(arg(0), 4, 0x100);
        if (!m.read(0x48c458)) return 0x11;  // 002df290 has not run
        if (arg(1) >= 4) return 0x17;
        const auto out = scratch_ + 0x54;
        if (const auto r = library_(m, 0x2e51b8, {arg(1), out})) return r;
        if (const auto w = m.read(out)) {
            m.write(arg(0), 4, m.read(w + 0x400));  // 0x100 once 002ec738 reset the world
            return 0;
        }
        // No world record: 002dd178 returns 0 with 0x100. PORT-ONLY: world 0 before the barrier made it (the
        // original's exists from the world join 002e0118, not ported) gives the transport's index, which the
        // barrier then stores at +0x400.
        if (arg(1) == 0 && !disconnected_) m.write(arg(0), 4, transport_.local_index());
        return 0;
    }
    case 0x2ec6a0: {  // leave the world (132ln5h7): the gate, the lock 00302260 / 003023d8, then 002e1a20
        if (const auto gate = library_gate(m)) return gate;
        if (arg(0) >= 4) return 0x17;
        const auto out = scratch_ + 0x50;
        if (const auto r = library_(m, 0x2e51b8, {arg(0), out})) return r;
        if (!m.read(out)) return 0;  // no world record: 002e1a20 returns 0
        // PORT-ONLY stand-in for 002e1a20's 002e1470 (the socket layer), 002e6090 / 002de398 (the leave message
        // 0x50 to the peers), 002e5f68, 002df8e8 and 002db388: the transport's BYE to every link.
        transport_.close();
        left_ = true;
        return 0;
    }
    case 0x2ec738: {  // disconnect the world (132ln5h7): the gate, the lock, then 002e4ca8
        if (const auto gate = library_gate(m)) return gate;
        const auto world = arg(0);
        if (world >= 4) return 0x17;
        const auto out = scratch_ + 0x50;
        if (const auto r = library_(m, 0x2e51b8, {world, out})) return r;
        const auto w = m.read(out);
        if (!w) return 0;
        // PORT-ONLY: 002e4ca8's socket teardown (00314cb0 / 00314a98, 00303598 .. 003060f0, 002dcd00 / 002fba98 and
        // the hook *0x0048c470) is the transport's (closed by 002ec6a0 or here); the rest is 002e4ca8's own stores.
        if (!left_) transport_.close();
        disconnected_ = true;
        for (std::uint32_t c = 0; c < 0x100; ++c) {
            m.write(out + 4, 4, 0);
            library_(m, 0x2e52d8, {world, c, out + 4});
            if (const auto member = m.read(out + 4))
                for (std::uint32_t k = 0; k < 0x2c; k += 4) m.write(member + k, 4, 0);
        }
        m.write(w + 0x43c, 4, 0xffffffffU);
        m.write(w + 0x404, 4, 0x100);
        m.write(w + 0x438, 4, 0);
        m.write(w + 0x400, 4, 0x100);
        for (std::uint32_t k = 0x428; k <= 0x434; k += 4) m.write(w + k, 4, 0);
        m.write(w + 0x440, 4, 0);
        m.write(0x58df80, 4, 0);
        m.write(0x58d17c, 4, 0);
        return 0;
    }
    case 0x2ebba0:  // the session host's client index
        if (arg(0) != 0 || !arg(1)) return 0x17;
        m.write(arg(1), 4, 0);
        return 0;
    case 0x2ebc48:
        if (!arg(0)) return 0x17;
        m.write(arg(0), 4, local_clock());
        return 0;
    case 0x2ebce0:
        if (!arg(0)) return 0x17;
        m.write(arg(0), 4, session_clock());
        return 0;
    case 0x2eabf0: {
        const auto p = arg(0);
        if (!p) return 0x17;
        for (std::uint32_t k = 0; k < 0x38; k += 4) m.write(p + k, 4, 0);
        m.write(p + 0x00, 4, 1);
        m.write(p + 0x04, 4, 0xffffffffU);
        m.write(p + 0x1c, 4, 2);
        return 0;
    }
    case 0x2eac88: {
        const auto p = arg(0), out = arg(1);
        if (!out) return 0x17;
        m.write(out, 4, 1);
        m.write(out + 4, 4, 0x1e);
        if (!p) { m.write(out + 4, 4, 0x17); return 0x17; }
        if (m.read(p) != 1) return 0x26;
        std::uint32_t result = 0;
        if (m.read(p + 4) != 0 || m.read(p + 0x14) > 0x1000 || m.read(p + 0x18) > 0x800) result = 0x17;
        if (!result && barrier_) result = 0x17;
        if (!result && !m.read(p + 0x20)) unhosted(0x2e1990, "the synchronous start wait (no start callback)");
        if (!result) {
            barrier_ = true;
            host_flag_ = m.read(p + 0x1c);
            sync_cb_ = m.read(p + 0x20);
            sync_user_ = m.read(p + 0x24);
            joined_cb_ = m.read(p + 0x28);
            joined_user_ = m.read(p + 0x2c);
            const auto me = transport_.local_index();
            m.write(0x58806c, 4, session_clock());
            start_records(m, p);
            state_[me] = 3;
            const auto peers = transport_.clients();
            // 132fx5: 002e15e0's start message is four bytes {3, client, +0x14, +0x18}: the counts travel as their
            // low bytes (the game's 500 arrives as 0xf4), and the host's 002e8c50 reserves that many handles.
            for (const auto c : peers) send_control(c, kStartReady, {m.read(p + 0x14) & 0xff, m.read(p + 0x18) & 0xff});
            if (peers.empty()) {  // alone (002e1918): started at once; the callback comes with the next update
                state_[me] = 5;
                started_ = true;
                set_session_host(m, me);  // 002e5550(world, local) (132fx5)
                activate(m, 0, 0);
            }
        }
        m.write(out + 4, 4, result);
        return result;
    }
    case 0x2ec4f8:
        return update(m);
    case 0x2ea0a0:  // the game message send: the gate and the lock, then 002de398 (send_message)
        if (const auto gate = library_gate(m)) return gate;
        return send_message(m, arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6));
    case 0x2ec580: {
        const auto out = arg(0);
        if (!out) return 0xa;
        if (m.read(out) != 0x18) return 0x27;
        // 002e4c48 copies 002e47b0's block: +4 + 4 * world the world's status (LAN has world 0 only: its link
        // status; 002e47b0 reads 0x005227e0's game message error (0xc) for worlds 1..3 only), +0x14 the lobby's (none).
        for (std::uint32_t world = 0; world < 4; ++world) m.write(out + 4 + 4 * world, 4, world ? 0U : link_status_);
        m.write(out + 0x14, 4, 0);
        return 0;
    }
    case 0x2eb9a8: {
        const auto out = arg(0), world = arg(1), client = arg(2);
        if (!out) return 0x17;
        for (std::uint32_t k = 0; k < 0x10; k += 4) m.write(out + k, 4, 0);
        if (client >= 0x100 || world >= 4) return 0x17;
        if (world != 0) return 0xf;
        const auto clients = connected();
        if (std::find(clients.begin(), clients.end(), client) == clients.end()) return 8;
        const auto name = transport_.client_name(static_cast<std::uint16_t>(client));
        for (std::size_t k = 0; k < name.size() && k < 15; ++k) m.write(out + static_cast<std::uint32_t>(k), 1, static_cast<std::uint8_t>(name[k]));
        return 0;
    }
    case 0x2eb7a0: {  // a client's barrier state (002eb7a0 -> 002db1f0 -> 002e52d8: the world member's +0x14)
        // PORT-ONLY stand-in: the world member table is the lobby's (not ported); the state is LanDme's state_[].
        if (const auto gate = library_gate(m)) return gate;
        const auto out = arg(0), world = arg(1), client = arg(2);
        if (!out) return 0x17;
        m.write(out, 4, 0);
        if (client >= 0x100 || world >= 4) return 8;
        const auto clients = connected();
        if (world == 0 && std::find(clients.begin(), clients.end(), client) != clients.end())
            m.write(out, 4, state(static_cast<std::uint16_t>(client)));
        return 0;  // 002db1f0 returns 0 once its arguments pass, whatever 002e52d8 finds
    }
    case 0x2ecf08: {  // the network stack's counters (002ecf08 -> 002e6808 -> the IOP's 00302e68)
        // PORT-ONLY stand-in: the IOP socket counters are not modelled; every requested counter reads 0 (no sample).
        if (const auto gate = library_gate(m)) return gate;
        if (arg(0) != 0 && arg(0) != 0xffff) return 0xf;
        for (std::size_t k = 2; k < 6; ++k)
            if (arg(k)) m.write(arg(k), 4, 0);
        return 0;
    }
    default:
        unhosted(address, "a DME API call");
    }
}

}  // namespace omega::lan
