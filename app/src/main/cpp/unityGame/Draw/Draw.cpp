#include "Draw.h"
#include "../SharedGameData.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace draw {

// ═══════════════════════════════════════════════════════════════════════════════
// 小地图坐标映射
// ═══════════════════════════════════════════════════════════════════════════════

static constexpr float MAP_MIN_X = -50.0f;
static constexpr float MAP_MAX_X = 50.0f;
static constexpr float MAP_MIN_Z = -50.0f;
static constexpr float MAP_MAX_Z = 50.0f;

// 根据旋转模式变换归一化坐标
static void applyRotation(float& nx, float& nz, int rot) {
    float tx, tz;
    switch (rot) {
        default:
        case 0:  tx = nx;        tz = nz;        break;
        case 1:  tx = 1.f - nz;  tz = nx;        break;  // 90°
        case 2:  tx = 1.f - nx;  tz = 1.f - nz;  break;  // 180°
        case 3:  tx = nx;        tz = 1.f - nz;  break;  // 270°
    }
    nx = tx;
    nz = tz;
}

ImVec2 GameOverlay::worldToMinimap(const lol::UnityVector3& worldPos,
                                   ImVec2 minimapOrigin, float minimapSize,
                                   int rot) {
    float nx = (worldPos.x - MAP_MIN_X) / (MAP_MAX_X - MAP_MIN_X);
    float nz = (MAP_MAX_Z - worldPos.z) / (MAP_MAX_Z - MAP_MIN_Z);
    nx = std::clamp(nx, 0.0f, 1.0f);
    nz = std::clamp(nz, 0.0f, 1.0f);
    applyRotation(nx, nz, rot);
    return {minimapOrigin.x + nx * minimapSize,
            minimapOrigin.y + nz * minimapSize};
}

ImVec2 GameOverlay::xzToMinimap(float wx, float wz,
                                ImVec2 origin, float size,
                                int rot) {
    float nx = (wx - MAP_MIN_X) / (MAP_MAX_X - MAP_MIN_X);
    float nz = (MAP_MAX_Z - wz) / (MAP_MAX_Z - MAP_MIN_Z);
    nx = std::clamp(nx, 0.f, 1.f);
    nz = std::clamp(nz, 0.f, 1.f);
    applyRotation(nx, nz, rot);
    return {origin.x + nx * size,
            origin.y + nz * size};
}

// ═══════════════════════════════════════════════════════════════════════════════
// drawOverlay — 入口
// ═══════════════════════════════════════════════════════════════════════════════

void GameOverlay::drawOverlay(const lol::MiniMapData& data, bool inBattle) {
    if (inBattle) {
        if (m_enableESP)        drawESP(data);
        if (m_enableRadar)      drawRadar(data);
        if (m_enableSkillRange) drawSkillRange(data);
        if (m_enableMinion)     drawMinionESP(data);
    }
    // Game Info 最后绘制，确保它在最上层可接收触摸/拖拽
    if (m_enableInfo) drawInfoPanel(data, inBattle);
}

// ═══════════════════════════════════════════════════════════════════════════════
// drawInfoPanel — 状态/信息面板（左上角，按钮折叠）
// ═══════════════════════════════════════════════════════════════════════════════

