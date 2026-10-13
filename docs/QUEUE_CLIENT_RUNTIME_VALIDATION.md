# Client queue runtime interoperability investigation

## Result and scope

The raw-video queue progresses with a coherent installed SDK runtime in the
queue-owning client. A controlled synthetic queue fails when that client uses
the packaged Julia runtime while the daemon, observer, and queue module use
the installed SDK. Selecting the SDK for a real Julia client also passes the
physical camera observation test. No additional production queue algorithm,
ownership, scheduling, or memory-order change is justified by this result.

This investigation starts from core commit
`74586dffec547d2335a1cf65c62bf1bac57c77da` in worktree
`pipewireao-spa-plugins-core-queue-stall`, branch
`codex/queue-client-stall-20261013`. Experimental test options and source
prototypes remain separate from the production correction described in
[QUEUE_VIDEO_ACQUISITION_VALIDATION.md](QUEUE_VIDEO_ACQUISITION_VALIDATION.md).
No installed SDK file, global Julia artifact override, package version,
live RTC service, or camera feature setting was changed.

Evidence root: `/home/dgamroth/.cache/gui-shape-genicam-20261013/queue-stall/`.
Logs, maps, scripts, launch arguments, dumps, and investigative build artifacts
are retained there. These functional tests are not a latency benchmark or
real-time qualification.

## Finding RUNTIME-001

- Severity: major integration defect in the tested mixed runtime deployment.
- Confidence: high for the runtime-prefix boundary; unresolved for the precise
  native transport mechanism.
- Observed: the original Julia camera fixture maps the packaged artifact
  `8c1a3e9d3261ba839cf16f22202bbafdc9c38921` core library, client-node module,
  and SPA support plugin. The daemon and production GUI map `/opt/pipewireao`.
  The Julia manifest selects `PipeWireAO_jll` version `1.7.0+18`.
- Observed: the SDK and artifact have the same core SONAME
  `libpipewire-ao-0.3.so.0`; their public stream and metadata headers are
  byte-identical, but the core, client-node, and support binaries have different
  hashes. Exact paths/hashes are in `worker-runtime-comparison.json` and
  `runtime-library-comparison.json`.
- Observed: changing the synthetic queue owner's entire prefix to the artifact
  reproduces failed output twice. Keeping that owner on the SDK passes.
- Disposition: use a task-local coherent SDK prefix for this installed-module
  integration fixture. Report the packaged/installed interoperability boundary
  for upstream investigation; do not claim an enum-layout or ABI root cause.
  No external issue has been filed as part of this task.

The artifact wrapper loads absolute library products with `RTLD_DEEPBIND`.
Setting `LD_LIBRARY_PATH` alone does not replace those Julia library products.
Matching only the module search directory is insufficient evidence of runtime
coherence. Actual process maps must include core, libspa, support, client-node,
and the task-owned queue module.

## Discriminating controls

The autonomous synthetic source uses RT/POLL_DRIVER, deterministic GRAY8
640×480 payloads (307200 bytes), declared format rate 0/1, three buffers,
Header and Acquisition V2 metadata, and a 50 Hz publication deadline. It
rejects all node I/O to match the eGrabber source's Clock/Position behavior.
The observer is a separate SDK client with the production GUI's lazy/request
properties and stream flags. Tests pace observer triggers at 25 or 50 Hz.
The queue has capacity one, drop-oldest, and lease storage. Finite test length
is 64 published samples; replacements and sequence gaps are allowed.

| Queue owner / observation | Result | Evidence |
| --- | --- | --- |
| SDK C client, observer 50 Hz | 63 distinct frames through sequence 64, zero client errors | `poll-gray8-zero-gui-paced50.log` |
| SDK C client, observer 25 Hz | 31 distinct frames through sequence 64, zero client errors | `poll-gray8-zero-gui-paced25.log` |
| Packaged-prefix C client, SDK daemon/observer/module | FAIL twice: 64 queue capture publications, 63 replacements, zero deliveries/completions, zero queue protocol errors | `poll-gray8-zero-jll-owner50.log`, `poll-gray8-zero-jll-owner50-repeat.log` |
| SDK-backed Julia client, SDK daemon/observer/module | PASS twice: 61 and 62 distinct frames through sequence 64, zero client errors | `poll-gray8-zero-julia-sdk50-after.log`, `poll-gray8-zero-julia-sdk50-maps.log` |

All four runtime comparisons use the same autonomous source profile and SDK
daemon. C controls isolate the queue owner's runtime prefix. The Julia
follow-up establishes that a real Julia-owned module and continuously running
MainLoop progress with SDK products. The observer validates every received
payload, chunk, Header, and negotiated Acquisition record. Delivered sample
counts are not claimed to equal publications in this capacity-one topology.

