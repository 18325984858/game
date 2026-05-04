#include "AImGui.h"

#define TAG "UE4-GUI"
#include "Global.h"
#include "ANativeWindowCreator.h"
#include "ATouchEvent.h"

#include <ImGui-SharedDrawData/modules/ImGuiSharedDrawData.h>
#include <imgui/imgui_internal.h>
#include <zstd.h>
#include <netinet/tcp.h>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <cstring>

size_t android::anative_window_creator::detail::compat::SystemVersion = 13;

namespace
{
    bool TryLoadChineseFontFromFile(ImGuiIO &imguiIO, const char *path, float fontSizePixels, int fontNo = 0)
    {
        struct stat st{};
        if (nullptr == path || 0 != stat(path, &st) || !S_ISREG(st.st_mode))
            return false;

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (0 > fd)
            return false;

        int fileSize = static_cast<int>(st.st_size);
        void *data = IM_ALLOC(fileSize);
        if (nullptr == data)
        {
            close(fd);
            return false;
        }

        size_t totalRead = 0;
        while (totalRead < static_cast<size_t>(fileSize))
        {
            auto readSize = read(fd, reinterpret_cast<char *>(data) + totalRead, fileSize - totalRead);
            if (0 >= readSize)
                break;
            totalRead += static_cast<size_t>(readSize);
        }
        close(fd);

        if (totalRead != static_cast<size_t>(fileSize))
        {
            IM_FREE(data);
            return false;
        }

        ImFontConfig fontConfig;
        fontConfig.FontDataOwnedByAtlas = true;
        fontConfig.OversampleH = 1;
        fontConfig.OversampleV = 1;
        fontConfig.PixelSnapH = true;
        fontConfig.FontNo = fontNo;
        fontConfig.SizePixels = fontSizePixels;

        // 字形范围: ChineseSimplifiedCommon (~2500 常用字) + 显式补集。
        // 不能用 ChineseFull (整个 0x4E00~0x9FAF, ~21000 字符), 22px 字体下需要
        // 4096x4096+ atlas, 大量设备 Build()/纹理上传失败 → 整个菜单画不出。
        //
        // 这里把 DFM/PUBG 菜单、ESP、列表、通知里所有用到的中文短语
        // 显式追加, 保证 atlas 体积保持原来水平 (~1024x1024) 又不缺字。
        // 新增中文字符串时同步在这里追加, 否则会再次出现 ? 占位。
        static const char* kExtraGlyphTexts[] = {
            // DFM 菜单
            "\xe5\xaf\xb9\xe5\xb1\x80\xe4\xb8\xad\xe7\xad\x89\xe5\xbe\x85\xe7\x8e\xa9\xe5\xae\xb6\xe7\x89\xa9\xe8\xb5\x84\xe7\xae\xb1\xe5\xad\x90\xe7\x9b\xb8\xe6\x9c\xba\xe9\xaa\xa8\xe9\xaa\xbc\xe5\xb0\x84\xe7\xba\xbf\xe5\xb0\x8f\xe5\x9c\xb0\xe5\x9b\xbe\xe5\x88\x97\xe8\xa1\xa8\xe6\x98\xbe\xe7\xa4\xba" "3D"
            "\xe6\xa0\x87\xe7\xad\xbe\xe8\xa1\x80\xe6\x9d\xa1\xe8\xb7\x9d\xe7\xa6\xbb\xe5\x90\x8d\xe5\xad\x97\xe9\x98\x9f\xe5\x8f\x8b\xe6\x8a\xa4\xe7\x94\xb2\xe5\xa4\xb4\xe7\x9b\x94\xe8\xbf\x87\xe6\xbb\xa4\xe5\xbc\xb9\xe8\x8d\xaf\xe6\x9d\x82\xe7\x89\xa9\xe6\x97\xa5\xe5\xbf\x97\xe8\xbe\x93\xe5\x87\xba\xe6\x94\xb6\xe8\xb5\xb7\xe5\xb1\x95\xe5\xbc\x80",
            "\xe9\x99\x84\xe8\xbf\x91\xe4\xbb\xb6\xe5\xb7\xb2\xe5\xbc\x80\xe6\x9c\xaa\xe5\xbc\x80\xe8\xbf\x98\xe6\x9c\x89\xe4\xb8\xaa\xe6\x9c\xaa\xe7\x9f\xa5\xe5\xad\x98\xe6\xb4\xbb\xe5\x8f\x8b\xe6\x95\x8c\xe9\xbb\x98\xe8\xae\xa4\xe6\xad\xa6\xe5\x99\xa8\xe7\xae\xb1\xe6\x8a\xa4\xe7\x94\xb2\xe7\xae\xb1\xe6\x9d\x82\xe7\x89\xa9\xe7\xae\xb1\xe6\x8d\xae\xe7\x82\xb9\xe7\xae\xb1\xe5\x8d\x95\xe7\x89\xa9\xe5\x93\x81\xe7\xae\xb1\xe5\xb0\xb8\xe4\xbd\x93\xe7\xae\xb1\xe6\x97\xa0\xe5\xae\x9a\xe4\xbd\x8d",
            "\xe7\xaa\x81\xe5\x87\xbb\xe6\xad\xa5\xe6\x9e\xaa\xe5\x86\xb2\xe9\x94\x8b\xe6\x9e\xaa\xe7\xb2\xbe\xe7\xa1\xae\xe6\xad\xa5\xe6\x9e\xaa\xe7\x8b\x99\xe5\x87\xbb\xe6\xad\xa5\xe6\x9e\xaa\xe8\xbd\xbb\xe6\x9c\xba\xe6\x9e\xaa\xe9\x9c\xb0\xe5\xbc\xb9\xe6\x9e\xaa\xe6\x89\x8b\xe6\x9e\xaa\xe5\xa4\xb4\xe7\x9b\x94\xe8\x83\x8c\xe5\x8c\x85\xe6\x9e\xaa\xe5\x8f\xa3\xe6\x8f\xa1\xe6\x8a\x8a\xe7\x9e\x84\xe5\x87\x86\xe9\x95\x9c\xe5\xbc\xb9\xe5\x8c\xa3\xe6\x80\xa5\xe6\x95\x91\xe5\x8c\x85\xe6\xb3\xa8\xe5\xb0\x84\xe5\x99\xa8\xe8\x83\xbd\xe9\x87\x8f\xe9\xa5\xae\xe6\x96\x99\xe6\x89\x8b\xe9\x9b\xb7\xe9\x97\xaa\xe5\x85\x89\xe5\xbc\xb9\xe7\x83\x9f\xe9\x9b\xbe\xe5\xbc\xb9\xe6\xad\xa5\xe6\x9e\xaa\xe5\xbc\xb9\xe5\x86\xb2\xe9\x94\x8b\xe5\xbc\xb9\xe7\x8b\x99\xe5\x87\xbb\xe5\xbc\xb9\xe9\x9c\xb0\xe5\xbc\xb9\xe6\x89\x8b\xe6\x9e\xaa\xe5\xbc\xb9",
            "\xe8\xa3\x85\xe5\xa4\x87\xe9\x85\x8d\xe4\xbb\xb6\xe8\x8d\xaf\xe5\x93\x81\xe6\x94\xb6\xe9\x9b\x86\xe5\x93\x81\xe6\x8a\x95\xe6\x8e\xb7\xe7\x89\xa9\xe8\xbd\xbd\xe5\x85\xb7",
            // 通用 / 调试 / Toast
            "\xe6\x97\xa0\xe5\x90\x8d\xe5\x90\xaf\xe5\x8a\xa8\xe9\x80\x80\xe5\x87\xba\xe5\xa4\xb1\xe8\xb4\xa5\xe6\x88\x90\xe5\x8a\x9f\xe5\xae\x8c\xe6\x88\x90\xe5\xbc\x80\xe5\xa7\x8b\xe7\xbb\x93\xe6\x9d\x9f\xe5\xb7\xb2\xe5\xad\x98\xe5\x9c\xa8\xe8\xb7\xb3\xe8\xbf\x87\xe5\x81\x8f\xe7\xa7\xbb\xe8\xa7\xa3\xe6\x9e\x90\xe7\x9b\x91\xe6\x8e\xa7\xe7\xba\xbf\xe7\xa8\x8b\xe5\xbc\x95\xe6\x93\x8e\xe5\xb0\xb1\xe7\xbb\xaa\xe8\xbf\x9e\xe6\x8e\xa5\xe6\x9c\x8d\xe5\x8a\xa1\xe6\xb8\xb2\xe6\x9f\x93\xe5\x88\x9d\xe5\xa7\x8b\xe5\x8c\x96",
            "\xe5\xaf\xb9\xe5\xb1\x80\xe7\x9b\x91\xe6\x8e\xa7\xe6\x9c\xaa\xe5\x90\xaf\xe7\x94\xa8\xe5\xb7\xb2\xe5\x90\xaf\xe7\x94\xa8\xe5\xae\x89\xe5\x85\xa8\xe5\xb1\x8b\xe7\xa7\x92\xe9\x87\x8d\xe8\xaf\x95\xe4\xb8\xa2\xe5\xbc\x83\xe6\x94\xbe\xe5\xbc\x83\xe5\xb0\x9d\xe8\xaf\x95\xe6\x9c\xac\xe5\x9c\xb0\xe9\x98\x9f\xe4\xbc\x8d",
            // PUBG mhd 物资/载具/UI 词汇 (急救包/医疗箱/绷带/止痛药/肾上腺素/能量饮料/手雷/烟雾弹/燃烧弹/震爆弹/防弹衣/警用/军用/摩托车/特种/头盔/背包/级/九毫米/点马格南/散弹/号弹/药物资堆/死亡箱/空投/载具/油/正常/受损/严重/报废/训练人偶/飞机/皮卡/轿车/装甲/三轮卡车/摩托艇/船/快艇/坦克/武装/直升/潜水/雪地越野/房车/小巴/客货卡/迷彩涂装/跑/黄绿蓝红黑白紫粉灰金银棕橙/拖拉/收起/展开/锁定雷达位置/重置/提示/拖拽/下方/添加/范围/比缩放/可移动按钮/等)
            "\xe6\x80\xa5\xe6\x95\x91\xe5\x8c\x85\xe5\x8c\xbb\xe7\x96\x97\xe7\xae\xb1\xe7\xbb\xb7\xe5\xb8\xa6\xe6\xad\xa2\xe7\x97\x9b\xe8\x8d\xaf\xe8\x82\xbe\xe4\xb8\x8a\xe8\x85\xba\xe7\xb4\xa0\xe8\x83\xbd\xe9\x87\x8f\xe9\xa5\xae\xe6\x96\x99\xe6\x89\x8b\xe9\x9b\xb7\xe7\x83\x9f\xe9\x9b\xbe\xe5\xbc\xb9\xe7\x87\x83\xe7\x83\xa7\xe5\xbc\xb9\xe9\x9c\x87\xe7\x88\x86\xe5\xbc\xb9\xe9\x98\xb2\xe5\xbc\xb9\xe8\xa1\xa3\xe8\xad\xa6\xe7\x94\xa8\xe5\x86\x9b\xe7\x94\xa8\xe6\x91\xa9\xe6\x89\x98\xe8\xbd\xa6\xe7\x89\xb9\xe7\xa7\x8d\xe5\xa4\xb4\xe7\x9b\x94\xe8\x83\x8c\xe5\x8c\x85\xe7\xba\xa7\xe4\xb9\x9d\xe6\xaf\xab\xe7\xb1\xb3\xe7\x82\xb9\xe9\xa9\xac\xe6\xa0\xbc\xe5\x8d\x97\xe6\x95\xa3\xe5\xbc\xb9\xe5\x8f\xb7\xe5\xbc\xb9\xe8\x8d\xaf\xe7\x89\xa9\xe8\xb5\x84\xe5\xa0\x86\xe6\xad\xbb\xe4\xba\xa1\xe7\xae\xb1\xe7\xa9\xba\xe6\x8a\x95\xe8\xbd\xbd\xe5\x85\xb7\xe6\xb2\xb9\xe6\xad\xa3\xe5\xb8\xb8\xe5\x8f\x97\xe6\x8d\x9f\xe4\xb8\xa5\xe9\x87\x8d\xe6\x8a\xa5\xe5\xba\x9f\xe8\xae\xad\xe7\xbb\x83\xe4\xba\xba\xe5\x81\xb6\xe9\xa3\x9e\xe6\x9c\xba\xe7\x9a\xae\xe5\x8d\xa1\xe8\xbd\xbf\xe8\xbd\xa6\xe8\xa3\x85\xe7\x94\xb2\xe4\xb8\x89\xe8\xbd\xae\xe5\x8d\xa1\xe8\xbd\xa6\xe6\x91\xa9\xe6\x89\x98\xe8\x89\x87\xe8\x88\xb9\xe5\xbf\xab\xe8\x89\x87\xe5\x9d\xa6\xe5\x85\x8b\xe6\xad\xa6\xe8\xa3\x85\xe7\x9b\xb4\xe5\x8d\x87\xe6\xbd\x9c\xe6\xb0\xb4\xe9\x9b\xaa\xe5\x9c\xb0\xe8\xb6\x8a\xe9\x87\x8e\xe6\x88\xbf\xe8\xbd\xa6\xe5\xb0\x8f\xe5\xb7\xb4\xe5\xae\xa2\xe8\xb4\xa7\xe5\x8d\xa1\xe8\xbf\xb7\xe5\xbd\xa9\xe6\xb6\x82\xe8\xa3\x85\xe8\xb7\x91\xe9\xbb\x84\xe7\xbb\xbf\xe8\x93\x9d\xe7\xba\xa2\xe9\xbb\x91\xe7\x99\xbd\xe7\xb4\xab\xe7\xb2\x89\xe7\x81\xb0\xe9\x87\x91\xe9\x93\xb6\xe6\xa3\x95\xe6\xa9\x99\xe6\x8b\x96\xe6\x8b\x89\xe6\x94\xb6\xe8\xb5\xb7\xe5\xb1\x95\xe5\xbc\x80\xe9\x94\x81\xe5\xae\x9a\xe9\x9b\xb7\xe8\xbe\xbe\xe4\xbd\x8d\xe7\xbd\xae\xe9\x87\x8d\xe7\xbd\xae\xe6\x8f\x90\xe7\xa4\xba\xe6\x8b\x96\xe6\x8b\xbd\xe4\xb8\x8b\xe6\x96\xb9\xe6\xb7\xbb\xe5\x8a\xa0\xe8\x8c\x83\xe5\x9b\xb4\xe6\xaf\x94\xe7\xbc\xa9\xe6\x94\xbe\xe5\x8f\xaf\xe7\xa7\xbb\xe5\x8a\xa8\xe6\x8c\x89\xe9\x92\xae\xe7\xad\x89\xe5\x8c\x85\xe9\x85\x8d\xe4\xbb\xb6\xe6\x89\x8b\xe9\x9b\xb7\xe7\x83\x9f\xe9\x9b\xbe\xe5\xbc\xb9\xe9\x9c\x87\xe7\x88\x86\xe5\xbc\xb9\xe7\x87\x83\xe7\x83\xa7\xe7\x93\xb6\xe7\xaa\x81\xe5\x87\xbb\xe6\x9e\xaa\xe6\xad\xa5\xe6\x9e\xaa\xe5\x86\xb2\xe9\x94\x8b\xe6\x9e\xaa\xe7\xb2\xbe\xe7\xa1\xae\xe5\xb0\x84\xe6\x89\x8b\xe9\x9c\xb0\xe5\xbc\xb9\xe6\x9e\xaa\xe6\x89\x8b\xe6\x9e\xaa\xe8\xbd\xbb\xe6\x9c\xba\xe6\x9e\xaa\xe7\x8b\x99\xe5\x87\xbb\xe6\x9e\xaa\xe8\xbf\x91\xe6\x88\x98\xe5\x88\x80\xe9\x95\x90\xe5\xad\x90\xe6\xa3\x92\xe7\x90\x83\xe6\xa3\x8d\xe5\xb9\xb3\xe5\xba\x95\xe9\x94\x85",
        };
        ImFontGlyphRangesBuilder builder;
        // ä¼åä½¿ç¨ ChineseFull (å®æ´ CJK Unified Ideographs, ~21000 å­ç¬¦):
        // atlas 已设为 4096x4096, 现代设备充足. 失败时下方 Build() 会返回 false,
        // 调用方会清空并尝试下一个字体路径; 全部失败时回退到 ChineseSimplifiedCommon.
        builder.AddRanges(imguiIO.Fonts->GetGlyphRangesChineseFull());
        // 仍保留 kExtraGlyphTexts: 即使 Full 退化也覆盖菜单关键字
        for (const char* t : kExtraGlyphTexts) builder.AddText(t);
        static ImVector<ImWchar> chineseRanges;
        chineseRanges.clear();
        builder.BuildRanges(&chineseRanges);

        ImFont *font = imguiIO.Fonts->AddFontFromMemoryTTF(
                data,
                fileSize,
                fontSizePixels,
                &fontConfig,
                chineseRanges.Data);
        if (nullptr == font)
        {
            IM_FREE(data);
            return false;
        }

        if (!imguiIO.Fonts->Build())
        {
            LogDebug("[AImGui] Font build (Full) skipped, retry Common: %s", path);
            // ChineseFull 太大导致 Build 失败 (atlas 不够) → 用 Common 子集重试
            imguiIO.Fonts->Clear();
            ImFontGlyphRangesBuilder b2;
            b2.AddRanges(imguiIO.Fonts->GetGlyphRangesChineseSimplifiedCommon());
            for (const char* t : kExtraGlyphTexts) b2.AddText(t);
            static ImVector<ImWchar> r2; r2.clear(); b2.BuildRanges(&r2);
            // data 已被前一次 AddFontFromMemoryTTF 接管, 必须重新读
            int fd2 = open(path, O_RDONLY);
            if (fd2 < 0) return false;
            struct stat st2{};
            if (fstat(fd2, &st2) < 0) { close(fd2); return false; }
            int sz = static_cast<int>(st2.st_size);
            void* d2 = IM_ALLOC(sz);
            size_t got = 0;
            while (got < (size_t)sz) {
                auto r = read(fd2, (char*)d2 + got, sz - got);
                if (r <= 0) break;
                got += (size_t)r;
            }
            close(fd2);
            if (got != (size_t)sz) { IM_FREE(d2); return false; }
            ImFontConfig cfg2;
            cfg2.FontDataOwnedByAtlas = true;
            cfg2.OversampleH = 1;
            cfg2.OversampleV = 1;
            cfg2.PixelSnapH = true;
            cfg2.SizePixels = fontSizePixels;
            cfg2.FontNo = fontNo;
            font = imguiIO.Fonts->AddFontFromMemoryTTF(d2, sz, fontSizePixels, &cfg2, r2.Data);
            if (!font || !imguiIO.Fonts->Build()) {
                imguiIO.Fonts->Clear();
                return false;
            }
        }

        if (nullptr == font->FindGlyphNoFallback(static_cast<ImWchar>(0x4E2D)))
        {
            LogDebug("[AImGui] Font missing Chinese glyphs: %s", path);
            imguiIO.Fonts->Clear();
            return false;
        }

        LogInfo("[AImGui] Loaded Chinese font: %s (%d bytes, atlas %dx%d)",
                path,
                fileSize,
                imguiIO.Fonts->TexWidth,
                imguiIO.Fonts->TexHeight);
        return true;
    }

