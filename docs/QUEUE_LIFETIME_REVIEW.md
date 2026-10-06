# Queue stream lifetime review

Status: QL-001 and QL-002 corrected and independently reviewed; software
regressions pass. Installed RTC qualification remains a separate gate.

## Scope and evidence

- Queue source baseline: `655b352e35f2e9a5c913f14f816838f7949e1f83`.
- Review worktree: `pipewireao-spa-plugins-core-queue-lifetime`, branch
  `fix/queue-stream-lifetime-20261005`. The primary agent owns the concurrent
  `test-module.c` regression edit; this review changes only this document.
- Native PipeWire source: sibling `pipewire`, selected revision `42fdf`.
- Installed queue module SHA-256:
  `b702cea6b122c834f413b7dfbea65424b762a4d337d4f6c20253a0059f845d8f`.
- Observed crash evidence:
  `/home/dgamroth/.cache/rtc-observation-qualified-20261005/cohorts/classic-pilot5/core-gdb.log`.
  The main thread receives SIGABRT in `free`, called by
  `pw_properties_free`, `pw_stream_destroy`, queue `impl_destroy`,
  `pw_impl_module_destroy`, and `pw_context_destroy`.

This is read-only source/API analysis plus inspection of existing crash evidence.
No experiments or CPU workloads were launched by this reviewer.

## QL-001 — Queue retains stream pointers after core-owned destruction

Severity: high. Confidence: high. Disposition: confirmed source defect;
the observed stack is consistent with this mechanism. A focused fail-before
regression remains required to establish the exact reproduction independently.

Affected code: `src/modules/queue/module-queue.c`, capture/playback event tables
and `impl_destroy`.

Observed source ordering in the selected PipeWire owner:

1. `pw_context_destroy` disconnects every core before destroying modules
   (`src/pipewire/context.c`, approximately line 630).
2. `pw_core_disconnect` removes and destroys its proxy. Core proxy removal
   disconnects streams; `proxy_core_destroy` subsequently destroys all streams
   on its owned stream list (`src/pipewire/core.c:227`).
3. `pw_stream_destroy` emits the stream destroy event, disconnects when needed,
   removes the stream from its core, and frees stream properties/storage
   (`src/pipewire/stream.c:1788`).
4. Queue supplies no capture/playback destroy callbacks. Its fields therefore
   retain freed stream addresses. Its later module destruction calls stream
   APIs and `pw_stream_destroy` through those addresses.

Transferred capture/playback properties are already nulled when passed to
`pw_stream_new`. The confirmed defect is the retained stream lifetime, not
duplicate ownership of those constructor property arguments.

Trusted reference implementations `module-loopback.c` and
`module-filter-chain.c` install per-stream destroy callbacks that remove the
corresponding listener and clear the retained pointer. Filter-chain also
disconnects both streams before explicitly destroying either stream.

## Remediation constraints

Add capture/playback destruction observation and clear each retained stream
pointer exactly once. Preserve the existing main-loop ownership and public
stream APIs. A destroy notification must not recursively destroy that same
stream, free the module implementation, or release its base reference.

The queue needs more care than copying the loopback callback verbatim:

- The stream destroy event occurs **before** the stream's implicit disconnect.
  Removing its listener immediately suppresses subsequent `remove_buffer`
  callbacks. Queue lease returns, output FD disposal and pool bookkeeping
  currently use those callbacks.
- Stop/gate queue work before invalidating either pointer. Existing ownership
  and backpressure invokes hold implementation references and inspect the
  terminal `destroying` flag. Preserve their cancellation/unref path.
- Use existing stream deactivation/disconnection barriers to quiesce RT work.
  `pw_stream_set_active(false)` includes a data-loop barrier; listener removal
  also removes RT callbacks through the owner's data-loop operation. Do not
  introduce a new thread policy or arbitrary nested application loop locks.
- Preserve playback-before-capture retirement when live output leases can
  still refer to capture buffers. Do not queue a capture return through a
  nulled/freed stream, or free leased descriptors before the relevant output
  pool has been revoked.
- If destruction notification sets `destroying` before `impl_destroy`, separate
  the work fence from the one-time final-cleanup guard. The current CAS guard
  would otherwise skip final cleanup and the base-reference release.
- Queued invokes can outlive stream disconnection. They must stop accessing
  stream/context state once fenced and release their references exactly once.
  Merely retaining `impl` does not retain its streams or context.

The independent verification below records the implemented candidate against
these invariants.

## Required validation

1. Minimal no-frame regression: load the queue into a private context and
   destroy the context with the module still loaded. Cover copy and lease.
   The existing `test-module.c` explicitly destroys both loaded modules before
   the context, so it misses this destruction order. Demonstrate fail-before
   and pass-after using the same focused case.
