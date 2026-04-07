#include "UE4Draw.h"
#include "../../Log/log.h"
#include <cmath>
#include <algorithm>
#include <chrono>

#define DTAG "UE4Draw"
#define DLOG(level, fmt, ...) LOGT(DTAG, level, fmt, ##__VA_ARGS__)

namespace ue4draw {

namespace {
using Clock = std::chrono::steady_clock;

struct ViewPoint {
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

float distanceMeters(float x1, float y1, float z1, float x2, float y2, float z2) {
    const float dx = x1 - x2;
    const float dy = y1 - y2;
    const float dz = z1 - z2;
    return std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f;
}

float playerDistanceMeters(const DrawPlayerInfo& player, const ViewPoint& viewPoint) {
    return distanceMeters(player.posX, player.posY, player.posZ, viewPoint.x, viewPoint.y, viewPoint.z);
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

bool projectFallbackMarker(const DrawGameData& data,
                           const DrawPlayerInfo& player,
                           float screenW,
                           float screenH,
                           float& centerX,
                           float& topY,
                           float& bottomY,
                           float& boxW,
                           bool& edgeClamped) {
    constexpr float DEG2RAD = 3.14159265358979f / 180.0f;

    ViewPoint viewPoint;
    if (!tryGetViewPoint(data, viewPoint)) {
        return false;
    }

    const float dx = player.posX - viewPoint.x;
    const float dy = player.posY - viewPoint.y;
    const float dz = player.posZ - viewPoint.z;
    const float planarDist = std::sqrt(dx * dx + dy * dy);
    if (planarDist < 1.0f) {
        return false;
    }

    const float yawRad = data.camYaw * DEG2RAD;
    const float forward = dx * std::cos(yawRad) + dy * std::sin(yawRad);
    const float side = -dx * std::sin(yawRad) + dy * std::cos(yawRad);

    float fov = data.camFOV;
    if (!isValidNumber(fov) || fov < 30.0f || fov > 170.0f) {
        fov = 90.0f;
    }

    float tanHalfFov = std::tan(fov * 0.5f * DEG2RAD);
    if (!isValidNumber(tanHalfFov) || tanHalfFov < 0.2f) {
        tanHalfFov = std::tan(45.0f * DEG2RAD);
    }

    const float safeForward = std::fabs(forward) > 120.0f ? forward : (forward >= 0.0f ? 120.0f : -120.0f);
    const float normalizedX = std::clamp((side / safeForward) / tanHalfFov, -1.6f, 1.6f);
    const float normalizedY = std::clamp(dz / std::max(planarDist, 120.0f), -0.65f, 0.45f);

    float screenX = screenW * 0.5f + normalizedX * screenW * 0.38f;
    float screenY = screenH * 0.58f - normalizedY * screenH * 0.32f;

    constexpr float kMarginX = 32.0f;
    constexpr float kMarginTop = 42.0f;
    constexpr float kMarginBottom = 72.0f;
    edgeClamped = false;

    if (forward < 0.0f) {
        screenX = side >= 0.0f ? screenW - kMarginX : kMarginX;
        screenY = std::clamp(screenY, screenH * 0.25f, screenH * 0.75f);
        edgeClamped = true;
    }

    const float clampedX = std::clamp(screenX, kMarginX, screenW - kMarginX);
    const float clampedY = std::clamp(screenY, kMarginTop, screenH - kMarginBottom);
    if (clampedX != screenX || clampedY != screenY) {
        edgeClamped = true;
    }

    centerX = clampedX;
    const float distMetersValue = playerDistanceMeters(player, viewPoint);
    const float boxH = std::clamp(1050.0f / std::max(distMetersValue, 1.0f), 34.0f, 92.0f);
    boxW = boxH * 0.62f;
    topY = std::clamp(clampedY - boxH * 0.82f, 8.0f, screenH - boxH - 8.0f);
    bottomY = topY + boxH;
    return true;
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
    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;
    int espDrawCount = 0;
    int minimapDrawCount = 0;

    static bool s_hasLastMatchState = false;
    static bool s_lastInMatch = false;
    static Clock::time_point s_lastSummaryLog;

    if (!s_hasLastMatchState || s_lastInMatch != data.inMatch) {
        DLOG(LOG_LEVEL_INFO,
             "match state changed: inMatch=%d world=%s state=%s alive=%d/%d players=%zu",
             data.inMatch ? 1 : 0,
             data.worldName.c_str(),
             data.matchState.c_str(),
             data.aliveCount,
             data.totalCount,
             data.players.size());
        s_lastInMatch = data.inMatch;
        s_hasLastMatchState = true;
    }

    // 对局中时绘制 ESP 和小地图
    if (data.inMatch) {
        if (m_enableESP) {
            espDrawCount = drawESP(data, screenW, screenH);
        }
        if (m_enableMinimap) {
            minimapDrawCount = drawMinimap(data, screenW, screenH);
        }

        if (shouldLogEvery(s_lastSummaryLog, std::chrono::milliseconds(2000))) {
            DLOG(LOG_LEVEL_INFO,
                 "overlay summary: screen=%.0fx%.0f tracked=%zu alive=%d/%d esp=%d minimap=%d cam=(%.0f, %.0f, %.0f) fov=%.1f",
                 screenW,
                 screenH,
                 data.players.size(),
                 data.aliveCount,
                 data.totalCount,
                 espDrawCount,
                 minimapDrawCount,
                 data.camLocX,
                 data.camLocY,
                 data.camLocZ,
                 data.camFOV);
        }
    }

    drawTouchPointOverlay(m_enableTouchPoint, screenW, screenH);

    // 控制菜单始终显示
    drawMenu(data);
}

// =====================================================================
//  控制菜单
// =====================================================================
void UE4Overlay::drawMenu(const DrawGameData& data) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("PUBG ESP", &m_menuExpanded, flags)) {
        bool settingsChanged = false;

        // 对局状态
        if (data.inMatch) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "★ 对局中");
            ImGui::SameLine();
            ImGui::Text("Players: %d/%d", data.aliveCount, data.totalCount);
            ImGui::Text("World: %s", data.worldName.c_str());
            ImGui::Text("State: %s", data.matchState.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "○ 等待对局...");
        }
        ImGui::Separator();

