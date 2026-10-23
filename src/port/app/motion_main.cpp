#include "disc.hpp"
#include "path_motion.hpp"
#include "object_events.hpp"
#include "windows_hash.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
std::size_t queued_probe(std::ostream& out, const omega::Population& population) {
    out << "{\"scope\":\"Solo deferred motion probe; no mission script binding, linked actors, audio or networking\",\"objects\":[";
    std::size_t count = 0;
    for (const auto& object : population.objects) {
        if (object.kind != 2 || object.motion.mode != 1) continue;
        omega::PathWorld world;
        world.add(object);
        if (count++) out << ',';
        out << "{\"id\":" << object.id << ",\"trace\":[";
        bool first = true;
        const auto emit = [&](const char* phase, const std::vector<omega::ObjectDelivery>& deliveries = {}) {
            if (!first) out << ',';
            first = false;
            out << "{\"phase\":\"" << phase << "\",\"pending\":" << world.pending_count() << ",\"state\":";
            omega::write_motion_state(out, world.motion(object.id));
            out << ",\"deliveries\":[";
            for (std::size_t i = 0; i < deliveries.size(); ++i) {
                if (i) out << ',';
                const auto& delivery = deliveries[i];
                const auto& message = delivery.message;
                const char* outcome = "stale";
                switch (delivery.outcome) {
                case omega::ObjectDelivery::Outcome::intercepted: outcome = "intercepted"; break;
                case omega::ObjectDelivery::Outcome::native_consumed: outcome = "native_consumed"; break;
                case omega::ObjectDelivery::Outcome::unhandled: outcome = "unhandled"; break;
                case omega::ObjectDelivery::Outcome::stale: break;
                }
                out << "{\"receiver\":" << delivery.receiver << ",\"kind\":\""
                    << (message.kind == omega::ObjectMessage::Kind::trigger ? "trigger" : "pathed_notice")
                    << "\",\"action\":" << message.action << ",\"waypoint\":" << message.waypoint
                    << ",\"outcome\":\"" << outcome << "\"}";
            }
            out << "],\"effects\":[";
            const auto effects = world.take_effects();
            for (std::size_t i = 0; i < effects.size(); ++i) {
                if (i) out << ',';
                out << "{\"receiver\":" << effects[i].receiver << ",\"kind\":\"" << effects[i].event.kind
                    << "\",\"waypoint\":" << effects[i].event.waypoint << '}';
            }
            out << "]}";
        };
        emit("initial");
        world.script_trigger(object.id); emit("script_trigger");
        world.tick(object.id, 1.F/60.F); emit("tick_before_delivery");
        emit("deliver_trigger", world.pump());
        emit("deliver_moving_notice", world.pump());
        world.tick(object.id, 1.F/60.F); emit("first_motion_tick");
        emit("deliver_tick_notices", world.pump());
        world.script_stop(object.id); emit("script_stop");
        std::vector<omega::ObjectDelivery> delivered;
        for (int i = 0; i < 120; ++i) {
            world.tick(object.id, 1.F/60.F);
            auto batch = world.pump();
            delivered.insert(delivered.end(), batch.begin(), batch.end());
        }
        emit("advance_120_ticks", delivered);
        out << "]}";
    }
    out << "]}\n";
    return count;
}
}

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::filesystem::path iso;
        std::string mission = "BELARUS1";
        bool queued = false;
        for (int i=1; i<argc; ++i) {
            const std::wstring arg=argv[i];
            if (arg==L"--help") {
                std::cout<<"Usage: omega_motion --iso <verified USA ISO> [--mission TORONTO1|BELARUS1] [--queued]\n"
                            "Runs delivered-operation probes for mode-1 path objects; no game simulation or message queue.\n";
                return 0;
            }
            if (arg==L"--iso" && i+1<argc && iso.empty()) iso=argv[++i];
            else if (arg==L"--queued") queued = true;
            else if (arg==L"--mission" && i+1<argc) {
                const std::wstring value = argv[++i];
                if (value != L"TORONTO1" && value != L"BELARUS1") throw std::runtime_error("Unsupported motion mission");
                mission = value == L"TORONTO1" ? "TORONTO1" : "BELARUS1";
            }
            else throw std::runtime_error("Unknown or incomplete motion argument");
        }
        if (iso.empty()) throw std::runtime_error("A user-supplied ISO is required");
        if (!std::filesystem::is_regular_file("config/disc_us.json"))
            throw std::runtime_error("Run the motion probe from the repository root");
        auto stream=omega::open_verified_iso(iso,OMEGA_ISO_SIZE,OMEGA_ISO_SHA256);
        omega::DiscImage disc(stream,OMEGA_ISO_SIZE);
        const auto population=omega::decode_population(disc.read(disc.file({"GAMEDATA",mission,"DATA.POP"})));
        const auto output=std::filesystem::current_path()/"local"/"runtime"/(mission=="BELARUS1"?"":mission)/"motion";
        std::filesystem::create_directories(output);
        const auto report_path = output / (queued ? "queued_trace.json" : "trace.json");
        std::ofstream report(report_path,std::ios::binary|std::ios::trunc);
        if (!report) throw std::runtime_error("Cannot create motion trace");
        if (queued) {
            const auto count = queued_probe(report, population);
            report.close();
            if (!report) throw std::runtime_error("Cannot finish queued motion trace");
            std::cout << "Probed queued delivery for " << count << " objects; saved " << report_path << '\n';
            return 0;
        }
        report<<"{\"scope\":\"Unparented kind-2/mode-1 delivered operations; no script filters, queue, linked effects, physics or replication\",\"objects\":[";
        std::size_t count=0;
        for (const auto& object:population.objects) {
            if (object.kind!=2 || object.motion.mode!=1) continue;
            omega::PathMotion motion(object);
            if(count++) report<<',';
            report<<"{\"id\":"<<object.id<<",\"trace\":[";
            bool first=true;
            const auto emit=[&](const char* op) {
                if(!first) report<<','; first=false;
                report<<"{\"op\":\""<<op<<"\",\"state\":";
                omega::write_motion_state(report,motion); report<<'}';
            };
            constexpr float dt=1.F/60.F;
            emit("initial"); motion.trigger(); emit("trigger"); motion.tick(0); emit("tick_zero");
            for(int i=0;i<12;++i) { motion.tick(dt); emit("tick"); }
            motion.stop(); emit("stop");
            for(int i=0;i<120;++i) { motion.tick(dt); emit("tick"); }
            motion.seek(static_cast<int>(object.motion.points.size())-1); emit("seek_last");
            for(int i=0;i<120;++i) { motion.tick(dt); emit("tick"); }
            motion.seek(0); emit("seek_first");
            for(int i=0;i<120;++i) { motion.tick(dt); emit("tick"); }
            report<<"]}";
        }
        report<<"],\"unsupported\":[";
        bool first=true;
        for(const auto& object:population.objects) {
            if(object.kind!=3 && !(object.kind==2 && object.motion.mode!=1)) continue;
            if(!first) report<<','; first=false;
            report<<"{\"id\":"<<object.id<<",\"reason\":\"Lift subclass or non-mode-1 path behavior is unsupported\"}";
        }
        report<<"]}\n"; report.close();
        if(!report) throw std::runtime_error("Cannot finish motion trace");
        std::cout<<"Probed "<<count<<" mode-1 path objects; saved "<<(output/"trace.json")<<"\n"
                   "Ordered local motion events only; original gameplay fidelity remains unverified.\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"Native motion probe failed: "<<error.what()<<'\n'; return 1;
    }
}
