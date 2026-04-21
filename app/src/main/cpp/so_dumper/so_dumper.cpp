/**
 * @file    so_dumper.cpp
 * @brief   从运行中进程 dump SO 文件并修复 ELF 头
 *          - 需要 root 权限
 *          - 所有 /proc/<pid>/mem 与 /proc/<pid>/maps 读取统一经 MemReader
 *            (持久化 root shell + dd), 不再自己 popen("su -c dd ...");
 *          - listRunningApps 仍用 popen("su -c ps") 做一次性进程枚举.
 */
#include "so_dumper.h"
#include "../core/log/log.h"
#include "../memory/mem_reader.h"

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

// ELF 常量 (部分 NDK 版本可能缺失)
#ifndef DT_GNU_HASH
#define DT_GNU_HASH      0x6ffffef5
#endif
#ifndef DT_VERSYM
#define DT_VERSYM        0x6ffffff0
#endif
#ifndef DT_VERNEED
#define DT_VERNEED       0x6ffffffe
#endif
#ifndef DT_VERNEEDNUM
#define DT_VERNEEDNUM    0x6fffffff
#endif
#ifndef DT_VERDEF
#define DT_VERDEF        0x6ffffffc
#endif
#ifndef SHT_GNU_HASH
#define SHT_GNU_HASH     0x6ffffff6
#endif
#ifndef SHT_GNU_VERSYM
#define SHT_GNU_VERSYM   0x6fffffff
#endif
#ifndef SHT_GNU_verneed
#define SHT_GNU_verneed  0x6ffffffe
#endif

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

    // 不再 popen("su -c ps -A"): 复用 MemReader 已启动的持久 root shell
    // (避免反复用拉 su 子进程, 减少游戏反作弊对
    // "同设备出现多个 su 调用者" 的打分).
    std::string psOut;
    if (!MemReader::runRootShellCapture(
            "ps -A 2>/dev/null || ps -e 2>/dev/null || ps 2>/dev/null",
            psOut)) {
        LOG(LOG_LEVEL_ERROR, DTAG " runRootShellCapture(ps) 失败");
        return result;
    }

    char line[1024];
    bool firstLine = true;
    size_t linePos = 0;
    while (linePos < psOut.size()) {
        size_t nl = psOut.find('\n', linePos);
        std::string lineStr = psOut.substr(linePos,
            (nl == std::string::npos ? psOut.size() : nl) - linePos);
        linePos = (nl == std::string::npos) ? psOut.size() : nl + 1;
        if (lineStr.empty()) continue;
        if (firstLine) { firstLine = false; continue; }
        strncpy(line, lineStr.c_str(), sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';

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

        // 同包名可能存在多个子进程, 不能简单折叠成 baseName, 否则容易选错 PID。
        std::string baseName = cmdline;
        size_t colonPos = baseName.find(':');
        if (colonPos != std::string::npos) {
            baseName = baseName.substr(0, colonPos);
        }

        if (!fuzzyMatch(cmdline, filter) && !fuzzyMatch(baseName, filter)) continue;

        std::string uniqueKey = std::to_string(pid) + ":" + cmdline;
        if (seen.count(uniqueKey)) continue;
        seen.insert(uniqueKey);

        ProcessInfo info;
        info.pid = pid;
        info.packageName = cmdline;
        result.push_back(info);
    }

    std::sort(result.begin(), result.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  return a.packageName < b.packageName;
              });

    LOG(LOG_LEVEL_INFO, DTAG " 找到 %zu 个匹配进程 (filter='%s')",
                        result.size(), filter.c_str());
    return result;
}

// ─── 模块枚举 (通过 MemReader 读取目标进程 /proc/<pid>/maps) ─────────
//
// 统计规则:
//   - 只收录路径以 '/' 开头、且包含 ".so" 的映射 (排除 [vdso] 等匿名段)
//   - 同一 .so 可能被拆成 r-x / r-- / rw- 等多段; 这里按 path 归并,
//     baseAddr = 最小 start, endAddr = 最大 end, size = 各段字节之和
//     (不是 end-base 的虚拟跨度, 跨度里可能含 gap)
std::vector<ModuleInfo> listModules(int pid) {
    std::vector<ModuleInfo> result;

    // 通过 MemReader 的持久 root shell 读取 /proc/<pid>/maps,
    // 避免每次 popen(su) 的昂贵开销, 也与 mem_reader 共用一条 root 通道.
    std::string mapsText;
    if (!MemReader::readMaps(pid, mapsText)) {
        LOG(LOG_LEVEL_ERROR, DTAG " MemReader::readMaps 失败, pid=%d", pid);
        return result;
    }

    // 收集所有候选原生模块映射。
    // 注意: 新版应用可能把多个原生库直接从 base.apk 映射出来, 如果仅按 path 合并,
    // 会把不同库错误折叠成一个条目, 导致用户根本搜不到真实目标。
    struct ModuleAcc {
        ModuleInfo info;
        size_t mappedBytes; // 实际映射字节 (各区域之和)
    };
    std::map<std::string, ModuleAcc> moduleMap;

    // 按行扫描 mapsText
    size_t linePos = 0;
    while (linePos < mapsText.size()) {
        size_t nl = mapsText.find('\n', linePos);
        std::string line = mapsText.substr(linePos,
            (nl == std::string::npos ? mapsText.size() : nl) - linePos);
        linePos = (nl == std::string::npos) ? mapsText.size() : nl + 1;
        if (line.empty()) continue;

        // 格式: 7a1000-7a2000 r-xp 00000000 fd:00 12345  /path/to/lib.so
        uintptr_t start, end;
        char perms[8], path[512];
        unsigned long offset, dev1, dev2, inode;
        path[0] = '\0';

        int matched = sscanf(line.c_str(), "%lx-%lx %4s %lx %lx:%lx %lu %511s",
                             &start, &end, perms, &offset, &dev1, &dev2, &inode, path);
        if (matched < 7) continue;
        if (path[0] == '\0') continue;

        std::string pathStr(path);

        // 新版游戏可能把主库放到 APK / memfd / anon / ashmem 里, 不能只认传统 .so 路径。
        const bool isExec = strchr(perms, 'x') != nullptr;
        const bool looksLikeSo = pathStr.find(".so") != std::string::npos;
        const bool looksLikeApk = pathStr.find(".apk") != std::string::npos;
        const bool looksLikeHiddenLib =
                pathStr.find("memfd:") != std::string::npos ||
                pathStr.find("[anon:") != std::string::npos ||
                pathStr.find("/dev/ashmem") != std::string::npos;
        const bool looksLikeUeLib =
                pathStr.find("UE4") != std::string::npos ||
                pathStr.find("Unreal") != std::string::npos ||
                pathStr.find("libUE4") != std::string::npos ||
                pathStr.find("libUnreal") != std::string::npos;
        const bool fileBackedExec = !pathStr.empty() && pathStr[0] == '/' && isExec;

        if (!(looksLikeSo || looksLikeApk || looksLikeHiddenLib || looksLikeUeLib || fileBackedExec)) continue;

        char offBuf[32];
        snprintf(offBuf, sizeof(offBuf), "@0x%lx", offset);

        std::string displayName;
        size_t slashPos = pathStr.rfind('/');
        displayName = (slashPos != std::string::npos) ? pathStr.substr(slashPos + 1) : pathStr;
        if (displayName.empty()) displayName = pathStr;
        if (!looksLikeSo) {
            displayName += offBuf;
        }
        if (looksLikeHiddenLib && displayName.find("[hidden]") == std::string::npos) {
            displayName = std::string("[hidden] ") + displayName;
        }

        // 合并策略:
        //   - 真正的 .so 文件 (路径以 .so 结尾或含 .so.): 按 path 合并为一条,
        //     这样 r-xp / r--p / rw-p 多段会归为同一个模块, 用户不再看到一堆重复
        //     条目, 也保证传给 dumpAndFixSo 的 baseAddr 是 ELF 头所在段 (r-xp).
        //   - .apk / memfd / anon: 仍按 path|perms|offset 区分, 因为 base.apk
        //     可能直接映射出多个不同的原生库.
        auto isRealSoPath = [](const std::string& p) -> bool {
            if (p.size() >= 3 && p.compare(p.size() - 3, 3, ".so") == 0) return true;
            if (p.find(".so.") != std::string::npos) return true; // libfoo.so.1.2
            return false;
        };
        const bool mergeByPath = isRealSoPath(pathStr);
        std::string moduleKey = mergeByPath
                ? pathStr
                : (pathStr + "|" + perms + "|" + std::to_string(offset));

        if (moduleMap.find(moduleKey) == moduleMap.end()) {
            ModuleAcc acc;
            acc.info.path = pathStr;
            acc.info.name = displayName;
            acc.info.perms = perms;
            acc.info.baseAddr = start;
            acc.info.endAddr = end;
            acc.info.fileOffset = offset;
            acc.mappedBytes = end - start;
            moduleMap[moduleKey] = acc;
        } else {
            auto& acc = moduleMap[moduleKey];
            // 优先把 r-xp 段 (含 ELF 头) 当作主映射记录的 base/offset,
            // 否则保留地址最低的段.
            const bool incomingIsExec = (strchr(perms, 'x') != nullptr);
            const bool currentIsExec = (acc.info.perms.find('x') != std::string::npos);
            const bool preferIncoming =
                    (incomingIsExec && !currentIsExec) ||
                    (incomingIsExec == currentIsExec && start < acc.info.baseAddr);
            if (preferIncoming) {
                acc.info.baseAddr = start;
                acc.info.fileOffset = offset;
                acc.info.perms = perms;
            }
            if (end > acc.info.endAddr) acc.info.endAddr = end;
            acc.mappedBytes += (end - start);
        }
    }

    for (auto& kv : moduleMap) {
        auto& acc = kv.second;
        // size 报告实际映射字节, 而非虚拟地址跨度
        acc.info.size = acc.mappedBytes;
        result.push_back(acc.info);
    }

    // 按名称排序
    std::sort(result.begin(), result.end(),
              [](const ModuleInfo& a, const ModuleInfo& b) {
                  return a.name < b.name;
              });

    LOG(LOG_LEVEL_INFO, DTAG " 进程 %d 加载了 %zu 个 SO 模块", pid, result.size());
    return result;
}

