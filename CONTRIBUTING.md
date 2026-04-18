# Contributing to PoPMobile

Thanks for your interest in improving PoPMobile. Contributions — code, bug reports, docs, or testing on devices we haven't covered yet — are welcome.

## Ways to help

- **File bug reports** with device, Android version, pool, and relevant logs (from the in-app **Logs** tab)
- **Confirm issues** on your own hardware — we've tested on emulator + TCL A3X only
- **Suggest features** with a concrete use case
- **Send pull requests** for fixes and improvements (see below)

## Development setup

**Prerequisites:**

- [Android Studio](https://developer.android.com/studio) (latest stable)
- JDK 17 (bundled with Android Studio)
- Android SDK 34 + NDK (installed through Android Studio's SDK Manager)
- An Android device or emulator running API 26+ (Android 8.0+)

**Build and run:**

```bash
git clone https://github.com/proofofprints/PoPMobile.git
cd PoPMobile
./gradlew assembleDebug          # CLI build
# or open the folder in Android Studio and hit Run
```

The APK ends up at `app/build/outputs/apk/debug/app-debug.apk`. You don't need a release keystore for development — it's only required for signed release builds (see README for release signing).

## Project layout

```
app/src/main/
├── cpp/                       # Native kHeavyHash + mining loop (C, NDK)
├── java/com/proofofprints/popmobile/
│   ├── ui/                    # Compose UI: dashboard, settings, logs, QR, splash
│   ├── service/               # Foreground MiningService (lifecycle, thermal)
│   ├── stratum/               # Stratum v1 client
│   ├── mining/                # JNI bridge to the native miner
│   ├── popmanager/            # PoPManagerReporter (telemetry + remote commands)
│   └── thermal/               # Thermal monitor + auto-throttle
└── res/                       # Icons, themes, strings, layouts
```

## Pull request workflow

1. **Open an issue first** for anything non-trivial so we can agree on scope before you spend time on it
2. **Fork the repo** and create a branch: `git checkout -b fix/short-description`
3. **Keep the change focused** — one bug fix or one feature per PR, not a mix
4. **Follow the existing style**:
   - Kotlin: Android Studio's default formatter (4-space indent, no wildcard imports for production code)
   - C: match surrounding style; prefer explicit braces and small functions
   - Compose: extract composables that exceed ~60 lines
5. **Don't commit** keystores, `keystore.properties`, IDE files under `.idea/`, or `local.properties`
6. **Test on hardware** if possible, or emulator at minimum. Note what you tested in the PR body
7. **Open a pull request** against `main`

## PR description template

```
## Summary
What changed and why. 1–3 bullets.

## Test plan
- [ ] Built on Android Studio / gradle CLI
- [ ] Verified on <device, Android version>
- [ ] Specific scenarios exercised
```

## Code review expectations

- A maintainer will look at PRs within ~7 days. Nudge in the PR if it's been longer
- We optimize for **working + understandable** over clever. Readable is better than short
- Expect small nits — please don't take it personally. We want to keep the codebase consistent

## Commit messages

Single-line summaries are fine for small changes. For anything that touches multiple subsystems, write a short body explaining the *why*, not just the *what*. Examples of good titles:

- `Fix share rejection on pools that use extranonce`
- `Add thermal auto-throttle at 55°C`
- `Update README screenshots for v1.0`

## Security

**Do not open public issues or PRs for security vulnerabilities.** See [SECURITY.md](SECURITY.md) for the private reporting path.

## License

By submitting a PR you agree that your contribution is licensed under the same [MIT license](LICENSE) as the project.

## Questions

- **Bugs / feature discussion**: [GitHub Issues](https://github.com/proofofprints/PoPMobile/issues)
- **General questions**: [support@proofofprints.com](mailto:support@proofofprints.com)

Thanks for contributing.
