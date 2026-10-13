# Raw-video queue Acquisition negotiation

## Scope and source identities

This bounded interoperability correction starts from clean core `main`
`2908ed317b57b41eed64edd8633c6208c7c7ad9e` in the dedicated worktree
`pipewireao-spa-plugins-core-video-acquisition`, branch
`codex/queue-video-acquisition-20261013`. The older canonical core worktree stays
on `ac82518cc876cb6fce8baf55fc6875f8fba0f71f`, branch
`codex/preserve-sdk-latest-hold-20260930`, with its unrelated ndarray changes
untouched. Exact starting status is retained in the evidence root's
`baseline.json`.

Evidence root:
`/home/dgamroth/.cache/gui-shape-genicam-20261013/core-queue/`.
The cache-only build uses the installed `/opt/pipewireao` pkg-config files,
headers, libraries, and daemon, CPU15 and one build/test worker. No installed
artifact, SDK source, live RTC daemon, or camera setting is changed by this
work. These tests use synthetic sources and private fixture-owned daemons.

## Observed defect and correction

The physical camera investigation separately observed an eGrabber source's
three raw-video buffers containing Header32 but lacking its required
Acquisition96 metadata. The source rejected those buffers. The bounded physical transport result below belongs to the parent integration
task; it does not establish a real-time performance qualification.

The queue capture parameters advertised optional Acquisition metadata only for
NdArray. `update_capture_params` now makes that existing declaration for both
supported media profiles, NdArray and raw video. The metadata type remains
optional. Its declared feature value remains the public
`SPA_META_FEATURE_ACQUISITION_CURRENT`, equal to the V2 feature bit `2`, with a
mandatory feature property and record size `sizeof(struct spa_meta_acquisition)`
(96 bytes in the installed SDK). Producers that advertise Header only can
still negotiate. The change does not infer or create acquisition identities,
registry entries, timebase, calibration, exposure fields, or ownership fields.

The preallocated input/output pools, finite capacity, publication callback,
copy and lease paths, Busy handling, and output metadata forwarding are
unchanged. If Acquisition is negotiated, its added storage is 96 bytes per
buffer per pool; the capacity-one test uses three input and three output
buffers, hence 576 additional Acquisition record bytes. This excludes metadata
descriptors, alignment and allocation overhead; negotiated feature metadata
also adds a zero-size feature descriptor. This is a storage calculation,
not a measured timing claim.

Relevant requirements are [QUEUE-001/002/004/005/006](queue.md): ordinary
complete buffers, finite capacity, storage modes, bounded preallocated producer
work, and preservation of negotiated metadata. The production change is
limited to parameter negotiation; the tests exercise existing transfer paths.

## Discriminating regression

The existing remote Acquisition fixture links a required consumer directly to
the producer before attaching the queue. That admission order establishes an
Acquisition pool and masks the raw-video omission: the initial video extension
passed before the correction (`video-fail-before.log`). This result is retained
as a rejected regression topology, not failure evidence.

New `--queue-only` cases leave that direct consumer unlinked, so the queue is
the producer's first consumer. The same raw-video V2 case fails before the
production edit (`video-queue-first-before.log`): producer buffers have Header32
and no Acquisition, producer and observer publications/deliveries remain zero,
and both reject 119 samples. The unchanged case passes after the correction;
producer and observer buffers contain Acquisition96, and all 64 samples arrive
exactly, with zero client errors and normal daemon teardown.

The four new profiles use exact 8×8 GRAY16_LE raw video at a declared rate of
500 Hz, a three-buffer source pool, queue capacity1/drop-oldest, and separate
capture/playback loops. The original fixture remains an 8×8 U16_LE NdArray with
its explicit schema, element type, layout, and shape. Both contain 128 payload bytes with
stride16. The synthetic producer is paced near10 Hz and the test manually
triggers graphs; the declared format rate is not achieved-rate evidence.

The existing receipt oracle checks every payload word, chunk offset/size/stride,
Header sequence/PTS/flags, and, when negotiated, V2 Acquisition validity,
domain/generation/sequence, flags, monotonic timebase, and exposure start and
duration. Busy counts and queue counters remain diagnostics. Header-only
profiles omit Acquisition negotiation and semantic validation, proving the
new declaration does not require producers to supply that record.

