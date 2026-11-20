package com.microsoft.xboxone.smartglass;

import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * Starts the game and injects the payload into its process.
 *
 * Two files are pulled out of our own APK: the payload (libbklkill.so) and the
 * injector (libbkliject.so, an executable that is only named .so so the APK ships
 * it). The injector runs once through `su`, copies the payload into the game's
 * code_cache, dlopens it there and unlinks it again. Nothing keeps running on the
 * side: after this method returns the only trace is inside the game process.
 *
 * Both extracted files are deleted from our cache afterwards, so a later scan of
 * our data directory finds no injection artifacts.
 */
public class Initialize {

    private static final String TAG = "CoreEngine";
    private static final String TARGET_PACKAGE = "com.dts.freefiremax";
    private static final String PAYLOAD_ENTRY = "lib/arm64-v8a/libbklkill.so";
    private static final String INJECTOR_ENTRY = "lib/arm64-v8a/libbkliject.so";
    private static final String DAEMON_ENTRY = "lib/arm64-v8a/libbklkilld.so";

    static {
        try {
            System.loadLibrary("cnative");
        } catch (Throwable ignored) {
            // already loaded elsewhere
        }
    }

    public Initialize(final Context context) {
        // Protect the process before the heavy work starts: without a foreground
        // service the system drops us as soon as the game asks for RAM.
        WorkerService.start(context.getApplicationContext());

        launchTarget(context);

        new Thread(new Runnable() {
            @Override
            public void run() {
                SharedPreferences prefs =
                        context.getSharedPreferences("BKL_PREFS", Context.MODE_PRIVATE);
                // External mode first: the engine runs as a separate root process and
                // reads the game through a memory backend, so nothing is injected.
                if (prefs.getBoolean("BKL_EXTERNAL", true) && startExternal(context, prefs)) {
                    Log.i(TAG, "external mode running");
                    postFloater(context);
                    return;
                }
                inject(context);
            }
        }, "CoreEngine").start();
    }

    private void postFloater(final Context context) {
        new Handler(Looper.getMainLooper()).post(new Runnable() {
            @Override
            public void run() {
                new Floater(context);
            }
        });
    }

    /**
     * Starts the standalone daemon (libbklkilld.so, an executable) through su and
     * leaves it running. No ptrace, no dlopen into the game: the daemon attaches to
     * the game through /dev/<node>, the kernel driver or /proc/<pid>/mem.
     */
    private boolean startExternal(Context context, SharedPreferences prefs) {
        try {
            String apk = context.getApplicationInfo().sourceDir;
            File daemon = new File(context.getCacheDir(), ".cache_d");
            if (extract(context, apk, DAEMON_ENTRY, ".cache_d") == null) {
                // Two harmless reasons: the APK does not ship the daemon, or a
                // previous instance still has the file mapped (Linux refuses to
                // open a running image for writing with ETXTBSY). Reuse the copy
                // that is already on disk instead of falling back to injection.
                if (!daemon.isFile()) {
                    Log.w(TAG, "daemon missing from apk and cache");
                    return false;
                }
                Log.i(TAG, "daemon file busy, reusing existing copy");
            }
            daemon.setExecutable(true, true);

            boolean debug = prefs.getBoolean("BKL_DEBUG", false);
            int rootMethod = prefs.getInt("ROOT_METHOD_INDEX", 0);
            LoginActivity.setKernelDriverMode(rootMethod == 3);

            String output = runAsRoot(buildDaemonScript(daemon.getAbsolutePath(), debug));
            if (debug) Log.d(TAG, output == null ? "no output" : output);
            return output != null && output.contains("daemon_ok=1");
        } catch (Throwable t) {
            Log.e(TAG, "external start failed: " + t.getMessage());
            return false;
        }
    }

