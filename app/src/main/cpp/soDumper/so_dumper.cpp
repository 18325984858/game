/**
 * @file    so_dumper.cpp
 * @brief   从运行中进程 dump SO 文件并修复 ELF 头
 *          需要 root 权限 (所有 /proc 操作通过 su 执行)
 */
#include "so_dumper.h"
#include "../Log/log.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <elf.h>
#include <algorithm>
#include <set>
#include <map>
#include <vector>
#include <string>
#include <errno.h>

#define DTAG "[SoDumper]"

namespace SoDumper {

// ─── 工具函数 ───────────────────────────────────────────────────────

/**
 * 判断是否是应用进程 (包名格式: 含 .)
 */
static bool isAppProcess(const std::string& cmdline) {
    if (cmdline.empty()) return false;
    if (cmdline[0] == '/') return false;
    if (cmdline.find('.') == std::string::npos) return false;
    return true;
}

/**
 * 模糊匹配 (忽略大小写)
 */
static bool fuzzyMatch(const std::string& packageName, const std::string& filter) {
    if (filter.empty()) return true;
    std::string lower_pkg = packageName;
    std::string lower_filter = filter;
    std::transform(lower_pkg.begin(), lower_pkg.end(), lower_pkg.begin(), ::tolower);
    std::transform(lower_filter.begin(), lower_filter.end(), lower_filter.begin(), ::tolower);
    return lower_pkg.find(lower_filter) != std::string::npos;
}

// ─── 进程枚举 (通过 su 以 root 身份读取所有进程) ────────────────────

std::vector<ProcessInfo> listRunningApps(const std::string& filter) {
    std::vector<ProcessInfo> result;
    std::set<std::string> seen;

    // Android 非 root 进程只能看到自己的 /proc 条目
    // 使用 su 执行 ps -A 获取所有进程
    FILE* fp = popen("su -c 'ps -A 2>/dev/null || ps -e 2>/dev/null || ps 2>/dev/null'", "r");
    if (!fp) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "popen(su ps) 失败");
        return result;
    }

    char line[1024];
    // 跳过 header 行
    if (!fgets(line, sizeof(line), fp)) {
        pclose(fp);
        return result;
    }

    while (fgets(line, sizeof(line), fp)) {
        // ps 输出格式: USER PID PPID VSZ RSS WCHAN ADDR S NAME
        // PID 始终为第2列, NAME 始终为最后一列
        std::vector<const char*> tokens;
        char* tok = strtok(line, " \t\n\r");
        while (tok) {
            tokens.push_back(tok);
            tok = strtok(nullptr, " \t\n\r");
        }

        if (tokens.size() < 2) continue;

        int pid = atoi(tokens[1]);        // 第2列 = PID
        const char* name = tokens.back(); // 最后一列 = NAME

        if (pid <= 0) continue;

        std::string cmdline(name);
        if (!isAppProcess(cmdline)) continue;

        // 去掉子进程后缀 (com.example.app:service → com.example.app)
        std::string baseName = cmdline;
        size_t colonPos = baseName.find(':');
        if (colonPos != std::string::npos) {
            baseName = baseName.substr(0, colonPos);
        }

        if (!fuzzyMatch(baseName, filter)) continue;
        if (seen.count(baseName)) continue;
        seen.insert(baseName);

        ProcessInfo info;
        info.pid = pid;
        info.packageName = baseName;
        result.push_back(info);
    }
    pclose(fp);

    std::sort(result.begin(), result.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  return a.packageName < b.packageName;
              });

    __android_log_print(ANDROID_LOG_INFO, DTAG, "找到 %zu 个匹配进程 (filter='%s')",
                        result.size(), filter.c_str());
    return result;
}

// ─── 模块枚举 (通过 su 读取目标进程 maps) ───────────────────────────

