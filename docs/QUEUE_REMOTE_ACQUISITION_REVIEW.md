# Remote queue delivery review

Date: 2026-10-06. Status: QRA-001 confirmed by source analysis and a mapped
daemon trace; primary authorized a targeted public buffer-return correction.
The correction has been independently verified: functional, release and
AddressSanitizer/UndefinedBehaviorSanitizer suites each pass all ten targets.
Installed RTC/scientific qualification remains separate.

## Scope and source state

Worktree: `/home/dgamroth/workspaces/codex/pipewire/pipewireao-spa-plugins-core-queue-acquisition`.
Branch: `work/queue-remote-acquisition-20261006`.
Starting revision: `2cd248e9960dbddd352ad24e6cf37911fa877d8e`.
The initial working tree contained only the fixture worker's untracked
`src/modules/queue/test-remote-acquisition.{c,conf,sh}`. This reviewer changed
only this review document. No production code, installed artifact, scientific
workload or vendor HEART code was changed or run by the reviewer.

Native PipeWire source: sibling `pipewire`, revision
`42fdf86f4e66b15e7cc1d22404294f2234dfcb85`, which the fixture worker identifies
with the unchanged installed SDK. Reported installed core build ID:
`4c4765657fbfa8726d729722af47057d05a150dd`.

The reviewed path is the C queue module's capture/playback callback and public
`pw_stream` buffer ownership interface. The fixture uses a daemon-owned COPY
queue, capacity 1, drop-oldest, three capture buffers and three playback buffers.
External clients own a producer, required sink and observer. The required link
first establishes the input contract, followed by optional passive fan-out
through the queue. Capture and playback initially use separate selected daemon
data loops; all processes/threads are restricted to CPU 15. Input consists of
64 bounded synthetic ndarray publications, Header and exact 96-byte current
Acquisition metadata. There are no scientific frames or physical devices.

The contract under review is forward progress after an idle interval with
available output capacity. An independently driven observer must be able to
receive later retained queue contents. Drop-oldest may replace pending frames;
it does not authorize permanent delivery starvation. No throughput, jitter,
allocation or hard deadline claim is evaluated. The native code's existing
bounded pools and C11 atomic synchronization are retained; no optimization or
memory-order relaxation is proposed.

## Evidence ledger

Evidence root: `/home/dgamroth/.cache/rtc-queue-acquisition-20261006/`.
The independent reviewer inspected the logs and corresponding source. The
fixture worker performed the bounded runs; this reviewer did not rerun them.

| Evidence | Observation | Interpretation |
| --- | --- | --- |
| `fifth.log` | Producer 64, required sink 64, observer 1; first observed sequence 1 and metadata/payload correct; observer Busy changes 1→0 after public release | Input and first transfer work; the observer returns its buffer |
| `fifth.log` | Queue publications 64, deliveries 1, completions 1, replacements 62, active outputs 0, pending depth 1, completion depth 0, three buffers per side, configured generation 1, no protocol errors | Queue retains a later input and has no active output; this alone does not identify the failing layer |
| `header-only.log` | Same 64/64/1 outcome without Acquisition metadata; pool exhaustion 0; both streams streaming | Exact Acquisition metadata is not necessary for the failure |
| `same-loop.log` | Same 64/64/1 outcome with capture and playback on `queue-capture`; pool exhaustion 0, configured/ready, no protocol errors | Separate daemon loops are not necessary for the failure |
| `stream-debug.log` | `PIPEWIREAO_DEBUG=2,pw.stream:4`; pointer `0x556958860ba0` is created as `queue output`; its data thread repeatedly logs `update_requested: no free buffers 3` while the same stalled state persists | Graph processing reaches the native output callback gate, which suppresses the module callback |

In `stream-debug.log`, line 1079 maps the pointer to `queue output`; subsequent
`update_requested` entries at that exact pointer continue for the bounded run.
The final client counters are 64 publications, 64 required deliveries,
1 observer delivery, 1,185 producer/observer trigger completions, and no client
errors. The script prints daemon shutdown. Logs can include the daemon output
twice because the failed-run path prints it before cleanup prints it again;
duplicate text is not an independent repetition.

## QRA-001 — Retaining every unused output loan suppresses later callbacks

Severity: high (P1). Confidence: high. Disposition: confirmed defect; targeted
remediation authorized by the primary, implemented by the fixture/remediation
worker and independently verified within the CPU software scope below.
The explanation below combines a
source-derived valid sequence with the mapped runtime trace.