    private String buildDaemonScript(String daemonPath, boolean debug) {
        StringBuilder s = new StringBuilder();

        String rules = "";
        for (String rule : ruleList()) {
            rules += " '" + rule + "'";
        }
        s.append("command -v magiskpolicy >/dev/null 2>&1 && magiskpolicy --live")
                .append(rules).append(" 2>/dev/null\n");
        s.append("command -v supolicy >/dev/null 2>&1 && supolicy --live")
                .append(rules).append(" 2>/dev/null\n");
        s.append("echo 0 > /proc/sys/kernel/yama/ptrace_scope 2>/dev/null\n");

        s.append("DAE='").append(daemonPath).append("'\n");
        s.append("[ -f \"$DAE\" ] || { echo daemon_ok=0; exit 0; }\n");
        s.append("chmod 700 \"$DAE\" 2>/dev/null\n");
        if (debug) {
            s.append("export BKL_DEBUG=1\n");
        }
        // One instance only: the abstract socket is exclusive, and a second copy
        // could only fail to bind. Match on the on-disk name so an already
        // running daemon is detected instead of spawning a doomed duplicate.
        s.append("if command -v pgrep >/dev/null 2>&1 && pgrep -f '[c]ache_d' >/dev/null 2>&1;")
                .append(" then echo daemon_ok=1; exit 0; fi\n");
        s.append("setsid \"$DAE\" >/dev/null 2>&1 &\n");
        s.append("echo daemon_ok=1\n");
        return s.toString();
    }

    private void launchTarget(Context context) {
        try {
            PackageManager pm = context.getPackageManager();
            Intent launchIntent = pm.getLaunchIntentForPackage(TARGET_PACKAGE);
            if (launchIntent != null) {
                context.startActivity(launchIntent);
            }
        } catch (Throwable t) {
            Log.w(TAG, "launch failed: " + t.getMessage());
        }
    }

    private void inject(Context context) {
        File injector = null;
        File payload = null;
        try {
            String apk = context.getApplicationInfo().sourceDir;
            injector = extract(context, apk, INJECTOR_ENTRY, ".cache_a");
            payload = extract(context, apk, PAYLOAD_ENTRY, ".cache_b");
            if (injector == null || payload == null) {
                Log.e(TAG, "payload missing from apk");
                return;
            }
            injector.setExecutable(true, true);

            SharedPreferences prefs =
                    context.getSharedPreferences("BKL_PREFS", Context.MODE_PRIVATE);
            boolean debug = prefs.getBoolean("BKL_DEBUG", false);
            int rootMethod = prefs.getInt("ROOT_METHOD_INDEX", 0);

            LoginActivity.setKernelDriverMode(rootMethod == 3);

            String script = buildScript(injector.getAbsolutePath(),
                    payload.getAbsolutePath(), debug);
            String output = runAsRoot(script);

            int rc = -1;
            if (output != null) {
                for (String line : output.split("\n")) {
                    if (line.startsWith("inject_rc=")) {
                        try {
                            rc = Integer.parseInt(line.substring(10).trim());
                        } catch (NumberFormatException ignored) {
                        }
                    }
                }
                if (debug) {
                    Log.d(TAG, output);
                }
            }
            if (rc != 0) {
                Log.w(TAG, "inject rc=" + rc);
            }

            if (rc == 0) {
                new Handler(Looper.getMainLooper()).post(new Runnable() {
                    @Override
                    public void run() {
                        new Floater(context);
                    }
                });
            }
        } catch (Throwable t) {
            Log.e(TAG, "inject failed: " + t.getMessage());
        } finally {
            if (injector != null) injector.delete();
            if (payload != null) payload.delete();
        }
    }

    /** Copies one entry out of our own APK into the private cache. */
    private File extract(Context context, String apkPath, String entryName, String outName) {
        File out = new File(context.getCacheDir(), outName);
        try (ZipFile zip = new ZipFile(apkPath)) {
            ZipEntry entry = zip.getEntry(entryName);
            if (entry == null) return null;
            try (InputStream in = zip.getInputStream(entry);
                 OutputStream os = new FileOutputStream(out)) {
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) {
                    os.write(buf, 0, n);
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "extract " + entryName + ": " + e.getMessage());
            return null;
        }
        out.setReadable(true, true);
        return out.length() > 4096 ? out : null;
    }

