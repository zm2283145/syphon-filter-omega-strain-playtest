cmake_minimum_required(VERSION 3.24)
project(OmegaNativeResearch VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
    # Embedded symbols avoid the shared compiler-PDB service; the linker still emits a debug PDB.
    string(REGEX REPLACE "(^| )/Zi($| )" "\\1/Z7\\2" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Checkpoint 132vt1 (port-only): the PS Vita build (VitaSDK toolchain; research/VITA.md, tools/build_vita.ps1).
if(VITA)
    include(cmake/vita/OmegaVita.cmake)
endif()
# Checkpoint 132ps5c (port-only): the PS5 build (ps5-payload-sdk + ps5-opengl; research/PS5.md, tools/build_ps5.ps1).
if(OMEGA_PS5)
    include(cmake/ps5/OmegaPs5.cmake)
endif()

add_library(omega_core
    src/port/platform/worker_pool.cpp
    src/port/platform/small_alloc.cpp
    src/port/platform/thread_roles.cpp
    src/port/render/draw_prepare.cpp
    src/port/render/particle_tail.cpp
    src/main/scriptManager/script.cpp
    src/main/scriptManager/script_context.cpp
    src/main/sinit/engine_variables.cpp
    src/main/sinit/engine_globals.cpp
    src/port/probes/script_start_probe.cpp
    src/main/scriptUtils/mission_script_host.cpp
    src/port/play/mission_script_npcs.cpp
    src/main/GobjMan/script_objects_runtime.cpp
    src/main/gameSound/voice_sound_bridge.cpp
    src/main/globalization/localization.cpp
    src/main/hud/hud_script_state.cpp
    src/main/scriptManager/script_scheduler.cpp
    src/port/probes/objective_schedule.cpp
    src/main/Objective/objective_stage.cpp
    src/main/Objective/objective_outcome.cpp
    src/main/Objective/objective_markers.cpp
    src/main/inventory/item_inventory.cpp
    src/main/ai/team_relations.cpp
    src/main/Objective/objective_progression.cpp
    src/main/Objective/objective_registration.cpp
    src/main/globalization/string_pool.cpp
    src/port/probes/objective_stage_probe.cpp
    src/main/Objective/objectives.cpp
    src/main/Objective/objectives_overlay.cpp
    src/main/backpack/pickup_menu.cpp
    src/port/disc/disc.cpp
    src/port/mods/audio_files.cpp
    src/port/mods/bank_sounds.cpp
    src/port/mods/image_files.cpp
    src/port/mods/mod_archive.cpp
    src/port/mods/mod_fingerprint.cpp
    src/port/mods/mod_manager.cpp
    src/port/mods/mod_movie.cpp
    src/port/platform/platform_movie.cpp
    src/port/mods/mod_set.cpp
    src/port/mods/data_overrides.cpp
    src/port/mods/texture_replacement.cpp
    src/port/gfx/gltf.cpp src/port/gfx/character_model.cpp src/port/gfx/mesh_simplify.cpp src/port/gfx/static_model.cpp src/port/gfx/texture_blocks.cpp src/port/mods/model_overrides.cpp
    src/main/texman/texture.cpp
    src/main/texman/texture_binding.cpp
    src/main/vu1model/geometry.cpp
    src/main/vu1model/gs_alpha.cpp
    src/main/vu1model/scene_draw_order.cpp
    src/main/vu1model/material_anim.cpp
    src/main/population/population.cpp
    src/main/GameGOBJ/path_motion.cpp
    src/main/GameGOBJ/elevator_gobj.cpp
    src/main/GameGOBJ/tank.cpp
    src/main/GameGOBJ/sound_gobj.cpp
    src/main/Loader/zone_activation.cpp
    src/main/GameGOBJ/object_events.cpp
    src/main/collision/collision_face.cpp
    src/main/manUtils/appearance.cpp
    src/main/manUtils/character_appearance.cpp
    src/main/manUtils/man_sort.cpp
    src/main/manUtils/appearance_parts.cpp
    src/main/manUtils/agent_modify.cpp
    src/main/manUtils/agent_create.cpp
    src/main/Loader/npc_roster.cpp
    src/main/ska/animation_channel.cpp
    src/main/ska/animation_transition.cpp
    src/main/ska/animation_slider.cpp
    src/main/ska/animation_group.cpp
    src/main/ska/animation_graph.cpp
    src/main/ska/animation_graph_manifest.cpp
    src/main/ska/animation_player.cpp
    src/main/ska/animation_controller.cpp
    src/main/ska/animation_model.cpp
    src/main/ska/clip_playback.cpp
    src/main/skeleton/skeleton.cpp
    src/main/ska/animation_clip.cpp
    src/main/ska/animation_sampling.cpp
    src/main/ska/animation_pose.cpp
    src/main/ska/animation_root.cpp
    src/main/ska/animation_root_clip.cpp
    src/main/skeleton/skeleton_pose.cpp
    src/main/vu1model/skinned_mesh.cpp
    src/main/skeleton/bone_scales.cpp
    src/main/man/morph.cpp
    src/main/man/morph_host.cpp
    src/main/man/morph_resources.cpp
    src/main/human/actor_animation.cpp
    src/main/human/actor_navigation.cpp
    src/main/ska/animation_vector_curve.cpp
    src/main/human/actor_position_curve.cpp
    src/main/grenade/projectile_launch.cpp
    src/main/ska/actor_motion.cpp
    src/main/ska/level_animation.cpp
    src/main/ska/animation_rotation_curve.cpp
    src/main/human/actor_curve_update.cpp
    src/main/playerControls/actor_heading_state.cpp
    src/main/grenade/projectile_simulation.cpp
    src/main/grenade/projectile_candidates.cpp
    src/main/collision/los_query.cpp
    src/main/collision/los_provider.cpp
    src/main/collision/los_frame.cpp
    src/main/collision/col_gather.cpp
    src/main/collision/collision_triangles.cpp
    src/main/collision/collision_collection.cpp
    src/main/collision/live_gather.cpp
    src/main/humanCollision/actor_floor_response.cpp
    src/main/gobj/actor_world_history.cpp
    src/main/humanCollision/actor_contact_events.cpp
    src/main/humanCollision/actor_body_query.cpp
    src/main/humanCollision/actor_specialized_body_query.cpp
    src/main/collision/moving_collision_slide.cpp
    src/main/humanCollision/actor_response_placement.cpp
    src/main/human/actor_fader.cpp
    src/main/game/game_end_state.cpp
    src/main/machine/game_flow.cpp
    src/main/machine/level_load.cpp
    src/main/machine/level_load_state.cpp
    src/main/scriptManager/loadout_state.cpp
    src/main/machine/lobby_state.cpp
    src/main/machine/gameplay_state.cpp
    src/main/machine/app_teardown.cpp
    src/main/machine/card_wait_poll.cpp
    src/main/machine/teardown_state.cpp
    src/main/machine/world_teardown.cpp
    src/main/machine/level_teardown_callees.cpp
    src/main/gobj/static_instance.cpp
    src/main/xlib/video_frame.cpp
    src/main/xlib/fstream_loader.cpp
    src/main/game/remote_players_ready.cpp
    src/main/machine/play_native_flow.cpp
    src/port/play/play_level_teardown.cpp
    src/main/ai/team_setup.cpp
    src/main/gameFrontEnd/level_selector.cpp
    src/main/human/spawned_actor_ctor.cpp
    src/main/GobjMan/object_dynamic_id.cpp
    src/main/Generator/spawned_npc_msg.cpp
    src/main/AgentData/agent_perks.cpp
    src/main/npc/spawned_actor_ai.cpp
    src/main/gobj/actor_component_attach.cpp
    src/main/human/spawned_npc_dtor.cpp
    src/main/human/spawned_npc_teardown.cpp
    src/main/ai/spawned_actor_ai_base.cpp
    src/main/npc/ai_runtime_init.cpp
    src/main/gameGobjController/ai_component_bind.cpp
    src/main/human/spawned_npc_body.cpp
    src/main/weapon/inventory_services.cpp
    src/main/human/actor_checkpoint.cpp
    src/main/game/actor_frame_service.cpp
    src/main/humanCollision/actor_physical_prediction.cpp
    src/main/humanCollision/actor_physical_state.cpp
    src/main/humanCollision/actor_physical_initialization.cpp
    src/main/humanCollision/actor_auxiliary_probe.cpp
    src/main/humanCollision/actor_movement_adjustment.cpp
    src/main/humanCollision/actor_support_history.cpp
    src/main/humanCollision/actor_support_motion.cpp
    src/main/skeleton/actor_model_context.cpp
    src/main/human/actor_ladder.cpp
    src/main/human/actor_ladder_bindings.cpp
    src/main/man/actor_model_slots.cpp
    src/main/skeleton/attachment_transforms.cpp
    src/main/human/root_attachments.cpp
    src/main/human/actor_attachment_runtime.cpp
    src/main/game/actor_world_stack.cpp
    src/main/human/actor_animation_receiver.cpp
    src/main/human/actor_quick_turn.cpp
    src/main/ska/animated_character.cpp
    src/main/WaterFx/water_effects.cpp
    src/main/WaterFx/water_effect_draw.cpp
    src/main/humanCollision/actor_movement_scheduler.cpp
    src/main/humanCollision/actor_response_probes.cpp
    src/main/humanCollision/actor_response_setup.cpp
    src/main/humanCollision/actor_physical_response.cpp
    src/main/humanCollision/actor_gather_bounds.cpp
    src/main/humanCollision/actor_water_support.cpp
    src/main/humanCollision/actor_physical_frame.cpp
    src/main/humanCollision/actor_edge_presets.cpp src/main/humanCollision/actor_edge_detection.cpp src/main/playerControls/player_pad_control.cpp src/port/play/solo_actor_simulation.cpp src/port/play/actor_world.cpp src/main/path/npc_path_follower.cpp src/main/npc/npc_ai.cpp src/main/npc/npc_world.cpp src/main/npc/npc_remote.cpp src/main/Generator/npc_generator.cpp src/port/platform/float_environment.cpp
    src/main/playerControls/player_action_buttons.cpp src/main/human/actor_pad_actions.cpp src/main/weapon/actor_reload.cpp src/main/human/actor_flashbang_hide.cpp src/main/human/actor_goggles.cpp src/main/human/actor_roll_action.cpp src/main/human/actor_cancel_action.cpp src/main/human/actor_ledge_action.cpp src/main/human/actor_body_carry.cpp src/main/human/actor_object_action.cpp src/main/human/actor_hanging.cpp src/main/human/actor_damage.cpp src/main/human/actor_death.cpp src/main/human/actor_choke.cpp src/lib/game_libm.cpp src/main/lights/character_shadows.cpp
    src/main/gameLight/level_lights.cpp src/main/lights/shadow_scene.cpp src/main/human/caster_bounds.cpp src/main/human/actor_model_defaults.cpp
    src/main/hotbox/hotbox_world.cpp src/main/human/actor_running_jump.cpp src/main/human/actor_teleport.cpp
    src/main/gameCamera/camera_curves.cpp src/main/gameCamera/camera_rig.cpp src/main/gameCamera/camera_zoom.cpp src/main/gameCamera/player_camera.cpp src/main/gameCamera/camera_collision.cpp
)
# Every folder under src is an include directory: sources include each other by
# file name only (research/SOURCE_LAYOUT.md, tools/source_layout.csv).
file(GLOB_RECURSE OMEGA_SOURCE_FILES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" CONFIGURE_DEPENDS
    src/*.cpp src/*.hpp src/*.h src/*.c src/*.inl)
set(OMEGA_SOURCE_DIRS src)
foreach(omega_source_file IN LISTS OMEGA_SOURCE_FILES)
    get_filename_component(omega_source_dir "${omega_source_file}" DIRECTORY)
    list(APPEND OMEGA_SOURCE_DIRS "${omega_source_dir}")
endforeach()
list(REMOVE_DUPLICATES OMEGA_SOURCE_DIRS)
list(SORT OMEGA_SOURCE_DIRS)
target_include_directories(omega_core PUBLIC ${OMEGA_SOURCE_DIRS})
set_source_files_properties(src/main/collision/collision_face.cpp src/main/manUtils/character_appearance.cpp src/main/ska/animation_channel.cpp src/main/ska/animation_transition.cpp src/main/ska/animation_slider.cpp src/main/skeleton/skeleton_pose.cpp src/main/ska/animation_sampling.cpp src/main/ska/animation_pose.cpp src/main/ska/animation_root.cpp src/main/ska/animation_root_clip.cpp src/main/scriptManager/script.cpp src/main/hotbox/hotbox_world.cpp src/main/human/actor_running_jump.cpp src/main/human/actor_fader.cpp src/main/human/actor_checkpoint.cpp src/main/human/actor_teleport.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Combat (research/COMBAT.md, milestones C1a/C1b).
set(OMEGA_COMBAT_SOURCES src/main/weapon/weapon_database.cpp src/main/weapon/weapon_inventory.cpp src/main/weapon/weapon_field_of_view.cpp src/main/GuiEquipmentModify/equipment_loadout.cpp src/port/play/play_loadout.cpp src/main/AgentData/agent_save.cpp src/main/AgentData/agent_load.cpp src/main/AgentData/agent_refill.cpp src/main/AgentData/agent_parts.cpp src/main/AgentData/award_registry.cpp src/main/AgentData/awards_agent_host.cpp src/main/AgentData/profile_awards.cpp src/main/AgentData/parts_table.cpp src/main/MemCard/agent_save_flow.cpp src/main/MemCard/agent_card.cpp src/main/gui/frontend_formats.cpp src/main/gameGobjController/weapon_aim.cpp src/main/human/bullet_hit.cpp src/main/weapon/weapon_discharge.cpp src/main/bullet/bullet_pool.cpp src/main/bullet/bullet_render.cpp src/main/bullet/bullet_impact.cpp src/main/human/actor_fire.cpp src/port/play/combat_world.cpp src/main/weapon/weapon_update.cpp src/main/player/weapon_shot_frame.cpp src/main/hud/weapon_hud.cpp src/main/hud/hud_crosshair.cpp src/main/man/actor_hit_volumes.cpp src/main/man/model_attach_points.cpp src/main/man/model_stream_wait.cpp src/main/human/secondary_component.cpp src/port/play/combat_actor_world.cpp src/main/playerControls/player_weapon_input.cpp src/main/playerControls/player_targeting.cpp src/main/human/actor_aim_actions.cpp src/main/ai/npc_perception.cpp src/main/npc/npc_death_notice.cpp src/main/human/secondary_aim.cpp src/main/human/upper_body_pose.cpp src/main/human/upper_body_ik.cpp src/main/human/upper_body_combine.cpp src/main/human/upper_body_pass.cpp src/main/targetManager/target_visibility.cpp src/main/hudTargets/hud_targets.cpp src/main/collision/world_los.cpp src/port/play/combat_play.cpp src/port/play/pickup_menu_play.cpp src/main/AgentData/agent_statistics.cpp src/main/AgentData/stats_record.cpp src/main/human/hit_direction.cpp src/main/human/npc_dead_drop.cpp src/port/play/blast_world.cpp src/main/backpack/weapon_backpack.cpp src/main/backpack/backpack_object.cpp src/main/backpack/backpack_draw.cpp src/main/man/gun_model_load.cpp src/main/weapon/weapon_models.cpp src/main/man/gun_model_draw.cpp src/main/man/shell_eject.cpp src/main/playerControls/pad_interact_prompt.cpp src/main/playerControls/player_weapon_switch.cpp src/main/man/man_draw_gate.cpp src/port/play/port_cheats.cpp src/port/play/coop_partner.cpp src/port/play/coop_breadcrumbs.cpp src/port/play/coop_command_menu.cpp src/port/play/coop_partner_kit.cpp src/main/GameGOBJ/interact_gobj_pairing.cpp)
target_sources(omega_core PRIVATE ${OMEGA_COMBAT_SOURCES})
target_sources(omega_core PRIVATE src/main/AgentData/agent_network_codec.cpp)
target_sources(omega_core PRIVATE src/main/AgentData/agent_network_receive.cpp)
set_source_files_properties(${OMEGA_COMBAT_SOURCES} PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Combat items (research/COMBAT_ITEMS.md, milestones C7a/C6a/C7b/C6b).
set(OMEGA_COMBAT_ITEM_SOURCES src/main/GameGOBJ/explosion.cpp src/main/grenade/thrown_projectile.cpp src/main/grenade/area_effect.cpp src/main/human/secondary_playback.cpp src/main/human/actor_throw.cpp src/port/play/throw_world.cpp src/main/human/actor_melee.cpp src/main/game/takedown_camera.cpp src/main/human/actor_stun.cpp src/main/human/actor_effect_slots.cpp src/main/human/actor_stunned_action.cpp src/main/weapon/torch_light.cpp src/main/weapon/air_taser.cpp src/main/human/actor_ignition.cpp src/main/human/actor_burning.cpp src/main/NIEvent/ni_event_object.cpp src/main/gui/figure_camera.cpp src/main/gobj/gobj_damage.cpp src/main/skybox/skybox.cpp src/main/decal/decal_manager.cpp)
target_sources(omega_core PRIVATE ${OMEGA_COMBAT_ITEM_SOURCES})
set_source_files_properties(${OMEGA_COMBAT_ITEM_SOURCES} PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# The agent screens' camera presets (research/FRONTEND.md, Checkpoint 132cc6).
target_sources(omega_core PRIVATE src/main/manUtils/agent_camera.cpp)
set_source_files_properties(src/main/manUtils/agent_camera.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# The throw flow in the combat play session (research/COMBAT_ITEMS.md, "Play wiring").
target_sources(omega_core PRIVATE src/port/play/combat_throw_play.cpp)
set_source_files_properties(src/port/play/combat_throw_play.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Flashlight (research/FLASHLIGHT.md, milestones F1/F2).
target_sources(omega_core PRIVATE src/main/interface_model/ie_screen.cpp src/main/hud/hud_object.cpp src/main/hud/hud_radar.cpp src/main/hud/hud_markers.cpp src/main/hud/hud_callouts.cpp src/main/hud/hud_update.cpp src/main/hud/hud_screen_draw.cpp src/main/hud/hud_loading.cpp src/main/hud/hud_map.cpp)
set_source_files_properties(src/main/interface_model/ie_screen.cpp src/main/hud/hud_object.cpp src/main/hud/hud_radar.cpp src/main/hud/hud_markers.cpp src/main/hud/hud_callouts.cpp src/main/hud/hud_update.cpp src/main/hud/hud_screen_draw.cpp src/main/hud/hud_loading.cpp src/main/hud/hud_map.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# gameSound.cc's sound manager and request layer (research/GENERATOR_RUNTIME.md, checkpoint 119i).
# Voice lines 0018c770 / 00245400 and the static sound descriptors (checkpoint 119n).
target_sources(omega_core PRIVATE src/main/gameSound/game_sound.cpp src/main/gameSound/game_sound_voice.cpp)
set_source_files_properties(src/main/gameSound/game_sound.cpp src/main/gameSound/game_sound_voice.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Walking footsteps: the material property table and the gait / foot probe / dry branch (checkpoint 119m).
target_sources(omega_core PRIVATE src/main/collision/material_properties.cpp src/main/human/actor_footstep.cpp)
set_source_files_properties(src/main/collision/material_properties.cpp src/main/human/actor_footstep.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Disc audio decoding, the host mixer / WAV output and the play 989snd host (checkpoint 119i).
target_sources(omega_core PRIVATE src/port/platform/audio_formats.cpp src/port/platform/audio_output.cpp src/port/play/sound_iop_host.cpp src/port/play/spu2_voices.cpp src/lib/989snd/snd_volume.cpp src/lib/989snd/snd_bank.cpp)
target_sources(omega_core PRIVATE src/main/flashlight/flashlight.cpp)
set_source_files_properties(src/main/flashlight/flashlight.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# The manual aim view's lock and picks (research/COMBAT.md, "Checkpoint 117 aim view entry and lock").
target_sources(omega_core PRIVATE src/main/playerControls/aim_view_targeting.cpp)
set_source_files_properties(src/main/playerControls/aim_view_targeting.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Model light gather and the stealth pass (research/FLASHLIGHT.md 6.1/16, LEVEL_LIGHTS.md 11).
target_sources(omega_core PRIVATE src/main/lights/model_light_gather.cpp)
set_source_files_properties(src/main/lights/model_light_gather.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Terrain point lighting: 00373320 bounds path, 00371cf0 and the VU1 light pass (research/FLASHLIGHT.md 18).
target_sources(omega_core PRIVATE src/main/lights/terrain_point_lights.cpp)
set_source_files_properties(src/main/lights/terrain_point_lights.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Light corona / lens-flare sprite draw 003781b0 (research/FLASHLIGHT.md, "Checkpoint 118f").
target_sources(omega_core PRIVATE src/main/lights/corona_draw.cpp)
set_source_files_properties(src/main/lights/corona_draw.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# The front end's light files 004029b0 / 004053b0 and the VU1 model colour (research/FRONTEND.md, 132f6).
target_sources(omega_core PRIVATE src/main/lights/frontend_lights.cpp)
set_source_files_properties(src/main/lights/frontend_lights.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# NPC perception and alert (research/NPC_COMBAT.md, milestones N4a/N4b).
set(OMEGA_NPC_COMBAT_SOURCES src/main/ai/npc_perception_think.cpp src/main/npc/npc_alert.cpp src/main/path/npc_route.cpp src/main/npc/npc_script_control.cpp src/main/npc/npc_follow.cpp src/main/npc/npc_carry_body.cpp src/main/npc/npc_avoid_grenade.cpp src/main/npc/npc_flee.cpp)
target_sources(omega_core PRIVATE ${OMEGA_NPC_COMBAT_SOURCES})
set_source_files_properties(${OMEGA_NPC_COMBAT_SOURCES} PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# NPC attack and firing loop (research/NPC_COMBAT.md, milestone N5b).
target_sources(omega_core PRIVATE src/main/npc/npc_attack.cpp)
set_source_files_properties(src/main/npc/npc_attack.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# NPC node searches, LOS checks and cover modes (research/NPC_COMBAT.md, milestone N5c).
set(OMEGA_NPC_N5C_SOURCES src/main/game/npc_los_service.cpp src/main/path/npc_node_search.cpp src/main/npc/npc_cover.cpp src/main/npc/npc_search.cpp)
target_sources(omega_core PRIVATE ${OMEGA_NPC_N5C_SOURCES})
set_source_files_properties(${OMEGA_NPC_N5C_SOURCES} PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# ZMOVIES.HOG movies: PSS demux, MPEG-2 video, PCM audio, .SUB subtitles, player model
# (research/FRONTEND.md, Checkpoint 114). Integer-only decode, so no FP options needed.
target_sources(omega_core PRIVATE src/lib/libmpeg/mpeg2_decoder.cpp src/main/MoviePlayer/movie_pss.cpp src/main/MovieSubtitles/movie_subtitles.cpp src/main/MoviePlayer/movie_player.cpp)
# Checkpoint 132cp11: the movie subtitle overlay (00412600 camera, 00411600 / 00410e50 / 004118e0).
target_sources(omega_core PRIVATE src/main/MovieSubtitles/movie_subtitle_overlay.cpp)
set_source_files_properties(src/main/MovieSubtitles/movie_subtitle_overlay.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Front-end GUI widget runtime: canvas, screen stack, pad events, focus, GuiTitleScreen
# (research/FRONTEND.md, Checkpoint 118).
target_sources(omega_core PRIVATE src/main/guiManager/gui_runtime.cpp)
# Checkpoint 132cc4b: GuiAgentModify, the agent appearance editor (AGENTMOD.GUI).
target_sources(omega_core PRIVATE src/main/guiManager/gui_agent_modify.cpp)
set_source_files_properties(src/main/guiManager/gui_agent_modify.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Checkpoint 132cc5: GuiAgentCreate / GuiAgentSave, the new-agent editor (AGENTNEW.GUI, AGENTOPN.GUI).
target_sources(omega_core PRIVATE src/main/guiManager/gui_agent_create.cpp)
set_source_files_properties(src/main/guiManager/gui_agent_create.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Checkpoint 132hud: GuiGameScreen's weapon select (the hold-Select menu 0045ca20, GAMEGUI.GUI).
target_sources(omega_core PRIVATE src/main/guiManager/gui_game_screen.cpp)
set_source_files_properties(src/main/guiManager/gui_game_screen.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Checkpoint 132f11: the debrief's screens (SUMMARY / SCOREONE / AWARDS / the autosave, 00134110).
target_sources(omega_core PRIVATE src/main/guiManager/gui_end_game.cpp)
set_source_files_properties(src/main/guiManager/gui_end_game.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Checkpoint 132f12: the personnel statistics file (GuiMissionStatistics / GuiMissionStatList, STATS.GUI).
target_sources(omega_core PRIVATE src/main/guiManager/gui_personnel.cpp)
# Checkpoint 132ln2: GuiOnlineConnect (ONLINECN.GUI), the lobby strings and the navigator; its PORT-ONLY LAN mode.
target_sources(omega_core PRIVATE src/main/guiManager/gui_online_connect.cpp)
# Checkpoint 132ln3: GuiMissionSetup (MSNSETUP.GUI) and its PORT-ONLY LAN host.
target_sources(omega_core PRIVATE src/main/guiManager/gui_mission_setup.cpp)
target_sources(omega_core PRIVATE src/main/guiManager/gui_account_login.cpp)
# Checkpoint 132ln4: GuiNetJoinMission / GuiLobbyScreen (JOINMIS.GUI), GuiMissionList, GuiChatWindow; the PORT-ONLY LAN join.
target_sources(omega_core PRIVATE src/main/guiManager/gui_net_join_mission.cpp)
# Checkpoint 132on2c: GuiOnlineOptions (ONLIPREF.GUI), GuiNetMessages / GuiNetMessageArray (NETMESS.GUI).
target_sources(omega_core PRIVATE src/main/guiManager/gui_online_options.cpp src/main/guiManager/gui_net_messages.cpp)
# Host controllers as the DualShock 2 report the pad manager reads (00369cd0).
target_sources(omega_core PRIVATE src/port/platform/gamepad_reports.cpp src/main/playerControls/pad_vibration.cpp)
# Checkpoint 132cp3: the tremor manager (002722b0 / 002727b0 / 002726f0; camera shake and pad rumble).
target_sources(omega_core PRIVATE src/main/tremor/tremor.cpp)
set_source_files_properties(src/main/tremor/tremor.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Checkpoint 132cp3: the vision effects manager in mode 0 (00260990 / 002609e0 / 002604c0 / 0025fbb0 / 0025f930).
target_sources(omega_core PRIVATE src/main/visionFx/vision_fx.cpp src/main/visionFx/gob_glow.cpp src/main/man/skm_vision_shading.cpp
    src/main/visionFx/vision_corona.cpp)
set_source_files_properties(src/main/visionFx/vision_fx.cpp src/main/visionFx/gob_glow.cpp src/main/man/skm_vision_shading.cpp
    src/main/visionFx/vision_corona.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Checkpoint 126 front-end flow: profile services, PS2 card import, keyboard pad.
target_sources(omega_core PRIVATE src/main/AgentData/agent_profile_view.cpp src/port/platform/ps2_card_image.cpp
    src/port/platform/keyboard_pad.cpp src/main/machine/frontend_session.cpp)
# Checkpoint 132win13 (port-only): memory card rolling backups and the --card-export/--card-import/--card-restore tools.
target_sources(omega_core PRIVATE src/port/platform/card_backup.cpp src/port/app/card_tools.cpp)
# Checkpoint 132win14 (port-only): the PS2 save icons (0040d740's icon.sys / MEMCARD.ICN) for --card-export.
target_sources(omega_core PRIVATE src/port/platform/ps2_save_icons.cpp)
# Checkpoint 132ob6 (port-only): the port settings file and the front end's CPU Partner options row.
target_sources(omega_core PRIVATE src/port/platform/port_settings.cpp src/port/app/port_options_row.cpp src/port/app/port_settings_page.cpp)
# Checkpoint 132win3 (port-only): the interactive loops' frame pacing.
target_sources(omega_core PRIVATE src/port/platform/frame_pacer.cpp)
# Checkpoint 132vt1 (port-only, research/VITA.md): the PS Vita controls as the DualShock 2 report (portable, unit tested).
target_sources(omega_core PRIVATE src/port/platform/vita_pad_map.cpp)
# Checkpoint 132win6 (port-only): the keyboard / mouse bindings and the controller connection state.
target_sources(omega_core PRIVATE src/port/platform/input_bindings.cpp src/port/platform/pad_connection.cpp src/port/platform/mouse_look.cpp
    src/port/platform/display_settings.cpp)
# Checkpoint 132ln (port-only, research/LAN_PLAY.md §5.2 / §5.3): LAN discovery and transport groundwork,
# not wired into the game. The socket wrapper needs Winsock2 on Windows.
target_sources(omega_core PRIVATE src/port/net/lan_wire.cpp src/port/net/lan_socket.cpp src/port/net/lan_loopback.cpp
    src/port/net/lan_discovery.cpp src/port/net/lan_transport.cpp src/port/net/lan_migration.cpp
    src/port/net/lan_dme_commit.cpp src/port/net/lan_session.cpp
    src/port/net/lan_mod_gate.cpp)
# Checkpoint 132ln5 (research/NET_INLEVEL.md): DME.cc's in-level game side, and the port-only LanDme (the DME API
# stand-in) and LanNetCore (one network game's memory and host).
target_sources(omega_core PRIVATE src/main/DME/dme_game.cpp src/main/DME/dme_library.cpp src/main/DME/net_registrations.cpp
    src/port/net/lan_dme.cpp src/port/net/lan_net_core.cpp)
if(WIN32)
    target_link_libraries(omega_core PUBLIC ws2_32)
    # Checkpoint 132md15 (port-only, platform_movie.hpp): Media Foundation, the Windows movie backend for a mod's MP4
    # movie overrides (a system component: nothing is shipped).
    target_sources(omega_core PRIVATE src/port/platform/platform_movie_mf.cpp)
    target_compile_definitions(omega_core PRIVATE OMEGA_MEDIA_FOUNDATION)
    set_source_files_properties(src/port/platform/platform_movie_mf.cpp PROPERTIES COMPILE_DEFINITIONS "NOMINMAX;WIN32_LEAN_AND_MEAN")
    target_link_libraries(omega_core PUBLIC mfplat mfreadwrite mfuuid ole32 propsys)
endif()
# Checkpoint 132on1 (port-only, research/ONLINE.md section 5): online play's server side (omega_server) and the
# client transport. Standalone library (portable C++, no omega_core dependency) so the server can build alone.
add_library(omega_online STATIC src/port/online/online_hash.cpp src/port/online/online_protocol.cpp
    src/port/online/online_accounts.cpp src/port/online/online_server.cpp src/port/online/online_tcp.cpp
    src/port/online/online_client.cpp src/port/online/online_lobby.cpp src/port/online/online_mod_report.cpp
    src/port/online/online_stats.cpp src/port/online/online_files.cpp src/port/online/online_relay.cpp
    src/port/online/online_upnp.cpp src/port/online/online_portmap.cpp src/port/online/online_social.cpp)
target_include_directories(omega_online PUBLIC src/port/online)
if(WIN32)
    target_link_libraries(omega_online PUBLIC ws2_32 iphlpapi)
    target_compile_definitions(omega_online PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(omega_online PRIVATE /W4 /permissive- /EHsc)
endif()
target_link_libraries(omega_core PUBLIC omega_online)
# Checkpoint 132on2 (port-only, research/ONLINE.md section 9): the online lobby GuiOnlineConnect drives (FrontendLobby).
target_sources(omega_core PRIVATE src/port/app/online_lobby_host.cpp src/port/app/online_relay_socket.cpp)
add_executable(omega_server src/port/app/omega_server_main.cpp src/port/app/omega_server_load.cpp)
target_link_libraries(omega_server PRIVATE omega_online)
if(WIN32)
    target_compile_definitions(omega_server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(MSVC)
    target_compile_options(omega_server PRIVATE /W4 /permissive- /EHsc)
endif()
set_source_files_properties(src/main/guiManager/gui_runtime.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# The GUI 2D draw path: widget draws, text layout, font strings to primitive batches
# (research/FRONTEND.md, Checkpoint 122a).
target_sources(omega_core PRIVATE src/main/gui/gui_draw.cpp src/main/gui/gui_decor.cpp
    src/main/interface_model/ie_draw.cpp)
set_source_files_properties(src/main/gui/gui_draw.cpp src/main/gui/gui_decor.cpp src/main/interface_model/ie_draw.cpp
    PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
# Particle definitions, emitters and their draw (research/EFFECTS.md, Checkpoint 122d).
target_sources(omega_core PRIVATE src/main/particle/particle_def.cpp src/main/particle/particle_emitter.cpp src/main/particle/particle_vu1.cpp src/main/particle/particle_asset.cpp src/main/particle/effect_service.cpp src/main/particle/smoke_field.cpp src/main/particle/weather_field.cpp)
set_source_files_properties(src/main/particle/particle_def.cpp src/main/particle/particle_emitter.cpp src/main/particle/particle_vu1.cpp src/main/particle/particle_asset.cpp src/main/particle/effect_service.cpp src/main/particle/smoke_field.cpp src/main/particle/weather_field.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
add_executable(omega_collision src/port/app/collision_main.cpp)
target_link_libraries(omega_collision PRIVATE omega_core)
add_executable(omega_actor_physics src/port/app/actor_physics_main.cpp)
target_link_libraries(omega_actor_physics PRIVATE omega_core)
add_executable(omega_character src/port/app/character_main.cpp src/port/probes/pose_sequence.cpp src/port/probes/clip_playback_sequence.cpp src/port/probes/animation_playback_sequence.cpp src/port/probes/skin_sequence.cpp)
target_link_libraries(omega_character PRIVATE omega_core)
set_source_files_properties(src/main/ska/animation_group.cpp src/main/ska/animation_graph.cpp src/main/ska/animation_player.cpp src/main/ska/animation_controller.cpp src/main/vu1model/skinned_mesh.cpp src/main/ska/clip_playback.cpp src/main/man/morph_host.cpp src/main/man/morph_resources.cpp src/main/human/actor_animation.cpp src/main/human/actor_navigation.cpp src/main/human/actor_ladder.cpp src/main/skeleton/attachment_transforms.cpp src/main/human/root_attachments.cpp src/main/ska/animated_character.cpp src/main/ska/animation_vector_curve.cpp src/main/human/actor_position_curve.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/grenade/projectile_launch.cpp src/main/population/population.cpp src/main/GameGOBJ/path_motion.cpp src/main/GameGOBJ/elevator_gobj.cpp src/main/GameGOBJ/tank.cpp src/main/Loader/zone_activation.cpp src/main/human/actor_animation_receiver.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/ska/actor_motion.cpp src/main/ska/level_animation.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/ska/animation_rotation_curve.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/human/actor_curve_update.cpp src/main/playerControls/actor_heading_state.cpp src/main/grenade/projectile_simulation.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/grenade/projectile_candidates.cpp src/main/humanCollision/actor_physical_prediction.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/humanCollision/actor_physical_state.cpp src/main/humanCollision/actor_physical_initialization.cpp src/main/humanCollision/actor_auxiliary_probe.cpp src/main/humanCollision/actor_movement_adjustment.cpp src/main/humanCollision/actor_support_history.cpp src/main/humanCollision/actor_support_motion.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/collision/los_query.cpp src/main/collision/los_provider.cpp src/main/collision/los_frame.cpp src/main/skeleton/actor_model_context.cpp src/main/collision/col_gather.cpp src/main/collision/collision_triangles.cpp src/main/gobj/actor_world_history.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
target_sources(omega_core PRIVATE src/main/collision/collision_slide.cpp)
set_source_files_properties(src/main/WaterFx/water_effects.cpp src/main/WaterFx/water_effect_draw.cpp src/main/humanCollision/actor_movement_scheduler.cpp src/main/humanCollision/actor_response_probes.cpp src/main/humanCollision/actor_response_setup.cpp src/main/humanCollision/actor_physical_response.cpp src/main/humanCollision/actor_gather_bounds.cpp src/main/humanCollision/actor_water_support.cpp src/main/humanCollision/actor_physical_frame.cpp src/main/humanCollision/actor_edge_presets.cpp src/main/humanCollision/actor_edge_detection.cpp src/main/playerControls/player_pad_control.cpp src/port/play/solo_actor_simulation.cpp src/port/play/actor_world.cpp src/main/path/npc_path_follower.cpp src/main/npc/npc_ai.cpp src/main/npc/npc_world.cpp src/main/npc/npc_remote.cpp src/main/Generator/npc_generator.cpp src/main/human/actor_ledge_action.cpp src/main/human/actor_body_carry.cpp src/main/human/actor_object_action.cpp src/main/human/actor_hanging.cpp src/main/human/actor_damage.cpp src/main/human/actor_death.cpp src/main/human/actor_choke.cpp src/lib/game_libm.cpp src/main/lights/character_shadows.cpp src/main/gameLight/level_lights.cpp src/main/lights/shadow_scene.cpp src/main/human/caster_bounds.cpp src/main/human/actor_model_defaults.cpp src/port/platform/float_environment.cpp src/main/gameCamera/camera_curves.cpp src/main/gameCamera/camera_rig.cpp src/main/gameCamera/camera_zoom.cpp src/main/gameCamera/player_camera.cpp src/main/gameCamera/camera_collision.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/humanCollision/actor_contact_events.cpp src/main/humanCollision/actor_response_placement.cpp src/main/humanCollision/actor_body_query.cpp src/main/humanCollision/actor_specialized_body_query.cpp src/main/collision/moving_collision_slide.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
set_source_files_properties(src/main/collision/live_gather.cpp src/main/humanCollision/actor_floor_response.cpp src/main/collision/collision_slide.cpp src/main/DME/dme_game.cpp PROPERTIES COMPILE_OPTIONS
    "$<$<CXX_COMPILER_ID:MSVC>:/fp:strict>;$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
add_executable(omega_animation src/port/app/animation_main.cpp src/port/probes/root_sequence.cpp src/port/probes/transition_sequence.cpp src/port/probes/slider_sequence.cpp src/port/probes/group_sequence.cpp src/port/probes/graph_sequence.cpp src/port/probes/morph_host_sequence.cpp)
target_link_libraries(omega_animation PRIVATE omega_core)
if(MSVC)
    target_compile_options(omega_core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(omega_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/disc_us.json" DISC_PROFILE)
    string(JSON ISO_SIZE GET "${DISC_PROFILE}" iso_size)
    string(JSON ISO_SHA256 GET "${DISC_PROFILE}" iso_sha256)
    string(JSON EXE_SIZE GET "${DISC_PROFILE}" executable_size)
    string(JSON EXE_SHA256 GET "${DISC_PROFILE}" executable_sha256)
    string(JSON BOOT_EXECUTABLE GET "${DISC_PROFILE}" boot_executable)
    add_library(omega_windows src/port/disc/windows_hash.cpp src/port/disc/derived_data.cpp)
    target_include_directories(omega_windows PUBLIC ${OMEGA_SOURCE_DIRS})
    target_compile_definitions(omega_windows PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(omega_windows PUBLIC bcrypt omega_core)
    # Port-only (checkpoint 132win8): the extractor ports must round binary32 exactly as the Python tools do.
    set_source_files_properties(src/port/disc/derived_data.cpp PROPERTIES COMPILE_OPTIONS /fp:strict)
    add_executable(omega_objectives src/port/app/native_main.cpp)
    add_executable(omega_assets src/port/app/asset_main.cpp)
    # Checkpoint 132md1 (port-only): the mod tool (research/MODDING.md): texture / file export into a mod's layout.
    add_executable(omega_mod_tool src/port/app/mod_tool_main.cpp)
# Checkpoint 132mt1 (port-only): the renderer abstraction layer's Direct3D 11 backend (src/port/gfx, research/RENDERER.md);
# every target that draws links it.
# Checkpoint 132mt2: the render thread (gfx_render_thread, backend-neutral) goes with it.
set(OMEGA_GFX_D3D11_SOURCES src/port/gfx/gfx_d3d11.cpp src/port/gfx/gfx_d3d11_shaders.cpp src/port/gfx/gfx_render_thread.cpp
    src/port/gfx/gfx_pc_backend.cpp)
# Checkpoint 132vk1 (port-only, research/VULKAN.md): the PC's second renderer backend, native Vulkan 1.3 beside
# Direct3D 11 (--renderer vulkan / renderer=vulkan; D3D11 stays the default). It needs nothing installed beyond the GPU
# driver: the vulkan-1.dll loader is opened at run time, the Khronos headers are vendored (vendor/vulkan-headers) and the
# SPIR-V is embedded (gfx_vulkan_spirv.cpp, made by tools/build_vulkan_spirv.py from the D3D11 HLSL).
option(OMEGA_PC_VULKAN "Build the PC Vulkan renderer backend (--renderer vulkan)" ON)
if(OMEGA_PC_VULKAN)
    list(APPEND OMEGA_GFX_D3D11_SOURCES src/port/gfx/gfx_vulkan.cpp src/port/gfx/gfx_vulkan_spirv.cpp)
    set_source_files_properties(src/port/gfx/gfx_vulkan.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vulkan-headers/include")
    add_compile_definitions(OMEGA_PC_VULKAN=1)
endif()
add_executable(omega_frontend_formats src/port/app/frontend_formats_main.cpp src/port/render/gui_renderer.cpp
    src/port/app/host_window.cpp src/port/app/loading_screen_host.cpp src/port/platform/crash_report.cpp
    ${OMEGA_GFX_D3D11_SOURCES})
target_link_libraries(omega_frontend_formats PRIVATE omega_core d3d11 dxgi d3dcompiler user32)
target_compile_definitions(omega_frontend_formats PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
add_executable(omega_movie src/port/app/movie_main.cpp)
target_link_libraries(omega_movie PRIVATE omega_core)
    add_executable(omega_scene src/port/app/scene_main.cpp src/port/render/scene_renderer.cpp ${OMEGA_GFX_D3D11_SOURCES})
    add_executable(omega_motion src/port/app/motion_main.cpp)
    add_executable(omega_actor src/port/app/actor_preview_main.cpp src/port/render/scene_renderer.cpp ${OMEGA_GFX_D3D11_SOURCES})
    # Checkpoint 132win8 (port-only): the play host is compiled once and linked twice: omega_play (console, the
    # scripted / diagnostic entry) and OmegaStrain (the player-facing GUI-subsystem launcher, launcher.hpp).
    add_library(omega_play_objects OBJECT src/port/app/play_main.cpp src/port/app/launcher.cpp src/port/app/launcher_mod_panel.cpp src/port/app/frontend_app.cpp src/port/app/frontend_disc.cpp src/port/app/host_window.cpp
        src/port/app/agent_preview.cpp src/port/app/loading_screen_host.cpp src/port/render/scene_renderer.cpp
        src/port/render/gui_renderer.cpp src/port/render/particle_renderer.cpp src/port/render/corona_renderer.cpp src/port/platform/audio_device.cpp src/port/platform/gamepad_device.cpp
        src/port/render/frame_overlay.cpp src/port/platform/crash_report.cpp src/port/platform/gamepad_simulation.cpp
        src/port/app/pacing_probe.cpp ${OMEGA_GFX_D3D11_SOURCES})
    target_link_libraries(omega_assets PRIVATE gdi32 user32)
    target_link_libraries(omega_scene PRIVATE d3d11 dxgi d3dcompiler user32 gdi32)
    target_link_libraries(omega_actor PRIVATE d3d11 dxgi d3dcompiler user32 gdi32)
    add_executable(omega_play $<TARGET_OBJECTS:omega_play_objects>)
    add_executable(omega_strain WIN32 $<TARGET_OBJECTS:omega_play_objects>)
    set_target_properties(omega_strain PROPERTIES OUTPUT_NAME OmegaStrain)
    target_link_options(omega_strain PRIVATE /ENTRY:wmainCRTStartup)
    foreach(target omega_play omega_strain)
        target_link_libraries(${target} PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 xaudio2 ole32 setupapi hid comdlg32 shell32
            omega_core omega_windows)
        # Checkpoint 132win12 (port-only): the linker map next to the exe names the crash reporter's throw addresses
        # (crash_report.hpp); it does not change the executable.
        target_link_options(${target} PRIVATE /MAP)
    endforeach()
    set_source_files_properties(src/port/app/play_main.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
    foreach(target omega_objectives omega_assets omega_mod_tool omega_scene omega_motion omega_actor omega_play_objects)
        target_link_libraries(${target} PRIVATE omega_core omega_windows)
        target_compile_definitions(${target} PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
            OMEGA_ISO_SIZE=${ISO_SIZE}ULL
            OMEGA_ISO_SHA256="${ISO_SHA256}"
        )
    endforeach()
    target_compile_definitions(omega_play_objects PRIVATE OMEGA_EXE_SIZE=${EXE_SIZE}ULL OMEGA_EXE_SHA256="${EXE_SHA256}"
        OMEGA_BOOT_EXECUTABLE="${BOOT_EXECUTABLE}")
    # The physics harness can also read terrain COL5 and the player start from the verified disc.
    target_link_libraries(omega_actor_physics PRIVATE omega_windows)
    target_compile_definitions(omega_actor_physics PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
        OMEGA_HAS_VERIFIED_ISO OMEGA_ISO_SIZE=${ISO_SIZE}ULL OMEGA_ISO_SHA256="${ISO_SHA256}")
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(omega_core_tests tests/native_tests.cpp tests/texture_tests.cpp tests/material_anim_tests.cpp tests/geometry_tests.cpp tests/population_tests.cpp tests/path_motion_tests.cpp tests/object_event_tests.cpp tests/objective_binding_tests.cpp tests/collision_face_tests.cpp tests/appearance_tests.cpp tests/animation_channel_tests.cpp tests/skeleton_tests.cpp tests/animation_clip_tests.cpp tests/skeleton_pose_tests.cpp)
    target_link_libraries(omega_core_tests PRIVATE omega_core)
    target_sources(omega_core_tests PRIVATE tests/animation_sampling_tests.cpp tests/script_scheduler_tests.cpp tests/script_schedule_vm_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_pose_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/worker_pool_tests.cpp)   # 132mt3 (port-only)
    target_sources(omega_core_tests PRIVATE tests/small_alloc_tests.cpp)   # 132vc2 (port-only)
    target_sources(omega_core_tests PRIVATE tests/mod_tests.cpp tests/mod_audio_tests.cpp tests/mod_online_tests.cpp tests/mod_bank_sound_tests.cpp tests/mod_manager_tests.cpp tests/mod_archive_tests.cpp tests/mod_movie_tests.cpp tests/platform_movie_tests.cpp tests/mod_boundary_tests.cpp tests/mod_data_tests.cpp)   # 132md1 (port-only)
    target_sources(omega_core_tests PRIVATE tests/model_tests.cpp)   # 132md2 (port-only)
    target_sources(omega_core_tests PRIVATE tests/texture_binding_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_root_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/aim_view_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/aim_entry_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/aim_move_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/lock_move_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/pick_camera_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/camera_zoom_sweep_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/crouch_camera_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/hud_crosshair_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/audio_tests.cpp tests/snd989_tests.cpp tests/snd_bank_tests.cpp tests/spu2_voices_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/game_sound_tests.cpp tests/hud_object_tests.cpp tests/hud_lock_colour_tests.cpp tests/hud_lock_tests.cpp tests/hud_ticker_tests.cpp tests/weapon_reticle_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/material_properties_tests.cpp tests/footstep_tests.cpp tests/surface_impact_tests.cpp tests/voice_sound_tests.cpp
    tests/fstream_loader_tests.cpp tests/bullet_hold89_tests.cpp tests/bullet_explosion_tests.cpp
    tests/weapon_discharge_projectile_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/aim_camera_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/aim_bit3_tests.cpp tests/upper_body_pose_tests.cpp tests/upper_body_sampler_tests.cpp tests/upper_body_ik_tests.cpp tests/upper_body_combine_tests.cpp tests/upper_body_pass_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/weapon_fov_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/objective_stage_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/objective_outcome_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/objective_progression_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/objective_registration_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/string_pool_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/frontend_flow_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/script_string_tests.cpp tests/script_opcode_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/script_context_tests.cpp tests/engine_variables_tests.cpp tests/mission_script_host_tests.cpp tests/mission_script_npc_tests.cpp tests/npc_generator_tests.cpp tests/npc_remote_tests.cpp tests/generator_tick_tests.cpp tests/generator_network_tests.cpp tests/generator_spawn_actor_tests.cpp tests/object_dynamic_id_tests.cpp tests/get_npc_tests.cpp tests/generator_link_receive_tests.cpp tests/weapon_reload_tests.cpp tests/reload_action_tests.cpp tests/equip_goggles_tests.cpp tests/script_ai_native_tests.cpp tests/goggles_vision_tests.cpp tests/request_facial_tests.cpp tests/script_look_at_tests.cpp tests/generator_spawn_view_tests.cpp tests/spawned_actor_ctor_tests.cpp tests/spawned_actor_ai_tests.cpp tests/spawned_actor_ai_base_tests.cpp tests/spawned_actor_ai_attach_tests.cpp tests/ai_runtime_init_tests.cpp tests/ai_component_bind_tests.cpp tests/spawned_npc_body_tests.cpp tests/inventory_services_tests.cpp tests/mission_script_world_tests.cpp tests/zone_activation_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_component_attach_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/spawned_npc_dtor_tests.cpp tests/spawned_npc_teardown_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_root_clip_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/objective_marker_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/script_object_runtime_tests.cpp tests/localization_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/voice_sound_bridge_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_transition_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_slider_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_group_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/clip_playback_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_graph_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_graph_manifest_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/skinned_mesh_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_player_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_controller_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/bone_scales_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_model_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/morph_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/morph_host_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/morph_resources_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_animation_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_navigation_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_vector_curve_tests.cpp tests/actor_position_curve_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/projectile_launch_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_motion_tests.cpp tests/level_animation_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animation_rotation_curve_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_curve_update_tests.cpp tests/actor_heading_state_tests.cpp tests/projectile_simulation_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/projectile_candidates_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/los_query_tests.cpp tests/los_water_pass_tests.cpp tests/actor_frame_service_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_physical_prediction_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_physical_state_tests.cpp tests/actor_movement_adjustment_tests.cpp tests/actor_support_history_tests.cpp tests/actor_support_motion_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_auxiliary_probe_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_physical_initialization_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/los_provider_tests.cpp tests/los_frame_tests.cpp tests/actor_model_context_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/col_gather_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/collision_triangles_tests.cpp tests/actor_world_history_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/live_gather_tests.cpp tests/collision_collection_tests.cpp tests/actor_floor_response_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/collision_slide_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_contact_events_tests.cpp tests/actor_response_placement_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_body_query_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_specialized_body_query_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/moving_collision_slide_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_fader_tests.cpp tests/game_end_state_tests.cpp tests/game_flow_tests.cpp tests/level_load_tests.cpp tests/level_load_state_tests.cpp tests/team_setup_tests.cpp tests/level_selector_tests.cpp tests/loadout_state_tests.cpp tests/app_teardown_tests.cpp tests/card_wait_poll_tests.cpp tests/teardown_state_tests.cpp tests/world_teardown_tests.cpp tests/level_teardown_callees_tests.cpp tests/play_level_teardown_tests.cpp tests/lobby_state_tests.cpp tests/gameplay_state_tests.cpp tests/play_native_flow_tests.cpp tests/video_frame_tests.cpp tests/renderer_init_tests.cpp tests/remote_ready_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_checkpoint_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_teleport_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/water_effects_tests.cpp tests/actor_movement_scheduler_tests.cpp tests/actor_movement_pass_tests.cpp tests/actor_response_probes_tests.cpp tests/actor_response_setup_tests.cpp tests/actor_physical_response_tests.cpp tests/actor_gather_bounds_tests.cpp tests/actor_water_support_tests.cpp tests/actor_physical_frame_tests.cpp tests/actor_edge_presets_tests.cpp tests/player_pad_control_tests.cpp tests/float_environment_tests.cpp tests/camera_tests.cpp tests/actor_edge_detection_tests.cpp tests/actor_quick_turn_tests.cpp tests/actor_flashbang_hide_tests.cpp tests/actor_roll_action_tests.cpp tests/solo_roll_tests.cpp tests/actor_ledge_input_tests.cpp tests/actor_ledge_action_tests.cpp tests/actor_body_carry_tests.cpp tests/actor_object_action_tests.cpp tests/solo_ledge_tests.cpp tests/actor_ledge_resolve_tests.cpp tests/actor_hanging_tests.cpp tests/game_libm_tests.cpp tests/solo_hang_tests.cpp tests/actor_ladder_detection_tests.cpp tests/actor_ladder_mount_tests.cpp tests/actor_ladder_body_tests.cpp tests/actor_ladder_exit_tests.cpp tests/actor_ladder_token_tests.cpp tests/solo_ladder_tests.cpp tests/character_shadows_tests.cpp tests/level_lights_tests.cpp tests/shadow_scene_tests.cpp tests/caster_bounds_tests.cpp tests/actor_model_defaults_tests.cpp tests/hotbox_tests.cpp tests/solo_hotbox_tests.cpp tests/running_jump_tests.cpp tests/actor_damage_tests.cpp tests/actor_death_tests.cpp tests/actor_choke_tests.cpp tests/solo_fall_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_ladder_tests.cpp tests/actor_model_slots_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_ladder_bindings_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/attachment_transforms_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/root_attachments_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_attachment_runtime_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_world_stack_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/actor_animation_receiver_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/animated_character_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/character_appearance_tests.cpp tests/agent_modify_tests.cpp tests/agent_create_tests.cpp tests/agent_camera_tests.cpp tests/npc_roster_tests.cpp tests/actor_world_tests.cpp tests/npc_ai_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/flashlight_tests.cpp tests/corona_draw_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/model_light_gather_tests.cpp tests/terrain_point_lights_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/explosion_tests.cpp tests/thrown_projectile_tests.cpp tests/area_effect_tests.cpp tests/combat_items_scenario_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/secondary_playback_tests.cpp tests/actor_throw_tests.cpp tests/throw_flow_scenario_tests.cpp tests/actor_melee_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/combat_throw_play_tests.cpp tests/throw_aim_view_tests.cpp tests/takedown_camera_tests.cpp tests/actor_stun_tests.cpp tests/effect_slots_tests.cpp tests/stunned_action_tests.cpp tests/cancel_action_tests.cpp tests/torch_light_tests.cpp tests/air_taser_tests.cpp tests/ignition_tests.cpp tests/actor_burning_tests.cpp tests/ni_event_object_tests.cpp tests/figure_camera_tests.cpp tests/sound_gobj_tests.cpp tests/lift_tests.cpp tests/tank_tests.cpp tests/npc_follow_tests.cpp tests/npc_carry_body_tests.cpp tests/npc_avoid_grenade_tests.cpp tests/skybox_tests.cpp tests/decal_manager_tests.cpp tests/water_effect_draw_tests.cpp tests/blood_decal_tests.cpp tests/object_damage_tests.cpp tests/hotbox_eye_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/weapon_inventory_tests.cpp tests/equipment_loadout_tests.cpp tests/play_loadout_tests.cpp tests/agent_save_tests.cpp tests/agent_load_tests.cpp tests/agent_refill_tests.cpp tests/parts_tail_tests.cpp tests/award_registry_tests.cpp tests/awards_agent_tests.cpp tests/parts_table_tests.cpp tests/agent_save_flow_tests.cpp tests/agent_card_tests.cpp tests/frontend_formats_tests.cpp tests/gui_runtime_tests.cpp tests/weapon_aim_tests.cpp tests/bullet_hit_tests.cpp tests/bullet_list_tests.cpp tests/bullet_render_tests.cpp tests/weapon_discharge_tests.cpp tests/combat_scenario_tests.cpp tests/weapon_update_tests.cpp tests/actor_hit_volumes_tests.cpp tests/model_attach_points_tests.cpp tests/model_stream_wait_tests.cpp tests/secondary_component_tests.cpp tests/player_targeting_tests.cpp tests/npc_perception_tests.cpp tests/npc_death_notice_tests.cpp tests/secondary_aim_tests.cpp tests/target_visibility_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/agent_network_codec_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/agent_network_receive_tests.cpp)
    if(MSVC)
        set_source_files_properties(tests/agent_save_flow_tests.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
        set_source_files_properties(tests/award_registry_tests.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
        set_source_files_properties(tests/gui_runtime_tests.cpp PROPERTIES COMPILE_OPTIONS /bigobj)
    endif()
    target_sources(omega_core_tests PRIVATE tests/npc_perception_think_tests.cpp tests/npc_alert_tests.cpp tests/npc_alert_scenario_tests.cpp tests/npc_route_tests.cpp tests/npc_route_scenario_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/agent_statistics_tests.cpp tests/stats_record_tests.cpp tests/hit_direction_tests.cpp tests/npc_dead_drop_tests.cpp tests/node_matrix_tests.cpp tests/weapon_backpack_tests.cpp tests/gun_model_bounds_tests.cpp tests/frontend_lights_tests.cpp tests/weapon_models_tests.cpp tests/shell_eject_tests.cpp tests/backpack_object_tests.cpp tests/backpack_crate_tests.cpp tests/backpack_draw_tests.cpp tests/pad_interact_prompt_tests.cpp tests/hud_update_tests.cpp tests/man_draw_gate_tests.cpp tests/hud_screen_draw_tests.cpp tests/hud_callout_tests.cpp tests/hud_map_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/npc_attack_tests.cpp tests/npc_attack_scenario_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/npc_search_tests.cpp tests/npc_look_tests.cpp tests/npc_takecover_tests.cpp tests/npc_controller_message_tests.cpp tests/npc_path_link_jump_tests.cpp tests/freed_ai_resume_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/agent_perks_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/movie_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/gui_draw_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/movie_subtitle_overlay_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/gui_decor_tests.cpp tests/hud_text_place_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/scene_draw_order_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/particle_tests.cpp tests/effect_service_tests.cpp tests/smoke_field_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/gamepad_reports_tests.cpp tests/pad_vibration_tests.cpp tests/player_weapon_switch_tests.cpp tests/tremor_tests.cpp
    tests/vision_fx_tests.cpp tests/vision_modes_tests.cpp tests/skm_vision_shading_tests.cpp tests/gob_glow_tests.cpp
    tests/vision_corona_tests.cpp tests/man_sort_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/objectives_overlay_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/coop_partner_tests.cpp tests/coop_breadcrumbs_tests.cpp tests/coop_command_menu_tests.cpp tests/interact_gobj_pairing_tests.cpp tests/host_window_tests.cpp tests/frame_pacer_tests.cpp tests/card_tools_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/port_cheats_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/online_server_tests.cpp)   # 132on1 (port-only)
target_sources(omega_core_tests PRIVATE tests/lan_net_tests.cpp tests/lan_inlevel_tests.cpp tests/dme_game_tests.cpp
    tests/net_registry_tests.cpp tests/lan_mod_gate_tests.cpp)
target_sources(omega_core_tests PRIVATE tests/input_device_tests.cpp tests/port_settings_page_tests.cpp)
    target_sources(omega_core_tests PRIVATE tests/vita_pad_map_tests.cpp)   # 132vt1 (port-only)
    target_sources(omega_core_tests PRIVATE tests/pickup_menu_tests.cpp tests/pickup_menu_frame_tests.cpp)
    target_compile_definitions(omega_core_tests PRIVATE OMEGA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_compile_definitions(omega_core_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(omega_core_tests PRIVATE omega_windows)
        # The front-end session test reads the disc as the window does.
        target_sources(omega_core_tests PRIVATE src/port/app/frontend_disc.cpp src/port/app/agent_preview.cpp src/port/render/gui_renderer.cpp src/port/render/particle_renderer.cpp
            src/port/app/host_window.cpp src/port/app/loading_screen_host.cpp
            src/port/render/scene_renderer.cpp src/port/render/corona_renderer.cpp tests/corona_device_tests.cpp
            src/port/render/frame_overlay.cpp tests/frame_overlay_tests.cpp tests/gfx_render_thread_tests.cpp tests/gfx_skinning_capability_tests.cpp tests/gfx_texture_blocks_tests.cpp tests/gfx_vulkan_tests.cpp ${OMEGA_GFX_D3D11_SOURCES}
            src/port/platform/audio_device.cpp tests/audio_device_tests.cpp
            src/port/app/launcher.cpp src/port/app/launcher_mod_panel.cpp tests/launcher_tests.cpp tests/launcher_mod_panel_tests.cpp
            src/port/platform/crash_report.cpp tests/crash_report_tests.cpp
            src/port/platform/gamepad_device.cpp src/port/platform/gamepad_simulation.cpp tests/gamepad_simulation_tests.cpp)
        target_link_libraries(omega_core_tests PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 xaudio2 ole32 comdlg32 shell32 setupapi hid)
        # Checkpoint 132win8: the launcher and the derived-data tests use the supported disc's profile.
        target_compile_definitions(omega_core_tests PRIVATE OMEGA_ISO_SIZE=${ISO_SIZE}ULL OMEGA_ISO_SHA256="${ISO_SHA256}"
            OMEGA_EXE_SIZE=${EXE_SIZE}ULL OMEGA_EXE_SHA256="${EXE_SHA256}" OMEGA_BOOT_EXECUTABLE="${BOOT_EXECUTABLE}")
        target_compile_definitions(omega_core_tests PRIVATE OMEGA_FRONTEND_DISC=1)
    endif()
    add_test(NAME omega_core_tests COMMAND omega_core_tests)
endif()
if(VITA)
    include(cmake/vita/OmegaVitaTargets.cmake)
endif()
if(OMEGA_PS5)
    include(cmake/ps5/OmegaPs5Targets.cmake)
endif()