        // 功能开关
        settingsChanged |= ImGui::Checkbox("ESP Box", &m_enableESP);
        settingsChanged |= ImGui::Checkbox("Snap Line", &m_enableSnapline);
        settingsChanged |= ImGui::Checkbox("HP Bar", &m_enableHP);
        settingsChanged |= ImGui::Checkbox("Name", &m_enableName);
        settingsChanged |= ImGui::Checkbox("Distance", &m_enableDistance);
        settingsChanged |= ImGui::Checkbox("Show Teammate", &m_enableTeammate);
        ImGui::Separator();

        settingsChanged |= ImGui::Checkbox("Minimap", &m_enableMinimap);
        if (m_enableMinimap) {
            settingsChanged |= ImGui::SliderFloat("Map Size", &m_minimapSize, 100.0f, 400.0f, "%.0f");
        }
        settingsChanged |= ImGui::Checkbox("Fallback ESP", &m_enableFallbackESP);
        settingsChanged |= ImGui::Checkbox("Player List", &m_enablePlayerList);
        settingsChanged |= ImGui::Checkbox("Touch Point", &m_enableTouchPoint);
        ImGui::Separator();
        settingsChanged |= ImGui::SliderFloat("Max Dist", &m_espMaxDist, 100.0f, 2000.0f, "%.0f m");

        if (data.inMatch) {
            ImGui::Text("ESP Draw: precise %d  fallback %d", m_lastPreciseESP, m_lastFallbackESP);
        }
        if (m_enableTouchPoint) {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.MouseDown[0] && ImGui::IsMousePosValid()) {
                ImGui::Text("Touch: %.0f, %.0f", io.MousePos.x, io.MousePos.y);
            } else {
                ImGui::TextDisabled("Touch: idle");
            }
        }

