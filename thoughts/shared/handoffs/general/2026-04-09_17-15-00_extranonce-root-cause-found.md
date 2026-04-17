---
date: "2026-04-09T17:15:00-04:00"
session_name: general
git_commit: 31d49b8
branch: claude/android-miner-planning-0KJtY
repository: KASMobileMiner
topic: "KAS Mobile Miner - Extranonce Root Cause Found for Invalid Difficulty"
tags: [mining, kaspa, kheavyhash, stratum, extranonce, android, ndk]
status: in_progress
last_updated: "2026-04-09"
type: implementation_strategy
root_span_id: ""
turn_span_id: ""
---

# Handoff: Extranonce is the root cause of 100% share rejection — fix identified but not yet implemented

## Task(s)

### Completed
- **kHeavyHash SELFTEST PASSED** — Runtime self-test with known test vector `pre_pow=[0x2A]*32, ts=5435345234, nonce=432432432` produces expected hash `5a5bcd6e352eb8c8...`. The compiled hash algorithm is definitively correct.
- **Share submit ID conflict fixed** — Changed from hardcoded `id: 4` to incrementing `messageId++` with `pendingShareIds` tracking set
- **Local difficulty target fixed** — All-0xFF for diff ≤ 1.0 (matching KASDeck behavior)
- **Raw stratum message logging** — Promoted `<<<` and `>>>` messages to INFO level, unknown methods logged as WARNING
- **Full header hash logging** — New job messages now log all 32 bytes of parsed headerHash

### ROOT CAUSE IDENTIFIED (not yet fixed)
- **`mining.set_extranonce` is the cause of 100% share rejection**
- The Rust bridge sends `{"method":"mining.set_extranonce","params":["001e"]}` immediately after authorize
- We IGNORE this message completely
- The bridge prepends `"001e"` to our submitted nonce hex before parsing as uint64
- Our 16-char nonce hex + 4-char extranonce = 20 chars → overflows uint64 → wrong nonce → wrong hash → "Invalid difficulty"

### Pending
- **Implement extranonce handling** — See Action Items below
- **Commit and push** — All changes are uncommitted

## Critical References
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt` — Must handle `mining.set_extranonce`
- `app/src/main/cpp/mining_engine.c` — Must incorporate extranonce into nonce range
- `thoughts/shared/handoffs/general/2026-04-09_12-27-00_kasmobileminer-invalid-difficulty-debug.md` — Previous handoff with full algorithm details

## Recent changes
- `app/src/main/cpp/kheavyhash.c:175-207` — `set_target_from_difficulty()` rewritten: all-0xFF for diff ≤ 1.0, fixed byte range for higher diffs
- `app/src/main/cpp/mining_engine.c:156-212` — Added `run_selftest()` function with known test vector, called at `mining_start()`
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt:44` — Added `pendingShareIds` synchronized set
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt:95-108` — `submitShare()` uses incrementing IDs, tracks in pendingShareIds
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt:136` — `<<<` log promoted to INFO
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt:121` — `>>>` log promoted to INFO
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt:160` — Unknown methods logged as WARNING with raw JSON
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt:162-180` — Response handler uses pendingShareIds instead of hardcoded id==4
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt:225-227` — Full headerHash hex logged with new job

## Learnings

### ROOT CAUSE: Extranonce (CRITICAL)
The Rust-based Kaspa stratum bridge (`kaspanet/rusty-kaspa` beta) sends `mining.set_extranonce` with params `["001e"]`. This is a 2-byte (4 hex char) prefix that the bridge **prepends to the nonce** when validating shares. From `bridge/src/share_handler.rs`:
```
combined_nonce = format!("{}{:0>width$}", extranonce_val, submitted_nonce_hex, width = 16 - extranonce.len())
```
Since we submit 16 hex chars and bridge prepends 4 more, the combined 20 hex chars overflow uint64, causing wrong nonce → wrong hash → "Invalid difficulty".

### Bridge is Rust, NOT Go
- The user runs the **Rust** rusty-kaspa stratum bridge (beta), NOT `onemorebsmith/kaspa-stratum-bridge` (Go)
- The Rust bridge HAS share difficulty validation (error code 23 in `stratum_context.rs`)
- The Go bridge has difficulty checking COMMENTED OUT (always returns true) — researching the Go bridge was a dead end
- The Rust bridge source is at `kaspanet/rusty-kaspa`, subdirectory `bridge/src/`

### Bridge Stratum Flow (confirmed from logcat)
```
1. mining.subscribe → {"result":[true,"EthereumStratum/1.0.0"]}
2. mining.authorize → {"result":true}
3. mining.set_extranonce → params:["001e"]  ← WE IGNORE THIS
4. mining.set_difficulty → params:[0.0001]
5. mining.notify → params:["1",[u64,u64,u64,u64],timestamp]
```

### KASDeck runs on the same bridge
The user confirmed KASDeck Arduino runs on the same Rust bridge. KASDeck likely either:
- Handles extranonce properly, OR
- The bridge recognizes KASDeck's user-agent and uses a different code path (IceRiver/BzMiner paths exist)

