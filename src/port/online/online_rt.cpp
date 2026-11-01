// Checkpoint 133on1: see online_rt.hpp (research/ONLINE.md §8.4).
#include "online_rt.hpp"

#include "online_hash.hpp"

#include <algorithm>
#include <cstdio>

namespace omega::online {

std::string hex_preview(std::span<const std::uint8_t> bytes, std::size_t limit) {
    std::string out;
    char b[4];
    for (std::size_t i = 0; i < bytes.size() && i < limit; ++i) {
        std::snprintf(b, sizeof b, "%02x", bytes[i]);
        if (i) out.push_back(' ');
        out += b;
    }
    if (bytes.size() > limit) out += " ...";
    return out;
}

std::vector<std::uint8_t> rt_message(std::uint8_t type, std::span<const std::uint8_t> payload) {
    const std::size_t n = std::min<std::size_t>(payload.size(), 0xffff);
    std::vector<std::uint8_t> out;
    out.reserve(3 + n);
    out.push_back(type);
    out.push_back(static_cast<std::uint8_t>(n & 0xff));
    out.push_back(static_cast<std::uint8_t>(n >> 8));
    out.insert(out.end(), payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(n));
    return out;
}

RtSession::Output RtSession::feed(std::span<const std::uint8_t> bytes) {
    Output out;
    if (bad_) return out;
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::size_t at = 0;
    while (buffer_.size() - at >= 3) {
        const std::uint8_t type = buffer_[at];
        std::size_t len = static_cast<std::size_t>(buffer_[at + 1]) | static_cast<std::size_t>(buffer_[at + 2]) << 8;
        // 134on5: an encrypted message's length names its data only; the u32 hash before it is extra
        // (rt_msg_client_process_recv_buf 0x002ff308: n += 4 when the length is non-zero and bit 7 is set).
        if ((type & rtmsg::kEncrypted) && len) len += 4;
        if (buffer_.size() - at - 3 < len) break;
        message(type, std::span<const std::uint8_t>(buffer_.data() + at + 3, len), out);
        at += 3 + len;
        if (out.close) {
            bad_ = true;
            break;
        }
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(at));
    if (buffer_.size() > 0x10003) {  // a length field can name at most 0xffff bytes
        out.log.push_back("rt: stream out of step, closing");
        out.close = true;
        bad_ = true;
    }
    return out;
}

void RtSession::set_security(std::shared_ptr<const rtcrypto::RsaKey> server_key, bool encrypt_replies, KeySource keys) {
    server_key_ = std::move(server_key);
    encrypt_replies_ = encrypt_replies;
    keys_ = std::move(keys);
}

void RtSession::reply(std::vector<std::uint8_t>& to, std::uint8_t type, std::span<const std::uint8_t> payload) const {
    const auto m = seal(rt_message(type, payload));
    to.insert(to.end(), m.begin(), m.end());
}

std::vector<std::uint8_t> RtSession::seal(std::span<const std::uint8_t> rt_bytes) const {
    if (!secure_ || !encrypt_replies_) return {rt_bytes.begin(), rt_bytes.end()};
    std::vector<std::uint8_t> out;
    out.reserve(rt_bytes.size() + 16);
    std::size_t at = 0;
    while (rt_bytes.size() - at >= 3) {
        const std::uint8_t type = rt_bytes[at];
        const std::size_t len = static_cast<std::size_t>(rt_bytes[at + 1]) | static_cast<std::size_t>(rt_bytes[at + 2]) << 8;
        if (rt_bytes.size() - at - 3 < len || (type & rtmsg::kEncrypted) || !len) {  // not ours to encrypt: copy
            const std::size_t n = std::min(rt_bytes.size() - at, 3 + len);
            out.insert(out.end(), rt_bytes.begin() + static_cast<std::ptrdiff_t>(at),
                       rt_bytes.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
            continue;
        }
        // KM_Encrypt 0x002f9780's context-3 branch: the hash of the plaintext, then RCEncrypt salted with it.
        std::vector<std::uint8_t> data(rt_bytes.begin() + static_cast<std::ptrdiff_t>(at + 3),
                                       rt_bytes.begin() + static_cast<std::ptrdiff_t>(at + 3 + len));
        const std::uint32_t h = rtcrypto::rt_hash(data, rtcrypto::kCtxClientSession);
        rtcrypto::rc_encrypt(rc_key_, h, data);
        out.push_back(static_cast<std::uint8_t>(type | rtmsg::kEncrypted));
        out.push_back(rt_bytes[at + 1]);
        out.push_back(rt_bytes[at + 2]);
        for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(h >> (8 * k)));
        out.insert(out.end(), data.begin(), data.end());
        at += 3 + len;
    }
    return out;
}

void RtSession::crypt_key_public(std::span<const std::uint8_t> client_key, Output& out) {
    if (client_key.size() != rtcrypto::kBlock) {
        out.log.push_back("rt: the client's public key is " + std::to_string(client_key.size()) + " bytes, not 64; closing");
        out.close = true;
        return;
    }
    rtcrypto::Block cn;
    std::copy(client_key.begin(), client_key.end(), cn.begin());
    if (!(cn[0] & 1)) {
        out.log.push_back("rt: the client's public key is no RSA modulus (even); closing");
        out.close = true;
        return;
    }
    rtcrypto::Block k{};
    if (keys_) k = keys_();
    else {
        const auto r = random_bytes(rtcrypto::kBlock);
        std::copy(r.begin(), r.end(), k.begin());
    }
    while (!rtcrypto::less_than(k, cn)) k[rtcrypto::kBlock - 1] >>= 1;  // below the client's modulus (no adjust)
    // 0x00300248: type 0x13 carries the 64-byte key, here RSA-encrypted to the client's key (KM_Decrypt's
    // context 7 decrypts it with the client's private key and checks the hash of the plaintext).
    const std::uint32_t h = rtcrypto::rt_hash(k, rtcrypto::kCtxRsa);
    std::vector<std::uint8_t> payload;
    for (int i = 0; i < 4; ++i) payload.push_back(static_cast<std::uint8_t>(h >> (8 * i)));
    std::vector<std::uint8_t> cipher(k.begin(), k.end());
    rtcrypto::rsa_encrypt17(cn, cipher);
    payload.insert(payload.end(), cipher.begin(), cipher.end());
    out.to_client.push_back(rtmsg::ServerCryptKeyPeer | rtmsg::kEncrypted);
    out.to_client.push_back(static_cast<std::uint8_t>(rtcrypto::kBlock));
    out.to_client.push_back(0);
    out.to_client.insert(out.to_client.end(), payload.begin(), payload.end());
    rc_key_ = k;
    secure_ = true;
    out.log.push_back("rt: the client's RSA public key received; sent its RC key (type 0x93, RSA to the client's key); "
                      "the session is encrypted from here (context 3)");
}

void RtSession::message(std::uint8_t type, std::span<const std::uint8_t> p, Output& out) {
    std::uint8_t base = type & 0x7f;
    char head[96];
    std::snprintf(head, sizeof head, "rt: type 0x%02x, %zu byte(s)", type, p.size());
    std::vector<std::uint8_t> plain;
    bool was_encrypted = false;
    if ((type & rtmsg::kEncrypted) && !p.empty()) {
        // 134on5: [u32 hash][data]; for the targeted kinds (2, 3, 0x1b) a plain u16 target precedes the
        // encrypted part, as the client's receive side has it (0x002ff4d8).
        if (p.size() < 4) {
            out.log.push_back(std::string(head) + ": encrypted message shorter than its hash; dropped");
            return;
        }
        const std::uint32_t h = p[0] | static_cast<std::uint32_t>(p[1]) << 8 | static_cast<std::uint32_t>(p[2]) << 16 |
                                static_cast<std::uint32_t>(p[3]) << 24;
        const std::size_t skip = (base == rtmsg::ClientAppList || base == rtmsg::ClientAppSingle || base == 0x1b) ? 2 : 0;
        if (p.size() < 4 + skip) {
            out.log.push_back(std::string(head) + ": encrypted message without its target; dropped");
            return;
        }
        plain.assign(p.begin() + 4, p.end());
        std::span<std::uint8_t> data(plain.data() + skip, plain.size() - skip);
        const std::uint8_t ctx = rtcrypto::hash_context(h);
        if (!rtcrypto::hash_present(h)) {
            // no hash bits: the client sent it plain (KM_Decrypt's case 0)
        } else if (ctx == rtcrypto::kCtxRsa) {
            if (!server_key_) {
                out.log.push_back(std::string(head) + ": encrypted to the disc's Medius public key, and omega_server "
                                                      "has no original_rt_key (ONLINE_DEPLOY.md; or apply the "
                                                      "DISABLE_SECURITY patch, ONLINE.md 8.3); closing");
                out.close = true;
                return;
            }
            rtcrypto::rsa_decrypt(*server_key_, data);
            if (rtcrypto::rt_hash(data, ctx) != h && data.size() == rtcrypto::kBlock) {  // KM_RSAAdjust
                rtcrypto::Block b;
                std::copy(data.begin(), data.end(), b.begin());
                b = rtcrypto::add_mod_2_512(b, server_key_->n);
                std::copy(b.begin(), b.end(), data.begin());
            }
            if (rtcrypto::rt_hash(data, ctx) != h) {
                out.log.push_back(std::string(head) + ": RSA (context 7) hash differs: the key is not the disc's; "
                                                      "closing");
                out.close = true;
                return;
            }
        } else if (ctx == rtcrypto::kCtxClientSession && secure_) {
            rtcrypto::rc_decrypt(rc_key_, h, data);
            if (rtcrypto::rt_hash(data, ctx) != h) {
                out.log.push_back(std::string(head) + ": RC (context 3) hash differs; dropped");
                return;
            }
        } else {
            char b[96];
            std::snprintf(b, sizeof b, ": encrypted with context %u, which this session holds no key for; dropped", ctx);
            out.log.push_back(std::string(head) + b);
            return;
        }
        was_encrypted = rtcrypto::hash_present(h);
        p = plain;
        std::snprintf(head, sizeof head, "rt: type 0x%02x (decrypted, context %u), %zu byte(s)", base, ctx, p.size());
    }
    switch (base) {
    case rtmsg::ClientConnectTcp: {
        // 0x002fea88: version 1.8.0x84, u16 target, u32 software id, 64-byte signature, connect data.
        std::string what = std::string(head) + ": connect (TCP)";
        if (p.size() >= 9) {
            char b[96];
            std::snprintf(b, sizeof b, ", client version %u.%u.%u, target %u, software id %u", p[0], p[1], p[2],
                          p[3] | p[4] << 8, static_cast<unsigned>(p[5] | p[6] << 8 | p[7] << 16 | static_cast<unsigned>(p[8]) << 24));
            what += b;
        }
        if (p.size() > 9 + 64) what += ", data " + hex_preview(p.subspan(9 + 64));
        out.log.push_back(what);
        // 0x002ff868: accept (b0 1, b1 8 >= 7, b2 0x84), the client's index, n, a zero key; then complete (26).
        std::vector<std::uint8_t> accept = {1, 8, 0x84, static_cast<std::uint8_t>(index_ & 0xff),
                                            static_cast<std::uint8_t>(index_ >> 8), 1, 0};
        accept.resize(accept.size() + 16, 0);
        reply(out.to_client, rtmsg::ServerConnectAccept, accept);
        const std::uint8_t done[2] = {1, 0};
        reply(out.to_client, rtmsg::ServerConnectComplete, done);
        out.log.push_back("rt: connect accepted (type 7, client index " + std::to_string(index_) +
                          ") and completed (type 26)" + (secure_ && encrypt_replies_ ? ", encrypted" : ""));
        connected_ = true;
        out.connected = true;
        return;
    }
    case rtmsg::ClientCryptKeyPublic:
        if (!server_key_) {
            out.log.push_back(std::string(head) + ": the client's RSA public key (it holds an encryption key: the "
                                                  "DISABLE_SECURITY patch is not applied) and omega_server has no "
                                                  "original_rt_key (ONLINE.md 8.7); closing");
            out.close = true;
            return;
        }
        out.log.push_back(std::string(head) + ": the client's RSA public key");
        crypt_key_public(p, out);
        return;
    case rtmsg::ClientConnectUdp:
        out.log.push_back(std::string(head) + ": connect (UDP kind) on the TCP stream; ignored");
        return;
    case rtmsg::ClientEcho: {
        // 134on4: the keep-alive (0x002fddf0 queues type 5, byte 0xa5, every 15 s in state 8); the client's type-5
        // handler 0x002ff760 takes one off its count of unanswered echoes, and any received byte restarts its 35 s
        // receive timer (0x002fdae8). The answer is type 5 with the same payload.
        reply(out.to_client, rtmsg::ServerEcho, p);
        out.log.push_back(std::string(head) + ": echo " + hex_preview(p) + ", answered (type 5)");
        return;
    }
    case rtmsg::ClientDisconnect:
        out.log.push_back(std::string(head) + ": the client disconnects");
        out.close = true;
        return;
    case rtmsg::ClientAppToServer:
    case rtmsg::ClientAppSingle:
    case rtmsg::ClientAppList: {
        std::span<const std::uint8_t> body = p;
        if (base != rtmsg::ClientAppToServer) {
            if (body.size() < 2) return;
            body = body.subspan(2);
        }
        if (body.size() < 2) {
            out.log.push_back(std::string(head) + ": app message without its channel / type");
            return;
        }
        if (!connected_) out.log.push_back(std::string(head) + ": app message before the connect");
        Frame f;
        if (body[0] == rtmsg::kMediusChannel) {
            f.type = body[1];
        } else if (body[0] == rtmsg::kMediusClass4Channel && body[1] < mid::kClass4Count) {
            f.type = static_cast<std::uint16_t>(mid::kClass4Base + body[1]);  // 134on4: class-4 entry -> its id
        } else if (body[0] == rtmsg::kMgclChannel && body[1] < mid::kMgclCount) {
            f.type = static_cast<std::uint16_t>(mid::kMgclBase + body[1]);  // 134on4 batch 3: MGCL (class 3)
        } else {
            out.log.push_back(std::string(head) + ": app channel " + std::to_string(body[0]) + " type " +
                              std::to_string(body[1]) + " (no Medius registration), " + hex_preview(body.subspan(2)));
            return;
        }
        f.body.assign(body.begin() + 2, body.end());
        out.medius.push_back(std::move(f));
        return;
    }
    default:
        out.log.push_back(std::string(head) + ": not handled, " + hex_preview(p));
        return;
    }
}

std::vector<std::uint8_t> RtSession::from_server_frame(const Frame& f, std::string* note) {
    if (f.type == rt::SystemMessage) return rt_message(rtmsg::ServerSystemMessage, f.body);
    if (f.type >= 0xF000) return {};
    std::uint8_t channel = rtmsg::kMediusChannel;
    std::uint16_t sub = f.type;
    if (f.type >= mid::kClass4Base && f.type < mid::kClass4Base + mid::kClass4Count) {
        channel = rtmsg::kMediusClass4Channel;  // 134on4: the class-4 registrations (contacts 259 / 262, ...)
        sub = static_cast<std::uint16_t>(f.type - mid::kClass4Base);
    } else if (f.type >= mid::kMgclBase && f.type < mid::kMgclBase + mid::kMgclCount) {
        channel = rtmsg::kMgclChannel;  // 134on4 batch 3: the MGCL answers and the host's JoinGame request
        sub = static_cast<std::uint16_t>(f.type - mid::kMgclBase);
    } else if (f.type >= mid::kClass4Base) {
        if (note) *note = "Medius id " + std::to_string(f.type) + " is no registration of RegisterPackets; not sent";
        return {};
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(2 + f.body.size());
    payload.push_back(channel);
    payload.push_back(static_cast<std::uint8_t>(sub));
    payload.insert(payload.end(), f.body.begin(), f.body.end());
    if (note && payload.size() >= 0x1ff)
        *note = "Medius id " + std::to_string(f.type) + " is " + std::to_string(payload.size()) +
                " bytes: sent whole (the server side of the client's 0x1ea-byte split is not decoded) [U]";
    return rt_message(rtmsg::ServerApp, payload);
}

}  // namespace omega::online
