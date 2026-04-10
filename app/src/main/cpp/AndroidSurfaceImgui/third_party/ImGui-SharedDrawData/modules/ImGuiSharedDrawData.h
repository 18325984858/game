#ifndef IMGUI_SHARED_DRAWDATA_H // !IMGUI_SHARED_DRAWDATA_H
#define IMGUI_SHARED_DRAWDATA_H

#include <imgui/imgui.h>

#include <stdint.h>
#include <string.h>

#include <string>
#include <vector>
#include <unordered_set>

namespace ImGui
{
    inline std::vector<uint8_t> GetSharedFontData()
    {
        auto &imguiIO = ImGui::GetIO();

        std::vector<uint8_t> sharedFontData;
        uint8_t *pixelData = nullptr;
        int width = 0, height = 0;

        auto WriteData = [&](void *data, size_t size)
        {
            auto begin = reinterpret_cast<uint8_t *>(data);
            auto end = begin + size;
            sharedFontData.insert(sharedFontData.end(), begin, end);
        };

        imguiIO.Fonts->GetTexDataAsAlpha8(&pixelData, &width, &height);
        if (1 > width || 1 > height)
            return sharedFontData;

        // Write width
        WriteData(&width, sizeof(width));
        // Write height
        WriteData(&height, sizeof(height));
        // Write data
        WriteData(pixelData, width * height);

        return sharedFontData;
    }

    inline void SetSharedFontData(const std::vector<uint8_t> &data)
    {
        auto &imguiIO = ImGui::GetIO();

        // Need at least width(4) + height(4) + 1 byte of pixel data
        if (data.size() < sizeof(int) * 2 + 1)
            return;
        if (!imguiIO.Fonts->IsBuilt())
        {
            if (!imguiIO.Fonts->Build())
                return;
        }

        size_t readIndex = 0;
        int originDataSize = imguiIO.Fonts->TexWidth * imguiIO.Fonts->TexHeight;

        // Read width
        int newWidth = 0, newHeight = 0;
        memcpy(&newWidth, data.data() + readIndex, sizeof(newWidth));
        readIndex += sizeof(newWidth);
        // Read height
        memcpy(&newHeight, data.data() + readIndex, sizeof(newHeight));
        readIndex += sizeof(newHeight);

        // Validate dimensions
        if (newWidth <= 0 || newHeight <= 0)
            return;
        // Check for integer overflow
        if (newWidth > 32768 || newHeight > 32768)
            return;
        int newDataSize = newWidth * newHeight;
        // Check remaining data is sufficient
        if (data.size() - readIndex < static_cast<size_t>(newDataSize))
            return;

        imguiIO.Fonts->TexWidth = newWidth;
        imguiIO.Fonts->TexHeight = newHeight;

        // Read data
        if (originDataSize < newDataSize)
        {
            IM_FREE(imguiIO.Fonts->TexPixelsAlpha8);
            imguiIO.Fonts->TexPixelsAlpha8 = reinterpret_cast<uint8_t *>(IM_ALLOC(newDataSize));
        }
        memcpy(imguiIO.Fonts->TexPixelsAlpha8, data.data() + readIndex, newDataSize);

        if (nullptr != imguiIO.Fonts->TexPixelsRGBA32)
        {
            IM_FREE(imguiIO.Fonts->TexPixelsRGBA32);
            imguiIO.Fonts->TexPixelsRGBA32 = nullptr;
        }
    }

    inline const std::vector<uint8_t> &GetSharedDrawData()
    {
        static std::vector<uint8_t> sharedDrawData;
        auto WriteData = [](void *data, size_t size)
        {
            auto begin = reinterpret_cast<uint8_t *>(data);
            auto end = begin + size;
            sharedDrawData.insert(sharedDrawData.end(), begin, end);
        };

        auto drawData = ImGui::GetDrawData();

        sharedDrawData.clear();
        // Write cmd list count
        WriteData(&drawData->CmdListsCount, sizeof(drawData->CmdListsCount));
        // Write display pos
        WriteData(&drawData->DisplayPos, sizeof(drawData->DisplayPos));
        // Write frame buffer scale
        WriteData(&drawData->FramebufferScale, sizeof(drawData->FramebufferScale));

        for (const auto &cmdList : drawData->CmdLists)
        {
            // Vertex buffer
            {
                // Write vertexes count
                WriteData(&cmdList->VtxBuffer.Size, sizeof(cmdList->VtxBuffer.Size));
                // Write vertexes
                WriteData(cmdList->VtxBuffer.Data, cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
            }

            // Index buffer
            {
                // Write indexes count
                WriteData(&cmdList->IdxBuffer.Size, sizeof(cmdList->IdxBuffer.Size));
                // Write indexes
                WriteData(cmdList->IdxBuffer.Data, cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));
            }

            // Cmd buffer
            {
                // Write cmds count
                WriteData(&cmdList->CmdBuffer.Size, sizeof(cmdList->CmdBuffer.Size));
                // Write cmds — record offset before WriteData to avoid dangling pointer after realloc
                size_t cmdsOffset = sharedDrawData.size();
                WriteData(cmdList->CmdBuffer.Data, cmdList->CmdBuffer.Size * sizeof(ImDrawCmd));
                auto sharedCmds = reinterpret_cast<ImDrawCmd *>(sharedDrawData.data() + cmdsOffset);
                for (int i = 0; i < cmdList->CmdBuffer.Size; ++i)
                {
                    sharedCmds[i].UserCallback = nullptr;
                    sharedCmds[i].UserCallbackData = nullptr;
                }
            }
        }

        return sharedDrawData;
    }

