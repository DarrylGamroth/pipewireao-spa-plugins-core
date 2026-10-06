# Remote COPY queue Acquisition qualification

## Scope and identities

This is a bounded native software transport investigation, QRA001. The baseline
is `2cd248e9960dbddd352ad24e6cf37911fa877d8e`, on branch
`work/queue-remote-acquisition-20261006`, in the dedicated
`pipewireao-spa-plugins-core-queue-acquisition` worktree. Its baseline was clean.
The independent source review is
[QUEUE_REMOTE_ACQUISITION_REVIEW.md](QUEUE_REMOTE_ACQUISITION_REVIEW.md).

The installed native SDK source is
`42fdf86f4e66b15e7cc1d22404294f2234dfcb85`; its unchanged core library has
Build ID `4c4765657fbfa8726d729722af47057d05a150dd`. The baseline installed queue
module SHA-256 is
`08b4cd64c9da848937a3124c606ba7e2504e87869b1be625ad3678a615563149`.
This correction changes the queue module only. No native core, scientific
algorithm, plant configuration, model rate, transport, or installed artifact was
changed during these tests.

All native test processes and their inherited data loops were restricted to
CPU15. Scientific and vendor workloads on CPUs3/4/6/8/10/14 were left to their
owners. Each fixture owns a fresh private runtime directory, daemon, streams,
links, and queue. Teardown stops that daemon and removes that directory.

Evidence is retained under
`/home/dgamroth/.cache/rtc-queue-acquisition-20261006/`. `evidence.json` records
final source, binary, and log hashes. Meson test detail logs used for final gates
have a minimal inherited environment; they contain no session credentials.

## Public fixture

`src/modules/queue/test-remote-acquisition.c` uses public PipeWireAO/SPA APIs.
The daemon owns the queue module. An external client owns a synthetic producer,
a required consumer, and an independent observer. The required consumer is
linked first, establishing the producer's metadata pool before a passive
optional queue capture link is attached. This reproduces RTC's pool admission
order without executing scientific algorithms.

The synthetic payload is an 8×8 row-major U16_LE ndarray, schema
`org.pipewireao.test.queue.remote/1`, model format rate500Hz, fixed3 buffers.
The producer publishes64 samples, deliberately paced near10Hz; the main loop
manually polls the two independent graphs. This pacing is a functional fixture,
not an achieved-rate or latency qualification.

Each retained sample is checked against its exact64-word payload, Header
sequence/PTS/flags, and valid96-byte current Acquisition metadata with domain
`a0000000000000000000000000000000`, generation1, sequence1…64, explicit
monotonic timebase, matching exposure start, and duration2,000,000ns. The public
stream loan is released with `pw_stream_queue_buffer`. Public Busy counters
before/after release and locally assigned buffer ordinals are recorded as
diagnostics. These ordinals are client-local identities, not native buffer IDs.

Both queue nodes are bound through the public registry and their NodeInfo
property updates are retained. The logs include global IDs/serials, queue ID,
actual driver IDs and loop selectors, capture/requested/installed generations,
configured state, pool counts, active outputs, pending/completion depths, and
publications/deliveries/completions/replacements/protocol errors/pool exhaustion.
Busy and queue counters are diagnostic measurements; pass conditions are all64
exact publications and deliveries at both consumers, zero client errors, and
normal fixture teardown.

The queue is COPY, capacity1, drop-oldest. Its capture and playback loop names
are distinct and at most15 characters. The shared-loop control changes only the
playback selector. The Header-only control removes Acquisition negotiation and
validation. The pause control stops producer triggers at32, sends public Pause
and Start commands to the queue input, verifies input-stream `paused` and stable
required delivery count during the pause, then completes all64 exact samples.

## Failure evidence and correction

`fifth.log` first reproduced the valid remote topology failure: producer64,
required consumer64, observer1, all client errors0. The only observer receipt was
exact sequence1, and its Busy diagnostic changed1→0 on release. Queue pool
counts were3/3, all three generations1, configured true, active outputs0,
pending depth1, publications64/deliveries1/completions1/replacements62,
protocol errors0 and pool exhaustion0. Header-only and shared-loop controls
failed identically, excluding those features as sufficient explanations.

`stream-debug.log`, using `PIPEWIREAO_DEBUG=2,pw.stream:4`, maps the daemon's
queue output to pointer `0x556958860ba0`. The native SDK repeatedly reports
`update_requested(): no free buffers 3` for that exact stream. The queue had
retained all unused output loans outside the stream's free ring. Native output
`call_process()` therefore withheld subsequent callbacks, despite pending
input. This is a confirmed forward-progress defect; it is not evidence of a
scientific or payload corruption defect.

The correction tracks every dequeued output loan separately from generation
eligibility. It drains native free buffers first, selects/publishes at most one
output, then returns unused output loans through public `pw_stream_return_buffer`.
It also returns unused loans in the ownership playback transition. Returns are
outside the drain loop because the native API inserts them at the ring front.
Selected in-flight outputs, input completion semantics, generation fences,
failed-return ownership, and teardown guards are preserved.

