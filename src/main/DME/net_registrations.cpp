#include "net_registrations.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
using u32 = std::uint32_t;

enum class RegOp { Call, Bytes, Copy };
enum class RegArg { Const, Frame, Id };
struct RegArgSpec {
    RegArg kind;
    std::int64_t value;
};
struct RegEvent {
    RegOp op;
    u32 api;
    u32 nargs;
    RegArgSpec args[6];
    const char* input;  // Call: the bytes the DME reads through the frame pointer argument (written before the call)
    u32 input_len;
    u32 addr;  // Bytes / Copy: the global written
    u32 k;     // Copy: the call whose returned id is written
};
enum class RegFail { Return, ReturnError, Ignore };
struct RegFailure {
    RegFail kind;
    u32 ret;
    u32 begin, end;  // kRegFailEvents
};
struct RegFunction {
    u32 fn;
    u32 ev_begin, ev_end;  // kRegEvents
    u32 success_ret;
    u32 fail_index;  // kRegFailures: one row per Call event, in order
};
struct RegSeed {
    u32 addr;
    const char* bytes;
    u32 len;
};

#include "net_registrations.inl"

const RegFunction* find(u32 fn) {
    for (const auto& f : kRegFunctions)
        if (f.fn == fn) return &f;
    return nullptr;
}

bool returns_id(u32 api) { return api == 0x2e9be8 || api == 0x2e9cd0 || api == 0x2e9e50 || api == 0x2e9f08; }

// The argument the DME reads `input` through: 002e9be8's option block (a5), 002e9cd0's class name (a1).
unsigned input_arg(u32 api) {
    if (api == 0x2e9be8) return 5;
    if (api == 0x2e9cd0) return 1;
    throw std::runtime_error("net registrations: input bytes for an API that reads none");
}

struct Replay {
    LevelLoadMemory& m;
    LevelLoadHost& host;
    u32 sp;
    u32 ids[64] = {};

    u32 arg(const RegArgSpec& a) const {
        switch (a.kind) {
        case RegArg::Const: return static_cast<u32>(a.value);
        case RegArg::Frame: return sp + static_cast<u32>(static_cast<std::int32_t>(a.value));
        case RegArg::Id: return ids[a.value];
        }
        return 0;
    }

    void write_event(const RegEvent& e) {
        if (e.op == RegOp::Bytes) {
            for (u32 k = 0; k < e.input_len; ++k) m.write(e.addr + k, 1, static_cast<unsigned char>(e.input[k]));
        } else {
            m.write(e.addr, 4, ids[e.k]);
        }
    }

    u32 call(const RegEvent& e, unsigned index) {
        u32 a[6];
        for (u32 k = 0; k < 6; ++k) a[k] = arg(e.args[k]);
        if (e.input) {
            const auto p = a[input_arg(e.api)];
            for (u32 k = 0; k < e.input_len; ++k) m.write(p + k, 1, static_cast<unsigned char>(e.input[k]));
        }
        u32 r = 0;
        switch (e.nargs) {
        case 0: r = host.call(m, e.api, {}); break;
        case 1: r = host.call(m, e.api, {a[0]}); break;
        case 2: r = host.call(m, e.api, {a[0], a[1]}); break;
        case 3: r = host.call(m, e.api, {a[0], a[1], a[2]}); break;
        case 4: r = host.call(m, e.api, {a[0], a[1], a[2], a[3]}); break;
        case 5: r = host.call(m, e.api, {a[0], a[1], a[2], a[3], a[4]}); break;
        default: r = host.call(m, e.api, {a[0], a[1], a[2], a[3], a[4], a[5]}); break;
        }
        if (index >= 64) throw std::runtime_error("net registrations: more than 64 calls in one function");
        if (returns_id(e.api)) ids[index] = m.read(a[0]);
        return r;
    }

    u32 run(const RegFunction& f) {
        unsigned index = 0;
        for (u32 i = f.ev_begin; i < f.ev_end; ++i) {
            const auto& e = kRegEvents[i];
            if (e.op != RegOp::Call) {
                write_event(e);
                continue;
            }
            const auto r = call(e, index);
            const auto& fail = kRegFailures[f.fail_index + index];
            ++index;
            if (!r || fail.kind == RegFail::Ignore) continue;
            for (u32 j = fail.begin; j < fail.end; ++j) write_event(kRegFailEvents[j]);
            return fail.kind == RegFail::Return ? fail.ret : r;
        }
        return f.success_ret;
    }
};
}  // namespace

bool net_registration_has(std::uint32_t function) { return find(function) != nullptr; }

std::uint32_t net_registration_call(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t function,
                                    std::uint32_t stack) {
    const auto* f = find(function);
    if (!f) {
        char text[96];
        std::snprintf(text, sizeof text, "net_registration_call(0x%08x): not a registration function", function);
        throw std::runtime_error(text);
    }
    Replay replay{m, host, stack};
    return replay.run(*f);
}

void net_registry_image(LevelLoadMemory& m) {
    m.write(0x497e50, 4, 0x28ab);
    m.copy(0x48c4cd, "1.32.0089");
    for (const auto& s : kRegSeeds)
        for (u32 k = 0; k < s.len; ++k) m.write(s.addr + k, 1, static_cast<unsigned char>(s.bytes[k]));
}

void net_start_00165bc0(LevelLoadMemory& m, LevelLoadHost& host) {
    if (!m.read(0x4ee530, 1)) {
        m.write(0x4ee530, 1, 1);
        host.call(m, 0x3a6710, {1});
        if (!m.read(0x51eea8, 1)) {
            m.write(0x51eea0, 1, 1);
            host.call(m, 0x42af00, {});
        }
    }
    if (m.read(0x5723d0, 1)) return;
    for (const auto fn : kTopRegistrations) host.call(m, fn, {});
    host.call(m, 0x3fb250, {});
    m.write(0x5723d0, 1, 1);
}

void net_register_all_003fb250(LevelLoadMemory& m, LevelLoadHost& host) {
    for (const auto fn : kRegisterAllRegistrations) host.call(m, fn, {});
    m.write(0x55c7b8, 4, host.call(m, 0x42b1a0, {0}));
}

}  // namespace omega
