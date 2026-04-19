/**
 * @file    DfmDraw.cpp
 * @brief   DFM Overlay 绘制 — 玩家/物资/箱子 小地图 + ESP + 列表面板
 *          仿照 ueGamepubgmhd/Draw/UE4Draw.cpp 代码规范
 */
#include "DfmDraw.h"
#include "../../Log/log.h"
#include <imgui/imgui_internal.h>
#include <android/log.h>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <atomic>

namespace dfmdraw {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kMinEspDistanceMeters = 0.1f;
constexpr float kRelaxedProjectionDistanceMeters = 8.0f;
constexpr float kRelaxedProjectionMinDepth = 0.05f;

bool shouldLogEvery(Clock::time_point& lastLogTime, std::chrono::milliseconds interval) {
    const auto now = Clock::now();
    if (lastLogTime.time_since_epoch().count() != 0 && now - lastLogTime < interval) return false;
    lastLogTime = now;
    return true;
}

bool hasValidPos(float x, float y, float z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z)
        && (std::fabs(x) > 1.0f || std::fabs(y) > 1.0f || std::fabs(z) > 1.0f);
}

float distMeters(float x1, float y1, float z1, float x2, float y2, float z2) {
    float dx = x1 - x2, dy = y1 - y2, dz = z1 - z2;
    return std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0f;
}

float estimateFallbackHalfHeight(float distMetersValue) {
    return std::clamp(220.0f / std::max(distMetersValue, 0.5f), 24.0f, 120.0f);
}

bool projectToScreen(const dfm::DrawDfmData& cam,
                     float wx, float wy, float wz,
                     float screenW, float screenH,
                     float minDepth,
                     float& sx, float& sy,
                     float* outDepth = nullptr,
                     float* outFocal = nullptr) {
    constexpr float DEG2RAD = 3.14159265358979f / 180.0f;

    if (!std::isfinite(cam.camLocX) || !std::isfinite(cam.camYaw)) return false;
    float fov = cam.camFOV;
    if (fov < 30.0f || fov > 170.0f) fov = 90.0f;

    float pitch = std::remainder(cam.camPitch, 360.0f) * DEG2RAD;
    float yaw   = std::remainder(cam.camYaw, 360.0f) * DEG2RAD;
    float roll  = std::remainder(cam.camRoll, 360.0f) * DEG2RAD;

    float sp = std::sin(pitch), cp = std::cos(pitch);
    float sy_ = std::sin(yaw),  cy = std::cos(yaw);
    float sr = std::sin(roll),  cr = std::cos(roll);

    float axX = cp * cy, axY = cp * sy_, axZ = sp;
    float ayX = sr*sp*cy - cr*sy_, ayY = sr*sp*sy_ + cr*cy, ayZ = -sr*cp;
    float azX = -(cr*sp*cy + sr*sy_), azY = cy*sr - cr*sp*sy_, azZ = cr*cp;

    float dx = wx - cam.camLocX, dy = wy - cam.camLocY, dz = wz - cam.camLocZ;
    float cX = dx*ayX + dy*ayY + dz*ayZ;
    float cY = dx*azX + dy*azY + dz*azZ;
    float cZ = dx*axX + dy*axY + dz*axZ;

    if (outDepth) *outDepth = cZ;
    if (cZ <= minDepth) return false;

    constexpr float kRefAspect = 16.0f / 9.0f;
    float tanHalfBase = std::tan(fov * 0.5f * DEG2RAD);
    if (tanHalfBase < 0.01f) return false;
    float focal = screenH * 0.5f * kRefAspect / tanHalfBase;
    if (outFocal) *outFocal = focal;

    sx = screenW * 0.5f + cX * focal / cZ;
    sy = screenH * 0.5f - cY * focal / cZ;
    return std::isfinite(sx) && std::isfinite(sy);
}

} // namespace

// =====================================================================
//  工具函数
// =====================================================================

ImU32 DfmOverlay::hpColor(float ratio) {
    if (ratio > 0.5f) return IM_COL32(0, 255, 0, 255);
    if (ratio > 0.25f) return IM_COL32(255, 255, 0, 255);
    return IM_COL32(255, 0, 0, 255);
}

float DfmOverlay::distance3D(float x1, float y1, float z1, float x2, float y2, float z2) {
    float dx = x1 - x2, dy = y1 - y2, dz = z1 - z2;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// =====================================================================
//  世界坐标 → 屏幕坐标投影 (与 PUBG UE4Draw 相同算法)
// =====================================================================

bool DfmOverlay::worldToScreen(const dfm::DrawDfmData& cam,
                                float wx, float wy, float wz,
                                float screenW, float screenH,
                                float& sx, float& sy) {
    return projectToScreen(cam, wx, wy, wz, screenW, screenH, 1.0f, sx, sy);
}

// =====================================================================
//  主绘制入口
// =====================================================================

void DfmOverlay::drawOverlay(const dfm::DrawDfmData& data) {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (!ctx || !ctx->WithinFrameScope) return;

    ImGuiIO& io = ImGui::GetIO();
    float screenW = io.DisplaySize.x;
    float screenH = io.DisplaySize.y;

    drawMenu(data);

    // 触摸点指示器 (在屏幕上显示手指按下位置)
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseDown[0] && ImGui::IsMousePosValid()) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            float tx = std::clamp(io.MousePos.x, 0.0f, screenW);
            float ty = std::clamp(io.MousePos.y, 0.0f, screenH);
            dl->AddCircleFilled(ImVec2(tx, ty), 10.0f, IM_COL32(255, 80, 80, 200));
            dl->AddCircle(ImVec2(tx, ty), 18.0f, IM_COL32(255, 255, 255, 180), 0, 2.0f);
        }
    }

    if (!data.inMatch) return;

    if (m_enableESP) drawESP(data, screenW, screenH);
    if (m_enableBones) drawBones(data, screenW, screenH);
    if (m_enableLootESP) drawLootESP(data, screenW, screenH);
    if (m_enableMinimap) drawMinimap(data, screenW, screenH);
    if (m_enablePlayerList) drawPlayerList(data, screenW, screenH);
    if (m_enableLoot) drawLootList(data, screenW, screenH);
    drawNotifications(data, screenW, screenH);
}

// =====================================================================
//  菜单面板
// =====================================================================

