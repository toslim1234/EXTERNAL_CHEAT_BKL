package com.microsoft.xboxone.smartglass;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Thin Java wrapper over the external-mode engine that lives in the root daemon
 * (libbklkilld.so). Nothing is injected into the game: every call here is one
 * text command sent over the existing socket and answered by the daemon.
 *
 * <p>Replies start with {@code OK}, {@code ERR}, or are a bare value. All methods
 * are blocking; call them off the UI thread.
 */
public final class ExternalEngine {

    private ExternalEngine() {
    }

    /** Scanner value types, spelled exactly as the daemon expects them. */
    public static final String I8 = "i8";
    public static final String I16 = "i16";
    public static final String I32 = "i32";
    public static final String I64 = "i64";
    public static final String F32 = "f32";
    public static final String F64 = "f64";

    // ---------------------------------------------------------------- plumbing

    private static String raw(String command) {
        try {
            String reply = LoginActivity.extCommand(command);
            return reply == null ? "ERR null reply" : reply;
        } catch (Throwable t) {
            return "ERR " + t.getClass().getSimpleName() + ": " + t.getMessage();
        }
    }

    private static boolean ok(String reply) {
        return reply != null && reply.startsWith("OK");
    }

    /** Runs a command and returns the whole reply; use for STATUS / MAPS / HITS. */
    public static String command(String command) {
        return raw(command);
    }

    // ------------------------------------------------------------------ status

    /** {@code pid=… alive=… backend=… base=… hits=… freeze=… patches=…} */
    public static String status() {
        return raw("STATUS");
    }

    /** Blocking attach (waits for the game to appear, up to the daemon timeout). */
    public static boolean attach() {
        return ok(raw("ATTACH"));
    }

    public static boolean isAttached() {
        return status().contains("alive=1");
    }

    public static void detach() {
        raw("DETACH");
    }

    /** Base address of a module, 0 when it is not mapped. */
    public static long base(String module) {
        String reply = raw(module == null || module.isEmpty() ? "BASE" : "BASE " + module);
        return parseHex(reply);
    }

    /** {@code maps} filtered by substring; empty filter lists everything. */
    public static String maps(String filter) {
        return raw(filter == null || filter.isEmpty() ? "MAPS" : "MAPS " + filter);
    }

    // ------------------------------------------------------------------ memory

    /** Raw read; returns the hex dump, or null on failure. */
    public static String read(long address, int length) {
        String reply = raw(String.format(Locale.US, "READ 0x%x %d", address, length));
        return ok(reply) ? reply : null;
    }

    /** Reads one 32-bit word (little endian), 0 when the address is unreadable. */
    public static int readInt(long address) {
        byte[] b = readBytes(address, 4);
        if (b == null) return 0;
        return (b[0] & 0xff) | ((b[1] & 0xff) << 8) | ((b[2] & 0xff) << 16) | ((b[3] & 0xff) << 24);
    }

    /** Reads raw bytes from the hex dump, or null on failure. */
    public static byte[] readBytes(long address, int length) {
        String dump = read(address, length);
        if (dump == null) return null;
        List<Byte> bytes = new ArrayList<>();
        for (String line : dump.split("\n")) {
            String t = line.trim();
            if (t.length() < 16 + 2) continue;
            String[] parts = t.split("\\s+");
            if (parts.length < 2) continue;
            // parts[0] is the address, the rest are byte tokens until the ASCII column
            for (int i = 1; i < parts.length; i++) {
                String tok = parts[i];
                if (tok.length() != 2) continue;
                try {
                    bytes.add((byte) Integer.parseInt(tok, 16));
                } catch (NumberFormatException ignored) {
                    // ASCII column / padding
                }
            }
        }
        if (bytes.size() < length) return null;
        byte[] out = new byte[length];
        for (int i = 0; i < length; i++) out[i] = bytes.get(i);
        return out;
    }

    public static boolean write(long address, String hexBytes) {
        return ok(raw(String.format(Locale.US, "WRITE 0x%x %s", address, hexBytes)));
    }

    public static boolean writeInt(long address, int value) {
        // little endian, the only thing an ARM64 game ever uses
        String hex = String.format(Locale.US, "%02x%02x%02x%02x",
                value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff, (value >>> 24) & 0xff);
        return write(address, hex);
    }

    // ------------------------------------------------------------------ search

    /**
     * First scan.
     *
     * @param moduleOnly true scans only the game module (fast, usual case),
     *                   false walks every writable mapping (slow, capped)
     * @return number of hits, -1 on error
     */
    public static int scan(String type, double value, boolean moduleOnly) {
        String reply = raw(String.format(Locale.US, "SCAN %s %s %s", type,
                trimNumber(value), moduleOnly ? "module" : "all"));
        return ok(reply) ? parseHits(reply) : -1;
    }

