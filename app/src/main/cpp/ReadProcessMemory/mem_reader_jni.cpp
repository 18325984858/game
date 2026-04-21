/**
 * @file    mem_reader_jni.cpp
 * @brief   MemoryReaderActivity 的 JNI 桥接
 *          进程 / 模块列表直接复用 SoDumper 的实现
 */
#include <jni.h>
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <unistd.h>
#include <unordered_map>
#include <android/log.h>

#include "mem_reader.h"
#include "../soDumper/so_dumper.h"
#include "../Log/log.h"
#include "kp_ctl.h"

#include <thread>

#define MTAG "[MemReaderJNI]"

// 懒初始化：首次进入任意 MemReader JNI 时尝试启用 inject-hide 自身隐藏。
//
// ⚠️ 历史 bug 教训:
//   旧实现用 std::call_once 同步执行 KpCtl::isModuleLoaded() →
//   sc_hello_ok() → get_key() → detect_superkey_from_files() →
//   连续 5 次 popen("su -c 'cat ...'")。
//   未 root / root 管理器弹授权对话框 / KernelSU 未装时, 第一次 popen
//   会同步阻塞数十秒甚至永远; 因为是 call_once, 后续所有调用线程会在
//   此处永久排队, 表现为 UI "正在枚举进程..." 永久卡住, 所有依赖
//   nativeListRunningApps / nativeListModules 的页面全部失效.
//
// 修复策略:
//   - 用 atomic_flag 保证只触发一次, 但把 KP 探测/隐藏放到 detached
//     线程, 当前调用线程立即返回. 即使 root 探测永久阻塞也只会泄露
//     一个 worker 线程, 不会传染到 UI 路径.
//   - 隐藏失败/KPM 未加载是非致命的, 完全不应该影响进程枚举.
static void ensureInjectHide() {
    static std::atomic<bool> triggered{false};
    bool expected = false;
    if (!triggered.compare_exchange_strong(expected, true)) return;
    std::thread([]{
        if (KpCtl::isModuleLoaded()) {
            bool ok = KpCtl::hideSelf();
            LOG(LOG_LEVEL_INFO, MTAG " kp hideSelf ok=%d pid=%d", (int)ok, (int)getpid());
        } else {
            LOG(LOG_LEVEL_INFO, MTAG " kp KPM 未加载或 superkey 探测失败, 跳过隐藏");
        }
    }).detach();
}

extern "C" {

// 查询 root 是否未授权 (true = 已被判定拒绝, UI 可显示提示让用户去 APatch/KernelSU 授权).
JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeIsRootAuthDenied(
        JNIEnv*, jobject) {
    return MemReader::isRootAuthDenied() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jobjectArray JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeListRunningApps(
        JNIEnv* env, jobject, jstring jFilter) {

    ensureInjectHide();

    const char* filter = env->GetStringUTFChars(jFilter, nullptr);
    std::string filterStr(filter ? filter : "");
    env->ReleaseStringUTFChars(jFilter, filter);

    auto apps = SoDumper::listRunningApps(filterStr);

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)apps.size(), stringClass, nullptr);

    for (int i = 0; i < (int)apps.size(); i++) {
        std::string item = std::to_string(apps[i].pid) + ":" + apps[i].packageName;
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(item.c_str()));
    }
    return arr;
}

JNIEXPORT jobjectArray JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeListModules(
        JNIEnv* env, jobject, jint pid) {

    auto modules = SoDumper::listModules((int)pid);

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)modules.size(), stringClass, nullptr);

    for (int i = 0; i < (int)modules.size(); i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "0x%lx:0x%lx:%zu:",
                 (unsigned long)modules[i].baseAddr,
                 (unsigned long)modules[i].endAddr,
                 modules[i].size);
        std::string item = std::string(buf) + modules[i].name + ":" + modules[i].path;
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(item.c_str()));
    }
    return arr;
}

/**
 * 列出目标进程所有可读内存映射 (不仅限于 .so 模块)
 * 返回 String[] : ["baseAddr:endAddr:size:name:path:perms", ...]
 *   name 示例: [heap], [stack], [anon:libc_malloc], libUE4.so, <anon>
 *   path 示例: /data/app/.../libUE4.so, [heap], [anon:...], ""(匿名)
 *   只返回可读权限的映射 (第一个字符 == 'r')
 */
