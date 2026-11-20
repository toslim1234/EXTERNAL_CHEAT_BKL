#include "Ipc.h"

#include "Engine.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace bkl {
namespace ext {

namespace {

std::vector<std::string> tokens(const std::string &line) {
    std::vector<std::string> out;
    std::istringstream is(line);
    std::string t;
    while (is >> t) out.push_back(t);
    return out;
}

bool parse_u64(const std::string &s, uint64_t &out) {
    if (s.empty()) return false;
    char *end = nullptr;
    unsigned long long v = strtoull(s.c_str(), &end, 0);
    if (end == s.c_str()) return false;
    out = (uint64_t) v;
    return true;
}

bool parse_double(const std::string &s, double &out) {
    if (s.empty()) return false;
    char *end = nullptr;
    out = strtod(s.c_str(), &end);
    return end != s.c_str();
}

std::string hexdump(uint64_t addr, const std::vector<uint8_t> &bytes) {
    std::string out;
    char line[160];
    for (size_t i = 0; i < bytes.size(); i += 16) {
        snprintf(line, sizeof(line), "%016" PRIx64 "  ", addr + i);
        out += line;
        for (size_t k = 0; k < 16; ++k) {
            if (i + k < bytes.size()) {
                snprintf(line, sizeof(line), "%02x ", bytes[i + k]);
            } else {
                snprintf(line, sizeof(line), "   ");
            }
            out += line;
        }
        out += " |";
        for (size_t k = 0; k < 16 && i + k < bytes.size(); ++k) {
            uint8_t c = bytes[i + k];
            out.push_back((c >= 0x20 && c < 0x7f) ? (char) c : '.');
        }
        out += "|\n";
    }
    return out;
}

std::string cmd_scan(const std::vector<std::string> &t, bool range) {
    if (t.size() < 3) return "ERR usage: SCAN <type> <value> [module|all]";
    ValueType type;
    if (!parse_value_type(t[1], type)) return "ERR bad type";
    if (range && t.size() < 4) return "ERR usage: SCANR <type> <lo> <hi> [module|all]";

    double lo = 0, hi = 0;
    if (range) {
        if (!parse_double(t[2], lo) || !parse_double(t[3], hi)) return "ERR bad range";
    } else {
        if (!parse_double(t[2], lo)) return "ERR bad value";
        hi = lo;
    }

    const bool module_only = !(t.size() > (range ? 4u : 3u) && t[range ? 4 : 3] == "all");

    Engine &e = Engine::instance();
    if (!e.active()) return "ERR not attached";

    ScanOptions opt = e.default_scan_options(module_only, 4096);
    if (opt.ranges.empty()) return "ERR no scan range";

    size_t n = range ? e.scanner().scan_range(type, lo, hi, opt)
                     : e.scanner().scan_value(type, lo, opt);

    char out[128];
    snprintf(out, sizeof(out), "OK hits=%zu range=%s%s", n, module_only ? "module" : "all",
             e.scanner().truncated() ? " truncated=1(time_budget)" : "");
    return out;
}

std::string cmd_hits(const std::vector<std::string> &t) {
    size_t max = 32;
    if (t.size() > 1) {
        uint64_t v = 0;
        if (parse_u64(t[1], v)) max = (size_t) v;
    }
    Engine &e = Engine::instance();
    std::vector<Hit> hits = e.scanner().hits();
    std::string out;
    char line[96];
    snprintf(line, sizeof(line), "OK hits=%zu\n", hits.size());
    out += line;
    for (size_t i = 0; i < hits.size() && i < max; ++i) {
        snprintf(line, sizeof(line), "%016" PRIx64 "  %.6f\n", hits[i].addr, hits[i].value);
        out += line;
    }
    if (hits.size() > max) out += "...\n";
    return out;
}

std::string cmd_freeze(const std::vector<std::string> &t) {
    Engine &e = Engine::instance();
    if (t.size() < 2) return "ERR usage: FREEZE add|del|list|clear ...";

    if (t[1] == "list") {
        auto items = e.freezer().list();
        std::string out;
        char line[64];
        snprintf(line, sizeof(line), "OK freeze=%zu\n", items.size());
        out += line;
        for (const std::string &s : items) out += s + "\n";
        return out;
    }
    if (t[1] == "clear") {
        e.freezer().clear();
        return "OK cleared";
    }
    if (t.size() < 5) return "ERR usage: FREEZE add <addr> <type> <value> | FREEZE del <addr>";
    uint64_t addr = 0;
    if (!parse_u64(t[2], addr)) return "ERR bad address";

    if (t[1] == "del") {
        return e.freezer().remove(addr) ? "OK removed" : "ERR not found";
    }
    if (t[1] == "add") {
        ValueType type;
        double value = 0;
        if (!parse_value_type(t[3], type)) return "ERR bad type";
        if (!parse_double(t[4], value)) return "ERR bad value";
        return e.freezer().add(addr, type, value) ? "OK frozen" : "ERR freeze list full";
    }
    return "ERR unknown FREEZE subcommand";
}

std::string cmd_aob(const std::vector<std::string> &t) {
    if (t.size() < 2) return "ERR usage: AOB <pattern> [max]";
    // Re-join everything after the command so unquoted patterns with spaces work;
    // the optional trailing "max=<n>" keeps the argument list unambiguous.
    std::string pattern = t[1];
    size_t max = 8;
    for (size_t i = 2; i < t.size(); ++i) {
        if (t[i].rfind("max=", 0) == 0) {
            max = (size_t) strtoul(t[i].c_str() + 4, nullptr, 0);
        } else {
            pattern += " " + t[i];
        }
    }

    Pattern p;
    if (!parse_pattern(pattern, p)) return "ERR bad pattern";

    Engine &e = Engine::instance();
    uint64_t s = 0, end = 0;
    if (!module_range(e.pid(), e.module().c_str(), s, end)) return "ERR module not mapped";

    std::vector<uint64_t> found;
    e.patcher().find(s, end, p, max, found);

    std::string out;
    char line[64];
    snprintf(line, sizeof(line), "OK found=%zu\n", found.size());
    out += line;
    for (uint64_t a : found) {
        snprintf(line, sizeof(line), "%016" PRIx64 "\n", a);
        out += line;
    }
    return out;
}

std::string cmd_maps(const std::vector<std::string> &t) {
    Engine &e = Engine::instance();
    if (!e.active()) return "ERR not attached";
    const char *filter = t.size() > 1 ? t[1].c_str() : nullptr;
    std::string out;
    for (const MapRegion &r : read_maps(e.pid())) {
        if (filter && r.path.find(filter) == std::string::npos) continue;
        char line[400];
        snprintf(line, sizeof(line), "%016" PRIx64 "-%016" PRIx64 " %c%c%c %s\n",
                 r.start, r.end,
                 r.readable() ? 'r' : '-', r.writable() ? 'w' : '-',
                 r.executable() ? 'x' : '-', r.path.c_str());
        out += line;
        if (out.size() > 12 * 1024) {
            out += "...\n";
            break;
        }
    }
    return out.empty() ? std::string("OK no match") : out;
}

} // namespace

const char *command_help() {
    return "STATUS | ATTACH [pkg] | BASE [module] | MAPS [filter]\n"
           "READ <addr> <len> | WRITE <addr> <hex>\n"
           "SCAN <type> <value> [module|all] | SCANR <type> <lo> <hi> [module|all]\n"
           "REFINE exact <value> | REFINE changed|unchanged|increased|decreased\n"
           "HITS [max] | WHITS <value> | CLEAR\n"
           "FREEZE add <addr> <type> <value> | FREEZE del <addr> | FREEZE list | FREEZE clear\n"
           "AOB <pattern> [max=n] | PATCH <addr> <hex> | RESTORE <addr>";
}

std::string handle_command(const std::string &line) {
    std::vector<std::string> t = tokens(line);
    if (t.empty()) return "ERR empty";

    Engine &e = Engine::instance();
    const std::string &cmd = t[0];

    if (cmd == "HELP") return command_help();

    if (cmd == "STATUS") return e.status();

    if (cmd == "ATTACH") {
        bool ok = e.attach(t.size() > 1 ? t[1].c_str() : nullptr);
        return ok ? ("OK " + e.status()) : std::string("ERR attach failed");
    }

    if (cmd == "BASE") {
        uint64_t b = e.base(t.size() > 1 ? t[1].c_str() : nullptr);
        char out[64];
        snprintf(out, sizeof(out), "0x%" PRIx64, b);
        return out;
    }

    if (cmd == "MAPS") return cmd_maps(t);

    if (cmd == "READ") {
        uint64_t addr = 0, len = 0;
        if (t.size() < 3 || !parse_u64(t[1], addr) || !parse_u64(t[2], len))
            return "ERR usage: READ <addr> <len>";
        if (len == 0 || len > 4096) return "ERR len 1..4096";
        std::vector<uint8_t> buf(len);
        ssize_t n = e.read(addr, buf.data(), buf.size());
        if (n <= 0) return "ERR read failed";
        buf.resize((size_t) n);
        return hexdump(addr, buf);
    }

    if (cmd == "WRITE") {
        uint64_t addr = 0;
        if (t.size() < 3 || !parse_u64(t[1], addr)) return "ERR usage: WRITE <addr> <hex>";
        std::string hex = t[2];
        for (size_t i = 3; i < t.size(); ++i) hex += t[i];
        std::vector<uint8_t> bytes;
        if (!parse_hex_bytes(hex, bytes)) return "ERR bad hex";
        if (e.write(addr, bytes.data(), bytes.size()) != (ssize_t) bytes.size())
            return "ERR write failed";
        return "OK written";
    }

    if (cmd == "SCAN") return cmd_scan(t, false);
    if (cmd == "SCANR") return cmd_scan(t, true);

    if (cmd == "REFINE") {
        if (t.size() < 2) return "ERR usage: REFINE <mode> [value]";
        RefineMode mode;
        if (!parse_refine_mode(t[1], mode)) return "ERR bad mode";
        double value = 0, tol = 0;
        if (mode == RefineMode::Exact) {
            if (t.size() < 3 || !parse_double(t[2], value)) return "ERR value required";
            if (t.size() > 3) parse_double(t[3], tol);
        }
        char out[96];
        snprintf(out, sizeof(out), "OK hits=%zu", e.scanner().refine(mode, value, tol));
        return out;
    }

    if (cmd == "HITS") return cmd_hits(t);

    if (cmd == "WHITS") {
        double v = 0;
        if (t.size() < 2 || !parse_double(t[1], v)) return "ERR value required";
        return e.scanner().write_hits(v) ? "OK written" : "ERR partial write";
    }

    if (cmd == "CLEAR") {
        e.scanner().clear();
        return "OK cleared";
    }

    if (cmd == "FREEZE") return cmd_freeze(t);

    if (cmd == "AOB") return cmd_aob(t);

    if (cmd == "PATCH") {
        uint64_t addr = 0;
        if (t.size() < 3 || !parse_u64(t[1], addr)) return "ERR usage: PATCH <addr> <hex>";
        std::string hex = t[2];
        for (size_t i = 3; i < t.size(); ++i) hex += t[i];
        std::vector<uint8_t> bytes;
        if (!parse_hex_bytes(hex, bytes)) return "ERR bad hex";
        std::string err;
        return e.patcher().apply(addr, bytes, err) ? "OK patched" : ("ERR " + err);
    }

    if (cmd == "RESTORE") {
        uint64_t addr = 0;
        if (t.size() < 2 || !parse_u64(t[1], addr)) return "ERR usage: RESTORE <addr>";
        std::string err;
        return e.patcher().restore(addr, err) ? "OK restored" : ("ERR " + err);
    }

    if (cmd == "DETACH") {
        e.detach();
        return "OK detached";
    }

    return "ERR unknown command (try HELP)";
}

} // namespace ext
} // namespace bkl
