#ifndef SO_DUMPER_H
#define SO_DUMPER_H

#include <string>
#include <vector>

namespace SoDumper {

    /**
     * 进程信息
     */
    struct ProcessInfo {
        int pid;
        std::string packageName;
    };

    /**
     * SO 模块信息 (来自 /proc/pid/maps)
     */
    struct ModuleInfo {
        std::string name;       // so 文件名
        std::string path;       // 完整路径
        uintptr_t baseAddr;     // 内存起始地址
        uintptr_t endAddr;      // 内存结束地址
        size_t size;            // 映射总大小 (合并所有段)
    };

    /**
     * 枚举所有正在运行的应用进程
     * @param filter 模糊搜索的包名关键词 (空字符串返回全部)
     * @return 进程列表
     */
    std::vector<ProcessInfo> listRunningApps(const std::string& filter);

    /**
     * 枚举指定进程加载的所有 .so 模块
     * @param pid 进程 PID
     * @return 模块列表 (已去重, 合并连续映射段)
     */
    std::vector<ModuleInfo> listModules(int pid);

    /**
     * 从目标进程内存 dump 指定 SO 并修复 ELF
     * @param pid       目标进程 PID
     * @param module    要 dump 的模块信息
     * @param outPath   输出文件路径
     * @return 0 成功, 负数失败
     */
    int dumpAndFixSo(int pid, const ModuleInfo& module, const std::string& outPath);

} // namespace SoDumper

#endif // SO_DUMPER_H