JNIEXPORT jobjectArray JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeListAllRegions(
        JNIEnv* env, jobject, jint pid) {

    std::vector<std::string> items;
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "su -c 'cat /proc/%d/maps 2>/dev/null'", (int)pid);
    FILE* fp = popen(cmd, "r");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            uintptr_t start, end;
            char perms[8], path[512];
            unsigned long offset, dev1, dev2, inode;
            path[0] = '\0';
            int matched = sscanf(line, "%lx-%lx %4s %lx %lx:%lx %lu %511[^\n]",
                                 &start, &end, perms, &offset, &dev1, &dev2, &inode, path);
            if (matched < 7) continue;
            if (perms[0] != 'r') continue;  // 仅保留可读

            std::string pathStr = path;
            // 去掉 sscanf 收到的前置空白
            while (!pathStr.empty() && (pathStr.front() == ' ' || pathStr.front() == '\t'))
                pathStr.erase(pathStr.begin());

            std::string name;
            if (pathStr.empty()) {
                name = "<anon>";
                pathStr = "";
            } else if (pathStr[0] == '[') {
                name = pathStr;  // [heap] [stack] [anon:xxx] [vdso] ...
            } else if (pathStr[0] == '/') {
                size_t s = pathStr.rfind('/');
                name = (s != std::string::npos) ? pathStr.substr(s + 1) : pathStr;
            } else {
                name = pathStr;
            }

            char buf[96];
            snprintf(buf, sizeof(buf), "0x%lx:0x%lx:%zu:",
                     (unsigned long)start, (unsigned long)end,
                     (size_t)(end - start));
            items.emplace_back(std::string(buf) + name + ":" + pathStr + ":" + perms);
        }
        pclose(fp);
    }

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)items.size(), stringClass, nullptr);
    for (int i = 0; i < (int)items.size(); i++) {
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(items[i].c_str()));
    }
    return arr;
}

/**
 * 读取指定进程内存
 * @return byte[] 成功时非空, 失败时 length==0
 */
JNIEXPORT jbyteArray JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeReadMemory(
        JNIEnv* env, jobject,
        jint pid, jlong address, jint size) {

    if (pid <= 0 || size <= 0) {
        return env->NewByteArray(0);
    }

    std::vector<uint8_t> out;
    ssize_t got = MemReader::readMemory((int)pid,
                                        (uintptr_t)(uint64_t)address,
                                        (size_t)size, out);
    if (got <= 0) {
        return env->NewByteArray(0);
    }

    jbyteArray arr = env->NewByteArray((jsize)out.size());
    env->SetByteArrayRegion(arr, 0, (jsize)out.size(),
                            reinterpret_cast<const jbyte*>(out.data()));
    return arr;
}

/**
 * 写入指定进程内存
 * @return 实际写入字节数; <=0 表示失败
 */
JNIEXPORT jint JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeWriteMemory(
        JNIEnv* env, jobject,
        jint pid, jlong address, jbyteArray dataBytes) {

    if (pid <= 0 || dataBytes == nullptr) return -1;
    jsize n = env->GetArrayLength(dataBytes);
    if (n <= 0) return -1;

    std::vector<uint8_t> buf((size_t)n);
    env->GetByteArrayRegion(dataBytes, 0, n, reinterpret_cast<jbyte*>(buf.data()));

    ssize_t w = MemReader::writeMemory((int)pid,
                                       (uintptr_t)(uint64_t)address,
                                       buf);
    return (jint)w;
}

/**
 * 查找地址所在的 maps 区间
 * 返回 "baseHex:endHex:name:path:perms", 找不到返回 ""
 */
JNIEXPORT jstring JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeFindRegion(
        JNIEnv* env, jobject, jint pid, jlong address) {

    MemReader::RegionInfo info;
    if (!MemReader::findRegion((int)pid, (uintptr_t)(uint64_t)address, info)) {
        return env->NewStringUTF("");
    }
    char head[64];
    snprintf(head, sizeof(head), "0x%lx:0x%lx:",
             (unsigned long)info.baseAddr, (unsigned long)info.endAddr);
    std::string s = std::string(head) + info.name + ":" + info.path + ":" + info.perms;
    return env->NewStringUTF(s.c_str());
}

