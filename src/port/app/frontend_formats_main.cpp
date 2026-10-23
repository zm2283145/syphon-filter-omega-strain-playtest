// omega_frontend_formats: decodes .GUI / .IE / .FNT files with the native
// readers and prints the canonical dump that tools/frontend_formats.py canon
// prints, so tools/verify_frontend_formats.py can compare the two field by
// field over a whole extracted corpus.
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "award_registry.hpp"
#include "awards_agent_host.hpp"
#include "disc.hpp"
#include "frontend_formats.hpp"
#include "gui_draw.hpp"
#include "gui_renderer.hpp"
#include "gui_runtime.hpp"
#include "loading_screen_host.hpp"
#include "localization.hpp"
#include "parts_table.hpp"
#include "agent_save.hpp"
#include "profile_awards.hpp"
#include "script.hpp"
#include "string_pool.hpp"
#include "weapon_database.hpp"

namespace {
using namespace omega;

std::string quoted(const std::string& v) {
    std::string out = "\"";
    for (const unsigned char ch : v) {
        if (ch >= 0x21 && ch <= 0x7e && ch != '"' && ch != '\\') out += static_cast<char>(ch);
        else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", ch);
            out += buf;
        }
    }
    return out + "\"";
}

template <typename Range>
std::string floats(const Range& values) {
    std::string out;
    for (const float f : values) {
        std::uint32_t bits;
        std::memcpy(&bits, &f, sizeof bits);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", bits);
        if (!out.empty()) out += ' ';
        out += buf;
    }
    return out;
}

template <typename List>
std::string float_list(const List& list) {
    std::string out;
    for (const auto& item : list) {
        if (!out.empty()) out += ' ';
        out += floats(item);
    }
    return out;
}

void dump_gui_node(const GuiNode& w, int depth) {
    std::string line = "N " + std::to_string(depth) + ' ' + quoted(w.class_name) + ' ' + quoted(w.decorator_class) + ' ' +
                       quoted(w.name) + ' ' + floats(w.rect) + ' ' + std::to_string(w.flag2) + ' ' + std::to_string(w.flag4);
    if (w.text)
        line += " T " + quoted(w.text->text) + ' ' + quoted(w.text->font) + ' ' + floats(w.text->rgb) + ' ' +
                std::to_string(w.text->halign);
    if (w.edit) line += " E " + std::to_string(w.edit->max_len) + ' ' + std::to_string(w.edit->password);
    if (w.decorator) {
        const auto& d = *w.decorator;
        line += " D " + quoted(d.ie) + ' ' + quoted(d.node) + ' ' + floats(d.matrix) + ' ' + std::to_string(d.events.size());
        for (const auto& e : d.events)
            line += " e " + quoted(e.name) + ' ' + std::to_string(e.action) + ' ' + std::to_string(e.a) + ' ' + std::to_string(e.b);
    }
    line += " C " + std::to_string(w.children.size());
    std::cout << line << '\n';
    for (const auto& child : w.children) dump_gui_node(child, depth + 1);
}

const char* track_name(IeTrackKind kind) {
    switch (kind) {
    case IeTrackKind::vertex: return "VERTEX";
    case IeTrackKind::diffuse: return "DIFFUSE";
    case IeTrackKind::opacity: return "OPACITY";
    case IeTrackKind::rot_x: return "ROT_X";
    case IeTrackKind::rot_y: return "ROT_Y";
    case IeTrackKind::rot_z: return "ROT_Z";
    case IeTrackKind::uv_off_u: return "UVOFF_U";
    case IeTrackKind::uv_off_v: return "UVOFF_V";
    case IeTrackKind::uv_scl_u: return "UVSCL_U";
    case IeTrackKind::uv_scl_v: return "UVSCL_V";
    }
    return "?";
}

