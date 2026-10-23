#pragma once
// Checkpoint 132on1 (research/ONLINE.md): the online lobby protocol between the game and omega_server.
//
// Two layers, kept apart:
//  * The ORIGINAL message bodies. The game links Sony's Medius Client Library (MCL, the API functions
//    0x00466a28..0x0046e0f0). Each API function copies MessageID[21] / SessionKey[17] into the request, stores the
//    caller's callback in a slot (0x00497cf4..0x00497e1c) and sends the request struct with Net_SendLocked 0x002ea0a0
//    under a message type that is its registration index in 0x0046e790 (NetMsg_RegisterTypeCore 0x00305998, 270
//    registrations). The incoming handlers call the slot's callback with the response struct and return its size.
//    The ids, sizes and field offsets below are decoded from those functions and from the SFOLobby code that fills
//    the requests and reads the responses (ONLINE.md §3 / §4 give the address of each). Fields the client never
//    reads are left zero and named "opaque" in ONLINE.md.
//  * PORT-ONLY framing and extensions. The original reached the servers through the SCE-RT socket layer the port does
//    not run. Here one TCP stream carries frames [u32 length][u16 type][u16 zero][body]; type < 0x1000 is a Medius
//    message id with the original struct as body, type >= 0xF000 a port-only extension (hello with the platform,
//    roster with platforms, notices, admin, cheat policy, host endpoint). The extensions are clearly not original.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online {

