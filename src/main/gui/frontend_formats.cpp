#include "frontend_formats.hpp"

#include <cstring>
#include <stdexcept>
#include <string_view>

namespace omega {
namespace {

class Cursor {
public:
    Cursor(std::span<const std::uint8_t> bytes, std::size_t offset, const char* format)
        : bytes_(bytes), offset_(offset), format_(format) {}

    void need(std::size_t n) const {
        if (offset_ > bytes_.size() || n > bytes_.size() - offset_)
            fail("read past the end of the file");
    }
    std::uint8_t u8() {
        need(1);
        return bytes_[offset_++];
    }
    std::uint16_t u16() {
        need(2);
        const auto v = static_cast<std::uint16_t>(bytes_[offset_] | bytes_[offset_ + 1] << 8);
        offset_ += 2;
        return v;
    }
    std::uint32_t u32() {
        need(4);
        const auto v = static_cast<std::uint32_t>(bytes_[offset_]) |
                       static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 8 |
                       static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 16 |
                       static_cast<std::uint32_t>(bytes_[offset_ + 3]) << 24;
        offset_ += 4;
        return v;
    }
    float f32() {
        const auto bits = u32();
        float v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    template <std::size_t N>
    std::array<float, N> floats() {
        std::array<float, N> v{};
        for (auto& f : v) f = f32();
        return v;
    }
    std::string cstr() {
        const auto start = offset_;
        while (true) {
            need(1);
            if (bytes_[offset_++] == 0) break;
        }
        return {reinterpret_cast<const char*>(bytes_.data() + start), offset_ - 1 - start};
    }
    void align4() { offset_ = (offset_ + 3) & ~std::size_t{3}; }
    void skip(std::size_t n) {
        need(n);
        offset_ += n;
    }
    std::size_t offset() const { return offset_; }
    std::span<const std::uint8_t> bytes() const { return bytes_; }

    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error(std::string(format_) + ": " + what + " at offset " + std::to_string(offset_));
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_;
    const char* format_;
};

// The loaders copy names into 0x7e-byte buffers (a longer name is cut and gets
// a '\n'); the shipped names are far shorter, a longer one is not hosted.
void require_short_name(const Cursor& c, const std::string& name) {
    if (name.size() >= 0x7e) c.fail("name of 0x7e or more bytes is truncated by the loader (not hosted)");
}

// Counts are read as words, looped with a signed > 0 test and kept as u16.
std::uint32_t count16(Cursor& c, std::uint32_t value) {
    if (value > 0xffff) c.fail("count above 0xffff is truncated or skipped by the loader (not hosted)");
    return value;
}

// ------------------------------------------------------------------ .GUI

constexpr std::string_view gui_decorator_class = "GuiInterfaceDecorator";  // 0x4bf120

GuiDecorator read_gui_decorator(Cursor& c) {  // 0x416f70
    GuiDecorator d;
    d.ie = c.cstr();
    d.node = c.cstr();
    c.align4();
    d.matrix = c.floats<12>();
    const auto n = c.u16();
    d.events.reserve(n);
    for (std::uint16_t i = 0; i < n; ++i) {
        GuiEvent e;
        e.name = c.cstr();
        c.align4();
        e.action = c.u16();
        e.a = c.u16();
        e.b = c.u16();
        d.events.push_back(std::move(e));
    }
    return d;
}

GuiNode read_gui_node(Cursor& c, bool root, int depth) {  // 0x418b10
    if (depth > 64) c.fail("widget tree deeper than 64 levels");
    GuiNode node;
    node.class_name = c.cstr();
    node.decorator_class = c.cstr();
    require_short_name(c, node.class_name);
    require_short_name(c, node.decorator_class);
    // 0x418c44: a supplied root must be GuiWidget (0x4bf110), else no widget.
    if (root && node.class_name != "GuiWidget") c.fail("root class '" + node.class_name + "' is not GuiWidget");
    const auto load = gui_class_load(node.class_name);
    if (!load) c.fail("widget class '" + node.class_name + "' has no hosted Load");

    // 0x41f210
    node.name = c.cstr();
    c.align4();
    node.rect = c.floats<4>();
    node.flag2 = c.u16();
    node.flag4 = c.u16();
    if (*load != GuiLoadKind::base) {  // 0x41c560
        GuiText t;
        t.text = c.cstr();
        t.font = c.cstr();
        c.align4();
        t.rgb = c.floats<3>();
        t.halign = c.u32();
        node.text = std::move(t);
    }
    if (*load == GuiLoadKind::text_edit) {  // 0x41ace0
        GuiTextEdit e;
        e.max_len = c.u32();
        e.password = c.u16();
        node.edit = e;
    }
    if (node.decorator_class == gui_decorator_class) node.decorator = read_gui_decorator(c);
    else if (!node.decorator_class.empty()) c.fail("decorator class '" + node.decorator_class + "' is not hosted");

    const auto n = c.u16();  // unaligned, straight after the fields
    node.children.reserve(n);
    for (std::uint16_t i = 0; i < n; ++i) node.children.push_back(read_gui_node(c, false, depth + 1));
    return node;
}

// ------------------------------------------------------------------ .IE

IeTrack read_ie_track(Cursor& c) {  // 0x3f0bd0
    const auto name = c.cstr();
    c.align4();
    IeTrack t;
    static constexpr std::pair<std::string_view, IeTrackKind> scalar[] = {
        {"OPACITY", IeTrackKind::opacity}, {"ROT_X", IeTrackKind::rot_x},     {"ROT_Y", IeTrackKind::rot_y},
        {"ROT_Z", IeTrackKind::rot_z},     {"UVOFF_U", IeTrackKind::uv_off_u}, {"UVOFF_V", IeTrackKind::uv_off_v},
        {"UVSCL_U", IeTrackKind::uv_scl_u}, {"UVSCL_V", IeTrackKind::uv_scl_v}};
    if (name == "VERTEX") {
        t.kind = IeTrackKind::vertex;
        const auto n = count16(c, c.u32());
        t.vertex.resize(n);
        for (auto& keys : t.vertex) {
            const auto k = count16(c, c.u32());
            keys.reserve(k);
            for (std::uint32_t i = 0; i < k; ++i) keys.push_back(c.floats<4>());
        }
        return t;
    }
    if (name == "DIFFUSE") {
        t.kind = IeTrackKind::diffuse;
        const auto n = count16(c, c.u32());
        t.diffuse.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) t.diffuse.push_back(c.floats<4>());
        return t;
    }
    for (const auto& [label, kind] : scalar) {
        if (name != label) continue;
        t.kind = kind;
        const auto n = count16(c, c.u32());
        t.scalar.reserve(n);
        for (std::uint32_t i = 0; i < n; ++i) t.scalar.push_back(c.floats<2>());
        return t;
    }
    // The original stores a null track and continues without reading its keys.
    c.fail("unknown IE track '" + name + "' (not hosted)");
}

IeTextRecord read_ie_text(Cursor& c, std::uint16_t version) {  // 0x3f0770
    const auto name = c.cstr();
    require_short_name(c, name);
    c.align4();
    IeTextRecord r;
    if (name == "TEXT") {
        r.declared = version >= 2 ? c.u32() : 1;
        // The first point is always read, the second only when the signed
        // count is at least 2; the rest are skipped.
        const auto n = static_cast<std::int32_t>(r.declared);
        const std::uint32_t consumed = n < 2 ? 1 : static_cast<std::uint32_t>(n);
        c.need(std::size_t{12} * consumed);
        r.points.reserve(consumed);
        for (std::uint32_t i = 0; i < consumed; ++i) r.points.push_back(c.floats<3>());
        return r;
    }
    if (name == "NEXT") {
        r.next = true;
        r.points.push_back(c.floats<3>());
        return r;
    }
    // The original returns 0 (ignored by 0x3ef660) having read only the name.
    c.fail("unknown IE text record '" + name + "' (not hosted)");
}

IeNode read_ie_node(Cursor& c, std::uint16_t version, int depth) {  // 0x3ef660
    if (depth > 64) c.fail("IE tree deeper than 64 levels");
    IeNode node;
    node.name = c.cstr();
    node.texture = c.cstr();
    require_short_name(c, node.name);
    require_short_name(c, node.texture);
    c.align4();
    node.matrix = c.floats<12>();
    auto n = count16(c, c.u32());
    node.positions.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) node.positions.push_back(c.floats<3>());
    n = count16(c, c.u32());
    node.uvs.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) node.uvs.push_back(c.floats<2>());
    n = count16(c, c.u32());
    node.colors.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) node.colors.push_back(c.floats<4>());
    n = count16(c, c.u32());
    node.triangles.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::array<std::uint32_t, 9> t{};
        for (auto& w : t) w = c.u32();
        node.triangles.push_back(t);
    }
    n = count16(c, c.u32());
    node.tracks.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) node.tracks.push_back(read_ie_track(c));
    n = count16(c, c.u32());
    node.text.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) node.text.push_back(read_ie_text(c, version));
    n = count16(c, c.u32());
    node.children.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) node.children.push_back(read_ie_node(c, version, depth + 1));
    return node;
}

}  // namespace

