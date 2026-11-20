// bkliject — root-side arm64 ELF injector.
//
// Loads the payload shared object inside an already running target process
// (com.dts.freefiremax) by hijacking one of its threads with ptrace and calling the
// remote dlopen()/android_dlopen_ext().
//
// Usage:  <injector> <package> <payload.so>
//
// Requires: root + SELinux permission to ptrace the target (the caller installs
//           `allow su untrusted_app process ptrace` live rules).
//
// Stealth properties, on purpose:
//   * runs from a memfd or a private dir and wipes its own argv;
//   * PR_SET_DUMPABLE=0, thread name replaced, nothing written to logcat;
//   * payload is staged under the target's own code_cache with a random innocuous
//     name and UNLINKED right after dlopen, so /proc/<pid>/maps shows the mapping
//     as "(deleted)" and no payload file survives on disk;
//   * thread registers/stack are restored and the process is detached, so no
//     debugger or extra process stays behind.
//
// Exit codes are the only output. Set BKL_DEBUG=1 to get a step-by-step trace.

#include <android/log.h>
#include <asm/ptrace.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

static bool g_verbose = false;

static void ilog(const char *fmt, ...) {
    if (!g_verbose) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stdout, "%s\n", buf);
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// remote memory access
// ---------------------------------------------------------------------------

static bool vm_read(pid_t pid, uint64_t addr, void *buf, size_t len) {
    if (!addr || !len) return false;
    struct iovec local{buf, len};
    struct iovec remote{reinterpret_cast<void *>(addr), len};
    long n = syscall(SYS_process_vm_readv, pid, &local, 1UL, &remote, 1UL, 0UL);
    return n == static_cast<long>(len);
}

static bool vm_write(pid_t pid, uint64_t addr, const void *buf, size_t len) {
    if (!addr || !len) return false;
    struct iovec local{const_cast<void *>(buf), len};
    struct iovec remote{reinterpret_cast<void *>(addr), len};
    long n = syscall(SYS_process_vm_writev, pid, &local, 1UL, &remote, 1UL, 0UL);
    return n == static_cast<long>(len);
}

