// SPDX-License-Identifier: MIT
//
// ARM64 sleep-mask 风格调用栈伪造实现.
//
// 设计要点:
//   - AAPCS for AArch64: 函数序言 `stp x29, x30, [sp, #-N]!; mov x29, sp` 后,
//     [x29, #0]  = saved_FP (上一层的 x29)
//     [x29, #8]  = saved_LR (上一层的返回地址)
//     unwinder 通常做: fp = *fp; lr = *(fp+8); 直到 fp==0 或越界.
//   - 我们在线程栈低位预先布置好一串 [fake_FP, fake_LR] 对, 链尾 fake_FP=0,
//     然后切换 x29 到链顶, 调用 fn. fn 自身 prologue 会再压一层 [真x29, 真x30],
//     但那层的 saved_FP 是我们伪造的链顶 → unwinder 顺势爬进伪造链, 最后 fp=0 终止.
//     真实栈上的 LR 永远不会被走到.
//   - LR 池来自 libc.so / libart.so 等白名单模块: 扫 r-x 段找 0x94000000/0xD63F0000
//     (BL/BLR 编码) 后的下一条指令地址, 即为合法返回地址.

#include "stack_spoof.h"

#include "../core/log/log.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace stack_spoof {

namespace {

constexpr int kMaxLrPool = 64;
constexpr int kFakeFrames = 8;          // fake chain 层数
constexpr size_t kFakeStackBytes = 4096; // 每线程 fake stack 大小, 一页足够

struct LrPool {
    uintptr_t addrs[kMaxLrPool];
    int count = 0;
};

LrPool g_pool;
std::once_flag g_init_once;
std::atomic<bool> g_init_ok{false};

// 每线程的 fake stack 内存 + 链顶 FP 地址.
struct ThreadSpoofState {
    void* stack = nullptr;       // mmap 出来的 fake stack 缓冲 (kFakeStackBytes)
    uintptr_t chain_top_fp = 0;  // 切换 x29 时使用的值
};

pthread_key_t g_tls_key;
std::once_flag g_tls_once;

void tls_destructor(void* p) {
    auto* st = static_cast<ThreadSpoofState*>(p);
    if (!st) return;
    if (st->stack) munmap(st->stack, kFakeStackBytes);
    free(st);
}

void ensure_tls_key() {
    std::call_once(g_tls_once, []() {
        pthread_key_create(&g_tls_key, &tls_destructor);
    });
}

ThreadSpoofState* get_thread_state() {
    ensure_tls_key();
    auto* st = static_cast<ThreadSpoofState*>(pthread_getspecific(g_tls_key));
    if (st) return st;
    st = static_cast<ThreadSpoofState*>(calloc(1, sizeof(ThreadSpoofState)));
    if (!st) return nullptr;
    void* mem = mmap(nullptr, kFakeStackBytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        free(st);
        return nullptr;
    }
    st->stack = mem;
    pthread_setspecific(g_tls_key, st);
    return st;
}

// 找一个模块的 r-x 段, 返回 [base, end). 失败返回 false.
bool find_module_text(const char* needle, uintptr_t& out_base, uintptr_t& out_end) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        // 形如: 7xxxxx-7xxxxx r-xp 00000000 ... /path/to/libc.so
        if (!strstr(line, "r-xp")) continue;
        const char* slash = strrchr(line, '/');
        if (!slash) continue;
        if (!strstr(slash, needle)) continue;
        uintptr_t b = 0, e = 0;
        if (sscanf(line, "%lx-%lx", &b, &e) == 2 && b && e > b) {
            out_base = b;
            out_end = e;
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

// 在 [base, end) 里找 BL/BLR 指令, 把"BL/BLR 后的下一条指令地址"作为合法返回地址加入池.
// AArch64 编码:
//   BL  imm26   = 1001 01ii iiii iiii iiii iiii iiii iiii  (高 6 bit = 100101)
//   BLR Xn      = 1101 0110 0011 1111 0000 00nn nnn0 0000  (mask 0xFFFFFC1F == 0xD63F0000)
void harvest_lrs(uintptr_t base, uintptr_t end, int want) {
    const uint32_t* p = reinterpret_cast<const uint32_t*>(base);
    const uint32_t* e = reinterpret_cast<const uint32_t*>(end);
    // 步长大一点避免每条都收, 也避免靠得太近的返回地址在某些 unwinder 里被合并.
    constexpr int kStride = 0x800 / 4;
    for (; p + 1 < e && g_pool.count < want; p += kStride) {
        // 在窗口内扫一小段, 找第一条 BL/BLR.
        const uint32_t* q = p;
        const uint32_t* qe = q + kStride;
        if (qe > e - 1) qe = e - 1;
        for (; q < qe; ++q) {
            uint32_t insn = *q;
            bool is_bl  = (insn & 0xFC000000u) == 0x94000000u;
            bool is_blr = (insn & 0xFFFFFC1Fu) == 0xD63F0000u;
            if (!is_bl && !is_blr) continue;
            uintptr_t ret_addr = reinterpret_cast<uintptr_t>(q + 1);
            if (g_pool.count < kMaxLrPool) {
                g_pool.addrs[g_pool.count++] = ret_addr;
            }
            break;
        }
    }
}

void do_init(int min_frames) {
    static const char* const kModules[] = {
        "libc.so",
        "libart.so",
        "libandroid_runtime.so",
        "libutils.so",
        nullptr,
    };
    for (int i = 0; kModules[i] && g_pool.count < kMaxLrPool; ++i) {
        uintptr_t b = 0, e = 0;
        if (!find_module_text(kModules[i], b, e)) {
            LOG(LOG_LEVEL_WARN, "[stack_spoof] module not found: %s", kModules[i]);
            continue;
        }
        int before = g_pool.count;
        harvest_lrs(b, e, kMaxLrPool);
        LOG(LOG_LEVEL_INFO, "[stack_spoof] %s [%lx-%lx) +%d LRs",
            kModules[i], b, e, g_pool.count - before);
    }
    g_init_ok.store(g_pool.count >= min_frames, std::memory_order_release);
    LOG(LOG_LEVEL_INFO, "[stack_spoof] init done, pool=%d ok=%d",
        g_pool.count, g_init_ok.load() ? 1 : 0);
}

// 在 fake stack 里铺好 N 层 [FP, LR] 链, 返回链顶 FP 地址 (调用前要 mov x29, ret).
// 布局 (低地址→高地址):
//   [pair0_FP][pair0_LR][pair1_FP][pair1_LR]...[pairN-1_FP=0][pairN-1_LR=last]
// 链顶是 pair0, pair0_FP 指向 pair1, pair1_FP 指向 pair2, ..., 链尾 FP=0 终止.
uintptr_t build_fake_chain(ThreadSpoofState* st) {
    if (!st || !st->stack) return 0;
    if (g_pool.count <= 0) return 0;
    // 用 16 字节对齐, 每对 16 字节.
    auto* base = reinterpret_cast<uintptr_t*>(st->stack);
    // 把 chain 放在 fake stack 的中间, 前后留点余量便于栈采样器读越界不 crash.
    constexpr size_t kOff = kFakeStackBytes / 2 / sizeof(uintptr_t);
    uintptr_t* pairs = base + kOff;

    int n = kFakeFrames;
    for (int i = 0; i < n; ++i) {
        uintptr_t* slot_fp = pairs + i * 2;
        uintptr_t* slot_lr = pairs + i * 2 + 1;
        uintptr_t next_fp = (i == n - 1) ? 0
                                         : reinterpret_cast<uintptr_t>(pairs + (i + 1) * 2);
        // 从池子里轮询挑一个 LR.
        uintptr_t lr = g_pool.addrs[(i * 7 + 3) % g_pool.count];
        slot_fp[0] = next_fp;
        slot_lr[0] = lr;
    }
    st->chain_top_fp = reinterpret_cast<uintptr_t>(pairs);
    return st->chain_top_fp;
}

// 真正的 trampoline: 切 x29 → 调 fn(arg) → 还原 x29 → 返回 fn 的返回值.
//
// 重点:
//   - 不能在切了 x29 之后还指望编译器还原, 所以整个体用 naked 风格 + 自己控制 prologue/epilogue.
//   - 我们正常压栈保存 x29/x30/真FP, 然后 mov x29, fake_fp; blr fn; 恢复 x29/x30; ret.
//   - SP 不动, fn 内的局部变量仍在真实栈上 → 安全.
//   - 注意 X0=fn, X1=arg, X2=fake_fp 这个调用约定是函数自定义的, 入参顺序看下面声明.
//
// 残留泄漏点 (已知, 留 TODO):
//   - fn 自己 prologue 会把进入时的 x30 (= "blr x19" 后的下一条指令地址) 压到自己栈帧.
//     该地址落在本 .so (libdobbyproject) 内 → unwinder 会看到一帧 "trampoline 残骸".
//     之后才会跳进 fake chain. 想完全消掉这一帧, 需要把 spoof_trampoline 物理拷到
//     libUE4.so cave (见 parasite 模块) 里执行, 让那条 LR 也落在白名单模块.
//     单独做 sleep-mask 时这一帧通常无关紧要 (大多数检测看的是栈深处而不是栈顶).
extern "C" __attribute__((naked, noinline))
void* spoof_trampoline(void* (*fn)(void*) /*x0*/,
                       void* arg /*x1*/,
                       uintptr_t fake_fp /*x2*/) {
    asm volatile(
        // 标准 prologue: 保存调用者 FP/LR, 申请 32 字节栈 (16 对齐).
        "stp    x29, x30, [sp, #-32]!\n"
        "mov    x29, sp\n"
        // 把 fn 指针先挪到 callee-saved 寄存器, 因为我们待会要用 x0 传 arg.
        "stp    x19, x20, [sp, #16]\n"
        "mov    x19, x0\n"      // x19 = fn
        "mov    x20, x2\n"      // x20 = fake_fp
        // 关键: 切 x29 到伪造链顶. 此后任何顺 FP 走的 unwinder 都会爬进 fake chain.
        // 我们当前函数 prologue 已经把 [真x29, 真x30] 压到 [sp,#0], 但 unwinder
        // 拿到的 x29 已经被改成 fake_fp 了, 所以那一层不会被读到 — 它会直接读 fake[0].
        "mov    x29, x20\n"
        // 调用 fn(arg). arg 早已在 x1 → 挪到 x0.
        "mov    x0, x1\n"
        "blr    x19\n"
        // 还原.
        "ldp    x19, x20, [sp, #16]\n"
        "ldp    x29, x30, [sp], #32\n"
        "ret\n"
    );
}

} // namespace

bool init(int min_frames) {
    std::call_once(g_init_once, [&]() { do_init(min_frames); });
    return g_init_ok.load(std::memory_order_acquire);
}

void* call_spoofed(void* (*fn)(void*), void* arg) {
    if (!fn) return nullptr;
    if (!init()) {
        // 池子没建起来, 退化为直接调用 — 至少不要崩.
        return fn(arg);
    }
    auto* st = get_thread_state();
    if (!st) return fn(arg);
    uintptr_t fake_fp = build_fake_chain(st);
    if (!fake_fp) return fn(arg);
    return spoof_trampoline(fn, arg, fake_fp);
}

namespace {
struct SleepArg { uint64_t ns; int ret; };

void* sleep_thunk(void* p) {
    auto* a = static_cast<SleepArg*>(p);
    timespec req{};
    req.tv_sec  = static_cast<time_t>(a->ns / 1000000000ULL);
    req.tv_nsec = static_cast<long>(a->ns % 1000000000ULL);
    a->ret = nanosleep(&req, nullptr);
    return nullptr;
}
} // namespace

int spoofed_sleep_ns(uint64_t nanoseconds) {
    SleepArg a{nanoseconds, 0};
    call_spoofed(&sleep_thunk, &a);
    return a.ret;
}

} // namespace stack_spoof

// ─────────────────────────────────────────────────────────────────────────────
// extern "C" Frida 友好包装
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

int stack_spoof_init(int min_frames) {
    return stack_spoof::init(min_frames) ? 1 : 0;
}

void* stack_spoof_call(void* (*fn)(void*), void* arg) {
    return stack_spoof::call_spoofed(fn, arg);
}

int stack_spoof_sleep_ns(unsigned long long nanoseconds) {
    return stack_spoof::spoofed_sleep_ns(static_cast<uint64_t>(nanoseconds));
}

} // extern "C"
