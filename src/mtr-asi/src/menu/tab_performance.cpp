// Performance tab: the High-FPS smoothing block + Engine timestep correction.
//
// The big knob the player cares about: render at 240Hz while keeping the
// game's authored speed correct, and smoothly blend the camera (+ optionally
// Wilbur / NPCs) between game-logic ticks.
//
// Underneath it ties together three sibling subsystems:
//   - dt_correctness — writes flt_6FFCBC = real_dt at every consumer phase
//                       (~150 fixed-step subsystems become FPS-independent).
//   - sim_decouple    — throttles sim_aggregator + 9 alt-pump/render-path
//                       hooks to a chosen sim_hz; locks game logic to e.g.
//                       60 Hz while render runs free.
//   - interp          — slerp(view) + lerp(translation) + per-entity
//                       lerp/slerp for player & NPCs across sim ticks.
//
// All knobs live behind one big section_begin so the user can leave it
// collapsed if they don't care about high-refresh smoothing — most of the
// menu doesn't depend on this block.

#include "menu_internal.h"
#include "imgui.h"

#include <cstdint>

#include "mtr/sim_decouple.h"
#include "mtr/interp.h"
#include "mtr/dt_correctness.h"

namespace mtr::menu::detail {

void tab_performance() {
    if (bool open = section_begin("High-FPS smoothing")) {
        heading_with_info("Render at high FPS while keeping game speed correct",
            "The engine ties physics, animation and camera to render frames - "
            "rendering at 240 FPS makes everything run 4x fast. This feature "
            "limits the game logic to a chosen Hz while letting you render at "
            "any frame rate, then smoothly blends the camera (and optionally "
            "Wilbur / NPCs) between game-logic ticks for fluid motion.\n\n"
            "REQUIREMENT for the smoothing to be visible: render FPS must be "
            "HIGHER than game-logic Hz. At 60 FPS render + 60 Hz logic the "
            "smoothing has no in-between frames to blend across. Raise FPS "
            "limit (Tools > FPS limiter) to 120/144/240 to see the benefit.\n\n"
            "Technical: throttle hooks gate sim_aggregator (0x67F430) plus "
            "interp infra (slerp/lerp + cut detection) POST "
            "camera_apply_all_active (0x4C1E40). The 'Engine timestep "
            "correction' option below writes flt_6FFCBC (the engine's "
            "universal hardcoded 0.003 dt at 0x6FFCBC) to real elapsed time, "
            "making all 150+ fixed-step subsystems framerate-independent. "
            "Without it, only sim_hz=60 preserves authored game speed.");
        ImGui::Spacing();

        // Engine timestep correction — the foundational fix. Default ON.
        // When ON, the user can pick any sim_hz without slow-mo / fast-fwd
        // side effects, AND pathcam / HUD / particles run framerate-correct.
        bool dtc = mtr::dt_correctness::enabled();
        if (ImGui::Checkbox("Engine timestep correction (recommended ON)##dtc", &dtc)) {
            mtr::dt_correctness::set_enabled(dtc);
        }

        if (dtc) {
            ImGui::Indent();
            int ts = static_cast<int>(mtr::dt_correctness::time_scale());
            // Order MUST match TimeScale enum in dt_correctness.h:
            //   0=RealTime, 1=SlowMoAtLowSim, 2=Off, 3=VisualLockToSim.
            const char* ts_items[] = {
                "Real-time (sim_hz = fidelity dial, world plays at 1.0x)",
                "Slow-mo at low sim_hz (game speed = sim_hz / 60)",
                "Off (vanilla 0.003 fixed step)",
                "Visual lock to sim (choppy at sim_hz, low-fi look)",
            };
            ImGui::SetNextItemWidth(420.0f);
            if (ImGui::Combo("Time scale##timescale", &ts, ts_items, IM_ARRAYSIZE(ts_items))) {
                mtr::dt_correctness::set_time_scale(static_cast<mtr::dt_correctness::TimeScale>(ts));
            }
            ImGui::SameLine();
            info_pill(
                "Real-time: sim_hz controls the FIDELITY of game logic "
                "without changing world speed. Particles, animations, "
                "physics all play at 1.0 sec / sec at any sim_hz. The "
                "default and recommended setting.\n\n"
                "Slow-mo at low sim_hz: world speed scales linearly with "
                "sim_hz / 60. sim_hz=15 = quarter-speed cinematic slow-mo, "
                "sim_hz=120 = 2x fast-forward. Particles slow with the "
                "rest of the world (because we hook sub_4F45F0 too). "
                "Useful for cinematic effects and stress-testing the "
                "decouple.\n\n"
                "Visual lock to sim: visual-path consumers (particle "
                "integrator, render-path flt_6FFCBC + dword_6FFCA4) "
                "advance only on render frames where sim actually ticked, "
                "and pass dt=0 between sim ticks. At sim_hz=15 the "
                "glow / sprite / particle visuals update in 15 discrete "
                "steps per second while render still draws at 240 Hz. "
                "Camera interp + view smoothing are unaffected (those "
                "use real-render-dt independently). Use to test \"what "
                "does this look like at N Hz?\".\n\n"
                "Off: revert to vanilla engine timing \xE2\x80\x94 every fixed-step "
                "subsystem reads 0.003 regardless of actual rate. The "
                "game ships this way; only sim_hz=60 approximates correct "
                "speed.\n\n"
                "Technical: the scale factor multiplies dt at every "
                "write site (camera_apply, sim_aggregator, sub_4F45F0). "
                "RealTime = 1.0, SlowMo = sim_hz/60.0, Off = no write. "
                "VisualLockToSim is a sim-tick-edge gate, not a scale.");
            const float scale = mtr::dt_correctness::render_dt_scale();
            if (scale != 1.0f) {
                ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f),
                    "  Effective time scale: %.3fx (game runs at %d%% real-time)",
                    scale, static_cast<int>(scale * 100.0f + 0.5f));
            }

            ImGui::Spacing();
            ImGui::TextUnformatted("Particle / trail lifetime feel:");
            ImGui::SameLine();
            info_pill(
                "Lerps the dt seen by particle and trail systems "
                "(trail_subsystem_tick + particle_buckets_sweep_a/b) "
                "between engine-vanilla 0.003 and the corrected real-time "
                "dt. Physics, animation, and entity transforms are "
                "untouched \xE2\x80\x94 they still see the corrected dt.\n\n"
                "Why this exists: particle effects were authored against "
                "the engine's vanilla 0.003 fixed step. With dt-correctness "
                "ON, particles decay at TRUE real-time (1.0 sec/sec), "
                "which is several times faster than the engine ran "
                "originally. This slider gives the corrected world while "
                "letting particles keep their game-authored speed.\n\n"
                "0.00 = engine-vanilla 0.003 (looks like dtc OFF \xE2\x80\x94\n"
                "       particles 'abide the HZ properly'). DEFAULT.\n"
                "1.00 = strict real-time (matches dtc ON behavior).\n"
                "0.50 = halfway between the two.");
            float pmix = mtr::dt_correctness::particle_feel_mix();
            ImGui::SetNextItemWidth(280.0f);
            if (ImGui::SliderFloat("Particle feel##particle_feel", &pmix,
                                   0.0f, 1.0f, "%.2f")) {
                mtr::dt_correctness::set_particle_feel_mix(pmix);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Vanilla##pfVan")) {
                mtr::dt_correctness::set_particle_feel_mix(0.0f);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Real-time##pfRT")) {
                mtr::dt_correctness::set_particle_feel_mix(1.0f);
            }

            ImGui::Unindent();
        }
        ImGui::SameLine();
        info_pill(
            "Fixes the engine's hardcoded 0.003 sec timestep that makes "
            "everything (physics, animation, particles, camera spring, "
            "chain physics, HUD tweens) run faster or slower than authored "
            "depending on FPS.\n\n"
            "When ON: the engine integrates with real elapsed time per "
            "frame. Game runs at correct real-time speed at any render "
            "FPS or game-logic Hz. PathCam camera spring stays responsive "
            "instead of laggy. Animation and particles match real-time.\n\n"
            "When OFF: vanilla engine behaviour \xE2\x80\x94 every fixed-step "
            "subsystem advances by 0.003 sec per call regardless of how "
            "long actually elapsed. The game ships this way; sim_hz=60 "
            "approximates correct speed by accident, other values don't.\n\n"
            "Technical: writes flt_6FFCBC (0x6FFCBC) at the top of "
            "camera_apply_all_active (= real_render_dt for render-path "
            "consumers like PathCam/HUD/UV/screen-shake) and at the top "
            "of sim_aggregator (= real_sim_dt for sim-path consumers like "
            "physics_state_machine_tick / entity_transform_tick / "
            "anim_update_all_tracks). Throttles on render-path consumers "
            "are bypassed when this is on, since dt-correctness handles "
            "scaling correctly without needing decimation. See "
            "research/findings/dt-correctness-root-cause-2026-05-07.md.");

