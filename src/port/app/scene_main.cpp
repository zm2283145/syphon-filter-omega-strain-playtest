#include "binary.hpp"
#include "disc.hpp"
#include "geometry.hpp"
#include "gfx_d3d11.hpp"
#include "population.hpp"
#include "path_motion.hpp"
#include "native_paths.hpp"
#include "scene_renderer.hpp"
#include "windows_hash.hpp"

#include <windows.h>
#include <windowsx.h>

#include <filesystem>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

namespace {

struct State {
    omega::Mesh mesh;
    omega::CollisionMesh collision;
    omega::OrbitCamera camera;
    omega::Bounds camera_bounds;
    omega::SceneOptions options;
    std::unique_ptr<omega::SceneRenderer> renderer;
    std::wstring model;
    std::filesystem::path output;
    bool textures_available{};
    bool minimized{};
    bool dragging{};
    POINT last_mouse{};
    bool request_capture{};
    bool automatic_capture{};
    bool captured{};
    unsigned frames{};
    std::string error;

    State(omega::Mesh model_mesh, omega::CollisionMesh collision_mesh)
        : mesh(std::move(model_mesh)), collision(std::move(collision_mesh)), camera(mesh.bounds), camera_bounds(mesh.bounds) {}
};

void title(HWND window, const State& state) {
    const auto label = L"Native geometry (not gameplay): " + state.model
        + (state.options.textured ? L" | textures" : L" | diagnostic shading")
        + (state.options.wireframe ? L" | wireframe" : L"")
        + (state.options.collision ? L" | COL overlay" : L"")
        + L" | Drag/arrows orbit, wheel zoom, R reset, W/T/C modes, S capture, Esc close";
    if (!SetWindowTextW(window, label.c_str())) throw std::runtime_error("Cannot update scene-window title");
}

LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        state = static_cast<State*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    try {
        if (!state) return DefWindowProcW(window, message, wparam, lparam);
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            state->minimized = wparam == SIZE_MINIMIZED;
            if (state->renderer && !state->minimized)
                state->renderer->resize(LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_GETMINMAXINFO: {
            auto* sizes = reinterpret_cast<MINMAXINFO*>(lparam);
            sizes->ptMinTrackSize = {360, 260};
            return 0;
        }
        case WM_LBUTTONDOWN:
            state->dragging = true;
            state->last_mouse = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            SetCapture(window);
            return 0;
        case WM_LBUTTONUP:
            state->dragging = false;
            if (GetCapture() == window) ReleaseCapture();
            return 0;
        case WM_CAPTURECHANGED:
            state->dragging = false;
            return 0;
        case WM_MOUSEMOVE:
            if (state->dragging) {
                const POINT current{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                state->camera.orbit(float(current.x - state->last_mouse.x) * -0.007F,
                                    float(current.y - state->last_mouse.y) * 0.007F);
                state->last_mouse = current;
            }
            return 0;
        case WM_MOUSEWHEEL:
            state->camera.zoom(float(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA);
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { DestroyWindow(window); return 0; }
            if (wparam == VK_LEFT) state->camera.orbit(-0.1F, 0);
            if (wparam == VK_RIGHT) state->camera.orbit(0.1F, 0);
            if (wparam == VK_UP) state->camera.orbit(0, 0.1F);
            if (wparam == VK_DOWN) state->camera.orbit(0, -0.1F);
            if (wparam == 'R') state->camera = omega::OrbitCamera(state->camera_bounds);
            if (wparam == 'W') state->options.wireframe = !state->options.wireframe;
            if (wparam == 'T') {
                if (state->textures_available) state->options.textured = !state->options.textured;
                else std::cout << "Texture mode is disabled by --geometry-only.\n";
            }
            if (wparam == 'C') {
                state->options.collision = !state->options.collision;
                if (state->options.collision && state->collision.triangles.empty())
                    std::cout << "This COL file declares no collision triangles.\n";
            }
            if (wparam == 'S') state->request_capture = true;
            title(window, *state);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(state->error.empty() ? 0 : 1);
            return 0;
        default: return DefWindowProcW(window, message, wparam, lparam);
        }
    } catch (const std::exception& error) {
        state->error = error.what();
        DestroyWindow(window);
        return 0;
    }
}

void show(State& state, const std::vector<omega::Texture>& textures) {
    const auto instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"OmegaSceneResearchWindow";
    WNDCLASSW type{};
    type.hInstance = instance;
    type.lpfnWndProc = procedure;
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = class_name;
    if (!RegisterClassW(&type)) throw std::runtime_error("Cannot register scene window");
    RECT size{0, 0, 1000, 740};
    if (!AdjustWindowRect(&size, WS_OVERLAPPEDWINDOW, FALSE)) throw std::runtime_error("Cannot size scene window");
    const auto window = CreateWindowExW(0, class_name, L"Native geometry preview", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
        nullptr, nullptr, instance, &state);
    if (!window) throw std::runtime_error("Cannot create scene window");
    struct CloseWindow {
        HWND window;
        ~CloseWindow() { if (IsWindow(window)) DestroyWindow(window); }
    } close{window};
    state.renderer = std::make_unique<omega::SceneRenderer>(omega::gfx::d3d11::create_window_display(window), state.mesh,
                                                            state.collision, textures);
    title(window, state);
    std::cout << "Renderer: " << state.renderer->backend() << ". Diagnostic lighting; no game simulation.\n";
    ShowWindow(window, state.automatic_capture ? SW_SHOWNOACTIVATE : SW_SHOWNORMAL);
    bool running = true;
    while (running) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!running) break;
        if (state.minimized) { Sleep(30); continue; }
        state.renderer->draw(state.camera, state.options);
        ++state.frames;
        if (state.request_capture || (state.automatic_capture && state.frames >= 3)) {
            state.renderer->capture(state.output / "scene.bmp");
            state.request_capture = false;
            state.captured = true;
            std::cout << "Captured this renderer's own back buffer: " << (state.output / "scene.bmp") << '\n';
            if (state.automatic_capture) { DestroyWindow(window); continue; }
        }
        state.renderer->present();
    }
    if (!state.error.empty()) throw std::runtime_error(state.error);
    if (state.automatic_capture && !state.captured) throw std::runtime_error("Scene closed before capture");
    std::ofstream report(state.output / "render.json", std::ios::binary | std::ios::trunc);
    if (!report) throw std::runtime_error("Cannot create render report");
    report << "{\"backend\":" << std::quoted(state.renderer->backend()) << ",\"frames\":" << state.frames
        << ",\"captured\":" << (state.captured ? "true" : "false")
        << ",\"textured\":" << (state.options.textured ? "true" : "false")
        << ",\"wireframe\":" << (state.options.wireframe ? "true" : "false")
        << ",\"collision_overlay\":" << (state.options.collision ? "true" : "false")
        << ",\"scope\":\"Standalone asset geometry; diagnostic lighting, not gameplay\"}\n";
    report.close();
    if (!report) throw std::runtime_error("Cannot finish render report");
    state.renderer.reset();
    UnregisterClassW(class_name, instance);
}

