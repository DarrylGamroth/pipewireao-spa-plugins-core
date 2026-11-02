# pyRTC shared-memory bridge

This plugin connects ordinary PipeWireAO ndarray ports to the CPU
`ImageSHM` transport used by pyRTC. It exports two SPA factories:

- `api.pyrtc.source` reads an existing pyRTC shared-memory stream;
- `api.pyrtc.sink` writes PipeWire buffers to a pyRTC stream.

The plugin implements the pyRTC CPU shared-memory boundary directly and does
not load Python, NumPy, or pyRTC. GPU-handle streams are outside this bridge.

## Factory properties

Both factories require:

| Property | Meaning |
| --- | --- |
| `api.pyrtc.name` | pyRTC `ImageSHM` name, without a slash. |
| `api.pyrtc.schema` | Exact ndarray semantic schema published or accepted by the node. |
| `api.pyrtc.profile` | Optional deployment annotation published on the node; it does not participate in ndarray format negotiation. |

The source always attaches to an existing stream. The sink accepts
`api.pyrtc.access=create|attach`:

- `create` is the default. The selected ndarray format determines the shared
  memory size, NumPy dtype index, and shape. The node owns both objects and
  unlinks them during teardown. Creation refuses to replace either existing
  object.
- `attach` opens an existing exact-compatible stream, never unlinks it, and
  assumes the surrounding system has assigned exactly one writer.

## CPU shared-memory boundary

For a name `wfs`, pyRTC creates `/wfs` for the contiguous NumPy payload and
`/wfs_meta` for ten native `float64` metadata values:

| Index | pyRTC meaning |
| --- | --- |
| 0 | write count |
| 1 | realtime write timestamp in seconds |
| 2 | payload size in bytes |
| 3 | index in `pyRTC.utils.NP_DATA_TYPES` |
| 4–9 | shape dimensions, followed by zero values |

The bridge supports one through five positive dimensions and the pyRTC dtype
indices for `bool`, signed and unsigned 8/16/32/64-bit integers, IEEE
`float16`/`float32`/`float64`, and `complex64`/`complex128`. The corresponding
SPA ndarray is contiguous and row-major with the same shape. The five-dimension
limit reserves the zero terminator required by pyRTC's metadata reader. Only
little-endian hosts are currently supported because multi-byte SPA element
types have explicit little-endian storage.

The source polls the write timestamp and publishes one initial snapshot, then
one snapshot after each observed timestamp change. It reads the count with each
snapshot and carries that value as Header sequence. Header PTS remains invalid
because pyRTC records an absolute realtime timestamp rather than the active
PipeWire graph clock.

Source buffers require Header and version-1 BufferProperties, with a 512-byte
Properties default and a 4064-byte maximum, reserving the Header's 32 bytes in
the 4096-byte metadata budget. Each publication clears that Object to valid empty metadata:
the pyRTC ABI supplies no physical acquisition identity, exposure, electrical or
optical facts. The bridge does not manufacture them from its write counter or
realtime timestamp.

pyRTC `ImageSHM.write()` copies the payload before updating the count and
timestamp and does not expose a writer-in-progress marker or seqlock. The
source retries when metadata changes during its copy, but the pyRTC ABI cannot
guarantee a tear-free snapshot if a writer is modifying the payload while the
old metadata values remain visible. This is the same synchronization limit as
pyRTC's own non-blocking CPU reader. Deployments that require coherent frames
must schedule the copy away from the producer write or use a transport with an
explicit publication protocol.

The sink copies one complete input buffer into the payload and then updates the
count and realtime timestamp. Both directions therefore contain one deliberate
payload copy.
