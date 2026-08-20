# ZMK Studio runtime keymap extension

**Status:** design and implementation plan for the `studio-extension` branch.

## Goal

Make every configuration supported by the firmware live-editable through ZMK
Studio: bindings, layers, macros, combos, hold-taps, mod-morphs, tap-dances,
conditional layers, and input/mouse actions. The keyboard must persist the
result and report back exactly what is active.

This does **not** parse Devicetree source on the keyboard. Devicetree is a
build-time language. Instead, ZMK Map compiles its keymap model into a compact,
versioned runtime configuration bundle that the firmware validates and
interprets.

```text
ZMK Map / .keymap -> runtime configuration bundle -> Studio transport
                                                     -> stage + validate
                                                     -> persist + activate
                                                     -> checksum read-back
```

The firmware continues to contain the behavior engines; their definitions move
from compile-time Devicetree objects to runtime data. A genuinely new behavior
algorithm still requires firmware code, but ordinary shortcuts such as
`Cmd + Ctrl + Q`, macros, combos, and layer holds do not.

## Runtime model v1

```text
RuntimeConfig {
  schema_version
  revision
  layers[]
  bindings[]
  behavior_definitions[]
  macros[]
  combos[]
  checksum
}
```

Every record has a stable ID. Bindings and other records reference IDs rather
than Devicetree phandles. The initial supported behavior records are:

- simple key, modifier, layer, Bluetooth, and input/mouse actions;
- hold-tap: tap/hold bindings, timing, flavor, idle/quick-tap settings, and
  trigger-position rules;
- macro: ordered press/release/tap/wait instructions;
- combo: input positions, layers, timeout/release settings, and output binding;
- mod-morph, tap-dance, and conditional-layer records.

## Protocol

Add a versioned Runtime Keymap RPC family to `zmk-studio-messages`:

```text
GetRuntimeCapabilities
GetRuntimeConfig
BeginRuntimeUpdate
UploadRuntimeChunk
ValidateRuntimeUpdate
CommitRuntimeUpdate
AbortRuntimeUpdate
GetRuntimeConfigHash
```

`GetRuntimeCapabilities` reports protocol version, supported record types, and
firmware limits. `CommitRuntimeUpdate` is all-or-nothing: the keyboard only
activates a fully valid staged configuration. The client compares the returned
checksum with its submitted bundle before displaying a successful apply.

## Firmware implementation

1. Add fixed-size runtime pools behind Kconfig settings:

   ```text
   CONFIG_ZMK_RUNTIME_MAX_LAYERS
   CONFIG_ZMK_RUNTIME_MAX_BEHAVIORS
   CONFIG_ZMK_RUNTIME_MAX_COMBOS
   CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS
   ```

2. Decode uploads into a staging pool and validate all record IDs, layer and key
   positions, behavior parameters, ranges, and cross-references.

3. Serialize the committed configuration to ZMK settings/NVS. On boot, restore
   it before key processing begins.

4. Atomically replace the active runtime configuration only after persistence
   succeeds. Retain the prior configuration after a failed update.

5. Dispatch physical key events through runtime combo matching first, then
   runtime key bindings. Invoke existing behavior engines using runtime record
   data rather than Devicetree-only instances.

6. Keep compile-time keymap/Studio support as a fallback and provide an explicit
   restore-stock operation.

## Delivery order

1. Protocol definitions, generated bindings, capability handshake, and
   transactional upload/read-back.
2. Runtime simple bindings and direct composite HID shortcuts.
3. Runtime macros and combos.
4. Runtime hold-taps and layer holds.
5. Runtime mod-morphs, tap-dances, conditional layers, and input/mouse actions.
6. ZMK Map editor changes: creation/editing UI, capability-based controls,
   upload progress, and checksum verification.

## Totem acceptance tests

- Apply, reboot, and reload a five-layer Totem keymap without differences.
- Create a live `Cmd + Ctrl + Q` macro and verify the host receives it.
- Create, edit, and remove a multi-position combo.
- Create a hold-tap with custom timing/flavor/trigger positions and verify its
  tap and hold paths.
- Test mouse/input bindings and behavior parameter validation.
- Test central/peripheral split operation, including a temporarily disconnected
  peripheral half.
- Verify failed, oversized, or malformed updates preserve the prior keymap.

## Patch workflow

Maintain this as a focused ZMK fork patch series, pinned from
`config/west.yml`, alongside `zmk-ble.patch`:

```text
0001-studio-runtime-protocol.patch
0002-runtime-storage-and-transactions.patch
0003-runtime-macros-and-combos.patch
0004-runtime-hold-taps-and-behaviors.patch
0005-studio-runtime-capabilities.patch
```

The ZMK Map application changes should remain in its own repository, but every
firmware patch must retain wire compatibility with the matching Studio protocol
version.
