# pipewireao-spa-plugins-core

Standalone source and Debian/Ubuntu packaging for the `core` PipeWireAO SPA component.

This repository was split from
[`pipewireao-spa-plugins`](https://github.com/DarrylGamroth/pipewireao-spa-plugins)
at commit `8d2edcf`. The common public headers continue to be released
as the `pipewireao-spa-plugins-dev` binary package from the core repository.

## Package build

Supply the PipeWireAO build dependency through `APT_BUILD_PACKAGES` and its
runtime package expression through `HOST_DEPENDENCY`:

```console
export APT_BUILD_PACKAGES='pipewire-ao-dev'
export HOST_DEPENDENCY='pipewire-ao (>= 1.7)'
export MAINTAINER='Deployment Team <packages@example.org>'
docker buildx bake debian-13-package
docker buildx bake ubuntu-26-04-package
```

Vendor repositories also require the SDK development package in
`APT_BUILD_PACKAGES` and the corresponding runtime package in
`EXTRA_DEPENDS_JSON`. Private APT sources and credentials are passed with the
BuildKit secrets `apt_sources`, `apt_auth`, and `apt_keyring`.
