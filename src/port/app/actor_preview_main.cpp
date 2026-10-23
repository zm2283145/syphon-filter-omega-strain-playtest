#include "animation_graph_manifest.hpp"
#include "animation_player.hpp"
#include "animation_controller.hpp"
#include "animation_model.hpp"
#include "animated_character.hpp"
#include "bone_scales.hpp"
#include "morph.hpp"
#include "disc.hpp"
#include "gfx_d3d11.hpp"
#include "native_paths.hpp"
#include "scene_renderer.hpp"
#include "skeleton_pose.hpp"
#include "skinned_mesh.hpp"
#include "windows_hash.hpp"
#include <windowsx.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

namespace {
namespace fs=std::filesystem;
template<class T>T number(const std::wstring& text) {
    std::string s;
    for(const auto c:text){if(c>127)throw std::runtime_error("Invalid preview number");s.push_back(static_cast<char>(c));}
    T value{};
    auto r=std::from_chars(s.data(),s.data()+s.size(),value);
    if(r.ec!=std::errc{}||r.ptr!=s.data()+s.size()) throw std::runtime_error("Invalid preview number");
    return value;
}
void local_path(const fs::path& path) {
    const auto root=fs::absolute("local").lexically_normal(),full=fs::absolute(path).lexically_normal();
    const auto relative=full.lexically_relative(root);
    if(relative.empty()||relative.is_absolute()||*relative.begin()==".."||full==root)
        throw std::runtime_error("Preview data must stay under local");
    fs::path prefix;
    for(const auto& part:full) {
        prefix/=part;
        if(!prefix.has_relative_path())continue;
        if(part.wstring().find(L':')!=std::wstring::npos)throw std::runtime_error("Preview path stream rejected");
        const auto attrs=GetFileAttributesW(prefix.c_str());
        if(attrs!=INVALID_FILE_ATTRIBUTES&&(attrs&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Preview path redirect rejected");
    }
}
struct Preview {
    omega::Mesh mesh;
    std::unique_ptr<omega::SceneRenderer> renderer;
    std::unique_ptr<omega::OrbitCamera> camera;
    bool paused{},wire{},minimized{};
    std::string error;
};
LRESULT CALLBACK window_proc(HWND w,UINT m,WPARAM p,LPARAM l) {
    auto* state=reinterpret_cast<Preview*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(m==WM_NCCREATE){state=static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));}
    try {
        if(!state)return DefWindowProcW(w,m,p,l);
        if(m==WM_DESTROY){PostQuitMessage(0);return 0;}
        if(m==WM_SIZE){state->minimized=p==SIZE_MINIMIZED;if(state->renderer&&!state->minimized)state->renderer->resize(LOWORD(l),HIWORD(l));return 0;}
        if(m==WM_MOUSEWHEEL&&state->camera){state->camera->zoom(float(GET_WHEEL_DELTA_WPARAM(p))/WHEEL_DELTA);return 0;}
        if(m==WM_KEYDOWN&&state->camera){
            if(p==VK_ESCAPE)DestroyWindow(w);
            if(p==VK_SPACE)state->paused=!state->paused;
            if(p=='W')state->wire=!state->wire;
            if(p==VK_LEFT)state->camera->orbit(-.1f,0);if(p==VK_RIGHT)state->camera->orbit(.1f,0);
            if(p==VK_UP)state->camera->orbit(0,.1f);if(p==VK_DOWN)state->camera->orbit(0,-.1f);return 0;
        }
        if(m==WM_PAINT){PAINTSTRUCT paint{};BeginPaint(w,&paint);EndPaint(w,&paint);return 0;}
    }catch(const std::exception& e){state->error=e.what();DestroyWindow(w);return 0;}
    return DefWindowProcW(w,m,p,l);
}
omega::Mesh draw_mesh(const std::vector<omega::AnimatedCharacterGeometry>& parts) {
    omega::Mesh mesh;
    bool first=true;
    for(const auto& part:parts) {
        const auto& shape=part.mesh;
        const auto base=static_cast<std::uint32_t>(mesh.vertices.size());
        for(const auto& v:shape.vertices) {
            const auto& placed=v.position;
            const auto& normal=v.normal;
            const omega::Vec3 p{placed[0],placed[1],placed[2]};
            mesh.vertices.push_back({p,{normal[0],normal[1],normal[2]},v.uv,{.75f,.82f,.92f,1}});
            if(first){mesh.bounds={p,p};first=false;}
            else {auto& a=mesh.bounds.minimum;auto& b=mesh.bounds.maximum;
                a={std::min(a.x,p.x),std::min(a.y,p.y),std::min(a.z,p.z)};
                b={std::max(b.x,p.x),std::max(b.y,p.y),std::max(b.z,p.z)};}
        }
        for(const auto& t:shape.triangles)for(auto i:t)mesh.indices.push_back(base+i);
    }
    if(mesh.indices.empty())throw std::runtime_error("Preview parts have no triangles");
    mesh.materials.push_back({});mesh.surfaces.push_back({0,0,static_cast<std::uint32_t>(mesh.indices.size())});
    return mesh;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        fs::path iso,manifest_path,output,morph_config_path,speech_path,grip_map_path;
        std::wstring skeleton_name=L"MALE.SKEL",motion=L"CoreMotions",next_motion,scale_name;
        std::vector<std::pair<std::string,std::string>> part_names;
        std::vector<std::pair<unsigned,float>> channels;
        struct ShapeSelection{unsigned part;std::string kind,names,name;float weight;};
        std::vector<ShapeSelection> shapes;
        struct MorphBinding{unsigned part;std::string kind,names;};
        std::vector<MorphBinding> bindings;
        omega::MorphHostState morph_state;
        omega::MorphHostInput morph_input;
        omega::AnimatedCharacterResourceInput resources;
        unsigned capture_frame=0,change_frame=30;bool hidden=false,queue=false;
        for(int i=1;i<argc;++i){const std::wstring arg=argv[i];
            auto value=[&](){if(++i>=argc)throw std::runtime_error("Missing preview argument");return std::wstring(argv[i]);};
            if(arg==L"--iso")iso=value();else if(arg==L"--graph")manifest_path=value();
            else if(arg==L"--skeleton")skeleton_name=value();else if(arg==L"--motion")motion=value();
            else if(arg==L"--part")part_names.push_back({"MANSKM.HOG",omega::asset_basename(value())});
            else if(arg==L"--morph-part")part_names.push_back({"MORPH.HOG",omega::asset_basename(value())});
            else if(arg==L"--scale")scale_name=value();
            else if(arg==L"--morph-host")morph_config_path=value();
            else if(arg==L"--speech")speech_path=value();
            else if(arg==L"--grip-map")grip_map_path=value();
            else if(arg==L"--equipment"){
                const auto slot=number<unsigned>(value()),grip=number<unsigned>(value()),other=number<unsigned>(value());
                if(slot>1||grip>255||other>255||resources.equipment[slot])throw std::runtime_error("Invalid or duplicate preview equipment");
                resources.equipment[slot]=omega::MorphEquipmentGrip{static_cast<std::uint8_t>(grip),static_cast<std::uint8_t>(other)};
            }
            else if(arg==L"--gaze-world"){
                omega::RootVector target;for(auto& component:target)component=number<float>(value());
                resources.gaze=omega::CharacterGazeInput{};resources.gaze->live_target=target;
            }
            else if(arg==L"--bind-morph"){
                const auto part=number<unsigned>(value());
                const auto kind=omega::asset_basename(value()),names=omega::asset_basename(value());
                bindings.push_back({part,kind,names});
            }
            else if(arg==L"--gaze"){
                std::array<float,3> direction;for(auto& component:direction)component=number<float>(value());
                morph_input.local_gaze=direction;
            }
            else if(arg==L"--grip"){
                const auto hand=number<unsigned>(value()),shape=number<unsigned>(value()),secondary=number<unsigned>(value()),counter=number<unsigned>(value());
                if(hand>1||shape>255||secondary>255||counter>8)throw std::runtime_error("Invalid preview grip");
                morph_state.grips[hand].primary_shape=static_cast<std::uint8_t>(shape);
                morph_state.grips[hand].counter=static_cast<std::uint8_t>(counter);
                morph_input.secondary_grip_shapes[hand]=static_cast<std::uint8_t>(secondary);
            }
            else if(arg==L"--shape"){
                const auto part=number<unsigned>(value());
                const auto kind=omega::asset_basename(value()),names=omega::asset_basename(value()),name=omega::asset_basename(value());
                const auto weight=number<float>(value());shapes.push_back({part,kind,names,name,weight});
            }
            else if(arg==L"--channel"){const auto index=number<unsigned>(value());const auto v=number<float>(value());channels.push_back({index,v});}
            else if(arg==L"--capture-frame")capture_frame=number<unsigned>(value());
            else if(arg==L"--then")next_motion=value();
            else if(arg==L"--after")change_frame=number<unsigned>(value());
            else if(arg==L"--queue")queue=true;
            else if(arg==L"--output")output=value();else if(arg==L"--hidden")hidden=true;
            else if(arg==L"--help"){std::cout<<"omega_actor --iso <owned US ISO> --graph <local manifest> --part <SKM member> [--part ...] [--morph-part <SKM member>] [--scale <BON member>] [--motion <name>] [--then <name> --after N --queue] [--channel index value] [--shape part face|hand BIN shape weight] [--morph-host <local config> --bind-morph part face|hand0|hand1 BIN --speech <local LPD> --gaze X Y Z --grip hand primary secondary counter] [--grip-map <local map> --equipment slot grip other] [--gaze-world X Y Z W] [--capture-frame N --output <new local folder> --hidden]\nSpace pauses, arrows orbit, wheel zooms, W toggles wireframe. Untextured animation diagnostic; speech ticks explicitly advance once per frame.\n";return 0;}
            else throw std::runtime_error("Unknown preview argument");
        }
        if(iso.empty()||manifest_path.empty()||part_names.empty()||part_names.size()>32||capture_frame>10000||
            (capture_frame==0)!=output.empty()||(hidden&&!capture_frame))throw std::runtime_error("Incomplete preview arguments");
        local_path(manifest_path);
        if(capture_frame){local_path(output);if(fs::exists(output))throw std::runtime_error("Capture folder must be new");}
        auto stream=omega::open_verified_iso(iso,OMEGA_ISO_SIZE,OMEGA_ISO_SHA256);
        omega::DiscImage disc(stream,OMEGA_ISO_SIZE);
        auto manifest=omega::load_animation_graph_manifest(manifest_path);
        if(manifest.iso_sha256!=OMEGA_ISO_SHA256)throw std::runtime_error("Graph and ISO provenance differ");
        const auto skas=disc.archive_member({"GAMEDATA","COMMON","SKAS.HOG"},manifest.source_skas);
        std::istringstream hash_input(std::string(skas.begin(),skas.end()));
        if(omega::sha256_stream(hash_input)!=manifest.skas_sha256)throw std::runtime_error("Graph animation-set provenance differs");
        const auto skeleton=omega::decode_skeleton(disc.archive_member({"GAMEDATA","COMMON","SKEL.HOG"},omega::asset_basename(skeleton_name)));
        omega::AnimationClipBank clips;
        for(const auto& node:manifest.graph.nodes())if(node.kind==omega::AnimationGraphKind::Leaf||node.kind==omega::AnimationGraphKind::ControlledLeaf){
            if(!clips.contains(node.leaf.clip_key))clips.emplace(node.leaf.clip_key,omega::decode_animation_clip(
                disc.archive_member({"GAMEDATA","COMMON","SKA.HOG"},node.leaf.clip_key),static_cast<unsigned>(skeleton.bones.size())));
        }
        std::vector<omega::SkinnedMesh> parts;
        for(const auto& [archive,name]:part_names)parts.push_back(omega::decode_skinned_mesh(disc.archive_member({"GAMEDATA","COMMON",archive},name)));
        if(shapes.size()>256)throw std::runtime_error("Preview shape selection count");
        for(const auto& selection:shapes)if(selection.part>=parts.size())throw std::runtime_error("Preview shape part index");
        for(std::size_t part=0;part<parts.size();++part){
            const auto first=std::find_if(shapes.begin(),shapes.end(),[&](const auto& s){return s.part==part;});
            if(first==shapes.end())continue;
            if(first->kind!="face"&&first->kind!="hand")throw std::runtime_error("Preview shape kind must be face or hand");
            if(part_names[part].first!="MORPH.HOG")throw std::runtime_error("Preview shape requires a MORPH archive part");
            const auto names=omega::decode_shape_names(disc.archive_member({"GAMEDATA","COMMON","MORPH.HOG"},first->names));
            auto skf=part_names[part].second;skf.replace(skf.size()-4,4,".SKF");
            const auto morph=omega::decode_morph(disc.archive_member({"GAMEDATA","COMMON","MORPH.HOG"},skf),
                first->kind=="face"?omega::MorphKind::face:omega::MorphKind::hand,
                static_cast<unsigned>(names.size()),static_cast<unsigned>(parts[part].chunks.size()));
            std::vector<omega::SkinMorphTarget> targets;
            for(const auto& selection:shapes)if(selection.part==part){
                if(selection.kind!=first->kind||selection.names!=first->names)throw std::runtime_error("Preview shape table mismatch");
                std::array<std::uint8_t,16> key{};
                if(selection.name.size()>key.size())throw std::runtime_error("Preview shape key length");
                std::copy(selection.name.begin(),selection.name.end(),key.begin());
                const auto found=std::find(names.begin(),names.end(),key);
                if(found==names.end())throw std::runtime_error("Preview shape key unavailable");
                targets.push_back({static_cast<unsigned>(found-names.begin()),selection.weight});
            }
            parts[part]=omega::deform_skinned_mesh(parts[part],morph,targets);
        }
        omega::MorphHostConfig morph_config;
        if(!morph_config_path.empty()){
            local_path(morph_config_path);std::ifstream config_stream(morph_config_path);
            if(!config_stream)throw std::runtime_error("Cannot read morph configuration");
            morph_config=omega::read_morph_host_config(config_stream);
        }else{
            if(!bindings.empty()||!speech_path.empty()||morph_input.local_gaze||!grip_map_path.empty()||resources.gaze||
               morph_input.secondary_grip_shapes[0]||morph_input.secondary_grip_shapes[1])
                throw std::runtime_error("Live morph controls require an extracted morph configuration");
            morph_input.expressions_enabled=false;
        }
        if(!grip_map_path.empty()){
            local_path(grip_map_path);std::ifstream map_stream(grip_map_path);
            if(!map_stream)throw std::runtime_error("Cannot read grip map");
            resources.grip_map=omega::read_morph_grip_map(map_stream);
            omega::initialize_equipment_grips(morph_state,*resources.grip_map,resources.equipment);
        }else if(resources.equipment[0]||resources.equipment[1])throw std::runtime_error("Equipment requires an extracted grip map");
        std::vector<omega::AnimatedCharacterPart> character_parts;
        for(std::size_t i=0;i<parts.size();++i)character_parts.push_back({part_names[i].second,std::move(parts[i])});
        for(const auto& binding:bindings){
            if(binding.part>=character_parts.size()||part_names[binding.part].first!="MORPH.HOG")
                throw std::runtime_error("Invalid live morph part");
            auto& part=character_parts[binding.part];
            if(part.morph||std::any_of(shapes.begin(),shapes.end(),[&](const auto& shape){return shape.part==binding.part;}))
                throw std::runtime_error("Duplicate or mixed static/live morph binding");
            if(binding.kind=="face")part.binding=omega::CharacterMorphBinding::face;
            else if(binding.kind=="hand0")part.binding=omega::CharacterMorphBinding::hand0;
            else if(binding.kind=="hand1")part.binding=omega::CharacterMorphBinding::hand1;
            else throw std::runtime_error("Invalid live morph binding role");
            const auto names=omega::decode_shape_names(disc.archive_member({"GAMEDATA","COMMON","MORPH.HOG"},binding.names));
            auto skf=part.name;skf.replace(skf.size()-4,4,".SKF");
            part.morph=omega::decode_morph(disc.archive_member({"GAMEDATA","COMMON","MORPH.HOG"},skf),
                binding.kind=="face"?omega::MorphKind::face:omega::MorphKind::hand,
                static_cast<unsigned>(names.size()),static_cast<unsigned>(part.mesh.chunks.size()));
        }
        std::vector<omega::SkinMatrix> scales;
        if(!scale_name.empty())scales=omega::bone_scale_matrices(omega::decode_bone_scales(
            disc.archive_member({"GAMEDATA","COMMON","MANSKM.HOG"},omega::asset_basename(scale_name))));
        omega::AnimatedCharacter character(skeleton,manifest,std::move(character_parts),std::move(morph_config),std::move(scales));
        character.set_morph_state(morph_state);
        if(!speech_path.empty()){
            local_path(speech_path);const auto size=fs::file_size(speech_path);
            if(size>4*1024*1024)throw std::runtime_error("Speech file exceeds limit");
            std::ifstream speech_stream(speech_path,std::ios::binary);std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if(!speech_stream.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))throw std::runtime_error("Cannot read speech file");
            character.start_speech(omega::decode_morph_speech(bytes));
        }
        for(const auto& [index,v]:channels)character.set_value(index,v);
        character.play(omega::asset_basename(motion),omega::MotionInstallMode::reset);
        std::optional<std::string> subsequent;
        if(!next_motion.empty()){subsequent=omega::asset_basename(next_motion);(void)manifest.root(*subsequent);}
        Preview state;state.mesh=draw_mesh(character.geometry());state.camera=std::make_unique<omega::OrbitCamera>(state.mesh.bounds);
        const auto instance=GetModuleHandleW(nullptr);WNDCLASSW type{};type.hInstance=instance;type.lpfnWndProc=window_proc;
        type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.lpszClassName=L"OmegaActorAnimation";
        if(!RegisterClassW(&type))throw std::runtime_error("Cannot register animation window");
        const auto window=CreateWindowExW(0,type.lpszClassName,(L"Animation preview | "+motion+L" | Space pause, arrows orbit, W wireframe").c_str(),
            WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1000,760,nullptr,nullptr,instance,&state);
        if(!window)throw std::runtime_error("Cannot create animation window");
        struct Close{HWND window;~Close(){if(IsWindow(window))DestroyWindow(window);}}close{window};
        state.renderer=std::make_unique<omega::SceneRenderer>(omega::gfx::d3d11::create_window_display(window),state.mesh,omega::CollisionMesh{},std::vector<omega::Texture>{});
        if(!hidden)ShowWindow(window,capture_frame?SW_SHOWNOACTIVATE:SW_SHOWNORMAL);
        unsigned frame=0;bool running=true;
        if(capture_frame)fs::create_directories(output);
        while(running){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            if(message.message==WM_QUIT){running=false;break;}TranslateMessage(&message);DispatchMessageW(&message);}
            if(!running)break;if(state.minimized){Sleep(30);continue;}
            if(!state.paused){
                if(subsequent&&frame==change_frame){character.play(*subsequent,
                    queue?omega::MotionInstallMode::queue:omega::MotionInstallMode::replace);subsequent.reset();}
                morph_input.speech_elapsed_tick=static_cast<std::int32_t>(frame);
                character.step(manifest.graph,clips,{},morph_input,1.0f/60.0f,{},resources);
                state.mesh=draw_mesh(character.geometry());state.renderer->refresh_geometry();
                state.camera->target={(state.mesh.bounds.minimum.x+state.mesh.bounds.maximum.x)*.5f,
                    (state.mesh.bounds.minimum.y+state.mesh.bounds.maximum.y)*.5f,
                    (state.mesh.bounds.minimum.z+state.mesh.bounds.maximum.z)*.5f};++frame;}
            state.renderer->draw(*state.camera,{false,state.wire,false});
            if(capture_frame&&frame>=capture_frame){state.renderer->capture(output/"animation.bmp");break;}
            if(!hidden)state.renderer->present();
        }
        if(!state.error.empty())throw std::runtime_error(state.error);
        if(capture_frame){if(frame<capture_frame)throw std::runtime_error("Animation closed before capture");
            const auto& controller=character.model().controller();const auto& player=character.model().player();
            std::ofstream report(output/"animation.json");report<<std::setprecision(9)<<"{\"frames\":"<<frame<<",\"vertices\":"<<state.mesh.vertices.size()
                <<",\"triangles\":"<<state.mesh.indices.size()/3<<",\"active_motions\":"<<controller.active.size()
                <<",\"queued_motions\":"<<controller.queued.size()<<",\"applied_morph_targets\":"<<shapes.size()
                <<",\"live_morph_parts\":"<<bindings.size()<<",\"face_targets\":"<<static_cast<unsigned>(character.morph().active_count)
                <<",\"speech_active\":"<<(character.morph().speech_active?"true":"false")
                <<",\"position\":["<<player.position[0]<<','<<player.position[1]<<','<<player.position[2]<<"]}";
            if(!report)throw std::runtime_error("Cannot write animation capture report");}
        return 0;
    }catch(const std::exception& e){std::cerr<<"Animation preview failed: "<<e.what()<<'\n';return 1;}
}


