#pragma once
// Port-only (checkpoint 132win11, not the original): the in-game Port Settings page. The PS2 game has no such
// screen; its PC settings (port_settings.hpp) were file-only until now. The page is shown by the GUI screen
// PortGuiSettings (gui_runtime.hpp: PORTSETTINGS, synthesised from GAMEOPT2's widgets, port_cheats.hpp), reached
// from the title's Options (the "PORT SETTINGS" row under CPU PARTNER, port_options_row.hpp) and from the pause
// menu (its "Port Settings" entry). This is its model: the rows of each page, their values read from the port
// settings and the changes written back at once (update_port_settings_text keeps the file's comments).
//
// Pages (row 0 of each is the page selector; L1 / R1 turn the page anywhere):
//   DISPLAY    fullscreen, vsync, aspect, render_scale, frame_overlay, frame_overlay_key, background_audio (132win12)
//   INPUT      mouse_look, mouse_sensitivity, mouse_invert_y, controller_prompt, reset the key bindings, fov (132ws3:
//              FIELD OF VIEW, 70..150%, the wide view's; here because DISPLAY's eight rows fill the screen), movies
//              (132vt2a2: MOVIES, FILL / BARS / STRETCH, the wide movies' fit), renderer (132vk1, the PC: RENDERER,
//              DIRECT3D 11 / VULKAN, from the next start; research/VULKAN.md)
//   PERFORMANCE issue #4: GAMEPLAY FPS (native 29.97 Hz or enhanced 60 Hz, applied at the next level) and
//              RESOLUTION (native 640 x 448 picture density or the window resolution times render_scale)
//   PLAY KEYS  bind.play.<button> (16), bind.play.stick_left / stick_right.<dir> (8), walk, menu_dpad.<dir> (4)
//   MENU KEYS  bind.menu.<button> (16)
//   MODS       (checkpoint 132md4) mods (ALL MODS) and one row per mod folder of the mods folder (mod.<folder>=0|1,
//              over its manifest's enabled=), in resolution order; more than 7 mods: 6 a time and MORE MODS. A mod
//              change applies at the next start (the assets of a running session are not reloaded): a row whose
//              state differs from the running set shows "(NEXT)". Names are upper-cased and cut to 17 characters.
//              Checkpoint 132md14: a mod row's value is its place in the load order and state ("#2 ON"); Cross
//              toggles it, Left / Right move it up / down the order (the manifests' priority=, as the launcher's
//              mod panel and omega_mod_tool move write them).
// Left / Right cycle a value; Cross toggles it, or for a key row (and the overlay key) waits for the next key
// pressed (feed_keys; Esc cancels, F11 is reserved for the window), which replaces the input's key list.
// Checkpoint 132win12: on a key row Right waits for a key that joins the list (a key already there is left) and Left
// removes the list's last key (an emptied list is written "none", the input unbound); Cross on RENDER SCALE or
// SENSITIVITY types a free value (digits, ".", Backspace; Enter writes it when its reader takes it, in range;
// Esc cancels), where Left / Right step through the fixed list as before. None of this reaches the game: the
// values are the port's presentation and host input only.

#include "gui_runtime.hpp"
#include "keyboard_pad.hpp"
#include "mod_set.hpp"
#include "port_settings.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omega {

// The port settings the options rows and the page share: a file, or (no file: scripted runs without
// --port-settings) values kept in memory for the process, so tests never touch the player's file.
class PortSettingsStore {
public:
    explicit PortSettingsStore(std::filesystem::path file = {}) : file_(std::move(file)) {}
    PortSettings read() const { return file_.empty() ? memory_ : read_port_settings(file_); }
    void write(const PortSettings& s) {
        if (file_.empty()) memory_ = s;
        else write_port_settings(file_, s);
    }
    const std::filesystem::path& file() const { return file_; }
    std::string where() const { return file_.empty() ? std::string("memory (no settings file)") : file_.string(); }

private:
    std::filesystem::path file_;
    PortSettings memory_;
};

// The value of an "other" key (the last one, as the readers take it).
std::optional<std::string> port_setting_value(const PortSettings& s, const std::string& key);
// Sets an "other" key: the first occurrence takes the value, repeats are removed, a new key is appended.
void set_port_setting(PortSettings& s, const std::string& key, const std::string& value);
// Removes every "other" key starting with prefix; answers how many.
std::size_t erase_port_settings(PortSettings& s, const std::string& prefix);

struct PortSettingsHooks {
    // The game window's fullscreen (host_window.hpp); unset: the settings' value alone (hidden runs).
    std::function<bool()> fullscreen_get;
    std::function<void(bool)> fullscreen_set;
    // After every change written: the hosts apply the new values (bindings, display, pacing, mouse look ...).
    std::function<void(const PortSettings&)> changed;
};