static std::string read_cstr(pid_t pid, uint64_t addr, size_t maxlen = 512) {
    if (!addr) return "";
    std::string out;
    char chunk[64];
    while (out.size() < maxlen) {
        size_t want = std::min(sizeof(chunk), maxlen - out.size());
        if (!vm_read(pid, addr + out.size(), chunk, want)) break;
        size_t n = strnlen(chunk, want);
        out.append(chunk, n);
        if (n < want) break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// /proc/<pid>/maps
// ---------------------------------------------------------------------------

struct MapEntry {
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t offset = 0;
    char perms[5] = {0};
    std::string path;
};

static std::vector<MapEntry> read_maps(pid_t pid) {
    std::vector<MapEntry> out;
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/maps", pid);
    FILE *f = fopen(p, "re");
    if (!f) return out;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        MapEntry e;
        char perms[8] = {0};
        char dev[16] = {0};
        unsigned long long start = 0, end = 0, off = 0, inode = 0;
        int pathoff = 0;
        int got = sscanf(line, "%llx-%llx %7s %llx %15s %llu %n",
                         &start, &end, perms, &off, dev, &inode, &pathoff);
        if (got < 6) continue;
        e.start = start;
        e.end = end;
        e.offset = off;
        memcpy(e.perms, perms, 4);
        if (pathoff > 0 && pathoff < (int)strlen(line)) {
            std::string rest(line + pathoff);
            while (!rest.empty() && (rest.back() == '\n' || rest.back() == ' ')) rest.pop_back();
            e.path = rest;
        }
        out.push_back(e);
    }
    fclose(f);
    return out;
}

static uint64_t module_base(const std::vector<MapEntry> &maps, const char *name) {
    for (const auto &m : maps) {
        if (m.offset != 0) continue;
        if (m.path.empty()) continue;
        if (m.path.size() < strlen(name)) continue;
        if (m.path.compare(m.path.size() - strlen(name), strlen(name), name) == 0) return m.start;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ELF dynamic symbol lookup (section headers are present on Android system libs)
// ---------------------------------------------------------------------------

// Preferred path: walk PT_DYNAMIC. DT_SYMTAB / DT_STRTAB are already runtime
// addresses, so this does not depend on section headers being mapped.
static uint64_t elf_lookup_dynamic(pid_t pid, uint64_t base, const char *want) {
    Elf64_Ehdr eh{};
    if (!vm_read(pid, base, &eh, sizeof(eh))) return 0;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) return 0;
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_machine != EM_AARCH64) return 0;
    if (!eh.e_phnum || eh.e_phnum > 128 || !eh.e_phoff) return 0;

    std::vector<Elf64_Phdr> phs(eh.e_phnum);
    if (!vm_read(pid, base + eh.e_phoff, phs.data(), phs.size() * sizeof(Elf64_Phdr))) return 0;

    uint64_t dyn = 0;
    size_t dynsz = 0;
    for (const auto &p : phs) {
        if (p.p_type == PT_DYNAMIC) {
            dyn = base + p.p_vaddr;
            dynsz = p.p_memsz;
            break;
        }
    }
    if (!dyn || !dynsz) return 0;

    size_t dcount = std::min<size_t>(dynsz / sizeof(Elf64_Dyn), 512);
    std::vector<Elf64_Dyn> ds(dcount);
    if (!vm_read(pid, dyn, ds.data(), dcount * sizeof(Elf64_Dyn))) return 0;

    uint64_t symtab = 0, strtab = 0, hash = 0;
    for (const auto &d : ds) {
        if (d.d_tag == DT_NULL) break;
        if (d.d_tag == DT_SYMTAB) symtab = d.d_un.d_ptr;
        else if (d.d_tag == DT_STRTAB) strtab = d.d_un.d_ptr;
        else if (d.d_tag == DT_HASH) hash = d.d_un.d_ptr;
    }
    if (!symtab || !strtab) return 0;

    // Relocated modules store absolute addresses; keep relative ones usable too.
    if (symtab < base) symtab += base;
    if (strtab < base) strtab += base;

    size_t count = 4096;
    if (hash) {
        uint32_t hdr[2] = {0, 0};
        if (vm_read(pid, hash, hdr, sizeof(hdr)) && hdr[1] && hdr[1] <= 65536) count = hdr[1];
    }

    const size_t kBatch = 256;
    std::vector<Elf64_Sym> batch(kBatch);
    for (size_t i = 0; i < count; i += kBatch) {
        size_t n = std::min(kBatch, count - i);
        if (!vm_read(pid, symtab + i * sizeof(Elf64_Sym), batch.data(), n * sizeof(Elf64_Sym)))
            break;
        for (size_t k = 0; k < n; ++k) {
            const Elf64_Sym &s = batch[k];
            if (!s.st_name || !s.st_value) continue;
            char name[192] = {0};
            if (!vm_read(pid, strtab + s.st_name, name, sizeof(name) - 1)) continue;
            if (strcmp(name, want) == 0) return base + s.st_value;
        }
    }
    return 0;
}

// Fallback: section headers (works because .dynsym/.dynstr live in the first
// LOAD segment, which is mapped at file offset 0).
static uint64_t elf_lookup(pid_t pid, uint64_t base, const char *want) {
    Elf64_Ehdr eh{};
    if (!vm_read(pid, base, &eh, sizeof(eh))) return 0;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) return 0;
    if (eh.e_ident[EI_CLASS] != ELFCLASS64) return 0;
    if (eh.e_machine != EM_AARCH64) return 0;
    if (!eh.e_shoff || !eh.e_shnum || eh.e_shnum > 512) return 0;

    std::vector<Elf64_Shdr> shs(eh.e_shnum);
    if (!vm_read(pid, base + eh.e_shoff, shs.data(), shs.size() * sizeof(Elf64_Shdr))) return 0;

    for (const auto &sh : shs) {
        if (sh.sh_type != SHT_DYNSYM || sh.sh_entsize != sizeof(Elf64_Sym)) continue;
        if (sh.sh_link >= shs.size()) continue;
        const Elf64_Shdr &strh = shs[sh.sh_link];
        if (!strh.sh_offset || !strh.sh_size) continue;

        size_t count = sh.sh_size / sizeof(Elf64_Sym);
        if (!count || count > 65536) continue;
        std::vector<Elf64_Sym> syms(count);
        if (!vm_read(pid, base + sh.sh_offset, syms.data(), count * sizeof(Elf64_Sym))) continue;

        for (const auto &s : syms) {
            if (!s.st_name || s.st_value == 0) continue;
            if (s.st_name >= strh.sh_size) continue;
            char name[256] = {0};
            if (!vm_read(pid, base + strh.sh_offset + s.st_name, name, sizeof(name) - 1)) continue;
            if (strcmp(name, want) == 0) return base + s.st_value;
        }
    }
    return 0;
}

