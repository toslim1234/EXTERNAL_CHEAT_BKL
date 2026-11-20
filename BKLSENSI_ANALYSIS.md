# BKLSENSI_ANDROID_SRC — Deep Analysis Report

Target: `/storage/emulated/0/BKLSENSI_ANDROID_SRC`
Package: `com.microsoft.xboxone.smartglass` (Xbox SmartGlass er naam dhare masquerade)
Product name (runtime): **BYTE FORCE v2.1.3** / internal "BKL SENSI"
Build: AGP 8.2.1, compileSdk 34, minSdk 21, arm64-v8a only, NDK 24.0.8215888, `ndkBuild` (Android.mk, `-std=c++17`, `-Os`, `-s`, hidden visibility)
Git: `main` — 6 commits, working tree te 9 ta file modified/deleted (uncommitted)

---

## 1. Eti kichu — one-line verdict

Eta **injection-less, root-daemon based Free Fire cheat**: game process e kono `.so` inject hoy na. Ekta root daemon (`BKLKILL`) `process_vm_readv` / custom kernel driver diye Free Fire er memory pore, ESP + aim data hishab kore, ar abstract Unix socket diye overlay app ke pathay. Overlay app (`cnative`) shudhu draw kore ar toggle pathay.

Tai detection surface ta "game er vitore foreign module" na — rather **root process, abstract socket, /proc activity, SELinux policy change, ar memory write er game-state effect**.

---

## 2. Build pipeline (do-pass, embedded daemon)

```
app/src/main/jni/Android.mk
 ├── curl_static            PREBUILT_STATIC_LIBRARY (src/static/curl/<abi>/libcurl.a)
 ├── cnative                SHARED  (Client.cpp Namegun.cpp Socket/client.cpp MemfdHelper.cpp)
 └── BKLKILL                EXECUTABLE (Server.cpp Socket/server.cpp)  <-- daemon
```

Daemon ta APK te `.so` hishebe jay na. Tai:

1. Pass 1: `assembleDebug` → NDK `BKLKILL` executable build hoy (`app/build/.../obj/local/arm64-v8a/BKLKILL`).
2. `gen_bklkill.sh` → oi ELF ke `bklkill_bin.h` te byte-array kore (`unsigned char BKLKILL[]`, `BKLKILL_len`).
3. Pass 2: `assembleDebug` → `libcnative.so` er vitore daemon embed hoy.

Bortoman embedded daemon size: **442 600 bytes** (`bklkill_bin.h`, 2.7 MB source header).

### Bug B1 (build-breaking, already in your tree)
`gen_bklkill.sh` **deleted** in working tree (`git status: D gen_bklkill.sh`), kintu `.github/workflows/android-build.yml` er step "Regenerate bklkill_bin.h" e `sh gen_bklkill.sh` chole.
→ Ei deletion commit korle **GitHub Actions fail** hobe (`No such file or directory`). File ta HEAD e ache (`git show HEAD:gen_bklkill.sh`), tai `git checkout -- gen_bklkill.sh` diye ferot na dile CI bhangbe.

### Bug B2
Repo te `local.propertie` (missing `s`) naam e ekta **empty file** ache — useless. Asol `local.properties` alada ache, ignore kora.

### Note
`Server.cpp` change kore rebuild korle `gen_bklkill.sh` na chalale **purono daemon** embed thake — silent stale-daemon bug. CI te do-pass ache, local build e nei.

---

## 3. Runtime architecture

```
┌─────────────────────────────┐         ┌──────────────────────────────────┐
│ cheat app (UID u0_aXXX)     │         │ root daemon (UID 0, su domain)   │
│  MainActivity               │         │  /proc/self/fd/N  (memfd ELF)    │
│   └ LoginActivity (UI)      │         │  random 8-char comm name         │
│   └ Floater (overlay menu)  │         │                                  │
│   └ ESPView (Canvas 60fps)  │         │  threads:                        │
│  libcnative.so              │         │   SilentAimTh / AimHardTh        │
│   └ SocketClient ───────────┼── AF_UNIX abstract ──► SocketServer        │
│       request 20 B          │         │   AimColliderTh / PullEnemyTh    │
│       response ~11.2 KB     │         │   FlyHack / DiveKill / Rotate    │
│                             │         │   TeleportToMark                 │
└─────────────────────────────┘         │  read/write: process_vm_*        │
                                        │              or /dev/nh ioctl    │
                                        └──────────────┬───────────────────┘
                                                       │
                                            Free Fire (com.dts.freefiremax)
```

