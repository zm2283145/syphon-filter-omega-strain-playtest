#include "play_level_teardown.hpp"

#include "level_load.hpp"
#include "play_native_flow.hpp"
#include "teardown_state.hpp"
#include "video_frame.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace omega {
namespace {
constexpr std::uint32_t game = 0x4ffb50;
constexpr std::uint32_t video = 0x5381f0;
constexpr std::uint32_t app = 0x1d00000;       // $s0 of 00167e60 in the mirror (the goldens' app)
constexpr std::uint32_t stack = 0x1f00000;
constexpr std::uint32_t token_g = 0x0a000000;  // G's object fields: token_g + offset
constexpr std::uint32_t token_service = 0x0b000000;
constexpr std::uint32_t token_heap = 0x0c000000;
constexpr std::uint32_t component_vtable = token_service + 0x100;
constexpr std::uint32_t component_dtor = token_service + 0x1f8;   // its +8 slot (class not labelled)
constexpr std::uint32_t renderer_file = 0x4bc200;

std::string hex8(std::uint32_t v) {
    char t[9];
    std::snprintf(t, sizeof t, "%08x", v);
    return t;
}

struct Disposition {
    const char* kind;
    const char* role;
};

// Every service the teardown natives reach on the solo and MP paths, by address.
const std::map<std::uint32_t, Disposition>& dispositions() {
    static const std::map<std::uint32_t, Disposition> table{
        // World_Teardown 00134640
        {0x2569c0, {"pass", "G teardown head (role not labelled)"}},
        {0x3caa50, {"mirror", "G component lookup"}},
        {0x1c9680, {"pass", "level world service, first release (role not labelled)"}},
        {0x225cd0, {"mirror", "the objective world"}},
        {0x2261c0, {"pass", "objective world teardown (the pass's objective state)"}},
        {0x279d80, {"pass", "G+ec object destructor"}},
        {0x36b130, {"ps2", "allocator free"}},
        {0x10dc40, {"ps2", "_DisableIntc"}},
        {0x10dca8, {"ps2", "_EnableIntc"}},
        {0x26bd60, {"port", "pad vibration stop"}},
        {0x26be70, {"port", "pad vibration enable"}},
        {0x272920, {"port", "tremor list free (G+8c)"}},
        {0x2609e0, {"port", "vision effect mode (G+e0)"}},
        {0x260990, {"port", "vision effect flair (G+e0)"}},
        {0x25fbb0, {"port", "vision effect prepare (G+e0)"}},
        {0x2b2f80, {"pass", "water ripple emitter clear (G+e4)"}},
        {0x275550, {"pass", "role not labelled"}},
        {0x238f20, {"pass", "role not labelled"}},
        {0x3e2150, {"pass", "script cleanup (00555070; the mission script host)"}},
        {0x2683f0, {"pass", "role not labelled"}},
        {0x16aa80, {"pass", "team relations teardown"}},
        {0x3fdbb0, {"pass", "role not labelled"}},
        {0x136bd0, {"pass", "G release (role not labelled)"}},
        {0x1784b0, {"pass", "G+84 release (role not labelled)"}},
        {0x138c60, {"pass", "range erase (G+9c, G+c4)"}},
        {0x170d30, {"pass", "G+80 release (role not labelled)"}},
        {0x170570, {"pass", "role not labelled"}},
        {0x22d3c0, {"pass", "004f77c0 release (role not labelled)"}},
        {0x408ea0, {"pass", "0053bb90 release (role not labelled)"}},
        {0x3ec3e0, {"pass", "effects: release all"}},
        {0x136cd0, {"pass", "G release (role not labelled)"}},
        {0x257150, {"pass", "role not labelled"}},
        {0x42e6d0, {"network", "cNetObjectMgr_Reset"}},
        {0x3e56f0, {"network", "cCharActionMsg_DestinationParams_ClearPrevMsg"}},
        {0x42b2d0, {"network", "cDME_Restart"}},
        {0x44e340, {"network", "SFOLobby_ExitGame"}},
        {0x42b1f0, {"network", "Net_CleanupGame"}},
        // Checkpoint 132f10: 00134110 is the mission debrief (G+1c5 clear: level-end kinds 0 and 2, a success or
        // the start menu's Quit): GuiEndGameObjectives "SUMMARY" -> GuiEndGameStatistics "SCORES" / "SCOREONE" ->
        // GuiEndGameAwards "AWARDS" -> GuiAutoSaver, in its own GUI loop (LEVEL_END.md §6g). Checkpoint 132f11:
        // the play's action runs it (play_main's debrief loop).
        {0x134110, {"port", "the mission debrief (00134110)"}},
        {0x3fbcd0, {"pass", "objective transport map cleanup"}},
        {0x4294c0, {"network", "cDME_GetLobby (null offline)"}},
        {0x42a770, {"network", "cDME_StartUpdateThread"}},
        {0x3d0fb0, {"pass", "003d1020 static object release"}},
        {0x224530, {"mirror", "zone object"}},
        {0x224140, {"pass", "zone object release"}},
        {0x130b10, {"pass", "G release (role not labelled)"}},
        {0x1c3b00, {"pass", "G+88 release (role not labelled)"}},
        {0x132a40, {"pass", "G release (role not labelled)"}},
        {0x145200, {"pass", "weapon resources (G+1e0)"}},
        {0x4702a0, {"pass", "item inventory resources"}},
        {0x1c9710, {"pass", "level world service, level unload"}},
        {0x13e070, {"pass", "G+d0 release (role not labelled)"}},
        {0x3e9c20, {"pass", "script scheduler flush"}},
        {0x40c5d0, {"pass", "objective transport destruction"}},
        {0x40b830, {"pass", "G+1f4 / G+1f8 object release"}},
        {0x3a3670, {"pass", "0053b540 release (role not labelled)"}},
        {0x16f4d0, {"pass", "role not labelled"}},
        {0x2458e0, {"pass", "G+dc object destructor"}},
        {0x1309a0, {"pass", "G release (role not labelled)"}},
        {0x130950, {"pass", "G release (role not labelled)"}},
        {0x3cab50, {"mirror", "G component object (key 004ea0b0)"}},
        {component_dtor, {"pass", "vtable +8 destructor of the G component 004ea0b0 (class not labelled)"}},
        {0x135020, {"pass", "role not labelled"}},
        {0x3c2a80, {"pass", "role not labelled"}},
        {0x1af7a0, {"pass", "role not labelled"}},
        {0x40f570, {"pass", "role not labelled"}},
        {0x374020, {"pass", "level lights: free all four pools"}},
        {0x236f80, {"pass", "role not labelled"}},
        {0x3d5750, {"pass", "00543790 release (role not labelled)"}},
        {0x404fd0, {"pass", "0055dda0 destructor (role not labelled)"}},
        {0x407be0, {"pass", "role not labelled"}},
        {0x3eba50, {"mirror", "async loader"}},
        {0x3eb750, {"pass", "async loader stop"}},
        {0x227c60, {"pass", "objective deletion"}},
        {0x42a680, {"network", "cDME_StopUpdateThread"}},
        {0x3c8c70, {"pass", "pending messages cleared"}},
        // 003f9470 (not reached with the empty archive list)
        {0x36cd80, {"pass", "archive close"}},
        {0x3f9a30, {"pass", "archive list erase"}},
        // 0037e320
        {0x375cd0, {"ps2", "DMA object 00537f80 flush"}},
        {0x3813a0, {"renderer", "texture slot release"}},
        {0x383020, {"renderer", "texture slot manager destructor (+1058)"}},
        {0x3d27e0, {"renderer", "listener list destructor (+105c, vtable 004e00a8 +8)"}},
        {0x37b0c0, {"renderer", "video object reset"}},
        // 0036b250
        {0x36bc10, {"ps2", "heap pool free"}},
    };
    return table;
}

class TeardownHost final : public PlayFlowHost {
public:
    TeardownHost(const PlayTeardownActions& actions, PlayTeardownReport& report,
                 const PlayTeardownNetwork* network)
        : actions_(actions), report_(report), network_(network) {}