static uint64_t resolve_remote(pid_t pid, const std::vector<MapEntry> &maps, const char *lib,
                               const char *sym) {
    uint64_t base = module_base(maps, lib);
    if (!base) return 0;
    uint64_t addr = elf_lookup_dynamic(pid, base, sym);
    if (!addr) addr = elf_lookup(pid, base, sym);
    return addr;
}

// ---------------------------------------------------------------------------
// registers / remote call
// ---------------------------------------------------------------------------

struct Remote {
    pid_t pid = -1;
    user_pt_regs saved{};
    uint64_t scratch = 0;      // remote anonymous mapping
    uint64_t scratch_sp = 0;   // stack pointer used while calling
    uint64_t gadget = 0;       // `svc #0 ; ret` inside the target
    bool attached = false;
};

static bool get_regs(pid_t pid, user_pt_regs *out) {
    struct iovec iov{out, sizeof(*out)};
    return ptrace(PTRACE_GETREGSET, pid, reinterpret_cast<void *>(static_cast<long>(NT_PRSTATUS)),
                  &iov) == 0;
}

static bool set_regs(pid_t pid, const user_pt_regs *r) {
    struct iovec iov{const_cast<user_pt_regs *>(r), sizeof(*r)};
    return ptrace(PTRACE_SETREGSET, pid, reinterpret_cast<void *>(static_cast<long>(NT_PRSTATUS)),
                  &iov) == 0;
}

static bool wait_for_stop(pid_t pid, int *status, int timeout_ms) {
    for (int i = 0; i < timeout_ms; ++i) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            *status = st;
            return true;
        }
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        usleep(1000);
    }
    return false;
}

// Calls fn(a0..a5) on the hijacked thread. Return value lands in x0. The thread is
// pointed at `pc = 0` as a landing pad, so a SIGSEGV is expected; it is swallowed.
static bool call_remote(Remote &R, uint64_t fn, uint64_t x8, const uint64_t args[6],
                        uint64_t *ret) {
    user_pt_regs r = R.saved;
    for (int i = 0; i < 6; ++i) r.regs[i] = args[i];
    r.regs[8] = x8;
    r.pc = fn;
    r.regs[30] = 0;  // lr -> trap
    r.sp = R.scratch_sp;

    if (!set_regs(R.pid, &r)) return false;
    if (ptrace(PTRACE_CONT, R.pid, nullptr, nullptr) != 0) return false;

    int st = 0;
    if (!wait_for_stop(R.pid, &st, 15000)) {
        ilog("timeout waiting for remote call at 0x%llx", (unsigned long long)fn);
        return false;
    }

    user_pt_regs after{};
    bool ok = get_regs(R.pid, &after);
    if (ok) *ret = after.regs[0];

    // always restore, whatever happened
    set_regs(R.pid, &R.saved);
    return ok;
}

static bool remote_syscall(Remote &R, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t *ret) {
    uint64_t args[6] = {a0, a1, a2, a3, a4, a5};
    return call_remote(R, R.gadget, nr, args, ret);
}

// ---------------------------------------------------------------------------
// gadget search: `svc #0 ; ret`
// ---------------------------------------------------------------------------

