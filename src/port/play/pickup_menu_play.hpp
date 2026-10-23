#pragma once
// Checkpoint 132t: the play's host of the item pickup menu (pickup_menu.hpp,
// "pickup_menu.cc"), the Triangle confirm object a backpack's receiver
// 001762c0 builds on the HUD screen (*(*004ffc2c + 0x54)) and stores in the
// player component's +178 (input +128).
//
// PickupMenuScreenHost answers the GUI half of PickupMenuHost /
// PickupMenuFrameHost over the HUD's IeScreen: node, element and text
// handles are the screen's indices + 1 (0: null; 003eeb30's miss is 0);
// 003ef010 / 003e8eb0 / 003eebe0 / 003f45a0 / 003f44e0 / 003f4510 /
// 003e7eb0 / 003e7ec0 (-> 003d08b0) are the IeScreen operations, the text
// bytes +10 / +11 / +5b the text object's, 003d0810 with the pickup colour
// block 004f7e70 the text colour (SetPickupTextColor's values). 003fdb50
// resolves through the mission's string pools (unresolved: a NULL text,
// which throws). The preview objects 00259420 (0x68 bytes, vtable 004dc4a0:
// +60 = 0, +64 = 7, the 0040c240(0xf5) id and 003cf0e0 recorded) are
// session tokens whose +60 / +64 the element-data draw (002592f0 / 002593b0,
// the HUD weapon display's class) reads; 003cb230 marks one released. The
// sounds (001ca010 / 003c9420; 0020f860) are queued in `sounds` for the
// play's gameSound (Checkpoint 132f3). The game half (the inventory, the
// backpack and the player) is the session's.
#include "ie_screen.hpp"
#include "pickup_menu.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// A 00259420 object as the element-data draw reads it.
struct PickupMenuPreview {
    std::uint32_t block{};       // the 0036b180 allocation it was built in
    std::uint32_t model_60{};    // +60: a record +13c / +11c model handle (0: none)
    std::uint8_t byte_64{7};     // +64: 7 from the constructor (no weapon table entry)
    bool released{};             // 003cb230(preview, 0.1)
};

inline constexpr std::uint32_t pickup_menu_block_base=0xd0000000U;
inline constexpr std::uint32_t pickup_menu_preview_base=0xd4000000U;

// A sound message of the menu, in call order:
//   - named: 001ca010(msg, 3, name, 0, 0) (a SHELL.BNK GS_* name), 0020faa0
//     PLAY, sent with 003c9420(msg, target, transport) (1: delivered at once,
//     0: posted);
//   - otherwise: 0020f860(msg, descriptor, position) of a static descriptor,
//     +20 = 1, 003c9420(msg, 004ffb50, 0) (posted).
struct PickupMenuSound {
    std::string name;                // empty: the positional descriptor message
    std::int32_t transport{};
    std::uint32_t descriptor{};
    RootVector position{};
};

// What outlives one host: the previews by token, the allocation counters and
// the recorded calls and sounds of the current operation.
struct PickupMenuPlayState {
    std::map<std::uint32_t,PickupMenuPreview> previews;
    std::uint32_t blocks{},objects{};
    std::vector<std::string> notes;
    std::vector<PickupMenuSound> sounds;
};

class PickupMenuScreenHost : public PickupMenuHost, public PickupMenuFrameHost {
public:
    using Strings=std::function<std::optional<std::string>(std::string_view key)>;
    PickupMenuScreenHost(IeScreen& screen,const std::array<float,4>& pickup_colour,Strings strings,
                         std::optional<std::string> player_name,PickupMenuPlayState& state);

    // PickupMenuHost (the GUI half).
    std::uint32_t find_child(std::uint32_t node,std::string_view name) override;
    std::uint32_t create_element(std::uint32_t node) override;
    std::uint32_t element_node(std::uint32_t element) override;
    std::uint32_t element_text(std::uint32_t element) override;
    void evaluate(std::uint32_t element,float time) override;
    void fade(std::uint32_t element,float time) override;
    void set_text(std::uint32_t element,std::string_view text) override;
    std::string get_string(std::string_view key) override;
    std::uint32_t player(std::uint32_t list,std::int32_t index) override;
    std::string player_name(std::uint32_t player) override;
    void text_byte(std::uint32_t text,std::uint32_t offset,std::uint8_t value) override;
    void set_colour(std::uint32_t text,std::uint32_t colour) override;
    void set_visible(std::uint32_t node,std::int32_t a1,std::int32_t a2) override;
    std::uint32_t allocate(std::uint32_t size,std::string_view file,std::uint32_t line) override;
    std::uint32_t construct_preview(std::uint32_t block) override;
    void post_sound(std::string_view name,std::uint32_t target,std::int32_t flag) override;
    void release_preview(std::uint32_t preview,float seconds) override;
    void remove_element(std::uint32_t element) override;

    // PickupMenuFrameHost (the GUI half).
    void set_preview(std::uint32_t preview,std::uint32_t word_60,std::uint8_t byte_64) override;
    void set_element_data(std::uint32_t element,std::uint32_t data) override;
    void anim_range(std::uint32_t element,float from,float to,std::int32_t loop) override;
    void positional_sound(std::uint32_t descriptor,const RootVector& position) override;

    // The local player token 001314f0(004ffb50, -1) returns.
    static constexpr std::uint32_t player_token=1;
protected:
    IeScreen& screen_;
    PickupMenuPlayState& state_;
private:
    IeElement& element(std::uint32_t handle,const char* where);
    IeTextObject& text(std::uint32_t handle,const char* where);
    PickupMenuPreview& preview(std::uint32_t token,const char* where);
    std::array<float,4> colour_;
    Strings strings_;
    std::optional<std::string> player_name_;
};

}