class PortSettingsPage final : public gui::PortSettingsScreen {
public:
    enum class Page {
        display,
        input,
        play_keys_1,
        play_keys_2,
        play_keys_3,
        play_keys_4,
        menu_keys_1,
        menu_keys_2,
        performance,
        mods
    };
    static constexpr std::int32_t page_count = 10;

    PortSettingsPage(std::shared_ptr<PortSettingsStore> store, PortSettingsHooks hooks);

    std::string title() const override;
    std::string label(std::int32_t row) const override;
    std::string value(std::int32_t row) const override;
    bool act(std::int32_t row, Action a) override;
    std::uint64_t revision() const override { return revision_; }
    void opened() override;
    void closed() override;

    // A key row waits for a key (the hosts then feed the keyboard here and give the GUI no buttons).
    bool capturing() const { return capture_.has_value(); }
    // The keyboard now (KeyboardState: by virtual-key code). The first call takes the keys already down (the
    // Cross / Enter that started the capture) as held; the next key that goes down is bound. Answers true when
    // the capture ended (bound or cancelled).
    bool feed_keys(const KeyboardState& keys);
    // Port-only test (--port-settings-test-keys): key codes taken, one per capture, instead of the keyboard.
    void queue_test_keys(std::vector<std::uint8_t> keys) { test_keys_.assign(keys.begin(), keys.end()); }
    // Whether the hosts give the GUI no buttons this frame: while a key is awaited, and after a capture until
    // every input is up (input_down: any button or key held now), so the bound key or the Cross / Enter that
    // started it does not press a row again.
    bool holds_input(bool input_down);
    // Hosts that apply the changes (the front end, a play pass) besides hooks.changed; unlisten before they go.
    std::size_t listen(std::function<void(const PortSettings&)> f);
    void unlisten(std::size_t id);

    std::int32_t page() const { return page_; }
    void set_page(std::int32_t p);
    // Port-only (Vita): false hides the PC rows: the keyboard ones (the PLAY KEYS / MENU KEYS pages, INPUT's
    // RESET KEYS, DISPLAY's OVERLAY KEY) and the mouse ones (INPUT's MOUSE LOOK, SENSITIVITY, INVERT Y). The PAGE
    // row then cycles DISPLAY, INPUT, MODS. A hidden page is left for DISPLAY.
    void set_pc_rows(bool shown);
    bool pc_rows() const { return pc_rows_; }
    bool page_shown(std::int32_t p) const;
    std::int32_t rows_on_page() const;
    PortSettingsStore& store() { return *store_; }
    // Checkpoint 132md4: the MODS page's folder: set (play's --mods <folder>), else the settings' mods_folder= or
    // "mods".
    void set_mods_folder(std::optional<std::filesystem::path> folder);
    std::filesystem::path mods_folder() const;
    // The log of the changes made (one line each), taken by the hosts.
    std::vector<std::string> take_log();

private:
    struct Binding {
        std::string key;    // "bind.play.cross"
        std::string label;  // "CROSS"
        bool overlay_key = false;
    };
    std::vector<Binding> bindings_on(std::int32_t page) const;
    std::vector<std::uint8_t> keys_of(const PortSettings& s, const Binding& b) const;
    std::string binding_value(const PortSettings& s, const Binding& b) const;
    static std::string keys_text(const std::vector<std::uint8_t>& keys);
    bool feed_number(std::uint8_t vk);  // a typed value's key; true when the entry ended
    void commit(PortSettings& s, const std::string& what);
    void bind_key(std::uint8_t vk);
    std::int32_t step_page(bool forward) const;   // the next / previous shown page
    std::size_t row_id(std::size_t r) const;      // a shown row's place in its page's full row list

    bool pc_rows_ = true;

    const std::vector<mods::ModListing>& mod_list() const;  // 132md4: the MODS page's mods (cached)
    std::int32_t mod_rows() const;                         // the MODS page's mod rows (MORE MODS included)
    std::string mod_value(std::size_t r) const;
    bool mod_act(std::size_t r, Action a);

    std::shared_ptr<PortSettingsStore> store_;
    PortSettingsHooks hooks_;
    std::int32_t page_ = 0;
    std::uint64_t revision_ = 1;
    struct Capture {
        Binding target;
        std::optional<KeyboardState> held;
        bool append = false;                // 132win12: the key joins the list (Right) instead of replacing it (Cross)
        std::optional<std::string> text;    // 132win12: a typed value (render_scale / mouse_sensitivity), as typed so far
    };
    std::optional<Capture> capture_;
    std::deque<std::uint8_t> test_keys_;
    std::vector<std::string> log_;
    bool wait_release_ = false;
    std::size_t next_listener_ = 0;
    std::map<std::size_t, std::function<void(const PortSettings&)>> listeners_;
    std::optional<std::filesystem::path> mods_folder_;
    mutable std::optional<std::vector<mods::ModListing>> mod_list_;
    std::size_t mod_offset_ = 0;
};

}  // namespace omega