void dump_ie_node(const IeNode& n, int depth) {
    std::cout << "N " << depth << ' ' << quoted(n.name) << ' ' << quoted(n.texture) << ' ' << floats(n.matrix) << '\n';
    std::cout << "P " << n.positions.size() << ' ' << float_list(n.positions) << '\n';
    std::cout << "U " << n.uvs.size() << ' ' << float_list(n.uvs) << '\n';
    std::cout << "K " << n.colors.size() << ' ' << float_list(n.colors) << '\n';
    std::string tri;
    for (const auto& t : n.triangles)
        for (const auto w : t) {
            if (!tri.empty()) tri += ' ';
            tri += std::to_string(w);
        }
    std::cout << "Y " << n.triangles.size() << ' ' << tri << '\n';
    for (const auto& t : n.tracks) {
        if (t.kind == IeTrackKind::vertex) {
            std::string keys;
            for (const auto& k : t.vertex) {
                if (!keys.empty()) keys += ' ';
                keys += std::to_string(k.size()) + ' ' + float_list(k);
            }
            std::cout << "R VERTEX " << t.vertex.size() << ' ' << keys << '\n';
        } else if (t.kind == IeTrackKind::diffuse) {
            std::cout << "R DIFFUSE " << t.diffuse.size() << ' ' << float_list(t.diffuse) << '\n';
        } else {
            std::cout << "R " << track_name(t.kind) << ' ' << t.scalar.size() << ' ' << float_list(t.scalar) << '\n';
        }
    }
    for (const auto& x : n.text)
        std::cout << "X " << (x.next ? "NEXT" : "TEXT") << ' ' << x.points.size() << ' ' << float_list(x.points) << '\n';
    std::cout << "C " << n.children.size() << '\n';
    for (const auto& child : n.children) dump_ie_node(child, depth + 1);
}

void dump_fnt(const FontFile& f) {
    std::cout << "F " << int(f.version) << ' ' << int(f.sub) << ' ' << quoted(f.texture) << ' ' << int(f.cell_w) << ' '
              << int(f.height) << ' ' << f.count << ' ' << int(f.space) << ' ' << (f.kerning ? 1 : 0) << ' ' << f.consumed << '\n';
    std::string g;
    for (const auto& glyph : f.glyphs) {
        if (!g.empty()) g += ' ';
        g += floats(std::array<float, 4>{glyph.u0, glyph.u1, glyph.v0, glyph.v1});
    }
    std::cout << "g " << g << '\n';
    std::string k;
    for (const auto v : f.kern) {
        if (!k.empty()) k += ' ';
        k += std::to_string(int(v));
    }
    std::cout << "k " << k << '\n';
}

std::string upper_extension(const std::filesystem::path& p) {
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return e;
}

std::string json_text(const std::string& v) {
    std::string out = "\"";
    for (const unsigned char ch : v) {
        if (ch >= 0x20 && ch < 0x7f && ch != '"' && ch != '\\') out += static_cast<char>(ch);
        else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", ch);
            out += buf;
        }
    }
    return out + "\"";
}
std::string json_opt(const AwardsText& v) { return v ? json_text(*v) : "null"; }