// ---- Medius message ids (registration order in 0x0046e790) ----------------------------------------------------
namespace mid {
constexpr std::uint16_t SessionBegin = 27;            // API 0x0046af60, caller 0x0044c674 (0x0044c620)
constexpr std::uint16_t SessionBeginResponse = 28;    // handler 0x0046d7a8, callback 0x00446d00
constexpr std::uint16_t AccountLogin = 7;             // API 0x00469260, caller 0x00445494 (0x00445420)
constexpr std::uint16_t AccountLoginResponse = 8;     // handler 0x0046c9a0, callback 0x004475e0
constexpr std::uint16_t AccountRegistration = 9;      // API 0x00469458, caller 0x00445c90 (0x00445b90)
constexpr std::uint16_t AccountRegistrationResponse = 10;  // handler 0x0046ca60, callback 0x00447270
constexpr std::uint16_t AccountLogout = 21;           // API 0x00469318, caller 0x0044534c (0x004452f0)
constexpr std::uint16_t AccountLogoutResponse = 22;   // handler 0x0046ca28, callback 0x004474c0
// Checkpoint 132on2d (ONLINE.md §3.6): 17 is AccountUpdateStats (the agent statistics record; 132on1 read it as a
// channel join), sent by 004505e0 after the login, at each game entry and periodically in the lobby.
constexpr std::uint16_t AccountUpdateStats = 17;      // API 0x004695c8, caller 0x0045064c (0x004505e0)
constexpr std::uint16_t AccountUpdateStatsResponse = 18;  // handler 0x0046cb40, callback 0x00446cc0
constexpr std::uint16_t GetAccountStats = 49;         // API 0x0046a048, callers 0x0045127c (00451200) / 0x0044df20
constexpr std::uint16_t GetAccountStatsResponse = 50;  // handler 0x0046cdd0, callbacks 0x00446db0 / 0x004467d0
constexpr std::uint16_t PlayerReport = 1;             // API 0x0046a998, caller 0x004446e0 (in game, every 15 s)
constexpr std::uint16_t WorldReport = 0;              // API 0x0046aa18, caller 0x00444944 (the host's world)
constexpr std::uint16_t CreateGame = 253;             // API 0x00469b58, caller 0x00452590 (0x004524c0)
constexpr std::uint16_t CreateGameResponse = 34;      // handler 0x0046cc30, callback 0x00446f80
constexpr std::uint16_t JoinGame = 35;                // API 0x00469c10, caller 0x0044e800 (0x0044e430)
constexpr std::uint16_t JoinGameResponse = 36;        // handler 0x0046cc70, callback 0x0044e280
constexpr std::uint16_t GameList = 41;                // API 0x00469dc8, callers 0x00450510 / 0x00450bec
constexpr std::uint16_t GameListResponse = 42;        // handler 0x0046ccf0, callbacks 0x0044fa70 / 0x00447790
constexpr std::uint16_t GameInfo = 51;                // API 0x0046a0e8, callers 0x0044e0b4 / 0x0044ed6c
constexpr std::uint16_t GameInfoResponse = 52;        // handler 0x0046ce08, callbacks 0x00446b40 / 0x00446ae0
constexpr std::uint16_t ChatMessage = 59;             // API 0x0046a378, caller 0x004511a4 (0x00451050); no callback
constexpr std::uint16_t ChatFwdMessage = 60;          // handler 0x0046cef8, callback slot 0x00497d60
constexpr std::uint16_t GetPolicy = 71;               // API 0x0046a8f8, caller 0x00445768 (0x004456d0)
constexpr std::uint16_t GetPolicyResponse = 72;       // handler 0x0046d108, callback 0x004473c0
constexpr std::uint16_t GetAnnouncements = 75;        // API 0x0046ab38, caller 0x004455e8 (0x00445570)
constexpr std::uint16_t GetAllAnnouncements = 76;     // API 0x0046abd8 (no caller in the game)
constexpr std::uint16_t GetAnnouncementsResponse = 77;  // handler 0x0046d150, callback 0x004472c0
// Checkpoint 132on2c (research/ONLINE.md §3.2): JOINMIS's chat echo and the game room's player list.
constexpr std::uint16_t TextFilter = 152;             // API slot 0x00598780, caller 0x00451050 (before ChatMessage)
constexpr std::uint16_t TextFilterResponse = 153;     // handler 0x0046da80, callback 0x00447bb0 (the sender's echo)
constexpr std::uint16_t PlayerList = 220;             // API 0x0046c290, callers 0x00451780 / 0x00451848 (004516e0)
constexpr std::uint16_t PlayerListResponse = 221;     // handler 0x0046dd18, callback 0x00447a30
// Checkpoint 132on2g (research/ONLINE.md §3.7): the contact (buddy) list of SFOLobby_Contact.cc. Original wire: a
// PS2 session may use them like a port session.
constexpr std::uint16_t RemoveFromBuddyList = 65;     // caller 0x0044d9c0 (GUI 002adab0)
constexpr std::uint16_t RemoveFromBuddyListResponse = 66;  // callback 0x004466f0 (+238 done, +240 = status == 0)
constexpr std::uint16_t AddToBuddyListConfirmationResponse = 68;  // the inviter's answer; callback 0x004466b0
constexpr std::uint16_t GetBuddyListExtraInfo = 214;  // caller 0x0044d860 (also every SFOLOBBY_BUDDY_REFRESH_INTERVAL)
constexpr std::uint16_t GetBuddyListExtraInfoResponse = 215;  // callback 0x00446830 (one per contact, EndOfList +cc)
constexpr std::uint16_t AddToBuddyListConfirmation = 258;  // caller 0x0044db60 (GuiAgentInfo 0033c7c0)
constexpr std::uint16_t AddToBuddyListFwdConfirmation = 259;  // unsolicited to the invitee; callback 0x00446640
constexpr std::uint16_t AddToBuddyListFwdConfirmationResponse = 260;  // the invitee's answer (0x00446560), no reply
constexpr std::uint16_t GetBuddyInvitations = 261;    // caller 0x0044d780 (lobby tick 00442700 / room 004518f0)
constexpr std::uint16_t GetBuddyInvitationsResponse = 262;  // callback 0x00446740 (EndOfList +44)
// Checkpoint 132on2h (ONLINE.md §3.7): a name's account id, for CONTACTS' "invite by name" (0x002adab0 -> 0x00451370).
constexpr std::uint16_t AccountGetID = 23;            // API 0x0046b140 (slot 0x00598508), caller 0x00451370
constexpr std::uint16_t AccountGetIDResponse = 24;    // callback 0x00446e00 (copies 0x20 bytes to lobby +0xa68)
}  // namespace mid

