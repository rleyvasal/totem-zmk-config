Automated ZMK bump (policy **A**: stable releases by default; `main`/arbitrary refs only via manual dispatch).

| | |
|---|---|
| **Mode** | `__MODE__` |
| **Target label** | `__LABEL__` |
| **Upstream commit** | `zmkfirmware/zmk@__SHA__` (`__SHORT__`) |
| **Fork branch** | `__FORKBRANCH__` |
| **Release** | __RELEASE_TAG__ |
| **Release notes** | __RELEASE_URL__ |

(If **Release** / **Release notes** are empty, this was a `main` or `ref` dispatch, not a stable release.)

**Do not merge until you have flashed this PR's build artifact (left half) and verified all of the following on hardware:**

- [ ] **Typing** — keys register on both halves; homerow mods and combos behave
- [ ] **Profile switching** — `&bt BT_SEL` switches hosts and the newly-selected host types
- [ ] **No cross-talk** — only the active host stays connected; the other shows disconnected
- [ ] **Idle go-dark** — after ~20 min idle the host disconnects and does NOT wake/flap (test the plugged-in / external-monitor Mac specifically)
- [ ] **Reconnect** — a keypress wakes it, the host reconnects and types cleanly (macOS included, no "connected but can't type")
- [ ] **Battery** — idle drain in the normal range (~0.5–0.9 %/hr), no connect/disconnect churn

Merge to adopt this ZMK version. If anything regresses, close without merging (and open an issue with what broke).
