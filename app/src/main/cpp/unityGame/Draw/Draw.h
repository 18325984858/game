#ifndef DOBBY_PROJECT_DRAW_H
#define DOBBY_PROJECT_DRAW_H

#include <imgui/imgui.h>
#include "../lol/lolm.h"

// touch_input 中供绘制使用的函数 (实现在 start.cpp)
namespace touch_input {
    const char* getRotationLabel();
    void cycleRotation();
    int getRotation();
    void toggleSurfaceRotationLock();
    bool isSurfaceRotationLockEnabled();
    void toggleRotationCorrectionLock();
    bool isRotationCorrectionLocked();
}

namespace draw {

    class GameOverlay {
    public:
        // ImGui 覆盖层绘制 (状态面板 + ESP方框 + 小地图雷达)
        void drawOverlay(const lol::MiniMapData& data, bool inBattle);

    private:
        bool m_panelExpanded = true;
        bool m_enableESP     = false;
        bool m_enableRadar   = false;
        bool m_enableInfo    = true;
        bool m_enableHeroInfo = false;
        bool m_enableWards   = false;
        bool m_enableSkillRange = false;
        bool m_enableMinion  = false;
        bool m_enableAutoFarm = false;
        float m_lastHitHpThreshold = 120.0f;
        int  m_radarRotation = 0;

        // 小地图坐标映射
        static ImVec2 worldToMinimap(const lol::UnityVector3& worldPos,
                                     ImVec2 minimapOrigin, float minimapSize,
                                     int rot);
        static ImVec2 xzToMinimap(float wx, float wz, ImVec2 origin, float size,
                                  int rot);

        // 子绘制
        void drawInfoPanel(const lol::MiniMapData& data, bool inBattle);
        void drawESP(const lol::MiniMapData& data);
        void drawRadar(const lol::MiniMapData& data);
        void drawSkillRange(const lol::MiniMapData& data);
        void drawMinionESP(const lol::MiniMapData& data);
    };

} // namespace draw

#endif //DOBBY_PROJECT_DRAW_H