int dump_awards(const std::filesystem::path& iso) {
    std::ifstream stream(iso, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + iso.string());
    DiscImage disc(stream, std::filesystem::file_size(iso));
    const auto awards = parse_script(disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "AWARDS.SO"));
    const auto pool = decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"})));
    const auto db = WeaponDatabase::load(disc.read(disc.file({"GAMEDATA", "COMMON", "WEAPONS.WDB"})));
    std::vector<std::string> weapons;
    for (const auto& r : db.records()) weapons.push_back(r.display_name_80());
    const auto calls = read_awards_init(awards);
    const auto r = build_award_registries(pool, weapons, calls);
    const auto text = [&](std::int32_t id) {
        return id < 0 ? std::string("null") : json_text(pool.entries().at(static_cast<std::size_t>(id)).text);
    };
    auto& o = std::cout;
    o << "{\"calls\": " << calls.size() << ", \"bit_counter\": " << r.bit_counter << ", \"objective_counter\": "
      << r.objective_counter << ",\n\"solo\": [";
    for (std::size_t k = 0; k < 8; ++k) o << (k ? ", " : "") << r.solo[k];
    o << "], \"multi\": [";
    for (std::size_t k = 0; k < 8; ++k) o << (k ? ", " : "") << r.multi[k];
    o << "],\n\"unlocks\": [";
    for (std::size_t i = 0; i < r.unlocks.size(); ++i) {
        const auto& u = r.unlocks[i];
        o << (i ? ",\n " : "") << "{\"id\": " << u.key << ", \"name\": " << text(u.key) << ", \"bit\": " << u.bit
          << ", \"weapon\": " << u.weapon << "}";
    }
    o << "],\n\"objectives\": [";
    for (std::size_t i = 0; i < r.objectives.size(); ++i) {
        const auto& x = r.objectives[i];
        o << (i ? ",\n " : "") << "{\"id\": " << x.key << ", \"name\": " << text(x.key) << ", \"idx\": " << x.index
          << ", \"points\": " << x.points << "}";
    }
    const char* lists[] = {"SpecialRating", "Commendation", "Medal", "BonusLevel", "Omega"};
    for (std::size_t l = 0; l < 5; ++l) {
        o << "],\n\"" << lists[l] << "\": [";
        for (std::size_t i = 0; i < r.awards[l].size(); ++i) {
            const auto& x = r.awards[l][i];
            o << (i ? ",\n " : "") << "{\"id\": " << x.key << ", \"name\": " << text(x.key) << ", \"text_id\": "
              << x.requirement << ", \"arg_c\": " << x.count << ", \"unlock\": " << x.unlock << ", \"points\": " << x.points << "}";
        }
    }
    o << "],\n\"ratings\": [";
    for (std::size_t i = 0; i < r.ratings.size(); ++i) {
        const auto& x = r.ratings[i];
        o << (i ? ",\n " : "") << "{\"id\": " << x.key << ", \"name\": " << text(x.key) << ", \"points\": " << x.points
          << ", \"stat\": " << int(x.stat) << ", \"arg3\": " << x.low << ", \"threshold\": " << x.threshold
          << ", \"unlock\": " << x.unlock << "}";
    }
    o << "],\n\"levels\": [";
    for (std::size_t i = 0; i < r.levels.size(); ++i) {
        const auto& x = r.levels[i];
        o << (i ? ",\n " : "") << "{\"id\": " << x.key << ", \"name\": " << text(x.key) << ", \"bit\": " << x.bit
          << ", \"arg3\": " << x.mission << ", \"pair\": [" << x.pair_first << ", " << x.pair_second << "]}";
    }
    for (const auto* table : {&r.ranks, &r.dossiers}) {
        o << "],\n\"" << (table == &r.ranks ? "ranks" : "dossiers") << "\": [";
        for (std::size_t i = 0; i < table->size(); ++i) {
            const auto& x = (*table)[i];
            std::string t;
            if (x.text) t.assign(x.text->begin(), std::find(x.text->begin(), x.text->end(), '\0'));
            o << (i ? ",\n " : "") << "{\"name\": " << json_opt(x.name) << ", \"index\": " << x.index << ", \"threshold\": "
              << x.threshold << ", \"unlock\": " << x.unlock << ", \"text\": " << json_text(t) << ", \"rank_id\": "
              << x.title_key << "}";
        }
    }
    o << "],\n\"omega_requirements\": [";
    for (std::size_t i = 0; i < r.omega_requirements.size(); ++i) {
        const auto& x = r.omega_requirements[i];
        o << (i ? ",\n " : "") << "{\"name\": " << text(x.text_key) << ", \"str4\": " << json_opt(x.name) << ", \"c\": "
          << x.a << ", \"d\": " << x.b << ", \"e\": " << x.c << ", \"unlock\": " << x.unlock << "}";
    }
    o << "]}\n";
    return 0;
}

int dump_parts(const std::filesystem::path& iso) {
    std::ifstream stream(iso, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + iso.string());
    DiscImage disc(stream, std::filesystem::file_size(iso));
    const auto rows = parse_parts_prn(disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "PARTS.PRN"));
    std::cout << "[";
    for (std::size_t i = 0; i < rows.size(); ++i)
        std::cout << (i ? ",\n " : "") << "[" << json_text(rows[i].name) << ", " << rows[i].value << "]";
    std::cout << "]\n";
    return 0;
}

std::vector<std::uint8_t> unhex(const std::string& h) {
    if (h == "-") return {};
    if (h.size() % 2) throw std::runtime_error("odd hex length");
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < h.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoul(h.substr(i, 2), nullptr, 16)));
    return out;
}
std::string hex(std::span<const std::uint8_t> b) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (const auto x : b) out += {d[x >> 4], d[x & 15]};
    return out.empty() ? "-" : out;
}