void GameOverlay::drawInfoPanel(const lol::MiniMapData& data, bool inBattle) {
    // 当前关闭了 ImGui ini 持久化，FirstUseEver 会导致位置反复被重置。
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.65f);
    ImGui::Begin("Game Info", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);

    // ── 折叠/展开 按钮 ──
    if (ImGui::Button(m_panelExpanded ? "[-] Hide" : "[+] Show", ImVec2(120, 0))) {
        m_panelExpanded = !m_panelExpanded;
    }

    // ── 触摸旋转切换按钮 ──
    ImGui::SameLine();
    {
        char rotBtn[32];
        snprintf(rotBtn, sizeof(rotBtn), "Rot:%s", touch_input::getRotationLabel());
        if (ImGui::Button(rotBtn, ImVec2(100, 0))) {
            touch_input::cycleRotation();
        }
    }

    ImGui::SameLine();
    if (ImGui::Button(touch_input::isRotationCorrectionLocked() ? "Corr:ON" : "Corr:OFF", ImVec2(90, 0))) {
        touch_input::toggleRotationCorrectionLock();
    }

    ImGui::SameLine();
    if (ImGui::Button(touch_input::isSurfaceRotationLockEnabled() ? "Surf:ON" : "Surf:OFF", ImVec2(90, 0))) {
        touch_input::toggleSurfaceRotationLock();
    }

    if (!inBattle) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "[Waiting]");
        ImGui::End();
        return;
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "IN BATTLE");

    if (m_panelExpanded) {
        ImGui::Separator();

        // ── 功能开关 ──
        ImGui::Checkbox("ESP",       &m_enableESP);
        ImGui::SameLine();
        ImGui::Checkbox("Radar",     &m_enableRadar);
        ImGui::SameLine();
        ImGui::Checkbox("HeroInfo",  &m_enableHeroInfo);
        ImGui::SameLine();
        ImGui::Checkbox("Wards",     &m_enableWards);
        ImGui::SameLine();
        ImGui::Checkbox("SkillRng",  &m_enableSkillRange);
        ImGui::SameLine();
        ImGui::Checkbox("Minion",    &m_enableMinion);
        ImGui::SameLine();
        ImGui::Checkbox("AutoFarm",  &m_enableAutoFarm);
        if (m_enableMinion) {
            ImGui::SliderFloat("LastHit HP", &m_lastHitHpThreshold, 10.0f, 300.0f, "%.0f");
        }
        SharedGameData::getInstance().setAutoClearMinionsEnabled(m_enableAutoFarm);
        ImGui::Separator();

        // ── 敌方英雄信息 ──
        if (m_enableHeroInfo && ImGui::CollapsingHeader("Enemy Heroes", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Count: %d", (int)data.enemyHeroes.size());
            ImGui::Spacing();

            for (size_t i = 0; i < data.enemyHeroes.size(); i++) {
                const auto& hero = data.enemyHeroes[i];
                ImGui::PushID((int)i);

                const char* name = hero.heroName.empty() ? "Unknown" : hero.heroName.c_str();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                                   "[Lv.%u] %s", hero.heroLevel, name);

                if (!hero.summonerName.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                       "(%s)", hero.summonerName.c_str());
                }

                float hpRatio = (hero.maxHp > 0.0f) ? (hero.curHp / hero.maxHp) : 0.0f;
                hpRatio = std::clamp(hpRatio, 0.0f, 1.0f);
                ImVec4 hpColor = (hpRatio > 0.5f)  ? ImVec4(0.2f, 0.9f, 0.2f, 1.0f) :
                                 (hpRatio > 0.25f) ? ImVec4(0.9f, 0.9f, 0.2f, 1.0f) :
                                                      ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
                char hpOverlay[64];
                snprintf(hpOverlay, sizeof(hpOverlay), "%.0f / %.0f", hero.curHp, hero.maxHp);

                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, hpColor);
                ImGui::ProgressBar(hpRatio, ImVec2(280, 0), hpOverlay);
                ImGui::PopStyleColor();

                if (hero.hasWorldPos) {
                    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                                       "  Pos: (%.0f, %.0f, %.0f)",
                                       hero.worldPos.x, hero.worldPos.y, hero.worldPos.z);
                }

                ImGui::Separator();
                ImGui::PopID();
            }
        }

        // ── 眼位信息 ──
        if (m_enableWards && !data.wards.empty()) {
            if (ImGui::CollapsingHeader("Wards", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Text("Count: %d", (int)data.wards.size());
                for (size_t i = 0; i < data.wards.size(); i++) {
                    const auto& ward = data.wards[i];
                    if (ward.hasWorldPos) {
                        ImGui::Text("  Ward#%d (%.0f, %.0f) type=%d",
                                    (int)(i + 1),
                                    ward.worldPos.x, ward.worldPos.z,
                                    ward.iconType);
                    }
                }
            }
        }
    }

    ImGui::End();
}

// ═══════════════════════════════════════════════════════════════════════════════
// drawESP — ESP 方框覆盖层（World-to-Screen 屏幕投影）
// ═══════════════════════════════════════════════════════════════════════════════