    std::set<std::uint32_t> renderer_freed;
    std::size_t renderer_double{};

    std::uint32_t call(LevelLoadMemory& m, std::uint32_t address,
                       std::initializer_list<std::uint32_t> args) override {
        const auto& table = dispositions();
        const auto found = table.find(address);
        if (found == table.end()) {
            if (address == 0x134640 || address == 0x36c960 || address == 0x3f9470 || address == 0x37e320 ||
                address == 0x36b250 || address == 0x3d1020) {
                record(address, args, "native", native_role(address));
                return PlayFlowHost::call(m, address, args);
            }
            throw std::runtime_error("Play level teardown: no disposition for EE service " + hex8(address));
        }
        const auto& d = found->second;
        if (address == 0x4294c0 && !network_) {
            record(address, args, "mirror", "network manager (null offline)");
            return 0;
        }
        record(address, args, d.kind, d.role);
        if (std::string_view(d.kind) == "network") {
            if (!network_ || !network_->call)
                throw std::runtime_error("Play level teardown: missing multiplayer service " + hex8(address));
            return network_->call(m, address, args);
        }
        const auto* a = args.begin();
        switch (address) {
        case 0x26bd60:
            if (actions_.stop_pad_vibration) actions_.stop_pad_vibration();
            return 0;
        case 0x26be70:
            if (actions_.enable_pad_vibration) actions_.enable_pad_vibration(a[0]);
            return 0;
        case 0x272920:
            if (actions_.free_tremors) actions_.free_tremors();
            return 0;
        case 0x2609e0:
            if (actions_.vision_mode) actions_.vision_mode(a[1], a[2]);
            return 0;
        case 0x25fbb0:
            if (actions_.vision_prepare) actions_.vision_prepare();
            return 0;
        case 0x134110:
            if (actions_.debrief) actions_.debrief(a[0], a[1]);
            return 0;
        case 0x3caa50:
            return lookup(a[1]);
        case 0x225cd0: return token_service + 2;
        case 0x224530: return token_service + 4;
        case 0x3eba50: return token_service + 5;
        case 0x3cab50: return component_object;
        case 0x36b130:
            if (args.size() == 4 && a[2] == renderer_file) {
                ++report_.renderer_frees;
                if (!renderer_freed.insert(a[1]).second) ++renderer_double;
            } else {
                ++report_.world_frees;
            }
            return 0;
        default:
            return 0;
        }
    }

