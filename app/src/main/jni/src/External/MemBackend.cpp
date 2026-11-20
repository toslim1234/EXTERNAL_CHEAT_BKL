#include "MemBackend.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <sys/syscall.h>

#if defined(__aarch64__)
#  define SYS_PVM_READ  270
#  define SYS_PVM_WRITE 271
#elif defined(__arm__)
#  define SYS_PVM_READ  376
#  define SYS_PVM_WRITE 377
#elif defined(__x86_64__)
#  define SYS_PVM_READ  310
#  define SYS_PVM_WRITE 311
#else
#  define SYS_PVM_READ  -1
#  define SYS_PVM_WRITE -1
#endif

namespace bkl {
namespace ext {

const char *backend_name(BackendKind kind) {
    switch (kind) {
        case BackendKind::KernelIoctl: return "kernel-ioctl";
        case BackendKind::DevNode:     return "dev-node";
        case BackendKind::Kpm:         return "kpm-prctl";
        case BackendKind::ProcMem:     return "proc-mem";
        case BackendKind::ProcessVm:   return "process-vm";
        default:                       return "none";
    }
}

// ---------------------------------------------------------------------------
// 1. /dev/nh style ioctl driver (the one this project already shipped)
// ---------------------------------------------------------------------------
namespace {

struct nh_rw {
    pid_t pid;
    uint64_t addr;
    void *data;
    size_t size;
};

#define NH_DEVICE "/dev/nh"
#define NH_READ  _IOWR('N', 1, struct nh_rw)
#define NH_WRITE _IOWR('N', 2, struct nh_rw)

class KernelIoctlBackend : public MemBackend {
public:
    bool attach(pid_t pid) override {
        detach();
        fd_ = open(NH_DEVICE, O_RDWR | O_CLOEXEC);
        if (fd_ < 0) return false;
        pid_ = pid;
        return true;
    }

    void detach() override {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        pid_ = -1;
    }

    bool ready() const override { return fd_ >= 0; }

    ssize_t read(uint64_t addr, void *buf, size_t len) override {
        if (fd_ < 0) return -1;
        nh_rw rw{pid_, addr, buf, len};
        if (ioctl(fd_, NH_READ, &rw) != 0) return -1;
        return (ssize_t) len;
    }

    ssize_t write(uint64_t addr, const void *buf, size_t len) override {
        if (fd_ < 0) return -1;
        nh_rw rw{pid_, addr, const_cast<void *>(buf), len};
        if (ioctl(fd_, NH_WRITE, &rw) != 0) return -1;
        return (ssize_t) len;
    }

    BackendKind kind() const override { return BackendKind::KernelIoctl; }