void DfmOverlay::drawMenu(const dfm::DrawDfmData& data) {
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.8f);

    // 允许拖拽移动 + 标题栏折叠三角
    if (!ImGui::Begin("DFM Overlay", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // 窗口被折叠 (点击了标题栏三角), 只显示标题栏
        ImGui::End();
        return;
    }

    if (data.inMatch) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "对局中");
        ImGui::SameLine();
        ImGui::Text("玩家:%d 物资:%d 箱子:%d",
            static_cast<int>(data.players.size()),
            static_cast<int>(data.lootItems.size()),
            static_cast<int>(data.containers.size()));
        // 相机状态指示
        bool camOk = std::isfinite(data.camLocX) && (std::fabs(data.camLocX) > 1.0f || std::fabs(data.camLocY) > 1.0f);
        ImGui::TextColored(camOk ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
            "相机:%s fov=%.0f team=%d", camOk ? "OK" : "N/A", data.camFOV, data.myTeamId);
    } else {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "等待对局...");
    }

    ImGui::Separator();

    if (m_menuExpanded) {
        ImGui::Checkbox("3D ESP", &m_enableESP);
        ImGui::Checkbox("骨骼", &m_enableBones);
        ImGui::Checkbox("射线", &m_enableSnapline);
        ImGui::Checkbox("小地图", &m_enableMinimap);
        ImGui::Checkbox("玩家列表", &m_enablePlayerList);
        ImGui::Checkbox("物资显示", &m_enableLoot);
        ImGui::Checkbox("物资3D标签", &m_enableLootESP);
        ImGui::Checkbox("物资箱", &m_enableContainer);
        ImGui::Checkbox("显示血条", &m_enableHP);
        ImGui::Checkbox("显示距离", &m_enableDistance);
        ImGui::Checkbox("显示名字", &m_enableName);
        ImGui::Checkbox("显示队友", &m_enableTeammate);
        ImGui::Checkbox("护甲/头盔", &m_enableArmor);
        ImGui::Checkbox("过滤弹药", &m_filterAmmo);
        ImGui::Checkbox("过滤杂物", &m_filterJunk);
        ImGui::Checkbox("日志输出", &g_runtimeLogEnabled);
        ImGui::SliderFloat("ESP距离(m)", &m_espMaxDist, 50.0f, 1000.0f, "%.0f");
        ImGui::SliderFloat("地图范围(m)", &m_minimapRange, 50.0f, 500.0f, "%.0f");
        ImGui::SliderFloat("物资距离(m)", &m_lootMaxDist, 20.0f, 300.0f, "%.0f");
    }

    // [修复] ▲▼ 替换为 ASCII <</>>, 避免超出字体字形范围显示为 ?
    if (ImGui::Button(m_menuExpanded ? "收起 <<" : "展开 >>", ImVec2(-1, 0))) {
        m_menuExpanded = !m_menuExpanded;
    }

    ImGui::End();
}

// =====================================================================
//  3D ESP 绘制 (方框 + 血条 + 名字 + 距离 + 射线)
// =====================================================================

