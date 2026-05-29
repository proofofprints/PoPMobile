# Cutting a OverMobile release

Checklist to produce a signed APK, compute its checksum, and publish a GitHub release. Paste-ready commands are Windows cmd (the project's primary build environment); macOS/Linux equivalents are shown where they differ.

## 0. Prerequisites

- `popmobile-release.keystore` exists outside the repo (e.g. `L:\KaspaAndroidMiner\popmobile-release.keystore`)
- `keystore.properties` at the repo root points at it with the correct passwords (gitignored — see `keystore.properties.example`)
- `main` is clean and green; all PRs for this version are merged

## 1. Bump the version

In `app/build.gradle.kts`:

```kotlin
versionCode = <N+1>
versionName = "X.Y.Z"
```

Semver:

- **patch** (1.0.1 → 1.0.2) — bug fixes, internal refactors, doc-only
- **minor** (1.0.x → 1.1.0) — new features, backwards compatible
- **major** (1.x.x → 2.0.0) — anything that breaks upgrade-in-place (e.g. `applicationId` change)

Commit the bump on a PR branch, merge to `main` before you build. This keeps the git tag and the APK's internal version in sync.

## 2. Build the signed APK

**Android Studio:** `Build → Generate Signed Bundle / APK → APK → Next → release → Create`

**CLI (from the repo root):**
```cmd
gradlew assembleRelease
```
(Or `./gradlew assembleRelease` on macOS/Linux.)

The APK lands at `app\build\outputs\apk\release\app-release.apk`.

## 3. Rename the APK

Pull it out of the build tree and label it with the version:

```cmd
copy app\build\outputs\apk\release\app-release.apk popmobile-vX.Y.Z.apk
```

## 4. Compute the SHA-256

**Windows:**
```cmd
certutil -hashfile popmobile-vX.Y.Z.apk SHA256
```
Copy the hex string from the output (ignore the `CertUtil: -hashfile command completed successfully` line).

**macOS / Linux:**
```bash
shasum -a 256 popmobile-vX.Y.Z.apk
```

## 5. Create the `.sha256` sidecar file

**Windows:**
```cmd
echo <hash-from-step-4> *popmobile-vX.Y.Z.apk > popmobile-vX.Y.Z.apk.sha256
```

**macOS / Linux:**
```bash
shasum -a 256 popmobile-vX.Y.Z.apk > popmobile-vX.Y.Z.apk.sha256
```

Sanity check:
```cmd
type popmobile-vX.Y.Z.apk.sha256
```
It should print one line: `<hash> *popmobile-vX.Y.Z.apk`.

## 6. Publish the GitHub release

1. Open https://github.com/proofofprints/PoPMobile/releases/new
2. **Choose a tag** → type `vX.Y.Z` → "Create new tag: vX.Y.Z on publish"
3. **Target** → `main`
4. **Release title** → `vX.Y.Z`
5. **Description** → use the template below
6. **Attach binaries** → drag `popmobile-vX.Y.Z.apk` **and** `popmobile-vX.Y.Z.apk.sha256` into the upload area
7. **Publish release**

### Release notes template

```markdown
# OverMobile vX.Y.Z

## What's new

- Bullet 1
- Bullet 2

## Upgrading from v1.0.x

Seamless in-place upgrade. Settings, logs, and OverManager pairing are preserved.
(Only note this when applicationId is unchanged — otherwise users must uninstall first.)

## Checksums

- Windows: `certutil -hashfile popmobile-vX.Y.Z.apk SHA256`
- macOS / Linux: `shasum -a 256 popmobile-vX.Y.Z.apk`

SHA-256: `PASTE_HASH_HERE`

Or verify against the `.sha256` file in the assets:

- Linux / macOS: `sha256sum -c popmobile-vX.Y.Z.apk.sha256`
```

## 7. Smoke test on a real install

Before telling anyone about the release:

1. Uninstall any existing OverMobile from the emulator / test device
2. Sideload the APK you just uploaded (download from the release page, not from your local build)
3. Mine for ~30 seconds; verify accepted shares accumulate
4. Go to Settings → About → **Check for updates** → should say "You're on the latest version."

If anything's wrong, delete the release and tag, fix, cut vX.Y.(Z+1). Never rewrite a published tag.

## 8. Housekeeping

- Delete merged PR branches (`claude/*`, feature branches, etc.)
- If you skipped any follow-up items during review, open issues for them now so they don't get lost

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| `gradlew assembleRelease` succeeds but APK won't install on device | `keystore.properties` missing — APK is unsigned |
| `INSTALL_FAILED_UPDATE_INCOMPATIBLE` when upgrading | Different signing key than the installed version. Uninstall first. |
| `INSTALL_FAILED_VERSION_DOWNGRADE` | You decremented `versionCode`. Must be monotonically increasing. |
| In-app **Check for updates** doesn't see the new release | GitHub's CDN can take ~60s to propagate `releases/latest`. Retry in a minute. |
| Downloaded APK fails to open | `FileProvider` authority mismatch. `${applicationId}.fileprovider` in manifest must match `"${context.packageName}.fileprovider"` in code. |