### 3.1 Daemon launch (`Initialize.java` + `MemfdHelper.cpp`)
1. `getDaemonMemFdPath()` (MemfdHelper.cpp) → `memfd_create(<random 8 char>, MFD_CLOEXEC)` → `write(fd, BKLKILL, BKLKILL_len)` → return `"/proc/<getpid()>/fd/<fd>|<name>"`.
2. `Initialize` (line 227–239) path ta split kore `daemonPath`, `daemonName` ney.
3. `su` shell e script:
   - `killall -9 <daemonName>`
   - `sleep 0.2`
   - root method onujayi `magiskpolicy --live "<rule>"` ba `supolicy --live "<rule>"`
   - `nohup /proc/<pid>/fd/<n> > /dev/null 2>&1 &`
4. `Runtime.exec("su")` → stdin e script → `exit`.

SELinux rules (line 244–255) — 10 ta, notably:
`allow su untrusted_app process ptrace`, `..._32/..._30/..._29`, `allow su priv_app process ptrace`, tarpor `file { read write open getattr }` — ar `allow su su file { execute execute_no_trans ... }`.
→ `su` domain ke `untrusted_app` er upor **ptrace + file rw** deya hoy. Eta-i `process_vm_readv` kaaj korar precondition.
Purono version e `setenforce 0` chilo; **active switch e seta comment kora** (valo — kom noisy).

Root method mapping (`ROOT_METHOD_INDEX`, auto-detect `su -v` output / `/data/adb/*`):
`0` Magisk, `1` KernelSU/APatch, `2` Emulator/Cloud, `3` Kernel Driver (`/dev/nh`), default → `setKernelDriverMode(false)`.

### 3.2 Socket protocol (`Socket/Const.h`)
- `AF_UNIX`, `SOCK_STREAM`, **abstract namespace** (`sun_path[0] = '\0'`, tarpor `strcpy`).
- Name: `"УБИЙЦА. ХД"` (Cyrillic, 18 byte UTF-8) — `sun_path` er baki ta zero, ar dui pashei `sizeof(sockaddr_un)` (110) length dey, tai name padded.
- Framing: `uint32 htonl(len)` + payload.
- **`Request` = 20 B** → `{int Mode; bool m_IsOn; int value; int ScreenWidth; int ScreenHeight;}`
- **`Response` = 11 216 B** → `{bool Success; int PlayerCount; int EnemiesInFront; int EnemiesBehind; PlayerData Players[100];}` (PlayerData = 112 B)

Traffic: every ESP frame = 20 B up + 11 216 B down. 60 FPS e ~**0.67 MB/s**. Plus **every toggle** (`SendBool`) ek ta full 11 KB response wait kore — UI thread e synchronous.

---

## 4. Memory access layer (`Includes/Utils.h`)

```
Read<T>/Write<T>
 ├── g_useKernelDriver && /dev/nh open ok  → ioctl(NH_READ/NH_WRITE, struct nh_rw{pid,addr,data,size})
 │        (fail hole g_useKernelDriver=false, fallback)
 └── else syscall(SYS_process_vm_readv/writev)  (arm64: 270/271)
```

- `FindPid()` line 65: `popen("pidof com.dts.freefiremax")` → `sh` spawn (detectable, ar ei jonno `sh` binary er upor depend kore).
- `my_open_mem()` line 201: `openat(AT_FDCWD, "/proc/<pid>/mem", O_RDWR)` — `WriteIO/WriteIO1` use kore (raw patch), kintu main path `process_vm_*`.
- `KernelDriver.h`: `/dev/nh`, `NH_READ = _IOWR('N',1,nh_rw)`, `NH_WRITE = _IOWR('N',2,nh_rw)`. `isLoaded()` shudhu `open("/dev/nh")` — kono version/magic check nei.
- `libAddress = FindCorrectLibBase()` (Server.cpp:234): `/proc/<pid>/maps` theke **prothom 3 ta `libil2cpp.so`** ELF-header-valid base ney, tarpor `Read<uintptr_t>(base + GameFacade_c) > 0x100000000` diye validate kore. Fail hole `elf2`.