Affected queue code:
[module-queue.c](../src/modules/queue/module-queue.c),
`reclaim_playback_buffers`, `playback_process`, and `find_copy_output`.
Relevant native implementation: `src/pipewire/stream.c`, `update_requested`,
`call_process`, and `impl_node_process_output` at the native revision above.

The module drains every available playback buffer using
`pw_stream_dequeue_buffer` and retains those application loans in its
`output_available` slots. `playback_process` may then find the pending input
ring empty and return while still owning all unused playback loans.

The native stream invokes the output process callback only if
`update_requested` returns a positive value. When its free `dequeued` ring is
empty, that function returns `using_trigger ? 1 : 0`. Queue playback does not
call `pw_stream_trigger_process`, so `using_trigger` is false. The module's
private `output_available` flags do not put those retained loans back into the
native free ring.

A valid failing sequence is:

1. Playback dequeues its free pool, transfers the first input and queues one
   output to the observer; unused outputs remain held by the module.
2. The observer consumes and returns that output. Playback later dequeues it,
   clears its in-flight bookkeeping and reduces `active_outputs` to zero.
3. The pending input ring is empty during that callback, so playback returns
   holding all three output loans, without publishing an output.
4. A later capture callback admits a new pending item. Further graph cycles
   reach `call_process`, but `update_requested` sees the empty native free ring
   and suppresses the module's playback callback.
5. No playback callback can use the module's available output slots. Capture
   continues replacing the one pending item, producing 62 replacements and a
   permanent 64/1 publication/delivery discrepancy in this fixture.

The mapped `no free buffers 3` trace distinguishes this mechanism from absent
graph activation. The module's lack of a capture-to-playback wakeup was an
initial hypothesis, but a missing graph wakeup is not required to explain this
run: repeated graph requests reach the suppressing gate. Adding a wakeup alone
without repairing or deliberately changing the output-loan protocol is not an
established correction.

The external observer's successful `trigger_done` events never proved that
the queue's own playback callback ran. Likewise, public `pool-exhaustions=0`
counts the module's private selection failure, not the native stream's empty
free-buffer ring. These are distinct resource observations.

## Ownership and publication constraints for remediation

| State/resource | Owner/writer | Consumer and publication rule |
| --- | --- | --- |
| Capture buffer and input token | Capture loop until pending publication | Playback acquires published token/slot; completion returns ownership |
| Pending ring | Capture publisher; playback consumer; drop-oldest may claim the oldest entry | Existing release publication/acquire observation and claim protocol must remain intact |
| Completion ring | Playback publisher; capture consumer | Retain existing completion ordering and exactly one capture return |
| `output_available`, `output_in_flight`, output loan | Playback loop during processing | Availability means an actual application-owned `pw_stream` loan, not merely a pointer to a pool entry |
| Native free output ring | Public stream dequeue/return/queue operations | A returned unused loan can be dequeued again; a published loan is unavailable until downstream returns it |
| Pool generation/storage | Existing configuration and quiescence path | No descriptor mutation while exported or in use; preserve the prior lifetime fixes |

The public API supplies `pw_stream_return_buffer`, documented as returning a
buffer unused so it is immediately available to dequeue again, with RT-safe
semantics. The selected native implementation balances the output Busy claim,
clears the application's dequeued flag, and restores both if insertion fails.
This is a suitable interface for a narrowly scoped ownership correction.

A candidate correction should release unused output loans before returning
from playback without a published buffer, or avoid acquiring them until work
requires them. Preserve any actual in-flight output and the existing immutable
generation contract. COPY can select any suitable free output; LEASE associates
an output with a specific capture slot, so changes shared with LEASE must retain
that exact association and FIFO claim semantics.

Returning an output loan requires clearing the module's private available flag
only after successful return. The module must not subsequently write or queue
that buffer until a fresh public dequeue grants ownership again. Never publish
an empty buffer merely to keep callbacks flowing. Avoid returning and immediately
re-dequeuing the same loan in one unbounded loop: bound work by the configured
pool (at most 64 slots), and separate acquisition/selection from return of unused
loans. Preserve protocol error reporting and cleanup if a public return fails.

