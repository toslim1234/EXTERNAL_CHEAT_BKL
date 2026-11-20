#include "Engine.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace bkl {
namespace ext {

namespace {

const char *kDefaultPackage = "com.dts.freefiremax";
const char *kDefaultModule = "libil2cpp.so";
const char *kConfigPath = "/data/local/tmp/bkl_ext.cfg";

std::string config_value(const char *key) {
    std::ifstream f(kConfigPath);
    if (!f.is_open()) return std::string();
    std::string line;
    const size_t klen = strlen(key);
    while (std::getline(f, line)) {
        if (line.size() <= klen || line.compare(0, klen, key) != 0) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
        size_t s = v.find_first_not_of(" \t\"");
        if (s != std::string::npos) v.erase(0, s);
        size_t e = v.find_last_not_of(" \t\"");
        if (e != std::string::npos) v.erase(e + 1);
        return v;
    }
    return std::string();
}

std::vector<std::string> split_csv(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '|') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

MemBackend *make_backend(const std::string &name) {
    if (name == "ioctl" || name == "nh" || name == "driver") return make_kernel_ioctl();
    if (name == "dev" || name == "node") return make_dev_node();
    if (name == "kpm" || name == "neo") return make_kpm();
    if (name == "proc" || name == "procmem" || name == "root") return make_proc_mem();
    if (name == "vm" || name == "pvm") return make_process_vm();
    return nullptr;
}

} // namespace

Engine &Engine::instance() {
    static Engine e;
    return e;
}

Engine::~Engine() { detach(); }

std::string Engine::pick(const char *env_name, const char *cfg_key,
                         const std::string &fallback) {
    const char *env = env_name ? getenv(env_name) : nullptr;
    if (env && *env) return std::string(env);
    std::string cfg = config_value(cfg_key);
    if (!cfg.empty()) return cfg;
    return fallback;
}

bool Engine::active() const {
    return backend_ != nullptr && backend_->ready() && pid_ > 0;
}

bool Engine::attach(const char *package) {
    std::lock_guard<std::mutex> lk(m_);

    if (package && *package) package_ = package;
    if (package_.empty())
        package_ = pick("BKL_PACKAGE", "package", kDefaultPackage);
    if (module_.empty())
        module_ = pick("BKL_MODULE", "module", kDefaultModule);
    if (backend_order_.empty())
        backend_order_ = pick("BKL_BACKEND", "backend", "auto");

    if (active() && is_alive(pid_)) return true;
    return attach_locked();
}

void Engine::detach() {
    // Stop the watchdog first: it takes m_ while re-attaching, so joining under
    // the lock would deadlock against our own thread.
    if (watching_.exchange(false)) {
        if (watchdog_.joinable()) watchdog_.join();
    }
    freezer_.stop();
    std::lock_guard<std::mutex> lk(m_);
    if (backend_) {
        backend_->detach();
        delete backend_;
        backend_ = nullptr;
    }
    pid_ = -1;
    base_ = 0;
}

bool Engine::verify(MemBackend *b, uint64_t probe_addr) {
    if (!b || !b->ready() || probe_addr == 0) return false;
    uint8_t magic[4] = {0};
    ssize_t n = b->read(probe_addr, magic, sizeof(magic));
    if (n != (ssize_t) sizeof(magic)) return false;
    // A module base must start with ELF magic; a generic probe just has to read.
    if (probe_addr == base_ && base_ != 0) {
        return magic[0] == 0x7f && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
    }
    return true;
}

bool Engine::attach_locked() {
    if (backend_) {
        backend_->detach();
        delete backend_;
        backend_ = nullptr;
    }
    freezer_.stop();
    pid_ = -1;
    base_ = 0;

    const int timeout_s = [] {
        const char *s = getenv("BKL_ATTACH_TIMEOUT");
        return s ? atoi(s) : 30;
    }();
    const int attempts = timeout_s > 0 ? timeout_s * 4 : 1;

    for (int i = 0; i < attempts; ++i) {
        pid_t pid = find_pid(package_.c_str());
        if (pid > 0 && is_alive(pid)) {
            pid_ = pid;
            break;
        }
        if (i + 1 < attempts)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (pid_ <= 0) return false;

    base_ = module_base(pid_, module_.c_str());

    uint64_t probe = base_;
    if (probe == 0) {
        for (const MapRegion &r : read_maps(pid_)) {
            if (r.readable() && r.size() > 0) {
                probe = r.start;
                break;
            }
        }
    }

    std::vector<std::string> order;
    if (backend_order_ == "auto") {
        order = {"ioctl", "dev", "kpm", "proc", "vm"};
    } else {
        order = split_csv(backend_order_);
    }

    for (const std::string &name : order) {
        MemBackend *b = make_backend(name);
        if (!b) continue;
        if (!b->attach(pid_)) {
            delete b;
            continue;
        }
        if (!verify(b, probe)) {
            b->detach();
            delete b;
            continue;
        }
        backend_ = b;
        scanner_.set_io(this);
        freezer_.set_io(this);
        patcher_.set_io(this);
        freezer_.start();
        if (!watching_.exchange(true)) {
            watchdog_ = std::thread(&Engine::watchdog, this);
        }
        fail_streak_ = 0;
        return true;
    }

    return false;
}

ssize_t Engine::read(uint64_t addr, void *buf, size_t len) {
    std::lock_guard<std::mutex> lk(m_);
    if (!backend_ || !backend_->ready()) return -1;
    ssize_t n = backend_->read(addr, buf, len);
    if (n < 0 && is_alive(pid_)) {
        // A single failed read on a stale pointer is not a reason to re-attach.
    }
    return n;
}

ssize_t Engine::write(uint64_t addr, const void *buf, size_t len) {
    std::lock_guard<std::mutex> lk(m_);
    if (!backend_ || !backend_->ready()) return -1;
    return backend_->write(addr, buf, len);
}

uint64_t Engine::base(const char *module) {
    std::lock_guard<std::mutex> lk(m_);
    const char *want = (module && *module) ? module : module_.c_str();
    if (base_ != 0 && strcmp(want, module_.c_str()) == 0) return base_;
    if (pid_ <= 0) return 0;
    uint64_t b = module_base(pid_, want);
    if (b != 0 && strcmp(want, module_.c_str()) == 0) base_ = b;
    return b;
}

ScanOptions Engine::default_scan_options(bool module_only, size_t max_hits) {
    std::lock_guard<std::mutex> lk(m_);
    ScanOptions opt;
    opt.max_hits = max_hits;

    if (module_only && pid_ > 0) {
        uint64_t s = 0, e = 0;
        if (module_range(pid_, module_.c_str(), s, e)) {
            opt.ranges.push_back(ScanRange{s, e});
            return opt;
        }
    }

    if (pid_ <= 0) return opt;
    for (const MapRegion &r : read_maps(pid_)) {
        if (!r.readable() || r.executable() || r.size() == 0) continue;
        if (!r.writable() && !r.path.empty() && r.path[0] != '[') continue;
        opt.ranges.push_back(ScanRange{r.start, r.end});
    }
    return opt;
}

std::string Engine::status() {
    // Do not hold m_ while asking the scanner/freezer/patcher for their counts:
    // those objects take their own locks and call back into Engine::read (which
    // takes m_), so nesting the locks here would be an ABBA deadlock.
    pid_t pid;
    uint64_t base;
    std::string module;
    std::string backend;
    {
        std::lock_guard<std::mutex> lk(m_);
        pid = pid_;
        base = base_;
        module = module_;
        backend = backend_ ? std::string(backend_name(backend_->kind())) + ":" + backend_->detail()
                           : std::string("none");
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
             "pid=%d alive=%d backend=%s base=0x%llx module=%s hits=%zu freeze=%zu patches=%zu",
             pid, pid > 0 ? (int) is_alive(pid) : 0, backend.c_str(),
             (unsigned long long) base, module.c_str(),
             scanner_.count(), freezer_.list().size(), patcher_.patch_count());
    return std::string(buf);
}

void Engine::watchdog() {
    while (watching_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (!watching_.load()) break;

        bool dead;
        {
            std::lock_guard<std::mutex> lk(m_);
            dead = (pid_ <= 0) || !is_alive(pid_);
            if (dead) {
                freezer_.stop();
                if (backend_) {
                    backend_->detach();
                    delete backend_;
                    backend_ = nullptr;
                }
                pid_ = -1;
                base_ = 0;
            }
        }
        if (!dead) {
            fail_streak_ = 0;
            continue;
        }

        // The game was closed or restarted: wait for it again, but do not spin.
        if (fail_streak_ >= 20) {
            fail_streak_ = 0;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        ++fail_streak_;
        std::lock_guard<std::mutex> lk(m_);
        attach_locked();
    }
}

// ---------------------------------------------------------------------------

bool active() { return Engine::instance().active(); }

ssize_t read(uint64_t addr, void *buf, size_t len) {
    return Engine::instance().read(addr, buf, len);
}

ssize_t write(uint64_t addr, const void *buf, size_t len) {
    return Engine::instance().write(addr, buf, len);
}

uint64_t base(const char *module) { return Engine::instance().base(module); }

} // namespace ext
} // namespace bkl
