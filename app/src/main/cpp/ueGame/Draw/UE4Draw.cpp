#include "UE4Draw.h"
#include "../../Log/log.h"
#include <cmath>
#include <algorithm>

#define DTAG "UE4Draw"
#define DLOG(level, fmt, ...) LOGT(DTAG, level, fmt, ##__VA_ARGS__)

namespace ue4draw {

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

    // 对局中时绘制 ESP 和小地图
    if (data.inMatch) {
        if (m_enableESP) {
            drawESP(data, screenW, screenH);
        }
        if (m_enableMinimap) {
            drawMinimap(data, screenW, screenH);
        }
    }

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
        ImGui::Checkbox("ESP Box", &m_enableESP);
        ImGui::Checkbox("Snap Line", &m_enableSnapline);
        ImGui::Checkbox("HP Bar", &m_enableHP);
        ImGui::Checkbox("Name", &m_enableName);
        ImGui::Checkbox("Distance", &m_enableDistance);
        ImGui::Checkbox("Show Teammate", &m_enableTeammate);
        ImGui::Separator();

        ImGui::Checkbox("Minimap", &m_enableMinimap);
        if (m_enableMinimap) {
            ImGui::SliderFloat("Map Size", &m_minimapSize, 100.0f, 400.0f, "%.0f");
        }
        ImGui::Separator();
        ImGui::SliderFloat("Max Dist", &m_espMaxDist, 100.0f, 2000.0f, "%.0f m");
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
void UE4Overlay::drawESP(const DrawGameData& data, float screenW, float screenH) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##UE4ESP", nullptr,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float myCx = screenW * 0.5f;
    float myCy = screenH;

    for (const auto& p : data.players) {
        if (!p.isAlive) continue;
        if (p.isTeammate && !m_enableTeammate) continue;

        float dist = distance3D(p.posX, p.posY, p.posZ,
                                data.camLocX, data.camLocY, data.camLocZ) / 100.0f;
        if (dist > m_espMaxDist || dist < 1.0f) continue;

        // 投影脚部位置
        float footSX, footSY;
        if (!worldToScreen(data, p.posX, p.posY, p.posZ, screenW, screenH, footSX, footSY))
            continue;

        // 投影头部位置 (角色高度约180cm = 180 UE units)
        float headSX, headSY;
        if (!worldToScreen(data, p.posX, p.posY, p.posZ + 180.0f, screenW, screenH, headSX, headSY))
            continue;

        // 方框大小根据头脚屏幕距离计算
        float boxH = std::fabs(footSY - headSY);
        float boxW = boxH * 0.5f;
        if (boxH < 5.0f) continue;

        float cx = (footSX + headSX) * 0.5f;
        float topY = std::min(footSY, headSY);
        float botY = std::max(footSY, headSY);

        // 屏幕范围检查
        if (cx < -boxW || cx > screenW + boxW || topY < -boxH || botY > screenH + boxH)
            continue;

        ImU32 boxColor = p.isTeammate ? IM_COL32(0, 200, 0, 200) : IM_COL32(255, 50, 50, 200);
        float hpRatio = (p.healthMax > 0) ? (p.health / p.healthMax) : 0.0f;

        // 方框
        if (m_enableESP) {
            dl->AddRect(ImVec2(cx - boxW/2, topY), ImVec2(cx + boxW/2, botY),
                        boxColor, 0, 0, 2.0f);
        }

        // 射线
        if (m_enableSnapline) {
            dl->AddLine(ImVec2(myCx, myCy), ImVec2(cx, botY),
                        IM_COL32(255, 255, 255, 100), 1.0f);
        }

        // 血条 (方框左侧)
        if (m_enableHP) {
            float hpX = cx - boxW/2 - 5.0f;
            float hpFill = topY + (botY - topY) * (1.0f - hpRatio);
            dl->AddRectFilled(ImVec2(hpX - 3, topY), ImVec2(hpX, botY),
                              IM_COL32(0, 0, 0, 150));
            dl->AddRectFilled(ImVec2(hpX - 3, hpFill), ImVec2(hpX, botY),
                              hpColor(hpRatio));
        }

        // 名字
        if (m_enableName && !p.playerName.empty()) {
            ImVec2 textSize = ImGui::CalcTextSize(p.playerName.c_str());
            dl->AddText(ImVec2(cx - textSize.x/2, topY - textSize.y - 2),
                        IM_COL32(255, 255, 255, 230), p.playerName.c_str());
        }

        // 距离
        if (m_enableDistance) {
            char distBuf[32];
            snprintf(distBuf, sizeof(distBuf), "%.0fm", dist);
            ImVec2 textSize = ImGui::CalcTextSize(distBuf);
            dl->AddText(ImVec2(cx - textSize.x/2, botY + 2),
                        IM_COL32(200, 200, 200, 200), distBuf);
        }
    }

    ImGui::End();
}

// =====================================================================
//  小地图绘制
// =====================================================================
void UE4Overlay::drawMinimap(const DrawGameData& data, float screenW, float screenH) {
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
}

} // namespace ue4draw

OBFU_ATTRS_END