Candidate review identified two additional implementation requirements. First,
`output_available` is generation-qualified and can be false even while the
application owns a newly dequeued loan. Track actual loan ownership separately
so unconfigured or obsolete-generation loans are also returned. Second,
`ownership_playback_stage` calls `reclaim_playback_buffers` outside the ordinary
process callback; it needs the same return-unused step on the playback loop
before advancing the transition, or pause/resume can recreate the starvation.

Changing native private `using_trigger` state or weakening the native callback
gate is outside this module correction. A public trigger mechanism would need
its own lifecycle/scheduling contract; the native implementation detail is not
permission to depend on undocumented behavior.

## Independent remediation verification

The reviewed correction adds `output_dequeued` to represent the actual public
stream loan independently of generation-qualified `output_available`.
Reclamation marks each acquired loan. Successful publication clears the loan
flag; the selected output remains in flight. A bounded separate loop returns
unused, non-in-flight loans with `pw_stream_return_buffer`, clearing the loan
and availability flags only on success. A failed return retains ownership and
marks the existing protocol failure. Add/remove callbacks initialize or clear
the new flag under existing pool quiescence.

Every process exit after reclamation reaches that return loop, including absent
input, active output, unavailable generation and error paths. The independently
identified `ownership_playback_stage` call site also returns unused loans before
advancing the ownership transition. No input queue protocol, atomic ordering,
scientific behavior, native core code, graph driver or frame scheduler changed.
The extra work is bounded by the existing output pool; no latency or performance
measurement is claimed.

The test build used GCC 14.2.0, binutils 2.44, Meson 1.7.0, x86_64 and the installed
PipeWireAO 1.7.0 public API, as recorded in `setup.log`. The independently read
`debug-gates.log` and `debug-gates-detail.log` show **7/7 targets passed**: lifetime,
queue state, buffer transfer, module, live queue, remote Acquisition, and remote
registry. The live target covers COPY/LEASE drop-oldest, drop-newest and
backpressure, held output, observer reconnection, format/pool generation changes
and explicit module/context teardown.

The final functional rerun in `debug-ten.log` and `debug-ten-detail.log` passed
**10/10** targets, including the additional Header-only, Pause/Start and
shared-loop controls. The reviewed production source did not change between
the initial seven-target result and this final run.

The corrected remote Acquisition test changes the discriminating result from
**64 producer / 64 required / 1 observer** to **64 / 64 / 64**, with exact payload
and metadata, balanced observed Busy release, no client or protocol errors,
zero final active outputs/pending depth, and daemon exit status zero. Input is
paced at 10 publications per second while observer cycles continue roughly ten
times faster, retaining the idle intervals that exposed the defect.

The final completion ring can retain the last COPY return until another capture
callback or ordinary cleanup drains it. The test does not claim every internal
counter must become zero before teardown; its public children and daemon exit
successfully.

An additional public Pause/Start control passed in `pause-after.log`: with
producer triggers suspended after sequence 32, the fixture sends Pause to the
queue capture node, observes its published input stream state as paused and
the required sink count remaining 32 during two seconds of observer-only
cycles, then sends Start and resumes the original producer driver. All 64
frames reach both consumers exactly, with unchanged generation, no protocol
errors and daemon exit zero. This exercises the ownership-transition return
path as well as ordinary callback cleanup.

Earlier pause test attempts are retained as
`pause-state-assumption-rejected.log`, `pause-setup-rejected.log` and
`pause-mixed-activation-failed.log`. They either assumed that source
`set_active(false)` must emit a Paused stream state or combined source activation
changes with direct queue-node commands and failed to restart that fixture
graph. The final discriminating control uses only queue-node Pause/Start while
producer triggers are withheld. No production change was made for those failed
test-control assumptions; the mixed-control restart mechanism is not claimed
as a diagnosed production defect.

The initial sanitizer suite reached six passing targets; its live target failed
before context creation because the stripped test environment lacked a runtime
directory for the private socket. Logs are preserved as
`sanitize-gates-initial.log` and `sanitize-gates-initial-detail.log`. This is an
observed test environment failure, not a sanitizer finding in queue execution.
The final rerun with an owned private runtime directory passed **10/10** targets
in `sanitize-ten.log` and the preserved `sanitize-ten-detail.log`. The sanitizer build
uses `b_sanitize=address,undefined`; the daemon preloads the matching GCC ASan
runtime so the instrumented module runs under that runtime. Leak detection and
halt-on-error are enabled. The independent review found no sanitizer diagnostic.
The three additional targets are Header-only, public Pause/Start and shared-loop
remote controls. All four remote controls deliver 64/64/64, report no protocol
errors and terminate their daemon with status zero. The fixture asserts exact
frame ordering, metadata/payload validity and client error counts; Busy changes
and queue protocol counters are separately inspected diagnostic evidence. Each
final functional and sanitizer log contains 256 remote receipts, all showing
Busy 1→0; no nonzero queue protocol counter occurs, and all eight recorded
remote daemon exits are zero.