static uint64_t find_syscall_gadget(pid_t pid, const std::vector<MapEntry> &maps) {
    std::vector<uint8_t> buf(64 * 1024);
    for (const auto &m : maps) {
        if (m.perms[2] != 'x' || !m.start || m.end <= m.start) continue;
        for (uint64_t addr = m.start; addr + 8 <= m.end; addr += buf.size()) {
            size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), m.end - addr));
            if (!vm_read(pid, addr, buf.data(), want)) break;
            for (size_t i = 0; i + 8 <= want; i += 4) {
                uint32_t w0 = 0, w1 = 0;
                memcpy(&w0, &buf[i], 4);
                memcpy(&w1, &buf[i + 4], 4);
                if (w0 == 0xd4000001u && w1 == 0xd65f03c0u) return addr + i;
            }
            if (want < buf.size()) break;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// payload staging
// ---------------------------------------------------------------------------

static std::string read_cmdline_argv0(pid_t pid) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/cmdline", pid);
    FILE *f = fopen(p, "re");
    if (!f) return "";
    char buf[512] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!n) return "";
    std::string s(buf, strnlen(buf, n));
    return s;
}

static int read_target_uid(pid_t pid) {
    char p[64];
    snprintf(p, sizeof(p), "/proc/%d/status", pid);
    FILE *f = fopen(p, "re");
    if (!f) return -1;
    char line[512];
    int uid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            uid = atoi(line + 4);
            break;
        }
    }
    fclose(f);
    return uid;
}

static bool copy_file(const std::string &src, const std::string &dst, mode_t mode) {
    int in = open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        close(in);
        return false;
    }
    char buf[64 * 1024];
    bool ok = true;
    for (;;) {
        ssize_t rd = read(in, buf, sizeof(buf));
        if (rd == 0) break;
        if (rd < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        ssize_t off = 0;
        while (off < rd) {
            ssize_t wr = write(out, buf + off, rd - off);
            if (wr < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            off += wr;
        }
        if (!ok) break;
    }
    if (ok) { fchmod(out, mode); fsync(out); }
    close(out);
    close(in);
    return ok;
}

static unsigned rnd32() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned v = static_cast<unsigned>(ts.tv_nsec) ^
                 (static_cast<unsigned>(ts.tv_sec) * 2654435761u) ^
                 (static_cast<unsigned>(getpid()) << 16);
    v ^= v << 13;
    v ^= v >> 17;
    v ^= v << 5;
    return v;
}

// The target must be able to mmap the payload under its own SELinux domain, so the
// file is created inside its code_cache: the label (app_data_file + categories) is
// inherited from the directory and the owner is the game uid. The name is random and
// deliberately boring; it is removed again right after dlopen().
static std::string stage_payload(pid_t pid, const std::string &src) {
    std::string pkg = read_cmdline_argv0(pid);
    size_t cut = pkg.find(':');
    if (cut != std::string::npos) pkg.resize(cut);
    if (pkg.empty() || pkg[0] == '[') {
        ilog("cannot determine package from cmdline, injecting from original path");
        return src;
    }

    int uid = read_target_uid(pid);
    if (uid < 0) {
        ilog("cannot read target uid");
        return src;
    }

    std::string dir = "/data/data/" + pkg + "/code_cache";
    mkdir(dir.c_str(), 0771);
    struct stat st{};
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        ilog("no code_cache for %s, injecting from original path", pkg.c_str());
        return src;
    }

    char suffix[16];
    snprintf(suffix, sizeof(suffix), "%08x", rnd32());
    std::string dst = dir + "/.tmp_" + suffix;

    // 0600 + chown to the game uid: only the game (and root) can even see it, and it
    // disappears a few milliseconds after the linker has mapped it.
    if (!copy_file(src, dst, 0600)) {
        ilog("copy failed (%s) -> injecting from original path", strerror(errno));
        return src;
    }
    if (chown(dst.c_str(), uid, uid) != 0) {
        ilog("chown failed: %s", strerror(errno));
    }
    ilog("staged payload: %s (uid %d)", dst.c_str(), uid);
    return dst;
}

