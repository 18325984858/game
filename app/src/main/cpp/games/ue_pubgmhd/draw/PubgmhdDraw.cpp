#include "PubgmhdDraw.h"
#include "../../../core/log/log.h"
#include <imgui/imgui_internal.h>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <unordered_map>

#if defined(AI_OVERLAY_AVAILABLE)
#include "../../../ai_overlay/AIDetection.h"
#include "../../../ai_overlay/AimAssist.h"
#endif

#define DLOG(level, fmt, ...) LOG(level, fmt, ##__VA_ARGS__)

// =====================================================================
//  历史错误总结:
//  [BUG-3] ImGui::Begin assert "g.WithinFrameScope" failed
//    原因: 游戏引擎线程 (MainThread-UE4) 触发了注入的 ImGui context
//          的 assert, 但崩溃线程并非我们的 GUI 线程
//    修复: 在 drawOverlay 入口检查 WithinFrameScope, 但会导致不绘制
//          最终方案: 确保 AImGui BeginFrame 成功后才调用 drawOverlay
//          保留 WithinFrameScope 检查作为安全网
// =====================================================================

namespace ue4draw {

namespace {
using Clock = std::chrono::steady_clock;

bool isValidNumber(float value);
float sanitizeFov(float value);

struct ViewPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct CameraSpacePoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ProjectionViewport {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct ProjectionBasis {
    ProjectionViewport viewport{};
    float focalLength = 0.0f;
    float projectionWidth = 0.0f;
};

ProjectionViewport getProjectionViewport(float screenW, float screenH) {
    ProjectionViewport viewport{0.0f, 0.0f, screenW, screenH};
    if (!std::isfinite(screenW) || !std::isfinite(screenH) || screenW <= 0.0f || screenH <= 0.0f) {
        return viewport;
    }

    // UI/HUD safe bounds can be inset on cutout devices, but the UE4 SurfaceView
    // itself is full-screen. 3D projection must use the render surface, otherwise
    // off-center targets drift as the forced viewport center moves.
    return viewport;
}

bool buildProjectionBasis(const DrawGameData& data,
                          float screenW,
                          float screenH,
                          ProjectionBasis& outBasis) {
    constexpr float DEG2RAD = 3.14159265358979f / 180.0f;
    constexpr float kUE4ReferenceAspect = 16.0f / 9.0f;
    constexpr float kAspectSlack = 0.01f;

    const ProjectionViewport viewport = getProjectionViewport(screenW, screenH);
    if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height)
        || viewport.width <= 0.0f || viewport.height <= 0.0f) {
        return false;
    }

    const float tanHalfFov = std::tan(sanitizeFov(data.camFOV) * 0.5f * DEG2RAD);
    if (!isValidNumber(tanHalfFov) || tanHalfFov < 0.01f) {
        return false;
    }

    // PUBG mobile 在 21:9 设备 (2400x1080) 上直接全屏渲染, FOV 是横向 FOV 应用到
    // 整个 surface 宽度. 之前强制用 16:9 参考宽度 (1920) 算 focal 会导致 X 坐标
    // 被等比拉向中心 (1920/2400=0.8x), 屏幕上目标越偏离中心, ESP 越往中心偏.
    // 必须用 viewport 真实宽度计算 focal.
    (void)kUE4ReferenceAspect; (void)kAspectSlack;
    const float projectionWidth = viewport.width;

