# PipeWireAO SPA plugins: core

[![CI](https://github.com/DarrylGamroth/pipewireao-spa-plugins-core/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/DarrylGamroth/pipewireao-spa-plugins-core/actions/workflows/ci.yml)
[![Coverage](https://codecov.io/github/DarrylGamroth/pipewireao-spa-plugins-core/branch/main/graph/badge.svg)](https://app.codecov.io/github/DarrylGamroth/pipewireao-spa-plugins-core)

This repository is the SDK-independent foundation of a PipeWireAO plugin
deployment. Build it once, then add only the separate hardware and transport
plugin repositories required by the target system.

It provides:

- ndarray, NuVu, pyRTC, and HERMES-decoder SPA components;
- shared C headers and pkg-config metadata used by the other plugin
  repositories.

The development headers are an output of this source repository. They do not
need their own GitHub repository.

## Build and stage

The build requires the PipeWireAO 4.0.10 or later 4.x SDK, Meson, Ninja, Cargo, and Rust.
The Rust workspace pins its PipeWireAO bindings to an immutable Git revision;
Cargo fetches that revision or uses its local cache.

```console
meson setup build --prefix=/usr
meson compile -C build
meson test -C build --print-errorlogs
DESTDIR="$PWD/stage" meson install -C build
```

The staged tree is suitable for a target root filesystem, container, appliance
image, or package build. Install it before configuring a separate plugin
repository so `pipewireao-spa-plugins-0.1.pc` and the shared headers are
available.

PipeWireAO supplies the [bounded queue module](docs/queue.md) and the
`api.pipewireao.discard` sink. See
[`spa/plugins/pyrtc/README.md`](spa/plugins/pyrtc/README.md) for the pyRTC
bridge.

## Container and package recipes

`docker-bake.hcl` provides `debian-13-deploy` and
`ubuntu-26-04-deploy` targets for deployment images:

```console
docker buildx bake debian-13-deploy
```

The corresponding `debian-13-package` and `ubuntu-26-04-package` targets export
`.deb` artifacts instead. The supplied deployment-image recipes use those
packages internally, but a Meson staged install is equally valid for another
deployment system.

The container build requires access to the pinned Rust bindings and an
authorized source for the PipeWireAO build/runtime dependencies. BuildKit
secrets `apt_sources`, `apt_auth`, and `apt_keyring` can provide private package
sources without adding credentials to the image or repository.

## CI and coverage

GitHub Actions builds and tests against PipeWireAO 4.0.10 on Debian 13.
It runs the Meson suite and the
Rust workspace tests. The Rust report also includes Rust code executed by
Meson while loading the SPA plugins.

CI requires more than 90% line coverage in each LCOV report. The workflow
saves both reports as artifacts and uses GitHub OIDC for Codecov uploads.
Fork pull requests get the same coverage checks.

With the workflow dependencies, the Rust `llvm-tools` component, and
`cargo-llvm-cov` installed, start with a fresh coverage build directory.

```sh
coverage_env=$(cargo llvm-cov show-env --sh --remap-path-prefix)
eval "$coverage_env"
export LLVM_PROFILE_FILE="$PWD/build-coverage/rust-profiles/%p-%16m.profraw"
mkdir -p build-coverage/rust-profiles
CC=gcc CXX=g++ meson setup build-coverage -Dtests=enabled -Db_coverage=true
runtime_dir=$(mktemp -d)
export PIPEWIREAO_RUNTIME_DIR="$runtime_dir"
meson test -C build-coverage --print-errorlogs
gcovr --root . --lcov build-coverage/coverage-c.info --print-summary build-coverage
python3 scripts/rust-coverage.py build-coverage
```
