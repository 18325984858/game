#ifndef DFM_DRAW_H
#define DFM_DRAW_H

#include <imgui/imgui.h>
#include <string>
#include <vector>
#include "../dfm/dfm.h"

// =====================================================================
//  DFM Overlay 绘制系统 — 三角洲行动 玩家/物资/箱子 ESP + 小地图
//  仿照 ueGamepubgmhd/Draw/UE4Draw 代码规范
// =====================================================================

namespace dfmdraw {

class DfmOverlay {
public:
    void drawOverlay(const dfm::DrawDfmData& data);

private:
    // ── 菜单状态 ──
    bool m_menuExpanded    = true;
    bool m_enableESP       = true;     // 3D ESP 方框
    bool m_enableSnapline  = true;     // 底部射线
    bool m_enableName      = true;     // 玩家名
    bool m_enableHP        = true;     // 血条
    bool m_enableDistance   = true;     // 距离
    bool m_enableTeammate  = false;    // 显示队友
    bool m_enableMinimap   = true;     // 小地图
    bool m_enableLoot      = true;     // 物资显示
    bool m_enableLootESP   = true;     // 物资3D名称标签
    bool m_enableContainer = true;     // 物资箱
    bool m_enablePlayerList = false;   // 玩家列表面板
    bool m_enableArmor     = true;     // 护甲/头盔显示
    bool m_filterAmmo      = true;     // 过滤弹药
    bool m_filterJunk      = true;     // 过滤杂物
    float m_minimapSize    = 200.0f;
    float m_minimapRange   = 200.0f;   // 小地图半径 (米)
    float m_espMaxDist     = 500.0f;   // ESP 最大距离 (米)
    float m_lootMaxDist    = 100.0f;   // 物资最大显示距离 (米)

    // ── 子绘制 ──
    void drawMenu(const dfm::DrawDfmData& data);
    int  drawESP(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawLootESP(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawMinimap(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawPlayerList(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawLootList(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawNotifications(const dfm::DrawDfmData& data, float screenW, float screenH);

    // ── 通知缓存 (跨帧保持) ──
    std::vector<dfm::Notification> m_activeNotifications;
    std::chrono::steady_clock::time_point m_lastNotifTime;

    // ── 工具 ──
    static ImU32 hpColor(float ratio);
    static float distance3D(float x1, float y1, float z1, float x2, float y2, float z2);
    static bool  worldToScreen(const dfm::DrawDfmData& cam, float wx, float wy, float wz,
                               float screenW, float screenH, float& sx, float& sy);
};

} // namespace dfmdraw

#endif // DFM_DRAW_H
