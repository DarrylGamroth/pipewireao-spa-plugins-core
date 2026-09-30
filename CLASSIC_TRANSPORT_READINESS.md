# Classic transport readiness and graph Position

## Clean source boundary

This backport starts at committed `ac82518cc876cb6fce8baf55fc6875f8fba0f71f`
on branch `codex/classic-spa-readiness-clean` in
`pipewireao-spa-plugins-core-classic-clean`. The original owner worktree and
its pre-existing local changes remain untouched. No investigative snapshot
or latest-hold implementation is included.

The behavioral changes are ported from reviewed investigative commits
`4d0be46` and `900452c`. Preparation took place during the exclusive capacity
campaign. The clean backport's software verification was then executed in an
authorized build pause at source commit `fefc8f3`: 14 wrapper tests, 5 ndarray
tests, the full C regression, and the export check passed. Its release DSO is
preserved in `evidence/libspa-ndarray-clean.so` with SHA-256
`153839f352590a992655f327ef892c1dff3595ab9fbe80f183ed2aeb7ec6458c`.
The exact clean binary subsequently passed its own connected source and
five receiving-path qualification described below.

## Readiness

The generic wrapper publishes `SPA_NODE_FLAG_NEED_CONFIGURE` while any
required or configured port lacks its negotiated format, registered buffers,
or Buffers IO. This uses the existing `Port::ready()` predicate, matching
`State::ready()` and the requirements for `Start`.

Successful buffer and IO changes publish flag transitions. A failed format
change restores the old format but leaves its buffer pool cleared; the
resulting readiness change is published before returning the original error.

Node and port event records own their parameter arrays. Events are emitted
after releasing the callback gate, allowing a callback to inspect or start
the node. The wrapper uses shared handle references with an interior-mutable
hook list to avoid retaining an exclusive handle reference across callbacks.
This brings only the notification ownership/access primitives required by
the fix, without the unrelated observer and lease APIs.

## Graph Position

The wrapper advertises Position IO for every node, validates nonnull setup
size, and acknowledges setup and clear without retaining the pointer for a
node that does not consume Position. `Node::NEEDS_POSITION` defaults false;
optional consumers receive setup/clear through the defaulted `Node::set_io`
method and preserve their own returned errors. Clock and unknown IO remain
unsupported with `-ENOENT`.

This addresses the demonstrated local graph interoperability failure: the
current local host establishes its active driver identity after successful
synchronous Position acknowledgement. Legal rejection of unknown IO had
prevented processing in the tested composition. No PipeWire core changes,
clock population, scheduling policy, or new timing semantics are included.

Node parameter capacity is three: optional PropInfo and Props plus Position
IO. Existing nodes require no source change. The small Position POD builder
and generic IO enumeration are the only additional parameter dependencies.

## Regression coverage and outstanding execution

The established C video-view regression is carried unchanged from the
reviewed readiness patch. It checks format-only Start returning `-EIO`,
IO-last readiness in the copy arrangement, buffers-last readiness in the
shared-buffer arrangement, and pool/IO withdrawal. Reentrant Start from the
ready notification is checked in the shared, buffers-last arrangement;
the copy, IO-last arrangement checks ordinary Start and flag transitions.

The established failed-format and Position Rust regressions use a small
local fixed-port test node instead of the unrelated retained-lease fixture.
They check restored-format/lost-pool notification, valid and undersized
Position setup, clear, Clock rejection, enumeration termination, optional
consumer delegation/errors, and parameter counts with and without Props.

The focused `info_parameters_survive_reentrant_format_withdrawal` regression
clears a negotiated input format through the public node method during an
outer node-info callback and, separately, during an outer port-info callback.
Nested notifications occur, live Format parameter flags change from
READWRITE to WRITE, and the outer port array retains its configured values.
The outer node flags retain their ready value while live flags change to
NEED_CONFIGURE. Both outer parameter pointers are checked against the live
State arrays and read again before callback return. Node parameter records
are immutable through the clean public control API, so their ownership is
checked by distinct storage and valid unchanged contents rather than an
invented node-parameter mutation API.

