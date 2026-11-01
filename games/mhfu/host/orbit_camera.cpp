#include "orbit_camera.hpp"

#include "mhf/psp/host.hpp"

#if __has_include("mhfu_camera_support.hpp")
#include "mhfu_camera_support.hpp"
#else
#define MHF_MHFU_GAME_SYS_SINGLETON 0u
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mhf::psp {
namespace {

bool camera_trace_enabled() {
    static const bool enabled = std::getenv("MHF_CAMERA_LOG") != nullptr;
    return enabled;
}

void camera_trace(const char *reason) {
    if (!camera_trace_enabled()) return;
    static std::array<std::array<char, 192>, 16> seen{};
    for (const auto &entry : seen) {
        if (entry[0] != '\0' && std::strcmp(entry.data(), reason) == 0) return;
    }
    for (auto &entry : seen) {
        if (entry[0] == '\0') {
            std::snprintf(entry.data(), entry.size(), "%s", reason);
            std::fprintf(stderr, "[mhfu-camera] %s\n", reason);
            return;
        }
    }
}

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

constexpr std::uint32_t kCameraWyvernFindPlayer = 0xA81u;
constexpr std::uint32_t kCameraIsChangingArea = 0xA8Bu;
constexpr std::uint32_t kCameraLastSubCameraId = 0xA8Cu;
constexpr std::uint32_t kCameraAreaSubCameraId = 0xA8Du;
constexpr std::uint32_t kCameraPlayer = 0xA84u;
constexpr std::uint32_t kCameraAreaConfig = 0xA90u;

constexpr std::uint32_t kStdIsFalldown = 0x35u;
constexpr std::uint32_t kStdIsViewBlocked = 0x89u;
constexpr std::uint32_t kStdKabegiwaTimer = 0x8Bu;
constexpr std::uint32_t kStdIsKabegiwa = 0x8Cu;
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
constexpr std::uint32_t kPlayerPositionX = 0x200u;
constexpr std::uint32_t kPlayerTransform = 0x10u;

constexpr std::uint32_t kAreaMoveType = 0x02u;
constexpr std::uint32_t kAreaPanCameraPosition = 0x20u;
constexpr std::uint32_t kAreaPanTargetPosition = 0x2Cu;
constexpr std::uint32_t kAreaPanModelOffset = 0x38u;
constexpr std::uint32_t kAreaPanWorldOffset = 0x44u;

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

constexpr std::uint8_t kOrbitModeStandard = 1u;
constexpr std::uint8_t kOrbitModeOffsetPan = 2u;

struct OrbitState {
    bool active{};
    std::uint8_t mode{};
    std::uint8_t area_id{};
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
    float base_pan_x{};
    float base_pan_y{};
    float base_pan_z{};
};

OrbitState g_orbit;

void fallback(Runtime &runtime, Context &context, GuestFunction original, const char *reason);

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

// Rotate a camera position around the player's XZ target. The native STD
// routine can request a one-frame recenter while the orbit is active; applying
// the inverse angular correction to its resolved output preserves collision
// adjustments without allowing that recenter to take ownership of the view.


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


int active_special_sub_camera(const psprecomp::GuestMemory &memory, std::uint32_t camera) {
    // Camera::cam2view selects DEMO (5), PCHNGR (3), or PLAYER_EX (4).
    // Return the exact active camera so a fallback can be diagnosed without
    // changing the stock protection against scripted/aiming camera overrides.
    constexpr std::uint32_t special_ids[] = {3u, 4u, 5u};
    for (const std::uint32_t id : special_ids) {
        const std::uint32_t active = camera + 0xB0u + id * kSubCameraStride + kSubCameraIsActive;
        if (!memory.contains(active, 1u)) return -2;
        if (memory.load8(active) != 0u) return static_cast<int>(id);
    }
    return -1;
}

bool special_player_action(const psprecomp::GuestMemory &memory, std::uint32_t player) {
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
    return (player_flags & (kPlFlagToolbeltOpen | kPlFlagSupplyBoxOpen | kPlFlagDeliveryBoxOpen |
                            kPlFlagScopeCam | kPlFlagShoulderCam | kPlFlagFishing)) != 0u;
}







bool read_offset_pan_target(const psprecomp::GuestMemory &memory, std::uint32_t area,
                            std::uint32_t player, float player_x, float player_y, float player_z,
                            float &target_x, float &target_y, float &target_z) {
    if (!range_ok(memory, area, kAreaPanTargetPosition, 0x28u)) return false;

    const std::uint8_t target_type = memory.load8(area + 0x04u);
    switch (target_type) {
    case 0u: // FIXED
        target_x = load_float(memory, area + kAreaPanTargetPosition);
        target_y = load_float(memory, area + kAreaPanTargetPosition + 4u);
        target_z = load_float(memory, area + kAreaPanTargetPosition + 8u);
        break;
    case 1u: { // PLAYER_MODEL_OFFSET: transform the area-local offset by Player::transform
        if (!range_ok(memory, player, kPlayerTransform, 0x40u) ||
            !range_ok(memory, area, kAreaPanModelOffset, 12u)) return false;
        const float ox = load_float(memory, area + kAreaPanModelOffset);
        const float oy = load_float(memory, area + kAreaPanModelOffset + 4u);
        const float oz = load_float(memory, area + kAreaPanModelOffset + 8u);
        const std::uint32_t m = player + kPlayerTransform;
        target_x = load_float(memory, m + 0x00u) * ox + load_float(memory, m + 0x04u) * oy +
                   load_float(memory, m + 0x08u) * oz + load_float(memory, m + 0x30u);
        target_y = load_float(memory, m + 0x10u) * ox + load_float(memory, m + 0x14u) * oy +
                   load_float(memory, m + 0x18u) * oz + load_float(memory, m + 0x34u);
        target_z = load_float(memory, m + 0x20u) * ox + load_float(memory, m + 0x24u) * oy +
                   load_float(memory, m + 0x28u) * oz + load_float(memory, m + 0x38u);
        break;
    }
    case 2u: // PLAYER_WORLD_OFFSET
        if (!range_ok(memory, area, kAreaPanWorldOffset, 12u)) return false;
        target_x = player_x + load_float(memory, area + kAreaPanWorldOffset);
        target_y = player_y + load_float(memory, area + kAreaPanWorldOffset + 4u);
        target_z = player_z + load_float(memory, area + kAreaPanWorldOffset + 8u);
        break;
    default:
        return false;
    }

    // CameraAreaAxes::Y keeps only the configured world Y; XZ keeps configured X/Z.
    const std::uint8_t axes = memory.load8(area + 0x03u);
    if (axes == 0u) {
        target_y = load_float(memory, area + kAreaPanTargetPosition + 4u);
    } else if (axes == 1u) {
        target_x = load_float(memory, area + kAreaPanTargetPosition);
        target_z = load_float(memory, area + kAreaPanTargetPosition + 8u);
    }

    return std::isfinite(target_x) && std::isfinite(target_y) && std::isfinite(target_z);
}

void restore_offset_pan_profile(psprecomp::GuestMemory &memory) {
    if (!g_orbit.active || g_orbit.mode != kOrbitModeOffsetPan ||
        !range_ok(memory, g_orbit.area, kAreaMoveType, 1u) ||
        !range_ok(memory, g_orbit.area, kAreaPanCameraPosition, 12u)) {
        return;
    }
    // Avoid writing to an area record that has been recycled for another camera type/area.
    if (memory.load8(g_orbit.area + kAreaMoveType) != 3u ||
        memory.load8(g_orbit.area + 1u) != g_orbit.area_id) {
        return;
    }
    store_float(memory, g_orbit.area + kAreaPanCameraPosition, g_orbit.base_pan_x);
    store_float(memory, g_orbit.area + kAreaPanCameraPosition + 4u, g_orbit.base_pan_y);
    store_float(memory, g_orbit.area + kAreaPanCameraPosition + 8u, g_orbit.base_pan_z);
}

bool capture_offset_pan_orbit(OrbitState &state, const psprecomp::GuestMemory &memory,
                              std::uint32_t stage_sub_camera, std::uint32_t camera,
                              std::uint32_t area, std::uint32_t player) {
    if (!range_ok(memory, player, kPlayerPositionX, 12u) ||
        !range_ok(memory, area, kAreaPanCameraPosition, 12u) ||
        !range_ok(memory, area, 0u, 0x58u)) return false;

    const float player_x = load_float(memory, player + kPlayerPositionX);
    const float player_y = load_float(memory, player + kPlayerPositionX + 4u);
    const float player_z = load_float(memory, player + kPlayerPositionX + 8u);
    const float base_x = load_float(memory, area + kAreaPanCameraPosition);
    const float base_y = load_float(memory, area + kAreaPanCameraPosition + 4u);
    const float base_z = load_float(memory, area + kAreaPanCameraPosition + 8u);

    float target_x{}, target_y{}, target_z{};
    if (!std::isfinite(player_x) || !std::isfinite(player_y) || !std::isfinite(player_z) ||
        !std::isfinite(base_x) || !std::isfinite(base_y) || !std::isfinite(base_z) ||
        !read_offset_pan_target(memory, area, player, player_x, player_y, player_z,
                                target_x, target_y, target_z)) return false;

    const float dx = target_x - (player_x + base_x);
    const float dy = target_y - (player_y + base_y);
    const float dz = target_z - (player_z + base_z);
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float horizontal = std::hypot(dx, dz);
    if (!std::isfinite(distance) || !std::isfinite(horizontal) ||
        distance < kMinOrbitDistance || distance > kMaxOrbitDistance ||
        horizontal < 1.0f) return false;

    state = {};
    state.active = true;
    state.mode = kOrbitModeOffsetPan;
    state.area_id = memory.load8(area + 1u);
    state.sub_camera = stage_sub_camera;
    state.camera = camera;
    state.area = area;
    state.player = player;
    state.distance = distance;
    state.yaw = wrap_angle(std::atan2(dx, dz));
    state.elevation = std::clamp(std::atan2(-dy, horizontal), kMinElevation, kMaxElevation);
    state.initial_elevation = state.elevation;
    state.base_pan_x = base_x;
    state.base_pan_y = base_y;
    state.base_pan_z = base_z;
    return true;
}

void replace_offset_pan_camera(Runtime &runtime, Context &context, GuestFunction original,
                               std::uint32_t stg_sub_camera, std::uint32_t camera,
                               std::uint32_t area, std::uint32_t player) {
    auto &memory = runtime.memory();
    const std::uint32_t stg_data = stg_sub_camera + kSubCameraData;

    if (!range_ok(memory, stg_sub_camera, 0u, 0xA0u) ||
        !range_ok(memory, stg_sub_camera, kSubCameraIsActive, 1u) ||
        !range_ok(memory, stg_sub_camera, 0x98u, 1u) ||
        !range_ok(memory, stg_sub_camera, kSubCameraState, 1u) ||
        !range_ok(memory, stg_data, 0u, 0x5Cu) ||
        !range_ok(memory, camera, kCameraIsChangingArea, 1u) ||
        !range_ok(memory, camera, kCameraWyvernFindPlayer, 1u) ||
        !range_ok(memory, camera, 0xDC4u, 1u) ||
        !range_ok(memory, area, kAreaPanCameraPosition, 12u) ||
        !range_ok(memory, player, kPlayerPositionX, 12u) ||
        !range_ok(memory, player, kPlayerRotationY, 2u)) {
        fallback(runtime, context, original, "fallback: OFFSET_PAN camera/player structure range validation failed");
        return;
    }

    if (memory.load8(camera + kCameraIsChangingArea) != 0u ||
        memory.load8(camera + kCameraWyvernFindPlayer) != 0u) {
        fallback(runtime, context, original, "fallback: OFFSET_PAN area transition or scripted camera state");
        return;
    }

    const std::uint8_t active_camera_id = memory.load8(camera + 0xDC4u);
    const int active_special_id = active_special_sub_camera(memory, camera);
    if (active_special_id == -2) {
        fallback(runtime, context, original,
                 "fallback: special subcamera flag outside guest memory during OFFSET_PAN");
        return;
    }
    if (active_special_id >= 0 ||
        active_camera_id == 3u || active_camera_id == 4u || active_camera_id == 5u) {
        // Camera::cam2view gives PLAYER_EX/PCHNGR/DEMO priority over the area
        // camera. Let the game's special camera run unchanged, but do not
        // destroy our saved orbit: when the special view ends, resume it.
        const bool same_orbit = g_orbit.active && g_orbit.mode == kOrbitModeOffsetPan &&
            g_orbit.camera == camera && g_orbit.area == area && g_orbit.player == player &&
            g_orbit.area_id == memory.load8(area + 1u) && g_orbit.sub_camera == stg_sub_camera;
        if (g_orbit.active && g_orbit.mode == kOrbitModeOffsetPan) {
            if (same_orbit) {
                // The special camera owns the rendered view. Restore the area's
                // native pan profile while it runs; the saved yaw/elevation stay
                // host-side and are reapplied as soon as this view releases it.
                restore_offset_pan_profile(memory);
            } else {
                restore_offset_pan_profile(memory);
                g_orbit = {};
            }
        }
        char reason[176]{};
        std::snprintf(reason, sizeof(reason),
                      "defer: special subcamera %d selected=%u; preserving OFFSET_PAN orbit=%u",
                      active_special_id, static_cast<unsigned>(active_camera_id),
                      same_orbit ? 1u : 0u);
        camera_trace(reason);
        runtime.note_analog_camera_update(0u);
        if (original != nullptr) original(runtime, context);
        return;
    }

    // The STG function is called after STD in Camera::CameraMove. On the first
    // OFFSET_PAN frame its mode is still zero; let the native function initialize
    // its position/target, then take over on the next frame.
    const std::uint8_t stg_mode = memory.load8(stg_sub_camera + 0x98u);
    const std::uint8_t stg_active = memory.load8(stg_sub_camera + kSubCameraIsActive);
    const std::uint8_t stg_state = memory.load8(stg_sub_camera + kSubCameraState);
    if (stg_mode == 0u) {
        if (g_orbit.active && g_orbit.mode == kOrbitModeOffsetPan &&
            (g_orbit.camera != camera || g_orbit.area != area || g_orbit.player != player)) {
            restore_offset_pan_profile(memory);
            g_orbit = {};
            runtime.note_analog_camera_update(0u);
        }
        camera_trace("OFFSET_PAN native initialization frame; takeover on next STG update");
        if (original != nullptr) original(runtime, context);
        return;
    }
    if (stg_mode != 1u || stg_active == 0u || stg_state != 0u) {
        char reason[160]{};
        std::snprintf(reason, sizeof(reason),
                      "fallback: OFFSET_PAN STG state active=%u mode=%u state=%u",
                      static_cast<unsigned>(stg_active), static_cast<unsigned>(stg_mode),
                      static_cast<unsigned>(stg_state));
        fallback(runtime, context, original, reason);
        return;
    }

    if (special_player_action(memory, player)) {
        fallback(runtime, context, original, "fallback: player/action state is marked special during OFFSET_PAN");
        return;
    }

    if (!g_orbit.active || g_orbit.mode != kOrbitModeOffsetPan ||
        g_orbit.camera != camera || g_orbit.area != area || g_orbit.player != player ||
        g_orbit.area_id != memory.load8(area + 1u) || g_orbit.sub_camera != stg_sub_camera) {
        restore_offset_pan_profile(memory);
        g_orbit = {};
        if (!capture_offset_pan_orbit(g_orbit, memory, stg_sub_camera, camera, area, player)) {
            fallback(runtime, context, original, "fallback: could not capture initial OFFSET_PAN orbit");
            return;
        }
    }

    const float player_x = load_float(memory, player + kPlayerPositionX);
    const float player_y = load_float(memory, player + kPlayerPositionX + 4u);
    const float player_z = load_float(memory, player + kPlayerPositionX + 8u);
    float target_x{}, target_y{}, target_z{};
    if (!std::isfinite(player_x) || !std::isfinite(player_y) || !std::isfinite(player_z) ||
        !read_offset_pan_target(memory, area, player, player_x, player_y, player_z,
                                target_x, target_y, target_z)) {
        fallback(runtime, context, original, "fallback: OFFSET_PAN target transform could not be read");
        return;
    }

    const std::uint64_t now = runtime.now_us();
    float dt = 1.0f / 60.0f;
    if (g_orbit.last_update_us != 0u && now >= g_orbit.last_update_us) {
        dt = std::clamp(static_cast<float>(now - g_orbit.last_update_us) / 1'000'000.0f, 0.0f, 0.05f);
    } else if (g_orbit.last_update_us != 0u && now < g_orbit.last_update_us) {
        g_orbit.last_update_us = 0u;
    }

    const ControllerState &pad = controller_state(runtime);
    const float dx = shape_axis(pad.rx);
    const float dy = shape_axis(pad.ry);
    if ((pad.buttons & buttons::kLTrigger) != 0u) {
        const auto player_rotation = static_cast<std::int16_t>(memory.load16(player + kPlayerRotationY));
        const std::int32_t recentered = static_cast<std::int32_t>(player_rotation) + 0x8000;
        g_orbit.yaw = wrap_angle(static_cast<float>(recentered) / kAngleUnitsPerRadian);
        g_orbit.elevation = g_orbit.initial_elevation;
    } else {
        g_orbit.yaw = wrap_angle(g_orbit.yaw - dx * kYawSpeed * dt);
        g_orbit.elevation = std::clamp(g_orbit.elevation + dy * kPitchSpeed * dt,
                                       kMinElevation, kMaxElevation);
    }

    const float cos_elevation = std::cos(g_orbit.elevation);
    const float camera_x = target_x - std::sin(g_orbit.yaw) * cos_elevation * g_orbit.distance;
    const float camera_y = target_y + std::sin(g_orbit.elevation) * g_orbit.distance;
    const float camera_z = target_z - std::cos(g_orbit.yaw) * cos_elevation * g_orbit.distance;
    const float pan_x = camera_x - player_x;
    const float pan_y = camera_y - player_y;
    const float pan_z = camera_z - player_z;

    if (!std::isfinite(pan_x) || !std::isfinite(pan_y) || !std::isfinite(pan_z) ||
        std::fabs(pan_x) > 5000.0f || std::fabs(pan_y) > 5000.0f || std::fabs(pan_z) > 5000.0f) {
        fallback(runtime, context, original, "fallback: computed OFFSET_PAN camera position is invalid");
        return;
    }

    // Feed the desired orbit into the area profile and its native STG angles
    // immediately before the original cam_sub_stg() consumes them.
    store_float(memory, area + kAreaPanCameraPosition, pan_x);
    store_float(memory, area + kAreaPanCameraPosition + 4u, pan_y);
    store_float(memory, area + kAreaPanCameraPosition + 8u, pan_z);
    memory.store16(stg_data + 0x50u, 0u); // pitch_adj
    memory.store16(stg_data + 0x52u, 0u); // rotation_adj
    memory.store16(stg_data + 0x58u, angle_to_units(g_orbit.elevation)); // pitch
    memory.store16(stg_data + 0x5Au, angle_to_units(g_orbit.yaw)); // rotation

    g_orbit.last_update_us = now;
    if (camera_trace_enabled()) {
        static std::uint64_t last_input_trace_us = 0u;
        if (last_input_trace_us == 0u || now < last_input_trace_us ||
            now - last_input_trace_us >= 500'000u) {
            std::fprintf(stderr,
                "[mhfu-camera] OFFSET_PAN input area=0x%08X sub=(active=%u mode=%u state=%u) "
                "rx=%+.3f ry=%+.3f shaped=(%+.3f,%+.3f) yaw=%.3f elevation=%.3f "
                "player=(%.1f,%.1f,%.1f) target=(%.1f,%.1f,%.1f) pan=(%.1f,%.1f,%.1f)\n",
                static_cast<unsigned>(area),
                static_cast<unsigned>(memory.load8(stg_sub_camera + kSubCameraIsActive)),
                static_cast<unsigned>(memory.load8(stg_sub_camera + 0x98u)),
                static_cast<unsigned>(memory.load8(stg_sub_camera + kSubCameraState)),
                static_cast<double>(pad.rx), static_cast<double>(pad.ry),
                static_cast<double>(dx), static_cast<double>(dy),
                static_cast<double>(g_orbit.yaw), static_cast<double>(g_orbit.elevation),
                static_cast<double>(player_x), static_cast<double>(player_y), static_cast<double>(player_z),
                static_cast<double>(target_x), static_cast<double>(target_y), static_cast<double>(target_z),
                static_cast<double>(pan_x), static_cast<double>(pan_y), static_cast<double>(pan_z));
            last_input_trace_us = now;
        }
    }
    camera_trace("orbit active: OFFSET_PAN right-stick camera path accepted");
    runtime.note_analog_camera_update(now);

    // The original STG update consumes the adjusted OFFSET_PAN position/angles now.
    if (original != nullptr) original(runtime, context);
}

void fallback(Runtime &runtime, Context &context, GuestFunction original, const char *reason) {
    camera_trace(reason);
    restore_offset_pan_profile(runtime.memory());
    g_orbit = {};
    runtime.note_analog_camera_update(0u);
    if (original != nullptr) original(runtime, context);
}

void replace_standard_view_from_stage(Runtime &runtime, std::uint32_t camera) {
    auto &memory = runtime.memory();
    const std::uint32_t sub_camera = camera + kCameraFromSubCamera;
    const std::uint32_t std_data = sub_camera + kSubCameraData;

    if (!range_ok(memory, camera, kCameraAreaSubCameraId, 1u) ||
        memory.load8(camera + kCameraAreaSubCameraId) != 0u ||
        !range_ok(memory, camera, kCameraAreaConfig, 4u) ||
        !range_ok(memory, camera, kCameraPlayer, 4u) ||
        !range_ok(memory, camera, 0xDC4u, 1u) ||
        !range_ok(memory, sub_camera, 0u, 0x20u) ||
        !range_ok(memory, sub_camera, kSubCameraIsActive, 1u) ||
        !range_ok(memory, sub_camera, kSubCameraCurrentId, 1u) ||
        !range_ok(memory, sub_camera, kSubCameraState, 1u) ||
        !range_ok(memory, std_data, 0u, 0x90u)) {
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-after-STG rejected: camera or subcamera fields are unreadable");
        return;
    }

    const int active_special_id = active_special_sub_camera(memory, camera);
    const std::uint8_t selected_camera_id = memory.load8(camera + 0xDC4u);
    if (active_special_id >= 0 || selected_camera_id == 3u ||
        selected_camera_id == 4u || selected_camera_id == 5u) {
        // The game owns the rendered view during zoom/fishing, scope and demos.
        // Keep the saved hunt orbit so it can resume when that temporary view ends.
        runtime.note_analog_camera_update(0u);
        return;
    }

    const std::uint32_t area = memory.load32(camera + kCameraAreaConfig);
    const std::uint32_t player = memory.load32(camera + kCameraPlayer);
    if (area == 0u || player == 0u ||
        !range_ok(memory, player, kPlayerPositionX, 12u)) {
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-after-STG rejected: area or player pointer is invalid");
        return;
    }

    const std::uint8_t std_active = memory.load8(sub_camera + kSubCameraIsActive);
    const std::uint8_t current_id = memory.load8(sub_camera + kSubCameraCurrentId);
    const std::uint8_t state = memory.load8(sub_camera + kSubCameraState);
    if (current_id != 0xFFu || state != 0u) {
        runtime.note_analog_camera_update(0u);
        char reason[160]{};
        std::snprintf(reason, sizeof(reason),
                      "STD-after-STG waiting: active=%u current_id=0x%02X state=%u",
                      static_cast<unsigned>(std_active), static_cast<unsigned>(current_id),
                      static_cast<unsigned>(state));
        camera_trace(reason);
        return;
    }

    if (special_player_action(memory, player)) {
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-after-STG paused: player is in a protected action");
        return;
    }

    const float position_x = load_float(memory, sub_camera + 0x00u);
    const float position_y = load_float(memory, sub_camera + 0x04u);
    const float position_z = load_float(memory, sub_camera + 0x08u);
    const float target_x = load_float(memory, sub_camera + 0x10u);
    const float target_y = load_float(memory, sub_camera + 0x14u);
    const float target_z = load_float(memory, sub_camera + 0x18u);
    const float offset_x = position_x - target_x;
    const float offset_y = position_y - target_y;
    const float offset_z = position_z - target_z;
    const float horizontal = std::hypot(offset_x, offset_z);
    const float distance = std::sqrt(horizontal * horizontal + offset_y * offset_y);
    if (!std::isfinite(position_x) || !std::isfinite(position_y) ||
        !std::isfinite(position_z) || !std::isfinite(target_x) ||
        !std::isfinite(target_y) || !std::isfinite(target_z) ||
        !std::isfinite(distance) || distance < kMinOrbitDistance ||
        distance > kMaxOrbitDistance || horizontal < 1.0f) {
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-after-STG rejected: native camera position/target is invalid");
        return;
    }

    if (!g_orbit.active || g_orbit.mode != kOrbitModeStandard ||
        g_orbit.camera != camera || g_orbit.sub_camera != sub_camera ||
        g_orbit.area != area || g_orbit.player != player) {
        g_orbit = {};
        g_orbit.active = true;
        g_orbit.mode = kOrbitModeStandard;
        g_orbit.sub_camera = sub_camera;
        g_orbit.camera = camera;
        g_orbit.area = area;
        g_orbit.player = player;
        g_orbit.distance = distance;
        g_orbit.yaw = wrap_angle(std::atan2(offset_x, offset_z));
        g_orbit.elevation = std::clamp(std::atan2(offset_y, horizontal),
                                       kMinElevation, kMaxElevation);
        g_orbit.initial_elevation = g_orbit.elevation;
    }

    const std::uint64_t now = runtime.now_us();
    float dt = 1.0f / 60.0f;
    if (g_orbit.last_update_us != 0u && now >= g_orbit.last_update_us) {
        dt = std::clamp(static_cast<float>(now - g_orbit.last_update_us) /
                        1'000'000.0f, 0.0f, 0.05f);
    } else if (g_orbit.last_update_us != 0u && now < g_orbit.last_update_us) {
        g_orbit.last_update_us = 0u;
    }

    const ControllerState &pad = controller_state(runtime);
    const float dx = shape_axis(pad.rx);
    const float dy = shape_axis(pad.ry);
    if ((pad.buttons & buttons::kLTrigger) != 0u) {
        const auto player_rotation =
            static_cast<std::int16_t>(memory.load16(player + kPlayerRotationY));
        const std::int32_t recentered =
            static_cast<std::int32_t>(player_rotation) + 0x8000;
        g_orbit.yaw = wrap_angle(static_cast<float>(recentered) /
                                 kAngleUnitsPerRadian);
        g_orbit.elevation = g_orbit.initial_elevation;
    } else {
        g_orbit.yaw = wrap_angle(g_orbit.yaw - dx * kYawSpeed * dt);
        g_orbit.elevation = std::clamp(g_orbit.elevation + dy * kPitchSpeed * dt,
                                       kMinElevation, kMaxElevation);
    }
    g_orbit.last_update_us = now;

    // cam_sub_std has already run earlier in Camera::CameraMove and supplied
    // a collision-resolved native view. In tight wall/ceiling/fall states,
    // leave that safe native position alone rather than pushing the camera
    // through geometry. The orbit state and input continue to update.
    const bool collision_constrained =
        memory.load8(std_data + kStdIsFalldown) != 0u ||
        memory.load8(std_data + kStdIsViewBlocked) != 0u ||
        memory.load8(std_data + kStdIsKabegiwa) != 0u ||
        memory.load8(std_data + kStdKabegiwaTimer) != 0u;
    if (!collision_constrained) {
        const float cos_elevation = std::cos(g_orbit.elevation);
        const float camera_x = target_x +
            std::sin(g_orbit.yaw) * cos_elevation * g_orbit.distance;
        const float camera_y = target_y +
            std::sin(g_orbit.elevation) * g_orbit.distance;
        const float camera_z = target_z +
            std::cos(g_orbit.yaw) * cos_elevation * g_orbit.distance;
        if (std::isfinite(camera_x) && std::isfinite(camera_y) &&
            std::isfinite(camera_z)) {
            // Change only SubCamera's rendered position. StdCameraData remains
            // native, so the next frame's collision solver starts from its own
            // state and this post-STD stage hook applies the orbit again.
            store_float(memory, sub_camera + 0x00u, camera_x);
            store_float(memory, sub_camera + 0x04u, camera_y);
            store_float(memory, sub_camera + 0x08u, camera_z);
        }
    }

    runtime.note_analog_camera_update(now);
    if (camera_trace_enabled()) {
        static std::uint64_t last_input_trace_us = 0u;
        if (last_input_trace_us == 0u || now < last_input_trace_us ||
            now - last_input_trace_us >= 500'000u) {
            std::fprintf(stderr,
                "[mhfu-camera] STD-after-STG input rx=%+.3f ry=%+.3f "
                "shaped=(%+.3f,%+.3f) yaw=%.3f elevation=%.3f "
                "collision_constrained=%u target=(%.1f,%.1f,%.1f)\n",
                static_cast<double>(pad.rx), static_cast<double>(pad.ry),
                static_cast<double>(dx), static_cast<double>(dy),
                static_cast<double>(g_orbit.yaw),
                static_cast<double>(g_orbit.elevation),
                static_cast<unsigned>(collision_constrained),
                static_cast<double>(target_x), static_cast<double>(target_y),
                static_cast<double>(target_z));
            last_input_trace_us = now;
        }
    }
    camera_trace("orbit active: STD post-update right-stick camera path accepted");
}

void replace_standard_camera(Runtime &runtime, Context &context, GuestFunction original) {
    // The reliable per-frame camera point is the STG callback, which runs after
    // cam_sub_std. Keep this hook as a transparent native pass-through.
    camera_trace("replacement reached: mh_camera_orbit_update");
    if (original != nullptr) original(runtime, context);
}

} // namespace

void replace_stage_camera(Runtime &runtime, Context &context, GuestFunction original) {
    camera_trace("replacement reached: mh_camera_stage_update");
    auto &memory = runtime.memory();
    const std::uint32_t stage_sub_camera = context.gpr[4]; // SubCamera::cam_sub_stg this
    constexpr std::uint32_t kCameraFromStageSubCamera =
        kCameraFromSubCamera + 2u * kSubCameraStride;
    if (stage_sub_camera < kCameraFromStageSubCamera) {
        fallback(runtime, context, original, "fallback: STG SubCamera pointer is below expected Camera offset");
        return;
    }

    const std::uint32_t camera = stage_sub_camera - kCameraFromStageSubCamera;
    if (camera_trace_enabled()) {
        static std::uint64_t last_callback_trace_us = 0u;
        const std::uint64_t trace_now = runtime.now_us();
        if (last_callback_trace_us == 0u || trace_now < last_callback_trace_us ||
            trace_now - last_callback_trace_us >= 500'000u) {
            const ControllerState &trace_pad = controller_state(runtime);
            const std::uint8_t selected = range_ok(memory, camera, 0xDC4u, 1u)
                ? memory.load8(camera + 0xDC4u) : 0xFFu;
            const std::uint8_t area_sub = range_ok(memory, camera, kCameraAreaSubCameraId, 1u)
                ? memory.load8(camera + kCameraAreaSubCameraId) : 0xFFu;
            const std::uint8_t active3 = range_ok(memory, camera, kCameraFromSubCamera + 3u * kSubCameraStride + kSubCameraIsActive, 1u)
                ? memory.load8(camera + kCameraFromSubCamera + 3u * kSubCameraStride + kSubCameraIsActive) : 0xFFu;
            const std::uint8_t active4 = range_ok(memory, camera, kCameraFromSubCamera + 4u * kSubCameraStride + kSubCameraIsActive, 1u)
                ? memory.load8(camera + kCameraFromSubCamera + 4u * kSubCameraStride + kSubCameraIsActive) : 0xFFu;
            const std::uint8_t active5 = range_ok(memory, camera, kCameraFromSubCamera + 5u * kSubCameraStride + kSubCameraIsActive, 1u)
                ? memory.load8(camera + kCameraFromSubCamera + 5u * kSubCameraStride + kSubCameraIsActive) : 0xFFu;
            std::fprintf(stderr,
                "[mhfu-camera] STG callback camera=0x%08X area_sub=%u selected=%u flags=(%u,%u,%u) rx=%+.3f ry=%+.3f\n",
                static_cast<unsigned>(camera), static_cast<unsigned>(area_sub),
                static_cast<unsigned>(selected), static_cast<unsigned>(active3),
                static_cast<unsigned>(active4), static_cast<unsigned>(active5),
                static_cast<double>(trace_pad.rx), static_cast<double>(trace_pad.ry));
            last_callback_trace_us = trace_now;
        }
    }

    if (!range_ok(memory, camera, kCameraAreaSubCameraId, 1u) ||
        !range_ok(memory, camera, kCameraAreaConfig, 4u) ||
        !range_ok(memory, camera, kCameraPlayer, 4u)) {
        fallback(runtime, context, original, "fallback: STG camera structure range validation failed");
        return;
    }

    const std::uint8_t area_sub_id = memory.load8(camera + kCameraAreaSubCameraId);
    const std::uint32_t area = memory.load32(camera + kCameraAreaConfig);
    const std::uint32_t player = memory.load32(camera + kCameraPlayer);
    const bool is_offset_pan = area_sub_id == 2u &&
        range_ok(memory, area, kAreaMoveType, 1u) &&
        memory.load8(area + kAreaMoveType) == 3u;

    if (!is_offset_pan) {
        if (g_orbit.active && g_orbit.mode == kOrbitModeOffsetPan) {
            restore_offset_pan_profile(memory);
            g_orbit = {};
            runtime.note_analog_camera_update(0u);
        }

        // The game runs cam_sub_std before cam_sub_stg. The STG callback is
        // therefore the dependable post-update point for a normal follow view,
        // even in transitions where the STD replacement is not entered.
        if (original != nullptr) original(runtime, context);
        if (area_sub_id == 0u) {
            replace_standard_view_from_stage(runtime, camera);
        } else if (g_orbit.active && g_orbit.mode == kOrbitModeStandard) {
            const int special_id = active_special_sub_camera(memory, camera);
            if (special_id < 0) {
                g_orbit = {};
                runtime.note_analog_camera_update(0u);
            }
        }
        return;
    }

    replace_offset_pan_camera(runtime, context, original, stage_sub_camera,
                              camera, area, player);
}

void register_mhfu_orbit_camera(Runtime &runtime) {
    runtime.register_replacement("mh_camera_orbit_update", &replace_standard_camera);
    runtime.register_replacement("mh_camera_stage_update", &replace_stage_camera);
}

} // namespace mhf::psp
