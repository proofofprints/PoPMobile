---
date: "2026-04-17T15:30:00-04:00"
session_name: general
git_commit: ebed668
branch: claude/android-miner-planning-0KJtY
repository: KASMobileMiner
topic: "PoPMobile UI polish, QR scanner, pool error handling, PoPManager integration"
tags: [popmobile, ui-polish, qr-scanner, pool-error-handling, popmanager, android, kaspa]
status: complete
last_updated: "2026-04-17"
type: implementation_strategy
root_span_id: ""
turn_span_id: ""
---

# Handoff: PoPMobile polish (QR, pool errors, small-screen UI) — ready for real-device testing

## Task(s)

### Completed this session
- **Rebrand PoPMiner → PoPMobile** — app name, splash screen, top bar, notification, default worker name, PoPManager `manufacturer` field set to "Proof of Prints" (model stays "Mobile")
- **Launcher icon** — user created `mipmap/poplauncher` + `poplauncher_round` via Android Studio Image Asset Studio; manifest points to those
- **QR code scanning for wallet entry** — swapped from Google Play Services code scanner (needed on-demand download that failed) to ZXing bundled in APK; custom portrait-locked `QrScannerActivity` with square 280dp framing box, cyan theme laser, instructional banners
- **Pool connection error handling** — new `MiningService.PoolState` enum (`DISCONNECTED` / `CONNECTING` / `CONNECTED` / `ERROR`), `poolErrorReason` with friendly string ("Refused", "Timed out", "DNS failed", "Unreachable", "No network"), StatusCard shows yellow error state with specific reason
- **Session-active session tracking** — new `isSessionActive` flag separate from `miningEngine.isRunning`. Start Mining → session active immediately → button becomes Stop so user can cancel stuck connection attempts. 10s connect timeout on socket so "Refused" fires fast
- **Settings saved notice** — moved above Save button so it's visible without scrolling
- **Dashboard layout compacted** — 3-column compact stat rows replace 2×3 grid; fits on small screens like TCL A3X (~640dp)

### Carried over from earlier sessions (all complete)
- Extranonce handling in mining.submit (fixed 100% share rejection)
- PoPManager pairing code registration flow
- Remote command handling (set_config, set_threads, start, stop, restart) with pending acks queue
- PoPManager reporter runs independently of mining state

### Pending / ready to pick up
- **Release signing** for open-source release: generate dedicated keystore outside repo, add signing config in `app/build.gradle.kts` reading env vars or gitignored `keystore.properties`, bump `versionCode`/`versionName`
- **Test on real device** — user will install debug APK on TCL phone via USB (Android Studio Run button). Known to work on emulator after all recent fixes.
- **PoPManager mobile miners dashboard view parity with ASIC screen** — ongoing work in separate PoPManager code session (not this one)

## Critical References
- `app/src/main/java/com/proofofprints/kasminer/ui/MainActivity.kt` — dashboard + settings UI, QR launcher, StatusCard
- `app/src/main/java/com/proofofprints/kasminer/service/MiningService.kt` — mining lifecycle, PoolState, friendlyConnectError, stopMining
- `app/src/main/java/com/proofofprints/kasminer/popmanager/PoPManagerReporter.kt` — telemetry + pair/reconnect split, commands/acks

## Recent changes

### Rebrand PoPMiner → PoPMobile
- `app/src/main/res/values/strings.xml:3` — `app_name` = "PoPMobile"
- `app/src/main/AndroidManifest.xml:16` — `android:label="PoPMobile"`, also `android:icon="@mipmap/poplauncher"` + `android:roundIcon="@mipmap/poplauncher_round"` (line 15)
- `app/src/main/java/com/proofofprints/kasminer/ui/MainActivity.kt` — 2 hardcoded "PoPMiner" → "PoPMobile"
- `app/src/main/java/com/proofofprints/kasminer/ui/SplashActivity.kt:91` — title
- `app/src/main/java/com/proofofprints/kasminer/service/MiningService.kt:596` — notification title, default worker "KASMobile" → "PoPMobile"
- `app/src/main/java/com/proofofprints/kasminer/popmanager/PoPManagerReporter.kt:462` — `manufacturer` = "Proof of Prints"