Zero declared rate, exported client ownership, absent source Clock/Position,
lazy/request observer properties, and realistic trigger cadence individually
pass SDK controls. They are insufficient explanations for the original stall.
Earlier explicit named-loop synthetic variants deliver frames but assert
during teardown; those are separate fixture/lifecycle observations and are
excluded from the passing controls above.

`invalid memory type 4` appears at startup in the failing packaged-prefix
controls, but also at teardown in passing SDK controls. Its timing is retained
in the logs. It is not promoted to the microscopic stall mechanism.

## Task-local SDK recipe

The SDK has a multiarch layout; a cache-only artifact facade supplies the JLL
layout. Its `lib`, `include`, `share`, and `bin` entries are symlinks to the
corresponding installed SDK directories (`lib` targets
`/opt/pipewireao/lib/x86_64-linux-gnu`). The first depot contains only the
task-owned override:

```toml
# EVIDENCE/sdk-depot/artifacts/Overrides.toml
"8c1a3e9d3261ba839cf16f22202bbafdc9c38921" = "EVIDENCE/sdk-artifact"
```

Replace `EVIDENCE` with the absolute evidence-root path in both places. The
test process requires this environment; the complete `env` invocation below
passes it directly to Julia:

```text
JULIA_DEPOT_PATH=EVIDENCE/sdk-depot:/home/dgamroth/.julia
JULIA_NUM_THREADS=1
PIPEWIREAO_RUNTIME_DIR=/run/user/1000/gui-egrabber-20261013
XDG_RUNTIME_DIR=/run/user/1000/gui-egrabber-20261013
PIPEWIREAO_MODULE_DIR=EVIDENCE/../core-queue/build/src/modules/queue:/opt/pipewireao/lib/x86_64-linux-gnu/pipewire-ao-0.3
PIPEWIREAO_SPA_PLUGIN_DIR=/opt/pipewireao/lib/x86_64-linux-gnu/spa-ao-0.2
PIPEWIREAO_CONFIG_DIR=/opt/pipewireao/share/pipewire-ao
LD_LIBRARY_PATH=/opt/pipewireao/lib/x86_64-linux-gnu:/opt/pipewireao/lib
```

These are per-process environment assignments, not global configuration.
`verify-sdk-runtime.jl` verifies the library products. Complete maps after queue
readiness are in `poll-gray8-zero-julia-sdk50-maps.log`. The module's second
Core connection also needs its explicit `remote.name` argument; a differently
named synthetic daemon must pass its actual name. The physical private daemon
uses `pipewire-ao-0` within its isolated runtime directory.

For the original `scripts/run_egrabber_test_graph.jl`, `using PipeWireAO` runs
before `configure_environment!`. Set the artifact override and all initial
SDK environment paths before Julia starts. The helper subsequently constructs
config/plugin/module paths from its build arguments. A second cache-only
facade named `sdk-build` makes those constructed paths resolve to SDK files:

| Facade path | Symlink target |
| --- | --- |
| `sdk-build/src/daemon` | `/opt/pipewireao/share/pipewire-ao` |
| `sdk-build/src/modules` | `/opt/pipewireao/lib/x86_64-linux-gnu/pipewire-ao-0.3` |
| `sdk-build/spa/plugins` | `/opt/pipewireao/lib/x86_64-linux-gnu/spa-ao-0.2` |

With those directories and the artifact facade in place, pass the environment
directly to the command, including the vendor library directory:

```sh
env \
  JULIA_DEPOT_PATH=EVIDENCE/sdk-depot:/home/dgamroth/.julia \
  JULIA_NUM_THREADS=1 \
  PIPEWIREAO_RUNTIME_DIR=/run/user/1000/gui-egrabber-20261013 \
  XDG_RUNTIME_DIR=/run/user/1000/gui-egrabber-20261013 \
  PIPEWIREAO_MODULE_DIR=EVIDENCE/../core-queue/build/src/modules/queue:/opt/pipewireao/lib/x86_64-linux-gnu/pipewire-ao-0.3 \
  PIPEWIREAO_SPA_PLUGIN_DIR=/opt/pipewireao/lib/x86_64-linux-gnu/spa-ao-0.2 \
  PIPEWIREAO_CONFIG_DIR=/opt/pipewireao/share/pipewire-ao \
  LD_LIBRARY_PATH=/opt/pipewireao/lib/x86_64-linux-gnu:/opt/pipewireao/lib:/opt/euresys/egrabber/lib/x86_64 \
  taskset -c 14,15 /home/dgamroth/.juliaup/bin/julia --startup-file=no \
  --project=/home/dgamroth/workspaces/codex/pipewire/pipewireao-gui/scripts \
  /home/dgamroth/workspaces/codex/pipewire/pipewireao-gui/scripts/run_egrabber_test_graph.jl \
  --pipewire-build EVIDENCE/sdk-build --plugin-build EVIDENCE/sdk-build \
  --queue-build EVIDENCE/../core-queue/build \
  --runtime-directory /run/user/1000/gui-egrabber-20261013 \
  --producer gigelink --control remote \
  --interface-index 0 --device-index 0 --stream-index 0
```

