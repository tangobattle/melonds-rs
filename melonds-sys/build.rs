fn main() {
    let manifest_dir = std::path::PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let melonds_dir = manifest_dir.join("melonDS");

    // LTO stays off: MSVC emits LTCG objects that the final Rust link
    // (which does not pass /LTCG) cannot resolve.
    // The core static lib, built exactly as headless as it goes: no
    // frontend, no OpenGL, no GDB stub, and — not by choice — no JIT.
    //
    // melonDS gates the JIT on `cmake_dependent_option` over an
    // ARCHITECTURE detected with check_symbol_exists("__x86_64__"),
    // which is a GCC/Clang predefined macro that MSVC does not define.
    // The condition therefore fails and the option is FORCED off, so
    // -DENABLE_JIT=ON is silently ignored and no ARMJIT source compiles
    // (verified: zero ARMJIT objects, and identical throughput with the
    // flag on and off). Turning it on needs ARCHITECTURE forced plus an
    // assembler MSVC accepts for ARMJIT_x64 — worth doing, since two
    // interpreted ARM cores per console are what hold a link under
    // realtime.
    let dst = cmake::Config::new(&melonds_dir)
        .define("BUILD_QT_SDL", "OFF")
        .define("ENABLE_OGLRENDERER", "OFF")
        .define("ENABLE_GDBSTUB", "OFF")
        .define("ENABLE_JIT", "OFF")
        .define("ENABLE_LTO_RELEASE", "OFF")
        .define("DIRENT_INCLUDE_DIRS", melonds_dir.join("msvc-include"))
        .profile("Release")
        .build_target("core")
        .build();

    // Multi-config generators (Visual Studio) put libs under a
    // per-config subdirectory; single-config ones (Ninja/Makefiles)
    // don't. Probe both.
    let build = dst.join("build");
    for dir in [
        build.join("src").join("Release"),
        build.join("src"),
        build.join("src").join("teakra").join("src").join("Release"),
        build.join("src").join("teakra").join("src"),
    ] {
        println!("cargo:rustc-link-search=native={}", dir.display());
    }
    println!("cargo:rustc-link-lib=static=core");
    println!("cargo:rustc-link-lib=static=teakra");

    cc::Build::new()
        .cpp(true)
        .std("c++20")
        .file("shim/shim.cpp")
        .include(melonds_dir.join("src"))
        .include("shim")
        .flag_if_supported("/EHsc")
        .compile("melonds_shim");

    bindgen::Builder::default()
        .header(manifest_dir.join("shim").join("shim.h").to_str().unwrap().to_owned())
        .allowlist_item("Mds.*|mds_.*")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("failed to generate bindings")
        .write_to_file(out_dir.join("bindings.rs"))
        .expect("failed to write bindings");

    println!("cargo:rerun-if-changed=shim/shim.cpp");
    println!("cargo:rerun-if-changed=shim/shim.h");
    println!("cargo:rerun-if-changed=melonDS/src");
}
