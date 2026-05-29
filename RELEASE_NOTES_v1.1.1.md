# v1.1.1 — Thermal protection, live hashrate, OverManager QR pairing

First APK build of the 1.1 series. v1.1.0 was tagged but never published with
a binary, so this release rolls up everything that landed since v1.0.14.

**Tag:** `v1.1.1`
**versionCode:** `22`
**applicationId:** `com.proofofprints.kasminer` (unchanged — v1.0.x installs upgrade in place)

## Added

- **Live (windowed) hashrate.** The hashrate display now reflects what the
  device is actually doing right now (10-second window) instead of a lifetime
  average that smeared over throttling events. Decays cleanly and hard-zeroes
  once mining is stopped.
- **User-configurable thermal + power protection.** Settings now exposes
  pause / resume temperatures, a battery cutoff %, "require charging" toggle,
  and an "external power mode" for plugged-in stationary use. Replaces the
  hard-coded 70 / 60 °C defaults.
- **Protection-state banner.** A coloured banner above Start / Stop says why
  mining is paused, throttled, or running with reduced protections (battery
  cutoff, unplugged, thermal critical, cooling, sensor unavailable, external-
  power mode, etc.) — instead of failing silently.
- **Battery Saver detection.** When Android's Battery Saver is on, the banner
  shows "Battery Saver active — OS throttling" so a sudden hashrate drop
  isn't mistaken for an app bug.
- **Temperature + banner stay live when stopped.** Hitting Stop no longer
  freezes the TEMP / BATTERY / banner readouts at their last values; the
  thermal poller keeps running.
- **OverManager pairing QR scanner.** Scan a pairing code from OverManager
  instead of typing it. ZXing is bundled in the APK — no Play Services
  dependency, works fully offline.
- **In-app Thermal Diagnostics card** (Settings → Thermal Diagnostics).
  Lists every thermal zone the OS exposes, the one protection is acting on,
  PowerManager headroom + thermal-status bucket, SoC model, and a
  "Copy report" button so users on unsupported chipsets can paste a clean
  bug-report into a GitHub issue.

## Changed

- **Thermal-zone selection broadened for non-Snapdragon devices.** We now
  scan both `/sys/class/thermal/` and `/sys/devices/virtual/thermal/`,
  recognize more CPU/SoC hint zone types, and explicitly skip trip-point
  zones (which return the trip threshold instead of the live temperature)
  and the Qualcomm PMIC `soc` zone (battery state-of-charge, not
  temperature). Non-Snapdragon phones that previously showed `--` should
  now show a real temperature.
- **Battery percent now matches the OS status bar.** Reads
  `EXTRA_LEVEL / EXTRA_SCALE` from `ACTION_BATTERY_CHANGED` first, falls
  back to `BatteryManager.BATTERY_PROPERTY_CAPACITY`. Several OEMs return
  a smoothed, fast-charge-aware capacity that lags the real value by 5-10 %,
  so the in-app reading disagreed with the system tray (e.g. 90 % in app
  vs 95 % in OS).
- **Battery cutoff is honored while plugged in.** Previously the cutoff
  applied only on battery; the user's "stop at X %" preference now applies
  on AC too, so the device still trips at the threshold when plugged in.
- **Resume hard-capped below Pause.** The Resume slider can no longer be
  set above the Pause threshold (would cause the engine to immediately
  re-pause on resume). Thread count is also restored to the user's setting
  after a throttle event instead of staying clamped.
- **OverManager error parity.** "Pairing code expired" message now matches
  OverManager's canonical wording so support docs line up across both apps.

## Fixed

- **Stop-mining restart regression** in `statsUpdateLoop` — Stop followed
  by Start would leave the engine wedged because of a race between the
  stats loop and `onNewJob`. The two paths are now decoupled.
- **Thermal-pause race + unbounded pool reconnect.** A pause that landed
  during a connection retry could spawn a second reconnect loop racing
  the first; both paths are now re-entrancy-guarded.
- **Invisible CRITICAL thermal alert.** The stats loop was reading a stale
  `thermalState` from a register because the field wasn't `@Volatile`, so
  CRITICAL transitions never made it onto the banner — users only ever saw
  "PAUSED — cooling" instead of the actual cause.
- **`kotlinx.coroutines.isActive` import** in the thermal poller — release
  builds were failing to resolve it on some toolchains.
- **MiningEngine `LinkageError` on new JNI methods.** Devices upgrading
  from older builds with cached native libraries no longer crash the stats
  loop when a JNI method signature changes; the loop swallows
  `LinkageError` and keeps running.
- **Settings crash on rotation** caused by an early `return` inside the
  diagnostics-card composable.
- **Slider integer truncation** (thread count, temperature thresholds)
  was rounding to whole values mid-drag, plus a Compose hover-state crash
  on long-press of the slider thumb. Both fixed.
- **Battery percent drift** vs the OS status bar (see Changed).

## Notes

- v1.1.0's tag is left as-is for history; this release supersedes it.
- Attach `app/build/outputs/apk/release/app-release.apk` after a signed
  release build before publishing.
