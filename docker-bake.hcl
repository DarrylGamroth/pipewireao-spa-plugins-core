variable "COMPONENT" {
  default = "core"
}

variable "APT_BUILD_PACKAGES" { default = "" }
variable "APT_RUNTIME_PACKAGES" { default = "" }
variable "HOST_DEPENDENCY" { default = "" }
variable "MAINTAINER" { default = "" }
variable "EXTRA_DEPENDS_JSON" { default = "[]" }
variable "LIBRARY_DIRS_JSON" { default = "[]" }
variable "MESON_OPTIONS_JSON" { default = "[]" }
variable "DEB_VERSION" { default = "" }

target "package-common" {
  context    = "."
  dockerfile = "packaging/containers/Dockerfile.package"
  target     = "packages"
  contexts = {
    pipewire-rs = "../pipewire-rs"
  }
  args = {
    APT_BUILD_PACKAGES = APT_BUILD_PACKAGES
    COMPONENT          = COMPONENT
    DEB_VERSION        = DEB_VERSION
    EXTRA_DEPENDS_JSON = EXTRA_DEPENDS_JSON
    HOST_DEPENDENCY    = HOST_DEPENDENCY
    LIBRARY_DIRS_JSON  = LIBRARY_DIRS_JSON
    MAINTAINER         = MAINTAINER
    MESON_OPTIONS_JSON = MESON_OPTIONS_JSON
  }
}

target "debian-13-package" {
  inherits = ["package-common"]
  args = { BASE_IMAGE = "debian:trixie" }
  output = ["type=local,dest=dist/debian-13/${COMPONENT}"]
}

target "ubuntu-26-04-package" {
  inherits = ["package-common"]
  args = { BASE_IMAGE = "ubuntu:26.04" }
  output = ["type=local,dest=dist/ubuntu-26.04/${COMPONENT}"]
}

target "debian-13-deploy" {
  context    = "."
  dockerfile = "packaging/containers/Dockerfile.deploy"
  target     = "runtime"
  contexts = { packages = "target:debian-13-package" }
  args = {
    APT_RUNTIME_PACKAGES = APT_RUNTIME_PACKAGES
    BASE_IMAGE           = "debian:trixie"
  }
  tags = ["pipewireao-spa-${COMPONENT}:debian-13"]
}

target "ubuntu-26-04-deploy" {
  context    = "."
  dockerfile = "packaging/containers/Dockerfile.deploy"
  target     = "runtime"
  contexts = { packages = "target:ubuntu-26-04-package" }
  args = {
    APT_RUNTIME_PACKAGES = APT_RUNTIME_PACKAGES
    BASE_IMAGE           = "ubuntu:26.04"
  }
  tags = ["pipewireao-spa-${COMPONENT}:ubuntu-26.04"]
}

group "default" {
  targets = ["debian-13-package", "ubuntu-26-04-package"]
}