    bool IsFontFileName(const char *name)
    {
        if (nullptr == name)
            return false;

        auto len = strlen(name);
        if (len < 4)
            return false;

        const char *ext = name + len - 4;
        return 0 == strcasecmp(ext, ".ttf")
                || 0 == strcasecmp(ext, ".otf")
                || 0 == strcasecmp(ext, ".ttc");
    }

    void LoadPreferredImGuiFont(ImGuiIO &imguiIO, float fontSizePixels)
    {
        imguiIO.Fonts->TexDesiredWidth = 4096;

        static const char *fontPaths[] = {
                "/data/local/tmp/chinese.ttf",
                "/system/fonts/DroidSansFallback.ttf",
                "/system/fonts/NotoSansSC-Regular.ttf",
                "/system/fonts/NotoSansCJKsc-Regular.ttf",
                "/system/fonts/MiLanProVF.ttf",
                "/system/fonts/HarmonyOS_Sans_SC.ttf",
                "/system/fonts/OPPOSans-Regular.ttf",
                "/system/fonts/VivoSans-Regular.ttf",
                "/system/fonts/RobotoFallback-Regular.ttf",
                "/system/fonts/NotoSansSC-Regular.otf",
                "/system/fonts/NotoSansHans-Regular.otf",
                "/system/fonts/NotoSansCJKsc-Regular.otf",
                "/system/fonts/NotoSansSC-Regular.ttc",
                "/system/fonts/NotoSansCJKsc-Regular.ttc",
                "/system/fonts/NotoSansCJK-Regular.ttc",
        };

        for (const char *path : fontPaths)
        {
            if (TryLoadChineseFontFromFile(imguiIO, path, fontSizePixels))
                return;
        }

        DIR *dir = opendir("/system/fonts");
        if (nullptr != dir)
        {
            dirent *entry = nullptr;
            while (nullptr != (entry = readdir(dir)))
            {
                if (!IsFontFileName(entry->d_name))
                    continue;

                char path[512] = {};
                snprintf(path, sizeof(path), "/system/fonts/%s", entry->d_name);
                if (TryLoadChineseFontFromFile(imguiIO, path, fontSizePixels))
                {
                    closedir(dir);
                    return;
                }
            }
            closedir(dir);
        }

        imguiIO.Fonts->Clear();
        ImFontConfig fallbackConfig;
        fallbackConfig.SizePixels = fontSizePixels;
        imguiIO.Fonts->AddFontDefault(&fallbackConfig);
        imguiIO.Fonts->Build();
        LogInfo("[AImGui] Chinese font unavailable, fallback to default atlas %dx%d",
                imguiIO.Fonts->TexWidth,
                imguiIO.Fonts->TexHeight);
    }
}

