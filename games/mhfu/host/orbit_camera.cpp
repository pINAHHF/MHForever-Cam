#include "orbit_camera.hpp"

#include "mhf/psp/host.hpp"

#if __has_include("mhfu_camera_support.hpp")
#include "mhfu_camera_support.hpp"
#else
#define MHF_MHFU_GAME_SYS_SINGLETON 0u
#endif

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

namespace mhf::psp {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;
constexpr float kAngleUnitsPerRadian = 32768.0f / kPi;

// MHP2G/MHFU camera object layout, expressed relative to the STD SubCamera
// passed as this ($a0). Camera::subCameras[0] begins at Camera + 0xB0.
constexpr std::uint32_t kCameraFromSubCamera = 0xB0u;
constexpr std::uint32_t kSubCameraData = 0xA0u;
constexpr std::uint32_t kSubCameraStride = 0x1A0u;
constexpr std::uint32_t kSubCameraIsActive = 0x91u;
constexpr std::uint32_t kSubCameraCurrentId = 0x92u;
constexpr std::uint32_t kSubCameraState = 0x9Cu;

constexpr std::uint32_t kCameraHeightId = 0x14u;
constexpr std::uint32_t kCameraButtons = 0xA70u;
constexpr std::uint32_t kCameraRisingEdge = 0xA72u;
constexpr std::uint32_t kCameraWyvernFindPlayer = 0xA81u;
constexpr std::uint32_t kCameraIsChangingArea = 0xA8Bu;
constexpr std::uint32_t kCameraLastSubCameraId = 0xA8Cu;
constexpr std::uint32_t kCameraAreaSubCameraId = 0xA8Du;
constexpr std::uint32_t kCameraPlayer = 0xA84u;
constexpr std::uint32_t kCameraAreaConfig = 0xA90u;

constexpr std::uint32_t kStdIsFalldown = 0x35u;
constexpr std::uint32_t kStdGoalRotation = 0x80u;
constexpr std::uint32_t kStdButtons = 0x84u;
constexpr std::uint32_t kStdRisingEdge = 0x86u;
constexpr std::uint32_t kStdIsViewBlocked = 0x89u;
constexpr std::uint32_t kStdKabegiwaTimer = 0x8Bu;
constexpr std::uint32_t kStdIsKabegiwa = 0x8Cu;
constexpr std::uint32_t kStdInertiaTimer = 0x8Du;
constexpr std::uint32_t kStdIsFastRotate = 0x8Eu;
constexpr std::uint32_t kStdIsShoulderCam = 0x8Fu;
constexpr std::uint32_t kStdGunTargetingState = 0x91u;

constexpr std::uint32_t kPlayerRotationY = 0x1F4u;
constexpr std::uint32_t kPlayerPosture = 0x280u;
constexpr std::uint32_t kPlayerActionType = 0x294u;
constexpr std::uint32_t kPlayerActionId = 0x295u;
constexpr std::uint32_t kPlayerRideState = 0x298u;
constexpr std::uint32_t kPlayerDialogTimer = 0x552u;
constexpr std::uint32_t kPlayerScopeAimIncrement = 0x564u;
constexpr std::uint32_t kPlayerFlags = 0x614u;

constexpr std::uint32_t kGameSysFlags = 0x6AF14u;
constexpr std::uint32_t kLobbyTaskFlag = 1u << 0u;

constexpr std::uint32_t kAreaMoveType = 0x02u;
constexpr std::uint32_t kAreaHeightDefinition = 0x20u;
constexpr std::uint32_t kFollowEntriesOffset = 0x20u;
constexpr std::uint32_t kFollowEntryStride = 0x20u;
constexpr std::uint32_t kFollowEntryCount = 5u;
constexpr std::uint32_t kFollowEntryYOffset = 0x04u;
constexpr std::uint32_t kFollowEntryZOffset = 0x08u;
constexpr std::uint32_t kFollowEntryHeight = 0x10u;
constexpr std::uint32_t kFollowEntryMinHeight = 0x18u;

constexpr std::uint32_t kPlFlagToolbeltOpen = 1u << 0u;
constexpr std::uint32_t kPlFlagSupplyBoxOpen = 1u << 1u;
constexpr std::uint32_t kPlFlagDeliveryBoxOpen = 1u << 9u;
constexpr std::uint32_t kPlFlagScopeCam = 1u << 12u;
constexpr std::uint32_t kPlFlagShoulderCam = 1u << 15u;
constexpr std::uint32_t kPlFlagFishing = 1u << 19u;

constexpr float kDeadzone = 0.16f;
constexpr float kYawSpeed = 2.8f;
constexpr float kPitchSpeed = 2.2f;
constexpr float kMinElevation = -1.15f;
constexpr float kMaxElevation = 1.15f;
constexpr float kMinOrbitDistance = 80.0f;
constexpr float kMaxOrbitDistance = 1800.0f;

struct OrbitState {
    bool active{};
    std::uint32_t sub_camera{};
    std::uint32_t camera{};
    std::uint32_t area{};
    std::uint32_t player{};
    std::uint32_t follow_entry{};
    std::uint64_t last_update_us{};
    float yaw{};
    float elevation{};
    float initial_elevation{};
    float distance{};
    float target_height{};
    float min_height{};
};

OrbitState g_orbit;

bool range_ok(const psprecomp::GuestMemory &memory, std::uint32_t base,
              std::uint32_t offset, std::size_t length) {
    if (base == 0u || offset > UINT32_MAX - base) return false;
    return memory.contains(base + offset, length);
}

float load_float(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    return std::bit_cast<float>(memory.load32(address));
}

void store_float(psprecomp::GuestMemory &memory, std::uint32_t address, float value) {
    memory.store32(address, std::bit_cast<std::uint32_t>(value));
}

float shape_axis(float value) {
    value = std::clamp(value, -1.0f, 1.0f);
    const float magnitude = std::fabs(value);
    if (magnitude <= kDeadzone) return 0.0f;
    const float shaped = (magnitude - kDeadzone) / (1.0f - kDeadzone);
    return std::copysign(shaped, value);
}

float wrap_angle(float angle) {
    angle = std::fmod(angle, kTwoPi);
    if (angle < 0.0f) angle += kTwoPi;
    return angle;
}

std::uint16_t angle_to_units(float radians) {
    const auto rounded = static_cast<std::int32_t>(std::lround(radians * kAngleUnitsPerRadian));
    return static_cast<std::uint16_t>(rounded);
}

bool is_lobby_or_unknown(const psprecomp::GuestMemory &memory) {
#if MHF_MHFU_GAME_SYS_SINGLETON == 0
    (void)memory;
    return true;
#else
    constexpr std::uint32_t slot = MHF_MHFU_GAME_SYS_SINGLETON;
    if (!memory.contains(slot, 4u)) return true;
    const std::uint32_t game_sys = memory.load32(slot);
    if (game_sys < 0x08800000u || game_sys >= 0x0A000000u ||
        !range_ok(memory, game_sys, kGameSysFlags, 4u) ||
        !range_ok(memory, game_sys, 0x28u, 1u) ||
        !range_ok(memory, game_sys, 0x422u, 1u) ||
        !range_ok(memory, game_sys, 0x480u, 1u)) {
        return true;
    }
    // These independent fields make a false-positive singleton match very unlikely.
    const std::uint8_t player_id = memory.load8(game_sys + 0x28u);
    const std::uint8_t is_demo = memory.load8(game_sys + 0x422u);
    const std::uint8_t is_gallery_task = memory.load8(game_sys + 0x480u);
    if (player_id >= 4u || is_demo > 1u || is_gallery_task > 1u) return true;
    if (is_demo != 0u || is_gallery_task != 0u) return true;
    return (memory.load32(game_sys + kGameSysFlags) & kLobbyTaskFlag) != 0u;
#endif
}

bool special_sub_camera_active(const psprecomp::GuestMemory &memory, std::uint32_t camera) {
    // Camera::cam2view selects DEMO (5), PCHNGR (3), or PLAYER_EX (4).
    constexpr std::uint32_t special_ids[] = {3u, 4u, 5u};
    for (const std::uint32_t id : special_ids) {
        const std::uint32_t active = camera + 0xB0u + id * kSubCameraStride + kSubCameraIsActive;
        if (!memory.contains(active, 1u) || memory.load8(active) != 0u) return true;
    }
    return false;
}

bool special_player_state(const psprecomp::GuestMemory &memory, std::uint32_t player,
                          std::uint32_t std_data) {
    if (!range_ok(memory, player, kPlayerFlags, 4u) ||
        !range_ok(memory, player, kPlayerActionType, 2u) ||
        !range_ok(memory, player, kPlayerRideState, 1u) ||
        !range_ok(memory, player, kPlayerPosture, 1u) ||
        !range_ok(memory, player, kPlayerDialogTimer, 1u) ||
        !range_ok(memory, player, kPlayerScopeAimIncrement, 1u)) {
        return true;
    }

    const std::uint8_t action_type = memory.load8(player + kPlayerActionType);
    // NORMAL and ATTACK are ordinary hunt states. Damage, death, demos,
    // egg-carrying, chat, kitchen and farm actions retain the stock camera.
    if (action_type != 0u && action_type != 1u) return true;

    const std::uint8_t action_id = memory.load8(player + kPlayerActionId);
    if (action_id == 17u || action_id == 27u || action_id == 30u || action_id == 58u) {
        return true; // hang, climb-from-hang, finish-climb-from-hang
    }

    if (memory.load8(player + kPlayerRideState) != 0u ||
        memory.load8(player + kPlayerPosture) == 2u ||
        memory.load8(player + kPlayerDialogTimer) != 0u ||
        memory.load8(player + kPlayerScopeAimIncrement) != 0u) {
        return true;
    }

    const std::uint32_t player_flags = memory.load32(player + kPlayerFlags);
    if ((player_flags & (kPlFlagToolbeltOpen | kPlFlagSupplyBoxOpen | kPlFlagDeliveryBoxOpen |
                        kPlFlagScopeCam | kPlFlagShoulderCam | kPlFlagFishing)) != 0u) {
        return true;
    }

    if (!range_ok(memory, std_data, kStdIsFalldown, 1u) ||
        !range_ok(memory, std_data, kStdIsViewBlocked, 1u) ||
        !range_ok(memory, std_data, kStdGunTargetingState, 1u) ||
        !range_ok(memory, std_data, kStdIsShoulderCam, 1u) ||
        !range_ok(memory, std_data, kStdIsKabegiwa, 1u) ||
        !range_ok(memory, std_data, kStdKabegiwaTimer, 1u) ||
        !range_ok(memory, std_data, kStdIsFastRotate, 1u)) {
        return true;
    }

    if (memory.load8(std_data + kStdIsFalldown) != 0u ||
        memory.load8(std_data + kStdIsViewBlocked) != 0u ||
        memory.load8(std_data + kStdIsKabegiwa) != 0u ||
        memory.load8(std_data + kStdKabegiwaTimer) != 0u ||
        static_cast<std::int8_t>(memory.load8(std_data + kStdGunTargetingState)) >= 0 ||
        memory.load8(std_data + kStdIsShoulderCam) != 0u ||
        memory.load8(std_data + kStdIsFastRotate) != 0u) {
        return true;
    }

    return false;
}

bool standard_camera_context(const psprecomp::GuestMemory &memory, std::uint32_t sub_camera,
                             std::uint32_t camera, std::uint32_t &area_out,
                             std::uint32_t &player_out) {
    if (!range_ok(memory, camera, kCameraAreaSubCameraId, 1u) ||
        !range_ok(memory, camera, kCameraAreaConfig, 4u) ||
        !range_ok(memory, camera, kCameraPlayer, 4u) ||
        !range_ok(memory, camera, kCameraIsChangingArea, 1u) ||
        !range_ok(memory, camera, kCameraLastSubCameraId, 1u) ||
        !range_ok(memory, camera, kCameraWyvernFindPlayer, 1u) ||
        !range_ok(memory, camera, kCameraHeightId, 1u) ||
        !range_ok(memory, camera, kCameraButtons, 4u) ||
        !range_ok(memory, camera, 0xDC4u, 1u) ||
        !range_ok(memory, sub_camera, kSubCameraCurrentId, 1u) ||
        !range_ok(memory, sub_camera, kSubCameraState, 1u)) {
        return false;
    }

    if (memory.load8(camera + kCameraAreaSubCameraId) != 0u ||
        memory.load8(camera + kCameraIsChangingArea) != 0u ||
        memory.load8(camera + kCameraWyvernFindPlayer) != 0u) {
        return false;
    }

    const std::uint8_t last_sub_camera = memory.load8(camera + kCameraLastSubCameraId);
    if (last_sub_camera != 0u) return false;

    if (memory.load8(sub_camera + kSubCameraCurrentId) != 0xFFu ||
        memory.load8(sub_camera + kSubCameraState) != 0u) {
        return false;
    }

    if (special_sub_camera_active(memory, camera)) return false;

    const std::uint8_t active_sub_camera = memory.load8(camera + 0xDC4u);
    if (active_sub_camera == 3u || active_sub_camera == 4u || active_sub_camera == 5u) {
        return false;
    }

    area_out = memory.load32(camera + kCameraAreaConfig);
    player_out = memory.load32(camera + kCameraPlayer);
    if (area_out == 0u || player_out == 0u ||
        !range_ok(memory, area_out, kAreaHeightDefinition + kFollowEntriesOffset +
                                       kFollowEntryCount * kFollowEntryStride, 1u)) {
        return false;
    }

    // Only standard follow areas are eligible. Fixed pans, rails and offset pans
    // keep their original camera path, just like scripted and special cameras.
    if (memory.load8(area_out + kAreaMoveType) != 0u) return false;
    if (is_lobby_or_unknown(memory)) return false;

    return true;
}

bool capture_orbit(OrbitState &state, const psprecomp::GuestMemory &memory,
                   std::uint32_t sub_camera, std::uint32_t camera,
                   std::uint32_t area, std::uint32_t player, std::uint32_t std_data) {
    const std::uint8_t height_id = memory.load8(camera + kCameraHeightId);
    const std::uint32_t index = height_id == 0u ? 4u : std::min<std::uint32_t>(height_id - 1u, 4u);
    const std::uint32_t entry = area + kAreaHeightDefinition + kFollowEntriesOffset +
                                index * kFollowEntryStride;
    if (!memory.contains(entry, kFollowEntryStride) ||
        !range_ok(memory, std_data, kStdGoalRotation, 4u)) {
        return false;
    }

    const float y_offset = load_float(memory, entry + kFollowEntryYOffset);
    const float z_offset = load_float(memory, entry + kFollowEntryZOffset);
    const float height = load_float(memory, entry + kFollowEntryHeight);
    const float min_height = load_float(memory, entry + kFollowEntryMinHeight);
    if (!std::isfinite(y_offset) || !std::isfinite(z_offset) ||
        !std::isfinite(height) || !std::isfinite(min_height) ||
        std::fabs(y_offset) > 5000.0f || std::fabs(z_offset) > 5000.0f ||
        std::fabs(height) > 5000.0f || min_height < 0.0f || min_height > 2000.0f) {
        return false;
    }

    const float offset_y = y_offset - height;
    const float distance = std::hypot(offset_y, z_offset);
    if (!std::isfinite(distance) || distance < kMinOrbitDistance ||
        distance > kMaxOrbitDistance) {
        return false;
    }

    const std::int16_t rotation = static_cast<std::int16_t>(memory.load16(std_data + kStdGoalRotation + 2u));
    state = {};
    state.active = true;
    state.sub_camera = sub_camera;
    state.camera = camera;
    state.area = area;
    state.player = player;
    state.follow_entry = entry;
    state.distance = distance;
    state.elevation = std::clamp(std::atan2(offset_y, z_offset), kMinElevation, kMaxElevation);
    state.initial_elevation = state.elevation;
    state.target_height = height;
    state.min_height = min_height;
    state.yaw = wrap_angle(static_cast<float>(rotation) / kAngleUnitsPerRadian);
    return true;
}

void fallback(Runtime &runtime, Context &context, GuestFunction original) {
    g_orbit = {};
    runtime.note_analog_camera_update(0u);
    if (original != nullptr) original(runtime, context);
}

void replace_standard_camera(Runtime &runtime, Context &context, GuestFunction original) {
    auto &memory = runtime.memory();
    const std::uint32_t sub_camera = context.gpr[4]; // SubCamera::this
    if (sub_camera < kCameraFromSubCamera) {
        fallback(runtime, context, original);
        return;
    }

    const std::uint32_t camera = sub_camera - kCameraFromSubCamera;
    const std::uint32_t std_data = sub_camera + kSubCameraData;
    if (!range_ok(memory, std_data, kStdGoalRotation, 4u) ||
        !range_ok(memory, sub_camera, 0u, 0xA0u)) {
        fallback(runtime, context, original);
        return;
    }

    std::uint32_t area = 0u;
    std::uint32_t player = 0u;
    if (!standard_camera_context(memory, sub_camera, camera, area, player) ||
        special_player_state(memory, player, std_data)) {
        fallback(runtime, context, original);
        return;
    }

    // A wall/ceiling transition still uses the native camera resolver. Aiming,
    // climbing, falls, rides, scripted areas and cutscenes took the fallback above.
    if (!range_ok(memory, area, 0u, 0xE0u) ||
        !range_ok(memory, player, kPlayerRotationY, 2u) ||
        !range_ok(memory, area, 0x40u, 0x20u)) {
        fallback(runtime, context, original);
        return;
    }

    const std::uint8_t selected_height_id = memory.load8(camera + kCameraHeightId);
    const std::uint32_t selected_height_index = selected_height_id == 0u
        ? 4u : std::min<std::uint32_t>(selected_height_id - 1u, 4u);
    const std::uint32_t selected_entry = area + kAreaHeightDefinition + kFollowEntriesOffset +
                                         selected_height_index * kFollowEntryStride;
    if (!g_orbit.active || g_orbit.camera != camera ||
        g_orbit.sub_camera != sub_camera || g_orbit.area != area ||
        g_orbit.player != player || g_orbit.follow_entry != selected_entry) {
        if (!capture_orbit(g_orbit, memory, sub_camera, camera, area, player, std_data)) {
            fallback(runtime, context, original);
            return;
        }
    }

    const std::uint64_t now = runtime.now_us();
    float dt = 1.0f / 60.0f;
    if (g_orbit.last_update_us != 0u && now >= g_orbit.last_update_us) {
        dt = std::clamp(static_cast<float>(now - g_orbit.last_update_us) / 1'000'000.0f, 0.0f, 0.05f);
    } else if (g_orbit.last_update_us != 0u && now < g_orbit.last_update_us) {
        g_orbit.last_update_us = 0u;
    }

    runtime.note_analog_camera_update(now);
    const ControllerState &pad = controller_state(runtime);
    const float dx = shape_axis(pad.rx);
    const float dy = shape_axis(pad.ry);

    if ((pad.buttons & buttons::kLTrigger) != 0u) {
        const auto player_rotation = static_cast<std::int16_t>(memory.load16(player + kPlayerRotationY));
        const std::int32_t recentered = static_cast<std::int32_t>(player_rotation) + 0x8000;
        g_orbit.yaw = wrap_angle(static_cast<float>(recentered) / kAngleUnitsPerRadian);
        g_orbit.elevation = g_orbit.initial_elevation;
    } else {
        // Shape the axes independently. There is deliberately no radial deadzone
        // or diagonal normalization: moving one axis cannot inject motion into the other.
        g_orbit.yaw = wrap_angle(g_orbit.yaw - dx * kYawSpeed * dt);
        g_orbit.elevation = std::clamp(g_orbit.elevation + dy * kPitchSpeed * dt,
                                       kMinElevation, kMaxElevation);
    }

    g_orbit.last_update_us = now;

    // The native camera routine is retained as the wall/ground/safety solver.
    // D-pad stage changes are masked for this call. We temporarily rewrite only
    // the currently selected follow entry with our orbit elevation, then restore
    // that profile and every input byte after native collision handling.
    const std::uint32_t orbit_entry = g_orbit.follow_entry;
    if (!memory.contains(orbit_entry, kFollowEntryStride) ||
        !range_ok(memory, camera, kCameraHeightId, 1u) ||
        !range_ok(memory, std_data, kStdButtons, 4u)) {
        fallback(runtime, context, original);
        return;
    }

    const std::uint32_t saved_entry_y = memory.load32(orbit_entry + kFollowEntryYOffset);
    const std::uint32_t saved_entry_z = memory.load32(orbit_entry + kFollowEntryZOffset);
    const std::uint32_t saved_entry_height = memory.load32(orbit_entry + kFollowEntryHeight);
    const std::uint32_t saved_entry_min_height = memory.load32(orbit_entry + kFollowEntryMinHeight);
    const std::uint16_t saved_camera_buttons = memory.load16(camera + kCameraButtons);
    const std::uint16_t saved_camera_rising_edge = memory.load16(camera + kCameraRisingEdge);
    const std::uint16_t saved_buttons = memory.load16(std_data + kStdButtons);
    const std::uint16_t saved_rising_edge = memory.load16(std_data + kStdRisingEdge);

    const float camera_y_offset = g_orbit.target_height + g_orbit.distance * std::sin(g_orbit.elevation);
    const float camera_z_offset = g_orbit.distance * std::cos(g_orbit.elevation);
    if (!std::isfinite(camera_y_offset) || !std::isfinite(camera_z_offset)) {
        fallback(runtime, context, original);
        return;
    }

    store_float(memory, orbit_entry + kFollowEntryYOffset, camera_y_offset);
    store_float(memory, orbit_entry + kFollowEntryZOffset, camera_z_offset);
    store_float(memory, orbit_entry + kFollowEntryHeight, g_orbit.target_height);
    store_float(memory, orbit_entry + kFollowEntryMinHeight, g_orbit.min_height);

    const std::uint16_t yaw_units = angle_to_units(g_orbit.yaw);
    // std_cam_sw_set_sub() copies these Camera-level fields into StdCameraData.
    // Mask them only for the native camera call; the player's ordinary action
    // inputs live in Player.buttons and remain untouched.
    memory.store16(camera + kCameraButtons, 0u);
    memory.store16(camera + kCameraRisingEdge, 0u);
    memory.store16(std_data + kStdButtons, 0u);
    memory.store16(std_data + kStdRisingEdge, 0u);
    memory.store8(std_data + kStdIsViewBlocked, 0u);
    memory.store8(std_data + kStdIsFastRotate, 0u);
    memory.store8(std_data + kStdInertiaTimer, 0u);
    memory.store32(std_data + kStdGoalRotation,
                   static_cast<std::uint32_t>(yaw_units) |
                   (static_cast<std::uint32_t>(yaw_units) << 16u));

    if (original != nullptr) {
        original(runtime, context);
    }

    // If native code deliberately changed the yaw after our input (for example
    // its player-turn/auto-recenter branch), hand control back to the stock
    // camera instead of letting the two controllers fight over subsequent frames.
    const bool native_camera_override = memory.load16(std_data + kStdGoalRotation) != yaw_units ||
        memory.load8(std_data + kStdIsShoulderCam) != 0u ||
        static_cast<std::int8_t>(memory.load8(std_data + kStdGunTargetingState)) >= 0 ||
        memory.load8(std_data + kStdIsFastRotate) != 0u ||
        memory.load8(std_data + kStdIsFalldown) != 0u ||
        memory.load8(std_data + kStdIsKabegiwa) != 0u ||
        memory.load8(std_data + kStdIsViewBlocked) != 0u ||
        memory.load8(std_data + kStdKabegiwaTimer) != 0u;

    // Do not leave the temporary camera profile or masked controls in guest
    // memory. The native function keeps its valid pointer to the selected entry.
    // The next frame rebuilds the orbit profile from
    // the independent host-side yaw/elevation state.
    memory.store32(orbit_entry + kFollowEntryYOffset, saved_entry_y);
    memory.store32(orbit_entry + kFollowEntryZOffset, saved_entry_z);
    memory.store32(orbit_entry + kFollowEntryHeight, saved_entry_height);
    memory.store32(orbit_entry + kFollowEntryMinHeight, saved_entry_min_height);
    memory.store16(camera + kCameraButtons, saved_camera_buttons);
    memory.store16(camera + kCameraRisingEdge, saved_camera_rising_edge);
    memory.store16(std_data + kStdButtons, saved_buttons);
    memory.store16(std_data + kStdRisingEdge, saved_rising_edge);

    if (native_camera_override) {
        g_orbit = {};
        runtime.note_analog_camera_update(0u);
    }
}

} // namespace

void register_mhfu_orbit_camera(Runtime &runtime) {
    runtime.register_replacement("mh_camera_orbit_update", &replace_standard_camera);
}

} // namespace mhf::psp