void GameOverlay::drawESP(const lol::MiniMapData& data) {
    ImGuiIO& espIo = ImGui::GetIO();
    const float screenW = espIo.DisplaySize.x;
    const float screenH = espIo.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##ESP", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* espDraw = ImGui::GetWindowDrawList();

    for (const auto& hero : data.enemyHeroes) {
        if (!hero.hasScreenPos) continue;

        float sx = hero.screenX;
        float sy = screenH - hero.screenY;

        constexpr float kMargin = 100.0f;
        if (sx < -kMargin || sx > screenW + kMargin ||
            sy < -kMargin || sy > screenH + kMargin)
            continue;

        const float boxW = 60.0f;
        const float boxH = 120.0f;
        ImVec2 boxMin(sx - boxW * 0.5f, sy - boxH * 0.8f);
        ImVec2 boxMax(sx + boxW * 0.5f, sy + boxH * 0.2f);

        float hpRatio = (hero.maxHp > 0.0f) ? (hero.curHp / hero.maxHp) : 0.0f;
        hpRatio = std::clamp(hpRatio, 0.0f, 1.0f);
        ImU32 boxColor = (hpRatio > 0.5f)  ? IM_COL32(0, 255, 0, 220) :
                         (hpRatio > 0.25f) ? IM_COL32(255, 255, 0, 220) :
                                              IM_COL32(255, 0, 0, 220);

        espDraw->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 40));
        espDraw->AddRect(boxMin, boxMax, boxColor, 0.0f, 0, 2.0f);

        const float hpBarH = 4.0f;
        const float hpBarY = boxMin.y - hpBarH - 2.0f;
        ImVec2 hpBgMin(boxMin.x, hpBarY);
        ImVec2 hpBgMax(boxMax.x, hpBarY + hpBarH);
        ImVec2 hpFgMax(boxMin.x + (boxMax.x - boxMin.x) * hpRatio, hpBarY + hpBarH);
        espDraw->AddRectFilled(hpBgMin, hpBgMax, IM_COL32(0, 0, 0, 160));
        espDraw->AddRectFilled(hpBgMin, hpFgMax, boxColor);

        const char* name = hero.heroName.empty() ? nullptr : hero.heroName.c_str();
        if (name) {
            ImVec2 textSize = ImGui::CalcTextSize(name);
            float textX = sx - textSize.x * 0.5f;
            float textY = hpBarY - textSize.y - 1.0f;
            espDraw->AddText(ImVec2(textX + 1, textY + 1), IM_COL32(0, 0, 0, 200), name);
            espDraw->AddText(ImVec2(textX, textY), IM_COL32(255, 255, 255, 240), name);
        }

        char lvlBuf[16];
        snprintf(lvlBuf, sizeof(lvlBuf), "Lv%u", hero.heroLevel);
        espDraw->AddText(ImVec2(boxMax.x + 2, boxMin.y),
                         IM_COL32(255, 255, 100, 240), lvlBuf);

        char hpBuf[32];
        snprintf(hpBuf, sizeof(hpBuf), "%.0f/%.0f", hero.curHp, hero.maxHp);
        ImVec2 hpTextSize = ImGui::CalcTextSize(hpBuf);
        espDraw->AddText(ImVec2(sx - hpTextSize.x * 0.5f, boxMax.y + 2),
                         IM_COL32(200, 200, 200, 220), hpBuf);
    }

    // ── 眼/守卫 ESP 方框 ──
        if (m_enableWards) {
            for (const auto& ward : data.wards) {
                if (!ward.hasScreenPos) continue;

                float sx = ward.screenX;
                float sy = screenH - ward.screenY;

                constexpr float kMargin = 100.0f;
                if (sx < -kMargin || sx > screenW + kMargin ||
                    sy < -kMargin || sy > screenH + kMargin)
                    continue;

                const float boxW = 30.0f;
                const float boxH = 30.0f;
                ImVec2 boxMin(sx - boxW * 0.5f, sy - boxH * 0.5f);
                ImVec2 boxMax(sx + boxW * 0.5f, sy + boxH * 0.5f);

                const ImU32 wardColor = IM_COL32(50, 150, 255, 220);
                espDraw->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 40));
                espDraw->AddRect(boxMin, boxMax, wardColor, 0.0f, 0, 2.0f);

                const char* label = "Ward";
                ImVec2 textSize = ImGui::CalcTextSize(label);
                float textX = sx - textSize.x * 0.5f;
                float textY = boxMin.y - textSize.y - 1.0f;
                espDraw->AddText(ImVec2(textX + 1, textY + 1), IM_COL32(0, 0, 0, 200), label);
                espDraw->AddText(ImVec2(textX, textY), IM_COL32(100, 200, 255, 240), label);
            }
    }

    ImGui::End();
}

// ═══════════════════════════════════════════════════════════════════════════════
// drawRadar — 小地图雷达（全图视野, 显示所有玩家）
// ═══════════════════════════════════════════════════════════════════════════════

