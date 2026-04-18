#ifndef UE5_HEADER_H
#define UE5_HEADER_H

#include <cstdint>
#include <string>

namespace ue5dfm {

class UE5DfmDumper;  // 前向声明

/**
 * @brief UE5 IDA 头文件/脚本生成器 (DFM 版)
 *
 * 基于 UE5DfmDumper 已解析的 NamePool / GUObjectArray 数据,
 * 生成 IDA 可用的 .h 头文件和 script.json 脚本文件,
 * 格式模仿 ueGamepubgmhd/libUE4Header 的输出。
 *
 * 注意: UE5 反射结构与 UE4 不同:
 *   - Properties 存放于 UStruct::ChildProperties (FField 链表, Next=+0x18)
 *   - Functions / Enums 存放于 UStruct::Children (UField 链表, Next=+0x28)
 *   - FProperty 使用 FField 基类, 类名通过 FFieldClass 读取
 */
class UE5Header {
public:
    /**
     * @param dumper       已初始化的 UE5DfmDumper 实例
     * @param outputDir    输出目录 (如 "/data/data/.../cache/ue5_dump/")
     */
    UE5Header(UE5DfmDumper& dumper, const std::string& outputDir);

    /// 一键生成所有输出文件
    void start();

    /// 生成 IDA 头文件 (.h)
    void saveToIdaHeader(const std::string& path);

    /// 生成 IDA 脚本 JSON (.json)
    void saveToScriptJson(const std::string& path);

    static std::string cleanIdentifier(const std::string& name);

private:
    UE5DfmDumper& m_dumper;
    std::string m_outputDir;

    static std::string escapeJsonString(const std::string& s);
};

} // namespace ue5dfm

#endif // UE5_HEADER_H
