#pragma once
// Checkpoint 126: what the front-end session reads from the disc (fonts,
// STRINGS.DAT, the award tables, the screens and their decorator IEs, the
// ZMOVIES.HOG movies), bound as FrontendServices. Shared by the window
// (frontend_app.cpp) and the session tests.
#include "agent_modify.hpp"
#include "agent_preview.hpp"
#include "frontend_lights.hpp"
#include "frontend_session.hpp"
#include "gui_draw.hpp"
#include "gui_renderer.hpp"
#include "gun_model_draw.hpp"
#include "localization.hpp"
#include "model_overrides.hpp"
#include "movie_subtitle_overlay.hpp"
#include "profile_awards.hpp"
#include "texture.hpp"
#include "weapon_database.hpp"
#include "weapon_models.hpp"
#include "windows_hash.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace omega {

class DiscImage;

// The movie picture as one GUI quad over the whole 640 x 448 canvas space
// (host choice: the PS2 player's 003f7060 upload and sprite are not modelled),
// drawn from the texture slot movie_slot; movie_texture is a decoded picture as
// RGBA. Shared by the window (frontend_app.cpp) and the Vita app (vita_main.cpp).
constexpr std::uint32_t movie_slot = 0xffff;
// Port-only (checkpoint 132md14): `aspect` > 0 is a mod movie's display aspect (movie_overrides=, MovieStream::
// picture_aspect): wider than 4:3 it is letterboxed in the 4:3 canvas (and drawn at its own aspect under the wide
// frame: GuiDrawBatch::port_picture_aspect), narrower it is pillarboxed. 0: the disc movie's 4:3 quad.
GuiDrawList movie_list(float aspect = 0.0F);
Texture movie_texture(const mpeg2::Frame& frame);
// Checkpoint 132cp11: the subtitle object 0x571cf0's overlay over the picture:
// 004118e0's packet with the DEFAULT font (00418420's default handle, the
// object's +54) and TEX1 per 003bb620 from the last render mode.
void append_subtitles(GuiDrawList& list, const GuiDiscFonts& fonts, const movie::SubtitleTrack& track,
                      const std::array<float, 4>& colour, std::uint32_t render_mode);
// The projection append_subtitles uses (movie::subtitle_projection, computed once on the first call). Port-only
// (132vt2a): the first call runs the renderer setup 0037e780 over a mirror memory, ~1.5 s on the Vita, so the Vita
// app makes it while the front end loads instead of in a subtitled movie's first frame.
const movie::SubtitleProjection& front_end_subtitle_projection();
// A front-end screen's name for logs and the wide-background stems ("frontend.<name>", 132md12 / 132ws): the
// screen's GUI file stem. Shared by the window and the Vita app.
const char* frontend_class_name(gui::WidgetClass c);

class FrontendDisc {
public:
    // iso: the image disc reads (a second handle streams the movies).
    FrontendDisc(DiscImage& disc, const std::filesystem::path& iso);
    FrontendDisc(const FrontendDisc&) = delete;
    FrontendDisc& operator=(const FrontendDisc&) = delete;

    // The services over this disc and the PC card folder; they reference
    // this object, which must outlive the session.
    FrontendServices services(const std::filesystem::path& card_folder);

    const GuiDiscFonts& fonts() const { return fonts_; }
    GuiDecorAssets& assets() { return assets_; }

    // Checkpoint 132zf: FRONTEND/LANG/ENGLISH/ZEUS.HOG (the ZEUS files'
    // pictures): the member index without case (-1: none), the loaded
    // pictures by slot (from zeus_first_slot), and the 00380a40 / 003813a0
    // load and release (shared per stem, counted).
    static const std::vector<std::string>& zeus_hog();
    static constexpr std::int32_t zeus_first_slot = 0x8000;
    bool zeus_present() const { return zeus_present_; }
    std::int32_t zeus_lookup(const std::string& name) const;
    gui::GuiHost::BitmapTexture zeus_load(const std::string& name);
    void zeus_release(std::int32_t slot);
    // The ZEUS pictures, the archive-0 MANTEX.HOG pictures and the PORT-ONLY agent preview by slot.
    const std::map<std::uint32_t, Texture>& zeus_images() const { return zeus_images_; }

