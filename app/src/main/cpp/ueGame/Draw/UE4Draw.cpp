#include "UE4Draw.h"
#include "../../Log/log.h"
#include <imgui/imgui_internal.h>
#include <cmath>
#include <algorithm>
#include <chrono>

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
    constexpr auto kTransientHoldWindow = std::chrono::milliseconds(120);

    if (!data.inMatch) {
        s_lastStableData = data;
        s_lastStableTime = now;
        s_hasLastStableData = true;
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
    constexpr float DEG2RAD = 3.14159265358979f / 180.0f;
    constexpr float kNearDepth = 1.0f;

    CameraSpacePoint cameraPoint;
    if (!transformWorldToCamera(data, point.x, point.y, point.z, cameraPoint) || cameraPoint.z <= kNearDepth) {
        return false;
    }

    const float tanHalfFov = std::tan(sanitizeFov(data.camFOV) * 0.5f * DEG2RAD);
    if (!isValidNumber(tanHalfFov) || tanHalfFov < 0.01f) {
        return false;
    }

    const float focalLength = screenW * 0.5f / tanHalfFov;
    sx = screenW * 0.5f + cameraPoint.x * focalLength / cameraPoint.z;
    sy = screenH * 0.5f - cameraPoint.y * focalLength / cameraPoint.z;
    return isValidNumber(sx) && isValidNumber(sy);
}

