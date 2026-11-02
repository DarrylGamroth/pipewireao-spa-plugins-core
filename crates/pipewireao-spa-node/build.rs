//! Builds the narrow C bridge for fixed-choice POD normalization.

use std::env;

fn main() {
    println!("cargo:rerun-if-changed=src/pod.c");
    let spa = pkg_config::Config::new()
        .cargo_metadata(false)
        .probe("libspa-ao-0.2")
        .expect("PipeWireAO SPA headers are required");
    let mut build = cc::Build::new();
    build
        .file("src/pod.c")
        .file("../../spa/plugins/metadata.c")
        .include("../../include");
    let mut bindings = bindgen::Builder::default()
        .header("../../spa/plugins/metadata.h")
        .allowlist_type("pwao_metadata")
        .allowlist_function("pwao_metadata_parse")
        .blocklist_type("spa_meta_requirement")
        .raw_line("use libspa::sys::spa_meta_requirement;");
    println!("cargo:rerun-if-changed=../../spa/plugins/metadata.h");
    println!("cargo:rerun-if-changed=../../spa/plugins/metadata.c");
    println!("cargo:rerun-if-env-changed=PIPEWIREAO_SPA_INCLUDE_DIR");
    if let Some(include) = env::var_os("PIPEWIREAO_SPA_INCLUDE_DIR") {
        bindings = bindings.clang_arg(format!("-I{}", std::path::Path::new(&include).display()));
        build.include(include);
    }
    for include in spa.include_paths {
        bindings = bindings.clang_arg(format!("-I{}", std::path::Path::new(&include).display()));
        build.include(include);
    }
    bindings
        .generate()
        .expect("metadata profile bindings")
        .write_to_file(
            std::path::PathBuf::from(env::var_os("OUT_DIR").unwrap()).join("metadata.rs"),
        )
        .expect("write metadata profile bindings");
    build.warnings(true).compile("pipewireao_spa_pod");
}
