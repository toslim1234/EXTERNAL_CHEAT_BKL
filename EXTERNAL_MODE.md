# External mode — running the cheat without injecting into the game

This document describes the second way BKLKILL can drive Free Fire: instead of
`dlopen`-ing the payload inside the game process, a standalone root daemon
(`libbklkilld.so`, built from the `bklkilld` module) attaches to the game from
outside and reads/writes its memory through a kernel or procfs backend.

Both paths ship in the same APK. The app picks one at startup:

```
Initialize
 ├─ prefs BKL_EXTERNAL == true  → startExternal()  → bklkilld … → Floater
 └─ otherwise / on failure      → inject()         → libbkliject.so → payload
```

`startExternal()` returns `false` only if the daemon binary cannot be obtained
or `su` reports `daemon_ok=0`, so a phone with no usable backend still falls
back to the injection path instead of ending up with no menu.

---

## 1. Why no injection is needed

The injected payload owns the game's address space, so `Read<>`/`Write<>` are a
plain `memcpy` through a pointer. External mode has no such luxury: every access
is a syscall into a backend that knows how to reach another process's address
space. Five of them are implemented, tried in this order:

| Order | Backend id | Mechanism | Needs |
|---|---|---|---|
| 1 | `ioctl` | `ioctl(/dev/nh, …)` vendor kernel driver | root + `/dev/nh` |
| 2 | `dev` | vendor driver nodes (`/dev/wanbai`, `/dev/CheckMe`, `/dev/Ckanri`, `/dev/lanran`, `/dev/video188`, …) | root + node |
| 3 | `kpm` | KPM module: `prctl(0xdead, …)` then `prctl(0x4e41, op, msg, len, 0)` | root + KPM kernel module |
| 4 | `proc` | `/proc/<pid>/mem` with `pread`/`pwrite` | root (or `ptrace_scope=0` + same uid) |
| 5 | `vm` | `process_vm_readv`/`process_vm_writev` | `ptrace_scope=0` + same uid |

`auto` is the default; the first backend that attaches **and** passes the
verification probe wins. Verification reads 4 bytes at the target module base
and requires the ELF magic `\x7fELF`, so a backend that opens fine but returns
garbage is rejected rather than silently corrupting writes.

The KPM path mirrors what a KPM-based cheat does (see
`REVERSE_ENGINEERING/EXTERNAL_ROOT_DELTA_MOD_KPM_APK/ZYTRON PRO ROOT REA/ANALYSIS_ZYTRON.md`):
`prctl(0xdead, 0, 0, 0, 0)` registers the calling process as a KPM client, then
each transfer is one `prctl(0x4e41, op, &msg, sizeof(msg), 0)` with

```c
struct KpmMsg {          // 0x20 bytes header + payload
    uint32_t op;         // read or write op id
    uint32_t pid;
    uint64_t addr;
    uint64_t size;
    uint8_t  data[];     // up to 0x1000 bytes
};
```

**Caveat, stated plainly:** the numeric read/write op ids live inside the KPM
kernel module and are not recoverable from the APK, so they are not guessed.
The backend stays disabled unless both are supplied:

```
export BKL_KPM_OP_READ=0x…
export BKL_KPM_OP_WRITE=0x…
```

With either missing, `make_kpm()` reports not-ready and `auto` falls through to
`proc`/`vm`. Everything else in the KPM path (registration, message layout,
size cap, error handling) is implemented and used as soon as the ids are known.

---

## 2. Configuration

Read once at attach time, in this precedence order: environment, then config
file, then built-in default.

Config file: `/data/local/tmp/bkl_ext.cfg`, one `key=value` per line.

```ini
package=com.dts.freefiremax
module=libil2cpp.so
backend=auto
```

| Key | Env override | Default | Meaning |
|---|---|---|---|
| `package` | `BKL_PACKAGE` | `com.dts.freefiremax` | Game to attach to |
| `module` | `BKL_MODULE` | `libil2cpp.so` | Module used for base address and default scan range |
| `backend` | `BKL_BACKEND` | `auto` | `auto`, or a comma list such as `ioctl,proc` |
| — | `BKL_ATTACH_TIMEOUT` | `30` | Seconds to wait for the game pid (polled at 250 ms) |
| — | `BKL_DEV_NODES` | vendor list | Extra/first-choice `/dev` nodes for the `dev` backend |
| — | `BKL_KPM_OP_READ` | — | KPM read op id (disables KPM if unset) |
| — | `BKL_KPM_OP_WRITE` | — | KPM write op id (disables KPM if unset) |

Setting `backend=proc` pins `/proc/<pid>/mem` and skips driver probing entirely,
which is what you want while testing without a kernel module.

---

## 3. Behaviour after the game closes or restarts

The engine keeps working across restarts:

* A watchdog thread wakes every 3 s, checks `is_alive(pid)`, and on death drops
  the backend, clears the base and re-attaches (pid, backend, base, ELF probe).
* After 20 consecutive failed re-attach attempts it sleeps 5 s between rounds
  instead of hammering the kernel while the game is not running.
* `Server.cpp` does not exit if the first attach fails. It starts a detached
  retry thread that calls `attach()` every 5 s and updates `target_pid` /
  `libAddress` when it succeeds, so the app can connect before the game is up.
* Freeze entries and patch records survive a restart as *configuration*; their
  addresses are resolved again by the caller (scan / AOB) after re-attach.

---

## 4. Command interface