    outBasis.viewport = viewport;
    outBasis.projectionWidth = projectionWidth;
    outBasis.focalLength = projectionWidth * 0.5f / tanHalfFov;
    return isValidNumber(outBasis.focalLength) && outBasis.focalLength > 1.0f;
}

bool projectCameraPoint(const ProjectionBasis& basis,
                        const CameraSpacePoint& cameraPoint,
                        float& sx,
                        float& sy) {
    constexpr float kNearDepth = 1.0f;
    if (cameraPoint.z <= kNearDepth) {
        return false;
    }

    const ProjectionViewport& viewport = basis.viewport;
    sx = viewport.left + viewport.width * 0.5f + cameraPoint.x * basis.focalLength / cameraPoint.z;
    sy = viewport.top + viewport.height * 0.5f - cameraPoint.y * basis.focalLength / cameraPoint.z;
    return isValidNumber(sx) && isValidNumber(sy);
}

bool shouldLogEvery(Clock::time_point& lastLogTime, std::chrono::milliseconds interval) {
    const auto now = Clock::now();
    if (lastLogTime.time_since_epoch().count() != 0 && now - lastLogTime < interval) {
        return false;
    }
    lastLogTime = now;
    return true;
}

const char* onOff(bool value) {
    return value ? "on" : "off";
}

bool isValidNumber(float value) {
    return std::isfinite(value);
}

bool hasValidWorldPoint(float x, float y, float z) {
    return isValidNumber(x) && isValidNumber(y) && isValidNumber(z)
        && (std::fabs(x) > 1.0f || std::fabs(y) > 1.0f || std::fabs(z) > 1.0f);
}

bool hasValidCameraPose(const DrawGameData& data) {
    return hasValidWorldPoint(data.camLocX, data.camLocY, data.camLocZ)
        && isValidNumber(data.camPitch)
        && isValidNumber(data.camYaw)
        && isValidNumber(data.camRoll)
        && data.camFOV >= 30.0f
        && data.camFOV <= 170.0f;
}

const DrawPlayerInfo* findPlayerByKey(const DrawGameData& data, uint32_t playerKey) {
    for (const auto& player : data.players) {
        if (player.playerKey == playerKey) {
            return &player;
        }
    }
    return nullptr;
}

void copyCameraState(const DrawGameData& src, DrawGameData& dst) {
    dst.camLocX = src.camLocX;
    dst.camLocY = src.camLocY;
    dst.camLocZ = src.camLocZ;
    dst.camPitch = src.camPitch;
    dst.camYaw = src.camYaw;
    dst.camRoll = src.camRoll;
    dst.camFOV = src.camFOV;
}

void backfillTransientPlayerState(DrawGameData& current, const DrawGameData& previous) {
    for (auto& player : current.players) {
        const DrawPlayerInfo* previousPlayer = findPlayerByKey(previous, player.playerKey);
        if (nullptr == previousPlayer) {
            continue;
        }

        if (!hasValidWorldPoint(player.posX, player.posY, player.posZ)
            && hasValidWorldPoint(previousPlayer->posX, previousPlayer->posY, previousPlayer->posZ)) {
            player.posX = previousPlayer->posX;
            player.posY = previousPlayer->posY;
            player.posZ = previousPlayer->posZ;
        }
    }
}

DrawGameData stabilizeRenderData(const DrawGameData& data) {
    static DrawGameData s_lastStableData;
    static Clock::time_point s_lastStableTime;
    static Clock::time_point s_lastReuseLogTime;
    static bool s_hasLastStableData = false;

    const auto now = Clock::now();
    constexpr auto kTransientHoldWindow = std::chrono::milliseconds(500);

    if (!data.inMatch) {
        // 退出对局时清空缓存的玩家数据, 防止 ESP 残留
        s_lastStableData = DrawGameData{};
        s_lastStableTime = now;
        s_hasLastStableData = false;
        return data;
    }

    DrawGameData stabilized = data;
    bool reusedPreviousFrame = false;
    const bool canReusePrevious = s_hasLastStableData
        && s_lastStableData.inMatch
        && now - s_lastStableTime <= kTransientHoldWindow;

    if (canReusePrevious) {
        if (!hasValidCameraPose(stabilized) && hasValidCameraPose(s_lastStableData)) {
            copyCameraState(s_lastStableData, stabilized);
            reusedPreviousFrame = true;
        }

        if (!hasValidWorldPoint(stabilized.myPosX, stabilized.myPosY, stabilized.myPosZ)
            && hasValidWorldPoint(s_lastStableData.myPosX, s_lastStableData.myPosY, s_lastStableData.myPosZ)) {
            stabilized.myPosX = s_lastStableData.myPosX;
            stabilized.myPosY = s_lastStableData.myPosY;
            stabilized.myPosZ = s_lastStableData.myPosZ;
            reusedPreviousFrame = true;
        }

        if (stabilized.players.empty() && !s_lastStableData.players.empty()) {
            stabilized.players = s_lastStableData.players;
            stabilized.aliveCount = s_lastStableData.aliveCount;
            stabilized.totalCount = s_lastStableData.totalCount;
            if (stabilized.myTeamID < 0) {
                stabilized.myTeamID = s_lastStableData.myTeamID;
            }
            reusedPreviousFrame = true;
        } else if (!stabilized.players.empty()) {
            backfillTransientPlayerState(stabilized, s_lastStableData);
        }
    }

    const bool shouldRefreshStableData = hasValidCameraPose(stabilized)
        || hasValidWorldPoint(stabilized.myPosX, stabilized.myPosY, stabilized.myPosZ)
        || !stabilized.players.empty();
    if (shouldRefreshStableData) {
        s_lastStableData = stabilized;
        s_lastStableTime = now;
        s_hasLastStableData = true;
    }

    // 超过稳定窗口仍无新数据时, 清空缓存避免绘制过时数据
    if (s_hasLastStableData && now - s_lastStableTime > std::chrono::milliseconds(2000)) {
        s_lastStableData.players.clear();
        s_lastStableData.aliveCount = 0;
        s_hasLastStableData = false;
    }

    if (reusedPreviousFrame && shouldLogEvery(s_lastReuseLogTime, std::chrono::milliseconds(1000))) {
        DLOG(LOG_LEVEL_INFO,
             "render stabilization reused previous snapshot: players=%zu alive=%d total=%d",
             stabilized.players.size(),
             stabilized.aliveCount,
             stabilized.totalCount);
    }

    return stabilized;
}

float sanitizeAngleDegrees(float value) {
    return isValidNumber(value) ? std::remainder(value, 360.0f) : 0.0f;
}

float sanitizeFov(float value) {
    if (!isValidNumber(value) || value < 30.0f || value > 170.0f) {
        return 90.0f;
    }
    return value;
}

bool tryGetViewPoint(const DrawGameData& data, ViewPoint& outPoint) {
    if (hasValidWorldPoint(data.camLocX, data.camLocY, data.camLocZ)) {
        outPoint = {data.camLocX, data.camLocY, data.camLocZ};
        return true;
    }
    if (hasValidWorldPoint(data.myPosX, data.myPosY, data.myPosZ)) {
        outPoint = {data.myPosX, data.myPosY, data.myPosZ};
        return true;
    }
    return false;
}

bool tryGetCameraOrigin(const DrawGameData& data, ViewPoint& outPoint) {
    if (!hasValidCameraPose(data)) {
        return false;
    }

    outPoint = {data.camLocX, data.camLocY, data.camLocZ};
    return true;
}

float distanceMeters(float x1, float y1, float z1, float x2, float y2, float z2) {
    const float dx = x1 - x2;
    const float dy = y1 - y2;
    const float dz = z1 - z2;
    return std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f;
}

float playerDistanceMeters(const DrawPlayerInfo& player, const ViewPoint& viewPoint) {
    return distanceMeters(player.posX, player.posY, player.posZ, viewPoint.x, viewPoint.y, viewPoint.z);
}

bool hasUsablePlayerPosition(const DrawPlayerInfo& player) {
    return hasValidWorldPoint(player.posX, player.posY, player.posZ);
}

bool transformWorldToCamera(const DrawGameData& data,
                            float wx,
                            float wy,
                            float wz,
                            CameraSpacePoint& outPoint);

constexpr size_t toBoneIndex(DrawBoneId bone) {
    return static_cast<size_t>(bone);
}

struct BoneSegment {
    DrawBoneId from;
    DrawBoneId to;
};

constexpr BoneSegment kSkeletonSegments[] = {
    {DrawBoneId::Pelvis, DrawBoneId::SpineLower},
    {DrawBoneId::SpineLower, DrawBoneId::SpineUpper},
    {DrawBoneId::SpineUpper, DrawBoneId::Neck},
    {DrawBoneId::Neck, DrawBoneId::Head},
    {DrawBoneId::SpineUpper, DrawBoneId::LeftUpperArm},
    {DrawBoneId::LeftUpperArm, DrawBoneId::LeftLowerArm},
    {DrawBoneId::LeftLowerArm, DrawBoneId::LeftHand},
    {DrawBoneId::SpineUpper, DrawBoneId::RightUpperArm},
    {DrawBoneId::RightUpperArm, DrawBoneId::RightLowerArm},
    {DrawBoneId::RightLowerArm, DrawBoneId::RightHand},
    {DrawBoneId::Pelvis, DrawBoneId::LeftThigh},
    {DrawBoneId::LeftThigh, DrawBoneId::LeftCalf},
    {DrawBoneId::LeftCalf, DrawBoneId::LeftFoot},
    {DrawBoneId::Pelvis, DrawBoneId::RightThigh},
    {DrawBoneId::RightThigh, DrawBoneId::RightCalf},
    {DrawBoneId::RightCalf, DrawBoneId::RightFoot},
};

bool hasBonePoint(const DrawPlayerInfo& player, DrawBoneId bone) {
    return (player.boneMask & (1u << static_cast<uint32_t>(bone))) != 0;
}

int countTrackedBones(const DrawPlayerInfo& player) {
    uint32_t mask = player.boneMask;
    int count = 0;
    while (mask != 0) {
        count += static_cast<int>(mask & 1u);
        mask >>= 1u;
    }
    return count;
}

bool projectBonePoint(const DrawGameData& data,
                      const DrawPlayerInfo& player,
                      DrawBoneId bone,
                      float screenW,
                      float screenH,
                      float& sx,
                      float& sy) {
    if (!hasBonePoint(player, bone)) {
        return false;
    }

    const DrawBonePoint& point = player.bones[toBoneIndex(bone)];

    CameraSpacePoint cameraPoint;
    if (!transformWorldToCamera(data, point.x, point.y, point.z, cameraPoint)) {
        return false;
    }

    ProjectionBasis basis;
    if (!buildProjectionBasis(data, screenW, screenH, basis)) {
        return false;
    }

    return projectCameraPoint(basis, cameraPoint, sx, sy);
}

int drawPlayerSkeleton(ImDrawList* drawList,
                       const DrawGameData& data,
                       const DrawPlayerInfo& player,
                       ImU32 color,
                       float screenW,
                       float screenH,
                       float lineThickness,
                       float jointRadius,
                       ImU32 jointColor) {
    // 至少需要 3 个骨骼点才绘制
    if (countTrackedBones(player) < 3) return 0;

    int segmentCount = 0;
    for (const BoneSegment& segment : kSkeletonSegments) {
        float fromX, fromY, toX, toY;
        if (!projectBonePoint(data, player, segment.from, screenW, screenH, fromX, fromY)
            || !projectBonePoint(data, player, segment.to, screenW, screenH, toX, toY))
            continue;

        float dx = toX - fromX, dy = toY - fromY;
        if (dx * dx + dy * dy > screenH * screenH) continue;

        drawList->AddLine(ImVec2(fromX, fromY), ImVec2(toX, toY), color, lineThickness);
        segmentCount++;
    }

    if (jointRadius >= 0.5f) {
        for (size_t boneIndex = 0; boneIndex < kTrackedBoneCount; ++boneIndex) {
            float px, py;
            if (!projectBonePoint(data, player, static_cast<DrawBoneId>(boneIndex), screenW, screenH, px, py))
                continue;
            drawList->AddCircleFilled(ImVec2(px, py), jointRadius, jointColor);
        }
    }

    return segmentCount;
}

bool transformWorldToCamera(const DrawGameData& data,
                            float wx,
                            float wy,
                            float wz,
                            CameraSpacePoint& outPoint) {
    ViewPoint cameraOrigin;
    if (!tryGetCameraOrigin(data, cameraOrigin)) {
        return false;
    }
    if (!isValidNumber(wx) || !isValidNumber(wy) || !isValidNumber(wz)) {
        return false;
    }

    constexpr float DEG2RAD = 3.14159265358979f / 180.0f;
    const float pitch = sanitizeAngleDegrees(data.camPitch) * DEG2RAD;
    const float yaw = sanitizeAngleDegrees(data.camYaw) * DEG2RAD;
    const float roll = sanitizeAngleDegrees(data.camRoll) * DEG2RAD;

    const float sp = std::sin(pitch);
    const float cp = std::cos(pitch);
    const float sy = std::sin(yaw);
    const float cy = std::cos(yaw);
    const float sr = std::sin(roll);
    const float cr = std::cos(roll);

    const float axisXx = cp * cy;
    const float axisXy = cp * sy;
    const float axisXz = sp;

    const float axisYx = sr * sp * cy - cr * sy;
    const float axisYy = sr * sp * sy + cr * cy;
    const float axisYz = -sr * cp;

    const float axisZx = -(cr * sp * cy + sr * sy);
    const float axisZy = cy * sr - cr * sp * sy;
    const float axisZz = cr * cp;

    const float dx = wx - cameraOrigin.x;
    const float dy = wy - cameraOrigin.y;
    const float dz = wz - cameraOrigin.z;

    outPoint.x = dx * axisYx + dy * axisYy + dz * axisYz;
    outPoint.y = dx * axisZx + dy * axisZy + dz * axisZz;
    outPoint.z = dx * axisXx + dy * axisXy + dz * axisXz;

    return isValidNumber(outPoint.x) && isValidNumber(outPoint.y) && isValidNumber(outPoint.z);
}

float playerHealthRatio(const DrawPlayerInfo& player) {
    if (player.healthMax <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(player.health / player.healthMax, 0.0f, 1.0f);
}

const char* playerLabel(const DrawPlayerInfo& player) {
    if (!player.playerName.empty()) {
        return player.playerName.c_str();
    }
    return player.isAI ? "AI" : "Enemy";
}

void drawTouchPointOverlay(bool enabled, float screenW, float screenH) {
    if (!enabled) {
        return;
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (!io.MouseDown[0] || !ImGui::IsMousePosValid()) {
        return;
    }

    const float touchX = std::clamp(io.MousePos.x, 0.0f, screenW);
    const float touchY = std::clamp(io.MousePos.y, 0.0f, screenH);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    drawList->AddCircleFilled(ImVec2(touchX, touchY), 10.0f, IM_COL32(255, 80, 80, 230));
    drawList->AddCircle(ImVec2(touchX, touchY), 18.0f, IM_COL32(255, 255, 255, 220), 0, 2.0f);
}

bool projectFallbackArrow(const DrawGameData& data,
                          const DrawPlayerInfo& player,
                          float screenW,
                          float screenH,
                          float& arrowX,
                          float& arrowY,
                          float& angleRad) {
    CameraSpacePoint cameraPoint;
    if (!transformWorldToCamera(data, player.posX, player.posY, player.posZ + 90.0f, cameraPoint)) {
        return false;
    }

    constexpr float kNearDepth = 1.0f;
    constexpr float kMarginX = 40.0f;
    constexpr float kMarginY = 60.0f;

    ProjectionBasis basis;
    if (!buildProjectionBasis(data, screenW, screenH, basis)) {
        return false;
    }

    const bool behindCamera = cameraPoint.z <= kNearDepth;
    const float safeDepth = std::max(std::fabs(cameraPoint.z), kNearDepth);
    const ProjectionViewport& viewport = basis.viewport;

    float screenDx = cameraPoint.x * basis.focalLength / safeDepth;
    float screenDy = -cameraPoint.y * basis.focalLength / safeDepth;
    if (behindCamera) {
        screenDx = -screenDx;
        screenDy = -screenDy;
    }

    if (!isValidNumber(screenDx) || !isValidNumber(screenDy)) {
        return false;
    }
    if (std::fabs(screenDx) < 0.5f && std::fabs(screenDy) < 0.5f) {
        screenDy = behindCamera ? 1.0f : -1.0f;
    }

    const float halfW = std::max(viewport.width * 0.5f - kMarginX, 1.0f);
    const float halfH = std::max(viewport.height * 0.5f - kMarginY, 1.0f);
    const float scale = 1.0f / std::max(std::fabs(screenDx) / halfW, std::fabs(screenDy) / halfH);

    arrowX = std::clamp(viewport.left + viewport.width * 0.5f + screenDx * scale,
                        viewport.left + kMarginX,
                        viewport.left + viewport.width - kMarginX);
    arrowY = std::clamp(viewport.top + viewport.height * 0.5f + screenDy * scale,
                        viewport.top + kMarginY,
                        viewport.top + viewport.height - kMarginY);
    angleRad = std::atan2(screenDy, screenDx);
    return isValidNumber(arrowX) && isValidNumber(arrowY) && isValidNumber(angleRad);
}
} // namespace

// =====================================================================
//  工具函数
// =====================================================================
ImU32 UE4Overlay::hpColor(float ratio) {
    if (ratio > 0.5f) return IM_COL32(0, 255, 0, 255);       // 绿
    if (ratio > 0.25f) return IM_COL32(255, 255, 0, 255);     // 黄
    return IM_COL32(255, 0, 0, 255);                           // 红
}

float UE4Overlay::distance3D(float x1, float y1, float z1, float x2, float y2, float z2) {
    float dx = x1 - x2, dy = y1 - y2, dz = z1 - z2;
    return std::sqrt(dx*dx + dy*dy + dz*dz);
}

// =====================================================================
//  主绘制入口
// =====================================================================
void UE4Overlay::drawOverlay(const DrawGameData& data) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) {
        return;
    }

