# v1.2.0 — Rebrand to OverBuild Labs

The app is now **OverMobile** by **OverBuild Labs** — matches the
website, OverManager desktop app, and OverMiner hardware line. Application
identity (`applicationId`), on-disk preferences, and the signing keypair
are unchanged: existing PoPMobile installs upgrade to OverMobile in place
with no loss of wallet, pool, pairing, or settings state.

**Tag:** `v1.2.0`
**versionCode:** `23`
**applicationId:** `com.proofofprints.kasminer` (unchanged — v1.x installs upgrade in place)

## Changed

- **Brand → OverBuild Labs / OverMobile.** Every user-facing surface —
  launcher label, splash, top bar, About card, settings, log copy/paste
  reports, notification text, OverManager pairing prose — now reads
  OverMobile / OverBuild Labs / OverManager / OverMiner.
- **New launcher icon.** Full adaptive + legacy + round set across all
  five densities, generated from the OverBuild hex/cube on a `#0C0C0F`
  background.
- **OverBuild Labs palette.** Emerald-500 primary (`#10B981`),
  violet-500 accent (`#8B5CF6`), neutral grey-black surfaces
  (`#0C0C0F` page, `#16161B` cards) — matches the website and OverManager.
  ~80 hardcoded Kaspa-teal / gold / navy color literals migrated
  alongside the Material3 colorScheme so the entire dashboard tracks
  the brand instead of just the Theme.
- **Coin-agnostic logo.** The in-app Kaspa hex (top bar, splash, About
  card) is replaced by the OverBuild hex — OverMobile is no longer
  visually locked to Kaspa as the SoC roadmap broadens.
- **Dashboard typography polish.** Card titles (SHARES / HASHES /
  THREADS / TEMP / BATTERY / THERMAL) move to a 10sp sans-serif with
  letter-spacing for a cleaner data-dashboard feel; values stay in
  monospace so digits + units line up across each row. HASHRATE value
  now white against the emerald label.
- **Splash is single-phase.** One 2-second hold of the OverBuild hex +
  white "OverMobile" wordmark, then into the dashboard. Company
  attribution moves to the About card.
- **About card refreshed.** `support@overbuildlabs.com` and
  `https://www.overbuildlabs.com`. Info button replaced with Material's
  `Icons.Outlined.Info` — crisp at every density.
- **Worker name default** for fresh installs is now `OverMobile`;
  existing installs keep their saved value.
- **Device telemetry** reports `manufacturer="OverBuild Labs"` to
  OverManager (was `"Proof of Prints"`).
- **Top bar fits on one line.** Title shrunk to 15sp white monospace +
  28dp icon so the OverBuild hex, wordmark, info button, Dashboard
  toggle, and Settings toggle all fit without wrapping.

## Fixed

- `THERMAL WARNING` no longer clipped to `WARNIN` on narrow screens.
  Compact stat-card values drop to 16sp + ellipsis overflow so the
  longest labels fit cleanly.

## Deferred to a later release

- GitHub repository URLs still point at `github.com/proofofprints/...`
  — the org has not been renamed yet.
- `pool.proofofprints.com` infrastructure unchanged.
- QR pairing wire-type identifier `popmanager-register` accepted as-is
  until OverManager flips to emit `overmanager-register`.

## Notes

- `applicationId`, `namespace`, JNI symbol names, and on-disk
  SharedPreferences file names (`kas_miner`, `popmanager`) are
  unchanged. The rebrand is brand-only — protocol- and state-stable.
- Attach `app/build/outputs/apk/release/app-release.apk` after a
  signed release build before publishing.