std::optional<GuiLoadKind> gui_class_load(std::string_view name) {
    static constexpr std::string_view base[] = {
        "GuiWidget",          "GuiBitmapWidget",     "GuiTextListWidget",  "GuiScrollBarWidget",
        "GuiCharacterDisplay", "GuiSliderWidget",    "GuiTextArrayWidget", "GuiWeaponDisplay",
        "GuiMLTextWidget",    "GuiMissionList",      "GuiChatWindow",      "GuiSubTitleDisplay",
        "GuiAgentDisplay",    "GuiWeaponStats",      "GuiQuickChat",       "GuiNetMessageArray",
        "GuiMissionStatList"};
    if (name == "GuiTextWidget" || name == "GuiButtonWidget") return GuiLoadKind::text;
    if (name == "GuiTextEditWidget") return GuiLoadKind::text_edit;
    for (const auto b : base)
        if (name == b) return GuiLoadKind::base;
    return std::nullopt;
}

GuiFile decode_gui(std::span<const std::uint8_t> bytes) {  // 0x419020
    Cursor c(bytes, 0, "GUI");
    c.need(3);
    if (bytes[0] != 'G' || bytes[1] != 'U' || bytes[2] != 'I') c.fail("bad magic");
    c.skip(3);
    c.align4();
    GuiFile file;
    file.version = c.u16();
    file.root = read_gui_node(c, true, 0);
    file.consumed = c.offset();
    return file;
}