    // 数据源存活检测: 超过 5 秒无 pushData 则清除 ESP (游戏进程可能已死)
    const int64_t staleness = SharedUE4Data::getInstance().msSinceLastPush();
    const bool dataSourceStale = (staleness > 5000);

    const DrawGameData& inputData = data;
    DrawGameData staleOverride;
    const DrawGameData* renderInput = &inputData;
    if (dataSourceStale && inputData.inMatch) {
        staleOverride = inputData;
        staleOverride.inMatch = false;
        staleOverride.players.clear();
        staleOverride.aliveCount = 0;
        renderInput = &staleOverride;
        static Clock::time_point s_lastStaleLog;
        if (shouldLogEvery(s_lastStaleLog, std::chrono::milliseconds(3000))) {
            DLOG(LOG_LEVEL_INFO, "data source stale (%lldms), clearing overlay", (long long)staleness);
        }
    }

    const DrawGameData renderData = stabilizeRenderData(*renderInput);
    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;
    int espDrawCount = 0;
    int minimapDrawCount = 0;

    static bool s_hasLastMatchState = false;
    static bool s_lastInMatch = false;
    static Clock::time_point s_lastSummaryLog;

    if (!s_hasLastMatchState || s_lastInMatch != renderData.inMatch) {
        DLOG(LOG_LEVEL_INFO,
             "match state changed: inMatch=%d world=%s state=%s alive=%d/%d players=%zu",
             renderData.inMatch ? 1 : 0,
             renderData.worldName.c_str(),
             renderData.matchState.c_str(),
             renderData.aliveCount,
             renderData.totalCount,
             renderData.players.size());
        s_lastInMatch = renderData.inMatch;
        s_hasLastMatchState = true;
    }

    // 对局中时绘制 ESP 和小地图
    if (renderData.inMatch) {
        if (m_enableESP) {
            espDrawCount = drawESP(renderData, screenW, screenH);
        }
        if (m_enableItemESP || m_enableWeaponESP || m_enableVehicleESP || m_enableAirdropESP || m_enableDeathBoxESP) {
            drawWorldObjects(renderData, screenW, screenH);
        }
        if (m_enableMinimap) {
            minimapDrawCount = drawMinimap(renderData, screenW, screenH);
        }

        if (shouldLogEvery(s_lastSummaryLog, std::chrono::milliseconds(2000))) {
            ProjectionBasis basis;
            const bool hasProjectionBasis = buildProjectionBasis(renderData, screenW, screenH, basis);
            const ProjectionViewport viewport = hasProjectionBasis ? basis.viewport : getProjectionViewport(screenW, screenH);
            DLOG(LOG_LEVEL_INFO,
                 "overlay summary: screen=%.0fx%.0f viewport=(%.0f,%.0f %.0fx%.0f) projW=%.0f focal=%.1f tracked=%zu alive=%d/%d esp=%d minimap=%d cam=(%.0f, %.0f, %.0f) fov=%.1f",
                 screenW,
                 screenH,
                 viewport.left,
                 viewport.top,
                 viewport.width,
                 viewport.height,
                 hasProjectionBasis ? basis.projectionWidth : 0.0f,
                 hasProjectionBasis ? basis.focalLength : 0.0f,
                 renderData.players.size(),
                 renderData.aliveCount,
                 renderData.totalCount,
                 espDrawCount,
                 minimapDrawCount,
                 renderData.camLocX,
                 renderData.camLocY,
                 renderData.camLocZ,
                 renderData.camFOV);
        }
    }

    drawTouchPointOverlay(m_enableTouchPoint, screenW, screenH);

    // AI 屏幕检测 (始终运行, 与对局状态无关)
    if (m_enableAIDetect) {
        static int s_aiCallCnt = 0;
        if ((++s_aiCallCnt % 120) == 1) {
            LOG(LOG_LEVEL_INFO, "[AI/Draw] drawAIDetections call#%d screen=%.0fx%.0f", s_aiCallCnt, screenW, screenH);
        }
        drawAIDetections(screenW, screenH);
    }

    // 控制菜单始终显示
    drawMenu(renderData);
}

