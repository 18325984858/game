#include "stealth_hooks.h"

#include "../log/log.h"
#include "../../Dobby/include/dobby.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <sys/system_properties.h>

namespace {

bool containsNeedle(const char* text, const char* needle) {
    return text && needle && needle[0] && strstr(text, needle) != nullptr;
}

bool isHiddenModuleName(const char* name) {
    static const char* const needles[] = {
        "libdobbyproject",
        "dobbyproject",
        "libdobby.so",
        "Dobby",
        "dobby",
        "frida-agent",
        "frida",
        "gum-js-loop",
        "linjector",
        nullptr,
    };
    if (!name || !name[0]) return false;
    for (int i = 0; needles[i]; ++i) {
        if (containsNeedle(name, needles[i])) return true;
    }
    return false;
}

const char* spoofPropertyValue(const char* name) {
    struct Item { const char* key; const char* value; };
    static const Item items[] = {
        {"ro.boot.verifiedbootstate", "green"},
        {"ro.boot.vbmeta.device_state", "locked"},
        {"ro.boot.flash.locked", "1"},
        {"ro.boot.veritymode", "enforcing"},
        {"ro.boot.warranty_bit", "0"},
        {"ro.warranty_bit", "0"},
        {"ro.debuggable", "0"},
        {"ro.secure", "1"},
        {"ro.build.type", "user"},
        {"ro.build.tags", "release-keys"},
        {"ro.boot.selinux", "enforcing"},
        {nullptr, nullptr},
    };
    if (!name) return nullptr;
    for (int i = 0; items[i].key; ++i) {
        if (strcmp(name, items[i].key) == 0) return items[i].value;
    }
    return nullptr;
}

void* resolveSymbol(const char* image, const char* symbol) {
    void* addr = DobbySymbolResolver(image, symbol);
    if (!addr) addr = dlsym(RTLD_DEFAULT, symbol);
    return addr;
}

using DlIteratePhdrFn = int (*)(int (*)(struct dl_phdr_info*, size_t, void*), void*);
using DladdrFn = int (*)(const void*, Dl_info*);
using SystemPropertyGetFn = int (*)(const char*, char*);
using SystemPropertyReadFn = int (*)(const prop_info*, char*, char*);
using SystemPropertyReadCallbackFn = void (*)(const prop_info*, void (*)(void*, const char*, const char*, uint32_t), void*);

DlIteratePhdrFn origDlIteratePhdr = nullptr;
DladdrFn origDladdr = nullptr;
SystemPropertyGetFn origSystemPropertyGet = nullptr;
SystemPropertyReadFn origSystemPropertyRead = nullptr;
SystemPropertyReadCallbackFn origSystemPropertyReadCallback = nullptr;

struct IterateCtx {
    int (*callback)(struct dl_phdr_info*, size_t, void*);
    void* data;
};

int filteredPhdrCallback(struct dl_phdr_info* info, size_t size, void* data) {
    auto* ctx = static_cast<IterateCtx*>(data);
    if (!ctx || !ctx->callback) return 0;
    if (info && isHiddenModuleName(info->dlpi_name)) return 0;
    return ctx->callback(info, size, ctx->data);
}

int fakeDlIteratePhdr(int (*callback)(struct dl_phdr_info*, size_t, void*), void* data) {
    if (!origDlIteratePhdr || !callback) return 0;
    IterateCtx ctx{callback, data};
    return origDlIteratePhdr(filteredPhdrCallback, &ctx);
}

int fakeDladdr(const void* addr, Dl_info* info) {
    if (!origDladdr) return 0;
    int ret = origDladdr(addr, info);
    if (ret && info && isHiddenModuleName(info->dli_fname)) return 0;
    return ret;
}

int fakeSystemPropertyGet(const char* name, char* value) {
    const char* spoof = spoofPropertyValue(name);
    if (spoof && value) {
        strcpy(value, spoof);
        return static_cast<int>(strlen(spoof));
    }
    return origSystemPropertyGet ? origSystemPropertyGet(name, value) : 0;
}

int fakeSystemPropertyRead(const prop_info* pi, char* name, char* value) {
    if (!origSystemPropertyRead) return 0;
    int ret = origSystemPropertyRead(pi, name, value);
    const char* spoof = spoofPropertyValue(name);
    if (spoof && value) {
        strcpy(value, spoof);
        ret = static_cast<int>(strlen(spoof));
    }
    return ret;
}

struct PropCallbackCtx {
    void (*callback)(void*, const char*, const char*, uint32_t);
    void* cookie;
};

void filteredPropertyCallback(void* cookie, const char* name, const char* value, uint32_t serial) {
    auto* ctx = static_cast<PropCallbackCtx*>(cookie);
    if (!ctx || !ctx->callback) return;
    const char* spoof = spoofPropertyValue(name);
    ctx->callback(ctx->cookie, name, spoof ? spoof : value, serial);
}

void fakeSystemPropertyReadCallback(const prop_info* pi,
                                    void (*callback)(void*, const char*, const char*, uint32_t),
                                    void* cookie) {
    if (!origSystemPropertyReadCallback || !callback) return;
    PropCallbackCtx ctx{callback, cookie};
    origSystemPropertyReadCallback(pi, filteredPropertyCallback, &ctx);
}

void hookOne(const char* image, const char* symbol, void* replacement, void** origin) {
    void* addr = resolveSymbol(image, symbol);
    if (!addr || !replacement || !origin || *origin) return;
    if (DobbyHook(addr, reinterpret_cast<dobby_dummy_func_t>(replacement),
                  reinterpret_cast<dobby_dummy_func_t*>(origin)) != 0) {
        *origin = nullptr;
    }
}

} // namespace

void installStealthHooks() {
    static std::atomic<bool> installed{false};
    bool expected = false;
    if (!installed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    hookOne("libdl.so", "dl_iterate_phdr", reinterpret_cast<void*>(fakeDlIteratePhdr),
            reinterpret_cast<void**>(&origDlIteratePhdr));
    hookOne("libdl.so", "dladdr", reinterpret_cast<void*>(fakeDladdr),
            reinterpret_cast<void**>(&origDladdr));
    hookOne("libc.so", "__system_property_get", reinterpret_cast<void*>(fakeSystemPropertyGet),
            reinterpret_cast<void**>(&origSystemPropertyGet));
    hookOne("libc.so", "__system_property_read", reinterpret_cast<void*>(fakeSystemPropertyRead),
            reinterpret_cast<void**>(&origSystemPropertyRead));
    hookOne("libc.so", "__system_property_read_callback", reinterpret_cast<void*>(fakeSystemPropertyReadCallback),
            reinterpret_cast<void**>(&origSystemPropertyReadCallback));

    LOG(LOG_LEVEL_INFO, "[Stealth] hooks installed: phdr=%d dladdr=%d prop_get=%d prop_read=%d prop_cb=%d",
        origDlIteratePhdr != nullptr, origDladdr != nullptr, origSystemPropertyGet != nullptr,
        origSystemPropertyRead != nullptr, origSystemPropertyReadCallback != nullptr);
}
