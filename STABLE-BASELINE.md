# Stable BLE Baseline

## Validated baseline — 2026-08-15

Daily-driver on `fix/crosstalk` @ `7082589` for a week: no disconnects, and
macOS (profile 0) ↔ Windows (profile 2) switching stayed clean. Battery is
worse than earlier 0 dBm estimates (~3 days, Tuesday 100% → Friday 0%) and is
an accepted tradeoff for a split link that stays up.

The 2026-07-23 tag `ble-stable-2026-07-23` is the older exclusive-host-only
checkpoint. Do not flash it over this baseline unless you are bisecting.

### Validation results

- Switching between the macOS and Windows profiles is reliable.
- Only the selected host remains connected.
- No disconnection issues across a week of daily use, including down to 0%.
- Homerow mods no longer latch (280 ms term + `hold-trigger-on-release`).
- Windows reconnects more slowly than macOS, but within an acceptable time.

### Stable policy

The following settings in `config/totem.conf` define the validated policy and
are protected by `.github/workflows/verify-zmk-patch.yml`:

```ini
CONFIG_ZMK_SLEEP=n
CONFIG_TOTEM_EXCLUSIVE_HOST=y
CONFIG_TOTEM_LAZY_INACTIVE_HOST=n
CONFIG_TOTEM_IDLE_DISCONNECT=n
CONFIG_TOTEM_ADV_POST_SWITCH_DARK=n
CONFIG_TOTEM_EVICT_REQUIRES_BONDED_ACTIVE=y
CONFIG_TOTEM_WATCHDOG=n
CONFIG_TOTEM_HOST_EVENT_LOG=n
CONFIG_ZMK_SPLIT_BLE_PREF_INT=12
CONFIG_ZMK_SPLIT_BLE_PREF_TIMEOUT=400
CONFIG_BT_CTLR_TX_PWR_PLUS_8=y
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=n
CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_PROXY=n
CONFIG_TOTEM_RPA_DISCONNECT=n
CONFIG_TOTEM_ADV_RECONCILE=n
CONFIG_TOTEM_RECOVERY_REBOOT=n
```

The configuration uses ZMK fork revision
`074df9b70720de69c8f961fc8977bd5716adbf03` from `config/west.yml`.

Do **not** silently pick up main's later fork pin (`5606801c`, RPA disconnect
+ advertising reconcile + recovery reboot). That pin was never in the flashed
week. Re-validate Mac↔Win switch and the split link before moving the pin.

### Accepted tradeoff

Exclusive-host mode disconnects the inactive computer, so every profile switch
is a full BLE reconnect (Windows often slower than macOS). Split interval 15 ms
plus +8 dBm TX keep the halves together and stop hold-tap releases arriving
late enough to latch a modifier; that costs battery (~3 days vs the earlier
~5 day / 0.81 %/hr estimate at 0 dBm). Advertising still stops after five
minutes when the selected host is away.

### Recovery

If a later experiment regresses switching, the split link, or hold-tap
behavior, return to this baseline (this file + `config/totem.conf` + west pin
`074df9b`) rather than re-enabling watchdog, host-event-log, FAL, idle
disconnect, or the untested `5606801c` pin. Avoid resetting settings or
clearing bonds unless the host and keyboard bond records are known to disagree.