std::vector<ModuleInfo> listModules(int pid) {
    std::vector<ModuleInfo> result;

    // 使用 su 读取其他进程的 maps (非 root 无权限)
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "su -c 'cat /proc/%d/maps 2>/dev/null'", pid);
    FILE* fp = popen(cmd, "r");
    if (!fp) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "popen(su cat maps) 失败, pid=%d", pid);
        return result;
    }

    // 收集所有 .so 映射, 按路径合并
    std::map<std::string, ModuleInfo> moduleMap;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        // 格式: 7a1000-7a2000 r-xp 00000000 fd:00 12345  /path/to/lib.so
        uintptr_t start, end;
        char perms[8], path[512];
        unsigned long offset, dev1, dev2, inode;
        path[0] = '\0';

        int matched = sscanf(line, "%lx-%lx %4s %lx %lx:%lx %lu %511s",
                             &start, &end, perms, &offset, &dev1, &dev2, &inode, path);
        if (matched < 7) continue;
        if (path[0] == '\0') continue;

        std::string pathStr(path);
        // 只保留 .so 文件
        if (pathStr.find(".so") == std::string::npos) continue;
        // 过滤掉 [vdso] 等特殊映射
        if (pathStr[0] != '/') continue;

        if (moduleMap.find(pathStr) == moduleMap.end()) {
            ModuleInfo mod;
            mod.path = pathStr;
            // 提取文件名
            size_t slashPos = pathStr.rfind('/');
            mod.name = (slashPos != std::string::npos) ? pathStr.substr(slashPos + 1) : pathStr;
            mod.baseAddr = start;
            mod.endAddr = end;
            mod.size = end - start;
            moduleMap[pathStr] = mod;
        } else {
            auto& mod = moduleMap[pathStr];
            if (start < mod.baseAddr) mod.baseAddr = start;
            if (end > mod.endAddr) mod.endAddr = end;
            mod.size = mod.endAddr - mod.baseAddr;
        }
    }
    pclose(fp);

    for (auto& kv : moduleMap) {
        result.push_back(kv.second);
    }

    // 按名称排序
    std::sort(result.begin(), result.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  return a.name < b.name;
              });

    __android_log_print(ANDROID_LOG_INFO, DTAG, "进程 %d 加载了 %zu 个 SO 模块", pid, result.size());
    return result;
}

// ─── ELF 修复 ───────────────────────────────────────────────────────

/**
 * 修复 ELF 头部:
 * 1. 修复 section header (运行时通常被清除, 直接置零)
 * 2. 修复 program header 偏移
 * 3. 修复 PT_LOAD 段的文件偏移与大小使之与内存布局一致
 */
static bool fixElf64(uint8_t* data, size_t dataSize) {
    if (dataSize < sizeof(Elf64_Ehdr)) return false;

    auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(data);

    // 验证 ELF magic
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "不是有效的 ELF 文件");
        return false;
    }

    // section header 在内存 dump 中通常无效, 清零
    ehdr->e_shoff = 0;
    ehdr->e_shnum = 0;
    ehdr->e_shstrndx = 0;
    // e_shentsize 保留

    // 修复 program headers
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        __android_log_print(ANDROID_LOG_WARN, DTAG, "ELF 无 program header, 跳过修复");
        return true;
    }

    if (ehdr->e_phoff + ehdr->e_phnum * ehdr->e_phentsize > dataSize) {
        __android_log_print(ANDROID_LOG_WARN, DTAG, "program header 越界, 跳过修复");
        return true;
    }

    // 修复 PT_LOAD 段: 使 p_offset 与 p_vaddr 对齐
    for (int i = 0; i < ehdr->e_phnum; i++) {
        auto* phdr = reinterpret_cast<Elf64_Phdr*>(data + ehdr->e_phoff + i * ehdr->e_phentsize);

        if (phdr->p_type == PT_LOAD) {
            // 内存 dump 中 p_offset 应该等于 p_vaddr (相对于基址)
            phdr->p_offset = phdr->p_vaddr;
            // p_filesz 设为与 p_memsz 相同 (dump 后数据已在文件中)
            phdr->p_filesz = phdr->p_memsz;
        }
    }

    __android_log_print(ANDROID_LOG_INFO, DTAG, "ELF64 修复完成");
    return true;
}

static bool fixElf32(uint8_t* data, size_t dataSize) {
    if (dataSize < sizeof(Elf32_Ehdr)) return false;

    auto* ehdr = reinterpret_cast<Elf32_Ehdr*>(data);

    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "不是有效的 ELF 文件");
        return false;
    }

    ehdr->e_shoff = 0;
    ehdr->e_shnum = 0;
    ehdr->e_shstrndx = 0;

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return true;
    }

    if (ehdr->e_phoff + ehdr->e_phnum * ehdr->e_phentsize > dataSize) {
        return true;
    }

    for (int i = 0; i < ehdr->e_phnum; i++) {
        auto* phdr = reinterpret_cast<Elf32_Phdr*>(data + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type == PT_LOAD) {
            phdr->p_offset = phdr->p_vaddr;
            phdr->p_filesz = phdr->p_memsz;
        }
    }

    __android_log_print(ANDROID_LOG_INFO, DTAG, "ELF32 修复完成");
    return true;
}