// Request / response sizes (each incoming handler returns its struct size; each API passes its request size).
namespace msize {
constexpr std::size_t SessionBegin = 0x26, SessionBeginResponse = 0x30;
constexpr std::size_t AccountLogin = 0x66, AccountLoginResponse = 0xc4;
constexpr std::size_t AccountRegistration = 0x6c, AccountRegistrationResponse = 0x20;
constexpr std::size_t AccountLogout = 0x26, AccountLogoutResponse = 0x1c;
constexpr std::size_t AccountUpdateStats = 0x126, AccountUpdateStatsResponse = 0x1c;
constexpr std::size_t GetAccountStats = 0x2c, GetAccountStatsResponse = 0x148;
constexpr std::size_t PlayerReport = 0x118, WorldReport = 0x180;
constexpr std::size_t CreateGame = 0x94, CreateGameResponse = 0x20;
constexpr std::size_t JoinGame = 0xc4, JoinGameResponse = 0xbc;
constexpr std::size_t GameList = 0x30, GameListResponse = 0x70;
constexpr std::size_t GameInfo = 0x2c, GameInfoResponse = 0x18c;
constexpr std::size_t ChatMessage = 0x70, ChatFwdMessage = 0x80;
constexpr std::size_t GetPolicy = 0x2c, GetPolicyResponse = 0x120;
constexpr std::size_t GetAnnouncements = 0x2c, GetAnnouncementsResponse = 0x40c;
constexpr std::size_t TextFilter = 0x6c, TextFilterResponse = 0x5c;
constexpr std::size_t PlayerList = 0x1c, PlayerListResponse = 0xd0;
constexpr std::size_t RemoveFromBuddyList = 0x2c, RemoveFromBuddyListResponse = 0x1c;
constexpr std::size_t AddToBuddyListConfirmation = 0x30, AddToBuddyListConfirmationResponse = 0x40;
constexpr std::size_t AddToBuddyListFwdConfirmation = 0x40, AddToBuddyListFwdConfirmationResponse = 0x38;
constexpr std::size_t GetBuddyListExtraInfo = 0x15, GetBuddyListExtraInfoResponse = 0xd0;
constexpr std::size_t GetBuddyInvitations = 0x15, GetBuddyInvitationsResponse = 0x48;
constexpr std::size_t AccountGetID = 0x46, AccountGetIDResponse = 0x20;
}  // namespace msize

// Field offsets every message shares (the API functions copy these two fields first).
constexpr std::size_t kMessageIdOffset = 0x00, kMessageIdLength = 21;   // MessageID[21]
constexpr std::size_t kSessionKeyOffset = 0x15, kSessionKeyLength = 17;  // SessionKey[17] (requests)
constexpr std::size_t kStatusOffset = 0x18;                              // StatusCode (most responses)

// Status codes the client's callbacks test (0x004475e0 login, 0x00447270 registration, 0x00445a40 login flow).
namespace status {
constexpr std::int32_t Success = 0;
constexpr std::int32_t NoResult = 10;               // the game-list callback skips a row whose status is not 0
constexpr std::int32_t AccountAlreadyExists = -999;  // 0x00447270 -> error 0x3d ACCOUNT_ALREADY_EXISTS
constexpr std::int32_t AccountNotFound = -998;       // 0x004475e0 -> error 0x3b ACCOUNT_NOT_FOUND
constexpr std::int32_t AccountLoggedIn = -997;       // 0x004475e0 -> 0x3c; 0x00445a40 logs out and logs in again
constexpr std::int32_t WMError = -991;               // 0x00445a40 logs out and fails (error 1 GENERAL)
constexpr std::int32_t PlayerNotPrivileged = -989;   // 0x004475e0 -> error 1 GENERAL (omega_server: banned)
constexpr std::int32_t RequestDenied = -985;         // join refused (game full / closed)
constexpr std::int32_t InvalidPassword = -978;       // 0x004475e0 -> error 0x3a INCORRECTPASSWORD
constexpr std::int32_t GameNotFound = -977;
}  // namespace status

// GameInfo / game list world status (the lobby's game entries test 1 = open, 3 = closed, 0x0044e430).
constexpr std::int32_t kWorldStaging = 1, kWorldActive = 2, kWorldClosed = 3;

// Checkpoint 132on2d (ONLINE.md §3.6): the Stats[256] field of AccountUpdateStats / GetAccountStatsResponse /
// PlayerReport and the GameStats[256] of WorldReport (the game fills 0xfc of it, online_stats.hpp).
constexpr std::size_t kStatsFieldSize = 256;
// WorldReport (0, 0x00444944): Medius' layout after SessionKey[17] (+0) and WorldID (+14).
struct WorldReportFields {
    std::int32_t world = 0;           // +14
    std::int32_t player_count = 0;    // +18
    std::string game_name;            // +1c [64]
    std::vector<std::uint8_t> game_stats;  // +5c [256]
    std::int32_t min_players = 0, max_players = 0, game_level = 0, skill_level = 0, rules_set = 0;  // +15c..+16c
    std::int32_t generic[3] = {0, 0, 0};  // +170..+178
    std::int32_t world_status = 0;    // +17c
};
// The join request's host type word (+0x50 = 2, 0x0044e430; 0x00469c10 takes the peer-to-peer branch for 2).
constexpr std::int32_t kGameHostPeerToPeer = 2;

