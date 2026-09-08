# RuntimeConfig v2: database and registry contract

**Status:** Step 1 design contract. No key dispatch changes are authorized by
this document. `RuntimeConfig v2` replaces the experimental `RKB1` bundle as
the public runtime database format.

## Versioning

Three independent versions prevent an editor, database, and behavior engine
from being conflated:

| Version | Meaning | Rule |
| --- | --- | --- |
| Studio RPC protocol | Messages/transport used to stage and read a database | Add protobuf fields only; never reuse a field number. |
| RuntimeConfig format | Envelope, tables, IDs, and canonical byte layout | The first public format is `2`; unknown format versions are rejected. |
| Object version | Payload schema for one `type_id` | A registry advertises each supported version range. |

The device, not the editor, is authoritative about all supported format and
object versions.

## Encoding rules

- All integers are unsigned little-endian unless a field explicitly says
  otherwise.
- All structures are packed; there is no pointer, native alignment, or
  architecture-dependent field.
- All offsets are byte offsets. Object payload offsets are relative to the
  payload region; all other offsets are relative to byte zero of the database.
- All reserved fields and currently-unassigned flag bits must be zero.
- Object ID `0` means `NONE`; it cannot appear in the object table.
- The database is canonical: table regions are adjacent, records are sorted by
  ascending ID, and object payloads are adjacent in that same order. A decoder
  rejects gaps, overlaps, duplicate IDs, and trailing bytes.
- Layer names appear in layer order, concatenated without gaps or NUL bytes in
  the UTF-8 string region. Empty names have both offset and length set to zero.

## Fixed envelope

`RuntimeConfigHeader` is exactly 64 bytes:

| Offset | Size | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | ASCII `ZRC2` |
| 4 | 2 | `format_version` | Must equal `2` |
| 6 | 2 | `header_size` | Must equal `64` |
| 8 | 4 | `total_size` | Exact byte length uploaded and persisted |
| 12 | 4 | `revision` | Nonzero, monotonically increasing for one device |
| 16 | 8 | `layout_fingerprint` | Must equal the active firmware layout capability |
| 24 | 4 | `crc32` | CRC-32/IEEE of `[0, total_size)`, with these four bytes zeroed |
| 28 | 2 | `key_count` | Must equal the firmware capability |
| 30 | 2 | `layer_count` | `1..max_layers` |
| 32 | 2 | `object_count` | `0..max_objects` |
| 34 | 2 | `flags` | Zero in v2.0 |
| 36 | 4 | `layers_offset` | Start of `LayerRecord[layer_count]` |
| 40 | 4 | `bindings_offset` | Start of `uint16_t[layer_count * key_count]` |
| 44 | 4 | `objects_offset` | Start of `BehaviorObject[object_count]` |
| 48 | 4 | `payload_offset` | Start of the object payload region |
| 52 | 4 | `strings_offset` | Start of the UTF-8 string region |
| 56 | 4 | `strings_size` | Byte size of that region |
| 60 | 4 | `reserved` | Zero |

`layout_fingerprint` is a firmware-published 64-bit identifier for the exact
ordered physical key layout. ZMK Map reads it from the device and copies it into
an upload; it does not calculate or guess it. This prevents a configuration for
another 38-key layout from being silently applied merely because its key count
matches.

The regions are exactly, in order:

```text
[64-byte header]
[LayerRecord × layer_count]
[uint16 object ID × (layer_count × key_count)]
[BehaviorObject × object_count]
[object payload bytes]
[UTF-8 strings]
```

Thus:

```text
layers_offset   = 64
bindings_offset = layers_offset + layer_count * 8
objects_offset  = bindings_offset + layer_count * key_count * 2
payload_offset  = objects_offset + object_count * 12
strings_offset  = payload_offset + sum(object.payload_size)
total_size      = strings_offset + strings_size
```

All multiplication and addition is overflow-checked before bounds checks.

## Tables

### LayerRecord — 8 bytes

| Offset | Size | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 2 | `layer_id` | Equal to its ordinal: `0..layer_count-1` |
| 2 | 2 | `flags` | Zero in v2.0 |
| 4 | 2 | `name_offset` | Relative to `strings_offset`; ignored if `name_size == 0` |
| 6 | 2 | `name_size` | UTF-8 byte count; no NUL terminator |

Layer `0` is the initial/default active layer. Layer activation state is runtime
executor state, not a persisted mutable bit in the record.

### Binding table — 2 bytes per position

The binding at `layer L`, physical position `P` is:

```text
object_id = bindings[L * key_count + P]
```

`0` is an explicit transparent/no-runtime binding. Any nonzero value must name
an object table record. There are no Devicetree phandles in this table.

### BehaviorObject — 12 bytes