    const char *detail() const override { return NH_DEVICE; }

private:
    int fd_ = -1;
    pid_t pid_ = -1;
};

// ---------------------------------------------------------------------------
// 2. Vendor character device (/dev/<node>) addressed by file offset
// ---------------------------------------------------------------------------
class DevNodeBackend : public MemBackend {
public:
    bool attach(pid_t pid) override {
        detach();
        std::vector<std::string> nodes = candidate_nodes();
        for (const std::string &n : nodes) {
            int fd = open(n.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) fd = open(n.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            // A usable node either seeks to the target address or reports the pid
            // through an ioctl; we only support the seek flavour here.
            if (lseek(fd, 0, SEEK_SET) == (off_t) -1) {
                close(fd);
                continue;
            }
            fd_ = fd;
            pid_ = pid;
            path_ = n;
            return true;
        }
        return false;
    }

    void detach() override {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        pid_ = -1;
        path_.clear();
    }

    bool ready() const override { return fd_ >= 0; }

    ssize_t read(uint64_t addr, void *buf, size_t len) override {
        if (fd_ < 0) return -1;
        std::lock_guard<std::mutex> lk(m_);
        ssize_t n = pread(fd_, buf, len, (off_t) addr);
        if (n < 0 && errno == ESPIPE) {
            if (lseek(fd_, (off_t) addr, SEEK_SET) < 0) return -1;
            n = ::read(fd_, buf, len);
        }
        return n;
    }

    ssize_t write(uint64_t addr, const void *buf, size_t len) override {
        if (fd_ < 0) return -1;
        std::lock_guard<std::mutex> lk(m_);
        ssize_t n = pwrite(fd_, buf, len, (off_t) addr);
        if (n < 0 && errno == ESPIPE) {
            if (lseek(fd_, (off_t) addr, SEEK_SET) < 0) return -1;
            n = ::write(fd_, buf, len);
        }
        return n;
    }

    BackendKind kind() const override { return BackendKind::DevNode; }

    const char *detail() const override { return path_.c_str(); }

private:
    static std::vector<std::string> candidate_nodes() {
        std::vector<std::string> out;
        const char *env = getenv("BKL_DEV_NODES");
        if (env && *env) {
            std::string s(env), cur;
            for (char c : s) {
                if (c == ':' || c == ',') {
                    if (!cur.empty()) out.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        }
        // Same vendor node family Zytron probes for, plus this project's driver.
        static const char *kDefaults[] = {
                "/dev/nh", "/dev/wanbai", "/dev/CheckMe", "/dev/Ckanri",
                "/dev/lanran", "/dev/video188", "/dev/kpm", "/dev/neo",
        };
        for (const char *n : kDefaults) out.emplace_back(n);
        return out;
    }

    int fd_ = -1;
    pid_t pid_ = -1;
    std::string path_;
    std::mutex m_;
};

// ---------------------------------------------------------------------------
// 3. KPM / Neo prctl multiplexer
// ---------------------------------------------------------------------------
class KpmBackend : public MemBackend {
public:
    bool attach(pid_t pid) override {
        detach();
        if (!configured()) return false;
        if (!kpm_probe()) return false;
        pid_ = pid;
        ok_ = true;
        return true;
    }

    void detach() override {
        ok_ = false;
        pid_ = -1;
    }

    bool ready() const override { return ok_; }

    ssize_t read(uint64_t addr, void *buf, size_t len) override {
        return transfer(true, addr, buf, len);
    }

    ssize_t write(uint64_t addr, const void *buf, size_t len) override {
        return transfer(false, addr, const_cast<void *>(buf), len);
    }

    BackendKind kind() const override { return BackendKind::Kpm; }

    const char *detail() const override { return "prctl(0x4e41)"; }

private:
    static bool configured() {
        return read_op() > 0 && write_op() > 0;
    }

    static int read_op() {
        static int v = [] {
            const char *s = getenv("BKL_KPM_OP_READ");
            return s ? atoi(s) : 0;
        }();
        return v;
    }

    static int write_op() {
        static int v = [] {
            const char *s = getenv("BKL_KPM_OP_WRITE");
            return s ? atoi(s) : 0;
        }();
        return v;
    }

    ssize_t transfer(bool is_read, uint64_t addr, void *buf, size_t len) {
        const int op = is_read ? read_op() : write_op();
        if (!ok_ || op <= 0 || len == 0 || len > 0x1000) return -1;

        std::vector<uint8_t> msg(sizeof(KpmMsg) + len);
        auto *m = reinterpret_cast<KpmMsg *>(msg.data());
        m->op = (uint32_t) op;
        m->pid = (uint32_t) pid_;
        m->addr = addr;
        m->size = len;
        if (!is_read) memcpy(m->data, buf, len);

        long rc = prctl(0x4e41 /* 20033 */, m->op, msg.data(), msg.size(), 0);
        if (rc != 0) return -1;
        if (is_read) memcpy(buf, m->data, len);
        return (ssize_t) len;
    }

    bool ok_ = false;
    pid_t pid_ = -1;
};

// ---------------------------------------------------------------------------
// 4. /proc/<pid>/mem
// ---------------------------------------------------------------------------
class ProcMemBackend : public MemBackend {
public:
    bool attach(pid_t pid) override {
        detach();
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid);
        fd_ = open(path, O_RDWR | O_CLOEXEC);
        if (fd_ < 0) fd_ = open(path, O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) return false;
        pid_ = pid;
        return true;
    }

    void detach() override {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        pid_ = -1;
    }

    bool ready() const override { return fd_ >= 0; }

    ssize_t read(uint64_t addr, void *buf, size_t len) override {
        if (fd_ < 0) return -1;
        return pread(fd_, buf, len, (off_t) addr);
    }

    ssize_t write(uint64_t addr, const void *buf, size_t len) override {
        if (fd_ < 0) return -1;
        return pwrite(fd_, buf, len, (off_t) addr);
    }

    BackendKind kind() const override { return BackendKind::ProcMem; }

    const char *detail() const override { return "/proc/<pid>/mem"; }

private:
    int fd_ = -1;
    pid_t pid_ = -1;
};

// ---------------------------------------------------------------------------
// 5. process_vm_readv/writev against a foreign pid
// ---------------------------------------------------------------------------
class ProcessVmBackend : public MemBackend {
public:
    bool attach(pid_t pid) override {
        detach();
#if SYS_PVM_READ > 0
        pid_ = pid;
        ok_ = true;
        return true;
#else
        (void) pid;
        return false;
#endif
    }

    void detach() override {
        ok_ = false;
        pid_ = -1;
    }

    bool ready() const override { return ok_; }

    ssize_t read(uint64_t addr, void *buf, size_t len) override {
#if SYS_PVM_READ > 0
        if (!ok_) return -1;
        struct iovec local{buf, len};
        struct iovec remote{reinterpret_cast<void *>(addr), len};
        return (ssize_t) syscall(SYS_PVM_READ, pid_, &local, 1UL, &remote, 1UL, 0UL);
#else
        (void) addr; (void) buf; (void) len;
        return -1;
#endif
    }

    ssize_t write(uint64_t addr, const void *buf, size_t len) override {
#if SYS_PVM_WRITE > 0
        if (!ok_) return -1;
        struct iovec local{const_cast<void *>(buf), len};
        struct iovec remote{reinterpret_cast<void *>(addr), len};
        return (ssize_t) syscall(SYS_PVM_WRITE, pid_, &local, 1UL, &remote, 1UL, 0UL);
#else
        (void) addr; (void) buf; (void) len;
        return -1;
#endif
    }

    BackendKind kind() const override { return BackendKind::ProcessVm; }

private:
    bool ok_ = false;
    pid_t pid_ = -1;
};

} // namespace

bool kpm_probe() {
    errno = 0;
    // 0xdead is the handshake constant Zytron uses: a stock kernel rejects it
    // with EINVAL, a patched one answers.
    long rc = prctl(0xdead, 0, 0, 0, 0);
    return rc == 0;
}

MemBackend *make_kernel_ioctl() { return new KernelIoctlBackend(); }

MemBackend *make_dev_node() { return new DevNodeBackend(); }

MemBackend *make_kpm() { return new KpmBackend(); }

MemBackend *make_proc_mem() { return new ProcMemBackend(); }

MemBackend *make_process_vm() { return new ProcessVmBackend(); }

} // namespace ext
} // namespace bkl