        if (dtc) {
            ImGui::TextDisabled("  Render-path writes:   %llu (last %.4f s)",
                                static_cast<unsigned long long>(mtr::dt_correctness::render_writes()),
                                mtr::dt_correctness::last_render_dt());
            ImGui::TextDisabled("  Sim-path writes:      %llu (last %.4f s)",
                                static_cast<unsigned long long>(mtr::dt_correctness::sim_writes()),
                                mtr::dt_correctness::last_sim_dt());
            ImGui::TextDisabled("  Alt-pump-path writes: %llu (last %.4f s)",
                                static_cast<unsigned long long>(mtr::dt_correctness::alt_pump_writes()),
                                mtr::dt_correctness::last_alt_pump_dt());
            ImGui::TextDisabled("  Particle dt scaled:   %llu times",
                                static_cast<unsigned long long>(mtr::dt_correctness::particle_dt_overrides()));
        }
        ImGui::Spacing();

        // One-click presets that flip the underlying toggles. Tweak any
        // toggle below afterward if you need fine-grained control.
        ImGui::TextDisabled("Quick setup:");
        ImGui::SameLine();
        if (ImGui::SmallButton("Recommended##decouple_preset")) {
            // dt-correctness ON + 60 Hz logic + camera smoothing + halo
            // follow-fix. Wilbur + NPC interp stay OFF (their fence
            // assumption is unverified — gated under "All smoothing").
            //
            // The halo follow-fix (M3.3, sub_6678D0 PRE hook) makes
            // view_interp safe to ship in this preset by keeping world
            // markers anchored to their entities while the camera is
            // interp'd between sim ticks. Before the fix, view_interp
            // caused mission-objective halos / NPC tags to visibly
            // drift relative to their entity as the camera rotated
            // (verified 2026-05-08 with Wilbur's mom highlight).
            mtr::dt_correctness::set_enabled(true);
            mtr::sim_decouple::set_mode(mtr::sim_decouple::Mode::THROTTLE);
            mtr::sim_decouple::set_target_hz(60);
            mtr::interp::set_view_interp_enabled(true);
            mtr::interp::set_halo_interp_enabled(true);
            mtr::interp::set_player_interp_enabled(false);
            mtr::interp::set_npc_interp_enabled(false);
            mtr::sim_decouple::set_auto_disable_in_minigame(true);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("All smoothing##decouple_preset")) {
            // Aggressive: everything on, including the experimental Wilbur
            // and NPC interp paths.
            mtr::dt_correctness::set_enabled(true);
            mtr::sim_decouple::set_mode(mtr::sim_decouple::Mode::THROTTLE);
            mtr::sim_decouple::set_target_hz(60);
            mtr::interp::set_view_interp_enabled(true);
            mtr::interp::set_halo_interp_enabled(true);
            mtr::interp::set_player_interp_enabled(true);
            mtr::interp::set_npc_interp_enabled(true);
            mtr::sim_decouple::set_auto_disable_in_minigame(true);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Throttle only##decouple_preset")) {
            mtr::dt_correctness::set_enabled(true);
            mtr::sim_decouple::set_mode(mtr::sim_decouple::Mode::THROTTLE);
            mtr::sim_decouple::set_target_hz(60);
            mtr::interp::set_view_interp_enabled(false);
            mtr::interp::set_halo_interp_enabled(false);
            mtr::interp::set_player_interp_enabled(false);
            mtr::interp::set_npc_interp_enabled(false);
            mtr::sim_decouple::set_auto_disable_in_minigame(true);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Off##decouple_preset")) {
            mtr::dt_correctness::set_enabled(false);
            mtr::sim_decouple::set_mode(mtr::sim_decouple::Mode::OFF);
            mtr::interp::set_view_interp_enabled(false);
            mtr::interp::set_halo_interp_enabled(false);
            mtr::interp::set_player_interp_enabled(false);
            mtr::interp::set_npc_interp_enabled(false);
        }
        ImGui::SameLine();
        info_pill(
            "Recommended - throttle + camera smoothing + halo follow-fix. "
            "Best balance for high-FPS displays. Wilbur / NPC smoothing "
            "stay off here because their fence assumption is unverified.\n\n"
            "All smoothing - throttle + camera + halo + Wilbur + NPC. "
            "Best looks if it works. May cause visible jank or blur on "
            "Wilbur in some scenes (the Wilbur / NPC paths assume the "
            "engine doesn't read entity transforms between camera-apply "
            "and next sim \xE2\x80\x94 that's unverified).\n\n"
            "Throttle only - 60 Hz logic, no smoothing. Useful if "
            "smoothing misbehaves but you still want capped game speed "
            "with high-rate render.\n\n"
            "Off - vanilla behaviour (no throttle, no smoothing). Use if "
            "anything else misbehaves.");
        ImGui::Spacing();