// ---------------------------------------------------------------------------
// main flow
// ---------------------------------------------------------------------------

static int find_target_pid(const std::string &pkg) {
    DIR *d = opendir("/proc");
    if (!d) {
        ilog("opendir /proc failed: %s", strerror(errno));
        return -1;
    }
    int found = 0;
    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int p = atoi(de->d_name);
        if (p <= 1) continue;
        std::string cmd = read_cmdline_argv0(p);
        size_t colon = cmd.find(':');
        if (colon != std::string::npos) cmd.resize(colon);
        if (cmd == pkg) {
            found = p;
            break;
        }
    }
    closedir(d);
    return found;
}

// Runs with the target already attached and R.saved populated.
static int inject_payload(Remote &R, const std::string &remote_payload) {
    const pid_t pid = R.pid;
    std::vector<MapEntry> maps = read_maps(pid);

    R.gadget = find_syscall_gadget(pid, maps);
    if (!R.gadget) {
        ilog("no `svc #0; ret` gadget found in target");
        return 9;
    }
    ilog("svc gadget @ 0x%llx", (unsigned long long)R.gadget);

    uint64_t ret = 0;
    if (!remote_syscall(R, 222 /* __NR_mmap */, 0, 0x20000, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, static_cast<uint64_t>(-1), 0, &ret)) {
        ilog("remote mmap call failed");
        return 10;
    }
    if (static_cast<int64_t>(ret) < 0) {
        ilog("remote mmap returned -%lld", (long long)(-static_cast<int64_t>(ret)));
        return 11;
    }
    R.scratch = ret;
    R.scratch_sp = ret + 0x18000;  // 96 KB of stack, strings live near the base
    ilog("scratch @ 0x%llx", (unsigned long long)R.scratch);

    const uint64_t path_addr = R.scratch + 0x1000;
    if (!vm_write(pid, path_addr, remote_payload.c_str(), remote_payload.size() + 1)) {
        ilog("writing payload path into target failed");
        return 12;
    }

    const uint64_t dlopen_addr = resolve_remote(pid, maps, "libdl.so", "dlopen");
    const uint64_t dlerror_addr = resolve_remote(pid, maps, "libdl.so", "dlerror");
    const uint64_t aopen_addr = resolve_remote(pid, maps, "libdl.so", "android_dlopen_ext");
    ilog("dlopen=0x%llx dlerror=0x%llx android_dlopen_ext=0x%llx",
         (unsigned long long)dlopen_addr, (unsigned long long)dlerror_addr,
         (unsigned long long)aopen_addr);

    if (!dlopen_addr) {
        ilog("could not resolve dlopen in target");
        return 13;
    }

    uint64_t handle = 0;
    const uint64_t dl_args[6] = {path_addr, 2 /* RTLD_NOW */, 0, 0, 0, 0};
    if (!call_remote(R, dlopen_addr, 0, dl_args, &handle)) {
        ilog("remote dlopen call failed");
        return 14;
    }

    if (!handle && aopen_addr) {
        // android_dlopen_ext(path, flags, &extinfo{...}) — zeroed extinfo = plain dlopen
        const uint64_t ext = R.scratch + 0x800;
        uint64_t zero[8] = {0};
        vm_write(pid, ext, zero, sizeof(zero));
        const uint64_t ext_args[6] = {path_addr, 2, ext, 0, 0, 0};
        call_remote(R, aopen_addr, 0, ext_args, &handle);
    }

    if (!handle) {
        std::string err = "unknown";
        if (dlerror_addr) {
            uint64_t e = 0;
            const uint64_t none[6] = {0, 0, 0, 0, 0, 0};
            if (call_remote(R, dlerror_addr, 0, none, &e) && e) err = read_cstr(pid, e);
        }
        ilog("dlopen failed: %s", err.c_str());
        return 15;
    }

    ilog("INJECTED: handle=0x%llx", (unsigned long long)handle);

    if (g_verbose) {
        std::string base = remote_payload.substr(remote_payload.rfind('/') + 1);
        bool seen = false, deleted = false;
        for (const auto &m : read_maps(pid)) {
            if (m.path.find(base) != std::string::npos) {
                seen = true;
                if (m.path.find("(deleted)") != std::string::npos) deleted = true;
            }
        }
        ilog("payload in maps: %s%s", seen ? "yes" : "no", deleted ? " (deleted)" : "");
    }
    return 0;
}

