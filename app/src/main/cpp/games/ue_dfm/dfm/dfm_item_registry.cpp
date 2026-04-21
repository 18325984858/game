#include "dfm_item_registry.h"
#include "../../../core/log/log.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace dfm {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool ensureDir(const std::string& dir) {
    if (dir.empty()) return false;
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0) {
        return (st.st_mode & S_IFDIR) != 0;
    }
    // 尝试 mkdir -p (逐层); 大多数情况下 dir 是 app external_files, 已存在
    return ::mkdir(dir.c_str(), 0755) == 0;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

ItemRegistry& ItemRegistry::instance() {
    static ItemRegistry inst;
    return inst;
}

void ItemRegistry::init(const std::string& dir) {
    bool expected = false;
    if (!m_initialized.compare_exchange_strong(expected, true)) {
        return;  // 仅首次生效
    }
    ensureDir(dir);
    m_path = dir;
    if (!m_path.empty() && m_path.back() != '/') m_path += '/';
    m_path += "dfm_items.txt";
    load();
    LOG(LOG_LEVEL_INFO, "[ItemRegistry] init path=%s entries=%zu",
        m_path.c_str(), m_map.size());
}

bool ItemRegistry::load() {
    std::lock_guard<std::mutex> g(m_mtx);
    std::ifstream ifs(m_path);
    if (!ifs.is_open()) return false;

    m_map.clear();
    std::string line;
    while (std::getline(ifs, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        // 格式: idName\tdisplay\tclassName  (className 可空)
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        std::string idName = line.substr(0, t1);
        std::string disp;
        std::string cls;
        if (t2 == std::string::npos) {
            disp = line.substr(t1 + 1);
        } else {
            disp = line.substr(t1 + 1, t2 - t1 - 1);
            cls  = line.substr(t2 + 1);
        }
        if (idName.empty() || disp.empty()) continue;
        m_map[idName] = Entry{disp, cls};
    }
    m_lastSaveMs.store(nowMs(), std::memory_order_relaxed);
    return true;
}

void ItemRegistry::record(const std::string& idName,
                          const std::string& display,
                          const std::string& className) {
    if (idName.empty() || display.empty()) return;
    if (idName == "None" || idName == "?") return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> g(m_mtx);
        auto it = m_map.find(idName);
        if (it == m_map.end()) {
            m_map.emplace(idName, Entry{display, className});
            changed = true;
        } else if (it->second.display != display ||
                   (!className.empty() && it->second.className != className)) {
            it->second.display = display;
            if (!className.empty()) it->second.className = className;
            changed = true;
        }
    }
    if (changed) {
        m_dirty.store(true, std::memory_order_release);
        maybeAutoSave();
    }
}

std::string ItemRegistry::lookup(const std::string& idName) const {
    if (idName.empty()) return {};
    std::lock_guard<std::mutex> g(m_mtx);
    auto it = m_map.find(idName);
    return it == m_map.end() ? std::string{} : it->second.display;
}

size_t ItemRegistry::size() const {
    std::lock_guard<std::mutex> g(m_mtx);
    return m_map.size();
}

bool ItemRegistry::save() {
    std::string tmpPath;
    std::ostringstream oss;
    {
        std::lock_guard<std::mutex> g(m_mtx);
        if (m_path.empty()) return false;
        tmpPath = m_path + ".tmp";
        oss << "# DFM item registry (auto-generated)\n";
        oss << "# format: idName<TAB>display<TAB>className\n";
        oss << "# entries=" << m_map.size() << "\n";
        for (const auto& kv : m_map) {
            oss << kv.first << '\t' << kv.second.display << '\t'
                << kv.second.className << '\n';
        }
    }
    {
        std::ofstream ofs(tmpPath, std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs << oss.str();
        if (!ofs.good()) return false;
    }
    if (::rename(tmpPath.c_str(), m_path.c_str()) != 0) {
        ::remove(tmpPath.c_str());
        return false;
    }
    m_dirty.store(false, std::memory_order_release);
    m_lastSaveMs.store(nowMs(), std::memory_order_relaxed);
    return true;
}

void ItemRegistry::maybeAutoSave() {
    if (!m_dirty.load(std::memory_order_acquire)) return;
    int64_t last = m_lastSaveMs.load(std::memory_order_relaxed);
    if (nowMs() - last < 30000) return;  // 30s 内不重复写
    save();
}

} // namespace dfm
