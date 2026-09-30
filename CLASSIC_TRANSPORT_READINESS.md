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
Connected source and receiving-path qualification of this binary remains
pending. Prior snapshot-based qualifications do not validate the clean binary.

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

Connected FITS source and receiving-path qualification remain necessary before
calling the clean backport qualified. No timing/capacity or hardware claim,
live UDP run, main branch update, installation or push follows from these
software checks. The prior frozen private plugin directory remains unchanged.

## Delivery boundary

This clean baseline retains its existing two ndarray factories. The original
dirty owner and previously qualified binary contain a pre-existing third
factory. Integration with that local work and installation must preserve the
intended deployed factory set. Do not replace it with this software-tested but
not yet source-qualified clean binary or merge the investigative snapshot
merely to obtain its factories.
No installation, main branch update, push, HEART change, or scientific node
implementation change is part of this backport preparation.
