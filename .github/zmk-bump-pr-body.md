Automated ZMK bump (policy **A**: stable releases by default; `main`/arbitrary refs only via manual dispatch).

Upstream is merged into the previously pinned `rleyvasal/zmk-next` firmware,
preserving the custom Studio, runtime configuration, diagnostics, battery, and BLE changes.

| | |
|---|---|
| **Mode** | `__MODE__` |
| **Target label** | `__LABEL__` |
| **Upstream commit** | `zmkfirmware/zmk@__SHA__` (`__SHORT__`) |
| **Fork branch** | `__FORKBRANCH__` |
| **Fork tip (west pin)** | `__FORK_TIP__` |
| **Release** | __RELEASE_TAG__ |
| **Release notes** | __RELEASE_URL__ |

(If **Release** / **Release notes** are empty, this was a `main` or `ref` dispatch, not a stable release.)

**Do not merge until you have flashed this PR's build artifact (left half) and verified all of the following on hardware:**

- [ ] **Typing** — keys register on both halves; homerow mods and combos behave
- [ ] **Profile switching** — `&bt BT_SEL` switches hosts and the newly-selected host types
- [ ] **No cross-talk** — only the active host stays connected; the other shows disconnected
- [ ] **Reconnect** — the selected host reconnects and types cleanly after a disconnect; USB typing remains stable
- [ ] **Diagnostics** — retained logs can be retrieved and enabling live logs does not stall typing
- [ ] **Battery** — idle drain in the normal range (~0.5–0.9 %/hr), no connect/disconnect churn

Merge to adopt this ZMK version. If anything regresses, close without merging (and open an issue with what broke).
