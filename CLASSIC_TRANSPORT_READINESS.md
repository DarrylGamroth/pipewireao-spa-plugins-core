# Classic transport readiness and graph Position

## Clean source boundary

This backport starts at committed `ac82518cc876cb6fce8baf55fc6875f8fba0f71f`
on branch `codex/classic-spa-readiness-clean` in
`pipewireao-spa-plugins-core-classic-clean`. The original owner worktree and
its pre-existing local changes remain untouched. No investigative snapshot
or latest-hold implementation is included.

The behavioral changes are ported from reviewed investigative commits
`4d0be46` and `900452c`. This clean backport has not yet been built or tested:
the initial preparation took place during the exclusive capacity campaign.
Successful tests and five receiving-path qualifications of the prior
snapshot-based candidate do not constitute qualification of this backport.

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
IO-last and buffers-last readiness, pool/IO withdrawal, and reentrant Start
from the ready notification in both copy and shared-buffer arrangements.

The established failed-format and Position Rust regressions use a small
local fixed-port test node instead of the unrelated retained-lease fixture.
They check restored-format/lost-pool notification, valid and undersized
Position setup, clear, Clock rejection, enumeration termination, optional
consumer delegation/errors, and parameter counts with and without Props.

After the measurement hold ends, run the wrapper and ndarray test suites,
build the release ndarray plugin, and run the C regression against that
exact binary. Connected FITS source qualification remains necessary before
calling the clean backport qualified. The previously frozen private plugin
directory must remain unchanged while its measurements are in progress.

## Delivery boundary

This clean baseline retains its existing two ndarray factories. The original
dirty owner and previously qualified binary contain a pre-existing third
factory. Integration with that local work and installation must preserve the
intended deployed factory set. Do not replace it with this unqualified clean
binary or merge the investigative snapshot merely to obtain its factories.
No installation, main branch update, push, HEART change, or scientific node
implementation change is part of this backport preparation.