// Runs before any argument is used for anything else, so `ps` and
// /proc/<pid>/cmdline show an empty command line.
static void stealth_self(char **argv) {
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);          // no core dump, no /proc/<pid>/mem for others
    prctl(PR_SET_NAME, "Job.Worker", 0, 0, 0);   // boring comm name
    if (argv && argv[0]) {
        size_t n = 0;
        for (char **p = argv; *p; ++p) n += strlen(*p) + 1;
        memset(argv[0], 0, n);
    }
}

// Waits until the package is running *and* libil2cpp.so is mapped: injecting before
// the Unity runtime exists would load the payload into a process that has no offsets
// to work with. Cold start on a slow phone can take 30 s+.
static int wait_for_target(const std::string &pkg, int timeout_ms) {
    const int step = 500;
    for (int waited = 0; waited <= timeout_ms; waited += step) {
        const int pid = find_target_pid(pkg);
        if (pid > 0) {
            for (const auto &m : read_maps(pid)) {
                if (m.path.find("libil2cpp.so") != std::string::npos) return pid;
            }
        }
        usleep(step * 1000);
    }
    return -1;
}

int main(int argc, char **argv) {
    g_verbose = getenv("BKL_DEBUG") != nullptr;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <package> <payload>\n", argv[0]);
        return 2;
    }
    const std::string pkg = argv[1];
    const std::string payload = argv[2];

    stealth_self(argv);

    struct stat pst{};
    if (stat(payload.c_str(), &pst) != 0 || pst.st_size < 4096) {
        ilog("payload missing or truncated: %s", payload.c_str());
        return 3;
    }
    ilog("target=%s payload=%s (%lld bytes)", pkg.c_str(), payload.c_str(),
         (long long)pst.st_size);

    int timeout_ms = 45000;
    if (const char *w = getenv("BKL_WAIT")) {
        int secs = atoi(w);
        if (secs > 0 && secs <= 300) timeout_ms = secs * 1000;
    }

    const int pid = wait_for_target(pkg, timeout_ms);
    if (pid <= 0) {
        ilog("process %s is not running", pkg.c_str());
        return 5;
    }
    ilog("target pid=%d", pid);

    const std::string staged = stage_payload(pid, payload);
    const bool staged_here = staged != payload;

    Remote R;
    R.pid = pid;

    int rc = 6;
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) != 0) {
        ilog("PTRACE_ATTACH failed: %s", strerror(errno));
        if (staged_here) unlink(staged.c_str());
        return rc;
    }
    int st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFSTOPPED(st)) {
        ilog("attach wait failed");
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        if (staged_here) unlink(staged.c_str());
        return 7;
    }
    R.attached = true;
    ilog("attached (stop sig=%d)", WSTOPSIG(st));

    rc = 8;
    if (get_regs(pid, &R.saved)) {
        rc = inject_payload(R, staged);
    } else {
        ilog("GETREGSET failed: %s", strerror(errno));
    }

    set_regs(pid, &R.saved);
    if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) != 0) {
        ilog("PTRACE_DETACH failed: %s", strerror(errno));
    } else {
        ilog("detached");
    }

    // The mapping survives the unlink; only the path entry disappears. From here on
    // /proc/<pid>/maps shows "<code_cache>/.tmp_xxxxxxxx (deleted)" and no payload
    // file exists anywhere on the filesystem.
    if (staged_here) {
        if (unlink(staged.c_str()) != 0) {
            ilog("unlink failed: %s", strerror(errno));
        } else {
            ilog("staged payload removed from disk");
        }
    }
    return rc;
}