### Self-test proves hash algorithm is correct
```
SELFTEST PASSED — expected 5a5bcd6e352eb8c8, got 5a5bcd6e352eb8c8
```
The compiled kHeavyHash (cSHAKE256 + XoShiRo256++ + matrix rank check + >>10 shifts) is byte-for-byte correct.

## Post-Mortem

### What Worked
- **Self-test at startup**: Adding a known-answer test to `mining_engine.c` definitively proved the compiled hash is correct, eliminating build cache as a suspect
- **Raw message logging**: Promoting `<<<`/`>>>` to INFO revealed the `mining.set_extranonce` message that was being silently dropped
- **Unknown method warning**: Logging unknown stratum methods with full raw JSON immediately exposed the extranonce issue
- **Researching the actual bridge**: Knowing it's the Rust bridge (not Go) led to finding the extranonce prepend logic in `share_handler.rs`

### What Failed
- **Researching the Go bridge first**: Spent significant time analyzing `onemorebsmith/kaspa-stratum-bridge` which has completely different behavior (no difficulty checking, no extranonce by default)
- **Assuming hash mismatch**: Multiple sessions were spent verifying the hash algorithm when the issue was never the hash — it was the nonce value the bridge used
- **Python cross-verification**: While it proved the algorithm was correct, it also proved the algorithm was never the problem, wasting time on a red herring

### Key Decisions
- **All-0xFF target for diff ≤ 1.0**: Let every hash pass locally since the bridge validates anyway. Matches KASDeck behavior.
- **Incrementing share IDs**: Prevents authorize response (id=2) from being mistaken for share acceptance
- **Self-test before mining**: Catches build cache issues immediately rather than debugging hash mismatches

## Artifacts
- `app/src/main/cpp/mining_engine.c` — Self-test function + all-0xFF target
- `app/src/main/cpp/kheavyhash.c` — Verified correct kHeavyHash + fixed `set_target_from_difficulty`
- `app/src/main/java/com/proofofprints/kasminer/stratum/StratumClient.kt` — Incrementing IDs, raw message logging, pendingShareIds
- `thoughts/shared/handoffs/general/2026-04-09_12-27-00_kasmobileminer-invalid-difficulty-debug.md` — Previous handoff

## Action Items & Next Steps

### 1. IMPLEMENT EXTRANONCE HANDLING (highest priority)
In `StratumClient.kt`:
- Add `private var extranonce: String = ""` field
- Handle `mining.set_extranonce` in `handleMessage()`:
  ```kotlin
  "mining.set_extranonce" -> {
      val params = doc.getAsJsonArray("params")
      extranonce = params[0].asString
      Log.i(TAG, "Extranonce set: $extranonce")
  }
  ```
- Modify `submitShare()` to strip extranonce prefix from nonce hex:
  ```kotlin
  val fullNonceHex = String.format("%016x", nonce)
  // Bridge prepends extranonce, so we send only the extranonce2 portion
  val extranonce2Len = 16 - extranonce.length
  val nonceHex = "0x" + fullNonceHex.takeLast(extranonce2Len)
  ```

In `mining_engine.c` (or JNI/MiningEngine.kt):
- Pass extranonce to mining engine so nonce upper bytes are fixed to extranonce value
- When starting mining, set nonce range: `nonce_start = (extranonce_value << (extranonce2_bits)) | random_lower_bits`
- Each thread varies only the lower `extranonce2_size` bytes

### 2. Verify KASDeck extranonce handling
Check `C:\Users\Evan\Documents\Arduino\KASDeck\KASDeck.ino` for how it handles `mining.set_extranonce`. If KASDeck handles it, port that logic.

### 3. Test with bridge
After implementing extranonce, rebuild (`rm -rf app/.cxx` + Rebuild), deploy, mine, and verify shares are accepted.

### 4. Commit and push
All changes are uncommitted on `claude/android-miner-planning-0KJtY`. Need to commit, push, and PR to main.

### 5. UI polish (lower priority)
- Connection error display
- Greyed-out start button when unconfigured
- PopLogo3 on about/info page

## Other Notes

### Exact bridge messages (from logcat)
```
>>> {"id":1,"method":"mining.subscribe","params":["KASMobile"]}
<<< {"id":1,"result":[true,"EthereumStratum/1.0.0"]}
>>> {"id":2,"method":"mining.authorize","params":["kaspa:qyp...mpng5.KASMobile"]}
<<< {"id":2,"result":true}
<<< {"jsonrpc":"2.0","method":"mining.set_extranonce","params":["001e"]}
<<< {"jsonrpc":"2.0","method":"mining.set_difficulty","params":[0.0001]}
<<< {"id":1,"jsonrpc":"2.0","method":"mining.notify","params":["1",[u64,u64,u64,u64],timestamp]}
```

### Bridge diff_to_target computation
The Rust bridge has THREE different target computation modes selected by environment variables:
- Default: MAX_TARGET = 2^224 - 1
- `USE_STRATUM_TARGET_CALC=true`: KASPA_MAX_TARGET = 2^64 - 1
- `USE_ALTERNATIVE_TARGET_CALC=true`: Similar to stratum calc

The comparison is `pow_value >= pool_target` → reject (error 23).

### All code changes are uncommitted
The worktree at `.claude/worktrees/naughty-wright` has the working code. The main branch is at `31d49b8`.