The final unchanged fixture is also run against the installed baseline module
in `fail-before-final.log`, and against the corrected module in the final Meson
detail logs. Final gate outcomes and module identities follow below.

## Retained rejected setups and separate findings

- `first.log`: missing SDK shared-library search path, before daemon admission.
- `second.log`/`third.log`: isolated source→queue negotiation lacks Acquisition
  because queue capture advertises Header; this is insufficient to reproduce
  RTC's previously admitted required source pool. The fixture adds the required
  consumer before optional linking, rather than changing production metadata.
- `fourth.log`: implicit free-port selection cannot fan out an already-linked
  source. The final fixture uses exact public registry port IDs.
- `trace.log`: the generic `PIPEWIRE_DEBUG` variable was ignored; the SDK uses
  `PIPEWIREAO_DEBUG`. This log is not callback trace evidence.
- `sanitize-gates-initial*.log`: six tests passed; live setup aborted before
  queue creation because the minimal environment omitted a runtime directory.
  The rerun supplies an owned0700 runtime directory.
- `pause-setup-rejected.log`: a manual fixture command omitted the installed
  module fallback directory, before daemon admission.
- `pause-state-assumption-rejected.log`: source `set_active(false)` does not
  promise a public stream PAUSED notification; that test assumption was removed.
- `pause-mixed-activation-failed.log`: combining source active toggles with
  direct queue input Pause/Start did not restore graph completion. Cause and
  supported control semantics remain unresolved; no production change was made
  for that experiment. The isolated Pause/Start control stops manual ingress
  triggers and passes independently.

Final logs retain SDK warnings about unavailable D-Bus, initial missing format,
and mixer setup. These warnings are not discarded or established as causes of
QRA001. Installed RTC observer delivery, stall/kill/reattach isolation, generation
reset, endpoint replacement, and whole scientific equivalence remain separate
issue3 acceptance gates. No hardware or real-time performance claim is made.

## Final software gates

| Gate | Outcome | Durable evidence |
| --- | --- | --- |
| Identical final release client, baseline installed queue | FAIL:64 producer/64 required/1 observer, zero client errors | `fail-before-final.log` |
| Debug queue tests | PASS10/10 | `debug-ten.log`, `debug-ten-detail.log` |
| ASan+UBSan, leak detection and abort on error | PASS10/10 | `sanitize-ten.log`, `sanitize-ten-detail.log` |
| Release queue tests | PASS10/10 | `release-ten.log`, `release-ten-detail.log` |
| Wrapper rejects abnormal daemon exit after client success | PASS: daemon exit42 forces fixture exit1 | `teardown-negative.log` |
| Corrected release, strict daemon teardown check | PASS64/64/64, daemon exit0 | `strict-teardown-after.log` |

Each10-test suite includes the previous lifetime/state/buffer/module/live tests,
existing remote lease test, and four external COPY controls: current Acquisition,
Header-only, queue input Pause/Start, and shared server loop. The live test
includes COPY/LEASE overflow/backpressure, held output, observer reconnection,
format recreation, pool generation replacement, and eight context-first
teardowns spanning empty/queued/in-flight/backpressure occupancy. The private
lifetime test also retains the stats callback and queued invocation regressions.
The independent review verifies final results and their actual scope.

All three profiles compile the same production source SHA-256:
`0ab50a7ed8c503cf508316e81538249527eab3794b04b6577acabecfe222dd44`.
Compiled queue module SHA-256 values are:

- Debug: `c5c2353a1f990c19ce99c35a91ca8538c964349dce1d157b65154673ca7f6afc`.
- ASan+UBSan: `c055037fa1ddb1cd0ce1a7226d40ae4129fc8809651c71a1f71a3088f9bd9a69`.
- Release: `8f953946733fafa3aaa766518ba3c2128fd2e6977babe3b8d478050f20664835`.

Builds are task-owned `build/`, `sanitize/`, and `release/` directories under the
evidence root. Release uses Meson `--buildtype=release`, tests enabled, and
compiles only the eight C queue targets. No Cargo targets were built. Final
release configuration uses the installed SDK pkg-config files directly; test
configuration falls back to `prefix/share/pipewire-ao` when that SDK file omits
`confdatadir`. Daemon discovery uses `prefix/bin/pipewire-ao` so remote tests are
registered rather than silently skipped by looking under the config directory.

For the sanitizer daemon, `PIPEWIREAO_QUEUE_TEST_PRELOAD` supplies the compiler's
`libasan.so` before loading the instrumented queue module. Instrumented clients
already link that runtime. This affects only the owned test daemon. Final suites
run serially with an owned0700 `XDG_RUNTIME_DIR`; sanitizer options enable leak
detection and halt/abort on error. Shell and compiler checks, and `git diff --check`, also pass. No installation, merge, push, or scientific cohort is part
of this commit.
