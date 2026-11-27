# Change Log — ufsrvstun

## Release-261127 — 0.1.0
`d37c806` → `2fe4798` · 2026-11-19 → 2026-11-25

### New
- **TURN relay** — Allocate and Refresh transactions, channel bindings and ChannelData relay, relay sockets with a peer network policy and permissions, allocations released at their lease deadline
- **STUN** — a UDP listener with a configurable DTLS slot, STUN and DTLS protocol slots with their own listener endpoints, answered rejections, and a STUN command console
- **Configuration** — startup through the `ufsrvcorelib` pipeline, host bindings aligned with the family conventions, per-slot relay port ranges, and protocol slots gated on their config key
- **Tooling** — a standalone credential minting script and a Docker integration harness with an RTT baseline

### Fixes

### Deprecated
- (none)

### Removed
- The in-tree STUN/TURN codec (`src/rfc_codec`), now consumed from `ufsrvcorelib`