    /** Range scan, mainly for float/double searches (speed, health, coordinates). */
    public static int scanRange(String type, double lo, double hi, boolean moduleOnly) {
        String reply = raw(String.format(Locale.US, "SCANR %s %s %s %s", type,
                trimNumber(lo), trimNumber(hi), moduleOnly ? "module" : "all"));
        return ok(reply) ? parseHits(reply) : -1;
    }

    /** Narrows the current hits: exact / changed / unchanged / increased / decreased. */
    public static int refine(String mode, double value) {
        String reply = raw(String.format(Locale.US, "REFINE %s %s", mode, trimNumber(value)));
        return ok(reply) ? parseHits(reply) : -1;
    }

    public static int refine(String mode) {
        String reply = raw("REFINE " + mode);
        return ok(reply) ? parseHits(reply) : -1;
    }

    /** One scan result: address plus the value found there. */
    public static final class Hit {
        public final long address;
        public final double value;

        Hit(long address, double value) {
            this.address = address;
            this.value = value;
        }

        @Override
        public String toString() {
            return String.format(Locale.US, "0x%x = %s", address, trimNumber(value));
        }
    }

    /** Current hits as {@code address value} pairs, at most {@code max}. */
    public static List<Hit> hits(int max) {
        List<Hit> out = new ArrayList<>();
        String reply = raw("HITS " + max);
        for (String line : reply.split("\n")) {
            String[] parts = line.trim().split("\\s+");
            if (parts.length == 2 && parts[0].length() >= 8) {
                try {
                    out.add(new Hit(Long.parseUnsignedLong(parts[0], 16),
                            Double.parseDouble(parts[1])));
                } catch (NumberFormatException ignored) {
                    // header line or truncation marker
                }
            }
        }
        return out;
    }

    /** Writes {@code value} to every current hit, then returns how many took it. */
    public static boolean writeHits(double value) {
        return ok(raw("WHITS " + trimNumber(value)));
    }

    public static void clearHits() {
        raw("CLEAR");
    }

    // ------------------------------------------------------------------ freeze

    public static boolean freeze(long address, String type, double value) {
        return ok(raw(String.format(Locale.US, "FREEZE add 0x%x %s %s", address, type,
                trimNumber(value))));
    }

    public static boolean unfreeze(long address) {
        return ok(raw(String.format(Locale.US, "FREEZE del 0x%x", address)));
    }

    public static String freezeList() {
        return raw("FREEZE list");
    }

    public static void clearFreeze() {
        raw("FREEZE clear");
    }

    // ----------------------------------------------------------------- patches

    /** Pattern scan inside the game module; patterns accept {@code ??} wildcards. */
    public static List<Long> findPattern(String pattern, int max) {
        List<Long> out = new ArrayList<>();
        String reply = raw("AOB " + pattern + " max=" + max);
        for (String line : reply.split("\n")) {
            String t = line.trim();
            if (t.length() >= 8 && t.matches("[0-9a-fA-F]+")) {
                try {
                    out.add(Long.parseUnsignedLong(t, 16));
                } catch (NumberFormatException ignored) {
                    // header line
                }
            }
        }
        return out;
    }

    public static boolean patch(long address, String hexBytes) {
        return ok(raw(String.format(Locale.US, "PATCH 0x%x %s", address, hexBytes)));
    }

    public static boolean restore(long address) {
        return ok(raw(String.format(Locale.US, "RESTORE 0x%x", address)));
    }

    /** Replace the first {@code count} matches of {@code pattern} with {@code hexBytes}. */
    public static int patchPattern(String pattern, String hexBytes, int count) {
        int done = 0;
        for (long addr : findPattern(pattern, count)) {
            if (patch(addr, hexBytes)) done++;
        }
        return done;
    }

    // ------------------------------------------------------------------ helpers

    private static String trimNumber(double v) {
        if (v == Math.rint(v) && !Double.isInfinite(v) && Math.abs(v) < 9e15) {
            return String.valueOf((long) v);
        }
        return String.format(Locale.US, "%.6f", v);
    }

    private static int parseHits(String reply) {
        int i = reply.indexOf("hits=");
        if (i < 0) return -1;
        int j = i + 5;
        int end = j;
        while (end < reply.length() && Character.isDigit(reply.charAt(end))) end++;
        try {
            return Integer.parseInt(reply.substring(j, end));
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static long parseHex(String reply) {
        if (reply == null) return 0;
        String t = reply.trim();
        int start = t.startsWith("0x") ? 2 : 0;
        try {
            return Long.parseUnsignedLong(t.substring(start), 16);
        } catch (NumberFormatException e) {
            return 0;
        }
    }
}