int drawPlayerSkeleton(ImDrawList* drawList,
                       const DrawGameData& data,
                       const DrawPlayerInfo& player,
                       ImU32 color,
                       float screenW,
                       float screenH) {
    // 至少需要 5 个骨骼点才值得绘制 (避免零星点闪烁)
    if (countTrackedBones(player) < 5) return 0;

    int segmentCount = 0;
    for (const BoneSegment& segment : kSkeletonSegments) {
        float fromX, fromY, toX, toY;
        if (!projectBonePoint(data, player, segment.from, screenW, screenH, fromX, fromY)
            || !projectBonePoint(data, player, segment.to, screenW, screenH, toX, toY))
            continue;

        // 跳过投影后距离过大的线段 (异常数据保护)
        float dx = toX - fromX, dy = toY - fromY;
        if (dx * dx + dy * dy > screenH * screenH) continue;

        drawList->AddLine(ImVec2(fromX, fromY), ImVec2(toX, toY), color, 1.5f);
        segmentCount++;
    }

    // 只在有线段时才绘制关节点 (避免孤立点)
    if (segmentCount < 3) return segmentCount;

    for (size_t boneIndex = 0; boneIndex < kTrackedBoneCount; ++boneIndex) {
        float px, py;
        if (!projectBonePoint(data, player, static_cast<DrawBoneId>(boneIndex), screenW, screenH, px, py))
            continue;
        drawList->AddCircleFilled(ImVec2(px, py), 3.0f, IM_COL32(80, 255, 255, 200));
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

    constexpr float DEG2RAD = 3.14159265358979f / 180.0f;
    constexpr float kNearDepth = 1.0f;
    constexpr float kMarginX = 40.0f;
    constexpr float kMarginY = 60.0f;

    const float tanHalfFov = std::tan(sanitizeFov(data.camFOV) * 0.5f * DEG2RAD);
    if (!isValidNumber(tanHalfFov) || tanHalfFov < 0.01f) {
        return false;
    }

    const bool behindCamera = cameraPoint.z <= kNearDepth;
    const float safeDepth = std::max(std::fabs(cameraPoint.z), kNearDepth);
    const float focalLength = screenW * 0.5f / tanHalfFov;

    float screenDx = cameraPoint.x * focalLength / safeDepth;
    float screenDy = -cameraPoint.y * focalLength / safeDepth;
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

    const float halfW = std::max(screenW * 0.5f - kMarginX, 1.0f);
    const float halfH = std::max(screenH * 0.5f - kMarginY, 1.0f);
    const float scale = 1.0f / std::max(std::fabs(screenDx) / halfW, std::fabs(screenDy) / halfH);

    arrowX = std::clamp(screenW * 0.5f + screenDx * scale, kMarginX, screenW - kMarginX);
    arrowY = std::clamp(screenH * 0.5f + screenDy * scale, kMarginY, screenH - kMarginY);
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
    // 确认 ImGui frame 处于活跃状态, 防止从错误线程或 frame 外调用时 assert 崩溃
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) {
        return;
    }

    const DrawGameData renderData = stabilizeRenderData(data);
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
        if (m_enableMinimap) {
            minimapDrawCount = drawMinimap(renderData, screenW, screenH);
        }

        if (shouldLogEvery(s_lastSummaryLog, std::chrono::milliseconds(2000))) {
            DLOG(LOG_LEVEL_INFO,
                 "overlay summary: screen=%.0fx%.0f tracked=%zu alive=%d/%d esp=%d minimap=%d cam=(%.0f, %.0f, %.0f) fov=%.1f",
                 screenW,
                 screenH,
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

    // 控制菜单始终显示
    drawMenu(renderData);
}

// =====================================================================
//  控制菜单
// =====================================================================
void UE4Overlay::drawMenu(const DrawGameData& data) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("PUBG 绘制", &m_menuExpanded, flags)) {
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
        settingsChanged |= ImGui::Checkbox("射线", &m_enableSnapline);
        settingsChanged |= ImGui::Checkbox("血条", &m_enableHP);
        settingsChanged |= ImGui::Checkbox("名字", &m_enableName);
        settingsChanged |= ImGui::Checkbox("距离", &m_enableDistance);
        settingsChanged |= ImGui::Checkbox("显示队友", &m_enableTeammate);
        ImGui::Separator();

        settingsChanged |= ImGui::Checkbox("小地图", &m_enableMinimap);
        if (m_enableMinimap) {
            settingsChanged |= ImGui::SliderFloat("地图大小", &m_minimapSize, 100.0f, 400.0f, "%.0f");
            settingsChanged |= ImGui::SliderFloat("雷达范围", &m_minimapRangeMeters, 60.0f, 500.0f, "%.0f m");
        }
        settingsChanged |= ImGui::Checkbox("边缘箭头", &m_enableFallbackESP);
        settingsChanged |= ImGui::Checkbox("玩家列表", &m_enablePlayerList);
        settingsChanged |= ImGui::Checkbox("触点", &m_enableTouchPoint);
        ImGui::Separator();
        settingsChanged |= ImGui::SliderFloat("最大距离", &m_espMaxDist, 100.0f, 2000.0f, "%.0f m");

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
    constexpr float DEG2RAD = 3.14159265358979f / 180.0f;

    CameraSpacePoint cameraPoint;
    if (!transformWorldToCamera(cam, wx, wy, wz, cameraPoint)) {
        return false;
    }

    constexpr float kNearDepth = 1.0f;
    if (cameraPoint.z <= kNearDepth) {
        return false;
    }

    const float tanHalfFov = std::tan(sanitizeFov(cam.camFOV) * 0.5f * DEG2RAD);
    if (!isValidNumber(tanHalfFov) || tanHalfFov < 0.01f) {
        return false;
    }

    const float focalLength = screenW * 0.5f / tanHalfFov;
    sx = screenW * 0.5f + cameraPoint.x * focalLength / cameraPoint.z;
    sy = screenH * 0.5f - cameraPoint.y * focalLength / cameraPoint.z;

    return isValidNumber(sx) && isValidNumber(sy);
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
            if (drawPlayerSkeleton(dl, data, p, boxColor, screenW, screenH) > 0) {
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
            if (worldToScreen(data, p.posX, p.posY, p.posZ - kCharacterHalfHeight, screenW, screenH, footSX, footSY)
                && worldToScreen(data, p.posX, p.posY, p.posZ + kCharacterHalfHeight, screenW, screenH, headSX, headSY)) {
                const float boxH = std::fabs(footSY - headSY);
                const float boxW = boxH * 0.48f;
                const float cx = (footSX + headSX) * 0.5f;
                const float topY = std::min(footSY, headSY);
                const float botY = std::max(footSY, headSY);

                if (boxH >= 10.0f && boxH <= screenH * 0.9f
                    && cx >= -boxW && cx <= screenW + boxW
                    && topY >= -boxH && botY <= screenH + boxH) {
                    rendered = true;
                    preciseRendered = true;

                    dl->AddRect(ImVec2(cx - boxW / 2, topY), ImVec2(cx + boxW / 2, botY),
                                boxColor, 0, 0, 2.0f);

                    if (m_enableSnapline) {
                        dl->AddLine(ImVec2(myCx, myCy), ImVec2(cx, botY),
                                    IM_COL32(255, 255, 255, 100), 1.0f);
                    }

                    if (m_enableHP) {
                        const float hpX = cx - boxW / 2 - 5.0f;
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
                        dl->AddText(ImVec2(cx + boxW * 0.5f + 6.0f, topY), boneColor, boneBuf);
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
//  小地图绘制
// =====================================================================
int UE4Overlay::drawMinimap(const DrawGameData& data, float screenW, float screenH) {
    float mapSize = m_minimapSize;
    float mapX = screenW - mapSize - 15.0f;
    float mapY = 15.0f;

    ImGui::SetNextWindowPos(ImVec2(mapX, mapY));
    ImGui::SetNextWindowSize(ImVec2(mapSize, mapSize));
    ImGui::Begin("##UE4Minimap", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin(mapX, mapY);
    const float cx = mapX + mapSize / 2.0f;
    const float cy = mapY + mapSize / 2.0f;
    const float radarRadius = std::max(mapSize * 0.5f - 10.0f, 20.0f);
    const float rangeMeters = std::max(m_minimapRangeMeters, 60.0f);
    constexpr float DEG2RAD = 3.14159265358979f / 180.0f;
    const float yawRad = sanitizeAngleDegrees(data.camYaw) * DEG2RAD;
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

} // namespace ue4draw

OBFU_ATTRS_END
