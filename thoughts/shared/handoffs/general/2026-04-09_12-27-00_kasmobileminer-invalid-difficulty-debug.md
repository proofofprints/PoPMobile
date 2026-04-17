---
date: "2026-04-09T12:27:00+00:00"
session_name: general
git_commit: 31d49b8
branch: claude/android-miner-planning-0KJtY
repository: KASMobileMiner
topic: "KAS Mobile Miner - Invalid Difficulty Share Rejection Debug"
tags: [mining, kaspa, kheavyhash, cshake256, stratum, android, ndk]
status: in_progress
last_updated: "2026-04-09"
type: implementation_strategy
---

# Handoff: KASMobileMiner shares rejected as "Invalid difficulty" — hash verified correct but bridge still rejects

## Task(s)

### Completed
- **kHeavyHash algorithm implementation** — cSHAKE256 with domain separation ("ProofOfWorkHash", "HeavyHash"), XoShiRo256PlusPlus PRNG, matrix rank check, both shifts >>10. Verified against Python reference with test vectors AND real stratum data. Hash output matches perfectly.
- **Stratum protocol** — 3-param submit with 0x prefix on nonce hex, uint64 parsing via BigDecimal for values > 2^63
- **UI features** — PoPMiner branding, splash screen, logs, about page, difficulty display, rejected shares counter, save confirmation, WiFi lock, reconnect retry loop
- **Work buffer layout** — [0..31]=pre_pow_hash, [32..39]=timestamp LE, [40..71]=zeros, [72..79]=nonce LE at NONCE_OFFSET=72

### In Progress / BLOCKED
- **Share acceptance by rusty-kaspa bridge** — All shares are rejected as error code 23 "Invalid difficulty". The hash computation is **verified correct** (Python matches C output exactly with real stratum data), but the bridge still rejects every share. This worked on March 27 (7 accepted shares) but broke after a git reset + code restore cycle.

### KEY FINDING
The kHeavyHash was verified correct with REAL stratum data:
- `mining.notify` word 0: `10852994715058418037` (> 2^63, correctly parsed via BigDecimal)
- C code kHeavyHash output: `462c26b40dcccf7e...`
- Python kHeavyHash output: `462c26b40dcccf7e...`
- **MATCH** — hash computation is definitively correct

## Critical References
- `app/src/main/cpp/kheavyhash.c` — Verified working cSHAKE256 + XoShiRo256++ + rank check implementation
- `app/src/main/cpp/keccak.c` — cSHAKE256 implementation (padding 0x04, rate 136)
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt` — Stratum client with BigDecimal uint64 parsing

## Recent changes
- `app/src/main/cpp/kheavyhash.c` — Complete rewrite with cSHAKE256, PlusPlus PRNG, >>10 shifts, LE difficulty, rank check
- `app/src/main/cpp/keccak.c` — Full cSHAKE256 implementation replacing plain keccak256
- `app/src/main/cpp/keccak.h` — Updated header for cSHAKE256 API
- `app/src/main/cpp/mining_engine.c` — NONCE_OFFSET=72, timestamp at offset 32, current_timestamp variable, shares_rejected counter, SHARE_DEBUG logging
- `app/src/main/cpp/mining_engine.h` — Updated signatures (4-param mining_set_job, shares_rejected, increment_rejected)
- `app/src/main/cpp/jni_bridge.cpp` — Updated nativeSetJob to 4 params (headerHash, jobId, target, timestamp), added nativeGetSharesRejected/nativeIncrementRejected
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt:199` — BigDecimal parsing for uint64 words > 2^63
- `app/src/main/java/com/proofofprints/kasminer/mining/MiningEngine.kt` — Added timestamp param, sharesRejected, incrementRejected
- `app/src/main/java/com/proofofprints/kasminer/service/MiningService.kt` — WiFi lock, reconnect retry loop, LogManager calls, sharesRejected
- `app/src/main/java/com/proofofprints/kasminer/ui/MainActivity.kt` — PoPMiner branding, logs, about, difficulty display, rejected shares, save confirmation
- `app/src/main/AndroidManifest.xml` — SplashActivity as launcher, CHANGE_WIFI_STATE permission

## Learnings

### Gson Large Number Handling (CRITICAL)
- Gson's `asLong()` for JSON numbers > Long.MAX_VALUE falls back to `(long) Double.parseDouble()` which clips to Long.MAX_VALUE
- Gson's `asString()` for large numbers may return scientific notation (e.g., "1.2E19") which `Long.parseUnsignedLong()` can't parse
- **Fix**: Use `asBigDecimal().toPlainString().split(".")[0]` → `BigInteger` → `.toLong()` for correct unsigned uint64 parsing
- File: `StratumClient.kt:199`

### kHeavyHash Algorithm Differences (KASDeck vs rusty-kaspa)
| Component | Old (KASDeck/Go bridge) | New (rusty-kaspa bridge) |
|-----------|------------------------|--------------------------|
| Hash function | Keccak-256 (pad 0x01) | cSHAKE256 (pad 0x04, domain separation) |
| PRNG | XoShiRo256StarStar | XoShiRo256PlusPlus |
| Matrix shifts | sum1>>6 & 0xF0, sum2>>10 & 0x0F | Both >> 10: ((sum1>>10)<<4) \| (sum2>>10) |
| Matrix rank | No check | Must be full rank (64) |
| Nonce offset | byte 32 | byte 72 |
| Difficulty compare | Big-endian | Little-endian (byte[31] is MSB) |

