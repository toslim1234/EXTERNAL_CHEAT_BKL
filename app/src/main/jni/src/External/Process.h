// Target process discovery and map parsing for external (no-injection) mode.
#ifndef BKL_EXT_PROCESS_H
#define BKL_EXT_PROCESS_H

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace bkl {
namespace ext {

struct MapRegion {
    uint64_t start = 0;
    uint64_t end = 0;
    int prot = 0;          // PROT_* bits
    uint64_t offset = 0;
    std::string path;

    uint64_t size() const { return end > start ? end - start : 0; }
    bool readable() const;
    bool writable() const;
    bool executable() const;
};

// First pid whose /proc/<pid>/cmdline starts with the package name.
pid_t find_pid(const char *package);

bool is_alive(pid_t pid);

std::vector<MapRegion> read_maps(pid_t pid);

// Load bias of the first mapping of `name` (start - file offset), 0 if absent.
uint64_t module_base(pid_t pid, const char *name);

// [start, end) covered by every mapping of `name`; false if absent.
bool module_range(pid_t pid, const char *name, uint64_t &start, uint64_t &end);

// Basename of a maps path, stripping " (deleted)".
std::string map_basename(const std::string &path);

} // namespace ext
} // namespace bkl

#endif // BKL_EXT_PROCESS_H