// =====================================================================
//  控制菜单
// =====================================================================
void UE4Overlay::drawMenu(const DrawGameData& data) {
    // 与 DFM 一致: FirstUseEver — Always 会每帧强制覆盖位置,
    // 让 ImGui widget 的 active-id 命中测试出现一帧错位 (菜单看似可见但点不动).
    // AImGui 已设置 IniFilename=nullptr, 不存在 ini 持久化的屏外坐标问题.
    // 折叠↔展开切换时使用 m_menuLastPos 强制定位, 保证两窗口位置完全一致 (用户拖动后位置也保留).
    if (m_menuPosCaptured) {
        ImGui::SetNextWindowPos(m_menuLastPos, ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    }
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.8f);

    // 收起态: 仅渲染一个小标题条 + 展开按钮 (类似 DFM)
    if (m_menuCollapsed) {
        ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_AlwaysAutoResize;
        if (ImGui::Begin("##PubgMenuMini", nullptr, fl)) {
            // 实时记录拖动后的位置, 下次展开/重新折叠都跟随
            m_menuLastPos = ImGui::GetWindowPos();
            m_menuPosCaptured = true;

            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "PUBG");
            ImGui::SameLine();
            // 当前人数 (折叠态简化显示)
            if (data.inMatch) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%d/%d", data.aliveCount, data.totalCount);
            } else {
                ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "--");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("展开 ▼")) {
                m_menuCollapsed = false;
            }
        }
        ImGui::End();
        return;
    }

    // 注: p_open=nullptr 去掉关闭按钮, 防止用户误点导致菜单永久消失
    if (ImGui::Begin("PUBG 绘制", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // 实时记录展开态位置, 折叠时小窗口出现在同位置
        m_menuLastPos = ImGui::GetWindowPos();
        m_menuPosCaptured = true;

        // 标题栏旁的收起按钮 (DFM 风格)
        if (ImGui::SmallButton("收起 ▲")) {
            m_menuCollapsed = true;
            ImGui::End();
            return;
        }
        bool settingsChanged = false;

        // 对局状态
        if (data.inMatch) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "★ 对局中");
            ImGui::SameLine();
            ImGui::Text("玩家: %d/%d", data.aliveCount, data.totalCount);
            ImGui::Text("地图: %s", data.worldName.c_str());
            ImGui::Text("状态: %s", data.matchState.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "○ 等待对局...");
        }
        ImGui::Separator();

        // 功能开关
        settingsChanged |= ImGui::Checkbox("ESP 方框", &m_enableESP);
        settingsChanged |= ImGui::Checkbox("骨架线", &m_enableSkeleton);
        if (m_enableSkeleton) {
            settingsChanged |= ImGui::ColorEdit3("骨架颜色", m_skeletonColor,
                                                 ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
            ImGui::SameLine();
            ImGui::TextUnformatted("颜色");
            settingsChanged |= ImGui::SliderFloat("骨架粗细", &m_skeletonThickness, 0.5f, 5.0f, "%.1f");
            settingsChanged |= ImGui::SliderFloat("关节大小", &m_skelJointRadius, 0.0f, 8.0f, "%.1f");
            settingsChanged |= ImGui::Checkbox("远距离自动加粗", &m_skeletonAutoScale);
        }
        settingsChanged |= ImGui::Checkbox("射线", &m_enableSnapline);
        settingsChanged |= ImGui::Checkbox("血条", &m_enableHP);
        settingsChanged |= ImGui::Checkbox("名字", &m_enableName);
        settingsChanged |= ImGui::Checkbox("距离", &m_enableDistance);
        settingsChanged |= ImGui::Checkbox("显示队友", &m_enableTeammate);
        ImGui::Separator();

#if defined(AI_OVERLAY_AVAILABLE)
        if (ImGui::Checkbox("AI 屏幕检测", &m_enableAIDetect)) {
            settingsChanged = true;
            ai_overlay::AISharedData::getInstance().setEnabled(m_enableAIDetect);
        }
        if (m_enableAIDetect) {
            auto& shd = ai_overlay::AISharedData::getInstance();
            if (!shd.ready()) {
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                   "● 模型未就绪 (%s)",
                                   shd.lastError().empty() ? "等待加载" : shd.lastError().c_str());
            } else {
                int64_t age = shd.msSinceLastPush();
                int64_t us  = shd.lastInferUs();
                if (age < 0 || age > 1500) {
                    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f),
                                       "● 等待截屏 (%lldms)", (long long)age);
                } else {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f),
                                       "● 推理 %.1f ms / 帧龄 %lldms",
                                       us / 1000.0f, (long long)age);
                }
            }
            settingsChanged |= ImGui::SliderFloat("置信度阈值", &m_aiScoreThr, 0.10f, 0.90f, "%.2f");
            settingsChanged |= ImGui::SliderInt("推理间隔", &m_aiIntervalMs, 60, 500, "%d ms");
            settingsChanged |= ImGui::Checkbox("只看人物", &m_aiOnlyPerson);
            settingsChanged |= ImGui::ColorEdit3("AI框颜色", m_aiBoxColor,
                                                 ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
            ImGui::SameLine();
            ImGui::TextUnformatted("AI 框颜色");
            settingsChanged |= ImGui::SliderFloat("AI框粗细", &m_aiBoxThickness, 1.0f, 5.0f, "%.1f");
            settingsChanged |= ImGui::Checkbox("显示置信度", &m_aiDrawScore);
        }

        // ── AI 辅助瞄准 ──
        if (ImGui::Checkbox("AI 辅助瞄准", &m_aiAimEnable)) {
            settingsChanged = true;
            ai_overlay::AimAssist::getInstance().setEnabled(m_aiAimEnable);
        }
        if (m_aiAimEnable) {
            if (ImGui::Checkbox("仅视觉锁定 (不注入触屏)", &m_aiAimVisualOnly)) {
                settingsChanged = true;
                // 切换 visual <-> inject 模式需要重启 AimAssist 注入线程
                ai_overlay::AimAssist::getInstance().setEnabled(false);
                ai_overlay::AimAssist::getInstance().setVisualOnly(m_aiAimVisualOnly);
                ai_overlay::AimAssist::getInstance().setEnabled(true);
            }
            if (!m_aiAimVisualOnly) {
                bool ready = ai_overlay::AimAssist::getInstance().injectorReady();
                if (ready) {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "● 触屏注入器就绪");
                } else {
                    auto err = ai_overlay::AimAssist::getInstance().injectorError();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                       "● 注入器未就绪: %s", err.empty() ? "等待初始化" : err.c_str());
                }
            }
            settingsChanged |= ImGui::SliderFloat("FOV 半径", &m_aiAimFovRadius, 50.0f, 600.0f, "%.0f px");
            settingsChanged |= ImGui::SliderFloat("头部位置", &m_aiAimHeadRatio, 0.0f, 0.5f, "%.2f");
            settingsChanged |= ImGui::SliderInt("最低置信度%", &m_aiAimMinScore, 10, 90);
            settingsChanged |= ImGui::Checkbox("只锁定人物##aim", &m_aiAimOnlyPerson);
            if (!m_aiAimVisualOnly) {
                settingsChanged |= ImGui::SliderFloat("X 灵敏度", &m_aiAimSensitivityX, 0.1f, 3.0f, "%.2f");
                settingsChanged |= ImGui::SliderFloat("Y 灵敏度", &m_aiAimSensitivityY, 0.1f, 3.0f, "%.2f");
                settingsChanged |= ImGui::SliderFloat("平滑", &m_aiAimSmoothing, 0.0f, 0.95f, "%.2f");
                settingsChanged |= ImGui::Checkbox("需要按住触发键", &m_aiAimRequireTrigger);
                if (m_aiAimRequireTrigger) {
                    ImVec2 sz(80, 26);
                    ImGui::Button("按住开火", sz);
                    bool held = ImGui::IsItemActive();
                    ai_overlay::AimAssist::getInstance().setTrigger(held);
                    ImGui::SameLine();
                    ImGui::TextUnformatted(held ? "瞄准中" : "(松开)");
                } else {
                    ai_overlay::AimAssist::getInstance().setTrigger(true);
                }
            }
        }
        ImGui::Separator();