int DfmOverlay::drawESP(const dfm::DrawDfmData& data, float screenW, float screenH) {
    bool camValid = std::isfinite(data.camLocX) && std::isfinite(data.camYaw)
        && (std::fabs(data.camLocX) > 1.0f || std::fabs(data.camLocY) > 1.0f)
        && data.camFOV >= 30.0f && data.camFOV <= 170.0f;
    if (!camValid) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        char diagBuf[256];
        snprintf(diagBuf, sizeof(diagBuf),
            "ESP off: cam(%.0f,%.0f,%.0f) yaw=%.1f fov=%.0f myPos(%.0f,%.0f,%.0f) players=%zu",
            data.camLocX, data.camLocY, data.camLocZ,
            data.camYaw, data.camFOV,
            data.myPos.x, data.myPos.y, data.myPos.z,
            data.players.size());
        dl->AddText(ImVec2(screenW * 0.5f - 150, 40),
                    IM_COL32(255, 100, 100, 220), diagBuf);
        return 0;
    }

    // 创建透明全屏窗口绘制 ESP (与 UE4Draw 相同方式)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##DfmESP", nullptr,
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float myCx = screenW * 0.5f;
    float myCy = screenH;
    int drawn = 0;

    // p.pos 取自 RootComponent / ReplicatedMovement, 经测试更接近角色 "脚底"。
    // DFM 使用的腾讯定制 UE5 骨架, 实际站立全高 (脚底 → 头顶) 约 200~210cm,
    // 这个 200 已经包含 "head 骨骼 → 头顶" 的 ~20cm 间距。
    //  - 远距离骨骼读取失败时使用此 fallback;
    //  - 任何情况下顶部还会再加 kHeadCrownExtra 以确保框顶卡在头顶 (而非脖子)。
    constexpr float kCharacterHeight    = 200.0f;
    constexpr float kCharacterHalfHeight = kCharacterHeight * 0.5f;
    // UE Skeleton "head" 骨骼原点位于颅骨底 (≈ 脖子顶端),
    // 距头顶冠点约 22~25cm。用于把方框上沿抬到真正的头顶。
    constexpr float kHeadCrownExtra     = 25.0f;

    for (const auto& p : data.players) {
        if (p.hp <= 0) continue;
        if (!m_enableTeammate && p.teamId >= 0 && p.teamId == data.myTeamId) continue;
        if (!hasValidPos(p.pos.x, p.pos.y, p.pos.z)) continue;

        float dist = distMeters(p.pos.x, p.pos.y, p.pos.z,
                                data.camLocX, data.camLocY, data.camLocZ);
        if (dist > m_espMaxDist || dist < kMinEspDistanceMeters) continue;

        ImU32 color;
        if (p.isAI) color = IM_COL32(255, 255, 0, 200);
        else if (p.teamId >= 0 && p.teamId == data.myTeamId) color = IM_COL32(0, 255, 0, 200);
        else color = IM_COL32(255, 50, 50, 230);

        // ---- 头/脚世界坐标: 优先用真实骨骼(已通过解密的 ComponentToWorld 投到世界空间),
        //      否则退回 pos + 估算身高。骨骼 0=Head, 13=RFoot, 16=LFoot, 4=Pelvis。 ----
        float footWX = p.pos.x;
        float footWY = p.pos.y;
        float footWZ = p.pos.z;
        float headWX = p.pos.x;
        float headWY = p.pos.y;
        // 注意: 这里给 fallback 也加上了 kHeadCrownExtra, 避免远距离 fallback 时框顶卡在脖子
        float headWZ = p.pos.z + kCharacterHeight + kHeadCrownExtra;
        float centerWX = p.pos.x;
        float centerWY = p.pos.y;
        float centerWZ = p.pos.z + kCharacterHalfHeight;

        if (p.bonesValid) {
            const auto& head   = p.bones[0];   // Head 骨 (颅底)
            const auto& pelvis = p.bones[4];   // Pelvis
            const auto& rFoot  = p.bones[13];
            const auto& lFoot  = p.bones[16];
            const bool headOkBone = std::isfinite(head.x) &&
                (std::fabs(head.x) > 1.0f || std::fabs(head.y) > 1.0f);
            const bool rFootOk = std::isfinite(rFoot.x) &&
                (std::fabs(rFoot.x) > 1.0f || std::fabs(rFoot.y) > 1.0f);
            const bool lFootOk = std::isfinite(lFoot.x) &&
                (std::fabs(lFoot.x) > 1.0f || std::fabs(lFoot.y) > 1.0f);

            if (headOkBone) {
                // 用 head 骨水平位置, Z 加上 25cm 让框顶到头顶冠
                headWX = head.x;
                headWY = head.y;
                headWZ = head.z + kHeadCrownExtra;
            }
            if (rFootOk && lFootOk) {
                footWX = (rFoot.x + lFoot.x) * 0.5f;
                footWY = (rFoot.y + lFoot.y) * 0.5f;
                footWZ = std::min(rFoot.z, lFoot.z);
            } else if (rFootOk) {
                footWX = rFoot.x; footWY = rFoot.y; footWZ = rFoot.z;
            } else if (lFootOk) {
                footWX = lFoot.x; footWY = lFoot.y; footWZ = lFoot.z;
            }
            // 中心点: 用 pelvis 更稳, 否则取头脚中点
            if (std::isfinite(pelvis.x) &&
                (std::fabs(pelvis.x) > 1.0f || std::fabs(pelvis.y) > 1.0f)) {
                centerWX = pelvis.x; centerWY = pelvis.y; centerWZ = pelvis.z;
            } else {
                centerWX = (headWX + footWX) * 0.5f;
                centerWY = (headWY + footWY) * 0.5f;
                centerWZ = (headWZ + footWZ) * 0.5f;
            }
        }

        float footSX = 0.0f, footSY = 0.0f, headSX = 0.0f, headSY = 0.0f;
        float edgeMinDepth = dist <= kRelaxedProjectionDistanceMeters
            ? kRelaxedProjectionMinDepth
            : 1.0f;
        bool footOk = projectToScreen(data, footWX, footWY, footWZ,
                                      screenW, screenH, edgeMinDepth,
                                      footSX, footSY);
        bool headOk = projectToScreen(data, headWX, headWY, headWZ,
                                      screenW, screenH, edgeMinDepth,
                                      headSX, headSY);

        float centerSX = 0.0f, centerSY = 0.0f, centerDepth = 0.0f, centerFocal = 0.0f;
        bool centerOk = projectToScreen(data, centerWX, centerWY, centerWZ,
                                        screenW, screenH,
                                        kRelaxedProjectionMinDepth,
                                        centerSX, centerSY,
                                        &centerDepth, &centerFocal);

        bool canDrawBox = false;
        float cx = 0.0f, topY = 0.0f, botY = 0.0f;

        // ---- 优先策略: 用所有 17 骨骼的屏幕投影包围盒, 确保 ESP 框与骨骼绘制 1:1 一致
        //      (避免 head/foot 单点投影偶发失败导致 box 与 bones 大小不一致 "一大一小")
        if (p.bonesValid) {
            float minX = 0, maxX = 0, minY = 0, maxY = 0;
            int n = 0;
            for (int i = 0; i < dfm::PlayerInfo::BONE_COUNT; ++i) {
                const auto& b = p.bones[i];
                if (!std::isfinite(b.x)) continue;
                if (std::fabs(b.x) <= 1.0f && std::fabs(b.y) <= 1.0f) continue;
                float bsx = 0, bsy = 0;
                if (!projectToScreen(data, b.x, b.y, b.z, screenW, screenH,
                                     edgeMinDepth, bsx, bsy)) continue;
                if (n == 0) { minX = maxX = bsx; minY = maxY = bsy; }
                else {
                    if (bsx < minX) minX = bsx; else if (bsx > maxX) maxX = bsx;
                    if (bsy < minY) minY = bsy; else if (bsy > maxY) maxY = bsy;
                }
                ++n;
            }
            if (n >= 6) {
                cx = (minX + maxX) * 0.5f;
                topY = minY;
                botY = maxY;
                canDrawBox = true;
            }
        }

        if (!canDrawBox && footOk && headOk) {
            cx = (footSX + headSX) * 0.5f;
            topY = std::min(footSY, headSY);
            botY = std::max(footSY, headSY);
            canDrawBox = true;
        } else if (!canDrawBox && centerOk && dist <= kRelaxedProjectionDistanceMeters) {
            float halfHeightPx = estimateFallbackHalfHeight(dist);
            if (footOk) halfHeightPx = std::max(halfHeightPx, std::fabs(footSY - centerSY));
            if (headOk) halfHeightPx = std::max(halfHeightPx, std::fabs(centerSY - headSY));
            if (centerFocal > 0.0f && centerDepth > kRelaxedProjectionMinDepth) {
                halfHeightPx = std::max(halfHeightPx, centerFocal * kCharacterHalfHeight / centerDepth);
            }

            halfHeightPx = std::clamp(halfHeightPx, 18.0f, 140.0f);
            cx = centerSX;
            topY = headOk ? headSY : (centerSY - halfHeightPx);
            botY = footOk ? footSY : (centerSY + halfHeightPx);
            if (topY > botY) std::swap(topY, botY);
            canDrawBox = std::isfinite(cx) && std::isfinite(topY) && std::isfinite(botY)
                && (botY - topY) >= 8.0f;
        }

        if (canDrawBox) {
            float boxH = std::fabs(botY - topY);
            float boxW = boxH * 0.48f;

            if (boxH > screenH * 0.9f) { /* 太大跳过 */ }
            else if (boxH < 8.0f) {
                // 远距离: 菱形标记 + 距离
                if (cx >= -20 && cx <= screenW + 20 && topY >= -20 && botY <= screenH + 20) {
                    float mx = cx, my = (topY + botY) * 0.5f;
                    float sz = 6.0f;
                    ImVec2 diamond[4] = {
                        ImVec2(mx, my - sz), ImVec2(mx + sz, my),
                        ImVec2(mx, my + sz), ImVec2(mx - sz, my)
                    };
                    dl->AddConvexPolyFilled(diamond, 4, (color & 0x00FFFFFF) | 0x60000000);
                    dl->AddPolyline(diamond, 4, color, ImDrawFlags_Closed, 1.5f);

                    if (m_enableDistance) {
                        char buf[32]; snprintf(buf, sizeof(buf), "%.0fm", dist);
                        dl->AddText(ImVec2(mx - 12, my + sz + 2), IM_COL32(200, 200, 200, 200), buf);
                    }
                    if (m_enableSnapline)
                        dl->AddLine(ImVec2(myCx, myCy), ImVec2(mx, my), IM_COL32(255, 255, 255, 40), 1.0f);
                    drawn++;
                }
            } else if (cx >= -boxW && cx <= screenW + boxW && topY >= -boxH && botY <= screenH + boxH) {
                // ---- 主 ESP 方框 ----
                float left = cx - boxW * 0.5f;
                float right = cx + boxW * 0.5f;

                // 黑色描边 + 彩色方框
                dl->AddRect(ImVec2(left - 1, topY - 1), ImVec2(right + 1, botY + 1),
                            IM_COL32(0, 0, 0, 150), 0, 0, 2.5f);
                dl->AddRect(ImVec2(left, topY), ImVec2(right, botY), color, 0, 0, 2.0f);

                // 射线
                if (m_enableSnapline)
                    dl->AddLine(ImVec2(myCx, myCy), ImVec2(cx, botY), IM_COL32(255, 255, 255, 100), 1.0f);

                // 血条 (左侧竖条)
                if (m_enableHP && p.maxHp > 0) {
                    float ratio = std::clamp(p.hp / p.maxHp, 0.0f, 1.0f);
                    float hpX = left - 5.0f;
                    float hpFill = topY + (botY - topY) * (1.0f - ratio);
                    dl->AddRectFilled(ImVec2(hpX - 3, topY), ImVec2(hpX, botY), IM_COL32(0, 0, 0, 150));
                    dl->AddRectFilled(ImVec2(hpX - 3, hpFill), ImVec2(hpX, botY), hpColor(ratio));
                }

                // 名字 (顶部居中, 带背景)
                if (m_enableName && !p.playerName.empty()) {
                    const char* label = p.playerName.c_str();
                    ImVec2 textSize = ImGui::CalcTextSize(label);
                    float tx = cx - textSize.x * 0.5f;
                    float ty = topY - textSize.y - 2;
                    dl->AddRectFilled(ImVec2(tx - 2, ty - 1), ImVec2(tx + textSize.x + 2, ty + textSize.y + 1),
                                      IM_COL32(0, 0, 0, 120), 2.0f);
                    dl->AddText(ImVec2(tx, ty), IM_COL32(255, 255, 255, 230), label);
                }

                // 距离 (底部居中)
                if (m_enableDistance) {
                    char distBuf[32]; snprintf(distBuf, sizeof(distBuf), "%.0fm", dist);
                    ImVec2 textSize = ImGui::CalcTextSize(distBuf);
                    dl->AddText(ImVec2(cx - textSize.x * 0.5f, botY + 2), IM_COL32(200, 200, 200, 200), distBuf);
                }

                // 武器 (右侧)
                if (!p.weapon.empty())
                    dl->AddText(ImVec2(right + 4, topY), IM_COL32(150, 200, 255, 200), p.weapon.c_str());

                // 护甲/头盔 (右侧下方)
                if (m_enableArmor && (p.armor > 0 || p.helmet > 0)) {
                    char armorBuf[48]; snprintf(armorBuf, sizeof(armorBuf), "A:%.0f H:%.0f", p.armor, p.helmet);
                    dl->AddText(ImVec2(right + 4, topY + 14), IM_COL32(80, 200, 255, 190), armorBuf);
                }

                drawn++;
            }
        } else {
            // ---- 屏幕外箭头 (与 UE4Draw fallback arrow 相同) ----
            constexpr float DEG2RAD = 3.14159265358979f / 180.0f;
            float dx = p.pos.x - data.camLocX, dy = p.pos.y - data.camLocY;
            float camYawRad = std::remainder(data.camYaw, 360.0f) * DEG2RAD;
            float fwdX = std::cos(camYawRad), fwdY = std::sin(camYawRad);
            float rightX = -fwdY, rightY = fwdX;
            float dotFwd = dx * fwdX + dy * fwdY;
            float dotRight = dx * rightX + dy * rightY;

            if (std::fabs(dotFwd) > 0.1f || std::fabs(dotRight) > 0.1f) {
                float angle = std::atan2(dotRight, dotFwd);
                constexpr float kMargin = 40.0f;
                float halfW = screenW * 0.5f - kMargin;
                float halfH = screenH * 0.5f - kMargin;
                float scale = 1.0f / std::max(std::fabs(std::sin(angle)) / halfH,
                                               std::fabs(std::cos(angle)) / halfW);
                float ax = std::clamp(myCx + std::sin(angle) * std::min(scale, halfW), kMargin, screenW - kMargin);
                float ay = std::clamp(screenH * 0.5f - std::cos(angle) * std::min(scale, halfH), kMargin, screenH - kMargin);

                // 三角箭头
                float arrowAngle = std::atan2(ay - screenH * 0.5f, ax - myCx);
                ImVec2 tip(ax, ay);
                ImVec2 fwd2(std::cos(arrowAngle), std::sin(arrowAngle));
                ImVec2 side(-fwd2.y, fwd2.x);
                ImVec2 bl(tip.x - fwd2.x * 16 + side.x * 7, tip.y - fwd2.y * 16 + side.y * 7);
                ImVec2 br(tip.x - fwd2.x * 16 - side.x * 7, tip.y - fwd2.y * 16 - side.y * 7);
                dl->AddTriangleFilled(tip, bl, br, color);
                dl->AddTriangle(tip, bl, br, IM_COL32(0, 0, 0, 200), 1.5f);

                // 距离标签
                if (m_enableDistance) {
                    char buf[32]; snprintf(buf, sizeof(buf), "%.0fm", dist);
                    dl->AddText(ImVec2(ax - 12, ay + 10), IM_COL32(200, 200, 200, 200), buf);
                }
                drawn++;
            }
        }
    }

    // [debug] 限频(每 2s)输出本帧 ESP 实际通过过滤的目标, 用于和截图对照
    {
        static std::atomic<uint64_t> lastDumpMs{0};
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t prev = lastDumpMs.load(std::memory_order_relaxed);
        if (nowMs - prev > 2000) {
            lastDumpMs.store(nowMs, std::memory_order_relaxed);
            std::string desc;
            int n = 0;
            for (const auto& p : data.players) {
                if (p.hp <= 0) continue;
                if (!hasValidPos(p.pos.x, p.pos.y, p.pos.z)) continue;
                if (!m_enableTeammate && p.teamId >= 0 && p.teamId == data.myTeamId) continue;
                float d = distMeters(p.pos.x, p.pos.y, p.pos.z,
                                     data.camLocX, data.camLocY, data.camLocZ);
                if (d > m_espMaxDist || d < kMinEspDistanceMeters) continue;
                if (++n > 12) { desc += "..."; break; }
                char buf[160];
                snprintf(buf, sizeof(buf),
                    "[%s%sname='%s' team=%d %.1fm pos=(%.0f,%.0f,%.0f)] ",
                    p.isAI ? "AI " : "",
                    (p.teamId >= 0 && p.teamId == data.myTeamId) ? "TM " : "",
                    p.playerName.c_str(), p.teamId, d,
                    p.pos.x, p.pos.y, p.pos.z);
                desc += buf;
            }
            LOG(LOG_LEVEL_INFO,
                "[espDraw] drawn=%d totalPlayers=%zu candidates=%s",
                drawn, data.players.size(), desc.c_str());
        }
    }

    ImGui::End();
    return drawn;
}

