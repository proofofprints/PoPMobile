# Changelog

All notable changes to PoPMobile are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.9] — 2026-04-18
### Fixed
- **Actually fixed the Download-button crash** (v1.0.7 misdiagnosed it). The `NoSuchMethodError` on `KeyframesSpec.at(Object, Int)` wasn't from R8 after all — it was a real version mismatch between `material3` and `animation-core` inside our pinned `compose-bom`. Material3's `LinearProgressIndicator` calls an animation-core method that didn't ship in the version bundled at runtime. Replaced the Material3 indicator with a plain `Box` progress bar we draw ourselves, sidestepping the entire `KeyframesSpec` code path.

## [1.0.7] — 2026-04-18
### Attempted fix (did not fully work)
- Disabled R8 minification for release in the belief that R8 was stripping a Compose method. The NoSuchMethodError on `KeyframesSpec.at(Object, Int)` persisted even without minification, revealing the real cause was a library version mismatch rather than shrinking. Kept the disabled minification — it's still the right default for a ~2 MB APK — but the actual Download-button fix landed in v1.0.9.

## [1.0.6] — 2026-04-17
### Fixed
- **Tapping "Open Settings" on the install-permission prompt no longer marks the update as dismissed.** In v1.0.4/v1.0.5 the Settings detour closed the dialog through the same code path as "Later", which recorded the version as declined — so after granting the permission and returning, **Check for updates** would report "You're on the latest version" even though an update was pending. The dialog now separates "close without declining" (Open Settings, successful install, retry) from "user declined this version" (Later, Cancel).
- `Check for updates` (manual, `force = true`) now bypasses the dismissed-versions list as well as the 24-hour throttle. If you explicitly ask, we always show you what's available.

## [1.0.5] — 2026-04-17
### Added
- Update check now also runs on each `ON_RESUME` (when the app returns to the foreground), not only on cold launch. The 24-hour throttle inside `UpdateChecker` still applies, so keeping the app open for days no longer hides new releases behind a force-quit.

### Notes
- Primarily a pipeline-verification release after v1.0.4's install-flow fixes — a device on v1.0.4 should be offered this build through the in-app updater.

## [1.0.4] — 2026-04-17
### Fixed
- In-app update flow got stuck when the user hadn't yet granted "Install unknown apps" permission — the Settings screen would open but our dialog had no way to resume afterward. Now we **pre-check** the permission before kicking off the download and, if missing, show a clear "Open Settings" explainer that cleanly closes the dialog so the user can retry cleanly after granting.
- Install intent is now also fired by a manifest-registered `BroadcastReceiver` listening for `DownloadManager.ACTION_DOWNLOAD_COMPLETE`, so the install dialog still appears even if the app was killed during the download. The in-app flow and the receiver dedupe via a shared pending-download-id SharedPref — whichever path gets there first consumes the trigger.

## [1.0.3] — 2026-04-17
### Added
- `CHANGELOG.md` to record release history outside the GitHub Releases page.
- "Last checked X ago" line under the version in **Settings → About**, so you can tell at a glance when the app last pinged GitHub for a newer release.

### Notes
- Also serves as the end-to-end verification release for the in-app update checker introduced in v1.0.2 — devices on v1.0.2 will be offered this build.

## [1.0.2] — 2026-04-17
### Added
- In-app update checker backed by the GitHub Releases API. Checks once per 24 h on app launch and on demand via **Settings → About → Check for updates**. Downloads the APK via `DownloadManager`, hands off to the system installer through a `FileProvider`. OS signature verification means only APKs signed with the same release keystore can install over an existing PoPMobile.

### Changed
- About panel version is now read from `BuildConfig.VERSION_NAME` instead of being hardcoded.

## [1.0.1] — 2026-04-17
### Changed
- Internal Kotlin package renamed `com.proofofprints.kasminer` → `com.proofofprints.popmobile`; JNI bridge function names, native library name (`libpopmobile.so`), and build namespace updated accordingly.
- `applicationId` intentionally **kept** on the old namespace so existing v1.0.0 installs upgrade in place — settings, logs, and PoPManager pairing are preserved.
- Project-layout diagrams in `README.md` and `CONTRIBUTING.md` refreshed.

## [1.0.0] — 2026-04-17
### Added
- First public release.
- Native kHeavyHash miner (`arm64-v8a`, `armeabi-v7a`, `x86_64`) ported from KASDeck.
- Stratum v1 client with extranonce handling, 10-second connect timeout, and cancellable retry loop.
- Pool status UX with specific error reasons (`REFUSED` / `TIMED OUT` / `DNS FAILED` / `UNREACHABLE` / `NO NETWORK`).
- Thermal monitoring with auto-throttle.
- Live dashboard: hashrate, shares, total hashes, thread count, temperature, battery, thermal state.
- QR code scanning for the Kaspa wallet address (bundled ZXing).
- Foreground service with persistent notification and one-tap Stop.
- Mining log retaining the last four hours of stratum + share events.
- PoPManager pairing with per-device API keys, telemetry every 30 s, and remote commands (`set_config`, `set_threads`, `start`, `stop`, `restart`) applied and acknowledged.

[1.0.9]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.9
[1.0.7]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.7
[1.0.6]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.6
[1.0.5]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.5
[1.0.4]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.4
[1.0.3]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.3
[1.0.2]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.2
[1.0.1]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.1
[1.0.0]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.0
