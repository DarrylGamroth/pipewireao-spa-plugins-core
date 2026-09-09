# PipeWireAO SPA plugins: core

This repository is the SDK-independent foundation of a PipeWireAO plugin
deployment. Build it once, then add only the separate hardware and transport
plugin repositories required by the target system.

It provides:

- discard, ndarray, NuVu, pyRTC, and HERMES-decoder SPA components;
- the bounded queue PipeWireAO module;
- shared C headers and pkg-config metadata used by the other plugin
  repositories.

The development headers are an output of this source repository. They do not
need their own GitHub repository.

## Build and stage

The build requires PipeWireAO 1.7 or later, Meson, Ninja, Cargo, Rust, and a
sibling `pipewire-rs` checkout used by the Rust workspace.

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

See [`docs/queue.md`](docs/queue.md) for the queue module and
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

The container build expects the sibling `pipewire-rs` build context and an
authorized source for the PipeWireAO build/runtime dependencies. BuildKit
secrets `apt_sources`, `apt_auth`, and `apt_keyring` can provide private package
sources without adding credentials to the image or repository.