// =====================================================================
//  骨骼绘制 — 用线段连接骨骼关节点形成人体骨架
// =====================================================================
//
// 骨骼连接定义 (PlayerInfo.bones 索引):
//   0=Head, 1=Neck, 2=Chest, 3=Belly, 4=Pelvis
//   5=RShoulder, 6=RElbow, 7=RHand
//   8=LShoulder, 9=LElbow, 10=LHand
//   11=RThigh, 12=RKnee, 13=RFoot
//   14=LThigh, 15=LKnee, 16=LFoot
//
// 连接方式: 躯干(Head→Neck→Chest→Belly→Pelvis)
//           右臂(Chest→RShoulder→RElbow→RHand)
//           左臂(Chest→LShoulder→LElbow→LHand)
//           右腿(Pelvis→RThigh→RKnee→RFoot)
//           左腿(Pelvis→LThigh→LKnee→LFoot)

void DfmOverlay::drawBones(const dfm::DrawDfmData& data, float screenW, float screenH) {
    bool camValid = std::isfinite(data.camLocX) && std::isfinite(data.camYaw)
        && (std::fabs(data.camLocX) > 1.0f || std::fabs(data.camLocY) > 1.0f)
        && data.camFOV >= 30.0f && data.camFOV <= 170.0f;
    if (!camValid) return;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // 骨骼连接对: {from, to}
    static const int kBoneLinks[][2] = {
        // 躯干
        {0, 1},   // Head → Neck
        {1, 2},   // Neck → Chest
        {2, 3},   // Chest → Belly
        {3, 4},   // Belly → Pelvis
        // 右臂
        {2, 5},   // Chest → RShoulder
        {5, 6},   // RShoulder → RElbow
        {6, 7},   // RElbow → RHand
        // 左臂
        {2, 8},   // Chest → LShoulder
        {8, 9},   // LShoulder → LElbow
        {9, 10},  // LElbow → LHand
        // 右腿
        {4, 11},  // Pelvis → RThigh
        {11, 12}, // RThigh → RKnee
        {12, 13}, // RKnee → RFoot
        // 左腿
        {4, 14},  // Pelvis → LThigh
        {14, 15}, // LThigh → LKnee
        {15, 16}, // LKnee → LFoot
    };

    for (const auto& p : data.players) {
        if (p.hp <= 0 || !p.bonesValid) continue;
        if (!m_enableTeammate && p.teamId >= 0 && p.teamId == data.myTeamId) continue;

        float dist = distMeters(p.pos.x, p.pos.y, p.pos.z,
                                data.camLocX, data.camLocY, data.camLocZ);
        if (dist > m_espMaxDist || dist < kMinEspDistanceMeters) continue;

        // 颜色: AI=橙黄, 队友=亮绿, 敌人=亮青
        ImU32 boneColor;
        if (p.isAI) boneColor = IM_COL32(255, 196, 64, 235);
        else if (p.teamId >= 0 && p.teamId == data.myTeamId) boneColor = IM_COL32(64, 255, 128, 235);
        else boneColor = IM_COL32(64, 220, 255, 245);
        const ImU32 boneOutlineColor = IM_COL32(0, 0, 0, 210);

        // 投影所有骨骼到屏幕坐标
        float boneSX[dfm::PlayerInfo::BONE_COUNT];
        float boneSY[dfm::PlayerInfo::BONE_COUNT];
        bool  boneOk[dfm::PlayerInfo::BONE_COUNT];

        int projectedBoneCount = 0;
        for (int i = 0; i < dfm::PlayerInfo::BONE_COUNT; i++) {
            boneOk[i] = worldToScreen(data, p.bones[i].x, p.bones[i].y, p.bones[i].z,
                                       screenW, screenH, boneSX[i], boneSY[i]);
            if (boneOk[i]) ++projectedBoneCount;
        }

        if (projectedBoneCount < 4) continue;

        // 诊断: 周期性打印 head/foot 屏幕坐标 + 世界坐标, 看是否颠倒
        {
            static auto s_lastBoneDiag = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (now - s_lastBoneDiag > std::chrono::seconds(3)) {
                s_lastBoneDiag = now;
                __android_log_print(ANDROID_LOG_WARN, "DFM",
                    "[draw-bones] %s dist=%.1fm "
                    "headW=(%.0f,%.0f,%.0f) S=(%.0f,%.0f,ok=%d) "
                    "pelvW=(%.0f,%.0f,%.0f) S=(%.0f,%.0f,ok=%d) "
                    "rfootW=(%.0f,%.0f,%.0f) S=(%.0f,%.0f,ok=%d) "
                    "screenH=%.0f",
                    p.playerName.c_str(), dist,
                    p.bones[0].x, p.bones[0].y, p.bones[0].z, boneSX[0], boneSY[0], boneOk[0]?1:0,
                    p.bones[4].x, p.bones[4].y, p.bones[4].z, boneSX[4], boneSY[4], boneOk[4]?1:0,
                    p.bones[13].x, p.bones[13].y, p.bones[13].z, boneSX[13], boneSY[13], boneOk[13]?1:0,
                    screenH);
            }
        }

        // 线宽随距离缩放 (近粗远细)
        float thickness = std::clamp(3.6f - dist / 180.0f, 1.4f, 4.2f);
        float outlineThickness = thickness + 1.6f;

        // 绘制骨骼连线
        for (const auto& link : kBoneLinks) {
            int a = link[0], b = link[1];
            if (boneOk[a] && boneOk[b]) {
                dl->AddLine(ImVec2(boneSX[a], boneSY[a]),
                            ImVec2(boneSX[b], boneSY[b]),
                            boneOutlineColor, outlineThickness);
                dl->AddLine(ImVec2(boneSX[a], boneSY[a]),
                            ImVec2(boneSX[b], boneSY[b]),
                            boneColor, thickness);
            }
        }

        // 头部圆圈
        if (boneOk[0]) {
            // 头圆半径必须 < 人物投影身高的一定比例, 否则圆会从头一直盖到脚,
            // 视觉上等同于 "head 圆挪到了脚的位置". 用 head→pelvis 投影距离估算头大小.
            float headR;
            if (boneOk[4]) {
                // pelvis 在屏幕上, 用 (pelvis - head) 像素距离作为参考身高
                float dy = std::fabs(boneSY[4] - boneSY[0]);
                // 头部约占头-胯距离的 25% (颅顶到下颚)
                headR = std::clamp(dy * 0.25f, 2.5f, 15.0f);
            } else {
                headR = std::clamp(800.0f / dist, 2.5f, 8.0f);
            }
            dl->AddCircle(ImVec2(boneSX[0], boneSY[0]), headR, boneOutlineColor, 12, outlineThickness);
            dl->AddCircle(ImVec2(boneSX[0], boneSY[0]), headR, boneColor, 12, thickness);
        }

        // 关节点 (近距离显示)
        if (dist < 120.0f) {
            for (int i = 0; i < dfm::PlayerInfo::BONE_COUNT; i++) {
                if (boneOk[i]) {
                    dl->AddCircleFilled(ImVec2(boneSX[i], boneSY[i]), 3.0f, boneOutlineColor);
                    dl->AddCircleFilled(ImVec2(boneSX[i], boneSY[i]), 2.0f,
                                        boneColor);
                }
            }
        }
    }
}

