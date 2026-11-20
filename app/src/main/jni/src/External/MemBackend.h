// External memory backends.
//
// Every way of touching another process's memory that is worth supporting lives
// here behind one interface, so the engine can pick whichever one the phone
// actually allows and fall through to the next when it does not.
//
// Order used by the auto-detector (see Engine.cpp):
//     KernelIoctl  /dev/nh                 (existing BKL kernel driver)
//     DevNode      /dev/<vendor node>      (Zytron-style char device)
//     Kpm          prctl(0xdead) probe     (Neo/KPM kernel extension)
//     ProcMem      /proc/<pid>/mem         (root, plain file IO)
//     ProcessVm    process_vm_readv        (ptrace-capable root)
#ifndef BKL_EXT_MEMBACKEND_H
#define BKL_EXT_MEMBACKEND_H

#include <cstdint>
#include <cstddef>
#include <sys/types.h>

namespace bkl {
namespace ext {

enum class BackendKind : int {
    None = 0,
    KernelIoctl,
    DevNode,
    Kpm,
    ProcMem,
    ProcessVm,
};

const char *backend_name(BackendKind kind);

class MemBackend {
public:
    virtual ~MemBackend() = default;

    virtual bool attach(pid_t pid) = 0;

    virtual void detach() = 0;

    virtual bool ready() const = 0;

    // Both return the byte count transferred, or -1 on a hard error. A short
    // count is normal at the end of a mapping and must not be treated as fatal.
    virtual ssize_t read(uint64_t addr, void *buf, size_t len) = 0;

    virtual ssize_t write(uint64_t addr, const void *buf, size_t len) = 0;

    virtual BackendKind kind() const = 0;

    // Human-readable detail for status output (device path, fd, ...).
    virtual const char *detail() const { return ""; }
};

// Factories. Each returns nullptr when the backend is not usable on this device.
MemBackend *make_kernel_ioctl();
MemBackend *make_dev_node();
MemBackend *make_kpm();
MemBackend *make_proc_mem();
MemBackend *make_process_vm();

// ---------------------------------------------------------------- KPM protocol
//
// Zytron's KPM backend is reached through two prctl() options:
//     prctl(0xdead, 0, 0, 0, 0)          probe; stock kernels return EINVAL
//     prctl(0x4e41 /*20033*/, op, buf, len, 0)
// The op ids are per-module and are not knowable statically, so they are read
// from the environment and the backend stays disabled unless both are set:
//     BKL_KPM_OP_READ=<n>  BKL_KPM_OP_WRITE=<n>
// Payload layout (little endian, aarch64):
//     u32 op | u32 pid | u64 addr | u64 size | u8 data[size]
struct KpmMsg {
    uint32_t op;
    uint32_t pid;
    uint64_t addr;
    uint64_t size;
    uint8_t data[];
};

bool kpm_probe();

} // namespace ext
} // namespace bkl

#endif // BKL_EXT_MEMBACKEND_H
