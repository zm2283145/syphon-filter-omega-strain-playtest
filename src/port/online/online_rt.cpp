// Checkpoint 133on1: see online_rt.hpp (research/ONLINE.md §8.4).
#include "online_rt.hpp"

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
        const std::size_t len = static_cast<std::size_t>(buffer_[at + 1]) | static_cast<std::size_t>(buffer_[at + 2]) << 8;
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

void RtSession::message(std::uint8_t type, std::span<const std::uint8_t> p, Output& out) {
    const std::uint8_t base = type & 0x7f;
    char head[64];
    std::snprintf(head, sizeof head, "rt: type 0x%02x, %zu byte(s)", type, p.size());
    if (type & rtmsg::kEncrypted) {
        out.log.push_back(std::string(head) + ": encrypted (the client runs with security on: apply the "
                                              "DISABLE_SECURITY patch, ONLINE.md 8.3); dropped");
        return;
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
        const auto a = rt_message(rtmsg::ServerConnectAccept, accept);
        out.to_client.insert(out.to_client.end(), a.begin(), a.end());
        const std::uint8_t done[2] = {1, 0};
        const auto c = rt_message(rtmsg::ServerConnectComplete, done);
        out.to_client.insert(out.to_client.end(), c.begin(), c.end());
        out.log.push_back("rt: connect accepted (type 7, client index " + std::to_string(index_) +
                          ") and completed (type 26)");
        connected_ = true;
        out.connected = true;
        return;
    }
    case rtmsg::ClientCryptKeyPublic:
        out.log.push_back(std::string(head) + ": the client's RSA public key (it holds an encryption key: the "
                                              "DISABLE_SECURITY patch is not applied); omega_server has no RT "
                                              "encryption, closing");
        out.close = true;
        return;
    case rtmsg::ClientConnectUdp:
        out.log.push_back(std::string(head) + ": connect (UDP kind) on the TCP stream; ignored");
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
        if (body[0] != rtmsg::kMediusChannel) {
            out.log.push_back(std::string(head) + ": app channel " + std::to_string(body[0]) + " type " +
                              std::to_string(body[1]) + " (not Medius), " + hex_preview(body.subspan(2)));
            return;
        }
        Frame f;
        f.type = body[1];
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
    if (f.type > 0xff) {
        if (note) *note = "Medius id " + std::to_string(f.type) + " does not fit the Net sub-type byte [U]; not sent";
        return {};
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(2 + f.body.size());
    payload.push_back(rtmsg::kMediusChannel);
    payload.push_back(static_cast<std::uint8_t>(f.type));
    payload.insert(payload.end(), f.body.begin(), f.body.end());
    if (note && payload.size() >= 0x1ff)
        *note = "Medius id " + std::to_string(f.type) + " is " + std::to_string(payload.size()) +
                " bytes: sent whole (the server side of the client's 0x1ea-byte split is not decoded) [U]";
    return rt_message(rtmsg::ServerApp, payload);
}

}  // namespace omega::online
