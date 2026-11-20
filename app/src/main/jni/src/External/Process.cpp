#include "Process.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

namespace bkl {
namespace ext {

bool MapRegion::readable() const { return (prot & 0x1) != 0; }

bool MapRegion::writable() const { return (prot & 0x2) != 0; }

bool MapRegion::executable() const { return (prot & 0x4) != 0; }

std::string map_basename(const std::string &path) {
    static const char kDeleted[] = " (deleted)";
    std::string p = path;
    size_t d = p.rfind(kDeleted);
    if (d != std::string::npos) p.erase(d);
    size_t slash = p.rfind('/');
    if (slash != std::string::npos) p.erase(0, slash + 1);
    return p;
}

static bool read_small_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = ::read(fd, buf, cap - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    // cmdline is NUL separated; keep only the first token
    for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') break;
        if (buf[i] == '\n') { buf[i] = '\0'; break; }
    }
    return true;
}

pid_t find_pid(const char *package) {
    if (!package || !*package) return -1;

    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *e;
        char path[64], cmd[256];
        while ((e = readdir(d)) != nullptr) {
            if (!isdigit((unsigned char) e->d_name[0])) continue;
            snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
            if (!read_small_file(path, cmd, sizeof(cmd))) continue;
            if (strcmp(cmd, package) != 0) continue;
            pid_t pid = (pid_t) atoi(e->d_name);
            closedir(d);
            return pid;
        }
        closedir(d);
    }

    // Fallback for kernels with hidepid where /proc/<pid> of a foreign uid is
    // still readable through pidof (root only).
    char line[64];
    snprintf(line, sizeof(line), "pidof %s", package);
    FILE *p = popen(line, "r");
    if (p) {
        char out[32] = {0};
        if (fgets(out, sizeof(out), p)) {
            pid_t pid = (pid_t) atoi(out);
            pclose(p);
            if (pid > 0) return pid;
        }
        pclose(p);
    }
    return -1;
}

bool is_alive(pid_t pid) {
    if (pid <= 0) return false;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    char buf[512];
    if (!read_small_file(path, buf, sizeof(buf))) return false;
    // Field 3 is the state; 'Z' means a zombie that is about to disappear.
    const char *close_paren = strrchr(buf, ')');
    if (!close_paren || close_paren[1] != ' ') return true;
    return close_paren[2] != 'Z';
}

std::vector<MapRegion> read_maps(pid_t pid) {
    std::vector<MapRegion> out;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "re");
    if (!f) return out;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        MapRegion r;
        unsigned long long s = 0, e = 0, off = 0;
        char perms[8] = {0};
        char dev[16] = {0};
        unsigned long inode = 0;
        int consumed = 0;

        // 7c-1a3e4000-7c-1a41c000 r-xp 00000000 fd:01 1234   /system/lib64/libc.so
        if (sscanf(line, "%llx-%llx %7s %llx %15s %lu %n",
                   &s, &e, perms, &off, dev, &inode, &consumed) < 6) {
            continue;
        }
        r.start = s;
        r.end = e;
        r.offset = off;
        r.prot = 0;
        if (perms[0] == 'r') r.prot |= 0x1;
        if (perms[1] == 'w') r.prot |= 0x2;
        if (perms[2] == 'x') r.prot |= 0x4;
        if (consumed > 0 && (size_t) consumed < strlen(line)) {
            const char *p = line + consumed;
            while (*p == ' ' || *p == '\t') ++p;
            size_t len = strlen(p);
            while (len && (p[len - 1] == '\n' || p[len - 1] == '\r')) --len;
            r.path.assign(p, len);
        }
        out.push_back(std::move(r));
    }
    fclose(f);
    return out;
}

uint64_t module_base(pid_t pid, const char *name) {
    if (!name) return 0;
    uint64_t best = 0;
    for (const MapRegion &r : read_maps(pid)) {
        if (map_basename(r.path) != name) continue;
        uint64_t bias = r.start - r.offset;
        if (best == 0 || bias < best) best = bias;
    }
    return best;
}

bool module_range(pid_t pid, const char *name, uint64_t &start, uint64_t &end) {
    if (!name) return false;
    bool found = false;
    for (const MapRegion &r : read_maps(pid)) {
        if (map_basename(r.path) != name) continue;
        if (!found) {
            start = r.start;
            end = r.end;
            found = true;
        } else {
            if (r.start < start) start = r.start;
            if (r.end > end) end = r.end;
        }
    }
    return found;
}

} // namespace ext
} // namespace bkl
