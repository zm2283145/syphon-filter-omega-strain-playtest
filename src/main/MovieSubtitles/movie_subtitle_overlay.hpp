#pragma once
// The movie subtitle overlay of the subtitle object 0x571cf0 (MovieSubtitles.cc;
// research/FRONTEND.md "Subtitles", Checkpoint 132cp11): its orthographic
// camera (pushed by 00412600, refreshed by 00375390 / 00374df0 and projected by
// 0037d9a0), the line measure 00411600, the glyph sprites 00410e50 and the GS
// packet 004118e0 the movie thread builds each picture after the cue update
// 00411db0 (SubtitleTrack).
#include "gui_draw.hpp"
#include "movie_subtitles.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omega::movie {

// What the glyphs read of the renderer 0x5381f0 once 00412600 pushed the
// subtitle record: the matrix V+ec0 (0037d9a0, column-major: m[4 * c + r])
// and the record's GS scale (+90, +94; 004115c0) and offset (+a0, +a4; 00411580).
struct SubtitleProjection {
    std::array<float, 16> m{};
    float scale_x{}, scale_y{}, offset_x{}, offset_y{};
};

// The renderer as the front end's 0037e780(0x5381f0, 640, 448, 0x20, 0x10, 0,
// 0xfa0, 0x1f4) leaves it, then 00412600's push (0x412a60..0x412bc8: rect
// (20, 20, 600, 440), orthographic +104, near 0 / far 1, a fresh identity view
// matrix) and 0037d9a0.
SubtitleProjection subtitle_projection();

// The subtitle object's rect +40..+4c.
constexpr std::array<float, 4> subtitle_rect{20.0F, 20.0F, 600.0F, 440.0F};

// A string of the object: its bytes up to and including the NUL, and the end
// pointer (+6c[i]) as an index from the start; `no_end` is the null pointer.
constexpr std::size_t no_end = static_cast<std::size_t>(-1);
struct SubtitleText {
    std::span<const std::uint8_t> s;   // must hold the terminator
    std::size_t end = no_end;
};

// 00411600(font, s, end, &w, &h): the width (glyph widths plus advances and
// kerning) and the height cvt(+99 * +90). `tex_width` is the font texture's
// u16 width (*(*(*0x539248 + 8) + slot * 4) + 0x44) + 4).
struct SubtitleSize {
    float w{}, h{};
};
SubtitleSize subtitle_measure_00411600(const GuiFont& font, std::uint16_t tex_width, SubtitleText text);

// One GIF A+D pair.
struct GsPair {
    std::uint64_t data{};
    std::uint64_t reg{};
};

// 00410e50(font, out, x, y, s, colour, rect, end): two (UV, RGBAQ, XYZ2)
// vertices per glyph inside the rect, appended to `out`.
void subtitle_glyphs_00410e50(std::vector<GsPair>& out, const GuiFont& font, std::uint16_t tex_width,
                              std::uint16_t tex_height, const SubtitleProjection& projection, float x, float y,
                              SubtitleText text, const std::array<float, 4>& colour,
                              const std::array<float, 4>& rect);

// The fields 004118e0 reads (the debug clock block at +b0 is clear: 00412600
// zeroes it and nothing sets it).
struct SubtitleOverlayState {
    bool font = true;                  // +54 non-null
    bool visible = false;              // +ac
    std::uint32_t flags = 0;           // +a4
    std::vector<std::string> lines;    // +60[i], +78 of them (at most 3)
    std::vector<std::size_t> ends;     // +6c[i] - +60[i]; no_end for the null pointer
    std::array<float, 4> colour{};     // +80
    std::array<float, 4> rect = subtitle_rect;   // +40
};
// The object's state from the cue model: +6c is the line pointer plus its
// reveal count once a TYPE line started, else null; +80 the parsed Color (the
// cue load 00412150 copies feed object +50) or white when no cue is loaded.
SubtitleOverlayState subtitle_overlay_state(const SubtitleTrack& track, const std::array<float, 4>& colour);

// 003bb620(texrec): the font texture record's TEX1 word (+58) with its filter
// fields (& 0x1e0) set from the render mode *0x5392d0 the last draw left: a
// mode with 4 or 8 gives MMAG linear and MMIN 4 (mode & 4) or 5; else nearest.
std::uint64_t subtitle_tex1_003bb620(std::uint64_t tex1_58, std::uint32_t render_mode);

// 004118e0's packet: the DMA tag's QWC, the GIF tag's NLOOP and the pairs
// (the four state pairs TEX0 / TEX1 / ALPHA / PRIM, then the glyphs).
struct SubtitlePacket {
    std::uint32_t qwc{};
    std::uint32_t nloop{};
    std::vector<GsPair> pairs;
};
SubtitlePacket subtitle_packet_004118e0(const SubtitleOverlayState& state, const GuiFont& font,
                                        std::uint16_t tex_width, std::uint16_t tex_height,
                                        const SubtitleProjection& projection, std::uint64_t tex0,
                                        std::uint64_t tex1);

// 00410c20's Color value: four whitespace-separated numbers, each newlib strtod
// (00121bb0, correctly rounded) then the soft-float narrowing 0011c928, which
// truncates toward zero and flushes magnitudes below FLT_MIN to a signed zero
// (EE goldens); from 2^128 it throws (its overflow results are irregular).
// No Color key leaves the feed object's (1, 1, 1, 0.5).
std::array<float, 4> subtitle_colour_words(const std::string& value);
std::array<float, 4> subtitle_colour(const SubtitleFile& file);

// Host: the packet's sprites as one GUI batch of `slot` (render mode 5, filter
// the TEX1 pair's & 0x1e0; GS X / 16 - 2048 and 2048 - Y / 16 in canvas units,
// UV / 16 over the texture size as GuiVertex uv + 2048, the RGBAQ bytes as the
// vertex colour).
GuiDrawBatch subtitle_batch(const SubtitlePacket& packet, std::uint32_t slot, std::uint16_t tex_width,
                            std::uint16_t tex_height);

}  // namespace omega::movie
