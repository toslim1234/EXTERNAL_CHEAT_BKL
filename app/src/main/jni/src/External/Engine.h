// External engine facade.
//
// Holds the attached process, the selected memory backend, and the scanner /
// freezer / patcher built on top of it. This is the piece that makes the cheat
// work without injecting anything: the daemon owns a backend handle and every
// Read<>/Write<> in the project goes through it.
#ifndef BKL_EXT_ENGINE_H
#define BKL_EXT_ENGINE_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MemBackend.h"
#include "Patcher.h"
#include "Process.h"
#include "Scanner.h"

namespace bkl {
namespace ext {

class Engine : public MemIO {
public:
    static Engine &instance();

    // Finds the package, picks a backend, verifies it and caches the module base.
    // `package` may be null to use the configured default.
    bool attach(const char *package = nullptr);

    void detach();

    bool active() const;

    pid_t pid() const { return pid_; }

    uint64_t base(const char *module = nullptr);

    std::string status();

    // MemIO
    ssize_t read(uint64_t addr, void *buf, size_t len) override;

    ssize_t write(uint64_t addr, const void *buf, size_t len) override;

    Scanner &scanner() { return scanner_; }

    Freezer &freezer() { return freezer_; }

    Patcher &patcher() { return patcher_; }

    // Default scan window: the game module, or the whole readable address space
    // when the module is unknown.
    ScanOptions default_scan_options(bool module_only, size_t max_hits);

    const std::string &package() const { return package_; }

    const std::string &module() const { return module_; }

private:
    Engine() = default;

    ~Engine() override;

    bool attach_locked();

    bool verify(MemBackend *b, uint64_t probe_addr);

    void watchdog();

    static std::string pick(const char *env_name, const char *cfg_key,
                            const std::string &fallback);

    mutable std::mutex m_;
    pid_t pid_ = -1;
    MemBackend *backend_ = nullptr;
    uint64_t base_ = 0;
    std::string package_;
    std::string module_;
    std::string backend_order_;
    std::thread watchdog_;
    std::atomic<bool> watching_{false};
    int fail_streak_ = 0;

    Scanner scanner_;
    Freezer freezer_;
    Patcher patcher_;
};

// Convenience wrappers used by Utils.h so it does not have to know the class.
bool active();

ssize_t read(uint64_t addr, void *buf, size_t len);

ssize_t write(uint64_t addr, const void *buf, size_t len);

uint64_t base(const char *module = nullptr);

} // namespace ext
} // namespace bkl

#endif // BKL_EXT_ENGINE_H