### QR scanner
- `app/build.gradle.kts:92-96` — replaced `com.google.android.gms:play-services-code-scanner` with `com.journeyapps:zxing-android-embedded:4.3.0` + `com.google.zxing:core:3.5.3`
- `app/src/main/AndroidManifest.xml:11-12` — added `CAMERA` permission and `uses-feature camera required="false"`
- `app/src/main/AndroidManifest.xml:41-47` — registered `QrScannerActivity` with `android:screenOrientation="portrait"`
- `app/src/main/java/com/proofofprints/kasminer/ui/QrScannerActivity.kt` — NEW: extends plain `android.app.Activity` (not AppCompatActivity because theme is plain Android fullscreen), uses `CaptureManager` + `DecoratedBarcodeView`
- `app/src/main/res/layout/activity_qr_scanner.xml` — NEW: 280dp square framing rect, cyan `#49EACB` laser, dark translucent banners top/bottom
- `MainActivity.kt` — added `qrScanLauncher` via `registerForActivityResult(ScanContract())` + `pendingScanResult` field; `launchWalletQrScanner()` uses `setCaptureActivity = QrScannerActivity::class.java`; `handleScannedWalletAddress()` strips `kaspa:kaspa:` double-prefix and `?amount=...` query params, validates `kaspa:` prefix
- `MainActivity.kt` settings panel — wrapped Wallet Address `MinerTextField` in a `Row` with a "Scan QR" `OutlinedButton`

### Pool connection error handling
- `MiningService.kt:85-97` — added `enum class PoolState`, `@Volatile var poolState`, `poolErrorReason`, `isSessionActive`
- `MiningService.kt:350-354` — `startMining()` sets `isSessionActive = true`, `poolState = CONNECTING`, clears error
- `MiningService.kt:390-404` — new `friendlyConnectError()` helper maps exceptions to short strings
- `MiningService.kt:378-388` — initial connect failure sets `poolState = ERROR` with reason
- `MiningService.kt:466-471` — `onConnected()` sets `poolState = CONNECTED`
- `MiningService.kt:472-519` — `onDisconnected()` sets state based on `isStopping` (DISCONNECTED if user stopped, ERROR otherwise); retry loop also updates state per attempt
- `MiningService.kt:556-568` — retries-exhausted path sets `poolState = ERROR`
- `MiningService.kt:384-393` — `stopMining()` resets `isSessionActive = false`, `poolState = DISCONNECTED`, clears error
- `stratum/StratumClient.kt:56-66` — replaced blocking `Socket(host, port)` with `Socket()` + `connect(InetSocketAddress, 10_000)` (10s timeout); assigns socket ref before connect so `disconnect()` can interrupt

### UI polish for small screens (TCL A3X)
- `MainActivity.kt:141-148` — added state vars `poolState`, `poolErrorReason`, `isSessionActive`; polled each tick in `LaunchedEffect`
- `MainActivity.kt:278-283` — outer column padding reduced 16dp→12dp horiz, 16dp→10dp vert; row spacing 16dp→10dp
- `MainActivity.kt:310-316` — `StatusCard` signature: now takes `poolState`, `poolErrorReason`, `isSessionActive`, `difficulty`
- `MainActivity.kt:320-388` — dashboard body: 3×2 grid replaced with two 3-column compact rows (SHARES/HASHES/THREADS, TEMP/BATTERY/THERMAL)
- `MainActivity.kt:510-568` — `StatCard` font 28→24sp, padding 16dp→14/10dp; new `StatCardCompact` with 15sp/10sp/8dp padding, centered, maxLines=1
- `MainActivity.kt:441-506` — `StatusCard` rewritten: pool label shows error reason in yellow `#FFD700` (not red), "RETRYING" / "CONNECTING" / "CONNECTED" / "IDLE" status text
- `MainActivity.kt:392-418` — Start/Stop button uses `sessionLive = isSessionActive || isRunning`; button is red STOP MINING during connection attempts
- Start/Stop button: height 56dp→52dp, font 18sp→16sp
- Disclaimer: centered, 12sp→10sp, top padding 8dp→2dp; text shortened
- Settings panel "Settings saved!" toast moved above SAVE button (not below, where it was hidden)

## Learnings