| Offset | Size | Field | Rule |
| ---: | ---: | --- | --- |
| 0 | 2 | `object_id` | Unique nonzero ID, sorted ascending |
| 2 | 2 | `type_id` | Must exist in the compiled registry |
| 4 | 1 | `object_version` | Must be advertised for `type_id` |
| 5 | 1 | `flags` | Zero in v2.0 |
| 6 | 2 | `payload_offset` | Relative to `payload_offset` in the header |
| 8 | 2 | `payload_size` | Must meet the selected registry descriptor's limit |
| 10 | 2 | `reserved` | Zero |

Payloads are opaque to the envelope decoder. The selected registry descriptor
validates its own payload after the envelope validates size, bounds, and
cross-reference format.

## Registry IDs

The following core IDs are reserved. A type is usable only when its descriptor
is compiled in and advertised by the device.

| Type ID | Name | First payload version | Delivery step |
| ---: | --- | ---: | --- |
| `0x0001` | `KEYCODE` | 1 | Step 1 vertical slice |
| `0x0002` | `LAYER` | reserved | Layer step |
| `0x0003` | `MACRO` | reserved | Macro step |
| `0x0004` | `HOLD_TAP` | reserved | Hold-tap step |
| `0x0005` | `TAP_DANCE` | reserved | Tap-dance step |
| `0x0006` | `MOD_MORPH` | reserved | Later |
| `0x0007` | `CONDITIONAL_LAYER` | reserved | Later |
| `0x0008` | `OUTPUT_SELECT` | reserved | Later |
| `0x0009` | `INPUT_ACTION` | reserved | Later |

`0x0100..0x7FFF` is reserved for upstream ZMK additions; `0x8000..0xFFFF` is
experimental/private and must not be committed to a portable user database.
These IDs are provisional until an upstream RFC reserves them.

### KEYCODE object v1

The first executable type has one 4-byte payload:

```text
uint32_t zmk_encoded_keycode
```

It uses ZMK's existing encoded HID keyboard/consumer/mouse key representation,
including normal modifier bits. Its registry validator rejects zero, unsupported
usage pages, invalid modifier encodings, and values unavailable in the compiled
firmware. `Cmd+Ctrl+Q` is therefore data in one `KEYCODE` object, not a new
firmware behavior.

## Capability contract

The existing runtime capability response gains an append-only
`RuntimeConfigCapabilities` message before any v2 database is uploaded:

```text
RuntimeConfigCapabilities {
  repeated uint32 supported_format_versions
  uint64 layout_fingerprint
  uint32 key_count
  uint32 max_config_size
  uint32 max_layers
  uint32 max_objects
  repeated BehaviorTypeCapability behavior_types
}

BehaviorTypeCapability {
  uint32 type_id
  uint32 min_object_version
  uint32 max_object_version
  uint32 max_instances
  uint32 max_payload_size
  uint32 flags
}
```

The concrete protobuf field numbers are assigned only in the message-repository
patch, appended to existing messages. A client must require all of the
following before it enables an editor:

1. format version `2` is advertised;
2. the fingerprint and key count match the connected keyboard;
3. every used object type/version is advertised;
4. all configured object, layer, and byte counts fit the advertised limits.

The former broad `features` flags remain transitional discovery information and
are not sufficient authorization to upload a v2 database.

## Transaction rules

The current staged chunk transport remains the transport. For v2 it must gain
one optimistic-concurrency field:

```text
BeginRuntimeUpdate {
  format_version = 2
  base_revision  = active revision, or 0 when no database is active
  revision       = base_revision + 1
  total_size
  crc32
}
```

The keyboard rejects a begin request when `base_revision` does not equal its
active revision. It validates the header checksum against the transport
checksum, then validates the canonical envelope and every advertised behavior
object before writing the inactive persistent slot. On commit it cancels all
old runtime execution contexts, writes the new slot header last, and activates
the new database. A failed validation or write leaves the previous revision
active.

## Validation order

1. Stage size, chunk coverage, and transport CRC.
2. Header magic, version, zeroed fields, checksum, capacity, and layout match.
3. Exact contiguous table geometry and overflow-safe bounds.
4. UTF-8 names, ordinal layer IDs, object-ID uniqueness/order, and every
   binding reference.
5. Registry presence, type/object-version support, payload limit, and per-type
   payload validation.
6. Cross-object references, graph-cycle rules, timer/resource quotas, and
   split-locality restrictions as each engine is introduced.

Unknown values always fail closed. No partial database is ever activated.

## Step 1 acceptance tests

- Encode/decode a minimal two-layer `KEYCODE` object database to identical
  canonical bytes.
- Reject each invalid header/table invariant, mismatched fingerprint, duplicate
  ID, missing object reference, unknown type, unsupported type version,
  payload overlap, bad CRC, and stale `base_revision`.
- Prove interrupted persistence and reboot retain the previous active revision.
- With no active v2 database, verify that all normal Devicetree keymap behavior
  and Studio transport remain unchanged.
- Only after those tests, route a single safe `KEYCODE` object through the new
  registry on the Totem and prove apply/read-back/clear on both halves.