2. Preserve explicit module unload while the context remains alive. Confirm
   both nodes disappear and later context destruction remains safe.
3. Run existing queue ownership/lifecycle tests, including capture withdrawal,
   output removal, held lease/copy output, pending ownership transition and
   backpressure recovery. Confirm no extra buffer returns, protocol errors,
   surviving module/node resources or leaked owned descriptors.
4. Exercise context/core teardown after actual transfer on separate selected
   data loops. Inspect allocator/sanitizer evidence where available. Functional
   no-frame teardown alone does not qualify RT or lease ownership.
5. Rebuild/seal the exact native artifact and repeat the isolated RTC Classic
   teardown reproduction, then Copper acceptance. Preserve unrelated services
   and use the task's CPU allocation; CPU 0 is excluded.

The native correction does not by itself close RTC issue #3's scientific
equivalence, optional-observer isolation or lifecycle qualification gates.

## Independent remediation verification — 2026-10-05

Verifier: RTC observation qualification agent. This pass inspected the actual
production diff, the selected native PipeWire stream/context implementation,
and the existing regression logs. It changed only this review document and
launched no additional native workloads.

Reviewed production `module-queue.c` SHA-256:
`f4299a2fc3d1b0c4b335a1b2f566743614f5ba1e9190e3239d82393004c1e0bf`.
Reviewed private `test-lifetime.c` SHA-256:
`b869b813651cf2d5dc698f92d2370e92b5ef51d518573c425a4350d742e7402a`.

QL-001 remediation satisfies the inspected source invariants: a separate
`cleanup_started` guard preserves final cleanup after the early work fence;
deactivation uses the public data-loop barrier; playback disconnection precedes
capture disconnection while buffer listeners remain installed; destroy callbacks
remove their listener and clear the corresponding pointer; fenced process and
queued invoke callbacks avoid stream/context access and release their held
implementation reference. No additional concrete hazard was found in this pass.

### QL-002 — Dispatched stats callback accesses retired streams

Severity: high. Confidence: high. Disposition: resolved in reviewed candidate.
The independent inspection identified a window between stream-owned destruction
and deferred module cleanup: the stats timer still exists, but either stream
pointer can already be null. The public `pw_stream_get_state` implementation
dereferences its stream argument. `update_stats` therefore also requires the
terminal `destroying` fence before inspecting either stream.

The reviewed candidate includes that guard. The private callback test invokes
the actual static production callback with `destroying=true` and null streams
and context, then checks all four queued invoke cancellation paths release
exactly one reference. The implementation owner independently reproduced the
unguarded callback fault; inspection of
`~/.cache/rtc-queue-lifetime-20261005/stats-callback-before.log` confirms an
AddressSanitizer failure in `pw_stream_get_state` through `update_stats`, exit
`-6`. Inspection of `sanitize-final.log` confirms all five final candidate tests
pass: lifetime, buffer, state, module and live; no failures or timeouts.

The earlier conditional source review is resolved by this guard and regression.
Source review and inspected software regression evidence support publishing the
candidate for the isolated RTC acceptance reruns. Exact installed artifact
identity, Classic/Copper allocator-clean teardown, metadata delivery and full
scientific equivalence remain separate acceptance obligations; this review does
not declare those live RTC gates passed.

## Implementation validation

Evidence directory: `~/.cache/rtc-queue-lifetime-20261005`.

| Check | Result | Evidence |
| --- | --- | --- |
| Context-first no-frame regression on original production source | SIGSEGV, expected failing baseline | `fail-before.log` |
| Same regression after stream ownership correction | Pass | `pass-after.log` |
| Dispatched stats callback with only its guard removed in a task-owned source copy | ASan abort at `pw_stream_get_state` | `stats-callback-before.log` |
| Actual static stats and four queued invoke cancellation callbacks | Pass, one unref per invoke | `sanitize-final.log` |
| State, buffer, module, lifetime and live suites under ASan/UBSan | 5/5 pass, leak detection enabled | `sanitize-final.log` |
| Live transfer and teardown on two explicitly selected, distinct data loops | Pass under ASan/UBSan | `sanitize-separate-loops-qualified.log` |
| Final release candidate, all five suites with distinct data loops | 5/5 pass | `release-final-separated.log` |

The live fixture retains explicit module unload and adds eight context-first
cases: copy/lease × empty, queued, in-flight and backpressure. It asserts that
the producer and observer use distinct actual data loops. The installed SDK
library is not sanitizer-instrumented; the candidate module, state library and
test executables are instrumented in the sanitizer build.

Release module SHA-256:
`08b4cd64c9da848937a3124c606ba7e2504e87869b1be625ad3678a615563149`.

These are software ownership/lifecycle checks, not throughput measurements or
scientific observation-isolation acceptance.