Exact environment assignments and arguments are retained as an executable
argument array in `sdk-original-fixture-command.json`. Its `--help` import path
was checked separately without creating a connection or opening a device.
The original full fixture was not relaunched because it would collide with
the current source and queue node names. Its deployment recipe uses the same
validated SDK products; actual hardware pass-after evidence comes from the
separate existing-source probe described below. Run a full original fixture
only on an available private graph with its own source ownership and names.

## Bounded physical validation and cleanup

An additional SDK-backed Julia client queue was linked to the already running
camera source, serial `055070423014`, without stopping the original source,
daemon queue, or GUI and without writing camera properties. Queue module
SHA256 is `9894a3e7c71eadf87ab8988a58ec87bc023a5f04a93a4a86c9b7b26c24acacd6`.
The probe has no explicit capture/playback loop selectors. Its launch arguments
are in `camera-sdk-client-probe-launch.json`; its source is
`camera-sdk-client-queue.jl`.

The existing production NativeAdapter hardware test passes: 79 property
descriptors, 79 observed values, exact raw names/keys/shared controls, and
three distinct frame revisions 2, 3, 4, each 640×480 Gray8 / 307200 bytes.
One test passes in 0.46 seconds (`camera-sdk-client-adapter.log`). Actual probe
process maps confirm SDK core, libspa, support, and client-node plus the validated
task-owned module (`camera-sdk-client-maps.json`). The probe reports seven
deliveries and completions with zero queue protocol errors after the observer
disconnects.

All 79 observed camera values match across the probe interval. Relative to the
cold, non-acquiring baseline, 75 match; the differences are DeviceTemperature,
GevPrimaryApplicationSocket, GevSCPHostPort, and TLParamsLocked. Trigger,
exposure, gain, format, dimensions, and image orientation values match that
baseline. Dumps report the source's observed Props; this is not a claim that
every value was freshly read from hardware during each dump.

Only unit `pipewireao-gui-camera-sdk-client-probe-20261013.service` was stopped.
Its input/output nodes 50/48 disappear. Original source 39 and queue nodes
36/34 remain running. Original queue publications progress
734269 → 765212 → 767650, deliveries 14744 → 18215 → 18495, with protocol
errors zero throughout. Evidence: `camera-sdk-probe-before.json`,
`camera-sdk-probe-after.json`, `camera-sdk-probe-comparison.json`,
`camera-sdk-probe-stopped.json`, service-status files, and the phase-bounded
daemon journal. No RTC service operation is part of this experiment.

## Ownership and resource review

The strict frame path is C queue capture/playback followed by native Rust
observation. Julia performs module setup and services the control MainLoop;
it does not execute a per-frame Julia callback. SDK headers/libraries and the
normal compiled native paths are used. Synthetic pacing is functional test
load, not evidence of actual camera frame rate or latency distribution.

| Shared resource | Writer / reader / lifetime rule |
| --- | --- |
| Capture slot state and token | Capture producer and return consumer use existing C11 atomics and acquire/release transitions; no ordering change |
| Pending/completion rings | Existing bounded publication/return protocol transfers preallocated slot ownership; capacity and overflow policy unchanged |
| Output slots | Playback callback owns output state; copy/lease lifetime and Busy outcomes unchanged |
| Format/generation/pools | Cold lifecycle callbacks quiesce affected ownership before pool changes; existing generation checks retained |
| Native observer buffer | Buffer guard releases the native loan; observer copies into its bounded reusable owned-frame pool and publishes through a bounded queue |
| Observer admission | Existing Rust acquire/release admission gate separates accepted samples from display/control ticks; no atomic or synchronization change |

The negotiated camera profile has three native buffers per pool; all new
experiments keep queue capacity one. Capture work uses existing preallocated
slot/ring state; copy versus lease work and metadata copying are unchanged.
Native observer work is bounded by the finite native pool and fixed payload
size in these tests. Arc reference counting and cold/control allocations and
locking remain present. This review does not assert whole-system zero
allocation, lock freedom, WCET, jitter, tail latency, or throughput. No new
payload copy, hot-path log, allocation, polling agent, or synchronization
algorithm was introduced into production.

The remaining gap is the exact packaged/SDK transport failure mechanism and
its compatibility policy. An upstream investigation can start from the
two-process synthetic runtime-prefix reproducer and correlate callback
admission and native transport events. The scoped remedy here is deployment
coherence, backed by synthetic and bounded physical pass-after evidence.
