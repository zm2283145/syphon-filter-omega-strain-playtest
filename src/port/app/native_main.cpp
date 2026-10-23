#include "disc.hpp"
#include "objectives.hpp"
#include "objective_schedule.hpp"
#include "objective_stage_probe.hpp"
#include "objective_registration.hpp"
#include "localization.hpp"
#include "script_start_probe.hpp"
#include "windows_hash.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

int wmain(int argc, wchar_t* argv[]) {
    try {
        std::filesystem::path iso;
        std::wstring mode = L"both";
        std::wstring mission = L"BELARUS1";
        bool status_probe = false;
        bool schedule_probe = false;
        bool stage_probe = false;
        bool outcome_probe = false;
        bool progression_probe = false;
        bool progression_mapped_probe = false;
        bool completion_probe = false;
        bool completion_voice_probe = false;
        bool proust_probe = false;
        bool localization_probe = false;
        bool solo_end_probe = false;
        bool script_start_probe = false;
        for (int i = 1; i < argc; ++i) {
            const std::wstring argument = argv[i];
            if (argument == L"--help") {
                std::cout << "Usage: omega_objectives --iso <verified USA ISO> [--mode solo|coop|both]\n"
                             "                        [--mission TORONTO1|BELARUS1] (default BELARUS1) [--status-probe] [--schedule-probe] [--stage-probe] [--outcome-probe] [--progression-probe] [--progression-mapped-probe] [--completion-probe] [--completion-voice-probe]\n"
                             "                        [--proust-probe] [--localization-probe] [--solo-end-probe] (solo Carthage diagnostics)\n"
                             "                        [--script-start-probe] (Carthage script level start, hosting S1)\n"
                             "Run from the repository root. This is a console research prototype, not a playable game.\n";
                return 0;
            }
            if (argument == L"--iso" && i + 1 < argc && iso.empty()) iso = argv[++i];
            else if (argument == L"--mode" && i + 1 < argc) mode = argv[++i];
            else if (argument == L"--mission" && i + 1 < argc) mission = argv[++i];
            else if (argument == L"--status-probe" && !status_probe) status_probe = true;
            else if (argument == L"--schedule-probe" && !schedule_probe) schedule_probe = true;
            else if (argument == L"--stage-probe" && !stage_probe) stage_probe = true;
            else if (argument == L"--outcome-probe" && !outcome_probe) outcome_probe = true;
            else if (argument == L"--progression-probe" && !progression_probe) progression_probe = true;
            else if (argument == L"--progression-mapped-probe" && !progression_mapped_probe) progression_mapped_probe = true;
            else if (argument == L"--completion-probe" && !completion_probe) completion_probe = true;
            else if (argument == L"--completion-voice-probe" && !completion_voice_probe) completion_voice_probe = true;
            else if (argument == L"--proust-probe" && !proust_probe) proust_probe = true;
            else if (argument == L"--localization-probe" && !localization_probe) localization_probe = true;
            else if (argument == L"--solo-end-probe" && !solo_end_probe) solo_end_probe = true;
            else if (argument == L"--script-start-probe" && !script_start_probe) script_start_probe = true;
            else throw std::runtime_error("Unknown or incomplete argument; use --help");
        }
        if (iso.empty()) throw std::runtime_error("A user-supplied ISO is required; use --iso <path>");
        if (mode != L"solo" && mode != L"coop" && mode != L"both")
            throw std::runtime_error("Mode must be solo, coop or both");
        if (mission != L"TORONTO1" && mission != L"BELARUS1")
            throw std::runtime_error("Mission must be TORONTO1 or BELARUS1");
        const std::string mission_name = mission == L"TORONTO1" ? "TORONTO1" : "BELARUS1";
        if (schedule_probe && mission != L"TORONTO1") throw std::runtime_error("Schedule probe requires Carthage");
        if (stage_probe && mission != L"TORONTO1") throw std::runtime_error("Stage probe requires Carthage");
        if (outcome_probe && mission != L"TORONTO1") throw std::runtime_error("Outcome probe requires Carthage");
        if (progression_probe && mission != L"TORONTO1") throw std::runtime_error("Progression probe reports require Carthage");
        if (progression_mapped_probe && mission != L"TORONTO1") throw std::runtime_error("Mapped progression probe requires Carthage");
        if (completion_probe && mission != L"TORONTO1") throw std::runtime_error("Completion probe requires Carthage");
        if (completion_voice_probe && mission != L"TORONTO1") throw std::runtime_error("Completion voice probe requires Carthage");
        if ((proust_probe || localization_probe || solo_end_probe) && mission != L"TORONTO1")
            throw std::runtime_error("Population objective and localization probes require Carthage");
        if (script_start_probe && mission != L"TORONTO1") throw std::runtime_error("Script start probe requires Carthage");
        if (script_start_probe && mode != L"solo") throw std::runtime_error("Script start probe is solo only");
        const std::string mission_basename = mission == L"TORONTO1" ? "toronto1" : "belarus1";
        if (!std::filesystem::is_regular_file("config/disc_us.json"))
            throw std::runtime_error("Run this prototype from the repository root");
        std::cout << "Native research prototype: no rendering, playable gameplay or networking.\n"
                     "Verifying the complete user-supplied ISO...\n";
        auto stream = omega::open_verified_iso(iso, OMEGA_ISO_SIZE, OMEGA_ISO_SHA256);
        omega::DiscImage disc(stream, OMEGA_ISO_SIZE);
        std::cout << "Mission: " << mission_name << '\n';
        const auto raw = disc.archive_member({"GAMEDATA", mission_name, "SCRIPTS.HOG"}, "OBJECTIVES.SO");
        const auto script = omega::parse_script(raw);
        auto output = std::filesystem::current_path() / "local" / "runtime";
        if (mission == L"TORONTO1") output /= mission_name;
        std::filesystem::create_directories(output);
        omega::write_bytes(output / (mission_name + "_OBJECTIVES.SO"), raw);
        std::vector<omega::ObjectiveRun> runs;
        if (mode != L"coop") runs.push_back(omega::run_objectives(script, false));
        if (mode != L"solo") runs.push_back(omega::run_objectives(script, true));
        const auto report_path = output / (mission_basename + "_native_objectives.json");
        std::ofstream report(report_path, std::ios::binary | std::ios::trunc);
        if (!report) throw std::runtime_error("Cannot create native objective report");
        omega::write_objective_runs(report, runs);
        report.close();
        if (!report) throw std::runtime_error("Could not finish writing native objective report");
        if (script_start_probe) {
            const std::vector<std::string> archive{"GAMEDATA", mission_name, "SCRIPTS.HOG"};
            std::map<std::string, omega::Script> modules;
            for (const auto& member : disc.archive_member_names(archive)) {
                auto key = member.substr(0, member.find('.'));
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                modules.emplace(key, omega::parse_script(disc.archive_member(archive, member)));
            }
            std::ofstream probe(output / (mission_basename + "_script_start.json"), std::ios::binary | std::ios::trunc);
            omega::write_script_start_probe(probe, modules,
                omega::decode_population(disc.read(disc.file({"GAMEDATA", mission_name, "DATA.POP"}))));
            probe.close();
            if (!probe) throw std::runtime_error("Cannot finish script start report");
        }
        if (status_probe) {
            std::vector<bool> modes;
            if (mode != L"coop") modes.push_back(false);
            if (mode != L"solo") modes.push_back(true);
            std::ofstream statuses(output / (mission_basename + "_objective_status.json"),
                                   std::ios::binary | std::ios::trunc);
            if (!statuses) throw std::runtime_error("Cannot create objective status report");
            omega::write_objective_status_probe(statuses, script, modes);
            statuses.close();
            if (!statuses) throw std::runtime_error("Could not finish writing objective status report");
        }
        if (schedule_probe) {
            std::ostringstream trace;
            omega::write_objective_schedule_probe(trace, script);
            std::ofstream schedule(output / (mission_basename + "_objective_schedule.json"), std::ios::binary | std::ios::trunc);
            schedule << trace.str() << '\n';
            schedule.close();
            if (!schedule) throw std::runtime_error("Cannot finish objective schedule report");
        }
        if (stage_probe) {
            std::ostringstream trace;
            omega::write_objective_stage_probe(trace, script);
            std::ofstream stage(output / (mission_basename + "_objective_stage.json"), std::ios::binary | std::ios::trunc);
            stage << trace.str() << '\n';
            stage.close();
            if (!stage) throw std::runtime_error("Cannot finish objective stage report");
        }
        if (progression_probe) {
            const auto awards_raw = disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "AWARDS.SO");
            const auto awards = omega::parse_script(awards_raw);
            std::ostringstream trace;
            omega::write_objective_registration_probe(trace, awards);
            std::ofstream progression(output / (mission_basename + "_objective_progression.json"), std::ios::binary | std::ios::trunc);
            progression << trace.str();
            progression.close();
            if (!progression) throw std::runtime_error("Cannot finish objective progression report");
        }
        if (progression_mapped_probe) {
            const auto awards = omega::parse_script(disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "AWARDS.SO"));
            const auto pool = omega::decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"})));
            std::ostringstream trace;
            omega::write_objective_registration_probe(trace, awards, pool);
            std::ofstream progression(output / (mission_basename + "_objective_progression_mapped.json"), std::ios::binary | std::ios::trunc);
            progression << trace.str();
            progression.close();
            if (!progression) throw std::runtime_error("Cannot finish mapped objective progression report");
        }
        if (completion_probe) {
            const auto awards = omega::parse_script(disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "AWARDS.SO"));
            const auto pool = omega::decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"})));
            std::ostringstream trace;
            omega::write_objective_completion_probe(trace, script, awards, pool);
            std::ofstream completion(output / (mission_basename + "_objective_completion.json"), std::ios::binary | std::ios::trunc);
            completion << trace.str();
            completion.close();
            if (!completion) throw std::runtime_error("Cannot finish objective completion report");
        }
        if (completion_voice_probe) {
            const auto awards = omega::parse_script(disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "AWARDS.SO"));
            const auto pool = omega::decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"})));
            const auto voice = omega::parse_script(disc.archive_member({"GAMEDATA", mission_name, "SCRIPTS.HOG"}, "VOICE.SO"));
            std::ostringstream trace;
            omega::write_objective_voice_probe(trace, script, awards, pool, voice);
            std::ofstream completion(output / (mission_basename + "_objective_voice.json"), std::ios::binary | std::ios::trunc);
            completion << trace.str();
            completion.close();
            if (!completion) throw std::runtime_error("Cannot finish objective voice report");
        }
        if (proust_probe || localization_probe || solo_end_probe) {
            const auto base_pool = omega::decode_string_pool(disc.read(disc.file({"GAMEDATA", "COMMON", "STRINGS.DAT"})));
            const auto mission_pool = omega::decode_string_pool(disc.read(disc.file({"GAMEDATA", mission_name, "STRINGS.DAT"})));
            if (proust_probe || solo_end_probe) {
                const auto awards = omega::parse_script(disc.archive_member({"GAMEDATA", "COMMON", "SCRIPTS.HOG"}, "AWARDS.SO"));
                const auto voice = omega::parse_script(disc.archive_member({"GAMEDATA", mission_name, "SCRIPTS.HOG"}, "VOICE.SO"));
                const auto population = omega::decode_population(disc.read(disc.file({"GAMEDATA", mission_name, "DATA.POP"})));
                for (bool ending : {false, true}) {
                    if (ending ? !solo_end_probe : !proust_probe) continue;
                    std::ostringstream trace;
                    if (ending) omega::write_objective_solo_end_probe(trace, script, awards, base_pool, mission_pool, voice, population);
                    else omega::write_objective_proust_probe(trace, script, awards, base_pool, mission_pool, voice, population);
                    const auto suffix = ending ? "_objective_solo_end.json" : "_objective_proust.json";
                    std::ofstream result(output / (mission_basename + suffix), std::ios::binary | std::ios::trunc);
                    result << trace.str() << '\n';
                    result.close();
                    if (!result) throw std::runtime_error("Cannot finish population objective report");
                }
            }
            if (localization_probe) {
                std::ostringstream trace;
                omega::write_localization_probe(trace, base_pool, mission_pool);
                std::ofstream result(output / (mission_basename + "_localization.json"), std::ios::binary | std::ios::trunc);
                result << trace.str() << '\n';
                result.close();
                if (!result) throw std::runtime_error("Cannot finish localization report");
            }
        }
        if (outcome_probe) {
            std::ostringstream trace;
            omega::write_objective_outcome_probe(trace, script);
            std::ofstream outcomes(output / (mission_basename + "_objective_outcome.json"), std::ios::binary | std::ios::trunc);
            outcomes << trace.str();
            outcomes.close();
            if (!outcomes) throw std::runtime_error("Cannot finish objective outcome report");
        }
        for (const auto& run : runs) {
            std::cout << (run.multiplayer ? "Co-op" : "Solo") << ": " << run.objectives.size()
                      << " objectives, " << run.steps << " entry instructions, "
                      << run.calls.size() << " recorded callbacks.\n";
        }
        std::cout << "Extracted script and complete report: " << output.string() << '\n'
                  << "Population objective/localization probes resolve mission text; other reports retain symbolic keys. No original-game execution equivalence is claimed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native prototype failed: " << error.what() << '\n';
        return 1;
    }
}
