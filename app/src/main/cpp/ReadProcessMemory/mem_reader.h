/**
 * @file    mem_reader.h
 * @brief   任意进程内存读取 / 搜索 (通过 su + dd 读取 /proc/PID/mem)
 *          - 自动去除 aarch64 MTE/TBI 指针 tag
 *          - 持久化 root shell 减少每次 popen 开销
 *          - 提供地址归属查询 / 大块分段 / 字节模式搜索
 */
#ifndef MEM_READER_H
#define MEM_READER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MemReader {

    struct RegionInfo {
        uintptr_t baseAddr = 0;
        uintptr_t endAddr  = 0;
        std::string name;    // "[heap]" / "libUE4.so" / "<anon>"
        std::string path;    // 完整路径或 "[heap]"
        std::string perms;   // "rw-p"
    };

    /**
     * 从目标进程读取一段内存 (支持 MTE/TBI tag, 自动按页对齐, 大块自动分批)
     * @return 实际读取字节数；<=0 表示失败
     */
    ssize_t readMemory(int pid, uintptr_t address, size_t size,
                       std::vector<uint8_t>& out);

    /**
     * 查找地址所在的 /proc/PID/maps 区间
     */
    bool findRegion(int pid, uintptr_t address, RegionInfo& info);

    /**
     * 读取 /proc/PID/maps 的完整文本 (通过持久 root shell, 无需每次 popen).
     * @return true 表示成功；out 里会是原始的 maps 内容 (包含末尾换行)
     */
    bool readMaps(int pid, std::string& out);

    /**
     * 在指定地址范围内搜索字节模式
     * @param pattern 模式字节
     * @param mask    与 pattern 同长度的掩码 (0xFF 精确 / 0x00 通配)
     * @param maxHits 命中上限
     */
    std::vector<uintptr_t> searchPattern(int pid,
                                         uintptr_t rangeStart,
                                         uintptr_t rangeEnd,
                                         const std::vector<uint8_t>& pattern,
                                         const std::vector<uint8_t>& mask,
                                         size_t maxHits);

    /**
     * 向目标进程写入一段内存 (通过 root + base64 + dd)
     * 会自动 untag 地址.
     * @return 实际写入字节数；<=0 表示失败
     */
    ssize_t writeMemory(int pid, uintptr_t address,
                        const std::vector<uint8_t>& data);

    /**
     * 在持久化 root shell 中执行任意命令 (内部已序列化).
     * 提供给同模块复用, 避免到处 system("su -c ...")/popen("su -c ...")
     * 拉新的 su 子进程产生检测痕迹.
     */
    bool runRootShell(const std::string& cmd);

    /**
     * 在持久化 root shell 中执行命令, 把 stdout 写到内部随机 tmp 文件后读回.
     * 取代 popen("su -c ...") 模式.
     */
    bool runRootShellCapture(const std::string& cmd, std::string& out);

    /**
     * 查询 root shell 是否曾经因 su 不响应被判定为"未授权".
     * UI 可借此在失败提示里写明真实原因 (而不是含糊的"读取失败").
     */
    bool isRootAuthDenied();

} // namespace MemReader

#endif // MEM_READER_H
