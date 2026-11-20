#include "Patcher.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace bkl {
namespace ext {

static void normalise(std::string &s) {
    // Strip the decorators people paste from disassemblers: "\x48\x8b",
    // "0x48,0x8b", "48-8b".
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            ++i;
            continue;
        }
        if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            ++i;
            continue;
        }
        if (c == ' ' || c == '\t' || c == ',' || c == '-' || c == '|') continue;
        out.push_back(c);
    }
    s.swap(out);
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_pattern(const std::string &text, Pattern &out) {
    std::string s = text;
    normalise(s);
    out.bytes.clear();
    out.mask.clear();
    if (s.empty()) return false;

    for (size_t i = 0; i < s.size();) {
        if (s[i] == '?') {
            // "??" or "?"
            size_t skip = (i + 1 < s.size() && s[i + 1] == '?') ? 2 : 1;
            out.bytes.push_back(0);
            out.mask.push_back(0);
            i += skip;
            continue;
        }
        if (i + 1 >= s.size()) return false;
        int hi = hex_digit(s[i]);
        int lo = hex_digit(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.bytes.push_back((uint8_t) ((hi << 4) | lo));
        out.mask.push_back(1);
        i += 2;
    }
    return !out.bytes.empty();
}

bool parse_hex_bytes(const std::string &text, std::vector<uint8_t> &out) {
    std::string s = text;
    normalise(s);
    out.clear();
    if (s.empty() || (s.size() & 1)) return false;
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = hex_digit(s[i]);
        int lo = hex_digit(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back((uint8_t) ((hi << 4) | lo));
    }
    return !out.empty();
}

size_t Patcher::find(uint64_t start, uint64_t end, const Pattern &p, size_t max,
                     std::vector<uint64_t> &out) {
    out.clear();
    if (!io_ || p.bytes.empty() || end <= start) return 0;

    const size_t plen = p.bytes.size();
    const size_t chunk = 256 * 1024;
    std::vector<uint8_t> buf(chunk + plen);

    for (uint64_t addr = start; addr < end && out.size() < max;) {
        size_t want = (size_t) std::min<uint64_t>(chunk, end - addr);
        ssize_t n = io_->read(addr, buf.data(), want);
        if (n <= 0) {
            addr += 0x1000;
            continue;
        }
        size_t usable = (size_t) n;
        if (usable < plen) break;

        for (size_t off = 0; off + plen <= usable; ++off) {
            bool hit = true;
            for (size_t k = 0; k < plen; ++k) {
                if (p.mask[k] && buf[off + k] != p.bytes[k]) {
                    hit = false;
                    break;
                }
            }
            if (hit) {
                out.push_back(addr + off);
                if (out.size() >= max) break;
            }
        }
        if (usable < want) {
            addr += usable + 0x1000;
        } else {
            // Overlap so a match straddling the chunk boundary is not missed.
            addr += usable > (plen - 1) ? usable - (plen - 1) : usable;
            if (usable <= plen - 1) addr += 0x1000;
        }
    }
    return out.size();
}

bool Patcher::apply(uint64_t addr, const std::vector<uint8_t> &bytes, std::string &err) {
    if (!io_) {
        err = "no backend";
        return false;
    }
    if (bytes.empty()) {
        err = "empty patch";
        return false;
    }

    std::lock_guard<std::mutex> lk(m_);
    auto it = originals_.find(addr);
    if (it == originals_.end()) {
        std::vector<uint8_t> prev(bytes.size());
        if (io_->read(addr, prev.data(), prev.size()) != (ssize_t) prev.size()) {
            err = "cannot read original bytes";
            return false;
        }
        originals_[addr] = Original{prev};
    }
    if (io_->write(addr, bytes.data(), bytes.size()) != (ssize_t) bytes.size()) {
        err = "write failed";
        return false;
    }
    return true;
}

bool Patcher::restore(uint64_t addr, std::string &err) {
    if (!io_) {
        err = "no backend";
        return false;
    }
    std::lock_guard<std::mutex> lk(m_);
    auto it = originals_.find(addr);
    if (it == originals_.end()) {
        err = "nothing to restore";
        return false;
    }
    if (io_->write(addr, it->second.bytes.data(), it->second.bytes.size()) !=
        (ssize_t) it->second.bytes.size()) {
        err = "write failed";
        return false;
    }
    originals_.erase(it);
    return true;
}

size_t Patcher::apply_all(const std::vector<uint64_t> &addrs,
                          const std::vector<uint8_t> &bytes, std::string &err) {
    size_t ok = 0;
    for (uint64_t a : addrs) {
        std::string e;
        if (apply(a, bytes, e)) {
            ++ok;
        } else if (err.empty()) {
            err = e;
        }
    }
    return ok;
}

size_t Patcher::patch_count() const {
    std::lock_guard<std::mutex> lk(m_);
    return originals_.size();
}

} // namespace ext
} // namespace bkl