/**
 * 在地址范围内搜索字节模式.
 * @param patternBytes jbyte[]   pattern
 * @param maskBytes    jbyte[]   mask (长度需与 pattern 相同)
 * @param maxHits      上限
 * @return long[]  命中地址
 */
JNIEXPORT jlongArray JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeSearchPattern(
        JNIEnv* env, jobject,
        jint pid, jlong rangeStart, jlong rangeEnd,
        jbyteArray patternBytes, jbyteArray maskBytes, jint maxHits) {

    if (patternBytes == nullptr || maskBytes == nullptr) {
        return env->NewLongArray(0);
    }
    jsize pn = env->GetArrayLength(patternBytes);
    jsize mn = env->GetArrayLength(maskBytes);
    if (pn == 0 || pn != mn) {
        return env->NewLongArray(0);
    }

    std::vector<uint8_t> pat((size_t)pn), msk((size_t)mn);
    env->GetByteArrayRegion(patternBytes, 0, pn, reinterpret_cast<jbyte*>(pat.data()));
    env->GetByteArrayRegion(maskBytes,    0, mn, reinterpret_cast<jbyte*>(msk.data()));

    auto hits = MemReader::searchPattern((int)pid,
                                         (uintptr_t)(uint64_t)rangeStart,
                                         (uintptr_t)(uint64_t)rangeEnd,
                                         pat, msk,
                                         (size_t)(maxHits > 0 ? maxHits : 256));

    jlongArray ret = env->NewLongArray((jsize)hits.size());
    if (!hits.empty()) {
        std::vector<jlong> tmp(hits.size());
        for (size_t i = 0; i < hits.size(); i++) tmp[i] = (jlong)(uint64_t)hits[i];
        env->SetLongArrayRegion(ret, 0, (jsize)hits.size(), tmp.data());
    }
    return ret;
}

/**
 * 目标进程全局搜索.
 * 枚举 /proc/PID/maps 里所有可读映射, 依次调用 searchPattern, 累计命中.
 *
 * 返回 String[] , 每条格式: "addrHex|moduleName|moduleOffsetHex|path|perms"
 *   - 文件映射 (.so/.apk ...): moduleName=basename, moduleOffset=addr-moduleBase(同 path 最小 start)
 *   - [heap]/[stack]/<anon>  : moduleName=[heap]/<anon>/..., moduleOffset=addr-regionStart
 *
 * @param onlyWritable 若 true 只搜 rw-p (典型的堆/全局数据), 可大幅加速
 * @param skipBigRo    若 true 跳过 > 64MB 的只读文件映射 (避免重复扫超大 .apk 资源)
 */