static ImGuiKey KeyCodeToImGuiKey(int32_t keyCode)
{
    switch (keyCode)
    {
    case AKEYCODE_TAB:
        return ImGuiKey_Tab;
    case AKEYCODE_DPAD_LEFT:
        return ImGuiKey_LeftArrow;
    case AKEYCODE_DPAD_RIGHT:
        return ImGuiKey_RightArrow;
    case AKEYCODE_DPAD_UP:
        return ImGuiKey_UpArrow;
    case AKEYCODE_DPAD_DOWN:
        return ImGuiKey_DownArrow;
    case AKEYCODE_PAGE_UP:
        return ImGuiKey_PageUp;
    case AKEYCODE_PAGE_DOWN:
        return ImGuiKey_PageDown;
    case AKEYCODE_MOVE_HOME:
        return ImGuiKey_Home;
    case AKEYCODE_MOVE_END:
        return ImGuiKey_End;
    case AKEYCODE_INSERT:
        return ImGuiKey_Insert;
    case AKEYCODE_FORWARD_DEL:
        return ImGuiKey_Delete;
    case AKEYCODE_DEL:
        return ImGuiKey_Backspace;
    case AKEYCODE_SPACE:
        return ImGuiKey_Space;
    case AKEYCODE_ENTER:
        return ImGuiKey_Enter;
    case AKEYCODE_ESCAPE:
        return ImGuiKey_Escape;
    case AKEYCODE_APOSTROPHE:
        return ImGuiKey_Apostrophe;
    case AKEYCODE_COMMA:
        return ImGuiKey_Comma;
    case AKEYCODE_MINUS:
        return ImGuiKey_Minus;
    case AKEYCODE_PERIOD:
        return ImGuiKey_Period;
    case AKEYCODE_SLASH:
        return ImGuiKey_Slash;
    case AKEYCODE_SEMICOLON:
        return ImGuiKey_Semicolon;
    case AKEYCODE_EQUALS:
        return ImGuiKey_Equal;
    case AKEYCODE_LEFT_BRACKET:
        return ImGuiKey_LeftBracket;
    case AKEYCODE_BACKSLASH:
        return ImGuiKey_Backslash;
    case AKEYCODE_RIGHT_BRACKET:
        return ImGuiKey_RightBracket;
    case AKEYCODE_GRAVE:
        return ImGuiKey_GraveAccent;
    case AKEYCODE_CAPS_LOCK:
        return ImGuiKey_CapsLock;
    case AKEYCODE_SCROLL_LOCK:
        return ImGuiKey_ScrollLock;
    case AKEYCODE_NUM_LOCK:
        return ImGuiKey_NumLock;
    case AKEYCODE_SYSRQ:
        return ImGuiKey_PrintScreen;
    case AKEYCODE_BREAK:
        return ImGuiKey_Pause;
    case AKEYCODE_NUMPAD_0:
        return ImGuiKey_Keypad0;
    case AKEYCODE_NUMPAD_1:
        return ImGuiKey_Keypad1;
    case AKEYCODE_NUMPAD_2:
        return ImGuiKey_Keypad2;
    case AKEYCODE_NUMPAD_3:
        return ImGuiKey_Keypad3;
    case AKEYCODE_NUMPAD_4:
        return ImGuiKey_Keypad4;
    case AKEYCODE_NUMPAD_5:
        return ImGuiKey_Keypad5;
    case AKEYCODE_NUMPAD_6:
        return ImGuiKey_Keypad6;
    case AKEYCODE_NUMPAD_7:
        return ImGuiKey_Keypad7;
    case AKEYCODE_NUMPAD_8:
        return ImGuiKey_Keypad8;
    case AKEYCODE_NUMPAD_9:
        return ImGuiKey_Keypad9;
    case AKEYCODE_NUMPAD_DOT:
        return ImGuiKey_KeypadDecimal;
    case AKEYCODE_NUMPAD_DIVIDE:
        return ImGuiKey_KeypadDivide;
    case AKEYCODE_NUMPAD_MULTIPLY:
        return ImGuiKey_KeypadMultiply;
    case AKEYCODE_NUMPAD_SUBTRACT:
        return ImGuiKey_KeypadSubtract;
    case AKEYCODE_NUMPAD_ADD:
        return ImGuiKey_KeypadAdd;
    case AKEYCODE_NUMPAD_ENTER:
        return ImGuiKey_KeypadEnter;
    case AKEYCODE_NUMPAD_EQUALS:
        return ImGuiKey_KeypadEqual;
    case AKEYCODE_CTRL_LEFT:
        return ImGuiKey_LeftCtrl;
    case AKEYCODE_SHIFT_LEFT:
        return ImGuiKey_LeftShift;
    case AKEYCODE_ALT_LEFT:
        return ImGuiKey_LeftAlt;
    case AKEYCODE_META_LEFT:
        return ImGuiKey_LeftSuper;
    case AKEYCODE_CTRL_RIGHT:
        return ImGuiKey_RightCtrl;
    case AKEYCODE_SHIFT_RIGHT:
        return ImGuiKey_RightShift;
    case AKEYCODE_ALT_RIGHT:
        return ImGuiKey_RightAlt;
    case AKEYCODE_META_RIGHT:
        return ImGuiKey_RightSuper;
    case AKEYCODE_MENU:
        return ImGuiKey_Menu;
    case AKEYCODE_0:
        return ImGuiKey_0;
    case AKEYCODE_1:
        return ImGuiKey_1;
    case AKEYCODE_2:
        return ImGuiKey_2;
    case AKEYCODE_3:
        return ImGuiKey_3;
    case AKEYCODE_4:
        return ImGuiKey_4;
    case AKEYCODE_5:
        return ImGuiKey_5;
    case AKEYCODE_6:
        return ImGuiKey_6;
    case AKEYCODE_7:
        return ImGuiKey_7;
    case AKEYCODE_8:
        return ImGuiKey_8;
    case AKEYCODE_9:
        return ImGuiKey_9;
    case AKEYCODE_A:
        return ImGuiKey_A;
    case AKEYCODE_B:
        return ImGuiKey_B;
    case AKEYCODE_C:
        return ImGuiKey_C;
    case AKEYCODE_D:
        return ImGuiKey_D;
    case AKEYCODE_E:
        return ImGuiKey_E;
    case AKEYCODE_F:
        return ImGuiKey_F;
    case AKEYCODE_G:
        return ImGuiKey_G;
    case AKEYCODE_H:
        return ImGuiKey_H;
    case AKEYCODE_I:
        return ImGuiKey_I;
    case AKEYCODE_J:
        return ImGuiKey_J;
    case AKEYCODE_K:
        return ImGuiKey_K;
    case AKEYCODE_L:
        return ImGuiKey_L;
    case AKEYCODE_M:
        return ImGuiKey_M;
    case AKEYCODE_N:
        return ImGuiKey_N;
    case AKEYCODE_O:
        return ImGuiKey_O;
    case AKEYCODE_P:
        return ImGuiKey_P;
    case AKEYCODE_Q:
        return ImGuiKey_Q;
    case AKEYCODE_R:
        return ImGuiKey_R;
    case AKEYCODE_S:
        return ImGuiKey_S;
    case AKEYCODE_T:
        return ImGuiKey_T;
    case AKEYCODE_U:
        return ImGuiKey_U;
    case AKEYCODE_V:
        return ImGuiKey_V;
    case AKEYCODE_W:
        return ImGuiKey_W;
    case AKEYCODE_X:
        return ImGuiKey_X;
    case AKEYCODE_Y:
        return ImGuiKey_Y;
    case AKEYCODE_Z:
        return ImGuiKey_Z;
    case AKEYCODE_F1:
        return ImGuiKey_F1;
    case AKEYCODE_F2:
        return ImGuiKey_F2;
    case AKEYCODE_F3:
        return ImGuiKey_F3;
    case AKEYCODE_F4:
        return ImGuiKey_F4;
    case AKEYCODE_F5:
        return ImGuiKey_F5;
    case AKEYCODE_F6:
        return ImGuiKey_F6;
    case AKEYCODE_F7:
        return ImGuiKey_F7;
    case AKEYCODE_F8:
        return ImGuiKey_F8;
    case AKEYCODE_F9:
        return ImGuiKey_F9;
    case AKEYCODE_F10:
        return ImGuiKey_F10;
    case AKEYCODE_F11:
        return ImGuiKey_F11;
    case AKEYCODE_F12:
        return ImGuiKey_F12;
    default:
        return ImGuiKey_None;
    }
}