std::uint16_t gui_widget_flags(const GuiNode& node) {
    std::uint16_t flags = 0x26;  // 0x41f690
    flags = node.flag2 ? flags | 0x2 : flags & ~0x2;
    flags = node.flag4 ? flags | 0x4 : flags & ~0x4;
    return flags;
}

IeFile decode_ie(std::span<const std::uint8_t> bytes) {  // 0x3e7b00
    Cursor c(bytes, 0, "IE");
    c.skip(2);
    IeFile file;
    file.version = c.u16();
    file.root = read_ie_node(c, file.version, 0);
    file.consumed = c.offset();
    return file;
}

FontFile decode_fnt(std::span<const std::uint8_t> bytes) {  // 0x36ab80
    Cursor c(bytes, 0, "FNT");
    FontFile f;
    f.version = c.u8();
    f.sub = c.u8();
    f.name_field = c.u8();
    // The name is copied from here up to its terminator; the cursor then moves
    // by the length byte, whatever the string length was.
    {
        Cursor name(bytes, c.offset(), "FNT");
        f.texture = name.cstr();
        require_short_name(c, f.texture);
        for (auto& ch : f.texture)
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 0x20);
    }
    c.skip(f.name_field);
    f.cell_w = c.u8();
    f.height = c.u8();
    f.count = f.version == 3 ? c.u8() : 0x8e;
    if (f.count >= 0xb5) c.fail("glyph count of 0xb5 or more (the loader abandons the font)");
    f.glyphs.reserve(f.count);
    for (std::uint32_t i = 0; i < f.count; ++i) {
        const auto g = c.floats<4>();
        f.glyphs.push_back({g[0], g[1], g[2], g[3]});
    }
    const bool stored_space = (f.version == 2 && f.sub >= 4) || f.version >= 3;
    if (!stored_space) c.fail("space width computed from the texture (version < 3 font, not hosted)");
    f.space = static_cast<std::int8_t>(c.u8());
    const bool has_kern_flag = (f.version == 2 && f.sub != 0) || f.version >= 3;
    if (has_kern_flag) {
        f.kerning = c.u8() != 0;
        if (f.kerning) {
            const std::size_t n = std::size_t{f.count} * f.count;
            c.need(n);
            const auto* p = bytes.data() + c.offset();
            f.kern.assign(reinterpret_cast<const std::int8_t*>(p), reinterpret_cast<const std::int8_t*>(p) + n);
            c.skip(n);
        }
    }
    f.consumed = c.offset();
    return f;
}

}  // namespace omega
