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
constexpr std::uint32_t kPlayerPositionX = 0x200u;
constexpr std::uint32_t kPlayerTransform = 0x10u;

constexpr std::uint32_t kAreaMoveType = 0x02u;
constexpr std::uint32_t kAreaHeightDefinition = 0x20u;
constexpr std::uint32_t kAreaPanCameraPosition = 0x20u;
constexpr std::uint32_t kAreaPanTargetPosition = 0x2Cu;
constexpr std::uint32_t kAreaPanModelOffset = 0x38u;
constexpr std::uint32_t kAreaPanWorldOffset = 0x44u;
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


bool special_sub_camera_active(const psprecomp::GuestMemory &memory, std::uint32_t camera) {
    // Camera::cam2view selects DEMO (5), PCHNGR (3), or PLAYER_EX (4).
    constexpr std::uint32_t special_ids[] = {3u, 4u, 5u};
    for (const std::uint32_t id : special_ids) {
        const std::uint32_t active = camera + 0xB0u + id * kSubCameraStride + kSubCameraIsActive;
        if (!memory.contains(active, 1u) || memory.load8(active) != 0u) return true;
    }
    return false;
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

bool special_player_state(const psprecomp::GuestMemory &memory, std::uint32_t player,
                          std::uint32_t std_data) {
    if (special_player_action(memory, player)) return true;

    if (!range_ok(memory, std_data, kStdIsFalldown, 1u) ||
        !range_ok(memory, std_data, kStdIsViewBlocked, 1u) ||
        !range_ok(memory, std_data, kStdGunTargetingState, 1u) ||
        !range_ok(memory, std_data, kStdIsShoulderCam, 1u) ||
        !range_ok(memory, std_data, kStdIsKabegiwa, 1u) ||
        !range_ok(memory, std_data, kStdKabegiwaTimer, 1u) ||
        !range_ok(memory, std_data, kStdIsFastRotate, 1u)) {
        return true;
    }

    return memory.load8(std_data + kStdIsFalldown) != 0u ||
        memory.load8(std_data + kStdIsViewBlocked) != 0u ||
        memory.load8(std_data + kStdIsKabegiwa) != 0u ||
        memory.load8(std_data + kStdKabegiwaTimer) != 0u ||
        static_cast<std::int8_t>(memory.load8(std_data + kStdGunTargetingState)) >= 0 ||
        memory.load8(std_data + kStdIsShoulderCam) != 0u ||
        memory.load8(std_data + kStdIsFastRotate) != 0u;
}

bool standard_camera_context(const psprecomp::GuestMemory &memory, std::uint32_t sub_camera,
                             std::uint32_t camera, std::uint32_t &area_out,
                             std::uint32_t &player_out, char *reject_reason,
                             std::size_t reject_reason_size) {
    auto reject = [&](const char *message) {
        std::snprintf(reject_reason, reject_reason_size, "%s", message);
        return false;
    };
    if (!range_ok(memory, camera, kCameraAreaSubCameraId, 1u) ||
        !range_ok(memory, camera, kCameraAreaConfig, 4u) ||
        !range_ok(memory, camera, kCameraPlayer, 4u) ||
        !range_ok(memory, camera, kCameraIsChangingArea, 1u) ||
        !range_ok(memory, camera, kCameraLastSubCameraId, 1u) ||
        !range_ok(memory, camera, kCameraWyvernFindPlayer, 1u) ||
        !range_ok(memory, camera, kCameraHeightId, 1u) ||
        !range_ok(memory, camera, kCameraButtons, 4u) ||
        !range_ok(memory, camera, 0xDC4u, 1u) ||
        !range_ok(memory, sub_camera, kSubCameraIsActive, 1u) ||
        !range_ok(memory, sub_camera, kSubCameraCurrentId, 1u) ||
        !range_ok(memory, sub_camera, kSubCameraState, 1u)) {
        return reject("camera/subcamera structure range validation failed");
    }

    const std::uint8_t area_sub_id = memory.load8(camera + kCameraAreaSubCameraId);
    const std::uint32_t area_candidate = memory.load32(camera + kCameraAreaConfig);
    const bool area_type_readable = range_ok(memory, area_candidate, kAreaMoveType, 1u);
    const std::uint8_t move_type = area_type_readable
        ? memory.load8(area_candidate + kAreaMoveType) : 0xFFu;
    if (area_sub_id != 0u) {
        std::snprintf(reject_reason, reject_reason_size,
                      "area_sub_camera_id=%u (STD is 0), move_type=%s%u area=0x%08X",
                      static_cast<unsigned>(area_sub_id), area_type_readable ? "" : "unreadable/",
                      static_cast<unsigned>(move_type), static_cast<unsigned>(area_candidate));
        return false;
    }

    const std::uint8_t changing_area = memory.load8(camera + kCameraIsChangingArea);
    if (changing_area != 0u) {
        std::snprintf(reject_reason, reject_reason_size,
                      "camera is changing area (is_changing_area=%u)", static_cast<unsigned>(changing_area));
        return false;
    }
    const std::uint8_t wyvern_find_player = memory.load8(camera + kCameraWyvernFindPlayer);
    if (wyvern_find_player != 0u) {
        std::snprintf(reject_reason, reject_reason_size,
                      "wyvern-find-player camera mode is active (flag=%u)", static_cast<unsigned>(wyvern_find_player));
        return false;
    }

    const std::uint8_t last_sub_camera = memory.load8(camera + kCameraLastSubCameraId);
    if (last_sub_camera != 0u) {
        std::snprintf(reject_reason, reject_reason_size,
                      "last_sub_camera_id=%u (expected STD=0)", static_cast<unsigned>(last_sub_camera));
        return false;
    }

    const std::uint8_t std_active = memory.load8(sub_camera + kSubCameraIsActive);
    if (std_active == 0u) {
        return reject("STD subcamera is not active (normal follow camera inactive; lobby/special context)");
    }
    const std::uint8_t current_id = memory.load8(sub_camera + kSubCameraCurrentId);
    const std::uint8_t state = memory.load8(sub_camera + kSubCameraState);
    if (current_id != 0xFFu || state != 0u) {
        std::snprintf(reject_reason, reject_reason_size,
                      "STD subcamera state mismatch (active=%u current_id=0x%02X state=%u)",
                      static_cast<unsigned>(std_active), static_cast<unsigned>(current_id),
                      static_cast<unsigned>(state));
        return false;
    }

    for (const std::uint32_t id : {3u, 4u, 5u}) {
        const std::uint32_t active = camera + kCameraFromSubCamera + id * kSubCameraStride + kSubCameraIsActive;
        if (!memory.contains(active, 1u)) {
            return reject("special subcamera active flag outside guest memory");
        }
        const std::uint8_t active_value = memory.load8(active);
        if (active_value != 0u) {
            std::snprintf(reject_reason, reject_reason_size,
                          "special subcamera %u is active (isActive=%u)", static_cast<unsigned>(id), static_cast<unsigned>(active_value));
            return false;
        }
    }

    const std::uint8_t active_sub_camera = memory.load8(camera + 0xDC4u);
    if (active_sub_camera == 3u || active_sub_camera == 4u || active_sub_camera == 5u) {
        std::snprintf(reject_reason, reject_reason_size,
                      "active subcamera id=%u is a special mode", static_cast<unsigned>(active_sub_camera));
        return false;
    }

    area_out = memory.load32(camera + kCameraAreaConfig);
    player_out = memory.load32(camera + kCameraPlayer);
    if (area_out == 0u || player_out == 0u) {
        return reject("area or player pointer is null");
    }
    if (!range_ok(memory, area_out, kAreaHeightDefinition + kFollowEntriesOffset +
                                    kFollowEntryCount * kFollowEntryStride, 1u)) {
        std::snprintf(reject_reason, reject_reason_size,
                      "area follow-profile range invalid (area=0x%08X)", area_out);
        return false;
    }

    if (move_type != 0u) {
        std::snprintf(reject_reason, reject_reason_size,
                      "area move_type=%u is not STD follow (area=0x%08X)", static_cast<unsigned>(move_type), static_cast<unsigned>(area_out));
        return false;
    }

    // The original SubCamera::cam_sub_std sets isActive only after its
    // GameSys lobby early-return. That state is local to the camera object and
    // avoids relying on a region-specific GameSys singleton address.
    reject_reason[0] = '\0';
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
    state.mode = kOrbitModeStandard;
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
                              std::uint32_t sub_camera, std::uint32_t camera,
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
    state.sub_camera = sub_camera + 2u * kSubCameraStride;
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
                               std::uint32_t std_sub_camera, std::uint32_t camera,
                               std::uint32_t area, std::uint32_t player) {
    auto &memory = runtime.memory();
    const std::uint32_t stg_sub_camera = camera + kCameraFromSubCamera + 2u * kSubCameraStride;
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

    if (special_sub_camera_active(memory, camera)) {
        fallback(runtime, context, original, "fallback: special subcamera active during OFFSET_PAN");
        return;
    }
    const std::uint8_t active_camera_id = memory.load8(camera + 0xDC4u);
    if (active_camera_id == 3u || active_camera_id == 4u || active_camera_id == 5u) {
        fallback(runtime, context, original, "fallback: selected special subcamera during OFFSET_PAN");
        return;
    }

    // SubCamera::cam_sub_stg initializes its mode on the first visit to an area.
    // Let that one native frame establish the state; takeover begins from mode 1.
    if (memory.load8(stg_sub_camera + kSubCameraIsActive) == 0u ||
        memory.load8(stg_sub_camera + 0x98u) != 1u ||
        memory.load8(stg_sub_camera + kSubCameraState) != 0u) {
        fallback(runtime, context, original, "fallback: OFFSET_PAN subcamera is initializing or transitioning");
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
        if (!capture_offset_pan_orbit(g_orbit, memory, std_sub_camera, camera, area, player)) {
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
    // before cam_sub_stg() executes later in Camera::CameraMove().
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
                "player=(%.1f,%.1f,%.1f) target=(%.1f,%.1f,%.1f) pan=(%.1f,%.1f,%.1f)\\n",
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

    // The original STD camera update returns immediately because this area is STG;
    // the engine then advances its STG camera using the profile we just supplied.
    if (original != nullptr) original(runtime, context);
}

void fallback(Runtime &runtime, Context &context, GuestFunction original, const char *reason) {
    camera_trace(reason);
    restore_offset_pan_profile(runtime.memory());
    g_orbit = {};
    runtime.note_analog_camera_update(0u);
    if (original != nullptr) original(runtime, context);
}

void replace_standard_camera(Runtime &runtime, Context &context, GuestFunction original) {
    camera_trace("replacement reached: mh_camera_orbit_update");
    auto &memory = runtime.memory();
    const std::uint32_t sub_camera = context.gpr[4]; // SubCamera::this
    if (sub_camera < kCameraFromSubCamera) {
        fallback(runtime, context, original, "fallback: SubCamera pointer is below expected Camera offset");
        return;
    }

    const std::uint32_t camera = sub_camera - kCameraFromSubCamera;
    const std::uint32_t std_data = sub_camera + kSubCameraData;
    if (!range_ok(memory, std_data, kStdGoalRotation, 4u) ||
        !range_ok(memory, sub_camera, 0u, 0xA0u)) {
        fallback(runtime, context, original, "fallback: SubCamera/StdCameraData pointer is outside guest memory");
        return;
    }

    const std::uint32_t current_area = range_ok(memory, camera, kCameraAreaConfig, 4u)
        ? memory.load32(camera + kCameraAreaConfig) : 0u;
    const std::uint32_t current_player = range_ok(memory, camera, kCameraPlayer, 4u)
        ? memory.load32(camera + kCameraPlayer) : 0u;
    const std::uint8_t area_sub_id = range_ok(memory, camera, kCameraAreaSubCameraId, 1u)
        ? memory.load8(camera + kCameraAreaSubCameraId) : 0xFFu;
    const std::uint8_t move_type = range_ok(memory, current_area, kAreaMoveType, 1u)
        ? memory.load8(current_area + kAreaMoveType) : 0xFFu;

    // Leaving an OFFSET_PAN area must restore its immutable map profile before
    // the standard/rail/fixed camera path takes over.
    const bool same_offset_pan = area_sub_id == 2u && move_type == 3u &&
        current_area == g_orbit.area && current_player == g_orbit.player &&
        g_orbit.mode == kOrbitModeOffsetPan;
    if (g_orbit.active && g_orbit.mode == kOrbitModeOffsetPan && !same_offset_pan) {
        restore_offset_pan_profile(memory);
        g_orbit = {};
        runtime.note_analog_camera_update(0u);
    }

    if (area_sub_id == 2u && move_type == 3u) {
        replace_offset_pan_camera(runtime, context, original, sub_camera, camera,
                                  current_area, current_player);
        return;
    }

    std::uint32_t area = 0u;
    std::uint32_t player = 0u;
    char context_rejection[192]{};
    if (!standard_camera_context(memory, sub_camera, camera, area, player,
                                 context_rejection, sizeof(context_rejection))) {
        char message[224]{};
        std::snprintf(message, sizeof(message), "fallback: standard context rejected: %s",
                      context_rejection[0] != '\0' ? context_rejection : "unspecified");
        fallback(runtime, context, original, message);
        return;
    }
    if (special_player_state(memory, player, std_data)) {
        fallback(runtime, context, original, "fallback: player/action state is marked special");
        return;
    }

    // A wall/ceiling transition still uses the native camera resolver. Aiming,
    // climbing, falls, rides, scripted areas and cutscenes took the fallback above.
    if (!range_ok(memory, area, 0u, 0xE0u) ||
        !range_ok(memory, player, kPlayerRotationY, 2u) ||
        !range_ok(memory, area, 0x40u, 0x20u)) {
        fallback(runtime, context, original, "fallback: area/player structure range validation failed");
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
            fallback(runtime, context, original, "fallback: could not capture initial orbit profile (height/distance/rotation)");
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

    camera_trace("orbit active: analog right-stick camera path accepted");
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
    if (camera_trace_enabled()) {
        static std::uint64_t last_input_trace_us = 0u;
        if (last_input_trace_us == 0u || now < last_input_trace_us ||
            now - last_input_trace_us >= 500'000u) {
            std::fprintf(stderr,
                "[mhfu-camera] STD input area_sub=%u move_type=%u rx=%+.3f ry=%+.3f "
                "shaped=(%+.3f,%+.3f) yaw=%.3f elevation=%.3f\\n",
                static_cast<unsigned>(area_sub_id), static_cast<unsigned>(move_type),
                static_cast<double>(pad.rx), static_cast<double>(pad.ry),
                static_cast<double>(dx), static_cast<double>(dy),
                static_cast<double>(g_orbit.yaw), static_cast<double>(g_orbit.elevation));
            last_input_trace_us = now;
        }
    }

    // The native camera routine is retained as the wall/ground/safety solver.
    // D-pad stage changes are masked for this call. We temporarily rewrite only
    // the currently selected follow entry with our orbit elevation, then restore
    // that profile and every input byte after native collision handling.
    const std::uint32_t orbit_entry = g_orbit.follow_entry;
    if (!memory.contains(orbit_entry, kFollowEntryStride) ||
        !range_ok(memory, camera, kCameraHeightId, 1u) ||
        !range_ok(memory, std_data, kStdButtons, 4u)) {
        fallback(runtime, context, original, "fallback: selected orbit entry or camera input fields are outside guest memory");
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
        fallback(runtime, context, original, "fallback: computed orbit offset is not finite");
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