JNIEXPORT jobjectArray JNICALL
Java_com_example_dobbyproject_MemoryReaderActivity_nativeGlobalSearch(
        JNIEnv* env, jobject,
        jint pid, jbyteArray patternBytes, jbyteArray maskBytes,
        jint maxHits, jboolean onlyWritable, jboolean skipBigRo) {

    if (pid <= 0 || patternBytes == nullptr || maskBytes == nullptr) {
        jclass sc = env->FindClass("java/lang/String");
        return env->NewObjectArray(0, sc, nullptr);
    }
    jsize pn = env->GetArrayLength(patternBytes);
    jsize mn = env->GetArrayLength(maskBytes);
    if (pn == 0 || pn != mn) {
        jclass sc = env->FindClass("java/lang/String");
        return env->NewObjectArray(0, sc, nullptr);
    }

    std::vector<uint8_t> pat((size_t)pn), msk((size_t)mn);
    env->GetByteArrayRegion(patternBytes, 0, pn, reinterpret_cast<jbyte*>(pat.data()));
    env->GetByteArrayRegion(maskBytes,    0, mn, reinterpret_cast<jbyte*>(msk.data()));

    struct Region {
        uintptr_t start = 0, end = 0;
        uint64_t  fileOff = 0;
        std::string perms;
        std::string path;
        std::string name;
    };
    std::vector<Region> regions;
    std::unordered_map<std::string, uintptr_t> moduleBase; // path -> min start

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "su -c 'cat /proc/%d/maps 2>/dev/null'", (int)pid);
        FILE* fp = popen(cmd, "r");
        if (!fp) {
            jclass sc = env->FindClass("java/lang/String");
            return env->NewObjectArray(0, sc, nullptr);
        }
        char line[1024];
        while (fgets(line, sizeof(line), fp)) {
            uintptr_t st, en;
            char pr[8]; unsigned long off, d1, d2, ino;
            char pa[512]; pa[0] = '\0';
            int matched = sscanf(line, "%lx-%lx %4s %lx %lx:%lx %lu %511[^\n]",
                                 &st, &en, pr, &off, &d1, &d2, &ino, pa);
            if (matched < 7) continue;
            if (pr[0] != 'r') continue;

            Region r;
            r.start   = st;
            r.end     = en;
            r.fileOff = (uint64_t)off;
            r.perms   = pr;
            std::string pathStr = pa;
            while (!pathStr.empty() && (pathStr.front() == ' ' || pathStr.front() == '\t'))
                pathStr.erase(pathStr.begin());
            r.path = pathStr;
            if (pathStr.empty()) {
                r.name = "<anon>";
            } else if (pathStr[0] == '[') {
                r.name = pathStr;
            } else if (pathStr[0] == '/') {
                size_t sp = pathStr.rfind('/');
                r.name = (sp != std::string::npos) ? pathStr.substr(sp + 1) : pathStr;
                auto it = moduleBase.find(pathStr);
                if (it == moduleBase.end() || st < it->second) {
                    moduleBase[pathStr] = st;
                }
            } else {
                r.name = pathStr;
            }
            regions.push_back(std::move(r));
        }
        pclose(fp);
    }

    LOG(LOG_LEVEL_INFO, MTAG " global search: pid=%d regions=%zu pat=%zd onlyW=%d skipBigRo=%d",
          (int)pid, regions.size(), (ssize_t)pn,
          (int)onlyWritable, (int)skipBigRo);

    // 结果收集
    struct Hit { uintptr_t addr; const Region* region; };
    std::vector<Hit> allHits;
    allHits.reserve((size_t)(maxHits > 0 ? maxHits : 256));

    size_t limit = (size_t)(maxHits > 0 ? maxHits : 256);
    size_t scanned = 0;
    for (const auto& r : regions) {
        if (allHits.size() >= limit) break;

        // 跳过一些易出错 / 无意义区域
        if (r.name == "[vvar]" || r.name == "[vdso]" || r.name == "[vsyscall]") continue;
        if (onlyWritable && r.perms.size() >= 2 && r.perms[1] != 'w') continue;
        if (skipBigRo && !r.path.empty() && r.path[0] == '/'
            && r.perms.size() >= 2 && r.perms[1] != 'w'
            && (r.end - r.start) > (uintptr_t)(64ULL * 1024ULL * 1024ULL)) continue;

        size_t remain = limit - allHits.size();
        auto hits = MemReader::searchPattern(
                (int)pid, r.start, r.end, pat, msk, remain);
        scanned++;
        for (auto a : hits) {
            allHits.push_back({ a, &r });
            if (allHits.size() >= limit) break;
        }
    }

    LOG(LOG_LEVEL_INFO, MTAG " global search done: scanned %zu regions, %zu hits",
          scanned, allHits.size());

    // 组装 String[]
    std::vector<std::string> out;
    out.reserve(allHits.size());
    for (const auto& h : allHits) {
        const Region& r = *h.region;
        uintptr_t addr = h.addr;
        std::string line;
        line.reserve(128);
        char buf[96];

        // "addr|name|offsetHex|path|perms"
        snprintf(buf, sizeof(buf), "0x%lx|", (unsigned long)addr);
        line += buf;
        line += r.name;
        line += "|";
        if (!r.path.empty() && r.path[0] == '/') {
            auto it = moduleBase.find(r.path);
            uintptr_t base = (it != moduleBase.end()) ? it->second : r.start;
            snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)(addr - base));
            line += buf;
        } else {
            // 匿名 / [heap] / [stack]: 给 region 内偏移
            snprintf(buf, sizeof(buf), "+0x%lx", (unsigned long)(addr - r.start));
            line += buf;
        }
        line += "|";
        line += r.path;
        line += "|";
        line += r.perms;
        out.push_back(std::move(line));
    }

    jclass sc = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)out.size(), sc, nullptr);
    for (size_t i = 0; i < out.size(); i++) {
        env->SetObjectArrayElement(arr, (jsize)i, env->NewStringUTF(out[i].c_str()));
    }
    return arr;
}

} // extern "C"