static unsigned int KeyCodeToCharacter(int32_t keyCode, bool upperCase)
{
    switch (keyCode)
    {
    case AKEYCODE_SPACE:
        return ' ';
    // case AKEYCODE_ENTER:
    //     return '\n';
    case AKEYCODE_APOSTROPHE:
        return upperCase ? '"' : '\'';
    case AKEYCODE_COMMA:
        return upperCase ? '<' : ',';
    case AKEYCODE_MINUS:
        return upperCase ? '_' : '-';
    case AKEYCODE_PERIOD:
        return upperCase ? '>' : '.';
    case AKEYCODE_SLASH:
        return upperCase ? '?' : '/';
    case AKEYCODE_SEMICOLON:
        return upperCase ? ':' : ';';
    case AKEYCODE_EQUALS:
        return upperCase ? '+' : '=';
    case AKEYCODE_LEFT_BRACKET:
        return upperCase ? '{' : '[';
    case AKEYCODE_BACKSLASH:
        return upperCase ? '|' : '\\';
    case AKEYCODE_RIGHT_BRACKET:
        return upperCase ? '}' : ']';
    case AKEYCODE_GRAVE:
        return upperCase ? '~' : '`';
    case AKEYCODE_NUMPAD_0:
        return '0';
    case AKEYCODE_NUMPAD_1:
        return '1';
    case AKEYCODE_NUMPAD_2:
        return '2';
    case AKEYCODE_NUMPAD_3:
        return '3';
    case AKEYCODE_NUMPAD_4:
        return '4';
    case AKEYCODE_NUMPAD_5:
        return '5';
    case AKEYCODE_NUMPAD_6:
        return '6';
    case AKEYCODE_NUMPAD_7:
        return '7';
    case AKEYCODE_NUMPAD_8:
        return '8';
    case AKEYCODE_NUMPAD_9:
        return '9';
    case AKEYCODE_NUMPAD_DOT:
        return '.';
    case AKEYCODE_NUMPAD_DIVIDE:
        return '/';
    case AKEYCODE_NUMPAD_MULTIPLY:
        return '*';
    case AKEYCODE_NUMPAD_SUBTRACT:
        return '-';
    case AKEYCODE_NUMPAD_ADD:
        return '+';
    // case AKEYCODE_NUMPAD_ENTER:
    //     return '\n';
    case AKEYCODE_NUMPAD_EQUALS:
        return '=';
    case AKEYCODE_0:
        return upperCase ? ')' : '0';
    case AKEYCODE_1:
        return upperCase ? '!' : '1';
    case AKEYCODE_2:
        return upperCase ? '@' : '2';
    case AKEYCODE_3:
        return upperCase ? '#' : '3';
    case AKEYCODE_4:
        return upperCase ? '$' : '4';
    case AKEYCODE_5:
        return upperCase ? '%' : '5';
    case AKEYCODE_6:
        return upperCase ? '^' : '6';
    case AKEYCODE_7:
        return upperCase ? '&' : '7';
    case AKEYCODE_8:
        return upperCase ? '*' : '8';
    case AKEYCODE_9:
        return upperCase ? '(' : '9';
    case AKEYCODE_A:
        return upperCase ? 'A' : 'a';
    case AKEYCODE_B:
        return upperCase ? 'B' : 'b';
    case AKEYCODE_C:
        return upperCase ? 'C' : 'c';
    case AKEYCODE_D:
        return upperCase ? 'D' : 'd';
    case AKEYCODE_E:
        return upperCase ? 'E' : 'e';
    case AKEYCODE_F:
        return upperCase ? 'F' : 'f';
    case AKEYCODE_G:
        return upperCase ? 'G' : 'g';
    case AKEYCODE_H:
        return upperCase ? 'H' : 'h';
    case AKEYCODE_I:
        return upperCase ? 'I' : 'i';
    case AKEYCODE_J:
        return upperCase ? 'J' : 'j';
    case AKEYCODE_K:
        return upperCase ? 'K' : 'k';
    case AKEYCODE_L:
        return upperCase ? 'L' : 'l';
    case AKEYCODE_M:
        return upperCase ? 'M' : 'm';
    case AKEYCODE_N:
        return upperCase ? 'N' : 'n';
    case AKEYCODE_O:
        return upperCase ? 'O' : 'o';
    case AKEYCODE_P:
        return upperCase ? 'P' : 'p';
    case AKEYCODE_Q:
        return upperCase ? 'Q' : 'q';
    case AKEYCODE_R:
        return upperCase ? 'R' : 'r';
    case AKEYCODE_S:
        return upperCase ? 'S' : 's';
    case AKEYCODE_T:
        return upperCase ? 'T' : 't';
    case AKEYCODE_U:
        return upperCase ? 'U' : 'u';
    case AKEYCODE_V:
        return upperCase ? 'V' : 'v';
    case AKEYCODE_W:
        return upperCase ? 'W' : 'w';
    case AKEYCODE_X:
        return upperCase ? 'X' : 'x';
    case AKEYCODE_Y:
        return upperCase ? 'Y' : 'y';
    case AKEYCODE_Z:
        return upperCase ? 'Z' : 'z';
    default:
        return 0;
    }
}

namespace android
{
    // RenderClient 单例 (每个进程只有一个 RenderClient AImGui), 供 PublishMenuRectToServer
    // 全局函数定位激活实例.
    std::atomic<AImGui*> g_renderClientInstance{nullptr};

    // 旁路 packet 协议: client -> server, 用极不可能的 packetSize 哨兵 + 16 字节 payload (4 floats).
    static constexpr uint32_t kSidePacketMagic_MenuRect = 0xF1A75001u;

    // 前向声明: 在 EndFrame 中使用, 实现位于 PollInputReady 之后.
    static void AutoPublishMenusFromImGui(AImGui* self,
                                          std::vector<float>& scratch,
                                          uint64_t& outHash,
                                          float screenW, float screenH);

    AImGui::AImGui(const Options &options)
        : m_options(options)
    {
        LogInfo("[AImGui] ctor begin renderType=%d autoUpdateOrientation=%d screenWidth=%d screenHeight=%d rotateTheta=%d",
                static_cast<int>(m_options.renderType),
                m_options.autoUpdateOrientation ? 1 : 0,
                m_options.screenWidth,
                m_options.screenHeight,
                m_options.rotateTheta);
        InitEnvironment();
        LogInfo("[AImGui] ctor end state=%d nativeWindow=%p eglDisplay=%p eglSurface=%p eglContext=%p",
                m_state ? 1 : 0,
                m_nativeWindow,
                m_defaultDisplay,
                m_eglSurface,
                m_eglContext);
        // RenderClient: 注册自己为当前进程的 "菜单 rect 发布者" (单例).
        if (RenderType::RenderClient == m_options.renderType) {
            extern std::atomic<AImGui*> g_renderClientInstance;
            g_renderClientInstance.store(this, std::memory_order_release);
        }
    }

    AImGui::~AImGui()
    {
        if (RenderType::RenderClient == m_options.renderType) {
            extern std::atomic<AImGui*> g_renderClientInstance;
            AImGui* expected = this;
            g_renderClientInstance.compare_exchange_strong(expected, nullptr,
                                                           std::memory_order_acq_rel);
        }
        UnInitEnvironment();
    }