// =====================================================================
//  物资 3D ESP — 在世界坐标位置显示物资名称标签
// =====================================================================

void DfmOverlay::drawLootESP(const dfm::DrawDfmData& data, float screenW, float screenH) {
    bool camValid = std::isfinite(data.camLocX) && std::isfinite(data.camYaw)
        && (std::fabs(data.camLocX) > 1.0f || std::fabs(data.camLocY) > 1.0f)
        && data.camFOV >= 30.0f && data.camFOV <= 170.0f;
    if (!camValid) return;

    bool myPosValid = hasValidPos(data.myPos.x, data.myPos.y, data.myPos.z);
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // 物资颜色分类 (基于 ItemID 大类)
    auto lootColor = [](int32_t itemId) -> ImU32 {
        if (itemId <= 0) return IM_COL32(200, 200, 200, 200);
        int mainType = 0;
        if (itemId >= 10000000) {
            // MMSSXXXXXXX 格式: 前2位为大类
            int tmp = itemId;
            while (tmp >= 100) tmp /= 10;
            mainType = tmp;
        }
        switch (mainType) {
            case 10: return IM_COL32(255, 80, 80, 230);    // 武器 - 红
            case 11: return IM_COL32(80, 180, 255, 230);   // 装备 - 蓝
            case 13: return IM_COL32(150, 220, 150, 220);  // 配件 - 浅绿
            case 14: return IM_COL32(100, 255, 100, 230);  // 药品 - 绿
            case 15: return IM_COL32(255, 215, 0, 230);    // 收集品 - 金
            case 21: return IM_COL32(255, 150, 50, 230);   // 投掷物 - 橙
            case 37: return IM_COL32(180, 180, 130, 200);  // 弹药 - 灰黄
            default: return IM_COL32(200, 200, 200, 200);  // 其他 - 灰白
        }
    };

    for (const auto& item : data.lootItems) {
        if (!hasValidPos(item.pos.x, item.pos.y, item.pos.z)) continue;

        // 物资过滤 (基于 ItemID 大类)
        if (item.itemId > 0) {
            int mainType = 0;
            if (item.itemId >= 10000000) {
                int tmp = item.itemId;
                while (tmp >= 100) tmp /= 10;
                mainType = tmp;
            }
            if (m_filterAmmo && mainType == 37) continue;   // 过滤弹药
            if (m_filterJunk && mainType == 16) continue;   // 过滤杂物
        }

        // 距离过滤 (用相机位置计算)
        float dist = distMeters(item.pos.x, item.pos.y, item.pos.z,
                                data.camLocX, data.camLocY, data.camLocZ);
        if (dist > m_lootMaxDist) continue;

        // 世界坐标投影到屏幕 (物资位置略微抬高避免贴地)
        float sx, sy;
        if (!worldToScreen(data, item.pos.x, item.pos.y, item.pos.z + 30.0f,
                           screenW, screenH, sx, sy))
            continue;

        // 裁剪屏幕外
        if (sx < -50 || sx > screenW + 50 || sy < -20 || sy > screenH + 20) continue;

        ImU32 color = lootColor(item.itemId);

        // 方框大小随距离缩放 (近大远小)
        float boxSize = std::clamp(8.0f * (1.0f - dist / m_lootMaxDist) + 4.0f, 4.0f, 14.0f);

        // 绘制物资小方框
        dl->AddRectFilled(ImVec2(sx - boxSize, sy - boxSize),
                          ImVec2(sx + boxSize, sy + boxSize),
                          (color & 0x00FFFFFF) | 0x30000000, 1.0f); // 半透明填充
        dl->AddRect(ImVec2(sx - boxSize, sy - boxSize),
                    ImVec2(sx + boxSize, sy + boxSize),
                    color, 0, 0, 1.5f); // 外框

        // 构造显示文本: "名称 x数量 距离m"
        char label[128];
        if (item.stackCount > 1) {
            snprintf(label, sizeof(label), "%s x%d %.0fm",
                     item.itemName.c_str(), item.stackCount, dist);
        } else {
            snprintf(label, sizeof(label), "%s %.0fm",
                     item.itemName.c_str(), dist);
        }

        // 文本尺寸 (显示在方框上方)
        ImVec2 textSize = ImGui::CalcTextSize(label);
        float tx = sx - textSize.x * 0.5f;
        float ty = sy - boxSize - textSize.y - 3.0f;

        // 背景框
        dl->AddRectFilled(ImVec2(tx - 2, ty - 1),
                          ImVec2(tx + textSize.x + 2, ty + textSize.y + 1),
                          IM_COL32(0, 0, 0, 120), 2.0f);

        // 文本
        dl->AddText(ImVec2(tx, ty), color, label);
    }

    // 物资箱 3D 标签
    if (m_enableContainer) {
        for (const auto& cont : data.containers) {
            if (!hasValidPos(cont.pos.x, cont.pos.y, cont.pos.z)) continue;
            if (cont.finished) continue; // 跳过已搜完的箱子

            float dist = distMeters(cont.pos.x, cont.pos.y, cont.pos.z,
                                    data.camLocX, data.camLocY, data.camLocZ);
            if (dist > m_lootMaxDist * 1.5f) continue;  // 箱子显示距离稍远

            float sx, sy;
            if (!worldToScreen(data, cont.pos.x, cont.pos.y, cont.pos.z + 50.0f,
                               screenW, screenH, sx, sy))
                continue;

            if (sx < -50 || sx > screenW + 50 || sy < -20 || sy > screenH + 20) continue;

            ImU32 boxColor = cont.opened ? IM_COL32(120, 120, 120, 180)
                                         : IM_COL32(255, 200, 50, 230);

            // 箱子方框 (比物资稍大, 用菱形区分)
            float bsz = std::clamp(10.0f * (1.0f - dist / (m_lootMaxDist * 1.5f)) + 5.0f, 5.0f, 16.0f);
            ImVec2 diamond[4] = {
                ImVec2(sx, sy - bsz), ImVec2(sx + bsz, sy),
                ImVec2(sx, sy + bsz), ImVec2(sx - bsz, sy)
            };
            dl->AddConvexPolyFilled(diamond, 4, (boxColor & 0x00FFFFFF) | 0x30000000);
            dl->AddPolyline(diamond, 4, boxColor, ImDrawFlags_Closed, 1.5f);

            // 构造标签: "[类型] 物品数 距离"
            char label[128];
            if (!cont.items.empty()) {
                snprintf(label, sizeof(label), "[%s] %d件 %.0fm",
                         cont.boxType.c_str(), static_cast<int>(cont.items.size()), dist);
            } else {
                snprintf(label, sizeof(label), "[%s] %s %.0fm",
                         cont.boxType.c_str(),
                         cont.opened ? "已开" : "未开", dist);
            }

            ImVec2 textSize = ImGui::CalcTextSize(label);
            float tx = sx - textSize.x * 0.5f;
            float ty = sy - bsz - textSize.y - 3.0f;

            dl->AddRectFilled(ImVec2(tx - 2, ty - 1),
                              ImVec2(tx + textSize.x + 2, ty + textSize.y + 1),
                              IM_COL32(0, 0, 0, 120), 2.0f);
            dl->AddText(ImVec2(tx, ty), boxColor, label);

            // 显示箱内物品名 (最多3个)
            if (!cont.items.empty()) {
                float iy = ty + textSize.y + 2;
                int shown = 0;
                for (const auto& it : cont.items) {
                    if (shown >= 3) break;
                    char itemLabel[96];
                    snprintf(itemLabel, sizeof(itemLabel), "  %s x%d", it.name.c_str(), it.count);
                    ImVec2 itSize = ImGui::CalcTextSize(itemLabel);
                    float itx = sx - itSize.x * 0.5f;
                    dl->AddRectFilled(ImVec2(itx - 1, iy),
                                      ImVec2(itx + itSize.x + 1, iy + itSize.y),
                                      IM_COL32(0, 0, 0, 100), 1.0f);
                    dl->AddText(ImVec2(itx, iy), IM_COL32(220, 220, 180, 200), itemLabel);
                    iy += itSize.y + 1;
                    shown++;
                }
                if (cont.items.size() > 3) {
                    char moreBuf[32];
                    snprintf(moreBuf, sizeof(moreBuf), "  +%d...",
                             static_cast<int>(cont.items.size()) - 3);
                    dl->AddText(ImVec2(sx - 20, iy), IM_COL32(180, 180, 180, 160), moreBuf);
                }
            }
        }
    }
}