### QR scanning
- **Google Play Services code scanner needs on-demand module download** — fails on some devices with "Waiting for the Barcode UI module to be downloaded". `ModuleInstallClient.installModules()` wrapper can trigger the download but still unreliable.
- **ZXing-android-embedded is better for open source** — ~1MB APK cost but no Play Services dependency, works on AOSP/GrapheneOS/Fire tablets, bundled = works on first launch offline
- **ZXing `CaptureActivity` default is landscape with wide framing rect** — for QR wallet scanning you almost certainly want a custom activity with portrait orientation and square framing rect
- **`AppCompatActivity` needs an AppCompat theme** — extending `AppCompatActivity` with `@android:style/Theme.NoTitleBar.Fullscreen` crashes. Use plain `android.app.Activity` when theme is non-AppCompat.
- **Kaspa QR formats to handle**: bare `kaspa:qyp...`, double-prefixed `kaspa:kaspa:qyp...` (some wallets), and `kaspa:qyp...?amount=1.0` URI form with query params

### Pool connection state
- **Socket constructor blocks indefinitely on some networks** — always use explicit `Socket()` + `connect(addr, timeout)` for user-cancellable connections
- **Assign socket reference BEFORE connect()** so `disconnect()` from another thread can close a partially-initialized socket and interrupt the connect
- **User wants to cancel before first successful connect** — track session state separately from engine running state. The mining engine only starts after the first `mining.notify` job arrives from the pool, but the user has already "started" as soon as they tapped the button.
- **Yellow is the right color for recoverable errors** — red reads as "dead/permanent", yellow reads as "transient/retrying which matches reality (we're in a 10-retry backoff loop)

### Small-screen UI
- **TCL A3X is ~640dp tall** — any full-screen page needs to fit the most important content (start button) within ~600dp effective after status bar and app bar
- **3-column stat rows with 15sp values** are the sweet spot for small screens while still readable
- **Center the disclaimer line** — with reduced font size (10sp), centered text looks intentional instead of cramped left-aligned

## Post-Mortem

### What Worked
- **Swapping QR library mid-session**: ZXing was a cleaner fit for the open-source goal than Play Services. Would have saved time to go straight to ZXing originally (users care about working offline and no Google dependency).
- **Enum for pool state**: Much cleaner than multiple booleans (`isConnecting`, `isRetrying`, `hasError`) — impossible states are unrepresentable.
- **Session-active flag separate from engine-running flag**: Correctly models the "clicked Start but pool not yet connected" state that users can observe and cancel.
- **Explicit 10s socket timeout**: Turned a multi-minute silent hang into a 10s "Refused" toast — huge UX improvement for a single line change.

### What Failed
- **Play Services code scanner**: Worked on test devices but failed on TCL A3X with "Waiting for the Barcode UI module to be downloaded." `ModuleInstall.installModules()` pre-download didn't reliably fix it. Switched libraries.
- **Default ZXing CaptureActivity**: User saw it as a horizontal red line with no clear box — needed a custom Activity with square viewfinder.
- **Tried AppCompatActivity with non-AppCompat theme**: Caused a crash. Switched to plain `Activity`.

### Key Decisions
- **Decision: Use ZXing instead of Play Services code scanner**
  - Alternatives: ML Kit bundled (`com.google.mlkit:barcode-scanning`), keep Play Services code scanner with better retry logic
  - Reason: ZXing is Apache 2.0, works on all Android including AOSP, bundled in APK so works offline on first launch, standard for open-source apps
- **Decision: Custom `QrScannerActivity` instead of default ZXing Activity**
  - Alternatives: Use `setOrientationLocked(true)` + rely on defaults
  - Reason: Default is landscape with wide rect; users need portrait with square framing box for familiar UX
- **Decision: `isSessionActive` + `PoolState` instead of just a `poolConnected` boolean**
  - Alternatives: Single `mining_state` string, keep boolean and show error text below status
  - Reason: Three distinct user states (CONNECTING/CONNECTED/ERROR with retries in background) need distinct UI rendering; boolean conflates "haven't connected yet" with "failed to connect"
- **Decision: 10s socket connect timeout**
  - Alternatives: 5s (too aggressive for slow cellular), 30s (users perceive as hung)
  - Reason: 10s is long enough for slow mobile networks, short enough to feel responsive, matches industry standard (most browsers, curl default)
- **Decision: Yellow for pool errors during active session**
  - Alternatives: Red for all errors, orange
  - Reason: Yellow reads as "transient, we're retrying" whereas red reads as "permanently broken". During an active session the retries are real, so yellow is accurate.
- **Decision: Manufacturer = "Proof of Prints", Model = "Mobile"**
  - Alternatives: Manufacturer = "PoPMobile", single field
  - Reason: Matches existing ASIC convention in PoPManager (Iceriver/KS0). Brand name as manufacturer, product line as model.

## Artifacts