void GameOverlay::drawRadar(const lol::MiniMapData& data) {
    ImGuiIO& io = ImGui::GetIO();
    const float radarSize = std::min(io.DisplaySize.x, io.DisplaySize.y) * 0.35f;//设置雷达的尺寸为屏幕较小边的30%
    const float margin = 10.0f;
    const float winPad = 6.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(winPad, winPad));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));

    float btnW = radarSize * 0.12f;
    float winW = btnW + 4 + radarSize + winPad * 2;
    float winH = radarSize + winPad * 2;
    ImGui::SetNextWindowPos(
        ImVec2(margin, margin),
        ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Once);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("## Radar", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImVec2 cursor0 = ImGui::GetCursorScreenPos();

    // ── 左侧按钮栏 (竖向排列) ──
    {
        float btnH = radarSize * 0.15f; //设置按钮高度为雷达尺寸的15%

        // Rot 按钮
        const char* radarRotLabels[] = {"0", "90", "180", "270"};
        char rotLabel[32];
        snprintf(rotLabel, sizeof(rotLabel), "R:%s", radarRotLabels[m_radarRotation % 4]);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 4));
        if (ImGui::Button(rotLabel, ImVec2(btnW, btnH))) {
            m_radarRotation = (m_radarRotation + 1) % 4;
        }

        // Info 按钮
        if (ImGui::Button(m_enableInfo ? "I:ON" : "I:OF", ImVec2(btnW, btnH))) {
            m_enableInfo = !m_enableInfo;
        }
        ImGui::PopStyleVar();
    }

    // ── 雷达地图区域 (右侧) ──
    ImGui::SetCursorScreenPos(ImVec2(cursor0.x + btnW + 4, cursor0.y));
    ImVec2 O = ImGui::GetCursorScreenPos();
    ImVec2 E(O.x + radarSize, O.y + radarSize);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    #define M(wx,wz) xzToMinimap(wx, wz, O, radarSize, m_radarRotation)

    dl->PushClipRect(O, E, true);
    dl->AddRectFilled(O, E, IM_COL32(0, 0, 0, 0));

    // ── 河道 (蓝色斜带) ──
    {
        float rw = radarSize * 0.04f;
        ImVec2 r1 = M(2000, 12600), r2 = M(12600, 2000);
        dl->AddLine(r1, r2, IM_COL32(30, 50, 90, 180), rw);
        dl->AddLine(r1, r2, IM_COL32(50, 80, 140, 60), rw * 0.5f);
    }

    // ── 三条路线 (土黄色路径) ──
    {
        ImU32 laneCol = IM_COL32(80, 70, 45, 150);
        float lw = radarSize * 0.02f;
        dl->AddLine(M(1200,1200), M(1200,13400), laneCol, lw);
        dl->AddLine(M(1200,13400), M(13400,13400), laneCol, lw);
        dl->AddLine(M(1200,1200), M(13400,1200), laneCol, lw);
        dl->AddLine(M(13400,1200), M(13400,13400), laneCol, lw);
        dl->AddLine(M(2000,2000), M(12600,12600), laneCol, lw);
    }

    // ── 草丛 (浅绿小块) ──
    {
        ImU32 bushCol = IM_COL32(35, 65, 30, 200);
        float bs = radarSize * 0.02f;
        auto bush = [&](float wx, float wz) {
            ImVec2 c = M(wx, wz);
            dl->AddRectFilled(ImVec2(c.x-bs, c.y-bs*0.6f),
                              ImVec2(c.x+bs, c.y+bs*0.6f), bushCol, bs*0.3f);
        };
        bush(3500,10500); bush(4500,9500); bush(10500,5000); bush(9500,4500);
        bush(6000,8500);  bush(8500,6000); bush(3000,3000);  bush(11500,11500);
    }

    // ── 基地 (蓝/红) ──
    {
        float baseR = radarSize * 0.025f;
        ImVec2 bb = M(500, 500), rb = M(14100, 14100);
        dl->AddCircleFilled(bb, baseR, IM_COL32(30,100,220,180), 16);
        dl->AddCircle(bb, baseR, IM_COL32(60,140,255,220), 16, 1.5f);
        dl->AddCircleFilled(rb, baseR, IM_COL32(200,40,40,180), 16);
        dl->AddCircle(rb, baseR, IM_COL32(255,80,80,220), 16, 1.5f);
    }

    // ── 大龙/小龙 ──
    {
        float pitR = radarSize * 0.012f;
        ImVec2 baron = M(4200, 10200);
        dl->AddCircle(baron, pitR, IM_COL32(180,50,200,180), 12, 1.5f);
        dl->AddText(ImVec2(baron.x-3, baron.y-4), IM_COL32(200,100,220,200), "B");
        ImVec2 dragon = M(10200, 4200);
        dl->AddCircle(dragon, pitR, IM_COL32(220,160,30,180), 12, 1.5f);
        dl->AddText(ImVec2(dragon.x-3, dragon.y-4), IM_COL32(230,180,50,200), "D");
    }

    // ── 防御塔 (小方块) ──
    {
        float tw = radarSize * 0.006f;
        auto tower = [&](float wx, float wz, ImU32 col) {
            ImVec2 c = M(wx, wz);
            dl->AddRectFilled(ImVec2(c.x-tw,c.y-tw), ImVec2(c.x+tw,c.y+tw), col);
        };
        ImU32 bT = IM_COL32(60,140,255,200), rT = IM_COL32(255,80,80,200);
        tower(1200,5500,bT); tower(3500,3500,bT); tower(5500,1200,bT);
        tower(13400,9000,rT); tower(11000,11000,rT); tower(9000,13400,rT);
    }

    // ── 边框 ──
    dl->AddRect(O, E, IM_COL32(80,120,80,250), 0.0f, 0, 2.0f);

    // ── 动画脉冲 ──
    timespec ts_r{};
    clock_gettime(CLOCK_MONOTONIC, &ts_r);
    float anim = (float)ts_r.tv_sec + (float)ts_r.tv_nsec / 1e9f;
    float pulse = (std::sin(anim * 3.0f) + 1.0f) * 0.5f;

    const float heroR = std::max(radarSize * 0.018f, 3.0f);
    const float wardR = std::max(radarSize * 0.012f, 2.0f);

    // ── 绘制所有英雄 ──
    for (const auto& hero : data.enemyHeroes) {
        if (!hero.hasWorldPos) continue;
        ImVec2 p = worldToMinimap(hero.worldPos, O, radarSize, m_radarRotation);

        const bool isMyTeam = (hero.iconType == 1);

        if (isMyTeam) {
            float oR = heroR * (1.4f + 0.25f * pulse);
            dl->AddCircle(p, oR, IM_COL32(80, 220, 80, (int)(40 + 30 * pulse)), 0, 1.0f);
            dl->AddCircleFilled(p, heroR, IM_COL32(30, 200, 30, 180), 16);
            dl->AddCircle(p, heroR, IM_COL32(60, 255, 60, 240), 16, 1.5f);

            char lv[8]; snprintf(lv, sizeof(lv), "%u", hero.heroLevel);
            dl->AddText(ImVec2(p.x + heroR + 1, p.y - heroR - 2),
                        IM_COL32(100, 255, 100, 230), lv);
        } else {
            float hp = (hero.maxHp > 0) ? std::clamp(hero.curHp / hero.maxHp, 0.f, 1.f) : 0.f;
            float cr = (hp > 0.5f) ? (1.f - hp) * 2.f : 1.f;
            float cg = (hp > 0.5f) ? 1.f : hp * 2.f;
            ImU32 col  = IM_COL32((int)(cr * 255), (int)(cg * 255), 40, 255);
            ImU32 fill = IM_COL32((int)(cr * 255), (int)(cg * 255), 40, 120);

            float oR = heroR * (1.4f + 0.25f * pulse);
            dl->AddCircle(p, oR, IM_COL32((int)(cr * 255), (int)(cg * 255), 40,
                          (int)(30 + 25 * pulse)), 0, 1.0f);

            dl->AddCircleFilled(p, heroR, fill, 16);
            dl->AddCircle(p, heroR, col, 16, 1.5f);

            if (hero.maxHp > 0) {
                float bw = heroR * 2.f;
                float bh = std::max(1.5f, heroR * 0.2f);
                float bx = p.x - heroR;
                float by = p.y + heroR + 1.f;
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh),
                                  IM_COL32(0, 0, 0, 140));
                dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw * hp, by + bh), col);
            }

            char lv[8]; snprintf(lv, sizeof(lv), "%u", hero.heroLevel);
            dl->AddText(ImVec2(p.x + heroR + 1, p.y - heroR - 2),
                        IM_COL32(255, 255, 100, 220), lv);
        }
    }

    // ── 眼位 (蓝色小菱形) ──
    if (m_enableWards) {
        for (const auto& ward : data.wards) {
            if (!ward.hasWorldPos) continue;
            ImVec2 p = worldToMinimap(ward.worldPos, O, radarSize, m_radarRotation);
            dl->AddCircleFilled(p, wardR, IM_COL32(50, 150, 255, 100));
            dl->AddCircle(p, wardR, IM_COL32(100, 200, 255, 220), 0, 1.0f);
            float s = wardR * 0.5f;
            dl->AddQuadFilled(ImVec2(p.x, p.y - s), ImVec2(p.x + s, p.y),
                              ImVec2(p.x, p.y + s), ImVec2(p.x - s, p.y),
                              IM_COL32(120, 210, 255, 240));
        }
    }

    dl->PopClipRect();

    #undef M

    ImGui::SetCursorScreenPos(ImVec2(cursor0.x, std::max(ImGui::GetCursorScreenPos().y, O.y + radarSize)));
    ImGui::Dummy(ImVec2(btnW + 4 + radarSize, 0));

    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ═══════════════════════════════════════════════════════════════════════════════