// awards::SetAwardsAndBonuses over an agent state dumped from the EE
// (tools/verify_set_awards.py): lines "agent <hex>", "element <list> <flag>
// <hex>", "awardbits <hex>", "record <hex>", "part <row>". Prints the state after the call in
// the same form, plus "missing <text>" per part name not in the table.
//
// With `op` lines the state runs through the profile create / refill instead
// (tools/verify_profile_awards.py): "op create <slot> <nosave> <busy>" is
// 00333f60, "op refill <mark> <recount>" is 00335e50, both with the
// SetAwardsAndBonuses host of profile_refill_host; "levelcount <n>" is
// *00532af4, "tail <hex> <hex>" the reset's unwritten fill-record words and
// "rankbase <hex>" *(00532b50 + 8), the rank rows +b50 points into. "op load
// <slot>" is load_profile (00334790 with its refill) over "buffer <hex>" (the
// global buffer 0052afd0 before the load), "file <hex>" (what the card read
// 0040df00 copies into it), "name <hex>" (the name argument), "card <busy>
// <present> <formatted> <font> <glyphs> <id>", "appearance <hex>",
// "messages <hex>", "msgcount <n>", "scratch <hex>" and "lastcard <hex>".
// Adds "result <n>" (create, load), "rank <k>|-" (refill), and for the load
// "loadname", "appearance", "messages", "msgcount", "scratch", "lastcard" and
// "buffer".
struct DumpedState {
    std::vector<std::uint8_t> agent;
    AgentLoadState state;
    std::vector<std::string> op;
    std::uint32_t rank_base = 0;
    std::vector<std::uint8_t> buffer, file, name;
    AgentCardState card;
};

DumpedState read_state(const std::filesystem::path& state_path) {
    std::ifstream in(state_path);
    if (!in) throw std::runtime_error("cannot open " + state_path.string());
    DumpedState d;
    auto& state = d.state;
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string tag;
        ls >> tag;
        if (tag == "agent") {
            std::string h;
            ls >> h;
            d.agent = unhex(h);
        } else if (tag == "element") {
            std::size_t list = 0;
            int flag = 0;
            std::string h;
            ls >> list >> flag >> h;
            state.awards.at(list).push_back({static_cast<std::uint8_t>(flag), unhex(h)});
        } else if (tag == "awardbits") {
            std::string h;
            ls >> h;
            state.award_bits = unhex(h);
        } else if (tag == "record") {
            std::string h;
            ls >> h;
            const auto b = unhex(h);
            AgentLevelRecord rec{};
            if (b.size() != rec.size()) throw std::runtime_error("a level record is not 0x20 bytes");
            std::copy(b.begin(), b.end(), rec.begin());
            state.level_records.push_back(rec);
        } else if (tag == "part") {
            std::uint32_t row = 0;
            ls >> row;
            state.parts.push_back(row);
        } else if (tag == "levelcount") {
            ls >> state.level_count;
        } else if (tag == "tail") {
            ls >> std::hex >> state.reset_record_tail[0] >> state.reset_record_tail[1];
        } else if (tag == "rankbase") {
            ls >> std::hex >> d.rank_base;
        } else if (tag == "buffer" || tag == "file" || tag == "name" || tag == "appearance" || tag == "messages" ||
                   tag == "scratch") {
            std::string h;
            ls >> h;
            auto b = unhex(h);
            if (tag == "buffer") d.buffer = b;
            else if (tag == "file") d.file = b;
            else if (tag == "name") d.name = b;
            else {
                auto copy_to = [&](auto& out) {
                    if (b.size() != out.size()) throw std::runtime_error(tag + ": wrong size");
                    std::copy(b.begin(), b.end(), out.begin());
                };
                if (tag == "appearance") copy_to(state.appearance);
                else if (tag == "messages") copy_to(state.messages);
                else copy_to(state.string_scratch);
            }
        } else if (tag == "card") {
            int busy = 0, present = 0, formatted = 0, font = 0;
            ls >> busy >> present >> formatted >> font >> d.card.glyph_count >> std::hex >> d.card.card_id;
            d.card.busy = busy != 0;
            d.card.card_present = present != 0;
            d.card.card_formatted = formatted != 0;
            d.card.font_ready = font != 0;
        } else if (tag == "msgcount") {
            ls >> state.message_count;
        } else if (tag == "lastcard") {
            ls >> std::hex >> state.last_card_id;
        } else if (tag == "op") {
            for (std::string w; ls >> w;) d.op.push_back(w);
        } else if (!tag.empty()) {
            throw std::runtime_error("unknown state line " + tag);
        }
    }
    if (d.agent.size() != agent_record_size) throw std::runtime_error("the agent is not 0xb78 bytes");
    return d;
}

