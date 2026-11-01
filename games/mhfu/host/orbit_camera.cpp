#include "orbit_camera.hpp"

#include "mhf/psp/host.hpp"

#if __has_include("mhfu_camera_support.hpp")
#include "mhfu_camera_support.hpp"
#else
#define MHF_MHFU_GAME_SYS_SINGLETON 0u
#define MHF_MHFU_QUEST_SINGLETON 0u
#define MHF_MHFU_GAME_TASK_SINGLETON 0u
#define MHF_MHFU_TASK_MANAGER_SINGLETON 0u
#define MHF_MHFU_SYSTEM_SINGLETON 0u
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
    static std::array<std::array<char, 512>, 16> seen{};
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
constexpr std::uint32_t kCameraAreaSubCameraId = 0xA8Du;
constexpr std::uint32_t kCameraPlayer = 0xA84u;
constexpr std::uint32_t kCameraAreaConfig = 0xA90u;

constexpr std::uint32_t kGameSysFlags = 0x6AF14u;
constexpr std::uint32_t kGameSysFlagLobbyTask = 1u << 0u;
constexpr std::uint32_t kGameSysFlagGameTask = 1u << 9u;

// MHFU swaps task overlays into this fixed slot. The active overlay header
// identifies game_task.ovl versus lobby_task.ovl without relying on P2G
// singleton addresses that are not populated reliably in the US MHFU runtime.
constexpr std::uint32_t kTaskOverlayBase = 0x09A5F300u;
constexpr std::uint32_t kOverlayHeaderMagic = 0x336F574Du; // "MWo3"
constexpr std::uint32_t kOverlayNameOffset = 0x20u;
constexpr std::size_t kOverlayNameReadSize = 32u;
constexpr std::uint32_t kQuestMissionFlagMask =
    (1u << 0u) | (1u << 1u) | (1u << 2u) | (1u << 3u) | (1u << 4u) |
    (1u << 7u) | (1u << 11u) | (1u << 14u) | (1u << 18u) | (1u << 19u) |
    (1u << 21u) | (1u << 22u) | (1u << 23u) | (1u << 24u) | (1u << 25u) |
    (1u << 26u) | (1u << 29u) | (1u << 30u);

constexpr std::uint32_t kStdIsFalldown = 0x35u;
constexpr std::uint32_t kStdGoalPosition = 0x40u;
constexpr std::uint32_t kStdIsGroundAdjusted = 0x90u;
constexpr std::uint32_t kStdIsViewBlocked = 0x89u;
constexpr std::uint32_t kStdKabegiwaTimer = 0x8Bu;
constexpr std::uint32_t kStdIsKabegiwa = 0x8Cu;
constexpr std::uint32_t kStdFollowHeightDefinition = 0x6Cu;
constexpr std::uint32_t kStdFollowHeightEntry = 0x70u;
constexpr std::uint32_t kStdGoalRotation = 0x80u;
constexpr std::uint32_t kStdRotation = 0x82u;
constexpr std::uint32_t kStdIsFastRotate = 0x8Eu;
constexpr std::uint32_t kFollowHeightEntriesOffset = 0x20u;
constexpr std::uint32_t kFollowHeightEntrySize = 0x20u;
constexpr std::uint32_t kFollowHeightEntryCount = 7u;
constexpr std::uint32_t kFollowHeightYOffset = 0x04u;
constexpr std::uint32_t kFollowHeightZOffset = 0x08u;
constexpr std::uint32_t kFollowHeightHeight = 0x10u;

// Used only to validate the player structure; R1 no longer drives camera recentering.
constexpr std::uint32_t kPlayerRotationY = 0x1F4u;

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

constexpr std::uint32_t kPlFlagSupplyBoxOpen = 1u << 1u;
constexpr std::uint32_t kPlFlagDeliveryBoxOpen = 1u << 9u;
constexpr std::uint32_t kPlFlagScopeCam = 1u << 12u;
constexpr std::uint32_t kPlFlagShoulderCam = 1u << 15u;
constexpr std::uint32_t kPlFlagFishing = 1u << 19u;

// A radial deadzone keeps diagonal input continuous instead of zeroing
// one axis independently when the other axis is pushed farther.
constexpr float kDeadzone = 0.08f;
constexpr float kYawSpeed = 4.8f;
constexpr float kPitchSpeed = 3.6f;
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
    std::uint32_t follow_height_entry{};
    float base_y_offset{};
    float base_z_offset{};
    float base_height{};
};

OrbitState g_orbit;

void fallback(Runtime &runtime, Context &context, GuestFunction original, const char *reason);

bool range_ok(const psprecomp::GuestMemory &memory, std::uint32_t base,
              std::uint32_t offset, std::size_t length) {
    if (base == 0u || offset > UINT32_MAX - base) return false;
    return memory.contains(base + offset, length);
}

