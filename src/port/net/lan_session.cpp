// PORT-ONLY: LanSession (see lan_session.hpp).
#include "lan_session.hpp"

#include "lan_wire.hpp"

#include <random>
#include <vector>

namespace omega::lan {

std::uint32_t lan_build_hash(std::string_view iso_sha256, std::string_view executable_sha256,
                             std::uint16_t protocol_version, std::uint32_t port_revision) {
    std::vector<std::uint8_t> bytes;
    for (char c : std::string_view("SFOSLAN-BUILD")) bytes.push_back(static_cast<std::uint8_t>(c));
    bytes.push_back(0);
    bytes.push_back(static_cast<std::uint8_t>(protocol_version));
    bytes.push_back(static_cast<std::uint8_t>(protocol_version >> 8));
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(port_revision >> (8 * i)));
    for (std::string_view digest : {iso_sha256, executable_sha256}) {
        for (char c : digest) {
            const char l = c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
            bytes.push_back(static_cast<std::uint8_t>(l));
        }
        bytes.push_back(0);
    }
    return lan_crc32(bytes);
}

std::uint64_t lan_random_session_id() {
    std::random_device rd;
    std::uint64_t v = 0;
    while (v == 0) v = (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
    return v;
}

LanSession::LanSession(LanSessionConfig config) : config_(std::move(config)) {
    config_.discovery.build_hash = config_.build_hash;
    config_.discovery.protocol_version = kLanProtocolVersion;
}

LanSession::~LanSession() { leave(); }

bool LanSession::host(const LanHostSetup& setup) {
    if (transport_) return false;  // hosting, or joining a game
    if (setup.mission < 0 || setup.mission > 0xff || setup.num_agents < 2 || setup.num_agents > 4 ||
        setup.join_filter > 2)
        return false;
    LanAdvert advert;
    advert.protocol_version = kLanProtocolVersion;
    advert.build_hash = config_.build_hash;
    advert.session_id = config_.session_id ? config_.session_id() : lan_random_session_id();
    advert.host_name = setup.host_name.substr(0, kLanHostNameSize - 1);
    advert.mission_index = static_cast<std::uint8_t>(setup.mission);
    advert.mission_type = 0;  // GuiMissionSetup never reads its mission_type widget (no reference to the name)
    advert.agents_now = 1;
    advert.agents_max = static_cast<std::uint8_t>(setup.num_agents);
    advert.join_filter = setup.join_filter;
    advert.state = LanSessionState::Open;

    LanTransportConfig tc;
    tc.protocol_version = kLanProtocolVersion;
    tc.build_hash = config_.build_hash;
    tc.agent_name = advert.host_name;
    tc.session_id = advert.session_id;
    tc.max_clients = static_cast<std::uint16_t>(setup.num_agents);  // the host and its joiners
    tc.join_filter = config_.join_filter;                             // 132md5
    tc.clock = config_.clock;
    tc.socket_factory = config_.socket_factory;
    auto transport = std::make_unique<LanTransport>(tc);
    if (!transport->host(config_.game_port)) return false;
    advert.game_port = transport->local_port();
    auto discovery = std::make_unique<LanDiscoveryHost>(config_.discovery);
    if (!discovery->start(advert)) {
        transport->close();
        return false;
    }
    transport_ = std::move(transport);
    discovery_ = std::move(discovery);
    advert_ = advert;
    hosting_ = true;
    return true;
}

void LanSession::exit_game() {
    if (!hosting_) return;
    if (transport_) transport_->set_accepting_joins(false);
    if (discovery_) discovery_->stop();
    discovery_.reset();
}

void LanSession::leave() {
    if (discovery_) discovery_->stop();
    if (transport_) transport_->close();
    discovery_.reset();
    transport_.reset();
    hosting_ = false;
    joined_session_ = 0;
}

void LanSession::pump_lobby() {
    if (discovery_) discovery_->pump();
    if (browser_) browser_->pump();
}

void LanSession::pump() {
    if (discovery_) discovery_->pump();
    if (browser_) browser_->pump();
    if (transport_) transport_->poll({});
}

bool LanSession::browse(bool on) {
    if (!on) {
        if (browser_) browser_->stop();
        browser_.reset();
        return true;
    }
    if (browser_) return true;
    auto browser = std::make_unique<LanDiscoveryBrowser>(config_.discovery);
    if (!browser->start()) return false;
    browser_ = std::move(browser);
    return true;
}

std::vector<LanBrowseEntry> LanSession::games() const {
    return browser_ ? browser_->entries() : std::vector<LanBrowseEntry>{};
}

bool LanSession::join(std::uint64_t session_id, const std::string& agent_name) {
    if (transport_ || !browser_) return false;
    const LanBrowseEntry* entry = nullptr;
    for (const auto& e : browser_->entries())
        if (e.advert.session_id == session_id) entry = &e;
    if (!entry || entry->version_mismatch) return false;
    LanTransportConfig tc;
    tc.protocol_version = kLanProtocolVersion;
    tc.build_hash = config_.build_hash;
    tc.agent_name = agent_name.substr(0, kLanHostNameSize - 1);
    if (config_.join_extension) tc.join_extension = config_.join_extension();  // 132md5
    tc.clock = config_.clock;
    tc.socket_factory = config_.socket_factory;
    auto transport = std::make_unique<LanTransport>(tc);
    if (!transport->connect(entry->game_address)) return false;
    transport_ = std::move(transport);
    hosting_ = false;
    joined_session_ = session_id;
    return true;
}

bool LanSession::join_at(const LanAddress& host_address, const std::string& agent_name) {
    if (transport_) return false;
    LanTransportConfig tc;
    tc.protocol_version = kLanProtocolVersion;
    tc.build_hash = config_.build_hash;
    tc.agent_name = agent_name.substr(0, kLanHostNameSize - 1);
    if (config_.join_extension) tc.join_extension = config_.join_extension();  // 132md5
    tc.clock = config_.clock;
    tc.socket_factory = config_.socket_factory;
    auto transport = std::make_unique<LanTransport>(tc);
    if (!transport->connect(host_address)) return false;
    transport_ = std::move(transport);
    hosting_ = false;
    joined_session_ = 0;
    return true;
}

LanJoinState LanSession::join_state() const {
    if (!transport_ || hosting_) return LanJoinState::Idle;
    switch (transport_->state()) {
    case LanTransportState::Connecting: return LanJoinState::Pending;
    case LanTransportState::Connected:
        return joined_session_ == 0 || transport_->session_id() == joined_session_ ? LanJoinState::Accepted
                                                                                  : LanJoinState::Failed;
    case LanTransportState::Rejected:
        return transport_->join_error() == LanJoinError::Timeout ? LanJoinState::Failed : LanJoinState::Rejected;
    case LanTransportState::Disconnected: return LanJoinState::Failed;
    default: return LanJoinState::Idle;
    }
}

void LanSession::cancel_join() {
    if (!transport_ || hosting_) return;
    transport_->close();
    transport_.reset();
    joined_session_ = 0;
}

}  // namespace omega::lan