#endif

        settingsChanged |= ImGui::Checkbox("小地图", &m_enableMinimap);
        if (m_enableMinimap) {
            settingsChanged |= ImGui::SliderFloat("地图大小", &m_minimapSize, 100.0f, 400.0f, "%.0f");
            settingsChanged |= ImGui::SliderFloat("雷达范围", &m_minimapRangeMeters, 60.0f, 500.0f, "%.0f m");
            settingsChanged |= ImGui::Checkbox("锁定雷达位置", &m_minimapLocked);
            ImGui::SameLine();
            if (ImGui::SmallButton("重置")) {
                m_minimapPosX = ue4draw::kMinimapPosUnset;
                m_minimapPosY = ue4draw::kMinimapPosUnset;
                settingsChanged = true;
            }
            if (!m_minimapLocked) {
                const ImGuiIO& io = ImGui::GetIO();
                // 滑块允许负数: 雷达可部分移出屏幕 (例如贴左上贴边或溢出)
                // 范围 = [-mapSize+50, displaySize-50]: 至少保留 50px 在屏内可见
                float minX = -m_minimapSize + 50.0f;
                float minY = -m_minimapSize + 50.0f;
                float maxX = std::max(io.DisplaySize.x - 50.0f, minX + 1.0f);
                float maxY = std::max(io.DisplaySize.y - 50.0f, minY + 1.0f);
                float curX = (m_minimapPosX <= ue4draw::kMinimapPosUnset) ? (io.DisplaySize.x - m_minimapSize - 15.0f) : m_minimapPosX;
                float curY = (m_minimapPosY <= ue4draw::kMinimapPosUnset) ? 15.0f : m_minimapPosY;
                if (ImGui::SliderFloat("雷达X", &curX, minX, maxX, "%.0f")) { m_minimapPosX = curX; settingsChanged = true; }
                if (ImGui::SliderFloat("雷达Y", &curY, minY, maxY, "%.0f")) { m_minimapPosY = curY; settingsChanged = true; }
                ImGui::TextDisabled("提示: 解锁后可直接拖拽雷达, 也可用 X/Y 滑块; 锁定后位置固定");
            }
        }
        settingsChanged |= ImGui::Checkbox("边缘箭头", &m_enableFallbackESP);
        settingsChanged |= ImGui::Checkbox("玩家列表", &m_enablePlayerList);
        settingsChanged |= ImGui::Checkbox("触点", &m_enableTouchPoint);

        ImGui::Separator();
        ImGui::TextUnformatted("场景物件");
        settingsChanged |= ImGui::Checkbox("场景物资", &m_enableItemESP);
        settingsChanged |= ImGui::Checkbox("地面枪械", &m_enableWeaponESP);
        settingsChanged |= ImGui::Checkbox("载具显示", &m_enableVehicleESP);
        settingsChanged |= ImGui::Checkbox("空投显示", &m_enableAirdropESP);
        settingsChanged |= ImGui::Checkbox("死亡箱显示", &m_enableDeathBoxESP);
        settingsChanged |= ImGui::Checkbox("透视箱内物品", &m_enableBoxContentESP);
        if (m_enableItemESP || m_enableWeaponESP || m_enableVehicleESP || m_enableAirdropESP || m_enableDeathBoxESP) {
            settingsChanged |= ImGui::SliderFloat("物件距离", &m_worldObjMaxDist, 50.0f, 800.0f, "%.0f m");
        }
        ImGui::Separator();
        {
            bool aimbotOn = m_enableAimbot;
            if (ImGui::Checkbox("锁定目标 (开镜自瞄)", &aimbotOn)) {
                m_enableAimbot = aimbotOn;
                ue4draw::SharedUE4Data::getInstance().setAimbotEnabled(aimbotOn);
                settingsChanged = true;
            }
        }
        ImGui::Separator();
        settingsChanged |= ImGui::SliderFloat("最大距离", &m_espMaxDist, 100.0f, 2000.0f, "%.0f m");

        // 恢复内存按钮 (对局中显示)
        if (data.inMatch) {
            ImGui::Separator();
            const bool alreadyRestored = ue4draw::SharedUE4Data::getInstance().isMemoryRestored();
            if (alreadyRestored) {
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "✓ 内存已恢复 (安全)");
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.3f, 0.1f, 1.0f));
                if (ImGui::Button("恢复游戏数据 (结算前点击)", ImVec2(280.0f, 36.0f))) {
                    ue4draw::SharedUE4Data::getInstance().requestRestore();
                }
                ImGui::PopStyleColor(2);
            }
        }

        if (data.inMatch) {
            int playersWithBones = 0;
            int totalBonePoints = 0;
            for (const auto& player : data.players) {
                const int boneCount = countTrackedBones(player);
                if (boneCount > 0) {
                    playersWithBones++;
                    totalBonePoints += boneCount;
                }
            }

            ImGui::Text("ESP 绘制: 精确 %d  箭头 %d", m_lastPreciseESP, m_lastFallbackESP);
            ImGui::Text("骨骼数据: %d/%zu 玩家  点位 %d", playersWithBones, data.players.size(), totalBonePoints);
        }
        if (m_enableTouchPoint) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.MouseDown[0] && ImGui::IsMousePosValid()) {
                ImGui::Text("触点: %.0f, %.0f", io.MousePos.x, io.MousePos.y);
            } else {
                ImGui::TextDisabled("触点: 空闲");
            }
        }

        if (m_enablePlayerList && data.inMatch && ImGui::CollapsingHeader("追踪玩家", ImGuiTreeNodeFlags_DefaultOpen)) {
            ViewPoint viewPoint;
            const bool hasViewPoint = tryGetViewPoint(data, viewPoint);
            std::vector<const DrawPlayerInfo*> trackedPlayers;
            trackedPlayers.reserve(data.players.size());

            for (const auto& player : data.players) {
                if (!player.isAlive) continue;
                if (player.isTeammate && !m_enableTeammate) continue;
                trackedPlayers.push_back(&player);
            }

            if (hasViewPoint) {
                std::sort(trackedPlayers.begin(), trackedPlayers.end(), [&](const DrawPlayerInfo* lhs, const DrawPlayerInfo* rhs) {
                    return playerDistanceMeters(*lhs, viewPoint) < playerDistanceMeters(*rhs, viewPoint);
                });
            }

            const int visibleCount = std::min<int>(static_cast<int>(trackedPlayers.size()), 6);
            for (int index = 0; index < visibleCount; ++index) {
                const DrawPlayerInfo& player = *trackedPlayers[index];
                const float hpRatio = playerHealthRatio(player);
                const ImVec4 hpBarColor = hpRatio > 0.5f ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f)
                    : (hpRatio > 0.25f ? ImVec4(0.9f, 0.9f, 0.2f, 1.0f) : ImVec4(0.9f, 0.2f, 0.2f, 1.0f));

                char header[128];
                if (hasViewPoint) {
                    snprintf(header, sizeof(header), "%s [T%d] %.0fm",
                             playerLabel(player),
                             player.teamID,
                             playerDistanceMeters(player, viewPoint));
                } else {
                    snprintf(header, sizeof(header), "%s [T%d]",
                             playerLabel(player),
                             player.teamID);
                }
                ImGui::TextUnformatted(header);

                char hpOverlay[64];
                snprintf(hpOverlay, sizeof(hpOverlay), "%.0f / %.0f HP", player.health, player.healthMax);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hpBarColor);
                ImGui::ProgressBar(hpRatio, ImVec2(220.0f, 0.0f), hpOverlay);
                ImGui::PopStyleColor();

                char coordLine[128];
                snprintf(coordLine, sizeof(coordLine), "Pos: %.0f, %.0f, %.0f%s  Bones:%d/%d",
                         player.posX,
                         player.posY,
                         player.posZ,
                         player.isAI ? "  AI" : "",
                         countTrackedBones(player),
                         static_cast<int>(kTrackedBoneCount));
                ImGui::TextDisabled("%s", coordLine);

                if (index + 1 < visibleCount) {
                    ImGui::Separator();
                }
            }
        }

        if (settingsChanged) {
            DLOG(LOG_LEVEL_INFO,
                 "settings updated: esp=%s skeleton=%s snap=%s hp=%s name=%s dist=%s teammate=%s minimap=%s edgeArrow=%s playerList=%s touch=%s mapSize=%.0f mapRange=%.0f maxDist=%.0f",
                 onOff(m_enableESP),
                 onOff(m_enableSkeleton),
                 onOff(m_enableSnapline),
                 onOff(m_enableHP),
                 onOff(m_enableName),
                 onOff(m_enableDistance),
                 onOff(m_enableTeammate),
                 onOff(m_enableMinimap),
                 onOff(m_enableFallbackESP),
                 onOff(m_enablePlayerList),
                 onOff(m_enableTouchPoint),
                 m_minimapSize,
                 m_minimapRangeMeters,
                 m_espMaxDist);
        }
    }
    ImGui::End();
}