// drawSkillRange — 绘制己方英雄当前技能有效施法范围圆圈
// ═══════════════════════════════════════════════════════════════════════════════

void GameOverlay::drawSkillRange(const lol::MiniMapData& data) {
    ImGuiIO& io = ImGui::GetIO();
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    auto* dl = ImGui::GetForegroundDrawList();

    auto drawProjectedRing = [&](const std::vector<lol::ScreenPoint>& points,
                                 ImU32 lineColor,
                                 ImU32 fillColor) {
        if (points.size() < 8) return;

        std::vector<ImVec2> projectedPoints;
        projectedPoints.reserve(points.size());
        for (const auto& point : points) {
            projectedPoints.emplace_back(point.x, screenH - point.y);
        }

        dl->AddConvexPolyFilled(projectedPoints.data(), (int)projectedPoints.size(), fillColor);
        dl->AddPolyline(projectedPoints.data(), (int)projectedPoints.size(), lineColor, ImDrawFlags_Closed, 1.8f);
    };

    for (const auto& hero : data.enemyHeroes) {
        const bool isMyTeam = (hero.iconType == lol::MiniMapIconType_MyTeamHero);
        if (isMyTeam || hero.atkRange <= 0.0f || !hero.hasScreenPos) continue;

        if (hero.screenX < 0.0f || hero.screenX > screenW ||
            hero.screenY < 0.0f || hero.screenY > screenH) {
            continue;
        }

        // 敌方英雄: 简化为屏幕空间圆（无 Camera 访问，无法精确投影）
        const float screenRadius = hero.atkRange * 18.0f; // 粗略像素缩放
        ImVec2 center(hero.screenX, screenH - hero.screenY);
        dl->AddCircle(center, screenRadius, IM_COL32(255, 80, 80, 95), 40, 1.8f);
        dl->AddCircleFilled(center, screenRadius, IM_COL32(255, 60, 60, 14), 40);

        char enemyLabel[48];
        snprintf(enemyLabel, sizeof(enemyLabel), "%s %.1f",
                 hero.heroName.empty() ? "Enemy" : hero.heroName.c_str(),
                 hero.atkRange);
        dl->AddText(ImVec2(hero.screenX + 10.0f, screenH - hero.screenY - 18.0f),
                    IM_COL32(255, 120, 120, 235), enemyLabel);
    }

    if (data.mySkillRange <= 0.0f || !data.hasMyScreenPos || data.mySkillRangeScreenPoints.size() < 8) {
        return;
    }

    const ImU32 circleColor = IM_COL32(0, 255, 100, 90);
    const ImU32 fillColor   = IM_COL32(0, 255, 100, 18);
    const ImU32 textColor   = IM_COL32(0, 255, 100, 220);

    drawProjectedRing(data.mySkillRangeScreenPoints, circleColor, fillColor);

    char label[32];
    snprintf(label, sizeof(label), "%.1f", data.mySkillRange);
    dl->AddText(ImVec2(data.myScreenX + 10.0f, screenH - data.myScreenY - 18.0f), textColor, label);
}