// =====================================================================
//  小地图 — 左下角圆形
// =====================================================================

void DfmOverlay::drawMinimap(const dfm::DrawDfmData& data, float screenW, float screenH) {
    bool myPosValid = hasValidPos(data.myPos.x, data.myPos.y, data.myPos.z);
    float cx = m_minimapSize * 0.5f + 15.0f;
    float cy = m_minimapSize * 0.5f + 50.0f;  // 左上角, 与游戏内置小地图位置对齐
    float radius = m_minimapSize * 0.5f;
    float rangeUU = m_minimapRange * 100.0f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();

    dl->AddCircleFilled(ImVec2(cx, cy), radius, IM_COL32(0, 0, 0, 140));
    dl->AddCircle(ImVec2(cx, cy), radius, IM_COL32(255, 255, 255, 100), 0, 1.5f);
    dl->AddCircleFilled(ImVec2(cx, cy), 4.0f, IM_COL32(0, 200, 255, 255));

    if (!myPosValid) {
        dl->AddText(ImVec2(cx - 30, cy - 6), IM_COL32(255, 100, 100, 200), "无定位");
        return;
    }

    int drawnPlayers = 0;
    for (const auto& p : data.players) {
        if (!hasValidPos(p.pos.x, p.pos.y, p.pos.z)) continue;
        if (p.hp <= 0) continue;
        if (!m_enableTeammate && p.teamId >= 0 && p.teamId == data.myTeamId) continue;

        float dx = p.pos.x - data.myPos.x;
        float dy = p.pos.y - data.myPos.y;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > rangeUU) continue;

        float mapX = cx + (dx / rangeUU) * radius;
        float mapY = cy + (dy / rangeUU) * radius;

        ImU32 color;
        if (p.isAI) color = IM_COL32(255, 255, 0, 200);
        else if (p.teamId >= 0 && p.teamId == data.myTeamId) color = IM_COL32(0, 255, 0, 200);
        else color = IM_COL32(255, 50, 50, 230);

        dl->AddCircleFilled(ImVec2(mapX, mapY), 3.5f, color);
        drawnPlayers++;
    }

    if (m_enableLoot) {
        for (const auto& item : data.lootItems) {
            if (!hasValidPos(item.pos.x, item.pos.y, item.pos.z)) continue;
            float dx = item.pos.x - data.myPos.x;
            float dy = item.pos.y - data.myPos.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > rangeUU || dist > m_lootMaxDist * 100.0f) continue;

            float mapX = cx + (dx / rangeUU) * radius;
            float mapY = cy + (dy / rangeUU) * radius;
            dl->AddCircleFilled(ImVec2(mapX, mapY), 2.0f, IM_COL32(200, 200, 200, 150));
        }
    }

    if (m_enableContainer) {
        for (const auto& cont : data.containers) {
            if (!hasValidPos(cont.pos.x, cont.pos.y, cont.pos.z)) continue;
            float dx = cont.pos.x - data.myPos.x;
            float dy = cont.pos.y - data.myPos.y;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > rangeUU) continue;

            float mapX = cx + (dx / rangeUU) * radius;
            float mapY = cy + (dy / rangeUU) * radius;
            ImU32 boxColor = cont.opened ? IM_COL32(100, 100, 100, 120) : IM_COL32(255, 200, 50, 200);
            dl->AddRectFilled(ImVec2(mapX - 2, mapY - 2), ImVec2(mapX + 2, mapY + 2), boxColor);
        }
    }

    char label[64];
    snprintf(label, sizeof(label), "%.0fm | %d人", m_minimapRange, drawnPlayers);
    dl->AddText(ImVec2(cx - radius + 5, cy + radius - 16), IM_COL32(255, 255, 255, 180), label);
}