## Verification

| Gate | Result | Evidence |
| --- | --- | --- |
| Queue-first raw-video V2, copy, before production edit | FAIL | `video-queue-first-before.log` |
| Queue-first raw-video V2 and Header-only, copy and lease | PASS4/4 | `video-after.log`, `video-after-details.log` |
| Final restored queue suite, including the four added video cases | PASS14/14 | `queue-final-suite.log`, `queue-final-suite-details.log` |

Build configuration:

```sh
env PKG_CONFIG_PATH=/opt/pipewireao/lib/x86_64-linux-gnu/pkgconfig:/opt/pipewireao/share/pkgconfig \
    LD_LIBRARY_PATH=/opt/pipewireao/lib/x86_64-linux-gnu:/opt/pipewireao/lib \
    taskset -c 15 meson setup EVIDENCE/build WORKTREE -Dtests=enabled
```

Compile the module and seven C queue test executables with `meson compile -j 1`.
Run the registered `pipewireao-queue-*` tests serially using `meson test
--no-rebuild --print-errorlogs --num-processes 1` and the same installed library
path. The remote wrapper also exports `PIPEWIREAO_RUNTIME_DIR` to its owned
runtime directory, in addition to legacy and XDG runtime selectors. The SDK's
current runtime selector therefore remains isolated even if inherited from a
caller.

Shell syntax and `git diff --check` also pass.

The final module and source hashes are recorded in `evidence.json`. This bounded
run does not repeat earlier sanitizer or stress qualifications, measure timing,
or claim general physical-camera qualification. The bounded follow-up below
records the separate camera transport observation. No SDK installation or
live RTC change is part of this work.

## Bounded physical camera follow-up

The corrected module and production GUI adapter were observed on the isolated
ChinaVision GE34GM camera graph, serial `055070423014`. The initial client-owned
Julia queue delivered one frame per observer attachment, then stalled at its
output. An active read-only dump showed capture progressing, a ready output
pool, no active output loan, pending depth 1, and zero queue protocol errors.
This excludes a complete ingress stall; its output failure mechanism remains
unresolved. No further production queue change is justified by that trace.

Cache-only synthetic controls pass with a zero declared frame rate in a
daemon-owned copy queue, and with fixed and zero rates in client-owned copy and
lease queues. Zero frame rate or client ownership alone is insufficient to reproduce
the physical failure. Their investigative patch is retained as
`queue-stall-synthetic.patch`; these extra options are excluded from the
production diff. A cache-only explicit-loop client fixture remains available
for a later controlled experiment. It was not run against the camera because
the working GUI graph is being kept available.

A daemon-owned corrected lease queue with capture loop `rtc` (CPU14 busy-spin)
and playback loop `ordinary` passes the same production adapter hardware test:
79 descriptors and 79 current values, followed by three successive 640×480
Gray8 frames of 307200 bytes, in 0.62 seconds. Queue statistics report zero protocol
errors and continued ingress. Evidence is under the sibling `camera/` directory:
`native-daemon-queue-ready-stream.log`, `daemon-queue-ready-dump.json`,
`daemon-queue-settings-comparison.json`, and the daemon/source launch JSON files.
Daemon placement and explicit loop selection changed together; this test does
not distinguish which difference resolves the original client fixture stall.

The exact baseline/current property comparison finds 75 of 79 observed values
unchanged. Differences are DeviceTemperature, GevPrimaryApplicationSocket,
GevSCPHostPort, and TLParamsLocked. TriggerMode 0, ExposureTime 2500 µs, Gain 2.5,
Mono8, width 640, height 480, ReverseX=false, and ReverseY=true remain unchanged.
The adapter test sends no property commands. Source construction and graph
linking start SDK acquisition and allocate transport resources, as expected by
the authorized integration task.

Read-only `ss` peer mapping proves both original Julia fixture PipeWire
connections terminate at the private daemon socket
`/run/user/1000/gui-egrabber-20261013/pipewire-ao-0`; evidence is
`camera/socket-isolation-peers.txt`. A separately observed live RTC service
failure is not attributed to this graph. No RTC service operation was performed
by this worker. The task-owned camera graph remains running for GUI inspection.
