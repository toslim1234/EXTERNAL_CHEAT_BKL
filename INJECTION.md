# Injection & stealth — how BKLKILL now runs

Target: `com.dts.freefiremax`. Payload: `libbklkill.so`. Injector: `libbkliject.so`
(a PIE executable that is only named `.so` so the APK ships it).

## Chain

```
app (untrusted_app, uid A)
  │  su -c
  ▼
bkliject (uid 0)
  │  0. wait until the game is up and libil2cpp.so is mapped (45 s, BKL_WAIT=secs)
  │  1. /proc scan → game pid
  │  2. copy payload → /data/data/com.dts.freefiremax/code_cache/.tmp_xxxxxxxx
  │  3. chown game_uid, chmod 0600
  │  4. ptrace ATTACH → GETREGSET → save regs
  │  5. remote mmap(0x20000, RW) inside the game
  │  6. write payload path into that mapping
  │  7. resolve dlopen / android_dlopen_ext in the game's libdl.so
  │  8. call it (svc gadget + lr=0 trap), wait for the SIGSEGV stop
  │  9. SETREGSET → DETACH with signal 0 (fault swallowed)
  │ 10. unlink the staged file  →  maps shows "… (deleted)"
  ▼
game process (untrusted_app, uid B)
  └─ libbklkill.so constructor
       └─ thread "Job.Worker": 250 ms delay → environment_clean() → run_server(true)
            ├─ abstract unix socket (XOR-encoded name, no plain text in the .so)
            ├─ cheat threads (SilentAim, Fly, AimCollider, AimHard, PullEnemy, DiveKill, Rotate, Teleport)
            └─ memory access via process_vm_readv/writev against own pid
```

No daemon, no `su` process left alive, no payload file on disk after step 10.

## Staying alive

The overlay process would be the first thing the low-memory killer takes once the
game is in front. `WorkerService` is a minimal foreground service (`IMPORTANCE_MIN`,
empty title/text, `START_STICKY`) started before injection, so the process survives
while the game runs. Android 13+ may hide the notification entirely when
`POST_NOTIFICATIONS` was never granted; the service keeps running either way.

## What is hidden, and what is not

Hidden:

* payload mapping appears as `/data/data/com.dts.freefiremax/code_cache/.tmp_xxxxxxxx (deleted)` — no file to read, no header to hash on disk;
* staged name is random per run, mode 0600, owner = game uid;
* injector wipes its own `argv`, sets `PR_SET_DUMPABLE=0`, `comm` = `Job.Worker`, and writes nothing to logcat;
* payload build compiles every `LOGx` in `Server.cpp` to a no-op (`BKL_VERBOSE_LOG=0`);
* socket name is rebuilt at runtime from XOR bytes (`Socket/Const.h`), so `strings` shows nothing;
* payload thread is named `Job.Worker`;
* payload refuses to start when Frida is present: maps scan (`frida`, `gum-js`, `linjector`, `re.frida.server`) and probes on TCP 27042 / 27043;
* payload waits up to 2 s for `TracerPid` to clear (the injector detaches right after `dlopen`) and then gives up quietly.

Not hidden — do not pretend otherwise:

* the mapping still exists inside the process; a kernel driver or an `/proc/<pid>/maps` check that follows the deleted path can see a suspicious anonymous region;
* the game can still see the abstract socket in `/proc/net/unix` (name looks like a system service);
* nothing here defeats a kernel-level anti-cheat (KPM / hardware breakpoint detection);
* the APK still ships `libbklkill.so` and `libbkliject.so` as assets of the cheat app itself. If that APK is on the device, it is evidence.

## Build

```
./gradlew assembleDebug          # or the GitHub workflow (single pass now)
```

`app/src/main/jni/Android.mk` builds four modules:

| module | output | role |
|---|---|---|
| `cnative` | `libcnative.so` | overlay client |
| `bklkill` | `libbklkill.so` | injected payload (`-DBKL_PAYLOAD`) |
| `BKLKILL` | `BKLKILL` | legacy root daemon, not packaged |
| `bkliject` | `libbkliject.so` | injector executable |

`gen_bklkill.sh` and `bkliject_bin.h` are gone: the injector is shipped as a file in
the APK, extracted at runtime, executed, then deleted. No two-pass build.

## Use

The **Execute** button on the login screen runs `Initialize`:

1. launches the game;
2. pulls `lib/arm64-v8a/libbkliject.so` and `lib/arm64-v8a/libbklkill.so` out of the APK into the app cache;
3. `su` → `magiskpolicy --live` rules (ptrace + exec from `app_data_file` / `shell_data_file`);
4. runs the injector, reads `inject_rc=` from stdout, deletes both extracted files;
5. opens the overlay only when `inject_rc=0`.

Debug output: `adb shell setprop` is not used — set the boolean `BKL_DEBUG` in the
`BKL_PREFS` SharedPreferences and the injector runs with `BKL_DEBUG=1`, printing each
step.

## Exit codes

| rc | meaning |
|---|---|
| 0 | payload loaded |
| 2 | bad arguments |
| 3 | payload missing / < 4 KB |
| 5 | game process not running, or libil2cpp.so never appeared within the wait window |
| 6 | `PTRACE_ATTACH` failed (SELinux or ptrace_scope) |
| 7 | attach wait failed |
| 8 | `GETREGSET` failed |
| 9 | no `svc #0 ; ret` gadget in an executable mapping |
| 10 | remote `mmap` call did not return |
| 11 | remote `mmap` returned an error |
| 12 | could not write the path into the target |
| 13 | `dlopen` not resolvable in the target's `libdl.so` |
| 14 | `dlopen` call did not return |
| 15 | `dlopen` returned null (see `dlerror` text with `BKL_DEBUG=1`) |
| 90 | injector file missing after extraction |
| 126/127 | exec denied from cache → retried once from `/data/local/tmp` |

## Known risks (runtime, not tested here)

* Nothing in this repo could be built or executed on the device — no NDK. Only host
  compile checks with a stub `jni.h` were run.
* Hijacking the main thread means `dlopen` runs on the UI thread; if that thread is in
  an ART JNI-critical section the game can abort. If that shows up, attach to a
  secondary tid from `/proc/<pid>/task` instead.
* Bionic on Android 12+ may resolve `dlopen` through `libdl.so` trampolines; the
  dynamic-section symbol walk covers `DT_SYMTAB`/`DT_STRTAB`, with the old
  section-header walk as fallback.
* Re-injecting into the same process twice fails at `bind()` (socket already exists);
  the constructor guard prevents a double boot in one process.