    private String buildScript(String injectorPath, String payloadPath, boolean debug) {
        StringBuilder s = new StringBuilder();

        // magiskpolicy/supolicy accept several statements in one call: on a low-end
        // phone 18 separate invocations cost seconds, one call costs ~100 ms.
        String rules = "";
        for (String rule : ruleList()) {
            rules += " '" + rule + "'";
        }
        s.append("command -v magiskpolicy >/dev/null 2>&1 && magiskpolicy --live")
                .append(rules).append(" 2>/dev/null\n");
        s.append("command -v supolicy >/dev/null 2>&1 && supolicy --live")
                .append(rules).append(" 2>/dev/null\n");
        s.append("echo 0 > /proc/sys/kernel/yama/ptrace_scope 2>/dev/null\n");

        s.append("INJ='").append(injectorPath).append("'\n");
        s.append("PAY='").append(payloadPath).append("'\n");
        s.append("[ -f \"$INJ\" ] || { echo inject_rc=90; exit 0; }\n");
        s.append("chmod 700 \"$INJ\" 2>/dev/null\n");

        if (debug) {
            s.append("export BKL_DEBUG=1\n");
        }

        // First try from our own cache. Some ROMs refuse to exec app_data_file even
        // for root, in which case the binary is mirrored once into /data/local/tmp.
        s.append("\"$INJ\" ").append(TARGET_PACKAGE).append(" \"$PAY\"\n");
        s.append("rc=$?\n");
        s.append("if [ $rc -eq 126 ] || [ $rc -eq 127 ]; then\n");
        s.append("  T=\"/data/local/tmp/.tmp_$$\"\n");
        s.append("  cp \"$INJ\" \"$T\" && chmod 700 \"$T\" && \"$T\" ")
                .append(TARGET_PACKAGE).append(" \"$PAY\"\n");
        s.append("  rc=$?\n");
        s.append("  rm -f \"$T\"\n");
        s.append("fi\n");
        s.append("rm -f \"$INJ\"\n");
        s.append("echo inject_rc=$rc\n");
        return s.toString();
    }

    private static String[] ruleList() {
        return new String[]{
                "allow su untrusted_app process ptrace",
                "allow su untrusted_app_32 process ptrace",
                "allow su untrusted_app_29 process ptrace",
                "allow su untrusted_app_30 process ptrace",
                "allow su priv_app process ptrace",
                "allow su app_data_file file { read write open getattr create unlink setattr execute execute_no_trans }",
                "allow su app_data_file dir { search write add_name remove_name open read getattr }",
                "allow su shell_data_file file { read write open getattr create unlink setattr execute execute_no_trans }",
                "allow su shell_data_file dir { search write add_name remove_name open read getattr }",
        };
    }

    private String runAsRoot(String script) {
        StringBuilder out = new StringBuilder();
        Process su = null;
        try {
            su = Runtime.getRuntime().exec("su");
            DataOutputStream stdin = new DataOutputStream(su.getOutputStream());
            stdin.writeBytes(script);
            stdin.writeBytes("exit\n");
            stdin.flush();
            stdin.close();

            BufferedReader reader =
                    new BufferedReader(new InputStreamReader(su.getInputStream()));
            String line;
            while ((line = reader.readLine()) != null) {
                out.append(line).append('\n');
            }
            reader.close();

            BufferedReader err =
                    new BufferedReader(new InputStreamReader(su.getErrorStream()));
            while ((line = err.readLine()) != null) {
                out.append(line).append('\n');
            }
            err.close();

            su.waitFor();
        } catch (Exception e) {
            out.append("su failed: ").append(e.getMessage()).append('\n');
        } finally {
            if (su != null) su.destroy();
        }
        return out.toString();
    }
}