// =====================================================================
//  WorldToScreen — UE4 3D→2D 投影 (使用相机位置/旋转/FOV)
//  基于 PlayerCameraManager.CameraCache.POV
// =====================================================================
bool UE4Overlay::worldToScreen(const DrawGameData& cam,
                               float wx, float wy, float wz,
                               float screenW, float screenH,
                               float& sx, float& sy) {
    CameraSpacePoint cameraPoint;
    if (!transformWorldToCamera(cam, wx, wy, wz, cameraPoint)) {
        return false;
    }

    ProjectionBasis basis;
    if (!buildProjectionBasis(cam, screenW, screenH, basis)) {
        return false;
    }

    return projectCameraPoint(basis, cameraPoint, sx, sy);
}

// =====================================================================
//  ESP 方框绘制 (使用真实 WorldToScreen 投影)
// =====================================================================
int UE4Overlay::drawESP(const DrawGameData& data, float screenW, float screenH) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##UE4ESP", nullptr,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float myCx = screenW * 0.5f;
    float myCy = screenH;
    int preciseRenderedCount = 0;
    int fallbackRenderedCount = 0;

    ViewPoint viewPoint;
    const bool hasViewPoint = tryGetViewPoint(data, viewPoint);
    const bool hasPreciseCamera = hasValidCameraPose(data);

    for (const auto& p : data.players) {
        if (!p.isAlive) continue;
        if (p.isTeammate && !m_enableTeammate) continue;
        if (!hasUsablePlayerPosition(p)) continue;

        if (!hasViewPoint) continue;

        const float dist = playerDistanceMeters(p, viewPoint);
        if (dist > m_espMaxDist || dist < 1.0f) continue;

        const ImU32 boxColor = p.isTeammate ? IM_COL32(0, 200, 0, 200) : IM_COL32(255, 50, 50, 200);
        const float hpRatio = playerHealthRatio(p);
        const int boneCount = countTrackedBones(p);
        bool rendered = false;
        bool preciseRendered = false;

        if (hasPreciseCamera && m_enableSkeleton && p.boneMask != 0) {
            // 骨架颜色 (用户在菜单设置)
            const ImU32 skelLineColor = IM_COL32(
                std::clamp(int(m_skeletonColor[0] * 255.0f + 0.5f), 0, 255),
                std::clamp(int(m_skeletonColor[1] * 255.0f + 0.5f), 0, 255),
                std::clamp(int(m_skeletonColor[2] * 255.0f + 0.5f), 0, 255),
                235);
            const ImU32 skelJointColor = (skelLineColor & 0x00FFFFFF) | (200u << 24);
            // 远距离自动加粗: 100m 用基础粗细, 300m+ 加 1.5 倍, 500m+ 加 2 倍
            float thickness = m_skeletonThickness;
            float jointR    = m_skelJointRadius;
            if (m_skeletonAutoScale) {
                float scale = 1.0f;
                if      (dist >= 500.0f) scale = 2.2f;
                else if (dist >= 300.0f) scale = 1.7f;
                else if (dist >= 150.0f) scale = 1.3f;
                thickness = std::max(1.0f, m_skeletonThickness * scale);
                jointR    = std::max(2.0f, m_skelJointRadius   * scale);
            }
            if (drawPlayerSkeleton(dl, data, p, skelLineColor, screenW, screenH,
                                   thickness, jointR, skelJointColor) > 0) {
                rendered = true;
                preciseRendered = true;
            }
        }

        if (hasPreciseCamera) {
            constexpr float kCharacterHalfHeight = 88.0f;
            float footSX = 0.0f;
            float footSY = 0.0f;
            float headSX = 0.0f;
            float headSY = 0.0f;
            float boxLeft = 0.0f;
            float boxRight = 0.0f;
            float topY = 0.0f;
            float botY = 0.0f;
            bool hasBox = false;

            if (p.boneMask != 0) {
                float minX = screenW;
                float maxX = 0.0f;
                float minY = screenH;
                float maxY = 0.0f;
                int projectedBones = 0;
                for (size_t boneIndex = 0; boneIndex < kTrackedBoneCount; ++boneIndex) {
                    float boneX = 0.0f;
                    float boneY = 0.0f;
                    if (!projectBonePoint(data, p, static_cast<DrawBoneId>(boneIndex), screenW, screenH, boneX, boneY)) {
                        continue;
                    }
                    if (boneX < -screenW || boneX > screenW * 2.0f || boneY < -screenH || boneY > screenH * 2.0f) {
                        continue;
                    }
                    minX = std::min(minX, boneX);
                    maxX = std::max(maxX, boneX);
                    minY = std::min(minY, boneY);
                    maxY = std::max(maxY, boneY);
                    projectedBones++;
                }
                if (projectedBones >= 3) {
                    const float boneBoxH = std::max(maxY - minY, 12.0f);
                    const float boneBoxW = std::max(maxX - minX, boneBoxH * 0.32f);
                    const float padX = std::clamp(boneBoxW * 0.20f, 4.0f, 18.0f);
                    const float padTop = std::clamp(boneBoxH * 0.12f, 4.0f, 20.0f);
                    const float padBottom = std::clamp(boneBoxH * 0.08f, 3.0f, 16.0f);
                    boxLeft = minX - padX;
                    boxRight = maxX + padX;
                    topY = minY - padTop;
                    botY = maxY + padBottom;
                    hasBox = true;
                }
            }

            if (!hasBox
                && worldToScreen(data, p.posX, p.posY, p.posZ - kCharacterHalfHeight, screenW, screenH, footSX, footSY)
                && worldToScreen(data, p.posX, p.posY, p.posZ + kCharacterHalfHeight, screenW, screenH, headSX, headSY)) {
                const float rootBoxH = std::fabs(footSY - headSY);
                const float rootBoxW = rootBoxH * 0.48f;
                const float rootCx = (footSX + headSX) * 0.5f;
                boxLeft = rootCx - rootBoxW / 2.0f;
                boxRight = rootCx + rootBoxW / 2.0f;
                topY = std::min(footSY, headSY);
                botY = std::max(footSY, headSY);
                hasBox = true;
            }

            if (hasBox) {
                const float boxH = botY - topY;
                const float boxW = boxRight - boxLeft;
                const float cx = (boxLeft + boxRight) * 0.5f;

                if (boxH >= 10.0f && boxH <= screenH * 0.9f
                    && cx >= -boxW && cx <= screenW + boxW
                    && topY >= -boxH && botY <= screenH + boxH) {
                    rendered = true;
                    preciseRendered = true;

                    dl->AddRect(ImVec2(boxLeft, topY), ImVec2(boxRight, botY),
                                boxColor, 0, 0, 2.0f);

                    if (m_enableSnapline) {
                        dl->AddLine(ImVec2(myCx, myCy), ImVec2(cx, botY),
                                    IM_COL32(255, 255, 255, 100), 1.0f);
                    }

                    if (m_enableHP) {
                        const float hpX = boxLeft - 5.0f;
                        const float hpFill = topY + (botY - topY) * (1.0f - hpRatio);
                        dl->AddRectFilled(ImVec2(hpX - 3, topY), ImVec2(hpX, botY),
                                          IM_COL32(0, 0, 0, 150));
                        dl->AddRectFilled(ImVec2(hpX - 3, hpFill), ImVec2(hpX, botY),
                                          hpColor(hpRatio));
                    }

                    if (m_enableName) {
                        const char* label = playerLabel(p);
                        ImVec2 textSize = ImGui::CalcTextSize(label);
                        dl->AddText(ImVec2(cx - textSize.x / 2, topY - textSize.y - 2),
                                    IM_COL32(255, 255, 255, 230), label);
                    }

                    if (m_enableSkeleton) {
                        char boneBuf[32];
                        snprintf(boneBuf, sizeof(boneBuf), "B:%d", boneCount);
                        const ImU32 boneColor = boneCount > 0 ? IM_COL32(80, 255, 255, 230) : IM_COL32(255, 210, 80, 230);
                        dl->AddText(ImVec2(boxRight + 6.0f, topY), boneColor, boneBuf);
                    }

                    if (m_enableDistance) {
                        char distBuf[32];
                        snprintf(distBuf, sizeof(distBuf), "%.0fm", dist);
                        ImVec2 textSize = ImGui::CalcTextSize(distBuf);
                        dl->AddText(ImVec2(cx - textSize.x / 2, botY + 2),
                                    IM_COL32(200, 200, 200, 200), distBuf);
                    }
                }
            }
        }

        if (preciseRendered) {
            preciseRenderedCount++;
        }

        if (!rendered && m_enableFallbackESP && hasPreciseCamera) {
            float arrowX = 0.0f;
            float arrowY = 0.0f;
            float angleRad = 0.0f;
            if (projectFallbackArrow(data, p, screenW, screenH, arrowX, arrowY, angleRad)) {
                fallbackRenderedCount++;

                const ImVec2 tip(arrowX, arrowY);
                const ImVec2 forward(std::cos(angleRad), std::sin(angleRad));
                const ImVec2 side(-forward.y, forward.x);
                const float arrowLength = 18.0f;
                const float arrowWidth = 14.0f;
                const ImVec2 baseCenter(tip.x - forward.x * arrowLength,
                                        tip.y - forward.y * arrowLength);
                const ImVec2 baseLeft(baseCenter.x + side.x * (arrowWidth * 0.5f),
                                      baseCenter.y + side.y * (arrowWidth * 0.5f));
                const ImVec2 baseRight(baseCenter.x - side.x * (arrowWidth * 0.5f),
                                       baseCenter.y - side.y * (arrowWidth * 0.5f));

                dl->AddTriangleFilled(tip, baseLeft, baseRight, boxColor);
                dl->AddTriangle(tip, baseLeft, baseRight, IM_COL32(0, 0, 0, 220), 1.5f);
            }
        }
    }

    m_lastPreciseESP = preciseRenderedCount;
    m_lastFallbackESP = fallbackRenderedCount;

    ImGui::End();
    return preciseRenderedCount + fallbackRenderedCount;
}

