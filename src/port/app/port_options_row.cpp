#include "port_options_row.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
GuiNode* find_node(GuiNode& n, const std::string& name) {
    if (n.name == name) return &n;
    for (auto& c : n.children)
        if (GuiNode* r = find_node(c, name)) return r;
    return nullptr;
}
std::size_t child_index(const GuiNode& parent, const std::string& name) {
    for (std::size_t i = 0; i < parent.children.size(); ++i)
        if (parent.children[i].name == name) return i;
    throw std::runtime_error("Port options row (port-only): GAMEOPT " + parent.name + " has no " + name);
}
// The control page's row step: value_rumble - value_freelooky (rect y, decorator translation z).
std::pair<float, float> row_step(GuiNode& root) {
    GuiNode* grp = find_node(root, "control_grp");
    if (!grp) throw std::runtime_error("Port options row (port-only): GAMEOPT has no control_grp");
    const GuiNode& above = grp->children[child_index(*grp, "value_freelooky")];
    const GuiNode& rumble = grp->children[child_index(*grp, "value_rumble")];
    if (!above.decorator || !rumble.decorator)
        throw std::runtime_error("Port options row (port-only): GAMEOPT value rows have no decorator");
    return {rumble.rect[1] - above.rect[1], rumble.decorator->matrix[11] - above.decorator->matrix[11]};
}
// Copies grp's node 'source' one row step below it as 'name', right after it. Answers the copy's index.
std::size_t copy_below(GuiNode& grp, const char* source, const char* name, std::pair<float, float> step) {
    const std::size_t i = child_index(grp, source);
    GuiNode n = grp.children[i];
    n.name = name;
    n.rect[1] += step.first;
    if (n.decorator) n.decorator->matrix[11] += step.second;
    grp.children.insert(grp.children.begin() + static_cast<std::ptrdiff_t>(i + 1), std::move(n));
    return i + 1;
}
// Moves a widget and its children down by dy (rect top and decorator translation z; both are screen-absolute).
void shift_down(GuiNode& n, std::pair<float, float> dy) {
    n.rect[1] += dy.first;
    if (n.decorator) n.decorator->matrix[11] += dy.second;
    for (auto& c : n.children) shift_down(c, dy);
}
}

void inject_cpu_partner_row(GuiNode& root) {
    GuiNode* grp = find_node(root, "control_grp");
    if (!grp) throw std::runtime_error("CPU Partner row (port-only): GAMEOPT has no control_grp");
    for (const auto& c : grp->children)
        if (c.name == cpu_partner_button) return;
    const auto step = row_step(root);
    const std::size_t label = copy_below(*grp, "label_rumble", cpu_partner_label, step);
    if (!grp->children[label].text) throw std::runtime_error("CPU Partner row (port-only): label_rumble has no text");
    grp->children[label].text->text = cpu_partner_text;  // a literal (no "$" key: STRINGS.DAT has none)
    // label_rumble's box (110.3 wide) wraps "CPU PARTNER"; widen it about its centre to label_manualaim's 199.9.
    auto& lr = grp->children[label].rect;
    const float widen = std::max(0.0F, cpu_partner_label_width - lr[2]);
    lr[0] -= widen * 0.5F;
    lr[2] += widen;
    copy_below(*grp, "textbackground6", cpu_partner_background, step);
    copy_below(*grp, "value_rumble", cpu_partner_button, step);
}

void inject_video_rows(GuiNode& root) {
    GuiNode* grp = find_node(root, "video_grp");
    if (!grp) throw std::runtime_error("Video rows (port-only): GAMEOPT has no video_grp");
    for (const auto& c : grp->children)
        if (c.name == fullscreen_button) return;
    const auto full = row_step(root);
    const std::pair<float, float> step{full.first * video_row_step_scale, full.second * video_row_step_scale};
    const std::pair<float, float> twice{step.first * 2.0F, step.second * 2.0F};
    // Each copy goes right after its source: the VSYNC copy is made first so that FULLSCREEN's lands before it.
    const std::size_t vlabel = copy_below(*grp, "label_videomode", vsync_label, twice);
    if (!grp->children[vlabel].text) throw std::runtime_error("Video rows (port-only): label_videomode has no text");
    grp->children[vlabel].text->text = vsync_text;  // literals (no "$" key: STRINGS.DAT has none)
    const std::size_t label = copy_below(*grp, "label_videomode", fullscreen_label, step);
    grp->children[label].text->text = fullscreen_text;
    copy_below(*grp, "textbackground107", vsync_background, twice);
    copy_below(*grp, "textbackground107", fullscreen_background, step);
    copy_below(*grp, "value_videomode", vsync_button, twice);
    copy_below(*grp, "value_videomode", fullscreen_button, step);
    // "PRESS [] TO SAVE" (decorator z -149.1, 1.53 full steps below VIDEO MODE's -89.4) moves to one compressed
    // step below VSYNC: 132win2 moved it half a step for FULLSCREEN alone; VSYNC would now overlap it.
    GuiNode* save = find_node(root, save_message);
    const GuiNode& mode = grp->children[child_index(*grp, "value_videomode")];
    if (save && save->decorator && mode.decorator) {
        const float dz = mode.decorator->matrix[11] + 3.0F * step.second - save->decorator->matrix[11];
        if (dz < 0.0F) shift_down(*save, {dz * (full.first / full.second), dz});
    }
}