The app holds the existing abstract-socket connection. Binary frames keep
carrying `Request`/`Response`. A frame whose `Mode` is `250` carries one text
command instead, and the reply is a length-prefixed string frame:

```
app  → Request{ Mode = 250 }
app  → "<command line>\0-ish, length-prefixed>"
dmn  → "<reply string>"
```

From Java: `LoginActivity.extCommand("SCAN i32 100 module")`.
Under the hood `bkl::ext::handle_command()` parses the line.

| Command | Purpose |
|---|---|
| `STATUS` | pid, alive, backend + detail, base, module, hit/freeze/patch counts |
| `ATTACH [pkg]` | (Re)attach, optionally switching package |
| `BASE [module]` | Module base address (hex) |
| `MAPS [filter]` | Parsed `/proc/<pid>/maps`, filtered by substring |
| `READ <addr> <len>` | Hexdump, 1..4096 bytes |
| `WRITE <addr> <hex>` | Raw write, accepts `\x`, `0x`, comma/dash separators |
| `SCAN <type> <value> [module\|all]` | First scan, `type` ∈ `i8 i16 i32 i64 f32 f64` |
| `SCANR <type> <lo> <hi> [module\|all]` | Range scan (`f32`/`f64` mainly) |
| `REFINE exact <v> [tol]` | Keep hits equal to value (float tolerance optional) |
| `REFINE changed\|unchanged\|increased\|decreased` | Classic cheat-engine narrowing |
| `HITS [max]` | List current hits (`addr value`), default 32 |
| `WHITS <value>` | Write the same value to every hit |
| `CLEAR` | Drop the hit list |
| `FREEZE add <addr> <type> <value>` | Re-write a value every 50 ms |
| `FREEZE del <addr>` / `list` / `clear` | Manage the freeze list |
| `AOB <pattern> [max=n]` | Pattern scan inside the module; `??` wildcards |
| `PATCH <addr> <hex>` | Write bytes, remembering the original |
| `RESTORE <addr>` | Put the original bytes back |
| `DETACH` | Drop the backend and stop the freezer |
| `HELP` | Command list |

Scanning defaults: `module` restricts the window to the game module's mapped
range; `all` walks every readable, non-executable mapping (shared libraries and
anonymous heap), skipping filesystem-backed read-only regions that cannot hold
game state. Hits are capped at 4096 per scan and every scan has a 25 s
wall-clock budget — on a 1.8 GB phone a whole-address-space walk would otherwise
outlive the app's socket timeout. A scan that ran out of time reports
`truncated=1(time_budget)` so a 0-hit result is not mistaken for "value absent".

---

## 5. Java side

`ExternalEngine.java` wraps the command layer so UI code does not build strings
by hand. Every method is blocking — call it off the UI thread.

```java
long base = ExternalEngine.base("libil2cpp.so");
int hits  = ExternalEngine.scan(ExternalEngine.I32, 100, true);
int left  = ExternalEngine.refine("decreased");
if (left == 1) {
    ExternalEngine.Hit h = ExternalEngine.hits(1).get(0);
    ExternalEngine.freeze(h.address, ExternalEngine.I32, 100);
}
// pattern work
int patched = ExternalEngine.patchPattern("1F 20 03 D5 ?? ?? ?? ??", "1F 20 03 D5", 4);
```

Value helpers: `readInt`, `readBytes`, `writeInt`, `writeHits`, `hits`, `status`,
`maps`, `clearHits`, `unfreeze`, `freezeList`, `restore`. Everything else can go
through `ExternalEngine.command("...")` with the table above.

---

## 6. How the rest of the code sees it

`Includes/Utils.h` routes the project's `Read<T>` / `Write<T>` through the engine
when it is compiled with `-DBKL_EXTERNAL_ENGINE` and the engine is attached:

```cpp
#ifdef BKL_EXTERNAL_ENGINE
    if (bkl::ext::active())
        return bkl::ext::read(address, &buf, sizeof(T)) == sizeof(T) ? buf : T{};
#endif
```

In-process mode (`inProcess == true`, the injected payload) is untouched: it has
its own mappings and never calls the engine.

Build split (`jni/Android.mk`):

| Module | Sources | Macro | Role |
|---|---|---|---|
| `bklkilld` | `Server.cpp`, `Socket/server.cpp`, `src/External/*` | `BKL_EXTERNAL_ENGINE` | External daemon |
| `bklkill` | payload + game logic | `BKL_PAYLOAD` | Injected fallback |
| `cnative` | `Client.cpp`, widgets | — | App-side UI/JNI |

The payload build deliberately excludes `src/External/*` so the injected image
stays small; the daemon build excludes the widget/ESP code it can never use.

---

## 7. Verification status

Compile-checked on host for `aarch64-linux-android24` (`-fsyntax-only`), both
build variants:

* `src/External/{MemBackend,Process,Scanner,Patcher,Engine,Ipc}.cpp` — clean.
* `src/Server.cpp` with `-DBKL_EXTERNAL_ENGINE` and with `-DBKL_PAYLOAD` — clean.
* `src/Client.cpp` — clean apart from pre-existing nlohmann warnings.
* Java files are not compilable here (no Android SDK jar on device); they were
  checked for brace balance and symbol references only.

Not verified: runtime behaviour on a device. Backend availability, SELinux
policy for executing the daemon from the app cache, KPM op ids, and the actual
socket handshake are all unverified until this runs on the target phone.