// =====================================================================
//  场景物件绘制 (物资 / 枪械 / 载具 / 空投)
// =====================================================================
int UE4Overlay::drawWorldObjects(const DrawGameData& data, float screenW, float screenH) {
    if (data.worldObjects.empty()) return 0;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##UE4WorldObjs", nullptr,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    int rendered = 0;
    const float maxDistCm = m_worldObjMaxDist * 100.0f;
    const float camX = data.camLocX, camY = data.camLocY, camZ = data.camLocZ;

    // 用于堆叠相同位置的标签 (透视箱内物品时一个箱子里的所有 PickUp 重叠在同一屏幕坐标)
    std::unordered_map<int64_t, int> stackSlot;

    for (const auto& obj : data.worldObjects) {
        // 箱内物品: 只在透视开关开启时显示
        if (obj.inBox && !m_enableBoxContentESP) continue;

        bool enabled = false;
        ImU32 color = IM_COL32(255, 255, 255, 255);
        const char* prefix = "";
        switch (obj.kind) {
            case ue4draw::DrawWorldObjectKind::Item:
                if (!m_enableItemESP && !obj.inBox) continue;
                enabled = true; color = IM_COL32(0, 220, 255, 230); prefix = "物"; break;
            case ue4draw::DrawWorldObjectKind::Weapon:
                if (!m_enableWeaponESP && !obj.inBox) continue;
                enabled = true; color = IM_COL32(255, 215, 0, 240); prefix = "枪"; break;
            case ue4draw::DrawWorldObjectKind::Vehicle:
                if (!m_enableVehicleESP) continue;
                enabled = true; color = IM_COL32(255, 140, 0, 240); prefix = "车"; break;
            case ue4draw::DrawWorldObjectKind::Airdrop:
                if (!m_enableAirdropESP) continue;
                enabled = true; color = IM_COL32(255, 60, 60, 250); prefix = "空"; break;
            case ue4draw::DrawWorldObjectKind::DeathBox:
                if (!m_enableDeathBoxESP) continue;
                enabled = true; color = IM_COL32(220, 60, 220, 250); prefix = "死"; break;
        }
        if (!enabled) continue;

        const float dx = obj.posX - camX;
        const float dy = obj.posY - camY;
        const float dz = obj.posZ - camZ;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > maxDistCm) continue;

        float sx = 0.0f, sy = 0.0f;
        if (!worldToScreen(data, obj.posX, obj.posY, obj.posZ, screenW, screenH, sx, sy)) continue;
        if (sx < -50.0f || sx > screenW + 50.0f || sy < -50.0f || sy > screenH + 50.0f) continue;

        // 距离自适应大小: 远处更小
        float r = 6.0f;
        if (dist > 5000.0f) r = 4.0f;
        if (dist > 15000.0f) r = 3.0f;

        // 标签堆叠 key (8x8 像素桶)
        const int64_t bucketKey = (static_cast<int64_t>(static_cast<int>(sx) >> 3) << 32)
                                  | (static_cast<int64_t>(static_cast<int>(sy) >> 3) & 0xFFFFFFFF);
        const int slot = stackSlot[bucketKey]++;

        // 圆点只在该位置首次出现时绘制
        if (slot == 0) {
            dl->AddCircleFilled(ImVec2(sx, sy), r, color);
            dl->AddCircle(ImVec2(sx, sy), r + 1.0f, IM_COL32(0, 0, 0, 180), 0, 1.5f);
        }

        char text[96];
        if (obj.kind == ue4draw::DrawWorldObjectKind::Vehicle && obj.fuel >= 0.0f) {
            snprintf(text, sizeof(text), "%s %s 油%.0f%% %.0fm",
                     prefix, obj.label.c_str(), obj.fuel, dist / 100.0f);
        } else if (obj.inBox) {
            // 箱内物品: 不显示距离 (与箱子相同), 加 ▸ 表示子项
            snprintf(text, sizeof(text), "  ▸ %s %s", prefix, obj.label.c_str());
        } else {
            snprintf(text, sizeof(text), "%s %s %.0fm", prefix, obj.label.c_str(), dist / 100.0f);
        }
        ImVec2 ts = ImGui::CalcTextSize(text);
        float tx = sx + r + 4.0f;
        float ty = sy - ts.y * 0.5f + slot * (ts.y + 1.0f);
        dl->AddText(ImVec2(tx + 1, ty + 1), IM_COL32(0, 0, 0, 200), text);
        dl->AddText(ImVec2(tx, ty), color, text);
        rendered++;
    }

    ImGui::End();
    return rendered;
}