// ─── Dump + 修复 ────────────────────────────────────────────────────

int dumpAndFixSo(int pid, const ModuleInfo& module, const std::string& outPath) {
    __android_log_print(ANDROID_LOG_INFO, DTAG,
                        "开始 dump: pid=%d, module=%s, base=0x%lx, size=%zu",
                        pid, module.name.c_str(), (unsigned long)module.baseAddr, module.size);

    size_t totalSize = module.size;
    if (totalSize == 0 || totalSize > 512 * 1024 * 1024) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "模块大小异常: %zu", totalSize);
        return -2;
    }

    // SO 模块基地址一定是页对齐的 (mmap)
    const size_t PAGE_SIZE = 4096;
    size_t skipPages = module.baseAddr / PAGE_SIZE;
    size_t countPages = (totalSize + PAGE_SIZE - 1) / PAGE_SIZE;

    std::string rawPath = outPath + ".raw";

    // 使用 su + dd 从 /proc/pid/mem 读取内存
    // conv=noerror,sync: 遇到不可读页继续读取, 并用 0 填充
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "su -c 'mkdir -p /data/local/tmp/so_dump && "
        "chmod 777 /data/local/tmp/so_dump && "
        "dd if=/proc/%d/mem of=%s bs=4096 skip=%zu count=%zu conv=noerror,sync 2>/dev/null && "
        "chmod 666 %s'",
        pid, rawPath.c_str(), skipPages, countPages, rawPath.c_str());

    __android_log_print(ANDROID_LOG_INFO, DTAG, "执行: %s", cmd);
    int sysRet = system(cmd);
    if (sysRet != 0) {
        __android_log_print(ANDROID_LOG_WARN, DTAG, "dd 返回 %d, 尝试继续...", sysRet);
    }

    // 读取 dd 输出的 raw 文件
    FILE* rawFp = fopen(rawPath.c_str(), "rb");
    if (!rawFp) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "无法打开 raw 文件: %s", rawPath.c_str());
        return -4;
    }

    fseek(rawFp, 0, SEEK_END);
    size_t fileSize = (size_t)ftell(rawFp);
    fseek(rawFp, 0, SEEK_SET);

    // 取实际模块大小 (dd 可能多读了一页)
    size_t actualSize = std::min(fileSize, totalSize);
    if (actualSize == 0) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "raw 文件为空");
        fclose(rawFp);
        return -4;
    }

    uint8_t* buffer = new(std::nothrow) uint8_t[actualSize];
    if (!buffer) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "内存分配失败: %zu bytes", actualSize);
        fclose(rawFp);
        return -3;
    }

    size_t bytesRead = fread(buffer, 1, actualSize, rawFp);
    fclose(rawFp);

    __android_log_print(ANDROID_LOG_INFO, DTAG, "读取 raw 文件: %zu / %zu bytes", bytesRead, actualSize);

    if (bytesRead == 0) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "未读取到任何数据");
        delete[] buffer;
        return -4;
    }

    // 修复 ELF
    if (actualSize >= EI_NIDENT) {
        uint8_t elfClass = buffer[EI_CLASS];
        if (elfClass == ELFCLASS64) {
            fixElf64(buffer, actualSize);
        } else if (elfClass == ELFCLASS32) {
            fixElf32(buffer, actualSize);
        } else {
            __android_log_print(ANDROID_LOG_WARN, DTAG, "未知 ELF class: %d, 不进行修复", elfClass);
        }
    }

    // 写入修复后的文件
    FILE* outFp = fopen(outPath.c_str(), "wb");
    if (!outFp) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "无法创建输出文件: %s", outPath.c_str());
        delete[] buffer;
        return -5;
    }

    size_t written = fwrite(buffer, 1, actualSize, outFp);
    fclose(outFp);
    delete[] buffer;

    // 清理 raw 临时文件, 修复输出文件权限
    snprintf(cmd, sizeof(cmd),
        "su -c 'rm -f %s && chmod 644 %s && chmod 755 /data/local/tmp/so_dump'",
        rawPath.c_str(), outPath.c_str());
    system(cmd);

    if (written != actualSize) {
        __android_log_print(ANDROID_LOG_ERROR, DTAG, "写入不完整: %zu / %zu", written, actualSize);
        return -6;
    }

    __android_log_print(ANDROID_LOG_INFO, DTAG,
                        "Dump 成功: %s -> %s (%zu bytes)", module.name.c_str(), outPath.c_str(), actualSize);
    return 0;
}

} // namespace SoDumper
