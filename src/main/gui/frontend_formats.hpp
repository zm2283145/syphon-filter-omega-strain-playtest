#pragma once

// Front-end asset file decoders, read exactly as the EE loaders walk the HOG
// member buffer (little-endian; "align4" aligns the absolute pointer, and HOG
// members are 16-aligned so file offsets are equivalent).
//
//   .GUI  0x419020 header, 0x418b10 node, 0x41f210 / 0x41c560 / 0x41ace0
//         widget Load, 0x416f70 GuiInterfaceDecorator Load
//   .IE   0x3e7b00 header, 0x3ef660 node, 0x3f0bd0 track, 0x3f0770 TEXT/NEXT
//   .FNT  0x36ab80
//
// Every decoder reports how many bytes it consumed; the loaders never look at
// what follows (HOG padding). Inputs on which the EE would misparse or skip
// (an unknown widget class, a non-GuiWidget root, an unknown IE track or text
// record, the computed space width of old fonts) throw instead of being
// hosted.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// ---------------------------------------------------------------- .GUI

enum class GuiLoadKind : std::uint8_t {
    base,      // 0x41f210
    text,      // 0x41c560 (GuiTextWidget, GuiButtonWidget)
    text_edit  // 0x41ace0 (GuiTextEditWidget)
};

// The Load a GUI-file class uses (the 20 classes that occur in the shipped
// files; any other name is not hosted).
std::optional<GuiLoadKind> gui_class_load(std::string_view class_name);

struct GuiEvent {
    std::string name;
    std::uint16_t action = 0;  // 1 SetTime(a)+PlayTo(b), 2 ping-pong, 3 wrap
    std::uint16_t a = 0;
    std::uint16_t b = 0;
};

struct GuiDecorator {
    std::string ie;    // "" -> the interface's own IE
    std::string node;  // "" -> the widget name (+"_root" for the root)
    std::array<float, 12> matrix{};  // 4 rows x 3 (row 3 = translation)
    std::vector<GuiEvent> events;
};

struct GuiText {
    std::string text;  // "$KEY" is localised by 0x41beb0
    std::string font;  // lower-cased lookup 0x418420, "" -> default
    std::array<float, 3> rgb{};
    std::uint32_t halign = 0;
};

struct GuiTextEdit {
    std::uint32_t max_len = 0;
    std::uint16_t password = 0;
};

struct GuiNode {
    std::string class_name;
    std::string decorator_class;
    std::string name;
    std::array<float, 4> rect{};  // x, y (top), w, h
    std::uint16_t flag2 = 0;      // nonzero sets widget +14 bit 0x2
    std::uint16_t flag4 = 0;      // nonzero sets widget +14 bit 0x4
    std::optional<GuiText> text;
    std::optional<GuiTextEdit> edit;
    std::optional<GuiDecorator> decorator;  // only "GuiInterfaceDecorator"
    std::vector<GuiNode> children;
};

struct GuiFile {
    std::uint16_t version = 0;  // skipped unchecked by 0x419020
    GuiNode root;
    std::size_t consumed = 0;
};

GuiFile decode_gui(std::span<const std::uint8_t> bytes);

// Widget +14 after the base Load: constructor default 0x26 with bits 0x2/0x4
// set or cleared from the file (0x41f210).
std::uint16_t gui_widget_flags(const GuiNode& node);

// ---------------------------------------------------------------- .IE

enum class IeTrackKind : std::uint8_t {
    vertex, diffuse, opacity, rot_x, rot_y, rot_z, uv_off_u, uv_off_v, uv_scl_u, uv_scl_v
};

struct IeTrack {
    IeTrackKind kind = IeTrackKind::opacity;
    std::vector<std::array<float, 2>> scalar;                // {t, v}
    std::vector<std::array<float, 4>> diffuse;               // {t, r, g, b}
    std::vector<std::vector<std::array<float, 4>>> vertex;   // per vertex {t, x, y, z}
};

struct IeTextRecord {
    bool next = false;              // false: TEXT, true: NEXT
    std::uint32_t declared = 1;     // TEXT point count (version >= 2), else 1
    std::vector<std::array<float, 3>> points;  // every point consumed, file order
};

struct IeNode {
    std::string name;
    std::string texture;
    std::array<float, 12> matrix{};
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<float, 4>> colors;
    std::vector<std::array<std::uint32_t, 9>> triangles;
    std::vector<IeTrack> tracks;
    std::vector<IeTextRecord> text;
    std::vector<IeNode> children;
};

struct IeFile {
    std::uint16_t version = 0;  // -> global 0x55c0b8 (magic bytes are not checked)
    IeNode root;
    std::size_t consumed = 0;
};

IeFile decode_ie(std::span<const std::uint8_t> bytes);

// ---------------------------------------------------------------- .FNT

struct FontGlyph {
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0;
};

struct FontFile {
    std::uint8_t version = 0;
    std::uint8_t sub = 0;
    std::uint8_t name_field = 0;   // bytes skipped after the name length byte
    std::string texture;           // as stored by the loader: a-z upper-cased
    std::uint8_t cell_w = 0;       // +98
    std::uint8_t height = 0;       // +99
    std::uint32_t count = 0;       // +9c: read only for version 3, else 0x8e
    std::vector<FontGlyph> glyphs; // +a0
    std::int8_t space = 0;         // +94 (as float)
    bool kerning = false;          // +be0
    std::vector<std::int8_t> kern; // +be1, count * count
    std::size_t consumed = 0;
};

FontFile decode_fnt(std::span<const std::uint8_t> bytes);

}  // namespace omega