// ─── 进程内存读取 (统一走 MemReader, 与 mem_reader 共用持久 root shell) ──

/**
 * 从目标进程内存读取指定地址的数据到 buf.
 * 底层是 MemReader::readMemory (持久 RootShell + dd + /proc/PID/mem),
 * 自动处理 aarch64 MTE/TBI tag、页对齐和大块分段.
 * @return 实际读取的字节数, 0 表示失败
 */
static size_t readProcessMemory(int pid, uintptr_t addr, size_t size, uint8_t* buf) {
    if (!buf || size == 0) return 0;
    std::vector<uint8_t> tmp;
    ssize_t got = MemReader::readMemory(pid, addr, size, tmp);
    if (got <= 0) return 0;
    size_t copy = std::min<size_t>((size_t)got, size);
    memcpy(buf, tmp.data(), copy);
    return copy;
}

static bool hasElfMagicAt(int pid, uintptr_t addr) {
    uint8_t magic[SELFMAG] = {0};
    size_t got = readProcessMemory(pid, addr, sizeof(magic), magic);
    return got == sizeof(magic) && memcmp(magic, ELFMAG, SELFMAG) == 0;
}

static uintptr_t resolveElfBaseFromMaps(int pid, const ModuleInfo& module) {
    std::vector<uintptr_t> candidates;
    auto addCandidate = [&](uintptr_t addr) {
        if (addr == 0) return;
        for (uintptr_t existing : candidates) {
            if (existing == addr) return;
        }
        candidates.push_back(addr);
    };

    // 优先尝试当前选中映射及其由 offset 回推的地址。
    addCandidate(module.baseAddr);
    if (module.fileOffset <= module.baseAddr) {
        addCandidate(module.baseAddr - module.fileOffset);
    }

    std::string mapsText;
    if (MemReader::readMaps(pid, mapsText)) {
        size_t linePos = 0;
        while (linePos < mapsText.size()) {
            size_t nl = mapsText.find('\n', linePos);
            std::string line = mapsText.substr(linePos,
                (nl == std::string::npos ? mapsText.size() : nl) - linePos);
            linePos = (nl == std::string::npos) ? mapsText.size() : nl + 1;
            if (line.empty()) continue;

            uintptr_t start = 0, end = 0;
            char perms[8] = {0}, path[512] = {0};
            unsigned long offset = 0, dev1 = 0, dev2 = 0, inode = 0;
            int matched = sscanf(line.c_str(), "%lx-%lx %4s %lx %lx:%lx %lu %511s",
                                 &start, &end, perms, &offset, &dev1, &dev2, &inode, path);
            if (matched < 7) continue;

            std::string pathStr(path);
            bool sameModule = (!module.path.empty() && pathStr == module.path) ||
                              (!module.name.empty() && pathStr.find(module.name) != std::string::npos);
            if (!sameModule) continue;

            // 对应同一库的兄弟映射: 直接起始地址和 start-offset 两种都试。
            addCandidate(start);
            if (offset <= start) {
                addCandidate(start - offset);
            }
        }
    }

    for (uintptr_t candidate : candidates) {
        if (hasElfMagicAt(pid, candidate)) {
            LOG(LOG_LEVEL_INFO, DTAG " ELF 头定位成功: 0x%lx", (unsigned long)candidate);
            return candidate;
        }
    }

    LOG(LOG_LEVEL_WARN, DTAG " 无法从 maps/offset 自动定位 ELF 头, candidates=%zu",
                        candidates.size());
    return 0;
}

// ─── Dump + 修复 (PT_LOAD 段感知) ──────────────────────────────────

