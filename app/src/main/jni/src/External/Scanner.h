// Value scanner, range scanner and freeze list — the external-mode equivalent of
// the "BYTE / DWORD / QWORD / FLOAT / DOUBLE / RANGE" engine Zytron runs.
#ifndef BKL_EXT_SCANNER_H
#define BKL_EXT_SCANNER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bkl {
namespace ext {

enum class ValueType { I8, I16, I32, I64, F32, F64 };

size_t value_size(ValueType t);

bool parse_value_type(const std::string &s, ValueType &out);

const char *value_type_name(ValueType t);

enum class RefineMode { Exact, Changed, Unchanged, Increased, Decreased };

bool parse_refine_mode(const std::string &s, RefineMode &out);

struct Hit {
    uint64_t addr = 0;
    double value = 0.0;   // current
    double prev = 0.0;    // value at the previous scan
};

struct ScanRange {
    uint64_t start = 0;
    uint64_t end = 0;
};

struct ScanOptions {
    std::vector<ScanRange> ranges;  // empty -> every writable/anonymous readable map
    size_t max_hits = 4096;
    size_t chunk = 64 * 1024;
    // Hard wall-clock stop. A whole-address-space scan on a 1.8 GB phone can run
    // for minutes and the app would time out on the socket, so the walk returns
    // whatever it has collected so far instead of blocking forever.
    int time_budget_ms = 25000;
};

class MemIO {
public:
    virtual ~MemIO() = default;

    virtual ssize_t read(uint64_t addr, void *buf, size_t len) = 0;

    virtual ssize_t write(uint64_t addr, const void *buf, size_t len) = 0;
};

class Scanner {
public:
    void set_io(MemIO *io) { io_ = io; }

    // First scan: every location equal to `value` (or inside [lo, hi]).
    size_t scan_value(ValueType t, double value, const ScanOptions &opt);

    size_t scan_range(ValueType t, double lo, double hi, const ScanOptions &opt);

    // Follow-up scan over the current hit list.
    size_t refine(RefineMode mode, double value, double tolerance = 0.0);

    size_t count() const;

    // True when the last scan stopped because of its time budget rather than
    // because it finished the range. A truncated scan can report 0 hits even
    // though a value exists further on in the range.
    bool truncated() const { return truncated_.load(); }

    std::vector<Hit> hits() const;

    bool write_hits(double value);

    void clear();

private:
    size_t collect(ValueType t, const ScanOptions &opt,
                   const std::function<bool(double)> &accept);

    MemIO *io_ = nullptr;
    mutable std::mutex m_;
    std::atomic<bool> truncated_{false};
    std::vector<Hit> hits_;
    ValueType last_type_ = ValueType::I32;
};

// Rewrites a set of addresses every `period_ms`, independent of the scanner.
class Freezer {
public:
    ~Freezer();

    void set_io(MemIO *io) { io_ = io; }

    bool add(uint64_t addr, ValueType t, double value);

    bool remove(uint64_t addr);

    void clear();

    std::vector<std::string> list() const;

    void start(int period_ms = 50);

    void stop();

private:
    struct Entry {
        uint64_t addr;
        ValueType type;
        double value;
    };

    void loop(int period_ms);

    MemIO *io_ = nullptr;
    mutable std::mutex m_;
    std::vector<Entry> entries_;
    std::atomic<bool> run_{false};
    std::thread th_;
};

} // namespace ext
} // namespace bkl

#endif // BKL_EXT_SCANNER_H