### New files
- `app/src/main/java/com/proofofprints/kasminer/ui/QrScannerActivity.kt`
- `app/src/main/res/layout/activity_qr_scanner.xml`
- `app/src/main/res/mipmap-*/poplauncher.png` (generated by Image Asset Studio)
- `app/src/main/res/mipmap-*/poplauncher_round.png` (generated by Image Asset Studio)

### Modified files
- `app/build.gradle.kts` — dependencies swap
- `app/src/main/AndroidManifest.xml` — permissions, icon refs, new activity registration
- `app/src/main/res/values/strings.xml` — app name
- `app/src/main/java/com/proofofprints/kasminer/ui/MainActivity.kt` — dashboard layout, QR integration, StatusCard, settings panel
- `app/src/main/java/com/proofofprints/kasminer/ui/SplashActivity.kt` — splash title
- `app/src/main/java/com/proofofprints/kasminer/service/MiningService.kt` — PoolState, error handling, friendlyConnectError, session tracking
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt` — explicit socket connect timeout
- `app/src/main/java/com/proofofprints/kasminer/popmanager/PoPManagerReporter.kt` — manufacturer field

### Previous handoffs (for context)
- `thoughts/shared/handoffs/general/2026-04-09_17-15-00_extranonce-root-cause-found.md` — extranonce debugging
- `thoughts/shared/handoffs/general/2026-04-09_12-27-00_kasmobileminer-invalid-difficulty-debug.md` — invalid difficulty debugging

## Action Items & Next Steps

### Immediate (user will do after this session)
1. **Rebuild APK** — Android Studio Build → Build APK(s), then install via USB or transfer
2. **Test on TCL A3X real device** — verify:
   - Home screen icon shows dark PoP cubes (not green from original error)
   - Dashboard fits on small screen without scrolling to reach START MINING
   - Settings "Save" notice is visible without swiping down
   - Scan QR button opens portrait scanner with square box
   - Point at invalid pool (e.g., `1.2.3.4:5555`) → within 10s shows `POOL: REFUSED` in yellow, button says STOP, tapping Stop returns to IDLE

### Near-term (next session)
1. **Release signing setup**:
   - Generate keystore: `keytool -genkey -v -keystore popmobile-release.keystore -alias popmobile -keyalg RSA -keysize 2048 -validity 10000` (store OUTSIDE repo)
   - Add `keystore.properties` to `.gitignore` with storeFile/storePassword/keyAlias/keyPassword
   - Add `signingConfigs` block in `app/build.gradle.kts` reading from `keystore.properties`
   - Wire `buildTypes.release.signingConfig = signingConfigs.getByName("release")`
   - Bump `versionCode` and `versionName` before each release

2. **PoPManager dashboard mobile miner view** (tracked separately in PoPManager session) — parity with ASIC miners screen: search, filter by coin, grid/list toggle, coin icons, card shows battery+throttle instead of fans, offline threshold = 2× report interval

3. **Consider later polish**:
   - Rename Kotlin package from `com.proofofprints.kasminer` to `com.proofofprints.popmobile` — requires migrating all files + updating `applicationId` in gradle + re-signing. Do before first public release.
   - Rename internal class names: `KASMinerApp` → `PoPMobileApp`, `KASMinerTheme` → `PoPMobileTheme` (cosmetic, no user impact)
   - Native C log tag `KASMiner` → `PoPMobile` (cosmetic)

## Other Notes

- **GitHub repo rename** — user should rename the repo on GitHub from `KASMobileMiner` (or whatever it is) to `PoPMobile`. Single-click in GitHub → Settings → General → Repository name. Local clones get a reminder to update their remote URL.
- **Java package `com.proofofprints.kasminer` NOT renamed** — doing this requires migrating every Kotlin file's package statement + imports, updating `applicationId` in `build.gradle.kts` (which forces existing paired PoPManager devices to re-pair because `deviceId` is tied to app install). Recommend doing it once before going public.
- **Known context limit concern** — this session is approaching 1M tokens. This handoff captures the important state so a new session can resume cleanly.
- **Mining flow, share acceptance, and PoPManager integration are all verified working** on the emulator (thousands of accepted shares, remote commands applied with acks). The pending verification is only that the same works on a physical TCL device.
- **Branch: `claude/android-miner-planning-0KJtY`** — all changes uncommitted as of this handoff. User workflow: use `/commit` skill when ready to push.
- **Latest working commit before this session**: `ebed668` "Fix kHeavyHash, extranonce handling, and difficulty target for working mining"