    // Checkpoint 132cc4b: GuiAgentModify's services. The part cache 0055dda0 as frontCharacter
    // (0022a220) fills it from GAMEDATA/FRONTEND (built once, on the first call); archive 0's
    // pictures (0036e4b0: GuiAgentModify's patches, 00340320) from FRONTEND/MANTEX.HOG.
    AgentPartCache* agent_parts();
    gui::GuiHost::BitmapTexture mantex_load(const std::string& name);
    // Checkpoint 132cc6: the agent screens' model draw (GuiAgentCreate 0028d870, GuiAgentModify
    // 00341f00; `flush` the draw's address): the packing's parts (agent_preview.hpp, posed at `seconds`
    // into ANIMIDLE) placed by agent_camera_view's model rows and projected by its clip, as figure
    // batches the GUI renderer depth-tests; lit by MANLIGHT (004053b0's rows, frontend_light_colour).
    // The part textures take ZEUS-range slots kept for the session. Returns a note for the log.
    std::string agent_figure_draw(GuiDrawList& out, const AgentPacking& packing, const AgentCamera& camera,
                                  float seconds, std::uint32_t flush, const std::array<float, 4>& turns = {});
    // Checkpoint 132cc5: a disc file named by a backslash path (Skl_Load 00402840's reads).
    std::vector<std::uint8_t> frontend_file(const std::string& path);
    // Checkpoint 132f13: the weapon database *004ffd30 (COMMON\WEAPONS.WDB) and the model library over
    // the front end's db+1c archive COMMON\WPNHOG.HOG (00145160), made on the first call.
    WeaponModelLibrary* weapons();
    // Checkpoint 132f13: GuiWeaponDisplay's draw 00440580 in the front end (the personnel pages'
    // earned items): the gates on model +417, +54 == 1.0 and the bounds +3e0 / +3f0, the fit
    // matrices (weapon_display_matrices) and 003836b0(model, 2, a2, 1), lit by GUNLIGHT (the rows
    // 004053b0 left at 0055df80 / 0055df40), as play_main's preview_draw. The gun textures take
    // ZEUS-range slots kept for the session; `frame` is the counter 005383c8.
    void weapon_display_draw(GuiDrawList& out, IeDrawState* st, const gui::Widget& w, std::uint32_t frame);

private:
    struct ZeusSlot {
        std::string stem;
        std::int32_t refs = 0;
    };
    gui::GuiHost::BitmapTexture slot_load(const std::vector<std::string>& archive, const std::string& member,
                                          const std::string& stem);
    DiscImage& disc_;
    GuiDiscFonts fonts_;
    Localization strings_;
    ProfileAwards awards_;
    GuiDecorAssets assets_;
    IsoStream movie_stream_;  // std::ifstream; the Vita's 64-bit SceIo stream (windows_hash.hpp)
    std::vector<std::string> movie_members_;
    bool zeus_present_ = false;
    std::vector<std::string> zeus_members_;
    std::map<std::string, std::int32_t> zeus_slots_;
    std::map<std::int32_t, ZeusSlot> zeus_by_slot_;
    std::map<std::uint32_t, Texture> zeus_images_;
    std::int32_t zeus_next_ = 0;
    std::vector<std::string> mantex_members_;
    std::unique_ptr<AgentPartCache> agent_parts_;
    std::unique_ptr<AgentPreviewRenderer> preview_;
    std::optional<FrontendLights> lights_;
    std::map<std::string, std::int32_t> figure_slots_;
    std::vector<GuiVertex> figure_unique_;      // port-only scratch: each mesh vertex transformed once
    std::vector<std::uint8_t> figure_done_;
    const FrontendLights& lights();
    std::unique_ptr<WeaponDatabase> weapon_db_;
    std::unique_ptr<WeaponModelLibrary> weapon_library_;
    // per (model handle, part) the decoded LOD 0 part; per library texture id its slot (both dropped
    // when the library's generation changes).
    std::map<std::pair<std::int32_t, std::size_t>, std::shared_ptr<const GunPartMesh>> gun_parts_;
    std::map<std::int32_t, std::int32_t> gun_slots_;
    std::uint32_t gun_generation_ = 0;
    // Checkpoint 132md3 (port-only, model_overrides.hpp "gun models"): the mods' replacements of the displayed guns'
    // parts per model handle (COMMON's keys: the front end has no mission) and their images' slots; dropped with
    // gun_parts_. Empty without mods.
    std::map<std::int32_t, std::shared_ptr<const mods::GunModelOverrides>> gun_overrides_;
    std::map<const gfx::ReplacementModel*, std::int32_t> gun_mod_slots_;
    mods::GunPreviewCache gun_preview_cache_;  // 132md7: the replaced parts' preview vertices (MODDING.md "Speed")
};

}  // namespace omega