std::uint32_t word_arg(const std::vector<std::string>& op, std::size_t i) {
    if (i >= op.size()) throw std::runtime_error("op: missing argument");
    return static_cast<std::uint32_t>(std::stoll(op[i], nullptr, 0));
}

int set_awards(const std::filesystem::path& iso, const std::filesystem::path& state_path) {
    std::ifstream stream(iso, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + iso.string());
    DiscImage disc(stream, std::filesystem::file_size(iso));
    const auto awards = load_profile_awards(disc);
    auto d = read_state(state_path);
    auto& state = d.state;
    const std::span<std::uint8_t, agent_record_size> agent(d.agent.data(), agent_record_size);
    std::vector<std::string> missing;
    std::string extra;
    if (d.op.empty()) {
        AwardsAgentContext context{agent, state, awards.registries, awards.tables, awards.pool, awards.parts, {}};
        run_set_awards_and_bonuses(awards.awards, context);
        missing = context.missing_parts;
    } else {
        const auto host = profile_refill_host(awards, [&d](std::size_t k) {
            return static_cast<std::uint32_t>(d.rank_base + 0x24 * k);
        }, &missing);
        if (d.op[0] == "create") {
            const auto result = create_agent(agent, state, awards.tables, static_cast<std::int32_t>(word_arg(d.op, 1)),
                word_arg(d.op, 2), word_arg(d.op, 3) != 0, host, nullptr);
            extra = "result " + std::to_string(result) + '\n';
        } else if (d.op[0] == "load") {
            if (d.buffer.size() != agent_save_buffer_size) throw std::runtime_error("load: the buffer is not 0x4e20 bytes");
            AgentSaveBuffer buffer{};
            std::copy(d.buffer.begin(), d.buffer.end(), buffer.begin());
            const auto r = load_profile(awards, host, buffer, agent, std::span<const std::uint8_t>(d.name),
                static_cast<std::int32_t>(word_arg(d.op, 1)), state, d.card,
                [&d](std::string_view, AgentSaveBuffer& b) {
                    if (d.file.size() > b.size()) throw std::runtime_error("load: the file overruns the buffer");
                    std::copy(d.file.begin(), d.file.end(), b.begin());
                    return true;
                });
            std::ostringstream o;
            o << "result " << r.result << "\nloadname " << hex({reinterpret_cast<const std::uint8_t*>(r.file_name.data()), r.file_name.size()})
              << "\nappearance " << hex(state.appearance) << "\nmessages " << hex(state.messages) << "\nmsgcount "
              << state.message_count << "\nscratch " << hex(state.string_scratch) << "\nlastcard " << std::hex
              << state.last_card_id << std::dec << "\nbuffer " << hex(buffer) << '\n';
            extra = o.str();
        } else if (d.op[0] == "refill") {
            const auto r = refill_agent(agent, state, awards.tables, word_arg(d.op, 1), word_arg(d.op, 2), host);
            extra = "rank " + (r.rank ? std::to_string(*r.rank) : std::string("-")) + '\n';
        } else {
            throw std::runtime_error("unknown op " + d.op[0]);
        }
    }
    std::cout << "agent " << hex(d.agent) << '\n';
    for (std::size_t list = 0; list < agent_award_lists; ++list)
        for (const auto& e : state.awards[list]) std::cout << "element " << list << ' ' << int(e.flag) << ' ' << hex(e.bits) << '\n';
    std::cout << "awardbits " << hex(state.award_bits) << '\n';
    for (const auto& r : state.level_records) std::cout << "record " << hex(r) << '\n';
    for (const auto r : state.parts) std::cout << "part " << r << '\n';
    for (const auto& m : missing) std::cout << "missing " << hex({reinterpret_cast<const std::uint8_t*>(m.data()), m.size()}) << '\n';
    std::cout << extra;
    return 0;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Steps the GUI runtime (research/FRONTEND.md, Checkpoint 118) over a script in
// the tools/ee/gen_gui_runtime.py format and prints the trace: the hosted
// service calls (text objects with their rects / fonts / colours, popups,
// sounds, screen factory) and the widget / canvas state after every frame.
int gui_run(const std::string& script, const std::vector<std::vector<std::uint8_t>>& guis) {
    std::vector<GuiFile> files;
    for (const auto& b : guis) files.push_back(decode_gui(b));
    for (const auto& line : gui::run_gui_script(gui::parse_gui_script(script), files)) std::cout << line << '\n';
    return 0;
}

// FRONTEND/NTSC.HOG > <screen>.HOG > <screen>.GUI straight from the disc.
std::vector<std::uint8_t> frontend_gui(DiscImage& disc, const std::string& screen) {
    const auto hog = disc.archive_member({"GAMEDATA", "FRONTEND", "NTSC.HOG"}, screen + ".HOG");
    if (hog.size() < 20) throw std::runtime_error("truncated " + screen + ".HOG");
    const std::uint32_t index_size = static_cast<std::uint32_t>(hog[16]) | static_cast<std::uint32_t>(hog[17]) << 8 |
                                     static_cast<std::uint32_t>(hog[18]) << 16 |
                                     static_cast<std::uint32_t>(hog[19]) << 24;
    if (index_size < 20 || index_size > hog.size()) throw std::runtime_error("invalid " + screen + ".HOG header");
    const Extent e = hog_member(std::span(hog).first(index_size), hog.size(), screen + ".GUI");
    return {hog.begin() + static_cast<std::ptrdiff_t>(e.offset),
            hog.begin() + static_cast<std::ptrdiff_t>(e.offset + e.size)};
}

// The front-end screens of a script straight from the disc: TITLESCR.GUI is
// file 0, POPUP.GUI (the application message box, Checkpoint 118b) file 1 and
// KEYBOARD.GUI (the on-screen keyboard GuiKeyboard, Checkpoint 118d) file 2 and
// AGENTOPN.GUI (the agent load screen GuiAgentLoad, Checkpoint 118e) file 3 and
// ONOFFSEL.GUI (GuiOnlineOfflineSelect, Checkpoint 118f) file 4 and CMDCENTR.GUI
// (GuiCommandCenter, Checkpoint 118g) file 5.
std::vector<std::vector<std::uint8_t>> title_screen_guis(const std::filesystem::path& iso) {
    std::ifstream stream(iso, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + iso.string());
    DiscImage disc(stream, std::filesystem::file_size(iso));
    return {frontend_gui(disc, "TITLESCR"), frontend_gui(disc, "POPUP"), frontend_gui(disc, "KEYBOARD"),
            frontend_gui(disc, "AGENTOPN"), frontend_gui(disc, "ONOFFSEL"),
            frontend_gui(disc, "CMDCENTR")};
}

// --gui-capture: a front-end screen under the native GUI runtime (its root
// class on layer 0, no pad; TITLESCR by default, or a hosted screen loaded as
// the script runs load a root: AGENTOPN / ONOFFSEL / CMDCENTR), `frames`
// updates at 1/60 s, then the canvas draw (gui_draw) through the host 2D
// backend into a 640 x 448 BMP.
int gui_capture(const std::filesystem::path& iso, const std::filesystem::path& out, int frames,
                const std::string& screen) {
    const std::pair<const char*, gui::WidgetClass> classes[] = {{"TITLESCR", gui::WidgetClass::title},
                                                                {"AGENTOPN", gui::WidgetClass::agentload},
                                                                {"ONOFFSEL", gui::WidgetClass::onoffsel},
                                                                {"CMDCENTR", gui::WidgetClass::cmdcentr}};
    const auto cls = std::find_if(std::begin(classes), std::end(classes),
                                  [&](const auto& c) { return screen == c.first; });
    if (cls == std::end(classes)) throw std::runtime_error("screen " + screen + " has no hosted root class");
    std::ifstream stream(iso, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + iso.string());
    DiscImage disc(stream, std::filesystem::file_size(iso));
    const GuiDiscFonts fonts = load_gui_fonts(disc);
    const Localization strings(decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"}))));
    const GuiFile title = decode_gui(frontend_gui(disc, screen));
    GuiStaticHost host(fonts, [&](const std::string& key) -> std::optional<std::string> {
        const auto s = strings.resolve(key);
        return s ? std::optional<std::string>(std::string(*s)) : std::nullopt;
    });
    gui::GuiRuntime rt(host);
    rt.g.card.present = 1;
    rt.g.app_loaded = 1;
    rt.g.pad_state = 6;
    // 00419110 stores the file's name at the manager's +2c before the load
    // (the IE of the decorators that name none).
    rt.g.iface = screen;
    gui::Widget* root = rt.load(title.root, cls->second);
    // The decorator elements (Checkpoint 122b, gui_decor): 004178e0 for the
    // loaded tree, then the host bridge around the runtime.
    GuiDecorAssets assets(disc);
    GuiDecor decor([&](const std::string& name) { return assets.resolve(name); }, 0);
    decor.attach(rt, *root, title.root);
    decor.bridge_install(rt);
    rt.push(root, 0);
    for (int i = 0; i < frames; ++i) {
        host.ms = i * 1000 / 60;
        gui::PadInput in;
        rt.translate_pads(in);
        decor.bridge_begin_update(rt);
        rt.update(1.0F / 60.0F);
        decor.bridge_step(rt, 1.0F / 60.0F);
    }
    for (const auto& r : host.recorded) std::cout << "GUI capture: recorded " << r << " (not applied)\n";
    GuiDrawer drawer([&](std::uint32_t h) { return fonts.font(h); });
    IeDrawState state;
    drawer.set_decor(&decor, &state);
    GuiDrawList list;
    drawer.draw_canvas(list, rt, host.ms);
    GuiRenderer renderer(640, 448);
    for (const auto& [slot, image] : fonts.images) renderer.set_texture(slot, image);
    for (const auto& [slot, image] : assets.images()) renderer.set_texture(slot, image);
    renderer.clear({0.0F, 0.0F, 0.0F, 1.0F});
    renderer.draw(list);
    renderer.capture(out);
    std::size_t quads = gui_quads(list).size(), triangles = 0;
    for (const GuiDrawBatch& b : list.batches)
        if (b.flush == 0x371200) triangles += b.vertices.size() / 3;
    std::cout << "GUI capture: " << list.batches.size() << " batches, " << quads << " quads, " << triangles
              << " IE triangles after " << frames << " frames (" << screen << ") -> " << out.string() << '\n';
    return 0;
}

// --loading-capture: the level loading screen (hud_loading, checkpoint 126c)
// after `frames` thread frames on a synthetic clock (each frame is three
// NTSC vblanks, 0x4ee4f8 = 2: 3 x 1001 / 60000 s in 10 us ticks), drawn
// through the host 2D backend into a 640 x 448 BMP.
int loading_capture(const std::filesystem::path& iso, const std::filesystem::path& out, const std::string& level,
                    int frames) {
    std::ifstream stream(iso, std::ios::binary);
    if (!stream) throw std::runtime_error("cannot open " + iso.string());
    DiscImage disc(stream, std::filesystem::file_size(iso));
    auto assets = build_loading_screen(disc, level);
    IeDrawState state;
    GuiDrawList list;
    std::int64_t vblanks = 0;
    for (int i = 0; i < frames;) {
        const bool wake = loading_vblank_1667c0(assets->globals);
        ++vblanks;
        if (!wake) continue;
        list = loading_frame(*assets, state, static_cast<std::uint64_t>(vblanks * 100000 * 1001 / 60000));
        ++i;
    }
    GuiRenderer renderer(640, 448);
    for (const auto& [slot, image] : assets->images()) renderer.set_texture(slot, *image);
    renderer.clear({0.0F, 0.0F, 0.0F, 1.0F});
    renderer.draw(list);
    renderer.capture(out);
    std::string hog;
    for (const auto& c : assets->hog) hog += (hog.empty() ? "" : "\\") + c;
    std::cout << "Loading capture: " << level << " (" << hog << "), " << frames << " frames (" << vblanks
              << " vblanks), " << list.batches.size() << " batches, " << assets->screen.screen.drawn_texts().size()
              << " texts -> " << out.string() << '\n';
    loading_screen_destroy(assets->screen);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: omega_frontend_formats <dir-or-files...> | --awards <iso> | --parts <iso> | --set-awards <iso> <state>"
                     " | --gui-run <script> <.GUI...> | --gui-title <iso> <script> | --gui-capture <iso> <out.bmp> [frames] [SCREEN]"
                     " | --loading-capture <iso> <out.bmp> [LEVEL] [frames]\n";
        return 2;
    }
    if (std::string(argv[1]) == "--gui-capture") {
        if (argc < 4 || argc > 6) {
            std::cerr << "usage: omega_frontend_formats --gui-capture <iso> <out.bmp> [frames] [SCREEN]\n";
            return 2;
        }
        try {
            return gui_capture(argv[2], argv[3], argc >= 5 ? std::stoi(argv[4]) : 60, argc == 6 ? argv[5] : "TITLESCR");
        } catch (const std::exception& e) {
            std::cerr << "FAIL gui capture: " << e.what() << '\n';
            return 1;
        }
    }
    if (std::string(argv[1]) == "--loading-capture") {
        if (argc < 4 || argc > 6) {
            std::cerr << "usage: omega_frontend_formats --loading-capture <iso> <out.bmp> [LEVEL] [frames]\n";
            return 2;
        }
        try {
            return loading_capture(argv[2], argv[3], argc >= 5 ? argv[4] : "TORONTO1", argc == 6 ? std::stoi(argv[5]) : 60);
        } catch (const std::exception& e) {
            std::cerr << "FAIL loading capture: " << e.what() << '\n';
            return 1;
        }
    }
    if (std::string(argv[1]) == "--gui-run" || std::string(argv[1]) == "--gui-title") {
        const bool title = std::string(argv[1]) == "--gui-title";
        if (title ? argc != 4 : argc < 4) {
            std::cerr << "usage: omega_frontend_formats --gui-run <script> <.GUI...> | --gui-title <iso> <script>\n";
            return 2;
        }
        try {
            std::vector<std::vector<std::uint8_t>> guis;
            if (title) return gui_run(read_text(argv[3]), title_screen_guis(argv[2]));
            for (int i = 3; i < argc; ++i) {
                const std::string s = read_text(argv[i]);
                guis.emplace_back(s.begin(), s.end());
            }
            return gui_run(read_text(argv[2]), guis);
        } catch (const std::exception& e) {
            std::cerr << "FAIL gui: " << e.what() << '\n';
            return 1;
        }
    }
    if (std::string(argv[1]) == "--awards") {
        if (argc != 3) {
            std::cerr << "usage: omega_frontend_formats --awards <iso>\n";
            return 2;
        }
        try {
            return dump_awards(argv[2]);
        } catch (const std::exception& e) {
            std::cerr << "FAIL awards: " << e.what() << '\n';
            return 1;
        }
    }
    if (std::string(argv[1]) == "--set-awards") {
        if (argc != 4) {
            std::cerr << "usage: omega_frontend_formats --set-awards <iso> <agent state>\n";
            return 2;
        }
        try {
            return set_awards(argv[2], argv[3]);
        } catch (const std::exception& e) {
            std::cerr << "FAIL set-awards: " << e.what() << '\n';
            return 1;
        }
    }
    if (std::string(argv[1]) == "--parts") {
        if (argc != 3) {
            std::cerr << "usage: omega_frontend_formats --parts <iso>\n";
            return 2;
        }
        try {
            return dump_parts(argv[2]);
        } catch (const std::exception& e) {
            std::cerr << "FAIL parts: " << e.what() << '\n';
            return 1;
        }
    }
    std::vector<std::filesystem::path> files;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path p = argv[i];
        if (std::filesystem::is_directory(p)) {
            for (const auto& entry : std::filesystem::directory_iterator(p))
                if (entry.is_regular_file()) files.push_back(entry.path());
        } else {
            files.push_back(p);
        }
    }
    std::sort(files.begin(), files.end());
    int failures = 0;
    for (const auto& path : files) {
        const auto ext = upper_extension(path);
        if (ext != ".GUI" && ext != ".IE" && ext != ".FNT") continue;
        std::ifstream in(path, std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        try {
            if (ext == ".GUI") {
                const auto g = decode_gui(bytes);
                std::cout << "FILE " << path.filename().string() << ' ' << bytes.size() << '\n';
                std::cout << "G " << g.version << ' ' << g.consumed << '\n';
                dump_gui_node(g.root, 0);
            } else if (ext == ".IE") {
                const auto ie = decode_ie(bytes);
                std::cout << "FILE " << path.filename().string() << ' ' << bytes.size() << '\n';
                std::cout << "I " << ie.version << ' ' << ie.consumed << '\n';
                dump_ie_node(ie.root, 0);
            } else {
                const auto f = decode_fnt(bytes);
                std::cout << "FILE " << path.filename().string() << ' ' << bytes.size() << '\n';
                dump_fnt(f);
            }
        } catch (const std::exception& e) {
            std::cerr << "FAIL " << path.filename().string() << ": " << e.what() << '\n';
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