        using SDM = mtr::sim_decouple::Mode;
        const SDM cur_mode = mtr::sim_decouple::mode();
        bool throttle_on = (cur_mode == SDM::THROTTLE);
        if (ImGui::Checkbox("Lock game logic to 60 Hz", &throttle_on)) {
            mtr::sim_decouple::set_mode(throttle_on ? SDM::THROTTLE : SDM::OFF);
        }
        ImGui::SameLine();
        info_pill(
            "When ON, physics, animation, camera and AI run at a steady "
            "60 Hz no matter how fast you render. When OFF, those tick "
            "with each render frame - which is how the game ships, but "
            "any FPS unlock makes the game run too fast. Smoothing "
            "options below need this on to do anything.");

        ImGui::Spacing();
        int tgt = mtr::sim_decouple::target_hz();
        ImGui::SetNextItemWidth(280.0f);
        if (ImGui::SliderInt("Game logic rate##simhz", &tgt, 15, 240, "%d Hz")) {
            mtr::sim_decouple::set_target_hz(tgt);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("60##simhz_60")) { mtr::sim_decouple::set_target_hz(60); }
        ImGui::SameLine();
        info_pill(
            "Number of game-logic ticks per real second. 60 is the rate "
            "the game's physics and camera were authored for.\n\n"
            "When 'Engine timestep correction' is ON: any value gives "
            "correct real-time speed. Higher Hz = finer collision/AI "
            "resolution; lower Hz = less CPU. The smoothing layer below "
            "interpolates the visual between game-logic ticks.\n\n"
            "When 'Engine timestep correction' is OFF: only 60 Hz "
            "approximates correct speed. 30 = half-speed (slow-mo) and "
            "halves the resolution of camera collision, PathCam "
            "smoothing, chain physics. 120 = 2x fast-forward.\n\n"
            "Render frame rate is independent - set it under Tools > FPS "
            "limit. Setting render rate higher than sim rate is what the "
            "smoothing options below interpolate across.");
        if (tgt != 60 && !mtr::dt_correctness::enabled()) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                "  ! Game runs in %s at this rate (turn ON dt correction above, or use 60)",
                tgt < 60 ? "slow-motion" : "fast-forward");
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Live readings:");
        const double r_hz  = mtr::sim_decouple::measured_render_hz();
        const double s_hz  = mtr::sim_decouple::measured_sim_hz();
        ImGui::Text("  Render:      %6.1f FPS", r_hz);
        if (s_hz > 0.001) {
            ImGui::Text("  Game logic:  %6.1f Hz", s_hz);
        } else {
            ImGui::TextDisabled("  Game logic:     --- Hz  (waiting for first tick)");
        }
        const float a   = mtr::interp::current_alpha();
        const bool  cut = mtr::interp::is_cut_detected();
        ImGui::Text("  Blend:        %5.3f%s", a, cut ? "  (cut this frame)" : "");
        ImGui::TextDisabled("  Snapshots: %llu  Cuts: %llu  (smoothing ready: %s)",
                            static_cast<unsigned long long>(mtr::interp::snapshots_taken()),
                            static_cast<unsigned long long>(mtr::interp::cuts_detected()),
                            mtr::interp::has_two_snapshots() ? "yes" : "no");

        ImGui::TextDisabled("  Throttled this run: physics=%llu camera-path=%llu HUD=%llu UV=%llu",
                            static_cast<unsigned long long>(mtr::sim_decouple::sim_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::pathcam_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::overlay_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::uv_skipped()));
        ImGui::TextDisabled("  Alt loop: water=%llu cloth=%llu effects=%llu",
                            static_cast<unsigned long long>(mtr::sim_decouple::wave_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::chain_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::managed_skipped()));
        ImGui::TextDisabled("  Other: timers=%llu post-render=%llu alt-subsys=%llu audio=%llu",
                            static_cast<unsigned long long>(mtr::sim_decouple::timer_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::post_render_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::alt_subsys_skipped()),
                            static_cast<unsigned long long>(mtr::sim_decouple::alt_audio_skipped()));
        ImGui::TextDisabled("  Frame-dt corrections: %llu",
                            static_cast<unsigned long long>(mtr::sim_decouple::dt_corrections_applied()));

        ImGui::Spacing();
        if (ImGui::CollapsingHeader("Advanced - individual smoothing toggles, cut detection, aim snap")) {
        ImGui::Indent();
        ImGui::TextDisabled(
            "These are the individual knobs the presets above flip "
            "for you. Use them to A/B-test or run non-preset "
            "combinations. The Recommended preset already turns on "
            "the right combination for most users.");
        ImGui::Spacing();
        bool vi = mtr::interp::view_interp_enabled();
        if (ImGui::Checkbox("Smooth camera between sim ticks", &vi)) {
            mtr::interp::set_view_interp_enabled(vi);
        }
        ImGui::SameLine();
        info_pill(
            "Slerps the view matrix between sim ticks so the camera "
            "looks fluid at high render FPS even when sim_hz is low. "
            "Default OFF.\n\n"
            "Pair with \"Fix marker / halo offset\" below to keep the "
            "world markers (mission-objective halos, NPC tags, etc.) "
            "anchored to their entities while the camera is interp'd. "
            "Without that fix, markers visibly drift away from the "
            "entity they tag as the camera rotates.\n\n"
            "Technical: M3.1 view interp. POST hk_camera_apply_all_active "
            "(0x4C1E40). slerp(rotation) + lerp(translation) on the global "
            "view + world matrices (0x724C10 / 0x724C50). Inherent ~1 "
            "sim-window of latency vs uncapped render \xE2\x80\x94 that's the lerp-"
            "only design (D4).");
        if (vi) {
            ImGui::Indent();
            bool vics = mtr::interp::view_interp_camspace();
            if (ImGui::Checkbox("Use camera-world-space lerp (math-correct)", &vics)) {
                mtr::interp::set_view_interp_camspace(vics);
            }
            ImGui::SameLine();
            info_pill(
                "When ON (default): the view matrix is rebuilt each frame "
                "from interp(camera_world_pos) + slerp(rotation). This is "
                "the mathematically correct way to interpolate a view "
                "matrix because direct lerp(V0,V1) does not equal "
                "inv(lerp(cam0,cam1)).\n\n"
                "When OFF: direct slerp(rotation) + lerp(translation row). "
                "Approximately correct for small windows; can produce "
                "subtle warping artifacts on fast camera orbits at low "
                "sim Hz.\n\n"
                "Visible difference is sub-pixel at typical 60Hz sim + "
                "240Hz render. Mainly an A/B knob for verifying nothing "
                "regresses with the math fix.");

            bool hi = mtr::interp::halo_interp_enabled();
            if (ImGui::Checkbox("Fix marker / halo offset", &hi)) {
                mtr::interp::set_halo_interp_enabled(hi);
            }
            ImGui::SameLine();
            info_pill(
                "Keeps mission-objective halos, NPC tags, and other 3D "
                "world markers anchored to their entity while the camera "
                "is being interp'd between sim ticks.\n\n"
                "Without this: the halo's screen position is computed "
                "with the un-interp'd view (cached earlier in the frame), "
                "but the entity model is rendered with the interp'd view "
                "later. The mismatch shows up as a visible drift between "
                "the marker and the body it tags, modulating with camera "
                "rotation. Recommended ON whenever camera smoothing is "
                "ON.\n\n"
                "Technical: M3.3 halo follow-fix. PRE hook on "
                "HaloComponent::Update (sub_6678D0 @0x6678D0, vtable[+4] "
                "of vtable 0x6DD400, called from engine_pump_alt's per-"
                "frame component walk at 0x68149A). Save-write-restore "
                "fence on camera_struct[+0x110] (cached view-projection "
                "matrix). VP_interp = V_interp \xC3\x97 V_curr_inv \xC3\x97 VP_curr "
                "preserves any engine-specific adjustments inside VP "
                "while propagating the interp'd view delta. SEH-wrapped "
                "to skip the override if the camera struct is mid-tear-"
                "down (level transition).");
            ImGui::TextDisabled("  Halo overrides: %llu  skips: %llu",
                                static_cast<unsigned long long>(mtr::interp::halo_interp_writes()),
                                static_cast<unsigned long long>(mtr::interp::halo_interp_skips()));
            ImGui::Unindent();
        }
        ImGui::TextDisabled("  Frames written: %llu",
                            static_cast<unsigned long long>(mtr::interp::view_interp_writes()));

        ImGui::Spacing();
        bool pi = mtr::interp::player_interp_enabled();
        if (ImGui::Checkbox("Smooth Wilbur (player) between sim ticks", &pi)) {
            mtr::interp::set_player_interp_enabled(pi);
        }
        ImGui::SameLine();
        info_pill(
            "Smoothly blends Wilbur's position and rotation between the "
            "last two game-logic ticks each render frame. Pairs with the "
            "camera smoothing above for fluid third-person motion at "
            "high frame rates. Detects respawn / scripted teleports and "
            "snaps instead of smearing through them.\n\n"
            "Caveat: the engine may read entity transforms BETWEEN our "
            "camera-apply-POST write and the next sim's PRE-restore \xE2\x80\x94 if "
            "any such read computes a follow-spring or chase-camera, the "
            "result is jittery (fence assumption is unverified). If "
            "Wilbur looks blurry / janky with this on, turn it off and "
            "rely on view interp alone.\n\n"
            "Technical: M4 player interp. Entity pos at +0x58 (12 bytes), "
            "rotation 3x3 at +0x70 (36 bytes). Save-write-restore fence "
            "around the entity write (M2.3) so sim's PRE reads see clean "
            "(non-interp) state. SEH around per-tick writes against "
            "engine-freed entities.");
        ImGui::TextDisabled("  Frames written: %llu  teleports: %llu  handle swaps: %llu",
                            static_cast<unsigned long long>(mtr::interp::player_interp_writes()),
                            static_cast<unsigned long long>(mtr::interp::player_teleports_detected()),
                            static_cast<unsigned long long>(mtr::interp::player_handle_swaps()));
        const uint64_t fence_v = mtr::interp::player_fence_violations();
        if (fence_v > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.30f, 1.0f),
                "  Fence violations: %llu  (entity modified between our write and next sim - if this grows, the M4 fence assumption is broken)",
                static_cast<unsigned long long>(fence_v));
        } else if (pi) {
            ImGui::TextDisabled("  Fence violations: 0  (M4 save/write/restore fence is clean so far)");
        }
        if (ImGui::SmallButton("Force player relookup##player_relookup")) {
            mtr::interp::force_player_handle_refresh();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(use after mode change e.g. Wilbur \xe2\x86\x94 MiniHamster)");

        ImGui::Spacing();
        bool ni = mtr::interp::npc_interp_enabled();
        if (ImGui::Checkbox("Smooth NPCs between sim ticks", &ni)) {
            mtr::interp::set_npc_interp_enabled(ni);
        }
        ImGui::SameLine();
        info_pill(
            "Same idea as Wilbur smoothing but applied to every visible "
            "NPC (capped at 64 slots, aged-out after 6 sim ticks of "
            "absence). Per-NPC teleport detection means respawning "
            "enemies don't smear across the screen.\n\n"
            "Same fence caveat as Wilbur smoothing.\n\n"
            "Technical: M5 NPC interp. Walks the engine's per-frame "
            "transform list (dword_724DE4), reads inner-transform ptr at "
            "node+0x5C, applies the same lerp+slerp+fence pattern as M4 "
            "to each. Skips the player (M4 covers it) and any node with "
            "the +0x44 0x10 \"skip transform\" flag. Per-entity SEH "
            "guards against entities the engine freed mid-frame.");
        ImGui::TextDisabled("  Slots active: %llu  writes: %llu  teleports: %llu",
                            static_cast<unsigned long long>(mtr::interp::npc_active_slots()),
                            static_cast<unsigned long long>(mtr::interp::npc_interp_writes()),
                            static_cast<unsigned long long>(mtr::interp::npc_teleports_detected()));

        float t_tp = mtr::interp::player_teleport_threshold();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Teleport-detection distance", &t_tp, 1.0f, 100.0f, "%.1f units")) {
            mtr::interp::set_player_teleport_threshold(t_tp);
        }
        ImGui::SameLine();
        info_pill(
            "If Wilbur or an NPC moves further than this in a single "
            "game-logic tick, treat it as a teleport (respawn / script "
            "warp) and snap instead of smoothing. 10 is a good default - "
            "covers respawns without triggering on sprint or slide.");

        ImGui::Spacing();
        bool as = mtr::interp::aim_snap_enabled();
        if (ImGui::Checkbox("Aim snap (hold a key for zero-latency aim)", &as)) {
            mtr::interp::set_aim_snap_enabled(as);
        }
        ImGui::SameLine();
        info_pill(
            "While the bound key is held, the smoothing falls back to the "
            "freshest game-logic state (no interpolation lag). Useful for "
            "zero-latency aiming. Default key: right mouse button.");
        int vk = mtr::interp::aim_snap_vk();
        const char* vk_name =
            (vk == 0x01) ? "LMB" : (vk == 0x02) ? "RMB" : (vk == 0x04) ? "MMB" :
            (vk == 0x10) ? "Shift" : (vk == 0x11) ? "Ctrl" : (vk == 0x12) ? "Alt" :
            (vk == 0x46) ? "F" : (vk == 0x20) ? "Space" : "(custom)";
        ImGui::TextDisabled("  Bound: %s   Pressed now: %s", vk_name,
                            mtr::interp::aim_snap_active() ? "yes" : "no");
        if (ImGui::SmallButton("RMB##aimkey"))    mtr::interp::set_aim_snap_vk(0x02);
        ImGui::SameLine();
        if (ImGui::SmallButton("LMB##aimkey"))    mtr::interp::set_aim_snap_vk(0x01);
        ImGui::SameLine();
        if (ImGui::SmallButton("MMB##aimkey"))    mtr::interp::set_aim_snap_vk(0x04);
        ImGui::SameLine();
        if (ImGui::SmallButton("Shift##aimkey"))  mtr::interp::set_aim_snap_vk(0x10);
        ImGui::SameLine();
        if (ImGui::SmallButton("F##aimkey"))      mtr::interp::set_aim_snap_vk(0x46);

        ImGui::Spacing();
        ImGui::TextDisabled("Cut detection (skip smoothing on big jumps):");
        float t_thr = mtr::interp::cut_translation_threshold();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Position jump##cut", &t_thr, 0.5f, 50.0f, "%.2f units")) {
            mtr::interp::set_cut_translation_threshold(t_thr);
        }
        ImGui::SameLine();
        info_pill(
            "If the camera position jumps more than this in one game-logic "
            "tick, treat it as a cut and don't smooth across it. 5 units "
            "is the default. Lower = catches more cuts. Higher = fewer "
            "false positives during fast travel.");
        float r_thr = mtr::interp::cut_rotation_threshold_deg();
        ImGui::SetNextItemWidth(220.0f);
        if (ImGui::SliderFloat("Rotation jump##cut",  &r_thr, 5.0f, 180.0f, "%.0f\xC2\xB0")) {
            mtr::interp::set_cut_rotation_threshold_deg(r_thr);
        }
        ImGui::SameLine();
        info_pill(
            "If the camera rotates more than this in one game-logic tick, "
            "treat it as a cut. 30\xC2\xB0 is the default - matches a fast spin "
            "without tripping.");

        ImGui::Unindent();
        } // CollapsingHeader("Advanced ...")

        ImGui::Spacing();
        bool autodis = mtr::sim_decouple::auto_disable_in_minigame();
        if (ImGui::Checkbox("Auto-disable in mini-games", &autodis)) {
            mtr::sim_decouple::set_auto_disable_in_minigame(autodis);
        }
        ImGui::SameLine();
        info_pill(
            "When on (default), automatically turns the smoothing off "
            "during DigDug / MiniHamster / ChargeBall - those mini-games "
            "have their own simulation paths and don't need this. Disable "
            "only if you want to push past the supported scope.");

        const auto pm = mtr::sim_decouple::current_player_mode();
        const bool   mg = mtr::sim_decouple::minigame_detected();
        const char*  pml = mtr::sim_decouple::player_mode_label(pm);
        ImGui::Text("  Current mode: %s%s", pml,
                    (mg && autodis) ? "  (smoothing paused)" : "");

        ImGui::Spacing();
        bool log_on = mtr::sim_decouple::detailed_log_enabled();
        if (ImGui::Checkbox("Detailed log file", &log_on)) {
            mtr::sim_decouple::set_detailed_log_enabled(log_on);
        }
        ImGui::SameLine();
        info_pill(
            "Writes per-tick diagnostics to Game/mtr-asi-decouple.log. "
            "Off by default. Useful for debugging if smoothing misbehaves "
            "in some scene.");

        section_end(open);
    }
}

} // namespace mtr::menu::detail
