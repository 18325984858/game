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

float distanceMeters(float x1, float y1, float z1, float x2, float y2, float z2) {
    const float dx = x1 - x2;
    const float dy = y1 - y2;
    const float dz = z1 - z2;
    return std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f;
}

float playerDistanceMeters(const DrawPlayerInfo& player, const ViewPoint& viewPoint) {
    return distanceMeters(player.posX, player.posY, player.posZ, viewPoint.x, viewPoint.y, viewPoint.z);
}

bool transformWorldToCamera(const DrawGameData& data,
                            float wx,
                            float wy,
                            float wz,
                            CameraSpacePoint& outPoint) {
    ViewPoint cameraOrigin;
    if (!tryGetViewPoint(data, cameraOrigin)) {
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
        settingsChanged |= ImGui::Checkbox("射线", &m_enableSnapline);
        settingsChanged |= ImGui::Checkbox("血条", &m_enableHP);
        settingsChanged |= ImGui::Checkbox("名字", &m_enableName);
        settingsChanged |= ImGui::Checkbox("距离", &m_enableDistance);
        settingsChanged |= ImGui::Checkbox("显示队友", &m_enableTeammate);
        ImGui::Separator();

        settingsChanged |= ImGui::Checkbox("小地图", &m_enableMinimap);
        if (m_enableMinimap) {
            settingsChanged |= ImGui::SliderFloat("地图大小", &m_minimapSize, 100.0f, 400.0f, "%.0f");
        }
        settingsChanged |= ImGui::Checkbox("边缘箭头", &m_enableFallbackESP);
        settingsChanged |= ImGui::Checkbox("玩家列表", &m_enablePlayerList);
        settingsChanged |= ImGui::Checkbox("触点", &m_enableTouchPoint);
        ImGui::Separator();
        settingsChanged |= ImGui::SliderFloat("最大距离", &m_espMaxDist, 100.0f, 2000.0f, "%.0f m");

        if (data.inMatch) {
            ImGui::Text("ESP 绘制: 精确 %d  箭头 %d", m_lastPreciseESP, m_lastFallbackESP);
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
                 "settings updated: esp=%s snap=%s hp=%s name=%s dist=%s teammate=%s minimap=%s edgeArrow=%s playerList=%s touch=%s mapSize=%.0f maxDist=%.0f",
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
    const bool hasPreciseCamera = hasValidWorldPoint(data.camLocX, data.camLocY, data.camLocZ)
        || hasValidWorldPoint(data.myPosX, data.myPosY, data.myPosZ);

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
    snprintf(title, sizeof(title), "存活: %d", data.aliveCount);
    dl->AddText(ImVec2(mapX + 4, mapY + 2), IM_COL32(200, 200, 200, 220), title);

    ImGui::End();
    return renderedCount;
}

} // namespace ue4draw

OBFU_ATTRS_END