std::span<const std::uint8_t> nested(std::span<const std::uint8_t> archive, const std::string& name) {
    const auto extent = omega::hog_member(archive, archive.size(), name);
    return omega::checked_bytes(archive, static_cast<std::size_t>(extent.offset), static_cast<std::size_t>(extent.size));
}

}

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::filesystem::path iso;
        std::string archive = "MAPVUM.HOG";
        std::string model = "MODEL_C01";
        std::string mission = "BELARUS1";
        std::string texture_archive;
        bool geometry_only = false, export_only = false, capture = false, top = false, terrain = false;
        bool objects = false;
        bool isolate_object = false;
        bool player_start = false;
        std::optional<std::uint32_t> focus_object;
        std::optional<unsigned> motion_ticks;
        float motion_dt = 1.0F / 60.0F;
        bool supplied_motion_dt = false;
        std::optional<omega::Bounds> focus_bounds;
        omega::SceneOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::wstring argument = argv[i];
            if (argument == L"--help") {
                std::cout << "Usage: omega_scene --iso <USA ISO> [--archive MAPVUM.HOG] [--model MODEL_C01]\n"
                             "                   [--mission TORONTO1|BELARUS1] (TORONTO1 is Carthage: Quarantine Zone)\n"
                             "                   [--texture-archive MAPTEX.HOG] [--geometry-only] [--textured]\n"
                             "                   [--wireframe] [--collision] [--top] [--capture | --export-only]\n"
                             "                   [--terrain --geometry-only [--objects]] (stored object poses; no behavior)\n"
                             "                   [--focus-object <packed ID>] (requires --objects)\n"
                             "                   [--player-start] (focus on the loader's initial player pose; requires terrain)\n"
                             "                   [--isolate-object] (hide surroundings of the focused object)\n"
                             "                   [--motion-ticks N --motion-dt dt] (isolated mode-1 path pose after trigger)\n"
                             "Static VUMS/COL5 preview with diagnostic lighting, not a playable mission.\n";
                return 0;
            }
            if (argument == L"--iso" && i + 1 < argc && iso.empty()) iso = argv[++i];
            else if (argument == L"--mission" && i + 1 < argc) mission = omega::uppercase_asset(omega::asset_basename(argv[++i]));
            else if (argument == L"--archive" && i + 1 < argc) archive = omega::uppercase_asset(omega::asset_basename(argv[++i]));
            else if (argument == L"--model" && i + 1 < argc) model = omega::uppercase_asset(omega::asset_basename(argv[++i]));
            else if (argument == L"--texture-archive" && i + 1 < argc) texture_archive = omega::uppercase_asset(omega::asset_basename(argv[++i]));
            else if (argument == L"--geometry-only") geometry_only = true;
            else if (argument == L"--textured") options.textured = true;
            else if (argument == L"--wireframe") options.wireframe = true;
            else if (argument == L"--collision") options.collision = true;
            else if (argument == L"--top") top = true;
            else if (argument == L"--terrain") terrain = true;
            else if (argument == L"--objects") objects = true;
            else if (argument == L"--isolate-object") isolate_object = true;
            else if (argument == L"--player-start") player_start = true;
            else if (argument == L"--motion-ticks" && i + 1 < argc) {
                const std::wstring value = argv[++i];
                std::size_t end = 0;
                const auto ticks = std::stoull(value, &end);
                if (end != value.size() || ticks > 100000 || value.empty() || value.front() == L'-')
                    throw std::runtime_error("Motion ticks must be an integer from 0 to 100000");
                motion_ticks = static_cast<unsigned>(ticks);
            }
            else if (argument == L"--motion-dt" && i + 1 < argc) {
                const std::wstring value = argv[++i];
                std::size_t end = 0;
                motion_dt = std::stof(value, &end);
                if (end != value.size() || !std::isfinite(motion_dt) || motion_dt < 0)
                    throw std::runtime_error("Motion timestep must be finite and nonnegative");
                supplied_motion_dt = true;
            }
            else if (argument == L"--focus-object" && i + 1 < argc) {
                const std::wstring value = argv[++i];
                std::size_t end = 0;
                const auto id = std::stoull(value, &end, 0);
                if (end != value.size() || id > 0xffffffffULL || value.empty() || value.front() == L'-')
                    throw std::runtime_error("Invalid focus object ID");
                focus_object = static_cast<std::uint32_t>(id);
            }
            else if (argument == L"--capture") capture = true;
            else if (argument == L"--export-only") export_only = true;
            else throw std::runtime_error("Unknown or incomplete scene argument; use --help");
        }
        if (iso.empty()) throw std::runtime_error("A user-supplied ISO is required");
        if (mission != "TORONTO1" && mission != "BELARUS1") throw std::runtime_error("Supported research missions: TORONTO1, BELARUS1");
        if (capture && export_only) throw std::runtime_error("Choose capture or export-only");
        if (geometry_only && options.textured) throw std::runtime_error("Texture mode conflicts with --geometry-only");
        if (terrain && !geometry_only) throw std::runtime_error("Terrain assembly currently requires --geometry-only; full materials are unsupported");
        if (objects && !terrain) throw std::runtime_error("Stored object poses require --terrain --geometry-only");
        if (focus_object && !objects) throw std::runtime_error("Object focus requires --objects");
        if (isolate_object && !focus_object) throw std::runtime_error("Object isolation requires --focus-object");
        if (player_start && (!terrain || focus_object)) throw std::runtime_error("Player-start focus requires terrain and conflicts with object focus");
        if (motion_ticks && !isolate_object) throw std::runtime_error("Motion probe requires --isolate-object --focus-object --objects");
        if (supplied_motion_dt && !motion_ticks) throw std::runtime_error("Motion timestep requires --motion-ticks");
        if (!std::filesystem::is_regular_file("config/disc_us.json"))
            throw std::runtime_error("Run the geometry viewer from the repository root");
        if (texture_archive.empty()) texture_archive = archive == "MAPVUM.HOG" ? "MAPTEX.HOG" : "TEX.HOG";
        std::cout << "Verifying the complete ISO before loading original model data...\n";
        auto stream = omega::open_verified_iso(iso, OMEGA_ISO_SIZE, OMEGA_ISO_SHA256);
        omega::DiscImage disc(stream, OMEGA_ISO_SIZE);
        std::vector<std::uint8_t> container;
        std::span<const std::uint8_t> raw_model, raw_collision;
        omega::Mesh mesh;
        omega::CollisionMesh collision_mesh;
        const auto mission_output = std::filesystem::current_path() / "local" / "runtime"
            / (mission == "BELARUS1" ? "" : mission);
        const auto scene_output = mission_output
            / (player_start ? "player_start" : motion_ticks ? "motion_pose" : isolate_object ? "object_isolation" : objects ? "population_scene" : terrain ? "terrain" : "scene");
        std::filesystem::create_directories(scene_output);
        if (terrain) {
            const auto raw_population = disc.read(disc.file({"GAMEDATA", mission, "DATA.POP"}));
            const auto population = omega::decode_population(raw_population);
            if (player_start) {
                const auto pose = omega::npc_spawn_pose(population);
                const auto p = pose.position;
                focus_bounds = omega::Bounds{{p.x-8,p.y-8,p.z-8},{p.x+8,p.y+8,p.z+8}};
                std::ofstream spawn_report(scene_output / "player_start.json");
                spawn_report << std::setprecision(9) << "{\"id\":" << pose.id << ",\"position\":[" << p.x << ',' << p.y << ',' << p.z
                             << "],\"basis\":[";
                for (std::size_t i=0;i<9;++i) { if(i)spawn_report<<','; spawn_report<<pose.basis[i]; }
                spawn_report << "],\"node_override\":" << (pose.node_override?"true":"false") << "}\n";
                spawn_report.close();
                if (!spawn_report) throw std::runtime_error("Cannot finish player-start report");
                std::cout << "Focusing the first NPC's loader pose; no character, controller or physics is running.\n";
            }
            std::ofstream population_report(scene_output / "population.json");
            omega::write_population(population_report, population);
            population_report.close();
            if (!population_report) throw std::runtime_error("Cannot finish population report");
            omega::write_bytes(scene_output / "source.pop", raw_population);
            std::size_t empty = 0;
            for (const auto& item : population.terrain) {
                if (isolate_object) break;
                const auto name = omega::uppercase_asset(item.name.substr(0, item.name.size() - 4));
                try {
                    const auto data = disc.archive_member({"GAMEDATA", mission, "DATA.HOG"}, name + ".HOG");
                    const auto decoded = omega::decode_vum(nested(data, name + ".VUM"));
                    const auto decoded_col = omega::decode_col(nested(data, name + ".COL"));
                    empty += decoded.indices.empty();
                    omega::append_terrain(mesh, collision_mesh, decoded, decoded_col);
                } catch (const std::exception& error) {
                    throw std::runtime_error("Terrain " + name + ": " + error.what());
                }
            }
            std::cout << "Terrain definitions displayed: " << (isolate_object ? 0 : population.terrain.size()) << "; empty visual records: " << empty
                      << ". Particle placements decoded but not spawned: " << population.placements.size()
                      << "; game-object records: " << population.objects.size() << ".\n";
            if (objects) {
                std::size_t visible = 0, no_model = 0;
                for (const auto& object : population.objects) {
                    if (object.asset == "no_vum") { ++no_model; continue; }
                    if (isolate_object && *focus_object != object.id) continue;
                    const auto name = omega::uppercase_asset(object.asset.substr(0, object.asset.size() - 4));
                    try {
                        const auto data = disc.archive_member({"GAMEDATA", mission, "OBJECTS.HOG"}, name + ".HOG");
                        auto decoded = omega::decode_vum(nested(data, name + ".VUM"));
                        auto decoded_col = omega::decode_col(nested(data, name + ".COL"));
                        // Use a fixed conservative path envelope so rotation cannot leave the preview.
                        if (motion_ticks) {
                            auto initial_mesh = decoded;
                            auto initial_collision = decoded_col;
                            omega::apply_stored_pose(initial_mesh, initial_collision, object);
                            if (!initial_mesh.indices.empty()) focus_bounds = initial_mesh.bounds;
                            omega::PathMotion motion(object);
                            float radius = 0;
                            for (const auto& vertex : decoded.vertices) {
                                const auto p = vertex.position;
                                radius = std::max(radius, std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z));
                            }
                            radius *= 1.01F;
                            if (focus_bounds) for (const auto& p : object.motion.points) {
                                auto& low = focus_bounds->minimum;
                                auto& high = focus_bounds->maximum;
                                low = {std::min(low.x,p.x-radius), std::min(low.y,p.y-radius), std::min(low.z,p.z-radius)};
                                high = {std::max(high.x,p.x+radius), std::max(high.y,p.y+radius), std::max(high.z,p.z+radius)};
                            }
                            motion.trigger();
                            for (unsigned tick = 0; tick < *motion_ticks; ++tick) motion.tick(motion_dt);
                            omega::apply_stored_pose(decoded, decoded_col, motion.pose());
                            std::ofstream report(scene_output / "motion_state.json");
                            report << "{\"object_id\":" << object.id << ",\"ticks\":" << *motion_ticks
                                   << ",\"dt\":" << std::setprecision(9) << motion_dt << ",\"state\":";
                            omega::write_motion_state(report, motion);
                            report << "}\n";
                            report.close();
                            if (!report) throw std::runtime_error("Cannot finish motion pose report");
                        } else omega::apply_stored_pose(decoded, decoded_col, object);
                        if (!motion_ticks && focus_object && *focus_object == object.id && !decoded.indices.empty()) focus_bounds = decoded.bounds;
                        omega::append_terrain(mesh, collision_mesh, decoded, decoded_col);
                        ++visible;
                    } catch (const std::exception& error) {
                        throw std::runtime_error("Object " + std::to_string(object.id) + " (" + name + "): " + error.what());
                    }
                }
                std::cout << "Stored object poses: " << visible << "; no-model records retained: " << no_model
                          << ". Scripts, visibility rules and physics are not executed.\n";
                if (motion_ticks) std::cout << "Motion probe: delivered trigger, " << *motion_ticks << " updates at dt=" << motion_dt
                                           << "; showing one resulting pose, not live gameplay.\n";
                if (isolate_object) std::cout << "Object isolation enabled: surrounding terrain and other models deliberately hidden.\n";
            } else std::cout << "Game objects are not displayed; use --objects for stored poses.\n";
            for (const auto& section : population.sections)
                if (!section.decoded) std::cout << "Uninterpreted POP section " << section.tag << ": " << section.count << " declared records.\n";
            model = mission + (objects ? "_STORED_POSES" : "_TERRAIN");
            if (isolate_object) model += "_OBJECT_" + std::to_string(*focus_object);
            if (motion_ticks) model += "_MOTION_TICK_" + std::to_string(*motion_ticks);
            if (player_start) model += "_PLAYER_START";
        } else {
            container = disc.archive_member({"GAMEDATA", mission, archive}, model + ".HOG");
            raw_model = nested(container, model + ".VUM");
            raw_collision = nested(container, model + ".COL");
            mesh = omega::decode_vum(raw_model);
            collision_mesh = omega::decode_col(raw_collision);
        }
        State state(std::move(mesh), std::move(collision_mesh));
        if (focus_object || player_start) {
            if (!focus_bounds) throw std::runtime_error("Focus object is absent or has no visual geometry");
            state.camera = omega::OrbitCamera(*focus_bounds);
            state.camera_bounds = *focus_bounds;
        }
        state.options = options;
        state.model = std::wstring(model.begin(), model.end());
        state.output = scene_output;
        state.automatic_capture = capture;
        if (top) state.camera.pitch = 1.4F;
        std::vector<omega::Texture> textures;
        if (!geometry_only) {
            if (!state.mesh.color_patches.empty())
                throw std::runtime_error("Runtime color patches require --geometry-only; patch behavior is not implemented");
            for (const auto& material : state.mesh.materials) {
                if (material.passes.size() != 1 || material.shader != 2 || (material.flags & 38))
                    throw std::runtime_error("Animated/multipass material rendering is unsupported; use --geometry-only for static geometry.");
            }
            for (const auto& name : state.mesh.textures) {
                const auto filename = omega::uppercase_asset(name) + ".TDX";
                try {
                    textures.push_back(omega::decode_tdx(disc.archive_member(
                        {"GAMEDATA", mission, texture_archive}, filename)));
                } catch (const std::exception& error) {
                    throw std::runtime_error("Texture " + filename + ": " + error.what()
                        + ". Use --geometry-only for an explicitly untextured geometry inspection.");
                }
            }
            state.textures_available = true;
        }
        std::filesystem::create_directories(state.output);
        if (!terrain) {
            omega::write_bytes(state.output / "source.vum", raw_model);
            omega::write_bytes(state.output / "source.col", raw_collision);
        }
        std::ofstream model_report(state.output / "model.json", std::ios::binary | std::ios::trunc);
        if (!model_report) throw std::runtime_error("Cannot create decoded model report");
        omega::write_model(model_report, model, state.mesh, state.collision);
        model_report.close();
        if (!model_report) throw std::runtime_error("Cannot finish decoded model report");
        std::cout << model << ": " << state.mesh.vertices.size() << " source vertices, "
                  << state.mesh.indices.size() / 3 << " triangles, " << state.mesh.batches << " VIF batches, "
                  << state.collision.triangles.size() << " collision triangles.\n";
        if (geometry_only) std::cout << "Explicit geometry-only mode: no texture substitution or fallback.\n";
        if (!state.mesh.color_patches.empty())
            std::cout << "Retained " << state.mesh.color_patches.size() << " color patch sets; showing base colors without runtime patch updates.\n";
        if (!export_only) show(state, textures);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native geometry preview failed: " << error.what() << '\n';
        return 1;
    }
}
