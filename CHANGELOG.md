# Changelog

All notable changes to PoPMobile are recorded here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

[1.0.3]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.3
[1.0.2]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.2
[1.0.1]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.1
[1.0.0]: https://github.com/proofofprints/PoPMobile/releases/tag/v1.0.0