bool is_hunt_camera_context(const psprecomp::GuestMemory &memory, std::uint32_t camera) {
    // MHFU-specific task detection. The task overlay header at 0x09A5F300 is
    // swapped between lobby_task.ovl and game_task.ovl as the game changes
    // contexts. Read its MWo3 header and name directly.
    std::uint32_t overlay_magic = 0u;
    char overlay_name[kOverlayNameReadSize]{};
    const bool overlay_header_ok =
        memory.contains(kTaskOverlayBase, kOverlayNameOffset + kOverlayNameReadSize);
    if (overlay_header_ok) {
        overlay_magic = memory.load32(kTaskOverlayBase);
        for (std::size_t i = 0; i + 1u < sizeof(overlay_name); ++i) {
            const std::uint8_t ch = memory.load8(
                kTaskOverlayBase + kOverlayNameOffset + static_cast<std::uint32_t>(i));
            if (ch == 0u) break;
            overlay_name[i] = (ch >= 0x20u && ch <= 0x7Eu)
                ? static_cast<char>(ch) : '?';
        }
    }
    constexpr char kGameTaskName[] = "game_task";
    const bool name_is_game_task =
        std::strncmp(overlay_name, kGameTaskName, sizeof(kGameTaskName) - 1u) == 0 &&
        (overlay_name[sizeof(kGameTaskName) - 1u] == '\0' ||
         overlay_name[sizeof(kGameTaskName) - 1u] == '.');
    const bool game_task_overlay =
        overlay_header_ok && overlay_magic == kOverlayHeaderMagic && name_is_game_task;

    if (MHF_MHFU_GAME_SYS_SINGLETON == 0u ||
        !memory.contains(MHF_MHFU_GAME_SYS_SINGLETON, 4u)) {
        char reason[192]{};
        std::snprintf(reason, sizeof(reason),
                      "task overlay name='%s' magic=%08X valid=%u game_task=%u; "
                      "GameSys singleton slot unavailable",
                      overlay_name, static_cast<unsigned>(overlay_magic),
                      static_cast<unsigned>(overlay_header_ok),
                      static_cast<unsigned>(game_task_overlay));
        camera_trace(reason);
        return game_task_overlay;
    }
    const std::uint32_t game_sys = memory.load32(MHF_MHFU_GAME_SYS_SINGLETON);
    if (!range_ok(memory, game_sys, kGameSysFlags, 4u)) {
        camera_trace("camera task guard: GameSys flags are unreadable");
        return false;
    }

    const std::uint8_t pre_flag = memory.load8(game_sys + 0x6AF0Cu);
    const std::uint16_t stage_id = memory.load16(game_sys + 0x6AF0Eu);
    const std::uint8_t map_id = memory.load8(game_sys + 0x6AF10u);
    const std::uint16_t mission_id = memory.load16(game_sys + 0x6AF12u);
    const std::uint32_t flags = memory.load32(game_sys + kGameSysFlags);
    const bool lobby_task = (flags & kGameSysFlagLobbyTask) != 0u;
    const bool game_task_flag = (flags & kGameSysFlagGameTask) != 0u;

    std::uint32_t game_task = 0u;
    if (MHF_MHFU_GAME_TASK_SINGLETON != 0u &&
        memory.contains(MHF_MHFU_GAME_TASK_SINGLETON, 4u)) {
        game_task = memory.load32(MHF_MHFU_GAME_TASK_SINGLETON);
    }

    std::uint32_t task_manager = 0u;
    std::uint32_t active_tasks[4]{};
    bool game_task_active = false;
    if (MHF_MHFU_TASK_MANAGER_SINGLETON != 0u &&
        memory.contains(MHF_MHFU_TASK_MANAGER_SINGLETON, 4u)) {
        task_manager = memory.load32(MHF_MHFU_TASK_MANAGER_SINGLETON);
        if (game_task != 0u && range_ok(memory, game_task, 0u, 0x18u) &&
            range_ok(memory, task_manager, 0u, sizeof(active_tasks))) {
            for (std::size_t i = 0; i < 4u; ++i) {
                active_tasks[i] = memory.load32(task_manager + static_cast<std::uint32_t>(i * 4u));
                game_task_active = game_task_active || active_tasks[i] == game_task;
            }
        }
    }

    std::uint32_t quest = 0u;
    std::uint32_t quest_flags = 0u;
    std::uint32_t quest_camera_enemy = 0u;
    if (MHF_MHFU_QUEST_SINGLETON != 0u &&
        memory.contains(MHF_MHFU_QUEST_SINGLETON, 4u)) {
        quest = memory.load32(MHF_MHFU_QUEST_SINGLETON);
        if (range_ok(memory, quest, 0x24u, 8u)) {
            quest_camera_enemy = memory.load32(quest + 0x24u);
            quest_flags = memory.load32(quest + 0x28u);
        }
    }
    const bool quest_active = quest != 0u &&
        (quest_flags & kQuestMissionFlagMask) != 0u;

    std::uint32_t camera_player = 0u;
    std::uint32_t camera_area = 0u;
    std::uint8_t player_action = 0xFFu;
    bool camera_player_ok = false;
    if (range_ok(memory, camera, kCameraPlayer, 4u)) {
        camera_player = memory.load32(camera + kCameraPlayer);
        camera_player_ok = range_ok(memory, camera_player, kPlayerPositionX, 12u) &&
            range_ok(memory, camera_player, kPlayerActionType, 2u);
        if (camera_player_ok) player_action = memory.load8(camera_player + kPlayerActionType);
    }
    if (range_ok(memory, camera, kCameraAreaConfig, 4u)) {
        camera_area = memory.load32(camera + kCameraAreaConfig);
    }

    const std::uint32_t system = MHF_MHFU_SYSTEM_SINGLETON != 0u &&
        memory.contains(MHF_MHFU_SYSTEM_SINGLETON, 4u)
        ? memory.load32(MHF_MHFU_SYSTEM_SINGLETON) : 0u;

    const bool gs_slot_ok = memory.contains(MHF_MHFU_GAME_SYS_SINGLETON, 4u);
    const bool gt_slot_ok = MHF_MHFU_GAME_TASK_SINGLETON != 0u &&
        memory.contains(MHF_MHFU_GAME_TASK_SINGLETON, 4u);
    const bool tm_slot_ok = MHF_MHFU_TASK_MANAGER_SINGLETON != 0u &&
        memory.contains(MHF_MHFU_TASK_MANAGER_SINGLETON, 4u);
    const bool sys_slot_ok = MHF_MHFU_SYSTEM_SINGLETON != 0u &&
        memory.contains(MHF_MHFU_SYSTEM_SINGLETON, 4u);
    const bool quest_slot_ok = MHF_MHFU_QUEST_SINGLETON != 0u &&
        memory.contains(MHF_MHFU_QUEST_SINGLETON, 4u);

    char reason[512]{};
    std::snprintf(reason, sizeof(reason),
                  "taskOvl='%s' magic=%08X gameTaskOvl=%u | "
                  "GS=%08X sys=%08X pre=%u stg=%u map=%u mis=%u fl=%08X G=%u L=%u "
                  "GT=%08X TM=%08X live=%u Q=%08X qf=%08X qe=%08X qa=%u "
                  "camPlayer=%08X pOK=%u act=%u camArea=%08X "
                  "slots=(%08X,%08X,%08X,%08X,%08X) ok=(%u,%u,%u,%u,%u)",
                  overlay_name,
                  static_cast<unsigned>(overlay_magic),
                  static_cast<unsigned>(game_task_overlay),
                  static_cast<unsigned>(game_sys),
                  static_cast<unsigned>(system),
                  static_cast<unsigned>(pre_flag),
                  static_cast<unsigned>(stage_id),
                  static_cast<unsigned>(map_id),
                  static_cast<unsigned>(mission_id),
                  static_cast<unsigned>(flags),
                  static_cast<unsigned>(game_task_flag),
                  static_cast<unsigned>(lobby_task),
                  static_cast<unsigned>(game_task),
                  static_cast<unsigned>(task_manager),
                  static_cast<unsigned>(game_task_active),
                  static_cast<unsigned>(quest),
                  static_cast<unsigned>(quest_flags),
                  static_cast<unsigned>(quest_camera_enemy),
                  static_cast<unsigned>(quest_active),
                  static_cast<unsigned>(camera_player),
                  static_cast<unsigned>(camera_player_ok),
                  static_cast<unsigned>(player_action),
                  static_cast<unsigned>(camera_area),
                  static_cast<unsigned>(MHF_MHFU_GAME_SYS_SINGLETON),
                  static_cast<unsigned>(MHF_MHFU_GAME_TASK_SINGLETON),
                  static_cast<unsigned>(MHF_MHFU_TASK_MANAGER_SINGLETON),
                  static_cast<unsigned>(MHF_MHFU_SYSTEM_SINGLETON),
                  static_cast<unsigned>(MHF_MHFU_QUEST_SINGLETON),
                  static_cast<unsigned>(gs_slot_ok),
                  static_cast<unsigned>(gt_slot_ok),
                  static_cast<unsigned>(tm_slot_ok),
                  static_cast<unsigned>(sys_slot_ok),
                  static_cast<unsigned>(quest_slot_ok));
    camera_trace(reason);

    // The MHFU task overlay is the authoritative context indicator here.
    // Keep the old singleton reads in the trace for comparison, but do not let
    // their zero/uninitialised values disable the camera during a real quest.
    return game_task_overlay;
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


struct ShapedStick {
    float x{};
    float y{};
};

ShapedStick shape_stick(float x, float y) {
    x = std::clamp(x, -1.0f, 1.0f);
    y = std::clamp(y, -1.0f, 1.0f);

    const float magnitude = std::hypot(x, y);
    if (!std::isfinite(magnitude) || magnitude <= kDeadzone) return {};

    // Clamp only the vector length, not each component. This preserves the
    // stick's direction while mapping the circular deadzone to full range.
    const float limited_magnitude = std::min(magnitude, 1.0f);
    const float shaped_magnitude =
        (limited_magnitude - kDeadzone) / (1.0f - kDeadzone);
    const float scale = shaped_magnitude / magnitude;
    return {x * scale, y * scale};
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
        !range_ok(memory, player, kPlayerDialogTimer, 1u) ||
        !range_ok(memory, player, kPlayerScopeAimIncrement, 1u)) {
        camera_trace("protected player action: required player fields are unreadable");
        return true;
    }

    const std::uint8_t action_type = memory.load8(player + kPlayerActionType);
    const std::uint8_t action_id = memory.load8(player + kPlayerActionId);
    const std::uint8_t ride_state = memory.load8(player + kPlayerRideState);
    const std::uint8_t dialog_timer = memory.load8(player + kPlayerDialogTimer);
    const std::uint8_t scope_aim = memory.load8(player + kPlayerScopeAimIncrement);
    const std::uint32_t player_flags = memory.load32(player + kPlayerFlags);

    // NORMAL and ATTACK are ordinary hunt states. Damage, death, demos,
    // egg-carrying, chat, kitchen and farm actions retain the stock camera.
    if (action_type != 0u && action_type != 1u) {
        char reason[160]{};
        std::snprintf(reason, sizeof(reason),
                      "protected action type=%u id=%u",
                      static_cast<unsigned>(action_type),
                      static_cast<unsigned>(action_id));
        camera_trace(reason);
        return true;
    }

    // These IDs belong to NORMAL action states (hang/climb), not to ATTACK.
    // Action IDs are reused between action types, so applying this list to an
    // attack animation can accidentally freeze the custom camera.
    if (action_type == 0u &&
        (action_id == 17u || action_id == 27u || action_id == 30u || action_id == 58u)) {
        camera_trace("protected NORMAL hang/climb action");
        return true;
    }

    if (ride_state != 0u || dialog_timer != 0u || scope_aim != 0u ||
        (player_flags & (kPlFlagSupplyBoxOpen | kPlFlagDeliveryBoxOpen |
                         kPlFlagScopeCam | kPlFlagShoulderCam | kPlFlagFishing)) != 0u) {
        char reason[224]{};
        std::snprintf(reason, sizeof(reason),
                      "protected auxiliary type=%u id=%u ride=%u dialog=%u scope=%u flags=%08X",
                      static_cast<unsigned>(action_type),
                      static_cast<unsigned>(action_id),
                      static_cast<unsigned>(ride_state),
                      static_cast<unsigned>(dialog_timer),
                      static_cast<unsigned>(scope_aim),
                      static_cast<unsigned>(player_flags));
        camera_trace(reason);
        return true;
    }

    // posture==2 is the ordinary FLY posture, not a scripted-camera state.
    // Do not use it as a blanket camera lock during otherwise valid attacks.
    return false;
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

void restore_standard_camera_profile(psprecomp::GuestMemory &memory) {
    if (!g_orbit.active || g_orbit.mode != kOrbitModeStandard ||
        g_orbit.follow_height_entry == 0u ||
        !range_ok(memory, g_orbit.follow_height_entry, 0u, kFollowHeightEntrySize)) {
        return;
    }

    // The STD camera's native collision solver reads this follow-height entry
    // every frame. Restore the area's original profile whenever orbit control
    // is released so no custom pitch/distance leaks into another camera mode.
    store_float(memory, g_orbit.follow_height_entry + kFollowHeightYOffset,
                g_orbit.base_y_offset);
    store_float(memory, g_orbit.follow_height_entry + kFollowHeightZOffset,
                g_orbit.base_z_offset);
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
        restore_offset_pan_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace("defer: OFFSET_PAN area/script transition; preserving orbit");
        if (original != nullptr) original(runtime, context);
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
            g_orbit.camera == camera && g_orbit.player == player &&
            g_orbit.sub_camera == stg_sub_camera;
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
        const bool same_session = g_orbit.active &&
            g_orbit.mode == kOrbitModeOffsetPan && g_orbit.camera == camera &&
            g_orbit.player == player && g_orbit.sub_camera == stg_sub_camera;
        if (same_session) {
            restore_offset_pan_profile(memory);
            runtime.note_analog_camera_update(0u);
            camera_trace("OFFSET_PAN reinitializing new area; preserving hunt orbit");
        } else if (g_orbit.active && g_orbit.mode == kOrbitModeOffsetPan) {
            restore_offset_pan_profile(memory);
            g_orbit = {};
            runtime.note_analog_camera_update(0u);
            camera_trace("OFFSET_PAN camera/player session changed; clearing stale orbit");
        } else {
            camera_trace("OFFSET_PAN native initialization frame; takeover on next STG update");
        }
        if (original != nullptr) original(runtime, context);
        return;
    }
    if (stg_mode != 1u || stg_active == 0u || stg_state != 0u) {
        char reason[176]{};
        std::snprintf(reason, sizeof(reason),
                      "defer: OFFSET_PAN STG transition active=%u mode=%u state=%u; orbit retained",
                      static_cast<unsigned>(stg_active), static_cast<unsigned>(stg_mode),
                      static_cast<unsigned>(stg_state));
        restore_offset_pan_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace(reason);
        if (original != nullptr) original(runtime, context);
        return;
    }

    if (special_player_action(memory, player)) {
        restore_offset_pan_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace("defer: OFFSET_PAN protected player action; preserving orbit");
        if (original != nullptr) original(runtime, context);
        return;
    }

    if (!g_orbit.active || g_orbit.mode != kOrbitModeOffsetPan ||
        g_orbit.camera != camera || g_orbit.area != area || g_orbit.player != player ||
        g_orbit.area_id != memory.load8(area + 1u) || g_orbit.sub_camera != stg_sub_camera) {
        const bool preserve_angles = g_orbit.active && g_orbit.mode == kOrbitModeOffsetPan &&
            g_orbit.camera == camera && g_orbit.player == player &&
            g_orbit.sub_camera == stg_sub_camera;
        const float saved_yaw = g_orbit.yaw;
        const float saved_elevation = g_orbit.elevation;
        const float saved_initial_elevation = g_orbit.initial_elevation;
        const float saved_distance = g_orbit.distance;
        restore_offset_pan_profile(memory);
        restore_standard_camera_profile(memory);
        g_orbit = {};
        if (!capture_offset_pan_orbit(g_orbit, memory, stg_sub_camera, camera, area, player)) {
            fallback(runtime, context, original, "fallback: could not capture initial OFFSET_PAN orbit");
            return;
        }
        if (preserve_angles && saved_distance >= kMinOrbitDistance &&
            saved_distance <= kMaxOrbitDistance) {
            g_orbit.yaw = saved_yaw;
            g_orbit.elevation = saved_elevation;
            g_orbit.initial_elevation = saved_initial_elevation;
            g_orbit.distance = saved_distance;
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
    const ShapedStick stick = shape_stick(pad.rx, pad.ry);
    const float dx = stick.x;
    const float dy = stick.y;
    // R1 remains exclusively the game's item-selector button; it never recenters
    // or otherwise modifies the custom camera.
    g_orbit.yaw = wrap_angle(g_orbit.yaw - dx * kYawSpeed * dt);
    g_orbit.elevation = std::clamp(g_orbit.elevation + dy * kPitchSpeed * dt,
                                   kMinElevation, kMaxElevation);

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
                "rx=%+.3f ry=%+.3f shaped=(%+.3f,%+.3f) buttons=0x%08X "
                "yaw=%.3f elevation=%.3f player=(%.1f,%.1f,%.1f) "
                "target=(%.1f,%.1f,%.1f) pan=(%.1f,%.1f,%.1f)\n",
                static_cast<unsigned>(area),
                static_cast<unsigned>(memory.load8(stg_sub_camera + kSubCameraIsActive)),
                static_cast<unsigned>(memory.load8(stg_sub_camera + 0x98u)),
                static_cast<unsigned>(memory.load8(stg_sub_camera + kSubCameraState)),
                static_cast<double>(pad.rx), static_cast<double>(pad.ry),
                static_cast<double>(dx), static_cast<double>(dy),
                static_cast<unsigned>(pad.buttons),
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
    restore_standard_camera_profile(runtime.memory());
    restore_offset_pan_profile(runtime.memory());
    g_orbit = {};
    runtime.note_analog_camera_update(0u);
    if (original != nullptr) original(runtime, context);
}

void sync_standard_orbit_yaw_from_native(
    Runtime &runtime, std::uint32_t camera, std::uint32_t std_data) {
    auto &memory = runtime.memory();
    if (!g_orbit.active || g_orbit.mode != kOrbitModeStandard ||
        g_orbit.camera != camera ||
        !range_ok(memory, std_data, kStdRotation, 2u)) {
        return;
    }

    const auto native_rotation =
        static_cast<std::int16_t>(memory.load16(std_data + kStdRotation));
    const float native_yaw = wrap_angle(
        static_cast<float>(native_rotation) / kAngleUnitsPerRadian);
    if (!std::isfinite(native_yaw)) return;

    g_orbit.yaw = native_yaw;
    g_orbit.last_update_us = runtime.now_us();

    if (camera_trace_enabled()) {
        static std::uint64_t last_sync_trace_us = 0u;
        const std::uint64_t now = runtime.now_us();
        if (last_sync_trace_us == 0u || now < last_sync_trace_us ||
            now - last_sync_trace_us >= 500'000u) {
            std::fprintf(stderr,
                "[mhfu-camera] protected-state yaw sync native_yaw=%.3f yaw_units=%u\n",
                static_cast<double>(native_yaw),
                static_cast<unsigned>(static_cast<std::uint16_t>(native_rotation)));
            last_sync_trace_us = now;
        }
    }
}

void update_standard_orbit_before_native(Runtime &runtime, std::uint32_t camera) {
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
        restore_standard_camera_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-before-native deferred: camera or subcamera fields are unreadable; orbit retained");
        return;
    }

    const int active_special_id = active_special_sub_camera(memory, camera);
    const std::uint8_t selected_camera_id = memory.load8(camera + 0xDC4u);
    if (active_special_id >= 0 || selected_camera_id == 3u ||
        selected_camera_id == 4u || selected_camera_id == 5u) {
        // The game owns the rendered view during zoom/fishing, scope and demos.
        // Restore the native profile but retain the orbit to resume afterward.
        restore_standard_camera_profile(memory);
        runtime.note_analog_camera_update(0u);
        return;
    }

    const std::uint32_t area = memory.load32(camera + kCameraAreaConfig);
    const std::uint32_t player = memory.load32(camera + kCameraPlayer);
    if (area == 0u || player == 0u ||
        !range_ok(memory, player, kPlayerPositionX, 12u)) {
        restore_standard_camera_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-before-native deferred: area or player pointer is invalid; orbit retained");
        return;
    }

    const std::uint8_t std_active = memory.load8(sub_camera + kSubCameraIsActive);
    const std::uint8_t current_id = memory.load8(sub_camera + kSubCameraCurrentId);
    const std::uint8_t state = memory.load8(sub_camera + kSubCameraState);
    if (current_id != 0xFFu || state != 0u) {
        restore_standard_camera_profile(memory);
        runtime.note_analog_camera_update(0u);
        char reason[176]{};
        std::snprintf(reason, sizeof(reason),
                      "STD-before-native deferred: active=%u current_id=0x%02X state=%u; orbit retained",
                      static_cast<unsigned>(std_active), static_cast<unsigned>(current_id),
                      static_cast<unsigned>(state));
        camera_trace(reason);
        return;
    }

    if (special_player_action(memory, player)) {
        // Native camera actions can change yaw temporarily. Capture the live
        // native rotation so resuming standard orbit cannot snap to stale yaw.
        sync_standard_orbit_yaw_from_native(runtime, camera, std_data);
        restore_standard_camera_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-before-native deferred: protected player action; native yaw synchronized");
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
    // A collision can legitimately pull the rendered camera very close to
    // its target. Validate that the view vectors are finite, but validate
    // orbit distance from the unmodified follow profile below, not this
    // collision-shortened rendered position.
    if (!std::isfinite(position_x) || !std::isfinite(position_y) ||
        !std::isfinite(position_z) || !std::isfinite(target_x) ||
        !std::isfinite(target_y) || !std::isfinite(target_z) ||
        !std::isfinite(horizontal) || horizontal < 1.0f) {
        restore_standard_camera_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-before-native deferred: native camera position/target is invalid");
        return;
    }

    if (!range_ok(memory, std_data, kStdFollowHeightEntry, 4u)) {
        restore_standard_camera_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-before-native deferred: follow-height entry pointer is unreadable");
        return;
    }
    const std::uint32_t follow_entry = memory.load32(std_data + kStdFollowHeightEntry);
    const std::uint32_t follow_definition =
        memory.load32(std_data + kStdFollowHeightDefinition);
    constexpr std::uint32_t kFollowHeightDefinitionSize =
        kFollowHeightEntriesOffset + kFollowHeightEntryCount * kFollowHeightEntrySize;
    const bool follow_entry_is_area_profile =
        follow_definition != 0u &&
        range_ok(memory, follow_definition, 0u, kFollowHeightDefinitionSize) &&
        follow_entry >= follow_definition + kFollowHeightEntriesOffset &&
        follow_entry < follow_definition + kFollowHeightDefinitionSize &&
        ((follow_entry - (follow_definition + kFollowHeightEntriesOffset)) %
         kFollowHeightEntrySize) == 0u &&
        range_ok(memory, follow_entry, 0u, kFollowHeightEntrySize);
    if (!follow_entry_is_area_profile) {
        // During fall/kabegiwa camera states the game temporarily points this
        // field at standalone special profiles, not at the area's writable
        // seven-entry table. Never mutate those shared special presets.
        restore_standard_camera_profile(memory);
        runtime.note_analog_camera_update(0u);
        camera_trace("STD-before-native deferred: special follow-height profile; orbit retained");
        return;
    }

    const bool same_orbit = g_orbit.active && g_orbit.mode == kOrbitModeStandard &&
        g_orbit.camera == camera && g_orbit.sub_camera == sub_camera &&
        g_orbit.area == area && g_orbit.player == player &&
        g_orbit.follow_height_entry == follow_entry;
    if (!same_orbit) {
        const bool preserve_angles = g_orbit.active &&
            g_orbit.mode == kOrbitModeStandard && g_orbit.camera == camera &&
            g_orbit.sub_camera == sub_camera && g_orbit.player == player;
        const float saved_yaw = g_orbit.yaw;
        const float saved_elevation = g_orbit.elevation;
        const float saved_initial_elevation = g_orbit.initial_elevation;
        const float saved_distance = g_orbit.distance;
        // Restore the prior area's profile before capturing the new area's baseline.
        restore_standard_camera_profile(memory);
        g_orbit = {};

        const float base_y_offset = load_float(memory, follow_entry + kFollowHeightYOffset);
        const float base_z_offset = load_float(memory, follow_entry + kFollowHeightZOffset);
        const float base_height = load_float(memory, follow_entry + kFollowHeightHeight);
        const float vertical = base_y_offset - base_height;
        const float native_distance = std::hypot(base_z_offset, vertical);
        const auto native_rotation =
            static_cast<std::int16_t>(memory.load16(std_data + kStdRotation));
        const float native_yaw = wrap_angle(static_cast<float>(native_rotation) /
                                             kAngleUnitsPerRadian);
        if (!std::isfinite(base_y_offset) || !std::isfinite(base_z_offset) ||
            !std::isfinite(base_height) || !std::isfinite(native_distance) ||
            base_z_offset <= 1.0f || native_distance < kMinOrbitDistance ||
            native_distance > kMaxOrbitDistance) {
            runtime.note_analog_camera_update(0u);
            camera_trace("STD-before-native deferred: follow-height profile is invalid; orbit retained");
            return;
        }

        g_orbit.active = true;
        g_orbit.mode = kOrbitModeStandard;
        g_orbit.sub_camera = sub_camera;
        g_orbit.camera = camera;
        g_orbit.area = area;
        g_orbit.player = player;
        g_orbit.follow_height_entry = follow_entry;
        g_orbit.base_y_offset = base_y_offset;
        g_orbit.base_z_offset = base_z_offset;
        g_orbit.base_height = base_height;
        g_orbit.distance = preserve_angles && saved_distance >= kMinOrbitDistance &&
            saved_distance <= kMaxOrbitDistance ? saved_distance : native_distance;
        g_orbit.yaw = preserve_angles ? saved_yaw : native_yaw;
        g_orbit.elevation = preserve_angles ? saved_elevation :
            std::clamp(std::atan2(vertical, base_z_offset), kMinElevation, kMaxElevation);
        g_orbit.initial_elevation = preserve_angles ? saved_initial_elevation : g_orbit.elevation;
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
    const ShapedStick stick = shape_stick(pad.rx, pad.ry);
    const float dx = stick.x;
    const float dy = stick.y;
    // R1 is not a camera input. The native game keeps it for item selection.
    g_orbit.yaw = wrap_angle(g_orbit.yaw - dx * kYawSpeed * dt);
    g_orbit.elevation = std::clamp(g_orbit.elevation + dy * kPitchSpeed * dt,
                                   kMinElevation, kMaxElevation);
    g_orbit.last_update_us = now;

    // Feed the desired orbit into the native STD follow profile before
    // cam_sub_std runs. The original routine consumes these values in the same
    // frame, so its wall/ceiling/floor solver resolves the requested view
    // instead of chasing a profile written after the previous frame.
    const float vertical_offset = std::sin(g_orbit.elevation) * g_orbit.distance;
    const float horizontal_offset = std::cos(g_orbit.elevation) * g_orbit.distance;
    const float desired_y_offset = g_orbit.base_height + vertical_offset;
    const bool finite_profile = std::isfinite(vertical_offset) &&
        std::isfinite(horizontal_offset) && std::isfinite(desired_y_offset) &&
        horizontal_offset > 1.0f;
    if (finite_profile &&
        range_ok(memory, g_orbit.follow_height_entry, 0u, kFollowHeightEntrySize)) {
        store_float(memory, g_orbit.follow_height_entry + kFollowHeightYOffset,
                    desired_y_offset);
        store_float(memory, g_orbit.follow_height_entry + kFollowHeightZOffset,
                    horizontal_offset);
        const std::uint16_t yaw_units = angle_to_units(g_orbit.yaw);
        memory.store16(std_data + kStdGoalRotation, yaw_units);
        memory.store16(std_data + kStdRotation, yaw_units);
        memory.store8(std_data + kStdIsFastRotate, 0u);
    }

    const bool collision_constrained =
        memory.load8(std_data + kStdIsFalldown) != 0u ||
        memory.load8(std_data + kStdIsViewBlocked) != 0u ||
        memory.load8(std_data + kStdIsKabegiwa) != 0u ||
        memory.load8(std_data + kStdKabegiwaTimer) != 0u;

    runtime.note_analog_camera_update(now);
    if (camera_trace_enabled()) {
        static std::uint64_t last_input_trace_us = 0u;
        if (last_input_trace_us == 0u || now < last_input_trace_us ||
            now - last_input_trace_us >= 500'000u) {
            std::fprintf(stderr,
                "[mhfu-camera] STD-before-native input rx=%+.3f ry=%+.3f "
                "shaped=(%+.3f,%+.3f) buttons=0x%08X "
                "yaw=%.3f elevation=%.3f native_collision_flags=%u "
                "yaw_units=%u target=(%.1f,%.1f,%.1f)\n",
                static_cast<double>(pad.rx), static_cast<double>(pad.ry),
                static_cast<double>(dx), static_cast<double>(dy),
                static_cast<unsigned>(pad.buttons),
                static_cast<double>(g_orbit.yaw),
                static_cast<double>(g_orbit.elevation),
                static_cast<unsigned>(collision_constrained),
                static_cast<unsigned>(angle_to_units(g_orbit.yaw)),
                static_cast<double>(target_x), static_cast<double>(target_y),
                static_cast<double>(target_z));
            last_input_trace_us = now;
        }
    }
    camera_trace("orbit active: STD pre-native right-stick camera path accepted");
}


void apply_standard_orbit_pitch_after_native(Runtime &runtime, std::uint32_t camera) {
    auto &memory = runtime.memory();
    if (!g_orbit.active || g_orbit.mode != kOrbitModeStandard ||
        g_orbit.camera != camera) {
        return;
    }

    const std::uint32_t sub_camera = camera + kCameraFromSubCamera;
    const std::uint32_t std_data = sub_camera + kSubCameraData;
    if (g_orbit.sub_camera != sub_camera ||
        !range_ok(memory, sub_camera, 0u, 0x1Cu) ||
        !range_ok(memory, std_data, kStdIsFalldown, 1u) ||
        !range_ok(memory, std_data, kStdIsViewBlocked, 1u) ||
        !range_ok(memory, std_data, kStdIsKabegiwa, 1u) ||
        !range_ok(memory, std_data, kStdKabegiwaTimer, 1u) ||
        !range_ok(memory, std_data, 0u, 12u) ||
        !range_ok(memory, std_data, kStdGoalPosition + 4u, 4u) ||
        !range_ok(memory, std_data, kStdIsGroundAdjusted, 1u)) {
        return;
    }

    const bool collision_constrained =
        memory.load8(std_data + kStdIsFalldown) != 0u ||
        memory.load8(std_data + kStdIsViewBlocked) != 0u ||
        memory.load8(std_data + kStdIsKabegiwa) != 0u ||
        memory.load8(std_data + kStdKabegiwaTimer) != 0u;

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
    const float distance = std::sqrt(
        offset_x * offset_x + offset_y * offset_y + offset_z * offset_z);
    if (!std::isfinite(position_x) || !std::isfinite(position_y) ||
        !std::isfinite(position_z) || !std::isfinite(target_x) ||
        !std::isfinite(target_y) || !std::isfinite(target_z) ||
        !std::isfinite(horizontal) || !std::isfinite(distance) ||
        horizontal < 0.01f || distance < 1.0f) {
        return;
    }

    // Keep the native collision-safe position untouched while a wall/fall/
    // blocked-view contact is active, but adopt its resolved orbit angles.
    // Without this sync, g_orbit keeps the pre-contact angles and snaps back
    // to that stale orbit on the first frame after the native flag clears.
    if (collision_constrained) {
        g_orbit.yaw = wrap_angle(std::atan2(-offset_x, -offset_z));
        g_orbit.elevation = std::clamp(
            std::atan2(offset_y, horizontal), kMinElevation, kMaxElevation);
        g_orbit.last_update_us = runtime.now_us();
        return;
    }

    // The native STD routine eases its vertical camera vector toward the
    // follow-height profile. Correct only its resolved pitch here, after the
    // native update, while preserving its collision-adjusted radius and yaw.
    const float desired_elevation =
        std::clamp(g_orbit.elevation, kMinElevation, kMaxElevation);
    const float desired_vertical = std::sin(desired_elevation) * distance;

    // goal_position.y is calculated by the native STD floor solver every
    // frame. It equals the requested pitch in open space and is raised to
    // ground hit + min_height when the camera would penetrate the floor.
    // Do not gate this boundary on is_ground_adj: the final native position
    // clamp can protect the floor without setting that flag.
    const float requested_y = target_y + desired_vertical;
    const bool native_ground_adjusted =
        memory.load8(std_data + kStdIsGroundAdjusted) != 0u;
    const float native_safe_goal_y =
        load_float(memory, std_data + kStdGoalPosition + 4u);
    if (!std::isfinite(native_safe_goal_y)) return;
    const float corrected_y = std::max(requested_y, native_safe_goal_y);
    const bool floor_clamped = corrected_y > requested_y + 0.5f;

    const float corrected_vertical = corrected_y - target_y;
    if (!std::isfinite(corrected_vertical) ||
        std::fabs(corrected_vertical) >= distance) {
        return;
    }

    float corrected_horizontal = horizontal;
    float corrected_x = target_x + offset_x;
    float corrected_z = target_z + offset_z;

    if (floor_clamped) {
        // Keep the native collision-resolved XZ position when the floor
        // rejects the requested pitch. Sync the stored elevation to this
        // safe view so holding down cannot accumulate hidden pitch.
        g_orbit.elevation = std::clamp(
            std::atan2(corrected_vertical, corrected_horizontal),
            kMinElevation, kMaxElevation);
    } else {
        // In free space, preserve the existing constant-radius orbit and
        // exact pitch response.
        corrected_horizontal =
            std::sqrt(std::max(0.0f, distance * distance -
                                       corrected_vertical * corrected_vertical));
        const float horizontal_scale = corrected_horizontal / horizontal;
        if (!std::isfinite(horizontal_scale)) return;
        corrected_x = target_x + offset_x * horizontal_scale;
        corrected_z = target_z + offset_z * horizontal_scale;
    }
    if (!std::isfinite(corrected_x) || !std::isfinite(corrected_y) ||
        !std::isfinite(corrected_z)) {
        return;
    }

    // SubCamera::current_position is the rendered output, but the native
    // STD solver starts its next frame from StdCameraData::position at
    // std_data + 0x00. Keep both copies synchronized or the next native pass
    // pulls the view back toward the stale pre-correction position.
    //
    // Apply the same delta to the internal position rather than copying the
    // rendered Y directly: current_position includes a native camera-shake
    // offset that is added after StdCameraData::position is copied.
    const float native_position_x = load_float(memory, std_data + 0x00u);
    const float native_position_y = load_float(memory, std_data + 0x04u);
    const float native_position_z = load_float(memory, std_data + 0x08u);
    const float correction_dx = corrected_x - position_x;
    const float correction_dy = corrected_y - position_y;
    const float correction_dz = corrected_z - position_z;
    if (!std::isfinite(native_position_x) || !std::isfinite(native_position_y) ||
        !std::isfinite(native_position_z) || !std::isfinite(correction_dx) ||
        !std::isfinite(correction_dy) || !std::isfinite(correction_dz)) {
        return;
    }

    store_float(memory, sub_camera + 0x00u, corrected_x);
    store_float(memory, sub_camera + 0x04u, corrected_y);
    store_float(memory, sub_camera + 0x08u, corrected_z);
    store_float(memory, std_data + 0x00u, native_position_x + correction_dx);
    store_float(memory, std_data + 0x04u, native_position_y + correction_dy);
    store_float(memory, std_data + 0x08u, native_position_z + correction_dz);

    if (camera_trace_enabled()) {
        static std::uint64_t last_pitch_trace_us = 0u;
        const std::uint64_t now = runtime.now_us();
        if (last_pitch_trace_us == 0u || now < last_pitch_trace_us ||
            now - last_pitch_trace_us >= 500'000u) {
            const float before_elevation = std::atan2(offset_y, horizontal);
            std::fprintf(stderr,
                "[mhfu-camera] STD-post-native pitch desired=%.3f before=%.3f "
                "after=%.3f resolved_distance=%.1f ground_adjusted=%u "
                "floor_clamp=%u goal_y=%.1f corrected_y=%.1f\n",
                static_cast<double>(desired_elevation),
                static_cast<double>(before_elevation),
                static_cast<double>(std::atan2(corrected_vertical, corrected_horizontal)),
                static_cast<double>(distance),
                static_cast<unsigned>(native_ground_adjusted),
                static_cast<unsigned>(floor_clamped),
                static_cast<double>(native_safe_goal_y),
                static_cast<double>(corrected_y));
            last_pitch_trace_us = now;
        }
    }
}

void replace_standard_camera(Runtime &runtime, Context &context, GuestFunction original) {
    camera_trace("replacement reached: mh_camera_orbit_update");
    auto &memory = runtime.memory();
    const std::uint32_t sub_camera = context.gpr[4]; // SubCamera::cam_sub_std this
    if (camera_trace_enabled()) {
        static std::uint64_t last_std_hook_trace_us = 0u;
        const std::uint64_t now = runtime.now_us();
        if (last_std_hook_trace_us == 0u || now < last_std_hook_trace_us ||
            now - last_std_hook_trace_us >= 500'000u) {
            const std::uint32_t camera = sub_camera >= kCameraFromSubCamera
                ? sub_camera - kCameraFromSubCamera : 0u;
            const bool readable = range_ok(memory, camera, kCameraAreaSubCameraId, 1u) &&
                range_ok(memory, camera, 0xDC4u, 1u);
            const std::uint8_t area_sub = readable ? memory.load8(camera + kCameraAreaSubCameraId) : 0xFFu;
            const std::uint8_t selected = readable ? memory.load8(camera + 0xDC4u) : 0xFFu;
            const bool hunt = is_hunt_camera_context(memory, camera);
            const ControllerState &pad = controller_state(runtime);
            std::fprintf(stderr,
                "[mhfu-camera] STD hook this=0x%08X camera=0x%08X area_sub=%u selected=%u "
                "hunt=%u buttons=0x%08X R1=%u rx=%+.3f ry=%+.3f\n",
                static_cast<unsigned>(sub_camera), static_cast<unsigned>(camera),
                static_cast<unsigned>(area_sub), static_cast<unsigned>(selected),
                static_cast<unsigned>(hunt), static_cast<unsigned>(pad.buttons),
                static_cast<unsigned>((pad.buttons & buttons::kRTrigger) != 0u),
                static_cast<double>(pad.rx), static_cast<double>(pad.ry));
            last_std_hook_trace_us = now;
        }
    }
    if (sub_camera >= kCameraFromSubCamera) {
        const std::uint32_t camera = sub_camera - kCameraFromSubCamera;
        if (range_ok(memory, camera, kCameraAreaSubCameraId, 1u) &&
            memory.load8(camera + kCameraAreaSubCameraId) == 0u) {
            if (is_hunt_camera_context(memory, camera)) {
                update_standard_orbit_before_native(runtime, camera);
            } else {
                restore_standard_camera_profile(memory);
                restore_offset_pan_profile(memory);
                g_orbit = {};
                runtime.note_analog_camera_update(0u);
            }
        } else if (g_orbit.active && g_orbit.mode == kOrbitModeStandard) {
            restore_standard_camera_profile(memory);
            runtime.note_analog_camera_update(0u);
        }
    }
    if (original == nullptr) return;

    // The original SubCamera::Cam_senkai_chk() uses R_TRIGGER + RISING_EDGE
    // to rotate the camera while a bow is drawn. Keep the trigger available to
    // the rest of the game (including item selection), but hide it from this
    // camera-only update. Player::buttons and StdCameraData input are snapshots
    // consumed by cam_sub_std(); restore them immediately after the native call.
    bool r1_masked = false;
    std::uint32_t player_buttons_address = 0u;
    std::uint32_t saved_player_buttons = 0u;
    std::uint32_t std_buttons_address = 0u;
    std::uint32_t std_rising_edge_address = 0u;
    std::uint16_t saved_std_buttons = 0u;
    std::uint16_t saved_std_rising_edge = 0u;

    if (sub_camera >= kCameraFromSubCamera) {
        const std::uint32_t camera = sub_camera - kCameraFromSubCamera;
        const std::uint32_t std_data = sub_camera + kSubCameraData;
        if (range_ok(memory, std_data, 0x84u, 4u) &&
            range_ok(memory, camera, kCameraPlayer, 4u)) {
            const std::uint32_t player = memory.load32(camera + kCameraPlayer);
            player_buttons_address = player + 0x480u;
            if (player != 0u && range_ok(memory, player, 0x480u, 4u)) {
                std_buttons_address = std_data + 0x84u;
                std_rising_edge_address = std_data + 0x86u;
                saved_std_buttons = memory.load16(std_buttons_address);
                saved_std_rising_edge = memory.load16(std_rising_edge_address);
                saved_player_buttons = memory.load32(player_buttons_address);
                const std::uint16_t r1 = static_cast<std::uint16_t>(buttons::kRTrigger);
                if (((saved_std_buttons | saved_std_rising_edge) & r1) != 0u ||
                    (saved_player_buttons & buttons::kRTrigger) != 0u) {
                    memory.store16(std_buttons_address,
                        static_cast<std::uint16_t>(saved_std_buttons & ~r1));
                    memory.store16(std_rising_edge_address,
                        static_cast<std::uint16_t>(saved_std_rising_edge & ~r1));
                    memory.store32(player_buttons_address,
                        saved_player_buttons & ~buttons::kRTrigger);
                    r1_masked = true;
                }
            }
        }
    }

    if (r1_masked) {
        camera_trace("native camera R1 suppressed for this camera update; game input remains unchanged");
    }
    original(runtime, context);

    if (r1_masked) {
        memory.store16(std_buttons_address, saved_std_buttons);
        memory.store16(std_rising_edge_address, saved_std_rising_edge);
        memory.store32(player_buttons_address, saved_player_buttons);
    }
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
    const bool area_type_ok = range_ok(memory, area, kAreaMoveType, 1u);
    const std::uint8_t area_move_type = area_type_ok
        ? memory.load8(area + kAreaMoveType) : 0xFFu;
    const bool player_action_ok = range_ok(memory, player, kPlayerActionType, 1u);
    const std::uint8_t player_action = player_action_ok
        ? memory.load8(player + kPlayerActionType) : 0xFFu;
    const bool hunt_context = is_hunt_camera_context(memory, camera);
    const bool is_offset_pan = area_sub_id == 2u && area_type_ok && area_move_type == 3u;

    // This decision line is intentionally separate from the singleton trace:
    // it tells village and active-quest callback states apart and shows whether
    // the current camera area is eligible for the offset-pan implementation.
    if (camera_trace_enabled()) {
        static std::uint64_t last_decision_trace_us = 0u;
        const std::uint64_t now = runtime.now_us();
        if (last_decision_trace_us == 0u || now < last_decision_trace_us ||
            now - last_decision_trace_us >= 500'000u) {
            const ControllerState &pad = controller_state(runtime);
            std::fprintf(stderr,
                "[mhfu-camera] STG decision hunt=%u area_sub=%u area=0x%08X "
                "area_type=%u area_type_ok=%u offset_pan=%u player=0x%08X "
                "player_action=%u player_action_ok=%u rx=%+.3f ry=%+.3f\n",
                static_cast<unsigned>(hunt_context),
                static_cast<unsigned>(area_sub_id),
                static_cast<unsigned>(area),
                static_cast<unsigned>(area_move_type),
                static_cast<unsigned>(area_type_ok),
                static_cast<unsigned>(is_offset_pan),
                static_cast<unsigned>(player),
                static_cast<unsigned>(player_action),
                static_cast<unsigned>(player_action_ok),
                static_cast<double>(pad.rx), static_cast<double>(pad.ry));
            last_decision_trace_us = now;
        }
    }

    if (!hunt_context) {
        restore_standard_camera_profile(memory);
        restore_offset_pan_profile(memory);
        g_orbit = {};
        runtime.note_analog_camera_update(0u);
        camera_trace("custom camera disabled outside GAME_TASK");
        if (original != nullptr) original(runtime, context);
        return;
    }

    if (!is_offset_pan) {
        if (g_orbit.active && g_orbit.mode == kOrbitModeOffsetPan) {
            restore_offset_pan_profile(memory);
            g_orbit = {};
            runtime.note_analog_camera_update(0u);
        }

        // The camera dispatcher runs SubCamera::cam_sub() in ascending ID
        // order: STD (0) is processed before STG (2). On US MHFU the STD
        // replacement is not reliably reached every frame, while this STG
        // callback is. Use this post-STD point to seed/update the standard
        // follow profile from the now-resolved native camera vectors. The
        // profile is consumed by the next STD update, preserving native
        // collision handling and scripted-camera guards.
        if (area_sub_id == 0u) {
            update_standard_orbit_before_native(runtime, camera);
        }

        if (original != nullptr) original(runtime, context);
        if (area_sub_id == 0u && g_orbit.active &&
            g_orbit.mode == kOrbitModeStandard) {
            apply_standard_orbit_pitch_after_native(runtime, camera);
        }
        if (area_sub_id != 0u && g_orbit.active &&
            g_orbit.mode == kOrbitModeStandard) {
            restore_standard_camera_profile(memory);
            runtime.note_analog_camera_update(0u);
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