The fixture assumes synchronous listener/control calls on one main thread,
a stopped node, no concurrent processing, and no listener or handle destruction
during callbacks. Capture fields use `Cell`, avoiding an exclusive Rust borrow
across recursion. Callback-scoped pointers are never used after callback return.
This is a reentrancy regression, not a concurrent listener-mutation test.

The focused snapshot regression passed, failed with exit 101 under a temporary
negative control that emitted live State array pointers, and passed again
after exact source restoration. The video-view C function also failed on the
preserved pre-fix installed DSO's missing NEED_CONFIGURE flag and passed against
the clean DSO. Its focused main retains the same video-view assertions. The
full C comparison against that old installed DSO stops earlier on its different
shared-chunk behavior, so that earlier failure is not readiness evidence.

Commands, environment, logs, exit statuses and hashes are retained under
`evidence/`, with `evidence/clean-verification.json` as the index. All builds
and tests used CPU 14 and Cargo used one build job. An initial manual C compile
omitted the project's `-D_GNU_SOURCE`; the corrected command matches Meson's
feature macro and passed without source changes. The inherited libspa header
unused-parameter warning remains. The negative control was restored and its
binary was not used for C qualification or preserved as a deployment candidate.

## Connected clean-binary qualification

A new immutable private pair combines the exact clean ndarray DSO above and
qualified FITS SHA-256
`947ba07a07f5da0e8c60b408455933689e3cc7a7cc1ca8dbf8c358c9ac3f8a41` at
`/home/dgamroth/.cache/rtc-classic-spa-private-clean-plugins-20260930`.
Unrelated plugins remain symlinks to their installed directories. The previous
private pair and `/opt` were not changed.

The connected seven-frame source test captured exactly 224 datagrams, ordered
IDs 0–6, and matching fixed headers/pixels, with zero sink errors and normal
exit. Fresh sequential HEART, FGN-frame, JFG-frame, FGN-row, and JFG-row runs then
replayed the same 63-frame UInt16 corpus at 100 Hz, 2,000 µs readout and 11 rows
per packet. Each captured 2,016 WFS packets and 63 DM commands, ordered complete
WFS IDs 0–62, no payload mismatch or repeated/decreasing DM IDs, passing selected
source-arithmetic checks, zero sender errors, and normal process exits.

FGN/JFG receiver counts were 63 frames and zero rejection/drop/starvation, with
2,016 blocks in row mode. JFG retained final feedback passed: maximum error
1.19 × 10⁻⁷ µm in frame mode and 2.38 × 10⁻⁷ µm in row mode, with 74 nonzero
values matching the expectation. HEART matched its exact source-model command
bits and 76 expected clipped actuators. Its broad legacy `qualified` remains
false for the documented historical strict comparison/initial-state readback
limitations; `functional_wire_qualified` and the selected arithmetic policy
pass. FGN reports do not expose a separate retained-feedback comparison.

Commands, harness revision `ebe583f`, script and plugin hashes, process results,
counter validation, and Zstandard wire archive hashes are retained in
`/home/dgamroth/.cache/rtc-classic-spa-clean-five-paths63-20260930/manifest.json`.
The seven-frame result is at
`/home/dgamroth/.cache/rtc-classic-spa-clean-source7-20260930`.
Historical failed harness attempts remain separately preserved, and prior
three-factory qualification was not substituted for this clean-binary result.
This establishes functional transport and selected numerical consistency;
timing, capacity, hardware, main integration and deployment are not qualified
by these runs.

## Delivery boundary

This clean baseline retains its existing two ndarray factories. The original
dirty owner and previously qualified binary contain a pre-existing third
factory. Integration with that local work and installation must preserve the
intended deployed factory set. This source-qualified two-factory binary does
not authorize removal of an existing third factory. Do not merge the
investigative snapshot merely to obtain its factories.
No installation, main branch update, push, HEART change, or scientific node
implementation change is part of this backport preparation.