### Bug B3 (logic)
`FindCorrectLibBase()` `currentOffsets` use kore, ar `currentOffsets` version packet (Mode 100) asar **age-i** set hoy default `FFMax` e. Tai **India build (FFMaxInd)** e daemon start er somoy `GameFacade_c` bhul offset e probe kore → `elf1/elf2/elf3` selection bhul hote pare. Version packet main loop er vitore ashe, oi shomoy `libAddress` already fixed.

---

## 5. Offsets & version handling

| Struct | GameFacade_c | Player_c | IsHighFPS |
|---|---|---|---|
| `FFMax` (Global) | `0xAD6DC18` | `0xAD74BB0` | `0x8924DA0` |
| `FFMaxInd` (India) | `0xAD68E18` | `0xAD6FDA8` | `0x8907638` |

Version switch: `setCurrentVersion("v2.133.x")` → `FFMaxInd`, else `FFMax`. Client theke `SendVersionToServer(index)` (Mode 100) ar `setGameVersionNative()`.

### Bug B4 (version mapping, real)
`LoginActivity.autoDetectVersion()` (line 180–202):
```java
if (versionName.startsWith("2.126")) index = 0;
else if (versionName.startsWith("2.127")) index = 1;
String versionString = (index == 0) ? "v2.132.x" : "v2.133.x";
```
**Version detect `2.126`/`2.127`, kintu label `v2.132.x`/`v2.133.x`** — duita alada kotha. Aaj er FF version ei range e nei → detect fail, `VERSION_INDEX` default 0 e theke jay, `setGameVersionNative` call hoy na, tai `g_selectedVersion` `"v2.130.x"` (Client.h:54) default e theke jay → **India build e bhul offset set**. Eta manually version button diye thik korte hoy.

---

## 6. Feature inventory (client ID → server Mode)

| UI ID | Label | Effect | Server Mode |
|---|---|---|---|
| 1 | Enable Functions | `Actived.activar` (master) | 3 |
| 3 | Dive Kill | local player Y −0.9 | 59 |
| 4 | Fly Hack | root pos Y = ground+4.5, `+0x2098=true` | 41 |
| 5 | PC Movement | `UserControl+0x90=1`, `+0x9C=true` | 42 |
| 6 | Aim Collider | `ColliderBody = ColliderHead` (head hitbox) | 54 |
| 7 / 70 | Chest / Head Rate | hit pattern (chest:head ratio) | 62 / 70 |
| 8 | Aim Silent | ray dir overwrite (`AimInfo+RayDir`) | 57 |
| 9 / 10 | AimFov / AimDistance | FOV radius calc | 58 / 59 |
| 11–21, 31, 40 | ESP line/box/dist/name/health/count/weapon | client-side draw | — |
| 22 | Ignore Knock | skip `XPose==8` (downed) | 55 |
| 25 | Speed Hack | `Attributes+ForceSpeed = 6.8` | 52 |
| 26 | Scale Down | enemy `matrix+Scale = -1.3` | 53 |
| 28 | AimBot | `AimRotation` = LookRotation(cam→target) | 51 |
| 29 | Spin Bot | root `Rotation` quaternion spin | 50 |
| 30 | WallHack Bug | camera tilt ~75°, matrix rotate | 49 |
| 33 | Ghost Hack | `player+0x814 = true` | 48 |
| 36 | Smart Kill (KillInChest) | sniper/hp<50 → chest | 45 |
| 37 | Aim Magnet | enemy root pos camera er dike pull (5.0f, clamp 5.3) | 44 |
| 38 | TP Mark | map mark position → local root pos | 43 |
| 101–108 | Shortcut toggles | floating checkbox | — |

`blockedWeaponIds` (Server.cpp:153): grenade/melee/special (`6016,10006,-15524,-15521,601,603,1201,1204,602,608,23,1401,1006,1015`) — ei weapon e aim assist bondho (false-positive avoid).

---

## 7. ESP render pipeline

`ESPView` = transparent, `FLAG_NOT_TOUCHABLE`, `TYPE_APPLICATION_OVERLAY`, own `Thread` 60 FPS e `postInvalidate()`.

`onDraw` → `Floater.PxbftKZXivSr(espView, canvas, w, h)` → JNI `DrawESP(ESP)`:

1. Watermark (always, `pEspPlayer.Info`): `"BYTE FORCE v2.1.3 Debug Global|India" + " / " + (Kernel|Root)`.
2. `kbjisui8821 != 9251` → **return** (license gate).
3. FOV circle (optional), `getData()` **blocking** socket call.
4. Enemy count front/behind.
5. Per player: line (top/center/bottom), box (stroke/filled/corner/rounded, width = 0.48×height), health bar + `HP` text, name (4× black outline + white, "Bot"/"Traning Bot" fallback), distance `NM`, weapon name via `Namegun::GetGunName` (unknown → `ID: n`).

State color override: downed → red; `Wukong`/`WukongSpecial` → green line; `Oreon` → dark red; `Ball` → grey.

`TransType` (special transformation) e head position weird hole hip pos −0.5 fallback.

---

## 8. License / auth path

`Floater` login tab (only while `uisid2iuwaw == 8261`): `TextBoxWidget("License", 999)` + `ButtonWidget("Login", 1000)`.

`ChangesString(999, text)` → `espname12` (Client.cpp:81).
`ChangesID(1000, _)` (Client.cpp:622):
1. HWID = `Settings.Secure.android_id + "-bklsensi-sec"`, 20 char e truncate.
2. `GET https://keyauth.win/api/1.2/?type=init&name=BYTE&ownerid=9CPqAOZ6JG&ver=1.0`
3. `type=login&username=<espname12>&pass=<espname12>&hwid=<hwid>&name=BYTE&ownerid=9CPqAOZ6JG&sessionid=<sid>`
4. Success → `kbjisui8821 = 9251`, `uisid2iuwaw = 7821`, `Namegun::Init()`, `LoginActivity.loginSuccessFromNative()`.

### Findings
- **F1:** `username == password == license key`. KeyAuth 1.2 login e username field e license dilei hoy — kintu eta prothom dik theke e **key = credential** design, ar `&pass=` o same. Kono per-user identity nei.
- **F2:** `CURLOPT_SSL_VERIFYPEER 0`, `VERIFYHOST 0` (line 596–597) — MITM e license/session plaintext. `do_curl` (line 586) same.
- **F3:** `ownerid` + app name hardcoded, `OBFUSCATE` shudhu compile-time XOR — `strings` e na peleo runtime memory te plaintext.
- **F4:** `ShowEspwea()` (line 180) e **placeholder** `"Your_Url"` ar `"Your_secret_key"` — APK fingerprint check kaj kore na. `ShowApk()` (217) thik ache (`libBKLSEC.so` must exist, onno `.so` nai, dex ≤ 3), kintu `RunCheck()` (296) er call site **commented out** (line 699–710). → **integrity/license verifier practically disabled**.
- **F5:** Gate ekta `int kbjisui8821` (Client.cpp:78). Frida/memory patch e `9251` likhlei pura menu khule jay. Kono server-side session validation nei.

---

## 9. Stealth stack — ki ache, ki nei

**Ache:**
- Daemon file-less: `memfd_create` + `/proc/<pid>/fd/N` exec → disk e binary nei (APK er vitore header).
- Random 8-char process name per launch, `killall` diye purono instance clean.
- `-fvisibility=hidden`, `-s` (stripped), `-Os`, `--gc-sections`, `-icf=safe`, `--exclude-libs,ALL`.
- String obfuscation: `OBFUSCATE()` (compile-time) + `oxorany` (Server.cpp).
- Release build: R8 minify + shrinkResources, `-repackageclasses ''`, log stripping, aggressive optimization passes.

**Nei / durbal:**
- **`/proc/<pid>/exe` → `/memfd:xxxxxxxx (deleted)`** — je kono AC `/proc` scan e dhore felbe. `prctl(PR_SET_NAME)` ba argv spoof nei (HEX BLADE e chilo).
- **Abstract socket name constant** `"УБИЙЦА. ХД"` — memory scan e plaintext. Ar abstract socket e **file permission nei** → je kono app connect korte pare.
- **No `SO_PEERCRED` / `getsockopt` check** (verified: Socket code e nei) → **kono third-party app** daemon e connect kore (a) pura ESP data (shob player position) porte pare, (b) mode command pathate pare. Backlog 8, kintu `accept()` ekbar-i — race e age connect korle cheat app ta-i connect hote parbe na.
- `popen("pidof ...")` → `sh` child process, `/proc` e visible.
- `magiskpolicy --live` rules permanently policy change kore (session obdi), AC root-detection e policy dump dekhle `su → untrusted_app ptrace` rule suspicious.
- No anti-debug: `ptrace(PTRACE_TRACEME)` nei, `TracerPid` check nei, `frida` port check nei.
- `LibraryOk()` (LoginActivity:548) shudhu nijer `/proc/self/maps` e `/data/...*.so` khuje — trivial bypass, ar R8 er por oi loop ta roye geche kintu printf nei.
- `LoginActivity` line 74–79: 2 second por `Runtime.exec("su")` — shudhu root prompt trigger; fail korle `Process.killProcess(myPid())` (user experience e crash).

