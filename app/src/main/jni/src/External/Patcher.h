// Byte-pattern scanner / patcher: the external-mode version of Zytron's
// "[Patch] ..." engine (AOB patterns with wildcards, applied through the
// selected memory backend).
#ifndef BKL_EXT_PATCHER_H
#define BKL_EXT_PATCHER_H

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Scanner.h"

namespace bkl {
namespace ext {

struct Pattern {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;   // 1 = compare, 0 = wildcard
    bool empty() const { return bytes.empty(); }
};

// Accepts "48 8B ?? 0F", "48 8b ? 0f", "488b??0f" and "\x48\x8b".
bool parse_pattern(const std::string &text, Pattern &out);

// Accepts "90 90 90" / "909090" / "0x90,0x90".
bool parse_hex_bytes(const std::string &text, std::vector<uint8_t> &out);

class Patcher {
public:
    void set_io(MemIO *io) { io_ = io; }

    // Scans [start, end) in chunks, returns the first `max` matches.
    size_t find(uint64_t start, uint64_t end, const Pattern &p, size_t max,
                std::vector<uint64_t> &out);

    // Writes `bytes` at `addr`, remembering the previous content for restore().
    bool apply(uint64_t addr, const std::vector<uint8_t> &bytes, std::string &err);

    bool restore(uint64_t addr, std::string &err);

    size_t apply_all(const std::vector<uint64_t> &addrs,
                     const std::vector<uint8_t> &bytes, std::string &err);

    size_t patch_count() const;

private:
    struct Original {
        std::vector<uint8_t> bytes;
    };

    MemIO *io_ = nullptr;
    mutable std::mutex m_;
    std::map<uint64_t, Original> originals_;
};

} // namespace ext
} // namespace bkl

#endif // BKL_EXT_PATCHER_H
