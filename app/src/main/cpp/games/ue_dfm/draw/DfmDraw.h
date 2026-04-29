#ifndef DFM_DRAW_H
#define DFM_DRAW_H

#include <imgui/imgui.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
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
    bool m_enableESP       = false;    // 3D ESP 方框
    bool m_enableBones     = false;    // 骨骼绘制
    bool m_enableSnapline  = false;    // 底部射线
    bool m_enableName      = false;    // 玩家名
    bool m_enableHP        = false;    // 血条
    bool m_enableDistance   = false;    // 距离
    bool m_enableTeammate  = false;    // 显示队友
    bool m_enableMinimap   = false;    // 小地图
    bool m_enableLoot      = false;    // 物资显示
    bool m_enableLootESP   = false;    // 物资3D名称标签
    bool m_enableContainer = false;    // 物资箱
    bool m_enableRemoteOpen = false;   // 物资箱远程开箱按钮 (默认关 — 风控敏感)
    bool m_enablePlayerList = false;   // 玩家列表面板
    bool m_enableArmor     = false;    // 护甲/头盔显示
    bool m_filterAmmo      = true;     // 过滤弹药
    bool m_filterJunk      = true;     // 过滤杂物
    float m_minimapSize    = 200.0f;
    float m_minimapRange   = 200.0f;   // 小地图半径 (米)
    float m_espMaxDist     = 500.0f;   // ESP 最大距离 (米)
    float m_lootMaxDist    = 100.0f;   // 物资最大显示距离 (米)

    // ── 辅助瞄准信息显示 (只读视觉提示, 不接管输入) ──
    // 标记屏幕中心附近的最近敌人, 显示距离 + 子弹下坠预估补偿点。
    // 玩家自己手动对准, 不修改 ControlRotation/不注入触摸事件。
    bool  m_enableAimAssist     = false;   // 总开关 (默认关)
    bool  m_aimAssistShowDrop   = true;    // 在目标上方画"建议预瞄点"
    bool  m_aimAssistShowLead   = false;   // 移动目标提前量 (粗略, 仅作参考)
    int   m_aimAssistBoneIdx    = 1;       // 目标点: 0=头(BONE 0), 1=胸(BONE 2), 2=骨盆(BONE 4)
    float m_aimAssistMaxDist    = 200.0f;  // 米, 超出忽略
    float m_aimAssistFOVDeg     = 8.0f;    // 屏幕角度锥, 只考虑该锥内目标
    float m_aimAssistBulletVel  = 600.0f;  // m/s, 默认步枪初速 (用于下坠/提前量计算)

    // ── 子绘制 ──
    void drawMenu(const dfm::DrawDfmData& data);
    int  drawESP(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawBones(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawLootESP(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawMinimap(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawPlayerList(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawLootList(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawNotifications(const dfm::DrawDfmData& data, float screenW, float screenH);
    void drawAimAssist(const dfm::DrawDfmData& data, float screenW, float screenH);

    // ── 移动方向估算缓存 (用于提前量) ──
    struct PlayerVelSample {
        float x = 0.f, y = 0.f, z = 0.f;
        std::chrono::steady_clock::time_point t{};
    };
    std::unordered_map<uintptr_t, PlayerVelSample> m_lastPlayerSample;

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