    std::uint32_t call_float(LevelLoadMemory&, std::uint32_t address, std::initializer_list<std::uint32_t> args,
                             std::initializer_list<std::uint32_t> float_bits) override {
        if (address != 0x260990 || float_bits.size() != 4)
            throw std::runtime_error("Play level teardown: no disposition for EE float-argument service " +
                                     hex8(address));
        record(address, args, "port", dispositions().at(address).role);
        std::array<float, 4> f{};
        std::size_t i = 0;
        for (const auto bits : float_bits) {
            std::uint32_t b = bits;
            std::memcpy(&f[i++], &b, sizeof b);
        }
        if (actions_.vision_flair) actions_.vision_flair(f);
        return 0;
    }

    std::uint32_t call_framed(LevelLoadMemory& m, std::uint32_t address, std::initializer_list<std::uint32_t> args,
                              std::uint32_t sp) override {
        record(address, args, "native", "app teardown (00166190; app+274 = 0: it ran at state 0x12)");
        return LevelLoadHost::call_framed(m, address, args, sp);
    }

    static constexpr std::uint32_t component_object = token_service + 0x10;

private:
    static const char* native_role(std::uint32_t address) {
        switch (address) {
        case 0x134640: return "World_Teardown";
        case 0x36c960: return "deferred-free pump";
        case 0x3f9470: return "archive release (G+3c)";
        case 0x37e320: return "renderer teardown (005381f0)";
        case 0x36b250: return "heap slot release";
        default: return "static instance";
        }
    }
    static std::uint32_t lookup(std::uint32_t key) {
        switch (key) {
        case 0x4f2cc0: return token_service + 1;
        case 0x4ea0b0: return token_service + 3;
        case 0x4f7578: return token_service + 6;
        default: throw std::runtime_error("Play level teardown: 003caa50 key " + hex8(key) + " not in the mirror");
        }
    }
    void record(std::uint32_t address, std::initializer_list<std::uint32_t> args, const char* kind,
                const char* role) {
        report_.calls.push_back({address, std::vector<std::uint32_t>(args), kind, role});
    }
    const PlayTeardownActions& actions_;
    PlayTeardownReport& report_;
    const PlayTeardownNetwork* network_;
};

// The level load's renderer init on the mirror: allocations answer tokens.
class RendererInitHost final : public LevelLoadHost {
public:
    std::set<std::uint32_t> allocations;
    std::uint32_t call(LevelLoadMemory&, std::uint32_t address, std::initializer_list<std::uint32_t> args) override {
        switch (address) {
        case 0x36b180: {
            const auto size = args.begin()[1];
            const auto p = next_;
            next_ += (size + 0xff) & ~0xffU;
            allocations.insert(p);
            return p;
        }
        case 0x3831d0: return args.begin()[0];
        case 0x103710: return 1;
        case 0x102a28: case 0x102888: case 0x10d3e0: case 0x1037a8: case 0x3bb1c0: case 0x379fe0:
            return 0;
        default:
            throw std::runtime_error("Play level teardown: the renderer mirror's 0037e780 reached " + hex8(address));
        }
    }
private:
    std::uint32_t next_ = token_heap;
};
}  // namespace