// ═══════════════════════════════════════════════════════════════════════════════
// drawMinionESP — 小兵 ESP 方框 + 血条
// ═══════════════════════════════════════════════════════════════════════════════

void GameOverlay::drawMinionESP(const lol::MiniMapData& data) {
    if (data.minions.empty()) return;

    ImGuiIO& io = ImGui::GetIO();
    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;
    const bool hasSelfRange = data.mySkillRange > 0.0f;
    const float attackRangeSq = data.mySkillRange * data.mySkillRange;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(screenW, screenH));
    ImGui::Begin("##MinionESP", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoBackground |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (const auto& m : data.minions) {
        if (!m.hasScreenPos) continue;

        float sx = m.screenX;
        float sy = screenH - m.screenY; // Unity Y 翻转

        bool inAttackRange = false;
        if (hasSelfRange && m.hasWorldPos) {
            float dx = m.worldPos.x - data.myWorldPos.x;
            float dz = m.worldPos.z - data.myWorldPos.z;
            float distSq = dx * dx + dz * dz;
            inAttackRange = distSq <= attackRangeSq;
        }
        bool isLastHittable = m.isEnemy && inAttackRange && m.curHp > 0.0f && m.curHp <= m_lastHitHpThreshold;

        constexpr float kMargin = 50.0f;
        if (sx < -kMargin || sx > screenW + kMargin ||
            sy < -kMargin || sy > screenH + kMargin)
            continue;

        // 小兵方框尺寸 (比英雄小)
        const float boxW = 28.0f;
        const float boxH = 40.0f;
        ImVec2 boxMin(sx - boxW * 0.5f, sy - boxH * 0.7f);
        ImVec2 boxMax(sx + boxW * 0.5f, sy + boxH * 0.3f);

        // 血条颜色
        float hpRatio = (m.maxHp > 0.0f) ? (m.curHp / m.maxHp) : 0.0f;
        hpRatio = std::clamp(hpRatio, 0.0f, 1.0f);

        ImU32 boxColor = m.isEnemy ? IM_COL32(255, 100, 100, 180) : IM_COL32(100, 255, 100, 180);
        if (isLastHittable) {
            boxColor = IM_COL32(255, 220, 0, 255);
        }

        // 方框
        dl->AddRectFilled(boxMin, boxMax, IM_COL32(0, 0, 0, 30));
        dl->AddRect(boxMin, boxMax, boxColor, 0.0f, 0, 1.5f);
        if (isLastHittable) {
            dl->AddRect(ImVec2(boxMin.x - 2.0f, boxMin.y - 2.0f),
                        ImVec2(boxMax.x + 2.0f, boxMax.y + 2.0f),
                        IM_COL32(255, 255, 150, 220), 0.0f, 0, 2.0f);
            dl->AddCircle(ImVec2(sx, sy - boxH * 0.2f), boxW * 0.7f,
                          IM_COL32(255, 220, 0, 220), 24, 1.8f);
        }

        // 血条 (方框上方)
        const float hpBarH = 3.0f;
        const float hpBarY = boxMin.y - hpBarH - 1.0f;
        ImVec2 hpBgMin(boxMin.x, hpBarY);
        ImVec2 hpBgMax(boxMax.x, hpBarY + hpBarH);
        ImVec2 hpFgMax(boxMin.x + (boxMax.x - boxMin.x) * hpRatio, hpBarY + hpBarH);

        ImU32 hpColor = (hpRatio > 0.5f)  ? IM_COL32(0, 255, 0, 200) :
                        (hpRatio > 0.25f) ? IM_COL32(255, 255, 0, 200) :
                                             IM_COL32(255, 0, 0, 200);
        if (isLastHittable) {
            hpColor = IM_COL32(255, 220, 0, 255);
        }

        dl->AddRectFilled(hpBgMin, hpBgMax, IM_COL32(0, 0, 0, 140));
        dl->AddRectFilled(hpBgMin, hpFgMax, hpColor);

        if (isLastHittable) {
            const char* label = "LAST";
            ImVec2 textSize = ImGui::CalcTextSize(label);
            ImVec2 textPos(sx - textSize.x * 0.5f, boxMin.y - textSize.y - 6.0f);
            dl->AddText(ImVec2(textPos.x + 1.0f, textPos.y + 1.0f), IM_COL32(0, 0, 0, 220), label);
            dl->AddText(textPos, IM_COL32(255, 235, 80, 255), label);
        }
    }

    ImGui::End();
}

} // namespace draw

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif