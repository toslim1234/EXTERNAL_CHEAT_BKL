#include "Scanner.h"
#include "Process.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>

namespace bkl {
namespace ext {

size_t value_size(ValueType t) {
    switch (t) {
        case ValueType::I8:  return 1;
        case ValueType::I16: return 2;
        case ValueType::I32: return 4;
        case ValueType::I64: return 8;
        case ValueType::F32: return 4;
        case ValueType::F64: return 8;
    }
    return 4;
}

const char *value_type_name(ValueType t) {
    switch (t) {
        case ValueType::I8:  return "i8";
        case ValueType::I16: return "i16";
        case ValueType::I32: return "i32";
        case ValueType::I64: return "i64";
        case ValueType::F32: return "f32";
        case ValueType::F64: return "f64";
    }
    return "i32";
}

bool parse_value_type(const std::string &s, ValueType &out) {
    if (s == "i8" || s == "byte" || s == "BYTE") { out = ValueType::I8; return true; }
    if (s == "i16" || s == "word" || s == "WORD") { out = ValueType::I16; return true; }
    if (s == "i32" || s == "dword" || s == "DWORD") { out = ValueType::I32; return true; }
    if (s == "i64" || s == "qword" || s == "QWORD") { out = ValueType::I64; return true; }
    if (s == "f32" || s == "float" || s == "FLOAT") { out = ValueType::F32; return true; }
    if (s == "f64" || s == "double" || s == "DOUBLE") { out = ValueType::F64; return true; }
    return false;
}

bool parse_refine_mode(const std::string &s, RefineMode &out) {
    if (s == "exact" || s == "eq") { out = RefineMode::Exact; return true; }
    if (s == "changed" || s == "ch") { out = RefineMode::Changed; return true; }
    if (s == "unchanged" || s == "un") { out = RefineMode::Unchanged; return true; }
    if (s == "increased" || s == "inc") { out = RefineMode::Increased; return true; }
    if (s == "decreased" || s == "dec") { out = RefineMode::Decreased; return true; }
    return false;
}

static double load_as(ValueType t, const uint8_t *p) {
    switch (t) {
        case ValueType::I8: {
            int8_t v;
            memcpy(&v, p, 1);
            return (double) v;
        }
        case ValueType::I16: {
            int16_t v;
            memcpy(&v, p, 2);
            return (double) v;
        }
        case ValueType::I32: {
            int32_t v;
            memcpy(&v, p, 4);
            return (double) v;
        }
        case ValueType::I64: {
            int64_t v;
            memcpy(&v, p, 8);
            return (double) v;
        }
        case ValueType::F32: {
            float v;
            memcpy(&v, p, 4);
            return (double) v;
        }
        case ValueType::F64: {
            double v;
            memcpy(&v, p, 8);
            return v;
        }
    }
    return 0.0;
}

static void store_from(ValueType t, uint8_t *p, double d) {
    switch (t) {
        case ValueType::I8: {
            int8_t v = (int8_t) (int64_t) llround(d);
            memcpy(p, &v, 1);
            return;
        }
        case ValueType::I16: {
            int16_t v = (int16_t) (int64_t) llround(d);
            memcpy(p, &v, 2);
            return;
        }
        case ValueType::I32: {
            int32_t v = (int32_t) (int64_t) llround(d);
            memcpy(p, &v, 4);
            return;
        }
        case ValueType::I64: {
            int64_t v = (int64_t) llround(d);
            memcpy(p, &v, 8);
            return;
        }
        case ValueType::F32: {
            float v = (float) d;
            memcpy(p, &v, 4);
            return;
        }
        case ValueType::F64: {
            memcpy(p, &d, 8);
            return;
        }
    }
}

static bool region_wanted(const MapRegion &r) {
    if (!r.readable() || r.size() == 0) return false;
    if (r.executable()) return false;                 // code, never holds a value
    if (!r.writable() && !r.path.empty() && r.path[0] != '[') return false;
    // Skip our own mappings and kernel/shared-memory specials.
    if (r.path.find("libcnative") != std::string::npos) return false;
    if (r.path.find("libbklkill") != std::string::npos) return false;
    if (r.path.find("/dev/") != std::string::npos) return false;
    if (r.path.find("[vvar]") != std::string::npos) return false;
    if (r.path.find("[vsyscall]") != std::string::npos) return false;
    return true;
}

size_t Scanner::collect(ValueType t, const ScanOptions &opt,
                        const std::function<bool(double)> &accept) {
    if (!io_) return 0;

    std::vector<ScanRange> ranges = opt.ranges;
    if (ranges.empty()) {
        // The caller normally passes the module range; without one we fall back
        // to every interesting mapping of the attached process.
        ranges.clear();
    }

    const size_t step = value_size(t);
    const size_t chunk = std::max<size_t>(opt.chunk, 4096);
    std::vector<uint8_t> buf(chunk + step);
    std::vector<Hit> found;
    found.reserve(std::min<size_t>(opt.max_hits, 4096));

    const auto started = std::chrono::steady_clock::now();
    bool out_of_time = false;

    for (const ScanRange &r : ranges) {
        uint64_t addr = r.start;
        while (addr < r.end && found.size() < opt.max_hits) {
            if (opt.time_budget_ms > 0) {
                auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count();
                if (spent >= opt.time_budget_ms) {
                    out_of_time = true;
                    break;
                }
            }
            size_t want = (size_t) std::min<uint64_t>(chunk, r.end - addr);
            ssize_t n = io_->read(addr, buf.data(), want);
            if (n <= 0) {
                addr += 0x1000;      // unreadable hole; skip a page
                continue;
            }
            size_t usable = (size_t) n;
            for (size_t off = 0; off + step <= usable; off += step) {
                double v = load_as(t, buf.data() + off);
                if (std::isnan(v) && t != ValueType::F32 && t != ValueType::F64) continue;
                if (!accept(v)) continue;
                Hit h;
                h.addr = addr + off;
                h.value = v;
                h.prev = v;
                found.push_back(h);
                if (found.size() >= opt.max_hits) break;
            }
            addr += usable;
            if (usable < want) addr += 0x1000;
        }
        if (out_of_time || found.size() >= opt.max_hits) break;
    }

    {
        std::lock_guard<std::mutex> lk(m_);
        hits_.swap(found);
        last_type_ = t;
    }
    truncated_.store(out_of_time);
    return count();
}

size_t Scanner::scan_value(ValueType t, double value, const ScanOptions &opt) {
    const double eps = (t == ValueType::F32) ? 1e-5 : (t == ValueType::F64 ? 1e-9 : 0.0);
    return collect(t, opt, [value, eps](double v) {
        if (eps > 0.0) return std::fabs(v - value) <= eps;
        return v == value;
    });
}

size_t Scanner::scan_range(ValueType t, double lo, double hi, const ScanOptions &opt) {
    if (hi < lo) std::swap(lo, hi);
    return collect(t, opt, [lo, hi](double v) { return v >= lo && v <= hi; });
}

size_t Scanner::refine(RefineMode mode, double value, double tolerance) {
    if (!io_) return 0;

    std::vector<Hit> kept;
    {
        std::lock_guard<std::mutex> lk(m_);
        kept.reserve(hits_.size());
        const size_t step = value_size(last_type_);
        std::vector<uint8_t> buf(step);

        for (const Hit &h : hits_) {
            if (io_->read(h.addr, buf.data(), step) != (ssize_t) step) continue;
            double cur = load_as(last_type_, buf.data());
            bool keep = false;
            switch (mode) {
                case RefineMode::Exact:
                    keep = (tolerance > 0.0) ? std::fabs(cur - value) <= tolerance
                                             : cur == value;
                    break;
                case RefineMode::Changed:
                    keep = cur != h.value;
                    break;
                case RefineMode::Unchanged:
                    keep = cur == h.value;
                    break;
                case RefineMode::Increased:
                    keep = cur > h.value;
                    break;
                case RefineMode::Decreased:
                    keep = cur < h.value;
                    break;
            }
            if (keep) kept.push_back(Hit{h.addr, cur, h.value});
        }
        hits_.swap(kept);
    }
    return count();
}

size_t Scanner::count() const {
    std::lock_guard<std::mutex> lk(m_);
    return hits_.size();
}

std::vector<Hit> Scanner::hits() const {
    std::lock_guard<std::mutex> lk(m_);
    return hits_;
}

bool Scanner::write_hits(double value) {
    if (!io_) return false;
    std::lock_guard<std::mutex> lk(m_);
    const size_t step = value_size(last_type_);
    std::vector<uint8_t> buf(step);
    store_from(last_type_, buf.data(), value);
    bool ok = true;
    for (Hit &h : hits_) {
        if (io_->write(h.addr, buf.data(), step) != (ssize_t) step) ok = false;
        else h.value = value;
    }
    return ok;
}

void Scanner::clear() {
    std::lock_guard<std::mutex> lk(m_);
    hits_.clear();
}

// ---------------------------------------------------------------------------

Freezer::~Freezer() { stop(); }

bool Freezer::add(uint64_t addr, ValueType t, double value) {
    std::lock_guard<std::mutex> lk(m_);
    for (Entry &e : entries_) {
        if (e.addr == addr) {
            e.type = t;
            e.value = value;
            return true;
        }
    }
    if (entries_.size() >= 256) return false;
    entries_.push_back(Entry{addr, t, value});
    return true;
}

bool Freezer::remove(uint64_t addr) {
    std::lock_guard<std::mutex> lk(m_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].addr == addr) {
            entries_.erase(entries_.begin() + i);
            return true;
        }
    }
    return false;
}

void Freezer::clear() {
    std::lock_guard<std::mutex> lk(m_);
    entries_.clear();
}

std::vector<std::string> Freezer::list() const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    char line[96];
    for (const Entry &e : entries_) {
        snprintf(line, sizeof(line), "%llx %s %.6f",
                 (unsigned long long) e.addr, value_type_name(e.type), e.value);
        out.emplace_back(line);
    }
    return out;
}

void Freezer::start(int period_ms) {
    if (run_.exchange(true)) return;
    th_ = std::thread(&Freezer::loop, this, period_ms);
}

void Freezer::stop() {
    if (!run_.exchange(false)) return;
    if (th_.joinable()) th_.join();
}

void Freezer::loop(int period_ms) {
    while (run_.load()) {
        {
            std::lock_guard<std::mutex> lk(m_);
            if (io_) {
                for (const Entry &e : entries_) {
                    uint8_t buf[8];
                    store_from(e.type, buf, e.value);
                    io_->write(e.addr, buf, value_size(e.type));
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
    }
}

} // namespace ext
} // namespace bkl