// =====================================================================
//  玩家列表面板 (右侧)
// =====================================================================

void DfmOverlay::drawPlayerList(const dfm::DrawDfmData& data, float screenW, float screenH) {
    ImGui::SetNextWindowPos(ImVec2(screenW - 300, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(290, 400), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.7f);

    if (!ImGui::Begin("玩家", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    ImGui::Text("存活: %d / %d", data.aliveCount, data.totalCount);
    ImGui::Separator();

    bool myPosValid = hasValidPos(data.myPos.x, data.myPos.y, data.myPos.z);

    for (const auto& p : data.players) {
        if (p.hp <= 0 && !p.isAI) continue;

        const char* tag = p.isAI ? "[AI]" : (p.teamId >= 0 && p.teamId == data.myTeamId ? "[友]" : "[敌]");
        ImVec4 tagColor = p.isAI ? ImVec4(1, 1, 0, 1) :
            (p.teamId >= 0 && p.teamId == data.myTeamId ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0.3f, 0.3f, 1));

        ImGui::TextColored(tagColor, "%s", tag);
        ImGui::SameLine();
        ImGui::Text("%s", p.playerName.c_str());

        if (m_enableHP) {
            ImGui::SameLine();
            float ratio = (p.maxHp > 0) ? p.hp / p.maxHp : 0.0f;
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(hpColor(ratio)),
                " HP:%.0f/%.0f", p.hp, p.maxHp);
        }

        if (m_enableDistance && myPosValid && hasValidPos(p.pos.x, p.pos.y, p.pos.z)) {
            float dist = distMeters(p.pos.x, p.pos.y, p.pos.z, data.myPos.x, data.myPos.y, data.myPos.z);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), " %.0fm", dist);
        }

        if (!p.weapon.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1, 1), " %s", p.weapon.c_str());
        }

        if (m_enableArmor && (p.armor > 0 || p.helmet > 0)) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.3f, 0.7f, 1, 1), " A:%.0f H:%.0f", p.armor, p.helmet);
        }
    }

    ImGui::End();
}