// ---- PORT-ONLY: platforms (research/ONLINE.md §7) ----------------------------------------------------------------
// Ps2: the original PS2 game (real hardware or PCSX2) once the original wire layer is served (ONLINE.md §8).
enum class Platform : std::uint8_t { Unknown = 0, Pc = 1, Vita = 2, Ps4 = 3, Ps5 = 4, Switch = 5, Ps2 = 6 };
Platform platform_from_byte(std::uint8_t value);  // out-of-range values read as Unknown
std::string_view platform_name(Platform p);        // "pc", "vita", "ps4", "ps5", "switch", "ps2", "unknown"
std::string_view platform_badge(Platform p);       // "PC", "VITA", "PS4", "PS5", "NSW", "PS2", "?"
// A player name as the online lists and chat draw it: "[PC] name", or the bare name when the badges are hidden
// (port setting online_platform_badges=0).
std::string badged_name(std::string_view name, Platform p, bool show_badges);
std::optional<Platform> parse_platform(std::string_view name);
Platform this_platform();  // the build's own platform (PC today)

// ---- PORT-ONLY: extension message types --------------------------------------------------------------------------
namespace ext {
constexpr std::uint16_t Hello = 0xF001;         // client -> server: magic, protocol, platform, build, caps
constexpr std::uint16_t Welcome = 0xF002;       // server -> client: protocol, negotiated caps, allow_cheats, server name
constexpr std::uint16_t Roster = 0xF010;        // server -> client: every logged-in player, its platform and world
constexpr std::uint16_t AdminCommand = 0xF020;  // client -> server: one admin console line
constexpr std::uint16_t AdminReply = 0xF021;    // server -> client: the command's output
constexpr std::uint16_t Notice = 0xF030;        // server -> client: refusal reason, kick, broadcast
constexpr std::uint16_t CheatPolicy = 0xF040;   // server -> client: allow_cheats changed
constexpr std::uint16_t HostEndpoint = 0xF050;  // client -> server: the host's in-level UDP port for its world
constexpr std::uint16_t LeaveGame = 0xF051;     // client -> server: left / closed its world
constexpr std::uint16_t GamePassword = 0xF052;  // client -> server: its created world's join password (132on2c)
constexpr std::uint16_t ModReport = 0xF060;     // client -> server, after the hello: its mods (132md4, online_mod_report.hpp)
// 132on2e (online_relay.hpp): server -> client after its login: the relay UDP port, its virtual address and token.
constexpr std::uint16_t RelayTicket = 0xF070;
// 132on2e: server -> joiner before its JoinGameResponse: the world's host as a relay virtual address.
constexpr std::uint16_t RelayPeer = 0xF071;
// 132on2g (ONLINE.md §6.15): client -> server: the recent players request (empty body); server -> client: the list
// (u16 count, then per player u32 account, str name, u8 platform, u8 online, u32 seconds ago). The original has no
// recent-players list (no string, no message).
constexpr std::uint16_t RecentPlayers = 0xF080;
constexpr std::uint16_t RecentPlayersList = 0xF081;
// 132on2g: client -> server: u8 1 / 0, this player's chat filter on / off (the server's chat_filter is the default).
constexpr std::uint16_t ChatFilter = 0xF082;
}  // namespace ext

constexpr std::uint32_t kHelloMagic = 0x314d474fu;  // "OGM1"
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint16_t kDefaultServerPort = 10075;  // the Medius authentication server's customary port
constexpr std::size_t kMaxFrameBody = 64 * 1024;

namespace notice {
constexpr std::uint32_t Banned = 1, Kicked = 2, Broadcast = 3, CheatsDisabled = 4, GameClosed = 5, Info = 6;
constexpr std::uint32_t Shutdown = 7;  // 132on2g: the server stops or restarts soon (admin shutdown / restart)
}

struct Frame {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> body;
};

// [u32 length of type+pad+body][u16 type][u16 0][body], little endian.
std::vector<std::uint8_t> encode_frame(std::uint16_t type, std::span<const std::uint8_t> body);

// Accumulates stream bytes and yields whole frames. A length above kMaxFrameBody makes `bad()` true.
class FrameReader {
public:
    void feed(std::span<const std::uint8_t> bytes);
    std::optional<Frame> next();
    bool bad() const { return bad_; }

private:
    std::vector<std::uint8_t> buffer_;
    bool bad_ = false;
};