        if (m_enablePlayerList && data.inMatch && ImGui::CollapsingHeader("Tracked Players", ImGuiTreeNodeFlags_DefaultOpen)) {
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
                snprintf(coordLine, sizeof(coordLine), "Pos: %.0f, %.0f, %.0f%s",
                         player.posX,
                         player.posY,
                         player.posZ,
                         player.isAI ? "  AI" : "");
                ImGui::TextDisabled("%s", coordLine);

                if (index + 1 < visibleCount) {
                    ImGui::Separator();
                }
            }
        }

        if (settingsChanged) {
            DLOG(LOG_LEVEL_INFO,
                 "settings updated: esp=%s snap=%s hp=%s name=%s dist=%s teammate=%s minimap=%s fallback=%s playerList=%s touch=%s mapSize=%.0f maxDist=%.0f",
                 onOff(m_enableESP),
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

    // 相机旋转角度 (UE4: Pitch=上下, Yaw=左右, Roll=翻滚)
    float pitch = cam.camPitch * DEG2RAD;
    float yaw   = cam.camYaw * DEG2RAD;

    float cp = std::cos(pitch), sp = std::sin(pitch);
    float cy = std::cos(yaw),   sy_r = std::sin(yaw);

    // 旋转矩阵的三个轴 (UE4 左手坐标系)
    // Forward (X轴方向)
    float fwdX = cp * cy;
    float fwdY = cp * sy_r;
    float fwdZ = sp;
    // Right (Y轴方向)
    float rightX = -sy_r;
    float rightY = cy;
    float rightZ = 0.0f;
    // Up (Z轴方向)
    float upX = -sp * cy;
    float upY = -sp * sy_r;
    float upZ = cp;

    // 世界坐标差 (目标 - 相机)
    float dx = wx - cam.camLocX;
    float dy = wy - cam.camLocY;
    float dz = wz - cam.camLocZ;

    // 投影到相机坐标系
    float dot_fwd   = dx * fwdX   + dy * fwdY   + dz * fwdZ;
    float dot_right = dx * rightX + dy * rightY + dz * rightZ;
    float dot_up    = dx * upX    + dy * upY    + dz * upZ;

    // 在相机背后则不可见
    if (dot_fwd < 1.0f) return false;

    // 透视投影
    float fov = cam.camFOV > 0.0f ? cam.camFOV : 90.0f;
    float tanHalfFOV = std::tan(fov * 0.5f * DEG2RAD);

    sx = screenW * 0.5f + (dot_right / dot_fwd / tanHalfFOV) * screenW * 0.5f;
    sy = screenH * 0.5f - (dot_up / dot_fwd / tanHalfFOV) * screenW * 0.5f;

    return true;
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
    const bool hasPreciseCamera = hasValidWorldPoint(data.camLocX, data.camLocY, data.camLocZ);

    for (const auto& p : data.players) {
        if (!p.isAlive) continue;
        if (p.isTeammate && !m_enableTeammate) continue;

        if (!hasViewPoint) continue;

        const float dist = playerDistanceMeters(p, viewPoint);
        if (dist > m_espMaxDist || dist < 1.0f) continue;

        const ImU32 boxColor = p.isTeammate ? IM_COL32(0, 200, 0, 200) : IM_COL32(255, 50, 50, 200);
        const float hpRatio = playerHealthRatio(p);
        bool rendered = false;

        if (hasPreciseCamera) {
            float footSX = 0.0f;
            float footSY = 0.0f;
            float headSX = 0.0f;
            float headSY = 0.0f;
            if (worldToScreen(data, p.posX, p.posY, p.posZ, screenW, screenH, footSX, footSY)
                && worldToScreen(data, p.posX, p.posY, p.posZ + 180.0f, screenW, screenH, headSX, headSY)) {
                const float boxH = std::fabs(footSY - headSY);
                const float boxW = boxH * 0.5f;
                const float cx = (footSX + headSX) * 0.5f;
                const float topY = std::min(footSY, headSY);
                const float botY = std::max(footSY, headSY);

                if (boxH >= 5.0f
                    && cx >= -boxW && cx <= screenW + boxW
                    && topY >= -boxH && botY <= screenH + boxH) {
                    preciseRenderedCount++;
                    rendered = true;

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

        if (!rendered && m_enableFallbackESP) {
            float cx = 0.0f;
            float topY = 0.0f;
            float botY = 0.0f;
            float boxW = 0.0f;
            bool edgeClamped = false;
            if (projectFallbackMarker(data, p, screenW, screenH, cx, topY, botY, boxW, edgeClamped)) {
                fallbackRenderedCount++;

                const float hpBarHeight = 4.0f;
                const float hpBarTop = topY - hpBarHeight - 3.0f;
                const ImVec2 boxMin(cx - boxW / 2, topY);
                const ImVec2 boxMax(cx + boxW / 2, botY);

                dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, edgeClamped ? 70 : 90));
                dl->AddRect(boxMin, boxMax, boxColor, 0.0f, 0, edgeClamped ? 1.5f : 2.0f);

                if (m_enableSnapline) {
                    dl->AddLine(ImVec2(myCx, myCy), ImVec2(cx, botY),
                                IM_COL32(255, 255, 255, edgeClamped ? 55 : 85), 1.0f);
                }

                if (m_enableHP) {
                    dl->AddRectFilled(ImVec2(boxMin.x, hpBarTop), ImVec2(boxMax.x, hpBarTop + hpBarHeight),
                                      IM_COL32(0, 0, 0, 160));
                    dl->AddRectFilled(ImVec2(boxMin.x, hpBarTop),
                                      ImVec2(boxMin.x + boxW * hpRatio, hpBarTop + hpBarHeight),
                                      hpColor(hpRatio));
                }

                if (edgeClamped) {
                    const float direction = cx < screenW * 0.5f ? -1.0f : 1.0f;
                    dl->AddTriangleFilled(
                        ImVec2(cx, topY + 10.0f),
                        ImVec2(cx - direction * 10.0f, topY + 2.0f),
                        ImVec2(cx - direction * 10.0f, topY + 18.0f),
                        boxColor);
                }

                if (m_enableName) {
                    const char* label = playerLabel(p);
                    ImVec2 textSize = ImGui::CalcTextSize(label);
                    dl->AddText(ImVec2(cx - textSize.x / 2, topY - 18.0f),
                                IM_COL32(255, 255, 255, 220), label);
                }

                char infoBuf[48];
                snprintf(infoBuf, sizeof(infoBuf), "%.0fm %.0fHP", dist, p.health);
                ImVec2 infoSize = ImGui::CalcTextSize(infoBuf);
                dl->AddText(ImVec2(cx - infoSize.x / 2, botY + 2.0f),
                            IM_COL32(220, 220, 220, 210), infoBuf);
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

    // 背景
    dl->AddRectFilled(origin, ImVec2(mapX + mapSize, mapY + mapSize),
                      IM_COL32(20, 20, 20, 180));
    dl->AddRect(origin, ImVec2(mapX + mapSize, mapY + mapSize),
                IM_COL32(100, 100, 100, 255), 0, 0, 1.5f);

    // 地图范围 (PUBG 8x8km = 800000 UE units)
    constexpr float MAP_WORLD_SIZE = 800000.0f;
    float cx = mapX + mapSize / 2.0f;
    float cy = mapY + mapSize / 2.0f;

    // 绘制自己 (白色十字)
    dl->AddLine(ImVec2(cx - 5, cy), ImVec2(cx + 5, cy), IM_COL32(255, 255, 255, 255), 2.0f);
    dl->AddLine(ImVec2(cx, cy - 5), ImVec2(cx, cy + 5), IM_COL32(255, 255, 255, 255), 2.0f);

    // 绘制所有玩家
    int renderedCount = 0;
    for (const auto& p : data.players) {
        if (!p.isAlive) continue;
        if (p.isTeammate && !m_enableTeammate) continue;

        // 世界坐标 -> 小地图坐标 (以自己为中心)
        float dx = (p.posX - data.myPosX) / MAP_WORLD_SIZE * mapSize;
        float dy = (p.posY - data.myPosY) / MAP_WORLD_SIZE * mapSize;
        float px = cx + dy;  // Y轴对应左右
        float py = cy - dx;  // X轴对应前后

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
    snprintf(title, sizeof(title), "Alive: %d", data.aliveCount);
    dl->AddText(ImVec2(mapX + 4, mapY + 2), IM_COL32(200, 200, 200, 220), title);

    ImGui::End();
    return renderedCount;
}

} // namespace ue4draw

OBFU_ATTRS_END