// =====================================================================
//  物资列表面板 (左侧)
// =====================================================================

void DfmOverlay::drawLootList(const dfm::DrawDfmData& data, float screenW, float screenH) {
    if (data.lootItems.empty() && data.containers.empty()) return;

    bool myPosValid = hasValidPos(data.myPos.x, data.myPos.y, data.myPos.z);

    ImGui::SetNextWindowPos(ImVec2(10, screenH * 0.4f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.7f);

    if (!ImGui::Begin("物资", nullptr, ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::End();
        return;
    }

    if (!data.lootItems.empty() && myPosValid) {
        ImGui::Text("附近物资 (%d):", static_cast<int>(data.lootItems.size()));
        ImGui::Separator();

        struct SortedItem { float dist; const dfm::LootItem* item; };
        std::vector<SortedItem> sorted;
        sorted.reserve(data.lootItems.size());
        for (const auto& i : data.lootItems) {
            if (!hasValidPos(i.pos.x, i.pos.y, i.pos.z)) continue;
            // 应用物资过滤
            if (i.itemId > 0) {
                int mt = 0;
                if (i.itemId >= 10000000) { int t = i.itemId; while (t >= 100) t /= 10; mt = t; }
                if (m_filterAmmo && mt == 37) continue;
                if (m_filterJunk && mt == 16) continue;
            }
            float d = distMeters(i.pos.x, i.pos.y, i.pos.z, data.myPos.x, data.myPos.y, data.myPos.z);
            if (d <= m_lootMaxDist) sorted.push_back({d, &i});
        }
        std::sort(sorted.begin(), sorted.end(), [](const SortedItem& a, const SortedItem& b) { return a.dist < b.dist; });

        int shown = 0;
        for (const auto& s : sorted) {
            if (shown >= 20) break;
            ImGui::Text("%.0fm %s x%d", s.dist, s.item->itemName.c_str(), s.item->stackCount);
            shown++;
        }
        if (sorted.size() > 20) ImGui::Text("... 还有 %d 个", static_cast<int>(sorted.size()) - 20);
    }

    if (m_enableContainer && !data.containers.empty()) {
        ImGui::Spacing();
        int openCount = 0, closedCount = 0;
        for (const auto& c : data.containers) {
            if (c.opened || c.finished) openCount++; else closedCount++;
        }
        ImGui::Text("箱子 (%d): 未开%d 已开%d", static_cast<int>(data.containers.size()), closedCount, openCount);
        ImGui::Separator();

        int shown = 0;
        for (const auto& c : data.containers) {
            if (shown >= 10) break;
            if (c.finished) continue;

            float dist = myPosValid && hasValidPos(c.pos.x, c.pos.y, c.pos.z) ?
                distMeters(c.pos.x, c.pos.y, c.pos.z, data.myPos.x, data.myPos.y, data.myPos.z) : -1;

            ImVec4 stateColor = c.opened ? ImVec4(0.5f, 0.5f, 0.5f, 1) : ImVec4(1, 0.9f, 0.3f, 1);
            ImGui::TextColored(stateColor, "%s %s",
                c.opened ? "[已开]" : "[未开]", c.boxType.c_str());
            if (dist >= 0) { ImGui::SameLine(); ImGui::Text("%.0fm", dist); }

            for (const auto& it : c.items) {
                ImGui::Text("  - %s x%d", it.name.c_str(), it.count);
            }
            shown++;
        }
    }

    ImGui::End();
}

// =====================================================================
//  通知绘制 — 屏幕上方浮动消息 (击杀/受伤/新玩家)
// =====================================================================

void DfmOverlay::drawNotifications(const dfm::DrawDfmData& data, float screenW, float screenH) {
    auto now = Clock::now();

    // 合入新通知
    for (const auto& n : data.notifications) {
        m_activeNotifications.push_back(n);
    }

    // 计算 deltaTime
    float dt = 0.016f;
    if (m_lastNotifTime.time_since_epoch().count() != 0) {
        dt = std::chrono::duration<float>(now - m_lastNotifTime).count();
        if (dt > 0.5f) dt = 0.016f;
    }
    m_lastNotifTime = now;

    // 更新并绘制
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float y = 60.0f;

    for (auto it = m_activeNotifications.begin(); it != m_activeNotifications.end(); ) {
        it->timeLeft -= dt;
        if (it->timeLeft <= 0) {
            it = m_activeNotifications.erase(it);
            continue;
        }

        float alpha = std::min(it->timeLeft, 1.0f);
        uint32_t col = (it->color & 0x00FFFFFF) | (static_cast<uint32_t>(alpha * 255) << 24);

        ImVec2 textSize = ImGui::CalcTextSize(it->text.c_str());
        float tx = (screenW - textSize.x) * 0.5f;

        dl->AddRectFilled(ImVec2(tx - 6, y - 2),
                          ImVec2(tx + textSize.x + 6, y + textSize.y + 2),
                          IM_COL32(0, 0, 0, static_cast<int>(alpha * 150)), 3.0f);
        dl->AddText(ImVec2(tx, y), col, it->text.c_str());

        y += textSize.y + 6;
        ++it;
    }

    // 限制最多显示 8 条
    while (m_activeNotifications.size() > 8) {
        m_activeNotifications.erase(m_activeNotifications.begin());
    }
}

} // namespace dfmdraw