    inline ImDrawData *RenderSharedDrawData(std::vector<uint8_t> &data)
    {
        size_t readIndex = 0;
        const size_t dataSize = data.size();

        if (data.empty())
            return ImGui::GetDrawData();

        // Bounds check helper
        auto CanRead = [&](size_t bytes) -> bool
        {
            return readIndex + bytes <= dataSize;
        };

        // Read cmd lists count
        if (!CanRead(sizeof(int)))
            return ImGui::GetDrawData();
        int cmdListsCount = 0;
        memcpy(&cmdListsCount, data.data() + readIndex, sizeof(cmdListsCount));
        readIndex += sizeof(cmdListsCount);
        if (1 > cmdListsCount)
            return ImGui::GetDrawData();

        // Prepare draw data
        static bool showWindow = true;
        auto drawData = ImGui::GetDrawData();
        // Make cmd lists
        static std::vector<std::string> windowNames;
        if (cmdListsCount > static_cast<int>(windowNames.size()))
        {
            for (int i = static_cast<int>(windowNames.size()); i < cmdListsCount; ++i)
                windowNames.emplace_back("Window" + std::to_string(i));
        }
        do
        {
            ImGui::NewFrame();
            for (int i = 0; i < cmdListsCount; ++i)
            {
                ImGui::Begin(windowNames[i].data(), &showWindow);
                ImGui::End();
            }
            ImGui::Render();

            drawData = ImGui::GetDrawData();
        } while (0 == drawData->CmdListsCount);

        // Read display pos
        if (!CanRead(sizeof(drawData->DisplayPos)))
            return drawData;
        memcpy(&drawData->DisplayPos, data.data() + readIndex, sizeof(drawData->DisplayPos));
        readIndex += sizeof(drawData->DisplayPos);
        // Read frame buffer scale
        if (!CanRead(sizeof(drawData->FramebufferScale)))
            return drawData;
        memcpy(&drawData->FramebufferScale, data.data() + readIndex, sizeof(drawData->FramebufferScale));
        readIndex += sizeof(drawData->FramebufferScale);

        int cmdListIndex = 0;
        for (auto &cmdList : drawData->CmdLists)
        {
            if (cmdListIndex >= cmdListsCount)
                break;

            // ---- Vertex buffer: copy into ImGui-owned memory ----
            {
                if (!CanRead(sizeof(int)))
                    return drawData;
                int vtxCount = 0;
                memcpy(&vtxCount, data.data() + readIndex, sizeof(vtxCount));
                readIndex += sizeof(vtxCount);
                if (vtxCount < 0 || !CanRead(static_cast<size_t>(vtxCount) * sizeof(ImDrawVert)))
                    return drawData;
                cmdList->VtxBuffer.resize(vtxCount);
                memcpy(cmdList->VtxBuffer.Data, data.data() + readIndex, static_cast<size_t>(vtxCount) * sizeof(ImDrawVert));
                readIndex += static_cast<size_t>(vtxCount) * sizeof(ImDrawVert);
            }

            // ---- Index buffer: copy into ImGui-owned memory ----
            {
                if (!CanRead(sizeof(int)))
                    return drawData;
                int idxCount = 0;
                memcpy(&idxCount, data.data() + readIndex, sizeof(idxCount));
                readIndex += sizeof(idxCount);
                if (idxCount < 0 || !CanRead(static_cast<size_t>(idxCount) * sizeof(ImDrawIdx)))
                    return drawData;
                cmdList->IdxBuffer.resize(idxCount);
                memcpy(cmdList->IdxBuffer.Data, data.data() + readIndex, static_cast<size_t>(idxCount) * sizeof(ImDrawIdx));
                readIndex += static_cast<size_t>(idxCount) * sizeof(ImDrawIdx);
            }

            // ---- Cmd buffer: copy into ImGui-owned memory ----
            {
                if (!CanRead(sizeof(int)))
                    return drawData;
                int cmdCount = 0;
                memcpy(&cmdCount, data.data() + readIndex, sizeof(cmdCount));
                readIndex += sizeof(cmdCount);
                if (cmdCount < 0 || !CanRead(static_cast<size_t>(cmdCount) * sizeof(ImDrawCmd)))
                    return drawData;
                cmdList->CmdBuffer.resize(cmdCount);
                memcpy(cmdList->CmdBuffer.Data, data.data() + readIndex, static_cast<size_t>(cmdCount) * sizeof(ImDrawCmd));
                readIndex += static_cast<size_t>(cmdCount) * sizeof(ImDrawCmd);
            }

            ++cmdListIndex;
        }

        return drawData;
    }
}

#endif //! IMGUI_SHARED_DRAWDATA_H