### Work Buffer Layout
```
[0..31]  = pre_pow_hash (32 bytes from mining.notify)
[32..39] = timestamp (8 bytes, uint64 LE from mining.notify)
[40..71] = zero padding (32 bytes)
[72..79] = nonce (8 bytes, uint64 LE)
```

### Git State Warning
The main branch was reset to `31d49b8` (before cloud session commits). All working changes are UNCOMMITTED. The worktree at `.claude/worktrees/naughty-wright` also has the old (pre-fix) code — don't use it as reference.

## Post-Mortem

### What Worked
- **Python verification pipeline**: Computing kHeavyHash in Python and comparing byte-for-byte against C output definitively proved the hash algorithm is correct
- **SHARE_DEBUG logging**: Adding debug output for work buffer bytes, timestamp, nonce directly in C code was essential for verification
- **Test vector approach**: Using pre_pow_hash=[0x2A]*32, timestamp=5435345234, nonce=432432432 as a fixed test vector (expected: `5a5bcd6e352eb8c87c80d0f0574a45a5`) caught multiple implementation issues

### What Failed
- **Git reset + cloud session interaction**: User's cloud session pushed bad commits, we reset to pre-cloud state, but the C files (keccak.c, kheavyhash.c) reverted to OLD StarStar/keccak256 code since changes were never committed
- **Android Studio native build caching**: Even after rewriting C files, Android Studio served cached .so libraries. Required `rm -rf app/.cxx` + Rebuild Project
- **Multiple restore cycles**: The code was correct in our working session but lost across multiple git resets and restores

### Key Decisions
- **cSHAKE256 function approach** over hardcoded initial states: Both produce identical results (verified), but function approach is simpler to debug and doesn't require baking padding into constants
- **BigDecimal for uint64 parsing**: Chose `asBigDecimal().toPlainString()` over `parseUnsignedLong(asString())` because Gson may return scientific notation for large numbers
- **All-0xFF target for diff <= 1.0**: Let every hash pass locally since the bridge validates anyway. This means shares are submitted at maximum rate which may overwhelm the bridge.

## Artifacts
- `app/src/main/cpp/kheavyhash.c` — Verified correct kHeavyHash implementation
- `app/src/main/cpp/keccak.c` — cSHAKE256 implementation
- `app/src/main/cpp/mining_engine.c` — Mining engine with correct work buffer layout
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt` — Stratum client with BigDecimal parsing
- `app/src/main/java/com/proofofprints/kasminer/ui/MainActivity.kt` — Full UI with all features
- `KASDECK_PORT_REFERENCE.md` — Complete porting reference for Arduino/ESP32
- `app/src/main/java/com/proofofprints/kasminer/ui/SplashActivity.kt` — Splash screen
- `app/src/main/java/com/proofofprints/kasminer/LogManager.kt` — In-app logging

## Action Items & Next Steps

1. **INVESTIGATE why bridge rejects shares despite correct hash** — The hash computation is proven correct. Possible causes:
   - The bridge may be computing pre_pow_hash differently from what it sends in mining.notify (unlikely but possible)
   - The authorize response (`{"id":4,"result":true}`) is being misidentified as "Share accepted!" because our share submit also uses id=4. Fix: use incrementing IDs for submits, not fixed id=4
   - The bridge may require handling `mining.set_extranonce` which we currently ignore
   - Flooding: with target all-0xFF, we submit EVERY hash as a share. The bridge may be overwhelmed or rate-limiting

2. **Fix share submit ID conflict** — Change from fixed `id: 4` to incrementing `messageId++` for share submits, to avoid confusing authorize response with share responses

3. **Reduce share flooding** — Set a proper local difficulty target instead of all-0xFF so only real shares are submitted

4. **Commit all changes** — Everything is uncommitted. Need to commit to `claude/android-miner-planning-0KJtY` and force-push, then PR to main

5. **UI polish** — Connection error display, greyed-out start button when unconfigured, PopLogo3 on splash screen

## Other Notes

### Bridge Configuration
```yaml
# User's bridge at pool.proofofprints.com:5558
min_share_diff: 0.001
# Bridge vardiff has floor of 1.0 (needs patching to use min_share_diff as floor)
```

### Previous Working State
On March 27, the miner had 7 accepted shares (Acc: 7, Stl: 0, Inv: 0) on the bridge. The hash was verified working at that point. The issue started after a git reset that brought back old C code (StarStar, keccak256, wrong shifts).

### KASDeck Arduino Reference
The working mining logic has been documented in `KASDECK_PORT_REFERENCE.md` for porting to ESP32. The KASDeck Arduino code at `C:\Users\Evan\Documents\Arduino\KASDeck\KASDeck.ino` has already been updated with the correct cSHAKE256/PlusPlus implementation.