void inject_port_settings_row(GuiNode& root) {
    GuiNode* grp = find_node(root, "control_grp");
    if (!grp) throw std::runtime_error("Port Settings row (port-only): GAMEOPT has no control_grp");
    for (const auto& c : grp->children)
        if (c.name == port_settings_button) return;
    const auto step = row_step(root);
    const std::size_t label = copy_below(*grp, cpu_partner_label, port_settings_label, step);
    if (!grp->children[label].text) throw std::runtime_error("Port Settings row (port-only): no CPU PARTNER label text");
    grp->children[label].text->text = port_settings_text;
    copy_below(*grp, cpu_partner_background, port_settings_background, step);
    copy_below(*grp, cpu_partner_button, port_settings_button, step);
}

std::function<PortSettings()> bind_cpu_partner_row(FrontendServices& services, std::filesystem::path settings_file,
                                                   FullscreenRow fullscreen, VsyncRow vsync) {
    // No file (a scripted run without --port-settings): the rows keep their values in memory, so tests never
    // read or write the player's settings.
    return bind_port_settings_rows(services, std::make_shared<PortSettingsStore>(std::move(settings_file)),
                                std::move(fullscreen), std::move(vsync));
}

std::function<PortSettings()> bind_port_settings_rows(FrontendServices& services, std::shared_ptr<PortSettingsStore> store,
                                                   FullscreenRow fullscreen, VsyncRow vsync, PortSettingsPage* page) {
    if (!store) throw std::invalid_argument("Port options rows (port-only): no settings store");
    auto strings = services.strings;
    const auto read = [store] { return store->read(); };
    const auto write = [store](const PortSettings& s) { store->write(s); };
    const std::string where = store->where();
    const bool screen_row = fullscreen.get && fullscreen.set;
    services.port_rows.settings = page;
    services.port_rows.patch = [screen_row, page](const std::string& screen, GuiFile& file) {
        if (screen != "GAMEOPT") return;
        inject_cpu_partner_row(file.root);
        if (page) inject_port_settings_row(file.root);
        if (screen_row) inject_video_rows(file.root);
    };
    const auto show = [strings](gui::GuiRuntime& rt, gui::Widget* value, bool on) {
        const auto s = strings ? strings(on ? "MB_On" : "MB_Off") : std::nullopt;
        rt.set_text(value, s ? *s : std::string(), false);  // as go_refresh: a missing string shows nothing
    };
    const auto vsync_now = [read, vsync] { return vsync.get ? vsync.get() : read().vsync; };
    services.port_rows.wake = [read, show, fullscreen, vsync_now](gui::GuiRuntime& rt, gui::Widget* screen) {
        if (gui::Widget* v = rt.find(screen, cpu_partner_button)) show(rt, v, read().coop_partner);
        if (gui::Widget* v = rt.find(screen, port_settings_button)) rt.set_text(v, port_settings_open_text, false);
        if (fullscreen.get)
            if (gui::Widget* v = rt.find(screen, fullscreen_button)) show(rt, v, fullscreen.get());
        if (gui::Widget* v = rt.find(screen, vsync_button)) show(rt, v, vsync_now());
    };
    services.port_rows.pressed = [read, write, where, show, fullscreen, vsync, vsync_now](
                                     gui::GuiRuntime& rt, gui::Widget* screen, gui::Widget* sender) {
        if (!sender) return false;
        if (sender->name == port_settings_button) {  // 132win11: the Port Settings page
            if (!rt.port_open_settings(screen)) return false;
            std::cout << "Port settings (port-only): the PORT SETTINGS row opens the Port Settings page\n";
            return true;
        }
        if (sender->name == vsync_button) {  // 132win4
            const bool on = !vsync_now();
            PortSettings s = read();
            s.vsync = on;
            write(s);
            if (vsync.set) vsync.set(on);
            show(rt, sender, on);
            std::cout << "Port settings (port-only): VSync " << (on ? "on" : "off") << " written to " << where << "\n";
            return true;
        }
        if (sender->name == fullscreen_button && fullscreen.get && fullscreen.set) {
            const bool on = !fullscreen.get();
            PortSettings s = read();
            s.fullscreen = on;
            write(s);
            fullscreen.set(on);
            show(rt, sender, on);
            std::cout << "Port settings (port-only): Fullscreen " << (on ? "on" : "off") << " written to " << where << "\n";
            return true;
        }
        if (sender->name != cpu_partner_button) return false;
        PortSettings s = read();
        s.coop_partner = !s.coop_partner;
        write(s);
        show(rt, sender, s.coop_partner);
        std::cout << "Port settings (port-only): CPU Partner " << (s.coop_partner ? "on" : "off") << " written to " << where
                  << "\n";
        return true;
    };
    return read;
}

}  // namespace omega
