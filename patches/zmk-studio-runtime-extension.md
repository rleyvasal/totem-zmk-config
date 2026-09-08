# ZMK Studio runtime keymap prototype

**Status:** superseded as the target format by
[`zmk-runtime-behavior-model.md`](zmk-runtime-behavior-model.md). The
capability handshake, bounded transaction/persistence layer, and one-layer
simple-key binding executor remain useful prototype work, but this custom
bundle must not grow feature-by-feature into the public runtime format.

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

The implemented v1 subset has a fixed header followed by one complete layer of
four-byte encoded keyboard usages. The upper byte is ZMK's normal modifier mask,
so `Cmd + Ctrl + Q` is a single record; zero is an explicit empty binding. The
executor stages at most 2 KB in 32-byte chunks, verifies CRC-32 and every record,
then persists to the inactive one of two NVS slots. The slot header is written
last; after a failed write or power loss, boot selects the previous valid slot.
It emits the same keycode events as `&kp`, and releases held runtime keys before
atomically switching to a new map.

This is intentionally **one layer of simple keyboard usages only**. Macros,
combos, hold-taps, layers, consumer/mouse actions, and behavior definitions are
not accepted by v1 and are not advertised as runtime features.

Future records will have stable IDs and refer to each other rather than
Devicetree phandles. The planned behavior records are:

- simple key, modifier, layer, Bluetooth, and input/mouse actions;
- hold-tap: tap/hold bindings, timing, flavor, idle/quick-tap settings, and
  trigger-position rules;
- macro: ordered press/release/tap/wait instructions;
- combo: input positions, layers, timeout/release settings, and output binding;
- mod-morph, tap-dance, and conditional-layer records.

## Protocol

The implemented Runtime Keymap RPC family provides:

```text
GetRuntimeConfigInfo
BeginRuntimeUpdate
UploadRuntimeChunk
ValidateRuntimeUpdate
CommitRuntimeUpdate
AbortRuntimeUpdate
GetRuntimeConfigChunk
ResetRuntimeConfig
```

`ResetRuntimeConfig` erases only `runtime_keymap/*` persistence and immediately
returns to the compile-time keymap; unlike Studio's global reset, it does not
clear unrelated live keymap settings. `RuntimeCapabilities` remains in the
existing `get_keymap` response.
`CommitRuntimeUpdate` is all-or-nothing: the keyboard only activates a fully
received, checksum-valid bundle after its inactive persistent slot is complete.
The client will compare the returned checksum with its submitted bundle before
displaying a successful apply.

## Firmware implementation

1. **Implemented:** add bounded Kconfig storage for a 2 KB bundle and 32-byte
   chunks, a staged upload, CRC validation, two NVS slots, boot-time validation,
   and header-last atomic activation. A Studio settings reset clears both slots.

2. **Implemented (limited):** define a fixed bundle header and exactly one
   complete layer of validated simple keyboard bindings. Intercept physical
   positions before the Devicetree keymap and emit standard ZMK keycode events.
   A key release remains associated with the mapping that was active on press.

3. Extend the format into fixed-size behavior pools:

   ```text
   CONFIG_ZMK_RUNTIME_MAX_LAYERS
   CONFIG_ZMK_RUNTIME_MAX_BEHAVIORS
   CONFIG_ZMK_RUNTIME_MAX_COMBOS
   CONFIG_ZMK_RUNTIME_MAX_MACRO_STEPS
   ```

4. Validate all record IDs, layer and key
   positions, behavior parameters, ranges, and cross-references.

5. Dispatch physical key events through runtime combo matching first, then
   runtime key bindings. Invoke existing behavior engines using runtime record
   data rather than Devicetree-only instances.

6. Keep compile-time keymap/Studio support as a fallback and provide an explicit
   restore-stock operation.

## Delivery order

1. **Implemented:** protocol definitions, generated bindings, capability
   handshake, and transactional upload/read-back.
2. **Implemented (one layer):** runtime simple bindings and direct composite
   keyboard HID shortcuts.
3. Runtime macros and combos.
4. Runtime hold-taps and layer holds.
5. Runtime mod-morphs, tap-dances, conditional layers, and input/mouse actions.
6. ZMK Map editor changes: creation/editing UI, capability-based controls,
   upload progress, and checksum verification.

## Totem acceptance tests

Completed pre-flash checks for the storage and simple-binding increments:

- host transaction test of the exact production source, including forced
  mid-commit failure, malformed-bundle rejection, and simulated reboot recovery;
- host executor test of normal keys, `Cmd + Ctrl + Q`, and a live map swap while
  a key is held;
- Totem left-half `xiao_ble//zmk` Studio build: 279.5 KB flash / 86.0 KB RAM.

The simple-binding image remains unflashed until a dedicated hardware smoke
test is approved. The smoke test uploads a harmless all-`B` bundle, checks the
exact chunked read-back, and invokes `ResetRuntimeConfig` to restore the stock
keymap without changing other Studio settings.

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
0001-studio-runtime-capabilities-messages.patch
0002-studio-runtime-capabilities-firmware.patch
0003-runtime-storage-messages.patch
0004-runtime-storage-firmware.patch
0005-runtime-simple-bindings-messages.patch
0006-runtime-simple-bindings-firmware.patch
0007-runtime-macros-and-combos.patch
0008-runtime-hold-taps-and-behaviors.patch
```

The ZMK Map application changes should remain in its own repository, but every
firmware patch must retain wire compatibility with the matching Studio protocol
version.
