#!/usr/bin/env python3
"""Build one dependency-isolated PipeWireAO binary Debian package."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).with_name("components.json")
COPYRIGHT = Path(__file__).with_name("copyright")
DEFAULT_EPOCH = 946684800


def run(command: list[str], *, cwd: Path | None = None) -> str:
    try:
        completed = subprocess.run(
            command,
            cwd=cwd,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
        )
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout, file=sys.stderr, end="")
        raise SystemExit(f"command failed with status {error.returncode}: {command[0]}") from error
    return completed.stdout.strip()


def load_manifest() -> dict[str, object]:
    def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
        result: dict[str, object] = {}
        for key, value in pairs:
            if key in result:
                raise SystemExit(f"duplicate key {key!r} in {MANIFEST}")
            result[key] = value
        return result

    with MANIFEST.open(encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=unique_object)


def component_configuration(manifest: dict[str, object], name: str) -> dict[str, object]:
    components = manifest["components"]
    if not isinstance(components, dict) or name not in components:
        choices = ", ".join(sorted(components)) if isinstance(components, dict) else ""
        raise SystemExit(f"unknown component {name!r}; choose one of: {choices}")
    component = components[name]
    if not isinstance(component, dict):
        raise SystemExit(f"component {name!r} is not an object")
    return component


def meson_options(manifest: dict[str, object], component: dict[str, object]) -> list[str]:
    features = manifest.get("feature_options")
    enabled = component.get("enable", [])
    if not isinstance(features, list) or not all(isinstance(value, str) for value in features):
        raise SystemExit("feature_options must be a list of strings")
    if not isinstance(enabled, list) or not all(isinstance(value, str) for value in enabled):
        raise SystemExit("component enable must be a list of strings")
    unknown = sorted(set(enabled) - set(features))
    if unknown:
        raise SystemExit(f"component enables unknown Meson features: {', '.join(unknown)}")
    values = {feature: "disabled" for feature in features}
    values.update({feature: "enabled" for feature in enabled})
    return [f"-D{feature}={values[feature]}" for feature in features]


def require_tools(names: list[str]) -> None:
    missing = [name for name in names if shutil.which(name) is None]
    if missing:
        raise SystemExit(f"required tools are missing: {', '.join(missing)}")


def project_version(build_dir: Path) -> str:
    project = json.loads(run(["meson", "introspect", "--projectinfo", str(build_dir)]))
    version = project.get("version")
    if not isinstance(version, str) or not version:
        raise SystemExit("Meson did not report a project version")
    return version


def architecture(component: dict[str, object]) -> str:
    configured = component.get("architecture")
    if configured is not None:
        if not isinstance(configured, str) or not configured:
            raise SystemExit("component architecture must be a non-empty string")
        return configured
    return run(["dpkg-architecture", "-qDEB_HOST_ARCH"])


def elf_files(root: Path) -> list[Path]:
    result = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as stream:
                if stream.read(4) == b"\x7fELF":
                    result.append(path)
    return result


def validate_payload(root: Path, component: dict[str, object]) -> list[Path]:
    configured = component.get("payload_roots", ["usr/lib"])
    if not isinstance(configured, list) or not configured or not all(
        isinstance(value, str) and value for value in configured
    ):
        raise SystemExit("component payload_roots must be a non-empty list of strings")
    allowed = [Path(value) for value in configured]
    payload = [path for path in root.rglob("*") if path.is_file()]
    unexpected = [
        path.relative_to(root)
        for path in payload
        if not any(path.relative_to(root).is_relative_to(prefix) for prefix in allowed)
    ]
    if unexpected:
        names = ", ".join(str(path) for path in unexpected)
        raise SystemExit(f"component installed files outside its allowed payload roots: {names}")
    return payload


def shlib_dependencies(
    files: list[Path], package: str, library_dirs: list[Path], workspace: Path
) -> str:
    if not files:
        return ""
    debian = workspace / "debian"
    debian.mkdir(parents=True)
    (debian / "control").write_text(
        "Source: pipewireao-spa-plugins\n"
        "Section: libs\n"
        "Priority: optional\n"
        "Maintainer: PipeWireAO contributors <noreply@example.invalid>\n"
        "\n"
        f"Package: {package}\n"
        "Architecture: any\n"
        "Description: dependency scan fixture\n",
        encoding="utf-8",
    )
    command = ["dpkg-shlibdeps", "-O", f"--package={package}"]
    command.extend(f"-l{path}" for path in library_dirs)
    command.extend(f"-e{path}" for path in files)
    output = run(command, cwd=workspace)
    for line in output.splitlines():
        if line.startswith("shlibs:Depends="):
            return line.partition("=")[2]
    return ""


def control_description(component: dict[str, object]) -> str:
    synopsis = component.get("description")
    details = component.get("long_description")
    if not isinstance(synopsis, str) or not synopsis:
        raise SystemExit("component description must be a non-empty string")
    if not isinstance(details, str) or not details:
        raise SystemExit("component long_description must be a non-empty string")
    lines = [f"Description: {synopsis}"]
    lines.extend(f" {line}" for line in textwrap.wrap(details, width=76))
    return "\n".join(lines)


def validate_control_value(name: str, value: str) -> None:
    if not value or "\n" in value or "\r" in value:
        raise SystemExit(f"{name} must be a non-empty, single-line Debian control value")


def validate_meson_overrides(manifest: dict[str, object], options: list[str]) -> None:
    controlled = set(manifest["feature_options"]) | {"tests"}
    seen: set[str] = set()
    for option in options:
        if option.startswith("-") or "=" not in option or "\n" in option or "\r" in option:
            raise SystemExit(f"Meson override must have NAME=VALUE form: {option!r}")
        name, value = option.split("=", 1)
        if not name or not value:
            raise SystemExit(f"Meson override must have NAME=VALUE form: {option!r}")
        if name in controlled:
            raise SystemExit(f"Meson option {name!r} is controlled by the package component")
        if name in seen:
            raise SystemExit(f"duplicate Meson override for {name!r}")
        seen.add(name)


def json_string_list(name: str, value: str) -> list[str]:
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError as error:
        raise SystemExit(f"{name} must be a JSON list of strings: {error}") from error
    if not isinstance(parsed, list) or not all(isinstance(item, str) for item in parsed):
        raise SystemExit(f"{name} must be a JSON list of strings")
    return parsed


def installed_size(root: Path) -> int:
    total = sum(path.stat().st_size for path in root.rglob("*") if path.is_file())
    return max(1, (total + 1023) // 1024)


def write_md5sums(root: Path) -> None:
    records = []
    for path in sorted(root.rglob("*")):
        if path.is_file() and "DEBIAN" not in path.relative_to(root).parts:
            digest = hashlib.md5(path.read_bytes(), usedforsecurity=False).hexdigest()
            records.append(f"{digest}  {path.relative_to(root)}")
    (root / "DEBIAN" / "md5sums").write_text("\n".join(records) + "\n", encoding="utf-8")


def normalize_mtimes(root: Path, epoch: int) -> None:
    for path in sorted(root.rglob("*"), reverse=True):
        if not path.is_symlink():
            os.utime(path, (epoch, epoch))
    os.utime(root, (epoch, epoch))


def normalize_permissions(root: Path) -> None:
    for path in root.rglob("*"):
        if path.is_symlink():
            continue
        if path.is_dir():
            path.chmod(0o755)
        elif path.stat().st_mode & 0o111:
            path.chmod(0o755)
        else:
            path.chmod(0o644)
    root.chmod(0o755)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component")
    parser.add_argument("--list-components", action="store_true")
    parser.add_argument("--source-root", type=Path, default=ROOT)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    parser.add_argument("--version", help="complete Debian version; defaults to PROJECT_VERSION-REVISION")
    parser.add_argument("--revision", default="1")
    parser.add_argument("--host-dependency", help="Debian dependency expression for the PipeWireAO host")
    parser.add_argument(
        "--extra-depends",
        action="append",
        default=[],
        help="additional Debian dependency expression; repeat when needed",
    )
    parser.add_argument(
        "--extra-depends-json",
        default="[]",
        help="JSON list of additional Debian dependency expressions",
    )
    parser.add_argument(
        "--library-dir",
        action="append",
        default=[],
        type=Path,
        help="additional directory searched by dpkg-shlibdeps",
    )
    parser.add_argument(
        "--library-dirs-json",
        default="[]",
        help="JSON list of additional directories searched by dpkg-shlibdeps",
    )
    parser.add_argument(
        "--meson-option",
        action="append",
        default=[],
        metavar="NAME=VALUE",
        help="override or add a Meson option after component defaults",
    )
    parser.add_argument(
        "--meson-options-json",
        default="[]",
        help="JSON list of NAME=VALUE Meson options",
    )
    parser.add_argument("--maintainer", help="Debian package maintainer in NAME <EMAIL> form")
    parser.add_argument("--homepage", default="https://github.com/DarrylGamroth/pipewireao-spa-plugins")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    manifest = load_manifest()
    components = manifest.get("components")
    if not isinstance(components, dict):
        raise SystemExit("components must be an object")
    if args.list_components:
        for name, value in components.items():
            print(f"{name}\t{value['package']}")
        return
    if not args.component:
        raise SystemExit("--component is required unless --list-components is used")

    args.extra_depends.extend(
        json_string_list("--extra-depends-json", args.extra_depends_json)
    )
    args.library_dir.extend(
        Path(value) for value in json_string_list("--library-dirs-json", args.library_dirs_json)
    )
    args.library_dir = [path.resolve() for path in args.library_dir]
    args.meson_option.extend(
        json_string_list("--meson-options-json", args.meson_options_json)
    )

    component = component_configuration(manifest, args.component)
    validate_meson_overrides(manifest, args.meson_option)
    package = component.get("package")
    tags = component.get("install_tags")
    targets = component.get("build_targets", [])
    if not isinstance(package, str) or not package:
        raise SystemExit("component package must be a non-empty string")
    if not isinstance(tags, list) or not tags or not all(isinstance(tag, str) for tag in tags):
        raise SystemExit("component install_tags must be a non-empty list of strings")
    if targets is not None and (
        not isinstance(targets, list) or not all(isinstance(target, str) for target in targets)
    ):
        raise SystemExit("component build_targets must be null or a list of strings")
    if not args.host_dependency:
        raise SystemExit("--host-dependency is required so loader-only plugins cannot be under-declared")
    if not args.maintainer:
        raise SystemExit("--maintainer is required; deployment packages must not use placeholder identity")
    if component.get("private_runtime") and not args.extra_depends:
        raise SystemExit(
            f"component {args.component!r} requires at least one --extra-depends expression "
            "for its privately packaged SDK runtime"
        )
    validate_control_value("host dependency", args.host_dependency)
    validate_control_value("maintainer", args.maintainer)
    validate_control_value("homepage", args.homepage)
    for dependency in args.extra_depends:
        validate_control_value("extra dependency", dependency)

    require_tools(["meson", "dpkg", "dpkg-architecture", "dpkg-deb", "dpkg-shlibdeps"])
    source_root = args.source_root.resolve()
    if not (source_root / "meson.build").is_file():
        raise SystemExit(f"source root does not contain meson.build: {source_root}")
    with tempfile.TemporaryDirectory(prefix=f"pwao-deb-{args.component}-") as temporary:
        workspace = Path(temporary)
        build_dir = workspace / "build"
        package_root = workspace / "package"
        scan_workspace = workspace / "dependency-scan"
        setup = [
            "meson",
            "setup",
            str(build_dir),
            str(source_root),
            "--buildtype=release",
            "--prefix=/usr",
            f"--libdir=lib/{run(['dpkg-architecture', '-qDEB_HOST_MULTIARCH'])}",
            "--wrap-mode=nodownload",
            "-Dstrip=true",
            "-Dtests=disabled",
            *meson_options(manifest, component),
            *(f"-D{option}" for option in args.meson_option),
        ]
        run(setup)
        if targets is not None:
            compile_command = ["meson", "compile", "-C", str(build_dir), *targets]
            run(compile_command)
        run(
            [
                "meson",
                "install",
                "-C",
                str(build_dir),
                "--no-rebuild",
                "--destdir",
                str(package_root),
                "--tags",
                ",".join(tags),
            ]
        )
        payload = validate_payload(package_root, component)
        if not payload:
            raise SystemExit(f"component {args.component!r} installed no files")

        version = args.version or f"{project_version(build_dir)}-{args.revision}"
        run(["dpkg", "--validate-version", version])
        arch = architecture(component)

        document_dir = package_root / "usr" / "share" / "doc" / package
        document_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(COPYRIGHT, document_dir / "copyright")

        dependencies = shlib_dependencies(
            elf_files(package_root), package, args.library_dir, scan_workspace
        )
        declared = [args.host_dependency, *args.extra_depends]
        if component.get("depends_on_core"):
            declared.insert(0, f"pipewireao-spa-plugins-core (= {version})")
        if dependencies:
            declared.insert(0, dependencies)
        depends = ", ".join(dict.fromkeys(item.strip() for item in declared if item.strip()))

        metadata = package_root / "DEBIAN"
        metadata.mkdir()
        control = [
            f"Package: {package}",
            f"Version: {version}",
            f"Architecture: {arch}",
            f"Maintainer: {args.maintainer}",
            f"Section: {component.get('section', 'libs')}",
            "Priority: optional",
            f"Installed-Size: {installed_size(package_root)}",
            f"Depends: {depends}",
            f"Homepage: {args.homepage}",
            f"X-PipeWireAO-Component: {args.component}",
            control_description(component),
            "",
        ]
        (metadata / "control").write_text("\n".join(control), encoding="utf-8")
        write_md5sums(package_root)

        epoch = int(os.environ.get("SOURCE_DATE_EPOCH", str(DEFAULT_EPOCH)))
        normalize_permissions(package_root)
        normalize_mtimes(package_root, epoch)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        output = args.output_dir / f"{package}_{version}_{arch}.deb"
        environment = os.environ.copy()
        environment["SOURCE_DATE_EPOCH"] = str(epoch)
        subprocess.run(
            ["dpkg-deb", "--root-owner-group", "-Zxz", "-z9", "--build", package_root, output],
            check=True,
            env=environment,
        )
        print(output)


if __name__ == "__main__":
    main()
