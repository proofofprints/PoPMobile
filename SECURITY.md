# Security Policy

## Supported versions

Only the latest published release of PoPMobile receives security updates. If you're on an older APK, update to the [most recent release](https://github.com/proofofprints/PoPMobile/releases) before reporting an issue.

| Version | Supported |
| --- | --- |
| v1.0.x  | ✅ |
| older   | ❌ |

## Reporting a vulnerability

**Please do not open a public GitHub issue for security vulnerabilities.** Public disclosure before a fix is available puts users at risk.

Instead, report privately through either of these channels:

- **GitHub private reporting** (preferred): visit [Report a vulnerability](https://github.com/proofofprints/PoPMobile/security/advisories/new) and submit a draft security advisory
- **Email**: [support@proofofprints.com](mailto:support@proofofprints.com) with subject line `[PoPMobile security]`

Please include, where possible:

- A clear description of the issue and the impact (what an attacker can do)
- Steps to reproduce (proof-of-concept code, test wallet address, pool, logs, etc.)
- The app version (Settings → About) and Android version
- Your name or handle if you'd like credit in the advisory

## What to expect

- **Initial acknowledgement**: within 7 days
- **Status update**: within 30 days — we'll tell you whether we've reproduced the issue, our plan, and a rough timeline
- **Fix and coordinated disclosure**: we aim to publish a fixed release within 90 days of the initial report, sooner for high-severity issues. We'll credit you in the release notes and GitHub advisory unless you prefer to stay anonymous

## Scope

In scope:

- The PoPMobile Android app (this repository)
- The PoPManager reporter protocol as implemented by this app (auth, pairing, command handling)
- Native mining code (`app/src/main/cpp/*.c`) that runs with user data

Out of scope (please report to the respective project):

- The Kaspa network or any pool software — upstream at [kaspanet](https://github.com/kaspanet)
- PoPManager server — [proofofprints/PoPManager](https://github.com/proofofprints/PoPManager)
- Bugs that require an already-compromised device (e.g., root + hostile app already installed)
- Missing security hardening that doesn't correspond to a concrete exploit

## Safe-harbor

We will not pursue legal action against researchers who:

- Make a good-faith effort to avoid privacy violations, data destruction, or service disruption
- Report the issue privately before public disclosure
- Only test against wallets, pools, and PoPManager instances they own

Thanks for helping keep PoPMobile users safe.
