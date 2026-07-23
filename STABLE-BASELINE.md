# Stable BLE Baseline

## Validated baseline — 2026-07-23

This baseline records the BLE policy that was field-tested with macOS on profile
0 and Windows on profile 2. The firmware behavior was introduced by commit
`9592a41` (`Restore exclusive host wake policy`). The annotated tag
`ble-stable-2026-07-23` also includes this record and its CI guard; it does not
change the field-tested firmware configuration or the pinned ZMK source.

### Validation results

- Switching between the macOS and Windows profiles is reliable.
- Only the selected host remains connected.
- Both hosts reconnect after being out of range for 7–8 hours.
- The selected host can be woken from sleep by the keyboard.
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
```

The configuration uses ZMK fork revision
`074df9b70720de69c8f961fc8977bd5716adbf03` from `config/west.yml`.

### Accepted tradeoff

Exclusive-host mode deliberately disconnects the inactive computer. Switching
profiles therefore requires a complete BLE reconnect, and Windows may take
longer to discover and reconnect than macOS. Advertising still stops after the
configured finite timeout when the selected host is unavailable, limiting
battery drain without allowing inactive profiles to remain connected.

### Recovery

If a later BLE experiment regresses switching, wake, or long-absence reconnect,
build the firmware from tag `ble-stable-2026-07-23`. Avoid resetting settings or
clearing bonds unless the host and keyboard bond records are known to disagree.