// =====================================================================
//  小地图绘制
// =====================================================================
int UE4Overlay::drawMinimap(const DrawGameData& data, float screenW, float screenH) {
    float mapSize = m_minimapSize;
    // 默认位置: 屏幕右上角; 否则使用用户保存的坐标 (并夹紧在屏幕内)
    float defaultX = screenW - mapSize - 15.0f;
    float defaultY = 15.0f;
    float mapX = (m_minimapPosX <= ue4draw::kMinimapPosUnset) ? defaultX : m_minimapPosX;
    float mapY = (m_minimapPosY <= ue4draw::kMinimapPosUnset) ? defaultY : m_minimapPosY;
    // 允许负坐标: 雷达可超出屏幕边界, 仅保留至少 50px 在屏内
    const float minVisible = 50.0f;
    float clampMinX = -mapSize + minVisible;
    float clampMinY = -mapSize + minVisible;
    float clampMaxX = std::max(screenW - minVisible, clampMinX + 1.0f);
    float clampMaxY = std::max(screenH - minVisible, clampMinY + 1.0f);
    mapX = std::max(clampMinX, std::min(mapX, clampMaxX));
    mapY = std::max(clampMinY, std::min(mapY, clampMaxY));
    // 持久化夹紧后的值, 确保锁定/解锁切换时不会被默认值覆盖
    m_minimapPosX = mapX;
    m_minimapPosY = mapY;

    // 锁定时强制对齐 m_minimapPos*; 解锁时仅首次设置位置, 让 ImGui 自由拖拽
    ImGuiCond posCond = m_minimapLocked ? ImGuiCond_Always : ImGuiCond_Once;
    ImGui::SetNextWindowPos(ImVec2(mapX, mapY), posCond);
    ImGui::SetNextWindowSize(ImVec2(mapSize, mapSize), ImGuiCond_Always);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoSavedSettings;
    if (m_minimapLocked) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
    }
    ImGui::Begin("##UE4Minimap", nullptr, flags);

    if (!m_minimapLocked) {
        // 同步 ImGui 拖拽后的窗口位置回 m_minimapPos*, 锁定时不会回弹
        ImVec2 wp = ImGui::GetWindowPos();
        m_minimapPosX = wp.x;
        m_minimapPosY = wp.y;
        mapX = wp.x;
        mapY = wp.y;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin(mapX, mapY);
    const float cx = mapX + mapSize / 2.0f;
    const float cy = mapY + mapSize / 2.0f;
    const float radarRadius = std::max(mapSize * 0.5f - 10.0f, 20.0f);
    const float rangeMeters = std::max(m_minimapRangeMeters, 60.0f);
    const float yawRad = 0.0f;
    const float forwardX = std::cos(yawRad);
    const float forwardY = std::sin(yawRad);
    const float rightX = -forwardY;
    const float rightY = forwardX;
    const bool hasSelfPos = hasValidWorldPoint(data.myPosX, data.myPosY, data.myPosZ);

    // 背景
    dl->AddRectFilled(origin, ImVec2(mapX + mapSize, mapY + mapSize),
                      IM_COL32(20, 20, 20, 180));
    dl->AddRect(origin, ImVec2(mapX + mapSize, mapY + mapSize),
                IM_COL32(100, 100, 100, 255), 0, 0, 1.5f);

    dl->AddCircle(ImVec2(cx, cy), radarRadius, IM_COL32(140, 140, 140, 220), 48, 1.5f);
    dl->AddLine(ImVec2(cx - radarRadius, cy), ImVec2(cx + radarRadius, cy), IM_COL32(90, 90, 90, 140), 1.0f);
    dl->AddLine(ImVec2(cx, cy - radarRadius), ImVec2(cx, cy + radarRadius), IM_COL32(90, 90, 90, 140), 1.0f);

    // 绘制自己 (白色十字)
    dl->AddLine(ImVec2(cx - 5, cy), ImVec2(cx + 5, cy), IM_COL32(255, 255, 255, 255), 2.0f);
    dl->AddLine(ImVec2(cx, cy - 5), ImVec2(cx, cy + 5), IM_COL32(255, 255, 255, 255), 2.0f);

    // 绘制所有玩家
    int renderedCount = 0;
    for (const auto& p : data.players) {
        if (!p.isAlive) continue;
        if (p.isTeammate && !m_enableTeammate) continue;
        if (!hasSelfPos || !hasUsablePlayerPosition(p)) continue;

        const float dxMeters = (p.posX - data.myPosX) / 100.0f;
        const float dyMeters = (p.posY - data.myPosY) / 100.0f;
        const float rightMeters = dxMeters * rightX + dyMeters * rightY;
        const float forwardMeters = dxMeters * forwardX + dyMeters * forwardY;
        const float radialDistance = std::sqrt(rightMeters * rightMeters + forwardMeters * forwardMeters);
        if (!isValidNumber(radialDistance) || radialDistance > rangeMeters) continue;

        float px = cx + (rightMeters / rangeMeters) * radarRadius;
        float py = cy - (forwardMeters / rangeMeters) * radarRadius;

        // 限制在小地图范围内
        if (px < mapX || px > mapX + mapSize || py < mapY || py > mapY + mapSize) continue;

        ImU32 color = p.isTeammate ? IM_COL32(0, 200, 0, 255) : IM_COL32(255, 50, 50, 255);
        float radius = 3.0f;
        renderedCount++;

        dl->AddCircleFilled(ImVec2(px, py), radius, color);

        // AI 用空心圆区分
        if (p.isAI) {
            dl->AddCircle(ImVec2(px, py), radius + 1, IM_COL32(150, 150, 150, 200));
        }
    }

    // 标题
    char title[64];
    snprintf(title, sizeof(title), "存活: %d | %dm", data.aliveCount, static_cast<int>(rangeMeters));
    dl->AddText(ImVec2(mapX + 4, mapY + 2), IM_COL32(200, 200, 200, 220), title);

    ImGui::End();
    return renderedCount;
}

// =====================================================================
//  AI 屏幕检测绘制 (NCNN NanoDet 通过 screencap 旁路截屏, 不使用 Hook)
//  - 检测线程独立运行 (ai_overlay::AIPipeline)
//  - 此函数仅从 AISharedData 读最新一帧结果, 缩放映射到当前屏幕坐标
// =====================================================================
int UE4Overlay::drawAIDetections(float screenW, float screenH) {
#if defined(AI_OVERLAY_AVAILABLE)
    auto& shared = ai_overlay::AISharedData::getInstance();
    {
        static int s_dbg = 0;
        if ((++s_dbg % 120) == 1) {
            LOG(LOG_LEVEL_INFO, "[AI/Draw] entry valid=%d enabled=%d ready=%d",
                shared.valid()?1:0, shared.enabled()?1:0, shared.ready()?1:0);
        }
    }
    if (!shared.enabled() || !shared.ready()) return 0;

    // 同步 GUI 状态到 pipeline
    shared.setScoreThreshold(m_aiScoreThr);
    shared.setTargetClassFilter(m_aiOnlyPerson ? 0 : -1);

    std::vector<ai_overlay::DetectionBox> dets;
    int srcW = 0, srcH = 0;
    shared.getDetections(dets, srcW, srcH);
    if (dets.empty() || srcW <= 0 || srcH <= 0) return 0;

    // 截屏帧大小 → 当前 ImGui 屏幕大小
    const float sx = screenW / static_cast<float>(srcW);
    const float sy = screenH / static_cast<float>(srcH);

    // 与 drawESP 一致: 使用全屏透明窗口而非 BackgroundDrawList
    // (AImGui 转发不收 Background/Foreground draw list, 只转发 named window draw lists)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##UE4AI", nullptr,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 boxCol = IM_COL32(
        static_cast<int>(m_aiBoxColor[0] * 255),
        static_cast<int>(m_aiBoxColor[1] * 255),
        static_cast<int>(m_aiBoxColor[2] * 255),
        230);
    const ImU32 textCol = IM_COL32(255, 255, 255, 230);
    const ImU32 textBgCol = IM_COL32(0, 0, 0, 160);

    int drawn = 0;
    for (const auto& b : dets) {
        if (b.score < m_aiScoreThr) continue;
        const float x0 = b.x * sx;
        const float y0 = b.y * sy;
        const float x1 = (b.x + b.w) * sx;
        const float y1 = (b.y + b.h) * sy;
        dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), boxCol, 0.0f, 0, m_aiBoxThickness);
        if (m_aiDrawScore) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d %.0f%%", b.classId, b.score * 100.f);
            ImVec2 ts = ImGui::CalcTextSize(buf);
            dl->AddRectFilled(ImVec2(x0, y0 - ts.y - 2),
                              ImVec2(x0 + ts.x + 4, y0),
                              textBgCol);
            dl->AddText(ImVec2(x0 + 2, y0 - ts.y - 1), textCol, buf);
        }
        ++drawn;
    }

    // ── AI 辅助瞄准: 视觉锁定 + (可选)注入触屏 ──
    if (m_aiAimEnable) {
        auto& aim = ai_overlay::AimAssist::getInstance();
        // 同步 UI 配置 -> AimAssist
        aim.setVisualOnly(m_aiAimVisualOnly);
        aim.setFovRadius(m_aiAimFovRadius * srcW / std::max(1.f, screenW));
        aim.setHeadRatio(m_aiAimHeadRatio);
        aim.setMinScore(m_aiAimMinScore / 100.0f);
        aim.setOnlyPerson(m_aiAimOnlyPerson);
        aim.setSensitivity(m_aiAimSensitivityX, m_aiAimSensitivityY);
        aim.setRequireTrigger(m_aiAimRequireTrigger);

        // FOV 圆 (屏幕中心)
        ImVec2 center(screenW * 0.5f, screenH * 0.5f);
        dl->AddCircle(center, m_aiAimFovRadius,
                      IM_COL32(255, 255, 255, 70), 64, 1.0f);
        dl->AddCircleFilled(center, 2.5f, IM_COL32(255, 255, 255, 220));

        // 选最优目标 (在源帧坐标系下)
        ai_overlay::AimTarget tgt;
        if (aim.pickTarget(srcW * 0.5f, srcH * 0.5f, tgt)) {
            float tx = tgt.screenX * sx;
            float ty = tgt.screenY * sy;
            float bx0 = tgt.boxX * sx;
            float by0 = tgt.boxY * sy;
            float bx1 = (tgt.boxX + tgt.boxW) * sx;
            float by1 = (tgt.boxY + tgt.boxH) * sy;
            const ImU32 lockCol = IM_COL32(50, 255, 80, 230);
            // 锁定框加粗
            dl->AddRect(ImVec2(bx0, by0), ImVec2(bx1, by1), lockCol, 0, 0, 3.0f);
            // 准星到目标连线
            dl->AddLine(center, ImVec2(tx, ty), IM_COL32(50, 255, 80, 200), 1.5f);
            // 目标十字
            dl->AddCircle(ImVec2(tx, ty), 8.0f, lockCol, 0, 2.0f);
            dl->AddLine(ImVec2(tx - 12, ty), ImVec2(tx + 12, ty), lockCol, 1.5f);
            dl->AddLine(ImVec2(tx, ty - 12), ImVec2(tx, ty + 12), lockCol, 1.5f);
        }
    }

    ImGui::End();
    return drawn;
#else
    (void)screenW; (void)screenH;
    return 0;
#endif
}

} // namespace ue4draw

OBFU_ATTRS_END
