#pragma once
// The interact prompt of the player pad 0024b5d0: the block
// 0x0024ec1c..0x0024eea0 (after the camera bits 00247800 at 0x0024ec14,
// before the movement message 0x0024eea0), and the interact widget of the
// game screen GuiGameScreen (HUD *004ffc2c +6a4, 004187c0("GuiGameScreen",
// "GAMEGUI"), vtable 004e1620) it drives: 0045cf80 (shown), 0045cfb0 (hide)
// and 0045d070 (show text).
//
// The GUI manager is not hosted in play. The widget state the block reads and
// writes (+50's +14 bit 1, the screen's +54/+55/+58/+64, +e0's visibility)
// is modelled here; the widget calls whose effects are not modelled are kept,
// in order, in GameScreenInteract::log (the HudObject log convention):
//   "41beb0 <text>"           0041beb0(widget, text, 0), the text set
//   "41e0f0 10 <a3>"          0041e0f0(widget, widget, 10, a3, 0)
//   "41db80 textbackground"   0041db80(screen, 004c2238), the child lookup
//   "41e0f0 5"                0041e0f0(screen, child, 5, sp+bc, 0): the IE
//                             element query; with an element the resize
//                             after it is interact_prompt_resize below
//                             (Checkpoint 122b), run by the caller.
//   "242b00 <text>"           the HUD notice (the caller's notice, logged too)
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omega {

// GuiGameScreen fields the interact prompt touches.
struct GameScreenInteract {
    // +50: the "interact" child (0041db80 at 0x0045e14c, kept when IsA
    // GuiTextWidget 004a7520). GAMEGUI.GUI has it (a GuiTextWidget).
    bool widget_50{true};
    // +50's +14 bit 1 (GuiRuntime flag_visible): GAMEGUI.GUI loads
    // "interact" with flag2 = 0 (hidden).
    std::uint16_t widget_flags_14{};
    bool shown_54{};                 // +54 (constructor 0045e540: 0)
    bool client_55{};                // +55 (0; set by 0045d070 with a3: the script prompt)
    std::string client_text_58;      // +58 (a std::string, empty)
    std::uint8_t client_arg_64{1};   // +64 (constructor: 1)
    // +e0 "choosemenu" visible (0045bd00: +e0 && +e0's +14 bit 1); GAMEGUI.GUI
    // loads it with flag2 = 0.
    bool choose_menu_visible_e0{};
    std::vector<std::string> log;
};

// 0045cf80: +50 && (+50's +14 & 2).
bool game_screen_interact_shown(const GameScreenInteract& gs);
// 0045cfb0(gs, a1): nothing without +50. a1: +55 = 0; else +54 = 0 and, with
// +55, the client text again: 0041beb0(+50, +58 (001692d0), 0) and
// 0041e0f0(+50, +50, 10, +64, 0) (logged as for the show). Then +14 bit 1 =
// +55 || +54. The pad block's per-frame hide (a1 = 0) so keeps a script
// prompt on screen until the script clears it.
void game_screen_interact_hide(GameScreenInteract& gs, bool client);
// 0045d070(gs, text, a2, a3): nothing without +50; a null or empty text is
// 0045cfb0(gs, a3). a3 (the script prompt 0045eca0 / 0045ed00, both
// 0045d070(..., 1)): +55 = 1, +58 = text (std::string 0013b480 / 0013d680 /
// 00138b70), +64 = a2, +54 untouched; otherwise +54 = 1. Then +14 |= 2 and
// the logged widget calls above. Goldens: game_screen_interact_goldens.hpp
// (tools/ee/gen_game_screen_interact.py).
void game_screen_interact_show(GameScreenInteract& gs, const std::optional<std::string>& text, std::uint8_t a2,
                               bool client);
// game::DisplayInteract(GOBJ, key, JoystickButton) 0045e640: the button is
// the argument's byte +8 (lbu), the GOBJ goes through 003cc820 (identity) to
// 0045e930, which in solo (005721c8 clear) only calls 0045eca0(key, button):
// with the HUD (*004ffc2c) and its game screen (+6a4), 0045d070(gs,
// 003fdb50(key), button, 1). `get_string` is 003fdb50 (nullopt: NULL); it
// is not called without the HUD or the screen. The multiplayer message
// (vtable 004e1680, 003fdb40, 003fb020) is not ported.
void game_screen_display_interact(GameScreenInteract* gs, bool hud, const std::string& key, std::uint8_t button,
                                  const std::function<std::optional<std::string>(const std::string&)>& get_string);
// game::DisplayInteract(text, JoystickButton) 0045e6d0 (checkpoint 132ms1, TRAINING's
// climb-over box): the text word and the argument's byte +4 (lbu) go to 0045ed00, which
// with the HUD (*004ffc2c) and its game screen (+6a4) is 0045d070(gs, text, button, 1),
// in every mode (no 005721c8 test, no 003fdb50: the script passes game::GetString's text).
void game_screen_display_interact_text(GameScreenInteract* gs, bool hud, const std::optional<std::string>& text,
                                       std::uint8_t button);
// game::ClearInteract(GOBJ) 0045e5f0 -> 0045e730, solo: 0045ec60, which
// with the HUD and its game screen is 0045cfb0(gs, 1). game::ClearInteract()
// 0045e620 calls 0045ec60 directly (no 005721c8 test).
void game_screen_clear_interact(GameScreenInteract* gs, bool hud);

// The globals of the textbackground resize 0x0045d16c..0x0045d3b0.
struct InteractResizeGlobals {
    std::uint8_t cached_61c8 = 0;          // byte 005861c8 (bss: 0)
    std::array<float, 16> cache_61d0{};     // 005861d0: the element's first +30
    std::uint8_t b_6210 = 0;               // byte 00586210 (set to 1; read elsewhere)
    std::uint8_t first_7b10 = 1;           // byte 00497b10 (.data: 1)
    std::array<float, 4> rect_6220{};      // 00586220: the backgroundfortext rect, +8 less 70
};
// 0x0045d16c..0x0045d3b0 after "41e0f0 5" returned 1 with the element
// (sp+bc): the element matrix +30 is cached once (005861c8 / 005861d0);
// the first call looks up "backgroundfortext" under the element's node
// (003ee840(el +70, 004c2250); `background_rect` is 003ecb50(node, 0.0),
// nullopt when the node is missing) and keeps its rect with the width less
// 70.0 (00586228); byte 00497b10 is cleared either way. f4 is the interact
// font's width of the text (cvt.s.w of 0036aa50(+50's +54, text, 0x200);
// 0.0 without a font: `text_width` nullopt). Returns the matrix 003e8040
// gets: the cache when f4 <= w or w <= 0, else the cache with row 0 times
// f4 / w (vmulx.xyzw) and row 3 x plus (f4 - w) / 2.
std::array<float, 16> interact_prompt_resize(InteractResizeGlobals& g, const std::array<float, 16>& element_matrix,
                                             const std::function<std::optional<std::array<float, 4>>()>& background_rect,
                                             std::optional<std::int32_t> text_width);

// The box 00171260(*004ffbd0, 0) selects (select_interaction_hotbox).
struct PadInteractBox {
    std::uint32_t type_90{};
    std::optional<std::string> message_a0;   // nullopt: 0
};

struct PadInteractPromptInput {
    float pressure_f84{};       // sp+f84: pad word 0055a490 (D-pad Left pressure) /255
    bool previous_a5{};         // input +a5 (the previous frame's sp+f84 != 0, 0x0024fa98..)
    bool object_128{};          // input +128 (the 00253070 object)
    bool word_130{};            // input +130
    bool hud_6a8{}, hud_6a9{}, hud_6aa{};
    bool player_actor{true};    // s1: a2 when its +4c is *0049d010 (the actor class), else 0
    bool linked_model_3478{};   // 002501b0(actor + 3478)
    bool bits_3c_bit1{};        // 00250350(input + 38, 1): input +3c bit 1 (0x2)
    bool bits_3c_bit8{};        // 00250350(input + 38, 8): input +3c bit 8 (0x100)
    std::int32_t mode_320c{};   // actor +320c
    std::uint32_t flags_324c{}; // actor +324c
};
struct PadInteractPromptHost {
    std::function<std::optional<PadInteractBox>()> select_box;                 // 00171260(*004ffbd0, 0)
    std::function<std::optional<std::string>(const std::string&)> get_string; // 003fdb50 (nullopt: NULL)
    std::function<void(const std::optional<std::string>&)> notice;            // 00242b00(hud, text)
    // Checkpoint 132t: the +128 path. menu_update is 002526f0(+128, actor)
    // (true: the menu has entries), menu_close 00247320(input) (the destroy
    // 00252fb0, the free, +128 = 0). Empty: the path throws.
    std::function<bool()> menu_update;
    std::function<void()> menu_close;
    // Checkpoint 132hud: the map. map_toggle is 002427a0(hud) at 0x0024ec64 and returns HUD +6a9 as it
    // stands after the toggle (read at 0x0024ec70); map_sticks is 0026be80(HUD +6a0, pad) at 0x0024ec80.
    // Empty: those paths throw naming the address.
    std::function<bool()> map_toggle;
    std::function<void()> map_sticks;
};
// 0x0024ec1c..0x0024eea0 on the game screen (HUD +6a4). Throws at the map
// toggle 002427a0 and the map update 0026be80(+6a0) without their hooks,
// at the input +128 path (002526f0 / 00247320) without its hosts; and when the box path runs
// without a player actor (the original then reads +3478 / +320c through 0).
void run_pad_interact_prompt(const PadInteractPromptInput& in, GameScreenInteract& gs,
                             const PadInteractPromptHost& host);

} // namespace omega
