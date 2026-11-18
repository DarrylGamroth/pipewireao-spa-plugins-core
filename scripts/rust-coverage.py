#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Report Rust line coverage from Cargo tests and the SPA host tests."""

import argparse
import json
from pathlib import Path
import subprocess


def cargo_tests():
    executables = []
    with subprocess.Popen(
        ["cargo", "test", "--workspace", "--locked", "--message-format=json"],
        stdout=subprocess.PIPE,
        text=True,
    ) as cargo:
        for line in cargo.stdout:
            if not line.startswith("{"):
                print(line, end="", flush=True)
                continue
            event = json.loads(line)
            if (
                event.get("reason") == "compiler-artifact"
                and event["profile"]["test"]
                and event["executable"]
            ):
                executables.append(Path(event["executable"]))
        if cargo.wait():
            raise SystemExit(cargo.returncode)
    if not executables:
        raise SystemExit("Cargo reported no test executables")
    return executables


def llvm_tools():
    version = subprocess.check_output(["rustc", "-vV"], text=True)
    host = next(line.removeprefix("host: ") for line in version.splitlines()
                if line.startswith("host: "))
    sysroot = subprocess.check_output(["rustc", "--print", "sysroot"], text=True)
    return Path(sysroot.strip()) / "lib" / "rustlib" / host / "bin"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="instrumented Meson build directory")
    build = parser.parse_args().build
    objects = cargo_tests()
    # These plugins are loaded by Meson host tests.
    for directory, library in [
        ("hermes-decoder", "libspa-hermes-decoder.so"),
        ("ndarray", "libspa-ndarray.so"),
        ("nuvu", "libspa-nuvu.so"),
    ]:
        plugin = build / "spa" / "plugins" / directory / library
        if not plugin.is_file():
            raise SystemExit(f"Missing instrumented plugin {plugin}")
        objects.append(plugin)

    profiles = sorted((build / "rust-profiles").glob("*.profraw"))
    if not profiles:
        raise SystemExit("No Rust profiles found; build and test with coverage instrumentation")
    tools = llvm_tools()
    merged = build / "coverage-rust.profdata"
    subprocess.run([tools / "llvm-profdata", "merge", "-sparse",
                    "-o", merged, *profiles], check=True)

    # The coverage environment remaps workspace sources to relative paths.
    # Absolute paths belong to dependencies, generated code, or the sysroot.
    with (build / "coverage-rust.info").open("w", encoding="utf-8") as output:
        subprocess.run(
            [tools / "llvm-cov", "export", "--format=lcov", "--skip-functions",
             "--instr-profile", merged, "--ignore-filename-regex",
             r"^/|(^|/)(tests|examples|benches)/|/(tests|.+[-_]tests)\.rs$",
             *[f"-object={obj}" for obj in objects]],
            stdout=output,
            check=True,
        )


if __name__ == "__main__":
    main()
