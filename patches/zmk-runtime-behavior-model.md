# Runtime behavior-object model for ZMK

**Status:** design draft. This supersedes the feature-by-feature runtime bundle
as the target architecture. The existing storage and simple-binding prototype is
useful as a transport/persistence experiment, but is not the format to extend
for full live configuration.

## Objective

Allow ZMK Studio or ZMK Map to edit every *supported* keymap behavior live,
without compiling or parsing Devicetree on the keyboard.

The firmware still compiles the behavior implementations. Runtime configuration
creates and connects data instances of those implementations. A new C behavior
algorithm, hardware driver, display integration, GPIO feature, or BLE feature
still requires a firmware build; a new macro, combo, layer hold, or tap-dance
instance does not.

## Boundary between firmware and runtime data

| Compiled / Devicetree-defined | Runtime flash database |
| --- | --- |
| Matrix, GPIO, display, battery, BLE, split transport and physical layout | Layer names, enablement, and bindings |
| Behavior engines, their stable type IDs, schemas, and bounded resource pools | Behavior objects and parameters |
| HID/host output, timer/event scheduler, split locality rules | Macro programs, combos, and relationships between objects |
| Maximum counts, byte limits, validation rules, recovery keymap | Active runtime configuration revision |

Devicetree may continue to declare which engines are enabled and the resource
limits for a keyboard. It must not be the only place an individual behavior
instance can be defined.

## Model

```text
physical position event
        |
        v
runtime input resolver -- combo matcher --> active-layer resolver
                                             |
                                             v
                                      binding -> object ID
                                                     |
                                                     v
                                  behavior registry / execute engine
                                                     |
                                                     v
                                             ZMK events + HID
```

The runtime configuration is a single, versioned database. Its records are
referenced by stable numeric IDs rather than Devicetree phandles:

```text
RuntimeConfigHeader {
  magic, wire_version, schema_version, revision, payload_size, crc32
  layer_count, binding_count, object_count, combo_count, macro_step_count
}

Layer         { layer_id, flags, name_offset }
Binding       { layer_id, position, object_id }
BehaviorObject{ object_id, type_id, object_version, flags, payload_offset, payload_size }
Combo         { combo_id, flags, timeout_ms, position_set, output_object_id }
MacroStep     { opcode, argument }
```

`BehaviorObject.payload` is type-specific, length-delimited, and decoded only
by the matching compiled engine. Unknown types or versions are rejected; they
are never silently ignored.

The exact v2 envelope, canonical layout, type-ID allocation, capability
requirements, and transaction rules are defined in
[`zmk-runtime-config-v2-schema.md`](zmk-runtime-config-v2-schema.md). That
contract must be accepted before any v2 firmware or ZMK Map implementation is
started.

### Initial stable behavior types

1. `KEYCODE` — HID key/consumer/mouse usage plus modifiers.
2. `LAYER` — momentary, toggle, default, and one-shot layer operations.
3. `MACRO` — bounded press, release, tap, wait, and invocation instructions.
4. `HOLD_TAP` — tap object, hold object, flavor, timing, and trigger rules.
5. `TAP_DANCE` — bounded tap-count state machine and child object IDs.
6. `COMBO_OUTPUT` — output behavior object; combo matching is a configuration
   table evaluated before normal bindings.

Mod-morph, conditional layers, Bluetooth/output selection, and input actions
should use the same object/registry mechanism after the six types above are
proven. Hardware-control objects must state their split locality explicitly.

## Behavior registry and executor

The compiled registry maps `type_id` to an engine descriptor:

```text
BehaviorEngine {
  type_id, supported_object_versions, validate(payload),
  begin(press_context), release(press_context), cancel(press_context),
  required_capabilities, locality
}
```

The executor owns fixed, bounded per-press state. On press it records the exact
binding/object revision used; release and delayed callbacks use that recorded
context even if a new configuration is committed meanwhile. A configuration
commit therefore never leaves a held modifier, hold-tap, macro, or combo in an
ambiguous state: it first cancels/releases the old executor state, then swaps
the active database atomically.

Combo matching is not merely another key binding. It intercepts position events
before active-layer binding resolution, has a timeout and release policy, and
must either consume or replay its candidate presses deterministically. Hold-tap
and tap-dance likewise need timer-owned execution contexts. These state machines
are why a generic runtime registry is required instead of a parser that calls
arbitrary Devicetree behaviors.

For a split keyboard, central-side execution remains authoritative. Runtime
events carry source half/position and timestamp; engines with source or global
locality use the existing split transport rather than executing stateful logic
independently on each half.

## Persistence, validation, and recovery

- Use bounded fixed pools selected at build time; no runtime code loading and no
  unbounded heap allocation.
- Upload one complete database in chunks to RAM, validate all IDs, references,
  sizes, timers, graph cycles, and resource quotas, then write it atomically to
  an inactive NVS slot with a CRC-protected header written last.
- Retain the current active database on any validation, write, or boot-time
  integrity failure.
- Keep a compile-time recovery keymap and an RPC-only `Restore runtime config`
  action that removes this database without clearing bonds or unrelated settings.
- Advertise exact registry types, object versions, and resource limits through
  Studio capabilities. A client must disable unsupported editors rather than
  approximate the requested behavior.

## Migration

1. Keep Devicetree keymaps as the stock/recovery configuration.
2. Add the generic database envelope, registry metadata, capability report, and
   transaction storage without changing normal key dispatch.
3. Implement `KEYCODE` through the generic object interface and verify that a
   one-object runtime map can be applied and removed safely.
4. Add `LAYER` and `MACRO` engines using the same executor and per-press state.
5. Add the combo matcher, then hold-tap and tap-dance state machines.
6. Make ZMK Map translate its internal model to this database and read it back
   exactly; do not treat a `.keymap` text file as the wire format.

Each step has a static fallback and a hardware acceptance test. The current
simple-binding prototype should be retained only as a test fixture or replaced
by step 3; its `RKB1` format must not become public API.

## First implementation milestone

Implement the **registry skeleton plus `KEYCODE` object**:

- a compiled registry with one `KEYCODE` descriptor;
- object IDs and a layer/position-to-object binding table;
- capability reporting of supported type/version/limits;
- transaction validation and atomic activation using the existing storage work;
- exact read-back, clear/recovery, held-key swap, reboot, and split-half tests.

This validates the permanent architecture while retaining the low-risk behavior
already proven in hardware. No macros or combos are added until this vertical
slice works through the real Studio transport.