std::size_t PlayTeardownReport::count(const std::string& kind) const {
    std::size_t n = 0;
    for (const auto& c : calls) n += c.kind == kind;
    return n;
}

PlayTeardownReport run_play_level_teardown(const GameEndState& g, const PlayTeardownActions& actions,
                                          const PlayTeardownNetwork* network) {
    if (g.state_50 != 0x14) throw std::runtime_error("Play level teardown: G+50 is not 0x14");
    if (network && (!network->multiplayer_5721c8 || !network->call))
        throw std::runtime_error("Play level teardown: multiplayer network service is incomplete");
    if (network && !g.b1c5 && !actions.debrief)
        throw std::runtime_error("Play level teardown: multiplayer debrief service is missing");
    PlayTeardownReport report;
    LevelLoadMemory m;
    const auto w8 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 1, v); };
    const auto w32 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 4, v); };
    w8(game + 0x50, g.state_50);
    w8(game + 0x51, g.previous_51);
    w8(game + 0x1c4, g.b1c4);
    w8(game + 0x1c5, g.b1c5);
    w8(game + 0x1c6, g.b1c6);
    w8(game + 0x1c7, g.b1c7);
    w8(0x5721c8, network ? network->multiplayer_5721c8 : 0);
    w8(0x5721d8, network ? network->quit_latch_5721d8 : 0);
    w8(0x5723b0, network ? network->popup_kind_5723b0 : 0);
    w8(0x5721b8, network ? network->network_5721b8 : 0);
    w8(0x533880, 0);
    w32(0x533888, 0);
    w32(app + 0x274, 0);
    for (const std::uint32_t f : {0x80U, 0x84U, 0x88U, 0x8cU, 0xa4U, 0xccU, 0xd0U, 0xdcU, 0xe0U, 0xe4U, 0xe8U, 0xecU,
                                  0x1e0U, 0x1f4U, 0x1f8U})
        w32(game + f, token_g + f);
    w32(game + 0x3c + 4 + 4, game + 0x3c + 4);   // the archive list: sentinel's next is the sentinel
    w32(0x4ea0b8, 0);
    w32(TeardownHost::component_object, component_vtable);
    w32(component_vtable + 8, component_dtor);
    w32(0x533c90, 0);
    for (const std::uint32_t slot : {1U, 3U}) {
        const auto base = 0x5344b4 + 0x818 * slot;
        w32(base + 0x0, token_heap - 0x1000 * (slot + 1));
        w32(base + 0x10, 0);
    }
    // The ELF's listener-list vtable word 004e00b0 (+8 of 004e00a8): 003d27e0.
    w32(0x4e00b0, 0x3d27e0);

    w32(video, 0xffffffffU);
    RendererInitHost init;
    renderer_init_0037e780(m, init, video, 640, 448, 0x20, 0x10, 0, 0x157c, 0xffffffffU);
    report.renderer_allocations = init.allocations.size();

    TeardownHost host(actions, report, network);
    run_teardown_state(m, host, app, stack);
    report.state_50 = static_cast<std::uint8_t>(m.read(game + 0x50, 1));
    report.previous_51 = static_cast<std::uint8_t>(m.read(game + 0x51, 1));
    report.multiplayer_after = static_cast<std::uint8_t>(m.read(0x5721c8, 1));
    report.quit_latch_after = static_cast<std::uint8_t>(m.read(0x5721d8, 1));
    report.network_after = static_cast<std::uint8_t>(m.read(0x5721b8, 1));
    report.renderer_balanced = host.renderer_double == 0 && host.renderer_freed == init.allocations;
    if (m.read(video) != 0xffffffffU) throw std::runtime_error("Play level teardown: 0037e320 left the video object live");
    return report;
}

}  // namespace omega
