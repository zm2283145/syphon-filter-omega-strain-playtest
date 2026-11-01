#pragma once
// Checkpoint 133on1 (research/ONLINE.md §8.4): the SCE-RT TCP layer the ORIGINAL client (a PS2 or PCSX2 running the
// retail disc) speaks to its Medius servers, so omega_server can serve it next to the port's framing. Everything below
// is read from the matched decomp of the retail executable:
//
//  * Framing (rt_msg_client_set_header 0x003009c8 / get_header 0x00300a58): [u8 type][u16 length, little endian]
//    [payload]. Bit 7 of the type marks an encrypted message (rt_msg_client_process_message 0x002ff4d8: a u32 hash,
//    then RC4-style KM_Decrypt). This layer serves only the unencrypted client: the retail INI has no DISABLE_SECURITY,
//    so the disc needs the DISABLE_SECURITY patch (ONLINE.md §8.3); an encrypted message is logged and dropped.
//  * The client's connect (rt_msg_client_update_comm_wait 0x002fdc60): with an encryption key it first sends type
//    0x12 (its 64-byte public key, 0x002fe8d8); without one, type 0x00 (TCP; 0x15 for the UDP kind), built by
//    0x002fea88: u8 1, u8 8, u8 0x84 (rt_msg_client_get_client_version 0x003017f8), u16 the target, u32 the
//    software id (KM_GetSoftwareID), a 64-byte signature (zeros without a key), then the connect data.
//  * The server's answers (0x002ff868 / 0x002ffd70): type 7 accept: u8 b0 (non-zero), u8 b1 (>= 7,
//    rt_msg_client_is_server_compatible 0x003017b0), u8 b2, u16 the client's index (KM_SetLocalClientID), u16 n,
//    16 bytes of key; type 6 reject: b0 b1 b2, u8 reason, u16 n, 16 bytes; then type 26 complete: u16 (state 8).
//  * App messages: the client sends type 0x0b to the server (rt_msg_client_isend_target 0x002fd180 for id 0xffff;
//    3 / 2 carry a u16 target first), the server sends type 10 (0x00300068, no target). Their payload is the Net
//    layer's (NetISendMessage 0x002de398 / NetSendMessageByTCP 0x002de280): u8 channel, u8 sub-type, then the
//    struct. The Medius client library sends channel 1 and its registration index as the sub-type
//    (NetSendMessage(0x40, connection, 0xffff, 1, index, size, struct)), so the Medius ids of online_protocol.hpp
//    are the sub-types. A payload of 0x1ff bytes or more is split by the client into 0x1ea-byte chunks with a
//    0x14-byte header; the server side of that is not decoded [U].
//  * Other server types the client handles: 5 (an echo answer), 17 disconnect, 19 / 27 crypt keys, 20 session key,
//    28 system message (online_protocol.hpp rt::SystemMessage), 29 cheat query, 30 memory poke.

#include "online_protocol.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omega::online {

namespace rtmsg {
constexpr std::uint8_t ClientConnectTcp = 0x00;
constexpr std::uint8_t ClientAppList = 0x02, ClientAppSingle = 0x03;  // u16 target first
constexpr std::uint8_t ServerEcho = 0x05;
constexpr std::uint8_t ServerConnectReject = 0x06, ServerConnectAccept = 0x07;
constexpr std::uint8_t ServerApp = 0x0a, ClientAppToServer = 0x0b;
constexpr std::uint8_t ServerDisconnect = 0x11;
constexpr std::uint8_t ClientCryptKeyPublic = 0x12;
constexpr std::uint8_t ClientConnectUdp = 0x15;
constexpr std::uint8_t ServerConnectComplete = 0x1a;
constexpr std::uint8_t ServerSystemMessage = 0x1c;
constexpr std::uint8_t kEncrypted = 0x80;
constexpr std::uint8_t kMediusChannel = 1;
}  // namespace rtmsg

std::vector<std::uint8_t> rt_message(std::uint8_t type, std::span<const std::uint8_t> payload);

// One original client's TCP stream.
class RtSession {
public:
    struct Output {
        std::vector<std::uint8_t> to_client;  // RT bytes to write back
        std::vector<Frame> medius;            // Medius requests for OmegaServer (type = the sub-type)
        std::vector<std::string> log;
        bool connected = false;               // the RT connect was accepted just now
        bool close = false;
    };
    explicit RtSession(std::uint16_t client_index) : index_(client_index) {}
    Output feed(std::span<const std::uint8_t> bytes);
    bool connected() const { return connected_; }
    // An omega_server frame (encode_frame bytes) to RT bytes: a Medius response as type 10 [1][id][struct], the
    // system message as type 28; an extension (never sent to an original session) or an id over 255 gives nothing.
    static std::vector<std::uint8_t> from_server_frame(const Frame& f, std::string* note = nullptr);

private:
    void message(std::uint8_t type, std::span<const std::uint8_t> payload, Output& out);
    std::vector<std::uint8_t> buffer_;
    std::uint16_t index_ = 0;
    bool connected_ = false;
    bool bad_ = false;
};

std::string hex_preview(std::span<const std::uint8_t> bytes, std::size_t limit = 48);

}  // namespace omega::online