---

## 10. Code quality / bug list

| # | Where | Issue | Impact |
|---|---|---|---|
| B1 | root | `gen_bklkill.sh` deleted but CI needs it | CI fail / stale daemon |
| B2 | root | `local.propertie` typo, empty | confusion |
| B3 | Server.cpp:234 | version packet er age lib base resolve | wrong base on India |
| B4 | LoginActivity:187 | `2.126/2.127` → `v2.132/v2.133` mismatch | offsets silently wrong |
| B5 | Client.cpp:977 | `ftKZXivSr` connect fail korleo `connectClient=true` | retry nei, dead overlay |
| B6 | Client.h:127 | `SendFloat`: `float code = client.send(...)` + float→int cast | AimFov/AimDistance int-only, precision loss, warning |
| B7 | Client.h:139 | `getData` 11 KB struct by value, `onDraw` er vitore blocking | frame stall, 0.67 MB/s |
| B8 | Server.cpp:129 | `g_isScaled` map keyed by object ptr, kabhi clear hoy na | leak per match |
| B9 | Server.cpp:926 | `wasFiringLastFrame` unused; `lastTargetObj` unused | dead code |
| B10 | Server.cpp:1232 | `Read<uintptr_t>` diye `isFiring` (bool) — size mismatch | UB-ish (8 byte read) |
| B11 | Server.cpp:1280 | `Write(livePlayer + GhostHack, true)` — template arg bool, ok but `0x814` bool as assumption | fragile |
| B12 | Server.cpp:1014 | `finalAimHardBaseHead` shadow (local redeclare) | outer value unused |
| B13 | Server.cpp:1130 | `!isLocalWeaponBlocked` gate aim er jonno, kintu ESP tobuo dekhay | inconsistent |
| B14 | whole | `pPlayer`/`Actived` plain bools across 8 threads, no atomics | torn/stale reads |
| B15 | Client.cpp:193 | `std::stoull(decrypted)` — exception er jonno guard nei (placeholder hole throw) | crash on bad payload |
| B16 | RootServices/Tools | `RootService` manifest e declare nei (`<service>` nai), `Funçoes()` kokhono call hoy na | dead + broken code |
| B17 | Tools.java | `RandomAccessFile("/proc/<pid>/mem")` — app UID theke kaj korbe na | dead path |
| B18 | Floater:71 | `getBase64Icon()` declare, implementation `Widgets/Images.h` e — thik, kintu `.bak` file e duplicate | build e `.bak` ignored (ok) |
| B19 | Android.mk | `LOCAL_LDFLAGS` duibar define — prothom ta (`--gc-sections`, `--icf=safe`) **overwrite** hoye jay | dead sections rakha hocche, size loss |
| B20 | Android.mk | `-frtti` + `-fexceptions` + `-Os` + static libc++ | bloated binary, daemon 442 KB |

B19 particularly: duita `LOCAL_LDFLAGS :=` line ache; Make e second assignment first ke replace kore. Tai `-Wl,--gc-sections -Wl,--icf=safe` **kono din apply hoy na**.

---

## 11. Security assessment (apnar nijer project er drishtite)

| Vector | Risk | Note |
|---|---|---|
| Abstract socket, no peer check | **High (local)** | same device er je kono app ESP data porte/drive korte pare |
| Fixed socket name | Medium | static signature, memory/maps scan e dhora pore |
| memfd exec, no prctl spoof | High (AC) | `/proc/*/exe` → `(deleted)` |
| Live SELinux policy edit | High (AC) | policy dump e obvious |
| `pidof` via `sh` | Low | child process trail |
| KeyAuth plaintext over TLS-off | High | credential leak / crack |
| Client-side single int gate | High | trivial patch |
| Release signed with **debug key** | Medium | `cert = android debug` → repack/crack trivial, ar AC fingerprint |
| No server-side validation | High | offline license bypass |