int dumpAndFixSo(int pid, const ModuleInfo& module, const std::string& outPath,
                 std::string* actualOutPath) {
    LOG(LOG_LEVEL_INFO, DTAG " 开始 dump: pid=%d, module=%s, map_base=0x%lx, end=0x%lx, off=0x%lx, perms=%s, size=%zu",
                        pid, module.name.c_str(),
                        (unsigned long)module.baseAddr, (unsigned long)module.endAddr,
                        (unsigned long)module.fileOffset,
                        module.perms.c_str(),
                        module.size);

    uintptr_t baseAddr = resolveElfBaseFromMaps(pid, module);
    if (baseAddr == 0) {
        LOG(LOG_LEVEL_ERROR, DTAG " 无法定位有效 ELF 基址: map_base=0x%lx off=0x%lx path=%s",
                            (unsigned long)module.baseAddr,
                            (unsigned long)module.fileOffset,
                            module.path.c_str());
        return -2;
    }

    LOG(LOG_LEVEL_INFO, DTAG " 最终使用 ELF 基址: 0x%lx",
                        (unsigned long)baseAddr);

    // 临时目录由 MemReader 初始化时保证 (/data/local/tmp/so_dump)

    // ── Step 1: 读取首页, 获取 ELF 头 ──
    const size_t PAGE_SIZE = 4096;
    const size_t HEADER_PAGES = 4;  // 读 4 页 (16KB) 确保覆盖 phdr 表
    size_t headerReadSize = PAGE_SIZE * HEADER_PAGES;
    uint8_t* headerBuf = new(std::nothrow) uint8_t[headerReadSize];
    if (!headerBuf) return -3;
    memset(headerBuf, 0, headerReadSize);

    size_t headerGot = readProcessMemory(pid, baseAddr, headerReadSize, headerBuf);
    if (headerGot < sizeof(Elf64_Ehdr)) {
        LOG(LOG_LEVEL_ERROR, DTAG " 读取 ELF 头失败, got=%zu", headerGot);
        delete[] headerBuf;
        return -4;
    }

    // 验证 ELF magic
    if (memcmp(headerBuf, ELFMAG, SELFMAG) != 0) {
        LOG(LOG_LEVEL_ERROR, DTAG " 无效 ELF magic");
        delete[] headerBuf;
        return -2;
    }

    bool is64 = (headerBuf[EI_CLASS] == ELFCLASS64);
    LOG(LOG_LEVEL_INFO, DTAG " ELF class: %s", is64 ? "64-bit" : "32-bit");

    // ── Step 2: 解析 PT_LOAD 段 ──
    struct LoadSeg {
        uintptr_t vaddr;   // p_vaddr (相对于 ELF 基址 0)
        size_t offset;     // p_offset (原始文件偏移)
        size_t memsz;      // p_memsz
    };
    std::vector<LoadSeg> loadSegs;
    size_t outFileSize = 0;

    if (is64) {
        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(headerBuf);
        size_t phdrEnd = ehdr->e_phoff + (size_t)ehdr->e_phnum * ehdr->e_phentsize;
        if (phdrEnd > headerGot) {
            // phdr 表超出已读范围, 需要额外读取
            LOG(LOG_LEVEL_WARN, DTAG " phdr 表偏移 %zu 超出首次读取范围 %zu, 扩展读取",
                phdrEnd, headerGot);
            uint8_t* newBuf = new(std::nothrow) uint8_t[phdrEnd];
            if (!newBuf) { delete[] headerBuf; return -3; }
            memcpy(newBuf, headerBuf, headerGot);
            delete[] headerBuf;
            headerBuf = newBuf;
            headerReadSize = phdrEnd;
            // 读取剩余部分
            if (phdrEnd > headerGot) {
                size_t extra = readProcessMemory(pid, baseAddr + headerGot, phdrEnd - headerGot,
                                                 headerBuf + headerGot);
                headerGot += extra;
            }
            ehdr = reinterpret_cast<Elf64_Ehdr*>(headerBuf);
        }

        for (int i = 0; i < ehdr->e_phnum; i++) {
            auto* phdr = reinterpret_cast<Elf64_Phdr*>(
                headerBuf + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (phdr->p_type == PT_LOAD) {
                LoadSeg seg;
                seg.vaddr = phdr->p_vaddr;
                seg.offset = phdr->p_offset;
                seg.memsz = phdr->p_memsz;
                loadSegs.push_back(seg);
                // 使用 vaddr 布局输出文件 (file_offset == vaddr),
                // 这样所有 d_ptr / sh_addr 在文件中的偏移与内存中一致, IDA/section
                // 重建可以直接用 vaddr 当 file offset, 不需要再做 offset/vaddr 换算.
                size_t segEnd = seg.vaddr + seg.memsz;
                if (segEnd > outFileSize) outFileSize = segEnd;
                LOG(LOG_LEVEL_INFO, DTAG " PT_LOAD[%d]: vaddr=0x%lx offset=0x%lx memsz=0x%lx",
                    i, (unsigned long)seg.vaddr, (unsigned long)seg.offset,
                    (unsigned long)seg.memsz);
            }
        }
    } else {
        auto* ehdr = reinterpret_cast<Elf32_Ehdr*>(headerBuf);
        size_t phdrEnd = ehdr->e_phoff + (size_t)ehdr->e_phnum * ehdr->e_phentsize;
        if (phdrEnd > headerGot) {
            uint8_t* newBuf = new(std::nothrow) uint8_t[phdrEnd];
            if (!newBuf) { delete[] headerBuf; return -3; }
            memcpy(newBuf, headerBuf, headerGot);
            delete[] headerBuf;
            headerBuf = newBuf;
            headerReadSize = phdrEnd;
            if (phdrEnd > headerGot) {
                size_t extra = readProcessMemory(pid, baseAddr + headerGot, phdrEnd - headerGot,
                                                 headerBuf + headerGot);
                headerGot += extra;
            }
            ehdr = reinterpret_cast<Elf32_Ehdr*>(headerBuf);
        }

        for (int i = 0; i < ehdr->e_phnum; i++) {
            auto* phdr = reinterpret_cast<Elf32_Phdr*>(
                headerBuf + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (phdr->p_type == PT_LOAD) {
                LoadSeg seg;
                seg.vaddr = phdr->p_vaddr;
                seg.offset = phdr->p_offset;
                seg.memsz = phdr->p_memsz;
                loadSegs.push_back(seg);
                // vaddr 布局输出文件 (与 64-bit 分支保持一致)
                size_t segEnd = seg.vaddr + seg.memsz;
                if (segEnd > outFileSize) outFileSize = segEnd;
            }
        }
    }

    if (loadSegs.empty()) {
        LOG(LOG_LEVEL_ERROR, DTAG " 未找到 PT_LOAD 段");
        delete[] headerBuf;
        return -2;
    }

    LOG(LOG_LEVEL_INFO, DTAG " 找到 %zu 个 PT_LOAD 段, 输出文件大小: %zu bytes (%.2f MB)",
        loadSegs.size(), outFileSize, outFileSize / (1024.0 * 1024.0));

    // 输出文件大小检查 (基于 vaddr 布局, 段间真实间隙会零填充)
    if (outFileSize == 0 || outFileSize > ((size_t)2u << 30)) {
        LOG(LOG_LEVEL_ERROR, DTAG " 输出文件大小异常: %zu", outFileSize);
        delete[] headerBuf;
        return -2;
    }

    // ── Step 3: 分配输出缓冲区 (零初始化) ──
    uint8_t* outBuf = new(std::nothrow) uint8_t[outFileSize]();
    if (!outBuf) {
        LOG(LOG_LEVEL_ERROR, DTAG " 分配 %zu 字节失败", outFileSize);
        delete[] headerBuf;
        return -3;
    }

    // 先把 header 页写入 (含 ELF 头 + program headers)
    size_t hdrCopy = std::min(headerReadSize, outFileSize);
    memcpy(outBuf, headerBuf, hdrCopy);
    delete[] headerBuf;
    headerBuf = nullptr;

    // ── Step 4: 逐段 dump PT_LOAD 数据 (统一走 MemReader, 内部会按 1MB 分段) ──
    for (size_t si = 0; si < loadSegs.size(); si++) {
        auto& seg = loadSegs[si];
        uintptr_t memAddr = baseAddr + seg.vaddr;
        size_t readSize = seg.memsz;

        if (seg.vaddr + readSize > outFileSize) {
            readSize = outFileSize - seg.vaddr;
        }
        if (readSize == 0) continue;

        LOG(LOG_LEVEL_INFO, DTAG " Dump PT_LOAD[%zu]: mem=0x%lx -> file_off=0x%lx, size=%zu",
            si, (unsigned long)memAddr, (unsigned long)seg.vaddr, readSize);

        // 写入 vaddr 偏移 (= 文件偏移, 与 outFileSize 计算保持一致)
        size_t got = readProcessMemory(pid, memAddr, readSize, outBuf + seg.vaddr);
        LOG(LOG_LEVEL_INFO, DTAG " PT_LOAD[%zu]: 读取 %zu / %zu bytes", si, got, readSize);
    }

    // ── Step 5: IDA 兼容 ELF 修复 (重建 section headers) ──
    //
    // IDA 依赖 section headers 来定位 .dynsym / .dynstr / .plt 等表。
    // 内存 dump 后 section headers 已丢失, 这里从 PT_DYNAMIC 重建。

    // --- 5a: 修复 PT_LOAD 的 p_filesz / p_offset ---
    // 由于我们按 vaddr 布局输出文件, 必须把 p_offset 重写成 p_vaddr,
    // 否则 IDA 按 p_offset 读取段数据会读到错误位置。
    if (is64) {
        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(outBuf);
        for (int i = 0; i < ehdr->e_phnum; i++) {
            auto* phdr = reinterpret_cast<Elf64_Phdr*>(
                outBuf + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (phdr->p_type == PT_LOAD) {
                phdr->p_offset = phdr->p_vaddr;
                phdr->p_filesz = phdr->p_memsz;
            } else if (phdr->p_type == PT_DYNAMIC ||
                       phdr->p_type == PT_NOTE ||
                       phdr->p_type == PT_PHDR ||
                       phdr->p_type == PT_GNU_RELRO ||
                       phdr->p_type == PT_GNU_EH_FRAME) {
                // 其他需要寻址的段也同步成 vaddr 布局
                phdr->p_offset = phdr->p_vaddr;
                if (phdr->p_filesz > phdr->p_memsz) phdr->p_memsz = phdr->p_filesz;
            }
        }
    } else {
        auto* ehdr = reinterpret_cast<Elf32_Ehdr*>(outBuf);
        for (int i = 0; i < ehdr->e_phnum; i++) {
            auto* phdr = reinterpret_cast<Elf32_Phdr*>(
                outBuf + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (phdr->p_type == PT_LOAD) {
                phdr->p_offset = phdr->p_vaddr;
                phdr->p_filesz = phdr->p_memsz;
            } else if (phdr->p_type == PT_DYNAMIC ||
                       phdr->p_type == PT_NOTE ||
                       phdr->p_type == PT_PHDR ||
                       phdr->p_type == PT_GNU_RELRO ||
                       phdr->p_type == PT_GNU_EH_FRAME) {
                phdr->p_offset = phdr->p_vaddr;
                if (phdr->p_filesz > phdr->p_memsz) phdr->p_memsz = phdr->p_filesz;
            }
        }
    }

    // --- 5b: 查找 PT_DYNAMIC 的文件偏移和大小 ---
    size_t dynOff = 0, dynSize = 0;
    if (is64) {
        auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(outBuf);
        for (int i = 0; i < ehdr->e_phnum; i++) {
            auto* ph = reinterpret_cast<Elf64_Phdr*>(
                outBuf + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (ph->p_type == PT_DYNAMIC) {
                dynOff = ph->p_vaddr;   // 使用 vaddr (内存布局 = 文件偏移)
                dynSize = ph->p_memsz;
                break;
            }
        }
    } else {
        auto* ehdr = reinterpret_cast<Elf32_Ehdr*>(outBuf);
        for (int i = 0; i < ehdr->e_phnum; i++) {
            auto* ph = reinterpret_cast<Elf32_Phdr*>(
                outBuf + ehdr->e_phoff + i * ehdr->e_phentsize);
            if (ph->p_type == PT_DYNAMIC) {
                dynOff = ph->p_vaddr;
                dynSize = ph->p_memsz;
                break;
            }
        }
    }

    if (dynOff == 0 || dynOff + dynSize > outFileSize) {
        LOG(LOG_LEVEL_WARN, DTAG " PT_DYNAMIC 未找到或越界 (off=0x%lx sz=0x%lx), 跳过 section 重建",
            (unsigned long)dynOff, (unsigned long)dynSize);
        // 直接清零 section header 并跳过
        if (is64) {
            auto* e = reinterpret_cast<Elf64_Ehdr*>(outBuf);
            e->e_shoff = 0; e->e_shnum = 0; e->e_shstrndx = 0;
        } else {
            auto* e = reinterpret_cast<Elf32_Ehdr*>(outBuf);
            e->e_shoff = 0; e->e_shnum = 0; e->e_shstrndx = 0;
        }
        goto write_output;
    }

    {
        // --- 5c: 解析 dynamic entries, 收集各表地址和大小 ---
        // 所有 d_ptr 值在内存 dump 中可能是绝对地址 (baseAddr + vaddr),
        // 需要检测并减去 baseAddr 得到文件偏移。
        struct DynInfo {
            size_t dt_strtab    = 0;  // DT_STRTAB   -> .dynstr
            size_t dt_strsz     = 0;  // DT_STRSZ
            size_t dt_symtab    = 0;  // DT_SYMTAB   -> .dynsym
            size_t dt_syment    = 0;  // DT_SYMENT   (16 or 24)
            size_t dt_hash      = 0;  // DT_HASH     -> .hash
            size_t dt_gnu_hash  = 0;  // DT_GNU_HASH -> .gnu.hash
            size_t dt_rela      = 0;  // DT_RELA     -> .rela.dyn
            size_t dt_relasz    = 0;
            size_t dt_relaent   = 0;
            size_t dt_rel       = 0;  // DT_REL      -> .rel.dyn (32-bit)
            size_t dt_relsz     = 0;
            size_t dt_relent    = 0;
            size_t dt_jmprel    = 0;  // DT_JMPREL   -> .rela.plt / .rel.plt
            size_t dt_pltrelsz  = 0;
            size_t dt_pltrel    = 0;  // DT_PLTREL (DT_RELA=7 or DT_REL=17)
            size_t dt_pltgot    = 0;  // DT_PLTGOT   -> .got.plt
            size_t dt_init_arr  = 0;  // DT_INIT_ARRAY
            size_t dt_init_arrsz= 0;
            size_t dt_fini_arr  = 0;  // DT_FINI_ARRAY
            size_t dt_fini_arrsz= 0;
            size_t dt_versym    = 0;  // DT_VERSYM
            size_t dt_verneed   = 0;  // DT_VERNEED
        };
        DynInfo di;

        // 检测 linker 是否将 d_ptr 转换为绝对地址 (load_bias + vaddr).
        // 策略: 计算映像 vaddr 上限 maxVaddr, 仅当 DT_STRTAB 落在
        // [baseAddr, baseAddr + maxVaddr + slack] 范围内才判定为绝对地址,
        // 避免把大 vaddr (如 0x17900A20) 误判成绝对地址.
        bool absoluteAddrs = false;
        size_t maxVaddr = 0;
        for (auto& s : loadSegs) {
            size_t e = s.vaddr + s.memsz;
            if (e > maxVaddr) maxVaddr = e;
        }

        auto normAddr = [&](size_t val) -> size_t {
            if (absoluteAddrs && val >= baseAddr) return val - baseAddr;
            return val;
        };

        // 第一遍: 先读 DT_STRTAB 判断是否绝对地址
        size_t strtabRaw = 0;
        if (is64) {
            auto* dyn = reinterpret_cast<Elf64_Dyn*>(outBuf + dynOff);
            size_t count = dynSize / sizeof(Elf64_Dyn);
            for (size_t i = 0; i < count && dyn[i].d_tag != DT_NULL; i++) {
                if (dyn[i].d_tag == DT_STRTAB) {
                    strtabRaw = (size_t)dyn[i].d_un.d_ptr;
                    break;
                }
            }
        } else {
            auto* dyn = reinterpret_cast<Elf32_Dyn*>(outBuf + dynOff);
            size_t count = dynSize / sizeof(Elf32_Dyn);
            for (size_t i = 0; i < count && dyn[i].d_tag != DT_NULL; i++) {
                if (dyn[i].d_tag == DT_STRTAB) {
                    strtabRaw = (size_t)dyn[i].d_un.d_ptr;
                    break;
                }
            }
        }
        if (baseAddr > 0 && strtabRaw >= baseAddr &&
            strtabRaw < baseAddr + maxVaddr + 0x10000) {
            absoluteAddrs = true;
        }

        LOG(LOG_LEVEL_INFO, DTAG " Dynamic entries 地址模式: %s", absoluteAddrs ? "绝对地址" : "相对地址");

        // 第二遍: 完整解析
        if (is64) {
            auto* dyn = reinterpret_cast<Elf64_Dyn*>(outBuf + dynOff);
            size_t count = dynSize / sizeof(Elf64_Dyn);
            for (size_t i = 0; i < count && dyn[i].d_tag != DT_NULL; i++) {
                size_t v = (size_t)dyn[i].d_un.d_val;
                switch (dyn[i].d_tag) {
                    case DT_STRTAB:     di.dt_strtab    = normAddr(v); break;
                    case DT_STRSZ:      di.dt_strsz     = v; break;
                    case DT_SYMTAB:     di.dt_symtab    = normAddr(v); break;
                    case DT_SYMENT:     di.dt_syment    = v; break;
                    case DT_HASH:       di.dt_hash      = normAddr(v); break;
                    case DT_GNU_HASH:   di.dt_gnu_hash  = normAddr(v); break;
                    case DT_RELA:       di.dt_rela      = normAddr(v); break;
                    case DT_RELASZ:     di.dt_relasz    = v; break;
                    case DT_RELAENT:    di.dt_relaent   = v; break;
                    case DT_REL:        di.dt_rel       = normAddr(v); break;
                    case DT_RELSZ:      di.dt_relsz     = v; break;
                    case DT_RELENT:     di.dt_relent    = v; break;
                    case DT_JMPREL:     di.dt_jmprel    = normAddr(v); break;
                    case DT_PLTRELSZ:   di.dt_pltrelsz  = v; break;
                    case DT_PLTREL:     di.dt_pltrel    = v; break;
                    case DT_PLTGOT:     di.dt_pltgot    = normAddr(v); break;
                    case DT_INIT_ARRAY: di.dt_init_arr  = normAddr(v); break;
                    case DT_INIT_ARRAYSZ: di.dt_init_arrsz = v; break;
                    case DT_FINI_ARRAY: di.dt_fini_arr  = normAddr(v); break;
                    case DT_FINI_ARRAYSZ: di.dt_fini_arrsz = v; break;
                    case DT_VERSYM:     di.dt_versym    = normAddr(v); break;
                    case DT_VERNEED:    di.dt_verneed   = normAddr(v); break;
                }
            }
        } else {
            auto* dyn = reinterpret_cast<Elf32_Dyn*>(outBuf + dynOff);
            size_t count = dynSize / sizeof(Elf32_Dyn);
            for (size_t i = 0; i < count && dyn[i].d_tag != DT_NULL; i++) {
                size_t v = (size_t)dyn[i].d_un.d_val;
                switch (dyn[i].d_tag) {
                    case DT_STRTAB:     di.dt_strtab    = normAddr(v); break;
                    case DT_STRSZ:      di.dt_strsz     = v; break;
                    case DT_SYMTAB:     di.dt_symtab    = normAddr(v); break;
                    case DT_SYMENT:     di.dt_syment    = v; break;
                    case DT_HASH:       di.dt_hash      = normAddr(v); break;
                    case DT_GNU_HASH:   di.dt_gnu_hash  = normAddr(v); break;
                    case DT_RELA:       di.dt_rela      = normAddr(v); break;
                    case DT_RELASZ:     di.dt_relasz    = v; break;
                    case DT_RELAENT:    di.dt_relaent   = v; break;
                    case DT_REL:        di.dt_rel       = normAddr(v); break;
                    case DT_RELSZ:      di.dt_relsz     = v; break;
                    case DT_RELENT:     di.dt_relent    = v; break;
                    case DT_JMPREL:     di.dt_jmprel    = normAddr(v); break;
                    case DT_PLTRELSZ:   di.dt_pltrelsz  = v; break;
                    case DT_PLTREL:     di.dt_pltrel    = v; break;
                    case DT_PLTGOT:     di.dt_pltgot    = normAddr(v); break;
                    case DT_INIT_ARRAY: di.dt_init_arr  = normAddr(v); break;
                    case DT_INIT_ARRAYSZ: di.dt_init_arrsz = v; break;
                    case DT_FINI_ARRAY: di.dt_fini_arr  = normAddr(v); break;
                    case DT_FINI_ARRAYSZ: di.dt_fini_arrsz = v; break;
                    case DT_VERSYM:     di.dt_versym    = normAddr(v); break;
                    case DT_VERNEED:    di.dt_verneed   = normAddr(v); break;
                }
            }
        }

        LOG(LOG_LEVEL_INFO, DTAG " Dynamic: strtab=0x%lx(%zu) symtab=0x%lx hash=0x%lx gnu_hash=0x%lx",
            (unsigned long)di.dt_strtab, di.dt_strsz,
            (unsigned long)di.dt_symtab,
            (unsigned long)di.dt_hash, (unsigned long)di.dt_gnu_hash);

        // --- 5d: 推算 .dynsym 大小 ---
        // 方法: 从 DT_HASH 的 nchain 字段, 或从 DT_GNU_HASH 推算
        size_t dynsymCount = 0;
        size_t symEntSize = is64 ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
        if (di.dt_syment > 0) symEntSize = di.dt_syment;

        if (di.dt_hash && di.dt_hash + 8 <= outFileSize) {
            // DT_HASH: uint32_t nbucket, uint32_t nchain; nchain = symbol count
            uint32_t nchain = *reinterpret_cast<uint32_t*>(outBuf + di.dt_hash + 4);
            dynsymCount = nchain;
        } else if (di.dt_gnu_hash && di.dt_gnu_hash + 16 <= outFileSize) {
            // GNU hash: nbuckets, symoffset, bloom_size, bloom_shift
            // 然后 bloom[bloom_size], buckets[nbuckets], chains...
            // 需要找 bucket 中最大值, 然后从 chain 往后扫到 bit0==1
            uint32_t nbuckets   = *reinterpret_cast<uint32_t*>(outBuf + di.dt_gnu_hash + 0);
            uint32_t symoffset  = *reinterpret_cast<uint32_t*>(outBuf + di.dt_gnu_hash + 4);
            uint32_t bloom_size = *reinterpret_cast<uint32_t*>(outBuf + di.dt_gnu_hash + 8);

            size_t bloomBytes = bloom_size * (is64 ? 8 : 4);
            size_t bucketsOff = di.dt_gnu_hash + 16 + bloomBytes;
            size_t chainsOff  = bucketsOff + nbuckets * 4;

            if (bucketsOff + nbuckets * 4 <= outFileSize) {
                uint32_t* buckets = reinterpret_cast<uint32_t*>(outBuf + bucketsOff);
                uint32_t maxBucket = 0;
                for (uint32_t b = 0; b < nbuckets; b++) {
                    if (buckets[b] > maxBucket) maxBucket = buckets[b];
                }
                if (maxBucket >= symoffset) {
                    // 从 maxBucket 对应 chain 开始扫描到 bit0==1
                    size_t chainIdx = maxBucket - symoffset;
                    size_t chainOff = chainsOff + chainIdx * 4;
                    while (chainOff + 4 <= outFileSize) {
                        uint32_t entry = *reinterpret_cast<uint32_t*>(outBuf + chainOff);
                        chainIdx++;
                        chainOff += 4;
                        if (entry & 1) break; // last entry in chain
                    }
                    dynsymCount = symoffset + chainIdx;
                }
            }
        }

        if (dynsymCount == 0) {
            // 兜底 1: 从 symtab 到 strtab 估算 (要求 strtab 紧跟 symtab)
            if (di.dt_symtab && di.dt_strtab > di.dt_symtab && symEntSize > 0) {
                dynsymCount = (di.dt_strtab - di.dt_symtab) / symEntSize;
                LOG(LOG_LEVEL_INFO, DTAG " dynsym 兜底1 (strtab-symtab): count=%zu", dynsymCount);
            }
        }

        if (dynsymCount == 0) {
            // 兜底 2: 扫描所有 RELA / JMPREL 条目, 取 r_sym 最大值 + 1
            // 适用场景: ACE 等保护擦掉了 DT_HASH 内容 (nchain=0), 且没有 DT_GNU_HASH,
            //          且 .dynstr 在 .dynsym 之前无法用兜底 1.
            // 原理: 每条重定位条目都引用一个符号 index; 该 index 必 < dynsymCount,
            //       所以 max(r_sym)+1 是 dynsymCount 的可靠下界 (通常等于真实值).
            uint32_t maxSym = 0;
            auto scanRela = [&](size_t base, size_t sz, size_t entSize) {
                if (!base || !sz || !entSize) return;
                for (size_t off = base; off + entSize <= base + sz && off + entSize <= outFileSize;
                     off += entSize) {
                    uint32_t symIdx;
                    if (is64) {
                        // Elf64_Rela: r_offset(8) r_info(8) r_addend(8); r_sym = r_info >> 32
                        uint64_t r_info = *reinterpret_cast<uint64_t*>(outBuf + off + 8);
                        symIdx = (uint32_t)(r_info >> 32);
                    } else {
                        // Elf32_Rel(a): r_offset(4) r_info(4) [r_addend(4)]; r_sym = r_info >> 8
                        uint32_t r_info = *reinterpret_cast<uint32_t*>(outBuf + off + 4);
                        symIdx = r_info >> 8;
                    }
                    if (symIdx > maxSym && symIdx < 0x100000) maxSym = symIdx;
                }
            };
            size_t relaEnt = di.dt_relaent ? di.dt_relaent : (is64 ? 24 : 12);
            size_t relEnt  = di.dt_relent  ? di.dt_relent  : (is64 ? 16 : 8);
            scanRela(di.dt_rela, di.dt_relasz, relaEnt);
            scanRela(di.dt_rel,  di.dt_relsz,  relEnt);
            // .rela.plt / .rel.plt
            size_t jmpEnt = ((di.dt_pltrel == DT_RELA) || is64) ? relaEnt : relEnt;
            scanRela(di.dt_jmprel, di.dt_pltrelsz, jmpEnt);
            if (maxSym > 0) {
                dynsymCount = maxSym + 1;
                LOG(LOG_LEVEL_WARN, DTAG " dynsym 兜底2 (扫 RELA r_sym 最大值): count=%zu"
                                          " — 通常表明 DT_HASH 被反逆向工具擦零", dynsymCount);
            }
        }
        size_t dynsymSize = dynsymCount * symEntSize;

        LOG(LOG_LEVEL_INFO, DTAG " dynsym: count=%zu, entsize=%zu, total=%zu",
            dynsymCount, symEntSize, dynsymSize);

        // --- 5e: 计算 .hash 大小 ---
        size_t hashSize = 0;
        if (di.dt_hash && di.dt_hash + 8 <= outFileSize) {
            uint32_t nbucket = *reinterpret_cast<uint32_t*>(outBuf + di.dt_hash);
            uint32_t nchain  = *reinterpret_cast<uint32_t*>(outBuf + di.dt_hash + 4);
            hashSize = 8 + (nbucket + nchain) * 4;
        }

        // --- 5e2: 重建被反逆向工具擦零的 .hash 表 ---
        // bionic 自 Android 6 起优先使用 DT_GNU_HASH, DT_HASH 只是兼容字段,
        // 因此运行时几乎不读 .hash, ACE 等保护常把这块清零让 dumper 拿不到 nchain.
        // 我们用上面已得的 dynsymCount 重建一个最简但合法的 .hash:
        //   nbucket=1, nchain=dynsymCount, bucket[0]=0, chain[0..N-1]=0
        // 这样 IDA / readelf 加载时能正确解析符号数, .hash 链空也不影响静态分析.
        if (di.dt_hash && dynsymCount > 0 && hashSize <= 8) {
            size_t neededHashSize = 8 + (1 + dynsymCount) * 4;
            if (di.dt_hash + neededHashSize <= outFileSize) {
                uint32_t* h = reinterpret_cast<uint32_t*>(outBuf + di.dt_hash);
                h[0] = 1;                          // nbucket
                h[1] = (uint32_t)dynsymCount;      // nchain
                h[2] = 0;                          // bucket[0]
                for (size_t i = 0; i < dynsymCount; i++) h[3 + i] = 0;  // chain[]
                hashSize = neededHashSize;
                LOG(LOG_LEVEL_WARN, DTAG " .hash 已被运行时清零, 重建为 nbucket=1 nchain=%zu (size=0x%zx)",
                                          dynsymCount, hashSize);
            } else {
                LOG(LOG_LEVEL_WARN, DTAG " .hash 重建跳过: 重建后越界 (need=0x%zx, fileEnd=0x%zx)",
                                          neededHashSize, outFileSize - di.dt_hash);
            }
        }

        // --- 5f: 计算 .gnu.hash 大小 ---
        size_t gnuHashSize = 0;
        if (di.dt_gnu_hash && di.dt_gnu_hash + 16 <= outFileSize) {
            uint32_t nbuckets   = *reinterpret_cast<uint32_t*>(outBuf + di.dt_gnu_hash + 0);
            uint32_t symoffset  = *reinterpret_cast<uint32_t*>(outBuf + di.dt_gnu_hash + 4);
            uint32_t bloom_size = *reinterpret_cast<uint32_t*>(outBuf + di.dt_gnu_hash + 8);
            size_t bloomBytes = bloom_size * (is64 ? 8 : 4);
            size_t chainsStart = 16 + bloomBytes + nbuckets * 4;
            // chain count = dynsymCount - symoffset
            size_t chainCount = (dynsymCount > symoffset) ? (dynsymCount - symoffset) : 0;
            gnuHashSize = chainsStart + chainCount * 4;
        }

        // --- 5g: 构建 .shstrtab 字符串表 ---
        // 格式: \0name1\0name2\0...
        struct SecDef {
            const char* name;
            uint32_t type;
            uint64_t flags;
            size_t   addr;      // 文件偏移 (= vaddr for dump)
            size_t   size;
            uint32_t link;
            uint32_t info;
            size_t   addralign;
            size_t   entsize;
        };

        std::vector<SecDef> sections;
        // Section 0 is always NULL

        // Helper lambda to add section
        auto addSec = [&](const char* name, uint32_t type, uint64_t flags,
                          size_t addr, size_t sz, uint32_t link, uint32_t info,
                          size_t align, size_t entsz) {
            if (addr == 0 && sz == 0 && type != SHT_NULL) return; // skip empty
            sections.push_back({name, type, flags, addr, sz, link, info, align, entsz});
        };

        // 按地址排序后生成 section, 先收集所有有效段
        // .dynsym
        uint32_t dynsymIdx = 0, dynstrIdx = 0;

        if (di.dt_symtab && dynsymSize) {
            dynsymIdx = (uint32_t)sections.size() + 1; // +1 because NULL sec at [0]
        }
        addSec(".dynsym", SHT_DYNSYM, SHF_ALLOC,
               di.dt_symtab, dynsymSize,
               0 /* link=dynstr, 稍后修复 */, 1 /* info=first global */,
               is64 ? 8 : 4, symEntSize);

        if (di.dt_strtab && di.dt_strsz) {
            dynstrIdx = (uint32_t)sections.size() + 1;
        }
        addSec(".dynstr", SHT_STRTAB, SHF_ALLOC,
               di.dt_strtab, di.dt_strsz, 0, 0, 1, 0);

        addSec(".hash", SHT_HASH, SHF_ALLOC,
               di.dt_hash, hashSize, 0, 0, is64 ? 8 : 4, 4);

        addSec(".gnu.hash", SHT_GNU_HASH, SHF_ALLOC,
               di.dt_gnu_hash, gnuHashSize, 0, 0, is64 ? 8 : 4, 0);

        // .rela.dyn / .rel.dyn
        if (is64 || di.dt_rela) {
            addSec(".rela.dyn", SHT_RELA, SHF_ALLOC,
                   di.dt_rela, di.dt_relasz, 0, 0, 8,
                   di.dt_relaent ? di.dt_relaent : (is64 ? 24 : 12));
        }
        if (!is64 && di.dt_rel) {
            addSec(".rel.dyn", SHT_REL, SHF_ALLOC,
                   di.dt_rel, di.dt_relsz, 0, 0, 4,
                   di.dt_relent ? di.dt_relent : 8);
        }

        // .rela.plt / .rel.plt
        if (di.dt_jmprel && di.dt_pltrelsz) {
            bool isPltRela = (di.dt_pltrel == DT_RELA) || is64;
            addSec(isPltRela ? ".rela.plt" : ".rel.plt",
                   isPltRela ? SHT_RELA : SHT_REL, SHF_ALLOC | SHF_INFO_LINK,
                   di.dt_jmprel, di.dt_pltrelsz, 0, 0, 8,
                   isPltRela ? (is64 ? 24 : 12) : (is64 ? 16 : 8));
        }

        // .dynamic
        addSec(".dynamic", SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE,
               dynOff, dynSize, 0, 0, is64 ? 8 : 4,
               is64 ? sizeof(Elf64_Dyn) : sizeof(Elf32_Dyn));

        // .init_array
        addSec(".init_array", SHT_INIT_ARRAY, SHF_ALLOC | SHF_WRITE,
               di.dt_init_arr, di.dt_init_arrsz, 0, 0, is64 ? 8 : 4, is64 ? 8 : 4);

        // .fini_array
        addSec(".fini_array", SHT_FINI_ARRAY, SHF_ALLOC | SHF_WRITE,
               di.dt_fini_arr, di.dt_fini_arrsz, 0, 0, is64 ? 8 : 4, is64 ? 8 : 4);

        // .got.plt (大小未知, 尝试从 pltgot 到下一已知段的间隙估算)
        if (di.dt_pltgot) {
            // 粗略估算: GOT 至少包含 3 个保留条目 + PLT reloc count 个条目
            size_t ptrSize = is64 ? 8 : 4;
            size_t pltRelocCount = 0;
            if (di.dt_pltrelsz && di.dt_pltrel) {
                size_t relaEnt = (di.dt_pltrel == DT_RELA) ? (is64 ? 24 : 12) : (is64 ? 16 : 8);
                if (relaEnt > 0) pltRelocCount = di.dt_pltrelsz / relaEnt;
            }
            size_t gotPltSize = (3 + pltRelocCount) * ptrSize;
            addSec(".got.plt", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
                   di.dt_pltgot, gotPltSize, 0, 0, ptrSize, ptrSize);
        }

        // .gnu.version (DT_VERSYM)
        if (di.dt_versym && dynsymCount) {
            addSec(".gnu.version", SHT_GNU_VERSYM, SHF_ALLOC,
                   di.dt_versym, dynsymCount * 2, 0, 0, 2, 2);
        }

        // .gnu.version_r (DT_VERNEED) - 遍历 Verneed 链表算出真实字节数
        if (di.dt_verneed) {
            size_t verneedSize = 0;
            size_t cur = di.dt_verneed;
            // 安全上限: 防止恶意/损坏数据导致死循环
            for (int guard = 0; guard < 4096 && cur + 16 <= outFileSize; guard++) {
                // Elf{32,64}_Verneed 前 16 字节布局相同:
                //   uint16_t vn_version, vn_cnt;
                //   uint32_t vn_file, vn_aux, vn_next;
                uint16_t vn_cnt  = *reinterpret_cast<uint16_t*>(outBuf + cur + 2);
                uint32_t vn_aux  = *reinterpret_cast<uint32_t*>(outBuf + cur + 8);
                uint32_t vn_next = *reinterpret_cast<uint32_t*>(outBuf + cur + 12);

                size_t entrySize = 16; // Verneed 自身
                // 累加 vn_cnt 个 Vernaux (每个 16 字节)
                size_t auxCur = cur + vn_aux;
                for (uint16_t a = 0; a < vn_cnt && auxCur + 16 <= outFileSize; a++) {
                    uint32_t vna_next = *reinterpret_cast<uint32_t*>(outBuf + auxCur + 12);
                    entrySize = (auxCur + 16) - cur;
                    if (vna_next == 0) break;
                    auxCur += vna_next;
                }
                verneedSize += entrySize;
                if (vn_next == 0) break;
                cur += vn_next;
            }
            if (verneedSize == 0) verneedSize = 64; // 兜底
            addSec(".gnu.version_r", SHT_GNU_verneed, SHF_ALLOC,
                   di.dt_verneed, verneedSize, 0, 0, is64 ? 8 : 4, 0);
        }

        // 过滤掉 addr=0 && size=0 的无效 section (已在 addSec 中跳过)
        // 过滤掉越界的 section
        std::vector<SecDef> validSections;
        for (auto& s : sections) {
            if (s.addr + s.size <= outFileSize) {
                validSections.push_back(s);
            } else {
                LOG(LOG_LEVEL_WARN, DTAG " Section '%s' 越界 (off=0x%lx, sz=0x%lx), 跳过",
                    s.name, (unsigned long)s.addr, (unsigned long)s.size);
            }
        }
        sections = std::move(validSections);

        LOG(LOG_LEVEL_INFO, DTAG " 重建 %zu 个 section headers", sections.size());

        // 构建 .shstrtab
        std::vector<uint8_t> shstrtab;
        shstrtab.push_back(0); // 首字节 = \0
        std::vector<uint32_t> nameOffsets;
        for (auto& s : sections) {
            nameOffsets.push_back((uint32_t)shstrtab.size());
            shstrtab.insert(shstrtab.end(), s.name, s.name + strlen(s.name) + 1);
        }
        // .shstrtab 自身的名字
        uint32_t shstrtabNameOff = (uint32_t)shstrtab.size();
        const char* shstrtabName = ".shstrtab";
        shstrtab.insert(shstrtab.end(), shstrtabName, shstrtabName + strlen(shstrtabName) + 1);

        // --- 5h: 修正 link 字段 ---
        // 重新找 dynsymIdx 和 dynstrIdx 在最终数组中的位置 (+1 因为 NULL section 在 [0])
        dynsymIdx = 0;
        dynstrIdx = 0;
        for (size_t i = 0; i < sections.size(); i++) {
            if (strcmp(sections[i].name, ".dynsym") == 0) dynsymIdx = (uint32_t)(i + 1);
            if (strcmp(sections[i].name, ".dynstr") == 0) dynstrIdx = (uint32_t)(i + 1);
        }
        // 设置 link: .dynsym -> .dynstr, .hash/.gnu.hash -> .dynsym,
        // .rela.* -> .dynsym, .dynamic -> .dynstr
        for (auto& s : sections) {
            if (s.type == SHT_DYNSYM)       s.link = dynstrIdx;
            if (s.type == SHT_HASH)         s.link = dynsymIdx;
            if (s.type == SHT_GNU_HASH)     s.link = dynsymIdx;
            if (s.type == SHT_RELA)         s.link = dynsymIdx;
            if (s.type == SHT_REL)          s.link = dynsymIdx;
            if (s.type == SHT_DYNAMIC)      s.link = dynstrIdx;
            if (s.type == SHT_GNU_VERSYM)   s.link = dynsymIdx;
            if (s.type == SHT_GNU_verneed)  s.link = dynstrIdx;
        }

        // --- 5i: 追加 .shstrtab 数据 + section headers 到输出缓冲区 ---
        // 总 section 数 = 1 (NULL) + sections.size() + 1 (.shstrtab)
        size_t totalSections = 1 + sections.size() + 1;
        size_t shstrtabIdx = totalSections - 1; // .shstrtab 是最后一个
        size_t shentSize = is64 ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr);

        // 对齐到 8 字节
        size_t shstrtabOff = (outFileSize + 7) & ~(size_t)7;
        size_t shtOff = (shstrtabOff + shstrtab.size() + 7) & ~(size_t)7;
        size_t totalAppend = (shtOff - outFileSize) + totalSections * shentSize;
        size_t newFileSize = shtOff + totalSections * shentSize;

        // 重新分配输出缓冲区
        uint8_t* newBuf = new(std::nothrow) uint8_t[newFileSize]();
        if (!newBuf) {
            LOG(LOG_LEVEL_ERROR, DTAG " 追加 section header 内存分配失败");
            goto write_output;
        }
        memcpy(newBuf, outBuf, outFileSize);
        delete[] outBuf;
        outBuf = newBuf;

        // 写入 .shstrtab 数据
        memcpy(outBuf + shstrtabOff, shstrtab.data(), shstrtab.size());

        // 写入 section headers
        if (is64) {
            auto* shdrs = reinterpret_cast<Elf64_Shdr*>(outBuf + shtOff);
            // [0] NULL section
            memset(&shdrs[0], 0, sizeof(Elf64_Shdr));

            // [1..N] 重建的 sections
            for (size_t i = 0; i < sections.size(); i++) {
                auto& s = sections[i];
                auto& sh = shdrs[i + 1];
                memset(&sh, 0, sizeof(Elf64_Shdr));
                sh.sh_name      = nameOffsets[i];
                sh.sh_type      = s.type;
                sh.sh_flags     = s.flags;
                sh.sh_addr      = s.addr;     // vaddr = file offset for dump
                sh.sh_offset    = s.addr;     // file offset
                sh.sh_size      = s.size;
                sh.sh_link      = s.link;
                sh.sh_info      = s.info;
                sh.sh_addralign = s.addralign;
                sh.sh_entsize   = s.entsize;
            }

            // 最后一个 = .shstrtab
            auto& shstrSh = shdrs[totalSections - 1];
            memset(&shstrSh, 0, sizeof(Elf64_Shdr));
            shstrSh.sh_name      = shstrtabNameOff;
            shstrSh.sh_type      = SHT_STRTAB;
            shstrSh.sh_flags     = 0;
            shstrSh.sh_addr      = 0;
            shstrSh.sh_offset    = shstrtabOff;
            shstrSh.sh_size      = shstrtab.size();
            shstrSh.sh_addralign = 1;

            // 更新 ELF header
            auto* ehdr = reinterpret_cast<Elf64_Ehdr*>(outBuf);
            ehdr->e_shoff     = shtOff;
            ehdr->e_shnum     = (uint16_t)totalSections;
            ehdr->e_shentsize = sizeof(Elf64_Shdr);
            ehdr->e_shstrndx  = (uint16_t)shstrtabIdx;
        } else {
            auto* shdrs = reinterpret_cast<Elf32_Shdr*>(outBuf + shtOff);
            memset(&shdrs[0], 0, sizeof(Elf32_Shdr));

            for (size_t i = 0; i < sections.size(); i++) {
                auto& s = sections[i];
                auto& sh = shdrs[i + 1];
                memset(&sh, 0, sizeof(Elf32_Shdr));
                sh.sh_name      = nameOffsets[i];
                sh.sh_type      = s.type;
                sh.sh_flags     = (uint32_t)s.flags;
                sh.sh_addr      = (uint32_t)s.addr;
                sh.sh_offset    = (uint32_t)s.addr;
                sh.sh_size      = (uint32_t)s.size;
                sh.sh_link      = s.link;
                sh.sh_info      = s.info;
                sh.sh_addralign = (uint32_t)s.addralign;
                sh.sh_entsize   = (uint32_t)s.entsize;
            }

            auto& shstrSh = shdrs[totalSections - 1];
            memset(&shstrSh, 0, sizeof(Elf32_Shdr));
            shstrSh.sh_name      = shstrtabNameOff;
            shstrSh.sh_type      = SHT_STRTAB;
            shstrSh.sh_offset    = (uint32_t)shstrtabOff;
            shstrSh.sh_size      = (uint32_t)shstrtab.size();
            shstrSh.sh_addralign = 1;

            auto* ehdr = reinterpret_cast<Elf32_Ehdr*>(outBuf);
            ehdr->e_shoff     = (uint32_t)shtOff;
            ehdr->e_shnum     = (uint16_t)totalSections;
            ehdr->e_shentsize = sizeof(Elf32_Shdr);
            ehdr->e_shstrndx  = (uint16_t)shstrtabIdx;
        }

        outFileSize = newFileSize;
        LOG(LOG_LEVEL_INFO, DTAG " Section headers 重建完成: %zu sections, 文件大小 %zu -> %zu",
            totalSections, outFileSize - totalAppend, outFileSize);
    }

    write_output:

    // ── Step 6: \u51c6\u5907\u8f93\u51fa\u8def\u5f84 ──
    // \u907f\u514d\u5728\u78c1\u76d8\u4e0a\u7559\u4e0b\u5f3a\u7279\u5f81\u540d (\u5305\u540d / libUE4.so),
    // \u5982\u679c basename \u5305\u542b "libUE4" / "lib"+package \u7b49\u5b57\u6837, \u91cd\u5199\u4e3a hash \u968f\u673a\u540d\u3002
    auto looksSensitive = [&](const std::string& base) -> bool {
        std::string low = base;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low.find("libue4") != std::string::npos)    return true;
        if (low.find("libunreal") != std::string::npos) return true;
        if (low.find("com.tencent") != std::string::npos) return true;
        if (low.find(".so") != std::string::npos &&
            low.find("lib") != std::string::npos) return true;
        return false;
    };

    std::string finalPath = outPath;
    {
        size_t slash = finalPath.find_last_of("/\\");
        std::string dir  = (slash == std::string::npos) ? std::string() : finalPath.substr(0, slash + 1);
        std::string base = (slash == std::string::npos) ? finalPath : finalPath.substr(slash + 1);
        if (looksSensitive(base)) {
            // 16 hex char = 8 random bytes; \u540e\u7f00\u4e0d\u7528 .so \u907f\u514d\u88ab\u626b
            uint8_t r[8];
            FILE* urnd = fopen("/dev/urandom", "rb");
            if (urnd) { (void)fread(r, 1, sizeof(r), urnd); fclose(urnd); }
            else      { for (auto& b : r) b = (uint8_t)(rand() & 0xFF); }
            char hex[17];
            static const char* H = "0123456789abcdef";
            for (int i = 0; i < 8; i++) {
                hex[i*2]   = H[(r[i] >> 4) & 0xF];
                hex[i*2+1] = H[ r[i]       & 0xF];
            }
            hex[16] = '\0';
            finalPath = dir + std::string(hex) + ".bin";
            LOG(LOG_LEVEL_INFO, DTAG " \u8f93\u51fa\u540d\u542b\u654f\u611f\u5b57\u6837, \u6539\u540d\u4e3a: %s", finalPath.c_str());
        }
    }

    // ── Step 7: \u5199\u5165\u8f93\u51fa\u6587\u4ef6 ──
    FILE* outFp = fopen(finalPath.c_str(), "wb");
    if (!outFp) {
        LOG(LOG_LEVEL_ERROR, DTAG " 无法创建输出文件: %s (errno=%d, %s)",
            finalPath.c_str(), errno, strerror(errno));
        delete[] outBuf;
        return -5;
    }

    size_t written = fwrite(outBuf, 1, outFileSize, outFp);
    fclose(outFp);
    delete[] outBuf;

    // 修复权限: 复用持久 root shell, 不再 system("su -c ...") 拉新的 su 子进程
    {
        std::string cmd = "chmod 644 '" + finalPath + "'";
        MemReader::runRootShell(cmd);
    }

    if (written != outFileSize) {
        LOG(LOG_LEVEL_ERROR, DTAG " 写入不完整: %zu / %zu", written, outFileSize);
        return -6;
    }

    if (actualOutPath) *actualOutPath = finalPath;

    LOG(LOG_LEVEL_INFO, DTAG " Dump 成功: %s -> %s (%zu bytes, %.2f MB)",
                        module.name.c_str(), finalPath.c_str(),
                        outFileSize, outFileSize / (1024.0 * 1024.0));
    return 0;
}

} // namespace SoDumper