// ---- Fixed-layout struct helpers (the original structs are little-endian, like the EE) ----------------------------
std::int32_t get_i32(std::span<const std::uint8_t> s, std::size_t off);
void put_i32(std::span<std::uint8_t> s, std::size_t off, std::int32_t v);
std::string get_str(std::span<const std::uint8_t> s, std::size_t off, std::size_t len);  // to the first NUL
void put_str(std::span<std::uint8_t> s, std::size_t off, std::size_t len, std::string_view v);  // NUL-terminated, truncated

// ---- Extension bodies (little-endian, strings as u16 length + bytes) ----------------------------------------------
class ExtWriter {
public:
    ExtWriter& u8(std::uint8_t v);
    ExtWriter& u16(std::uint16_t v);
    ExtWriter& u32(std::uint32_t v);
    ExtWriter& str(std::string_view v);
    const std::vector<std::uint8_t>& bytes() const { return out_; }

private:
    std::vector<std::uint8_t> out_;
};

class ExtReader {
public:
    explicit ExtReader(std::span<const std::uint8_t> in) : in_(in) {}
    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::string str();
    bool ok() const { return ok_; }
    // Checkpoint 132md16: every byte read (an optional trailing field is absent).
    bool at_end() const { return pos_ >= in_.size(); }
private:
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// PORT-ONLY capabilities a port client announces in its hello (ONLINE.md §8): the server sends an extension only to
// a client that announced it. A session without a hello (and every original PS2 / PCSX2 session) gets none: exactly
// the original message set.
namespace cap {
constexpr std::uint32_t Roster = 1u << 0;        // ext::Roster (players with platforms: the badges)
constexpr std::uint32_t Notices = 1u << 1;       // ext::Notice (ban reason, kick, broadcast, game closed)
constexpr std::uint32_t CheatPolicy = 1u << 2;   // ext::Welcome's / ext::CheatPolicy's allow_cheats
constexpr std::uint32_t Admin = 1u << 3;         // ext::AdminCommand / AdminReply
constexpr std::uint32_t HostEndpoint = 1u << 4;  // ext::HostEndpoint / LeaveGame
// 132on2c: ext::GamePassword (a hosted game's password; the joiner's goes in JoinGame's own GamePassword +0x30).
constexpr std::uint32_t GamePassword = 1u << 5;
// 132md4: ext::ModReport (the server keeps it and applies its mod_policy to CreateGame / JoinGame).
constexpr std::uint32_t Mods = 1u << 6;
// 132on2e: ext::RelayTicket / RelayPeer (the in-level UDP relay through omega_server, online_relay.hpp).
constexpr std::uint32_t Relay = 1u << 7;
// 132on2g: ext::RecentPlayers / RecentPlayersList / ChatFilter (port-only social extras).
constexpr std::uint32_t Social = 1u << 8;
constexpr std::uint32_t All = Roster | Notices | CheatPolicy | Admin | HostEndpoint | GamePassword | Mods | Relay | Social;
}  // namespace cap

// Which wire a session arrived on (ONLINE.md §8). Port: the port client's TCP framing. Original: a session from the
// original game (PS2 hardware or PCSX2) through the original-wire layer (not written yet; the server core is ready
// for it). Original sessions never receive an extension and count as platform ps2.
enum class ClientKind : std::uint8_t { Port = 0, Original = 1 };

struct HelloInfo {
    std::uint16_t protocol = kProtocolVersion;
    Platform platform = Platform::Unknown;
    std::uint32_t build = 0;
    std::uint32_t caps = 0;
};
std::vector<std::uint8_t> encode_hello(const HelloInfo& h);
std::optional<HelloInfo> decode_hello(std::span<const std::uint8_t> body);

struct RosterEntry {
    std::uint32_t account_id = 0;
    Platform platform = Platform::Unknown;
    std::uint32_t world_id = 0;  // 0: in the lobby
    std::string name;
};
std::vector<std::uint8_t> encode_roster(const std::vector<RosterEntry>& entries);
std::optional<std::vector<RosterEntry>> decode_roster(std::span<const std::uint8_t> body);

// The game-list row's GameName text, decoded at 0x00447790: "%32s\t%d\t%d\t%d\t%d"; the client turns the name's spaces into 0xff for the scan and back, so the wire has spaces.
struct GameNameFields {
    std::string name;
    int a = 0, b = 0, c = 0, d = 0;  // 0x00447790 checks 1 <= a <= 4, 0 <= b < lobby+0x58, 0 <= c <= 5
};
std::optional<GameNameFields> parse_game_name(std::string_view text);
std::string format_game_name(const GameNameFields& f);

}  // namespace omega::online