---

## 12. KPM project er sathe tulona (porting note)

Ei project ta **root-daemon model** — jeta apnar Regedit/KPM requirement er ulto:
- ekhane: `su` + `/proc/<pid>/mem` + `process_vm_*` + `/dev/nh` + separate root process.
- KPM target: kernel-space only, **no daemon, no su, no `/proc/<pid>/mem`**.

Porting e je jinish gulo directly reuse hoy:
- `Offsets.h` er dui table (GameFacade/Player/… ) — KPM e same field offsets lagbe.
- `CreateDataList` er pointer chain + visibility/team/dead filter logic.
- `WorldToScreenPoint`, `GetPosition` (bone matrix chain), `GetCameraWorldPosManual`.
- Aim math: `Quaternion::LookRotation`, `AimRotation` write, `RayDir` silent aim, hit pattern (HeadRate/ChestRate) state machine.
- `Namegun` weapon ID table (~200 ID).

Ja baad jabe: `MemfdHelper`, `Initialize` (su/SELinux), `Socket/*`, `Server.cpp` er `main`, `Tools.java`, `RootServices.java`.

---

## 13. Next concrete steps (priority order)

1. **CI fix:** `git checkout -- gen_bklkill.sh` (ba workflow theke oi step ta sorao), ar `local.propertie` delete.
2. **B4 fix:** `autoDetectVersion` e version list ek jaygay rakho (`{"2.132","2.133"}` ↔ `{"v2.132.x","v2.133.x"}`), mismatch rakho na.
3. **B5 fix:** `startClient()` return value check koro, fail hole `connectClient=false` rekhe retry thread dao.
4. **B3 fix:** version packet main loop e giye `currentOffsets` set howar **por** `libAddress = FindCorrectLibBase()` re-resolve koro.
5. **Local socket hardening:** `getsockopt(SO_PEERCRED)` + expected UID check; name ta runtime-random (device-bound hash) koro.
6. **Debug key badlao** — release ke nijer keystore e sign koro, noyh hole repack kore keu license bypass korbe.
7. `RunCheck` enable korte hole `ShowEspwea` er `"Your_Url"`/`"Your_secret_key"` real value dao, ar `stoull` ke try/catch e rakho.
8. `Android.mk` er duplicate `LOCAL_LDFLAGS` merge koro (B19) — `--gc-sections --icf=safe` + `-latomic` ek jaygay.

---

## 14. Appendix — file map

| File | Lines | Role |
|---|---|---|
| `jni/src/Client.cpp` | 1094 | overlay/JNI, ESP draw, widget tree, KeyAuth |
| `jni/src/Server.cpp` | 1501 | daemon: memory R/W, aim/esp threads, main loop |
| `jni/src/Client.h` | 154 | protocol structs, socket helpers, `getData` |
| `jni/src/Server.h` | 191 | `InitServer`, `GetPosition`, `WorldToScreenPoint` |
| `jni/src/Includes/Utils.h` | ~475 | `Read/Write`, `FindPid`, `/proc/mem`, `D3DMatrix`, `MonoDictionary` |
| `jni/src/KernelDriver.h` | 65 | `/dev/nh` ioctl wrapper |
| `jni/src/MemfdHelper.cpp` | 57 | memfd daemon loader |
| `jni/src/Offsets.h` | 245 | Global/India offset tables |
| `jni/src/Namegun.cpp` | 223 | weapon ID → name |
| `jni/src/Socket/*` | 4 files | AF_UNIX client/server |
| `jni/src/bklkill_bin.h` | 36 887 | embedded daemon ELF (442 600 B) |
| `java/.../Initialize.java` | 369 | su + SELinux + daemon start (prothom 184 line dead comment) |
| `java/.../LoginActivity.java` | 570 | UI, root/version auto-detect, `LibraryOk` |
| `java/.../Floater.java` | 1111 | floating menu, tabs, widgets, shortcuts |
| `java/.../ESPView.java` | 147 | Canvas 60 FPS loop |
| `java/.../RootServices.java` + `Tools.java` | 58 + 120 | **dead** root IPC + `/proc/mem` patcher |