    void AImGui::BeginFrame()
    {
        if (!m_state)
            return;

        if (RenderType::RenderClient != m_options.renderType && nullptr != m_nativeWindow)
        {
            int currentWidth = ANativeWindow_getWidth(m_nativeWindow);
            int currentHeight = ANativeWindow_getHeight(m_nativeWindow);
            if (0 < currentWidth && 0 < currentHeight)
            {
                m_screenWidth = currentWidth;
                m_screenHeight = currentHeight;
            }
        }

        if (m_options.autoUpdateOrientation)
        {
            auto displayInfo = ANativeWindowCreator::GetDisplayInfo();

            // Check if display orientation is changed
            if (m_rotateTheta != displayInfo.theta)
            {
                UnInitEnvironment();
                InitEnvironment();
            }
        }

        ANativeWindowCreator::ProcessMirrorDisplay();

        ImGui_ImplOpenGL3_NewFrame();
        if (RenderType::RenderClient != m_options.renderType)
            ImGui_ImplAndroid_NewFrame();
        else
        {
            // Copy from imgui_impl_android.cpp
            timespec currentTimeSpec{};
            auto &imguiIO = ImGui::GetIO();

            imguiIO.DisplaySize = {static_cast<float>(m_screenWidth), static_cast<float>(m_screenHeight)};
            imguiIO.DisplayFramebufferScale = {1.0f, 1.0f};

            clock_gettime(CLOCK_MONOTONIC, &currentTimeSpec);
            double currentTime = static_cast<double>(currentTimeSpec.tv_sec) + currentTimeSpec.tv_nsec / 1000000000.0;
            imguiIO.DeltaTime = 0.0 < m_lastTime ? static_cast<float>(currentTime - m_lastTime) : static_cast<float>(1.0f / 60.0f);
            m_lastTime = currentTime;
        }
        if (RenderType::RenderClient == m_options.renderType || RenderType::RenderNative == m_options.renderType)
            ImGui::NewFrame();
    }
    void AImGui::EndFrame()
    {
        if (!m_state)
            return;

        if (RenderType::RenderClient == m_options.renderType)
        {
            // 自动收集 ImGui 顶层菜单 rect, 节流后通过旁路 packet 发给 server.
            // 必须在 ImGui::Render() 之前调用 (Render 之后 ctx->Windows 仍然保留, 但
            // 在此处先取最稳妥; 注: 调用顺序对结果无影响, Windows 已在 NewFrame/Begin 阶段更新).
            {
                static thread_local std::vector<float> s_menuScratch;
                uint64_t hash = 0;
                AutoPublishMenusFromImGui(this, s_menuScratch,
                                          hash,
                                          ImGui::GetIO().DisplaySize.x,
                                          ImGui::GetIO().DisplaySize.y);

                const auto now = std::chrono::steady_clock::now();
                const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_menuRectLastSendTime).count();
                const bool changed = (hash != m_menuRectLastSentHash);
                // 节流: 100ms 内变化或 1s 心跳 (心跳保证 server 端连接重建后能尽快拿到 rect).
                if ((changed && elapsedMs >= 100) || elapsedMs >= 1000)
                {
                    uint32_t magic = kSidePacketMagic_MenuRect;
                    uint32_t count = static_cast<uint32_t>(s_menuScratch.size() / 4);
                    if (count > kMaxMenuRects) count = kMaxMenuRects;
                    WriteData(&magic, sizeof(magic));
                    WriteData(&count, sizeof(count));
                    if (count > 0) {
                        WriteData(s_menuScratch.data(), count * 4 * sizeof(float));
                    }
                    m_menuRectLastSendTime = now;
                    m_menuRectLastSentHash = hash;
                }
            }

            ImGui::Render();
            const auto &sharedData = ImGui::GetSharedDrawData();
            if (!sharedData.empty())
            {
                if (!m_options.compressionFrameData)
                {
                    uint32_t packetSize = static_cast<uint32_t>(sharedData.size());
                    WriteData(&packetSize, sizeof(packetSize));
                    WriteData(const_cast<uint8_t *>(sharedData.data()), sharedData.size());
                    m_renderPacketCount++;
                    m_lastRenderPacketSize = packetSize;
                    m_lastRenderDecodedSize = sharedData.size();
                }
                else
                {
                    static std::vector<uint8_t> compressBuffer;
                    static std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> compressContext(ZSTD_createCCtx(), &ZSTD_freeCCtx);
                    size_t compressBufferSize = 7 + ZSTD_compressBound(sharedData.size());

                    if (nullptr == compressContext)
                    {
                        LogDebug("[-] Client can not create compress context");
                        m_state = false;
                        return;
                    }
                    if (compressBuffer.empty()) // First compression
                    {
                        ZSTD_CCtx_setParameter(compressContext.get(), ZSTD_c_compressionLevel, ZSTD_defaultCLevel());
                        ZSTD_CCtx_setParameter(compressContext.get(), ZSTD_c_checksumFlag, 1);
                    }
                    if (compressBuffer.size() < compressBufferSize)
                        compressBuffer.resize(compressBufferSize);

                    ZSTD_inBuffer input = {sharedData.data(), sharedData.size(), 0};
                    ZSTD_outBuffer output = {compressBuffer.data(), compressBuffer.size(), 0};
                    if (0 == ZSTD_compressStream2(compressContext.get(), &output, &input, ZSTD_e_end))
                    {
                        uint32_t packetSize = sizeof(uint32_t) + output.pos;
                        WriteData(&packetSize, sizeof(packetSize));
                        uint32_t sharedDataSize = sharedData.size();
                        WriteData(&sharedDataSize, sizeof(sharedDataSize));
                        WriteData(compressBuffer.data(), output.pos);
                        m_renderPacketCount++;
                        m_lastRenderPacketSize = packetSize;
                        m_lastRenderDecodedSize = sharedDataSize;
                    }
                    else
                        LogDebug("[-] Client compression frame data error");
                }

                if (0 == (m_renderPacketCount % 180))
                {
                    LogInfo("[AImGui] Client sent render packets=%llu lastPacket=%zu decoded=%zu",
                            static_cast<unsigned long long>(m_renderPacketCount),
                            m_lastRenderPacketSize,
                            m_lastRenderDecodedSize);
                }
            }
        }
        else if (RenderType::RenderServer == m_options.renderType)
        {
            switch (m_renderState)
            {
            case RenderState::ReadData:
            {
                // 客户端断连后持续清屏 + 交换, 确保所有 EGL 后备缓冲区里的旧画面都被擦掉
                // (单次 swap 只清掉一个 back buffer, Android 通常有 2-3 个缓冲, 必须连续清多帧)
                if (!m_clientConnected.load(std::memory_order_acquire)) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - m_clientDisconnectTime).count();
                    if (elapsed >= 800) {  // 短宽限期, 防止瞬断闪烁
                        glClear(GL_COLOR_BUFFER_BIT);
                        eglSwapBuffers(m_defaultDisplay, m_eglSurface);
                        if (m_renderFrameCount != 0) {
                            std::lock_guard<std::mutex> lock(m_renderDataMutex);
                            m_serverRenderData.clear();
                            m_renderFrameCount = 0;
                        }
                    }
                }
                break;
            }
            case RenderState::SetFont:
            {
                if (!m_options.exchangeFontData)
                {
                    m_renderState = RenderState::ReadData;
                    break;
                }

                {
                    std::lock_guard<std::mutex> lock(m_renderDataMutex);
                    ImGui_ImplOpenGL3_DestroyFontsTexture();
                    ImGui::SetSharedFontData(m_serverFontData);
                    ImGui_ImplOpenGL3_CreateFontsTexture();
                    LogInfo("[AImGui] Server applied font packet count=%llu size=%zu",
                            static_cast<unsigned long long>(m_fontPacketCount),
                            m_lastFontPacketSize);
                }
                m_renderState = RenderState::ReadData;

                break;
            }
            case RenderState::Rendering:
            {
                std::lock_guard<std::mutex> lock(m_renderDataMutex);
                auto drawData = ImGui::RenderSharedDrawData(m_serverRenderData);
                if (nullptr != drawData)
                {
                    if (m_options.exchangeFontData)
                    {
                        for (const auto &cmdList : drawData->CmdLists)
                        {
                            for (auto &cmd : cmdList->CmdBuffer)
                                cmd.TextureId = ImGui::GetIO().Fonts->TexID;
                        }
                    }

                    int totalCmdCount = 0;
                    for (int cmdListIndex = 0; cmdListIndex < drawData->CmdListsCount; ++cmdListIndex)
                        totalCmdCount += drawData->CmdLists[cmdListIndex]->CmdBuffer.Size;

                    if (0 == drawData->CmdListsCount || 0 == totalCmdCount)
                    {
                        m_renderState = RenderState::ReadData;
                        break;
                    }

                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(drawData);
                    eglSwapBuffers(m_defaultDisplay, m_eglSurface);
                    m_renderFrameCount++;
                    if (0 == (m_renderFrameCount % 180))
                    {
                        LogInfo("[AImGui] Server rendered frames=%llu packets=%llu drawLists=%d cmds=%d decoded=%zu",
                                static_cast<unsigned long long>(m_renderFrameCount),
                                static_cast<unsigned long long>(m_renderPacketCount),
                                drawData->CmdListsCount,
                                totalCmdCount,
                                m_lastRenderDecodedSize);
                    }
                }
                else
                {
                    LogError("[AImGui] Server RenderSharedDrawData returned null decoded=%zu packetCount=%llu",
                             m_lastRenderDecodedSize,
                             static_cast<unsigned long long>(m_renderPacketCount));
                }
                m_renderState = RenderState::ReadData;

                break;
            }
            default:
                break;
            }
        }
        else if (RenderType::RenderNative == m_options.renderType)
        {
            ImGui::Render();
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            eglSwapBuffers(m_defaultDisplay, m_eglSurface);
        }
    }

    void AImGui::ProcessInputEvent()
    {
        ATouchEvent::TouchEvent event{};

        if (!m_state)
            return;

        if (RenderType::RenderServer == m_options.renderType || RenderType::RenderNative == m_options.renderType)
        {
            static ATouchEvent touchEvent;

            if (!touchEvent.GetTouchEvent(&event))
                return;
            event.TransformToScreen(m_screenWidth, m_screenHeight, m_rotateTheta);

            if (RenderType::RenderServer == m_options.renderType)
            {
                if (-1 == m_clientFd)
                    return;
                std::lock_guard<std::mutex> lock(m_writeMutex);
                WriteData(&event, sizeof(event));
            }
        }
        else
        {
            auto readResult = ReadData(&event, sizeof(event));
            if (0 >= readResult)
            {
                // LogDebug("[-] Client can not read input event, readResult:%d  %d:%s", readResult, errno, strerror(errno));
                return;
            }
        }

        if (RenderType::RenderClient == m_options.renderType || RenderType::RenderNative == m_options.renderType)
        {
            auto &imguiIO = ImGui::GetIO();
            switch (event.type)
            {
            case ATouchEvent::EventType::Move:
            {
                imguiIO.AddMousePosEvent(event.x, event.y);
                break;
            }
            case ATouchEvent::EventType::TouchDown:
            case ATouchEvent::EventType::TouchUp:
            {
                imguiIO.AddMousePosEvent(event.x, event.y);
                imguiIO.AddMouseButtonEvent(0, ATouchEvent::EventType::TouchDown == event.type);
                break;
            }
            case ATouchEvent::EventType::KeyDown:
            case ATouchEvent::EventType::KeyUp:
            {
                auto imguiKey = KeyCodeToImGuiKey(event.keyCode);
                if (ImGuiKey_None == imguiKey)
                    break;

                switch (imguiKey)
                {
                case ImGuiKey_LeftCtrl:
                case ImGuiKey_RightCtrl:
                    imguiIO.AddKeyEvent(ImGuiMod_Ctrl, ATouchEvent::EventType::KeyDown == event.type);
                    break;
                case ImGuiKey_LeftShift:
                case ImGuiKey_RightShift:
                    imguiIO.AddKeyEvent(ImGuiMod_Shift, ATouchEvent::EventType::KeyDown == event.type);
                    break;
                case ImGuiKey_LeftAlt:
                case ImGuiKey_RightAlt:
                    imguiIO.AddKeyEvent(ImGuiMod_Alt, ATouchEvent::EventType::KeyDown == event.type);
                    break;
                default:
                    break;
                }
                imguiIO.AddKeyEvent(imguiKey, ATouchEvent::EventType::KeyDown == event.type);
                imguiIO.SetKeyEventNativeData(imguiKey, event.keyCode, event.scanCode);

                if (ATouchEvent::EventType::KeyDown != event.type)
                    break;
                unsigned int character = KeyCodeToCharacter(event.keyCode, ImGui::IsKeyDown(ImGuiMod_Shift));
                if (imguiIO.WantTextInput && 0 != character)
                    imguiIO.AddInputCharacter(character);
                break;
            }
            case ATouchEvent::EventType::Wheel:
            {
                imguiIO.AddMousePosEvent(std::abs(event.x), event.y);
                imguiIO.AddMouseWheelEvent(0, 0 > event.x ? -1 : 1);
                break;
            }
            default:
                break;
            }
        }
    }

    void AImGui::InjectExternalTouch(int action, float x, float y)
    {
        if (!m_state)
            return;

        ATouchEvent::TouchEvent ev{};
        ev.x = static_cast<int>(x);
        ev.y = static_cast<int>(y);
        ev.scanCode = 0;
        ev.keyCode  = 0;
        switch (action)
        {
            case 0: ev.type = ATouchEvent::EventType::TouchDown; break;
            case 1: ev.type = ATouchEvent::EventType::Move;      break;
            case 2: // Up
            case 3: // Cancel
                ev.type = ATouchEvent::EventType::TouchUp;       break;
            default: return;
        }

        // 已经在显示坐标系 (Java MotionEvent.getX/Y), 不再做旋转/缩放.
        // 仅做 clamp 防止越界引发 client 端 ImGui 命中测试异常.
        if (m_screenWidth > 0)
            ev.x = std::clamp(ev.x, 0, m_screenWidth - 1);
        if (m_screenHeight > 0)
            ev.y = std::clamp(ev.y, 0, m_screenHeight - 1);

        if (RenderType::RenderServer == m_options.renderType)
        {
            if (-1 == m_clientFd)
                return;
            std::lock_guard<std::mutex> lock(m_writeMutex);
            // 二次检查 (上锁后)
            if (-1 == m_clientFd)
                return;
            WriteData(&ev, sizeof(ev));
        }
        else if (RenderType::RenderNative == m_options.renderType)
        {
            auto &io = ImGui::GetIO();
            switch (ev.type)
            {
            case ATouchEvent::EventType::Move:
                io.AddMousePosEvent(static_cast<float>(ev.x), static_cast<float>(ev.y));
                break;
            case ATouchEvent::EventType::TouchDown:
            case ATouchEvent::EventType::TouchUp:
                io.AddMousePosEvent(static_cast<float>(ev.x), static_cast<float>(ev.y));
                io.AddMouseButtonEvent(0, ATouchEvent::EventType::TouchDown == ev.type);
                break;
            default:
                break;
            }
        }
    }

    void AImGui::PublishMenuRect(float x, float y, float w, float h)
    {
        // 兼容旧 API: 转发到自动 publish 的内部路径 (不再节流, 由 EndFrame 处统一节流).
        // 留作未来扩展; 当前业务代码不再调用.
        (void)x; (void)y; (void)w; (void)h;
    }

    // 内部: EndFrame 中自动调用, 枚举所有 ImGui 顶层窗口, 过滤出可交互菜单, 发给 server.
    static void AutoPublishMenusFromImGui(AImGui* /*self*/,
                                          std::vector<float>& scratch,
                                          uint64_t& outHash,
                                          float screenW, float screenH)
    {
        scratch.clear();
        outHash = 0;

        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx) return;

        const float kAlmostFullW = std::max(1.0f, screenW * 0.9f);
        const float kAlmostFullH = std::max(1.0f, screenH * 0.9f);

        int collected = 0;
        for (ImGuiWindow* w : ctx->Windows)
        {
            if (!w) continue;
            if (!w->WasActive) continue;
            if (w->Hidden) continue;
            // 排除带 NoInputs 标志的"画布"窗口 (ESP / Minimap / WorldObjs / AI 全屏覆盖层都设了它)
            if (w->Flags & ImGuiWindowFlags_NoInputs) continue;
            // 排除子/弹窗/Tooltip
            if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)) continue;
            // 排除接近全屏的窗口 (避免吞掉整个游戏画面)
            if (w->Size.x >= kAlmostFullW && w->Size.y >= kAlmostFullH) continue;
            if (w->Size.x < 4.0f || w->Size.y < 4.0f) continue;

            scratch.push_back(w->Pos.x);
            scratch.push_back(w->Pos.y);
            scratch.push_back(w->Size.x);
            scratch.push_back(w->Size.y);

            // 简易 hash (FNV-ish), 用于跨帧节流判等
            auto mix = [&](float v) {
                uint32_t bits = 0;
                std::memcpy(&bits, &v, sizeof(bits));
                outHash ^= bits + 0x9E3779B9ULL + (outHash << 6) + (outHash >> 2);
            };
            mix(w->Pos.x); mix(w->Pos.y); mix(w->Size.x); mix(w->Size.y);

            if (++collected >= AImGui::kMaxMenuRects) break;
        }
        outHash ^= static_cast<uint64_t>(collected) * 0x100000001B3ULL;
    }

    int AImGui::QueryMenuRects(float* outXywh, int capacity) const
    {
        if (!outXywh || capacity <= 0) return 0;
        std::lock_guard<std::mutex> lock(m_menuRectsMutex);
        const int n = std::min<int>(capacity, m_menuRectsCount.load(std::memory_order_relaxed));
        for (int i = 0; i < n * 4; ++i)
            outXywh[i] = m_menuRects[i];
        return n;
    }

    bool AImGui::PollInputReady(int timeoutMs) const
    {        if (!m_state || m_clientFd == -1)
            return false;
        pollfd pfd{.fd = m_clientFd, .events = POLLIN};
        return poll(&pfd, 1, timeoutMs) > 0 && (pfd.revents & POLLIN);
    }

    void AImGui::SetupWindowInfo(void *windowInfo)
    {
        ANativeWindowCreator::UpdateWindowInfo(m_nativeWindow, windowInfo);
    }

    bool AImGui::InitEnvironment()
    {
        LogInfo("[AImGui] InitEnvironment begin renderType=%d", static_cast<int>(m_options.renderType));

        m_usesExternalNativeWindow = nullptr != m_options.externalNativeWindow;

        // Initialize rpc
        m_transportAddress.sin_family = AF_INET;
        m_transportAddress.sin_port = htons(static_cast<uint16_t>(m_options.port > 0 ? m_options.port : 16888));
        if (RenderType::RenderClient == m_options.renderType)
        {
            inet_pton(AF_INET, m_options.clientConnectAddress.data(), &m_transportAddress.sin_addr);

            m_clientFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (0 > m_clientFd)
            {
                LogError("[-] Client fd create failed, m_clientFd:%d errno=%d:%s", m_clientFd, errno, strerror(errno));
                return false;
            }

            if (m_options.tcpNoDelay)
            {
                int optionValue = 1;
                setsockopt(m_clientFd, IPPROTO_TCP, TCP_NODELAY, &optionValue, sizeof(optionValue));
            }

            if (0 > connect(m_clientFd, reinterpret_cast<sockaddr *>(&m_transportAddress), sizeof(m_transportAddress)))
            {
                LogError("[-] Client connect to %s:%d failed, %d:%s",
                         m_options.clientConnectAddress.c_str(),
                         ntohs(m_transportAddress.sin_port),
                         errno,
                         strerror(errno));
                return false;
            }

            LogInfo("[AImGui] Client connected to %s:%d fd=%d",
                    m_options.clientConnectAddress.c_str(),
                    ntohs(m_transportAddress.sin_port),
                    m_clientFd);
        }
        else if (RenderType::RenderServer == m_options.renderType)
        {
            inet_pton(AF_INET, m_options.serverListenAddress.data(), &m_transportAddress.sin_addr);

            m_serverFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (0 > m_serverFd)
            {
                LogError("[-] Server fd create failed, m_serverFd:%d errno=%d:%s", m_serverFd, errno, strerror(errno));
                return false;
            }
            int optionValue = 1;
            if (0 > setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &optionValue, sizeof(int)))
            {
                LogError("[-] Server fd set reuseaddr failed, m_serverFd:%d errno=%d:%s", m_serverFd, errno, strerror(errno));
                return false;
            }

            if (0 > bind(m_serverFd, reinterpret_cast<sockaddr *>(&m_transportAddress), sizeof(m_transportAddress)))
            {
                LogDebug("[-] Server bind fd failed, %d:%s", errno, strerror(errno));
                return false;
            }

            if (0 > listen(m_serverFd, 1))
            {
                LogDebug("[-] Server listen fd failed, %d:%s", errno, strerror(errno));
                return false;
            }

            LogInfo("[AImGui] Server listening on %s:%d fd=%d",
                    m_options.serverListenAddress.c_str(),
                    ntohs(m_transportAddress.sin_port),
                    m_serverFd);
        }

        int displayTheta = m_options.rotateTheta;
        int displayWidth = m_options.screenWidth;
        int displayHeight = m_options.screenHeight;
        if (m_usesExternalNativeWindow)
        {
            if (0 >= displayWidth)
                displayWidth = ANativeWindow_getWidth(m_options.externalNativeWindow);
            if (0 >= displayHeight)
                displayHeight = ANativeWindow_getHeight(m_options.externalNativeWindow);
        }
        if (0 >= displayWidth || 0 >= displayHeight)
        {
            LogInfo("[AImGui] display size missing, query GetDisplayInfo");
            auto displayInfo = ANativeWindowCreator::GetDisplayInfo();
            displayTheta = displayInfo.theta;
            displayWidth = displayInfo.width;
            displayHeight = displayInfo.height;
        }
        LogInfo("[AImGui] display angle:%d width:%d height:%d", displayTheta, displayWidth, displayHeight);

        if (RenderType::RenderClient != m_options.renderType)
        {
            if (m_usesExternalNativeWindow)
            {
                m_nativeWindow = m_options.externalNativeWindow;
                LogInfo("[AImGui] using external native window=%p size=%dx%d", m_nativeWindow, displayWidth, displayHeight);
            }
            else
            {
                // Create native window
                LogInfo("[AImGui] creating native window");
                m_nativeWindow = ANativeWindowCreator::Create({.name = "AImGui", .width = displayWidth, .height = displayHeight, .skipScreenshot = false});
                if (nullptr == m_nativeWindow)
                {
                    LogError("[AImGui] ANativeWindow create failed");
                    return false;
                }

                // Acquire native window created by the private compositor path.
                LogInfo("[AImGui] acquiring native window=%p", m_nativeWindow);
                ANativeWindow_acquire(m_nativeWindow);
            }
            LogInfo("[AImGui] native window acquired size=%dx%d", ANativeWindow_getWidth(m_nativeWindow), ANativeWindow_getHeight(m_nativeWindow));
        }

        // EGL initialization
        LogInfo("[AImGui] eglGetDisplay begin");
        m_defaultDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (EGL_NO_DISPLAY == m_defaultDisplay)
        {
            LogError("[AImGui] EGL get default display failed: %d", eglGetError());
            return false;
        }

        LogInfo("[AImGui] eglInitialize begin");
        if (EGL_TRUE != eglInitialize(m_defaultDisplay, 0, 0))
        {
            LogError("[AImGui] EGL initialize failed: %d", eglGetError());
            return false;
        }

        EGLint numEglConfig = 0;
        EGLConfig eglConfig{};
        std::pair<EGLint, EGLint> eglConfigAttributeList[] = {
            {
                EGL_SURFACE_TYPE,
                RenderType::RenderClient != m_options.renderType ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
            },                                         // 根据服务类型选择渲染表面类型为窗口或像素缓冲区
            {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT}, // 使用OpenGL ES 2.0
            {EGL_RED_SIZE, 8},                         // 红色分量位数为8位
            {EGL_GREEN_SIZE, 8},                       // 绿色分量位数为8位
            {EGL_BLUE_SIZE, 8},                        // 蓝色分量位数为8位
            {EGL_ALPHA_SIZE, 8},                       // Alpha 位数为8位
            {EGL_DEPTH_SIZE, 24},                      // 深度缓冲位数为24位
            {EGL_STENCIL_SIZE, 8},                     // 模板缓冲位数为8位
            {EGL_SAMPLE_BUFFERS, 0},                   // 多重采样抗锯齿缓冲禁用
            {EGL_NONE, EGL_NONE},
        };
        LogInfo("[AImGui] eglChooseConfig begin");
        if (EGL_TRUE != eglChooseConfig(m_defaultDisplay, reinterpret_cast<const EGLint *>(eglConfigAttributeList), &eglConfig, 1, &numEglConfig))
        {
            LogError("[AImGui] EGL choose config failed: %d", eglGetError());
            return false;
        }
        if (0 == numEglConfig)
        {
            LogError("[AImGui] EGL choose config failed: unsupported config attribute list");
            return false;
        }

        if (RenderType::RenderClient != m_options.renderType)
        {
            EGLint eglBufferFormat;
            LogInfo("[AImGui] eglGetConfigAttrib begin");
            if (EGL_TRUE != eglGetConfigAttrib(m_defaultDisplay, eglConfig, EGL_NATIVE_VISUAL_ID, &eglBufferFormat))
            {
                LogError("[AImGui] EGL get config attribute failed: %d", eglGetError());
                return false;
            }
            LogInfo("[AImGui] ANativeWindow_setBuffersGeometry begin format=%d", eglBufferFormat);
            ANativeWindow_setBuffersGeometry(m_nativeWindow, 0, 0, eglBufferFormat);
            LogInfo("[AImGui] eglCreateWindowSurface begin nativeWindow=%p", m_nativeWindow);
            m_eglSurface = eglCreateWindowSurface(m_defaultDisplay, eglConfig, m_nativeWindow, nullptr);
        }
        else
        {
            std::pair<EGLint, EGLint> bufferAttribute[] = {
                {EGL_WIDTH, displayWidth},
                {EGL_HEIGHT, displayHeight},
                {EGL_NONE, EGL_NONE},
            };
            LogInfo("[AImGui] eglCreatePbufferSurface begin");
            m_eglSurface = eglCreatePbufferSurface(m_defaultDisplay, eglConfig, reinterpret_cast<const EGLint *>(bufferAttribute));
        }
        if (EGL_NO_SURFACE == m_eglSurface)
        {
            LogError("[AImGui] EGL create surface failed: %d", eglGetError());
            return false;
        }

        const char* glslVersion = "#version 300 es";
        int eglContextClientVersion = 3;

        const EGLint eglContextAttribListGles3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const EGLint eglContextAttribListGles2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

        LogInfo("[AImGui] eglCreateContext begin prefer=GLES3");
        m_eglContext = eglCreateContext(m_defaultDisplay, eglConfig, EGL_NO_CONTEXT, eglContextAttribListGles3);
        if (EGL_NO_CONTEXT == m_eglContext)
        {
            auto eglError = eglGetError();
            LogInfo("[AImGui] EGL create GLES3 context failed: %d, fallback to GLES2", eglError);
            m_eglContext = eglCreateContext(m_defaultDisplay, eglConfig, EGL_NO_CONTEXT, eglContextAttribListGles2);
            if (EGL_NO_CONTEXT == m_eglContext)
            {
                LogError("[AImGui] EGL create context failed after GLES2 fallback: %d", eglGetError());
                return false;
            }

            eglContextClientVersion = 2;
            glslVersion = "#version 100";
        }

        LogInfo("[AImGui] eglMakeCurrent begin");
        if (EGL_TRUE != eglMakeCurrent(m_defaultDisplay, m_eglSurface, m_eglSurface, m_eglContext))
        {
            LogError("[AImGui] EGL make current failed: %d", eglGetError());
            return false;
        }

        if (RenderType::RenderClient != m_options.renderType && m_options.disableVsync)
        {
            eglSwapInterval(m_defaultDisplay, 0);
            LogInfo("[AImGui] eglSwapInterval set to 0 for low latency");
        }

        const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* glslVersionRuntime = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        LogInfo("[AImGui] GL runtime context=GLES%d version=%s glsl=%s",
                eglContextClientVersion,
                glVersion ? glVersion : "(null)",
                glslVersionRuntime ? glslVersionRuntime : "(null)");

        // ImGui initialization
        LogInfo("[AImGui] ImGui init begin");
        IMGUI_CHECKVERSION();

        m_imguiContext = ImGui::CreateContext();
        if (nullptr == m_imguiContext)
        {
            LogError("[AImGui] ImGui create context failed");
            return false;
        }

        auto &imguiIO = ImGui::GetIO();

        imguiIO.IniFilename = nullptr;
        imguiIO.ConfigInputTrickleEventQueue = false;
        ImGui::StyleColorsDark();
        if (m_options.styleScale > 0.0f && m_options.styleScale != 1.0f)
            ImGui::GetStyle().ScaleAllSizes(m_options.styleScale);

        LoadPreferredImGuiFont(imguiIO, m_options.fontSizePixels > 0.0f ? m_options.fontSizePixels : 18.0f);
        if (RenderType::RenderClient == m_options.renderType && m_options.exchangeFontData)
        {
            auto sharedFontData = ImGui::GetSharedFontData();
            uint32_t packetSize = static_cast<uint32_t>(sharedFontData.size());
            // First packet
            WriteData(&packetSize, sizeof(packetSize));
            WriteData(sharedFontData.data(), sharedFontData.size());
            m_fontPacketCount = 1;
            m_lastFontPacketSize = sharedFontData.size();
            LogInfo("[AImGui] Client sent initial font packet size=%u", packetSize);
        }

        if (RenderType::RenderClient != m_options.renderType)
        {
            if (!ImGui_ImplAndroid_Init(m_nativeWindow))
            {
                LogError("[AImGui] ImGui init android implement failed");
                return false;
            }
        }
        else
        {
            imguiIO.BackendPlatformName = "imgui_impl_aimgui";
        }
        LogInfo("[AImGui] ImGui OpenGL backend init glsl=%s", glslVersion);
        if (!ImGui_ImplOpenGL3_Init(glslVersion))
        {
            LogError("[AImGui] ImGui init OpenGL3 failed");
            return false;
        }

        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.f, 0.f, 0.f, 0.f);

        m_rotateTheta = displayTheta;
        m_screenWidth = displayWidth;
        m_screenHeight = displayHeight;

        LogInfo("[AImGui] InitEnvironment success screen=%dx%d rotate=%d", m_screenWidth, m_screenHeight, m_rotateTheta);

        m_state = true;
        if (RenderType::RenderServer == m_options.renderType)
        {
            m_serverWorkerThread = std::make_unique<std::thread>(&AImGui::ServerWorker, this);
            LogInfo("[AImGui] Server worker thread started");
        }

        return true;
    }
    void AImGui::UnInitEnvironment()
    {
        m_state = false;

        int clientFd = m_clientFd;
        int serverFd = m_serverFd;
        m_clientFd = -1;
        m_serverFd = -1;

        if (-1 != clientFd)
        {
            shutdown(clientFd, SHUT_RDWR);
            close(clientFd);
        }
        if (-1 != serverFd)
        {
            shutdown(serverFd, SHUT_RDWR);
            close(serverFd);
        }

        if (nullptr != m_imguiContext)
        {
            ImGui_ImplOpenGL3_Shutdown();

            if (RenderType::RenderClient != m_options.renderType)
                ImGui_ImplAndroid_Shutdown();
            else
                ImGui::GetIO().BackendPlatformName = nullptr;
            ImGui::DestroyContext(m_imguiContext);
        }

        if (EGL_NO_DISPLAY != m_defaultDisplay)
        {
            eglMakeCurrent(m_defaultDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

            if (EGL_NO_CONTEXT != m_eglContext)
                eglDestroyContext(m_defaultDisplay, m_eglContext);
            if (EGL_NO_SURFACE != m_eglSurface)
                eglDestroySurface(m_defaultDisplay, m_eglSurface);

            eglTerminate(m_defaultDisplay);
        }

        if (RenderType::RenderClient != m_options.renderType)
        {
            if (nullptr != m_nativeWindow)
            {
                ANativeWindow_release(m_nativeWindow);
                if (!m_usesExternalNativeWindow)
                    android::ANativeWindowCreator::Destroy(m_nativeWindow);
            }
        }

        if (m_serverWorkerThread && m_serverWorkerThread->joinable())
            m_serverWorkerThread->join();
        m_serverWorkerThread.reset();

        m_imguiContext = nullptr;
        m_eglContext = EGL_NO_CONTEXT;
        m_eglSurface = EGL_NO_SURFACE;
        m_defaultDisplay = EGL_NO_DISPLAY;
        m_nativeWindow = nullptr;
        m_usesExternalNativeWindow = false;
    }

    void AImGui::ServerWorker()
    {
        while (m_state)
        {
            // 确保 listen socket 有效 (进程被 freeze/unfreeze 后 socket 可能失效)
            if (0 > m_serverFd)
            {
                m_serverFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                if (0 > m_serverFd)
                {
                    LogError("[-] ServerWorker: socket recreate failed, %d:%s", errno, strerror(errno));
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                int optVal = 1;
                setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &optVal, sizeof(int));
                if (0 > bind(m_serverFd, reinterpret_cast<sockaddr *>(&m_transportAddress), sizeof(m_transportAddress)))
                {
                    LogError("[-] ServerWorker: rebind failed, %d:%s", errno, strerror(errno));
                    close(m_serverFd);
                    m_serverFd = -1;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                if (0 > listen(m_serverFd, 1))
                {
                    LogError("[-] ServerWorker: relisten failed, %d:%s", errno, strerror(errno));
                    close(m_serverFd);
                    m_serverFd = -1;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                LogInfo("[AImGui] ServerWorker: listen socket recreated fd=%d port=%d",
                        m_serverFd, ntohs(m_transportAddress.sin_port));
            }

            m_clientFd = accept(m_serverFd, nullptr, nullptr);
            if (0 > m_clientFd)
            {
                if (m_state)
                {
                    LogDebug("[-] Server accept failed, %d:%s, will retry", errno, strerror(errno));
                    // 关闭失效的 listen socket，下次循环重建
                    close(m_serverFd);
                    m_serverFd = -1;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
                return;
            }

            if (m_options.tcpNoDelay)
            {
                int optionValue = 1;
                setsockopt(m_clientFd, IPPROTO_TCP, TCP_NODELAY, &optionValue, sizeof(optionValue));
            }

            {
                std::lock_guard<std::mutex> lock(m_renderDataMutex);
                m_serverFontData.clear();
                m_serverRenderData.clear();
                m_serverRenderDataBack.clear();
                m_renderState = RenderState::ReadData;
                m_fontPacketCount = 0;
                m_renderPacketCount = 0;
                m_renderFrameCount = 0;
                m_lastFontPacketSize = 0;
                m_lastRenderPacketSize = 0;
                m_lastRenderDecodedSize = 0;
                m_serverFontPacketReceived = false;
            }
            // 新连接: 清空菜单 rect 列表 (旧 client 残留的菜单不应继续吸收触摸).
            {
                std::lock_guard<std::mutex> lk(m_menuRectsMutex);
                m_menuRects.clear();
            }
            m_menuRectsCount.store(0, std::memory_order_release);
            LogInfo("[AImGui] Server accepted client fd=%d", m_clientFd);
            m_clientConnected.store(true, std::memory_order_release);

            uint32_t packetSize = 0;
            while (m_state)
            {
                if (static_cast<int>(sizeof(packetSize)) > ReadData(&packetSize, sizeof(packetSize)))
                {
                    if (m_state)
                        LogDebug("[-] Server can not read packet size, %d:%s", errno, strerror(errno));
                    break;
                }
                // 旁路 packet (菜单 rect 通知) — 在尺寸校验之前先处理, 因为哨兵值
                // 远超 m_maxPacketSize, 否则会被误判为非法 packet.
                if (kSidePacketMagic_MenuRect == packetSize)
                {
                    uint32_t count = 0;
                    if (static_cast<int>(sizeof(count)) > ReadData(&count, sizeof(count)))
                    {
                        LogDebug("[-] Side packet count read failed, %d:%s", errno, strerror(errno));
                        break;
                    }
                    if (count > AImGui::kMaxMenuRects)
                    {
                        LogDebug("[-] Side packet count too large: %u", count);
                        break;
                    }
                    float payload[AImGui::kMaxMenuRects * 4];
                    const size_t payloadBytes = static_cast<size_t>(count) * 4 * sizeof(float);
                    if (payloadBytes > 0 &&
                        static_cast<int>(payloadBytes) > ReadData(payload, payloadBytes))
                    {
                        LogDebug("[-] Side packet payload read failed, %d:%s", errno, strerror(errno));
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lk(m_menuRectsMutex);
                        m_menuRects.assign(payload, payload + count * 4);
                    }
                    m_menuRectsCount.store(static_cast<int>(count), std::memory_order_release);
                    continue;
                }
                if (packetSize > m_maxPacketSize)
                {
                    LogDebug("[-] Packet is too large: %2.f", packetSize / 1024.f / 1024.f);
                    break;
                }

                if (m_serverRenderDataBack.size() < packetSize)
                    m_serverRenderDataBack.resize(packetSize);
                auto readResult = ReadData(m_serverRenderDataBack.data(), packetSize);
                if (0 >= readResult)
                {
                    if (m_state)
                        LogDebug("[-] Client disconnect or read failed, readResult:%d  %d:%s", readResult, errno, strerror(errno));
                    break;
                }

                if (m_options.exchangeFontData && !m_serverFontPacketReceived) // NOTE: First packet is font data
                {
                    // 字体包必须在 ReadData 状态处理
                    if (RenderState::ReadData != m_renderState.load(std::memory_order_acquire))
                        continue;
                    std::lock_guard<std::mutex> lock(m_renderDataMutex);
                    m_serverFontData.swap(m_serverRenderDataBack);
                    m_serverFontPacketReceived = true;
                    m_fontPacketCount++;
                    m_lastFontPacketSize = packetSize;
                    LogInfo("[AImGui] Server received font packet size=%u", packetSize);
                    m_renderState.store(RenderState::SetFont, std::memory_order_release);
                }
                else
                {
                    // 渲染数据始终保存最新帧, 不再丢弃
                    std::lock_guard<std::mutex> lock(m_renderDataMutex);
                    if (!m_options.compressionFrameData)
                        m_serverRenderData.swap(m_serverRenderDataBack);
                    else
                    {
                        static std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decompressContext(ZSTD_createDCtx(), &ZSTD_freeDCtx);

                        if (nullptr == decompressContext)
                        {
                            LogDebug("[-] Server can not create decompress context");
                            m_state = false;
                            break;
                        }

                        uint32_t sharedDataSize = 0;
                        memcpy(&sharedDataSize, m_serverRenderDataBack.data(), sizeof(sharedDataSize));
                        if (m_serverRenderData.size() < sharedDataSize)
                            m_serverRenderData.resize(sharedDataSize);

                        ZSTD_inBuffer input{m_serverRenderDataBack.data() + sizeof(sharedDataSize), packetSize - sizeof(sharedDataSize), 0};
                        ZSTD_outBuffer output{m_serverRenderData.data(), m_serverRenderData.size(), 0};
                        if (0 != ZSTD_decompressStream(decompressContext.get(), &output, &input))
                            LogDebug("[-] Server decompression frame data error");

                        m_lastRenderDecodedSize = output.pos;
                    }
                    m_renderPacketCount++;
                    m_lastRenderPacketSize = packetSize;
                    if (0 == (m_renderPacketCount % 180) || 1 == m_renderPacketCount)
                    {
                        LogInfo("[AImGui] Server received render packet count=%llu packet=%zu decoded=%zu",
                                static_cast<unsigned long long>(m_renderPacketCount),
                                m_lastRenderPacketSize,
                                m_lastRenderDecodedSize);
                    }
                    m_renderState.store(RenderState::Rendering, std::memory_order_release);
                }
            }

            int clientFd = m_clientFd;
            m_clientFd = -1;
            if (-1 != clientFd)
            {
                shutdown(clientFd, SHUT_RDWR);
                close(clientFd);
            }

            // 客户端断开: 记录时间, 保留最后一帧数据, 由渲染线程在宽限期后清屏
            m_clientDisconnectTime = std::chrono::steady_clock::now();
            m_clientConnected.store(false, std::memory_order_release);            m_renderState.store(RenderState::ReadData, std::memory_order_release);

            // 客户端断开: 清空菜单 rect, 让 Java 侧立即移除残留的捕获窗
            // (避免下一个 client 还没连上前用户仍能在旧菜单位置吃到触摸).
            {
                std::lock_guard<std::mutex> lk(m_menuRectsMutex);
                m_menuRects.clear();
            }
            m_menuRectsCount.store(0, std::memory_order_release);

            if (m_state)
                LogInfo("[AImGui] Server client disconnected, waiting next client");
        }
    }

    int AImGui::ReadData(void *buffer, size_t readSize)
    {
        size_t packetReaded = 0;
        pollfd pfd{
            .fd = m_clientFd,
            .events = POLLIN,
        };

        while (packetReaded < readSize)
        {
            // Wait up to 60s. Client 端调用前已经用 PollInputReady(0) 检查过 POLLIN,
            // 不会真的等 60s; Server 端这边等下一个 packet, 1s 太短 — client 第一帧
            // BeginFrame/EndFrame 需要 ~1.4s 完成 (ImGui 第一帧字体 atlas 上传等),
            // 1s timeout 会让 server 在 client 写第一个 frame packet 前就 break+close,
            // 引发 client 后续 WriteData broken pipe, 菜单永远绘不出来。
            auto pollResult = poll(&pfd, 1, 60000);
            if (0 >= pollResult)
                return pollResult;

            auto readResult = read(m_clientFd, reinterpret_cast<char *>(buffer) + packetReaded, readSize - packetReaded);
            if (0 >= readResult)
                return readResult;
            packetReaded += readResult;
        }

        return static_cast<int>(packetReaded);
    }
    void AImGui::WriteData(void *data, size_t size)
    {
        if (RenderType::RenderServer == m_options.renderType && -1 == m_clientFd)
            return;

        size_t totalWritten = 0;
        while (totalWritten < size)
        {
            auto result = write(m_clientFd, reinterpret_cast<char *>(data) + totalWritten, size - totalWritten);
            if (0 >= result)
            {
                LogDebug("[-] WriteData failed, result:%zd  %d:%s", result, errno, strerror(errno));
                if (RenderType::RenderServer == m_options.renderType)
                {
                    int clientFd = m_clientFd;
                    m_clientFd = -1;
                    if (-1 != clientFd)
                    {
                        shutdown(clientFd, SHUT_RDWR);
                        close(clientFd);
                    }
                    return;
                }
                m_state = false;
                return;
            }
            totalWritten += result;
        }
    }
} // namespace android