The final release build uses `--buildtype=release` with tests enabled and passes
**10/10** targets in `release-ten.log` and its detailed log. Its four remote
controls also have 256 exact receipts with Busy 1→0, no nonzero protocol counters
and four daemon exits of zero. The final Meson-only fallback accepts an installed
SDK without the optional `confdatadir` pkg-config variable by using
`prefix/share/pipewire-ao`; the release configuration and tests exercise this
path. Earlier functional/sanitizer runs used the same actual configuration
path. This does not change the production module or fixture C code.

The unchanged final release client was rerun against the original installed
queue module (`08b4cd64c9da848937a3124c606ba7e2504e87869b1be625ad3678a615563149`)
in `fail-before-final.log`. It again fails at 64/64/1, active outputs zero,
pending depth one and 62 replacements, with no protocol/client errors; its daemon
exits zero. This rules out the intervening fixture refinements as the reason
for the corrected pass.

The final test wrapper additionally fails its exit status when daemon teardown
is abnormal. A bounded negative shim in `teardown-negative.log` reports daemon
exit 42 and is rejected; `strict-teardown-after.log` then passes the corrected
release module with 64/64/64 and daemon exit zero. The production module and C
fixture are unchanged. All prior recorded daemon exits were already zero, so
this additional assertion does not revise their measured outcomes.

Reviewed identities (SHA-256):

| Artifact | SHA-256 |
| --- | --- |
| `src/modules/queue/module-queue.c` | `0ab50a7ed8c503cf508316e81538249527eab3794b04b6577acabecfe222dd44` |
| `src/modules/queue/test-remote-acquisition.c` | `c1314544f5b2ac16bf46b1e802c00715929b15a5c806679ab27ea24f9bd10676` |
| `src/modules/queue/test-remote-acquisition.conf` | `602e796cc342e68d2f075c94033e743f96598a5f27cac5f3b44888dca1ab271e` |
| `src/modules/queue/test-remote-acquisition.sh` | `c50889fb7f409228569bbcdcf6b6e42aebd6a8f2a429ba77e563c26f7bbbad24` |
| `src/modules/queue/meson.build` | `a6263074fdaaf7ff0e116ac738ad43924d3347757eac9366aacc26608966f934` |
| Functional build `libpipewire-module-queue.so` | `c5c2353a1f990c19ce99c35a91ca8538c964349dce1d157b65154673ca7f6afc` |
| Sanitizer build `libpipewire-module-queue.so` | `c055037fa1ddb1cd0ce1a7226d40ae4129fc8809651c71a1f71a3088f9bd9a69` |
| Release build `libpipewire-module-queue.so` | `8f953946733fafa3aaa766518ba3c2128fd2e6977babe3b8d478050f20664835` |

## Verification coverage and remaining limits

| Gate | Disposition |
| --- | --- |
| Idle-gap forward progress | Passed: paced source publications with continued observer cycles reproduce the original failure and pass after correction |
| External COPY/Acquisition | Passed: 64 exact ordered payload/Header/Acquisition deliveries, retained before/after logs and artifact identities |
| Metadata and loop controls | Passed: Header-only and shared-loop targets in final functional, release and sanitizer suites |
| Retained observer output and overflow | Passed: existing live COPY/LEASE drop-oldest, drop-newest and backpressure cases retain then return output and inspect admitted/delivered behavior |
| Ownership and lifetime | Passed: existing live generation replacement, reconnect and destruction cases; new public Pause/Start control. Prior lifetime findings remain governed by [QUEUE_LIFETIME_REVIEW.md](QUEUE_LIFETIME_REVIEW.md) |
| Installed artifact and RTC qualification | Separate gate: reseal the intended installed module after review; installed observer isolation/scientific equivalence are not established by these CPU tests |

This investigation establishes a queue forward-progress defect in a bounded
synthetic CPU fixture. It does not establish a data corruption defect, a
scientific discrepancy, hard real-time behavior or physical system performance.
