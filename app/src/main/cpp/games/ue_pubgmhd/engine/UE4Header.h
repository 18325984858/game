#ifndef UE4_HEADER_H
#define UE4_HEADER_H

#include <cstdint>
#include <string>

namespace ue4 {

class UE4Dumper;  // 前向声明

/**
 * @brief UE4 IDA 头文件/脚本生成器
 *
 * 基于 UE4Dumper 已解析的 GNames/GUObjectArray 数据,
 * 生成 IDA 可用的 .h 头文件和 script.json 脚本文件,
 * 格式模仿 il2cppHeader 的输出。
 */
class UE4Header {
public:
    /**
     * @param dumper       已初始化的 UE4Dumper 实例
     * @param outputDir    输出目录 (如 "/data/data/.../cache/ue4_dump/")
     */
    UE4Header(UE4Dumper& dumper, const std::string& outputDir);

    /// 一键生成所有输出文件
    void start();

    /// 生成 IDA 头文件 (.h)
    void saveToIdaHeader(const std::string& path);

    /// 生成 IDA 脚本 JSON (.json)
    void saveToScriptJson(const std::string& path);

private:
    UE4Dumper& m_dumper;
    std::string m_outputDir;

    static std::string cleanIdentifier(const std::string& name);
    static std::string escapeJsonString(const std::string& s);
};

} // namespace ue4

#endif // UE4_HEADER_H
