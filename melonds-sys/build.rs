//! Builds melonDS and the embedding shim as a DLL, then links it.
//!
//! The core is compiled with **MSYS2 UCRT64** (melonDS's own recommended
//! Windows toolchain) rather than MSVC, because the JIT is GCC/Clang
//! code — GAS-syntax linkage stub, variable-length arrays, GNU
//! declarations — that MSVC rejects outright. Two interpreted ARM cores
//! per console cannot hold 60 fps for a link, so the JIT is not
//! optional here.
//!
//! Status: the DLL boundary itself is sound — with the JIT off the core
//! boots, emulates, and round-trips savestates bit-identically through
//! it. With the JIT on, the process dies of stack overflow during
//! construction (before a frame runs, and with fastmem both on and off,
//! and on a 256 MB stack — so it is unbounded recursion in JIT init, not
//! stack depth). Hence MELONDS_JIT, off by default.
//!
//! Mixing toolchains is safe because the seam is a **C ABI**: all C++
//! lives inside the DLL, libgcc/libstdc++ are linked into it statically,
//! and the Rust side (an `x86_64-pc-windows-msvc` build) sees plain C
//! functions through an import library. That keeps this crate usable
//! from a normal MSVC-target workspace with no toolchain migration.

use std::path::{Path, PathBuf};
use std::process::Command;

/// Root of an MSYS2 UCRT64 installation.
fn ucrt64_root() -> PathBuf {
    if let Ok(dir) = std::env::var("UCRT64_ROOT") {
        return PathBuf::from(dir);
    }
    for candidate in ["C:/msys64/ucrt64", "C:/msys32/ucrt64"] {
        if Path::new(candidate).join("bin/g++.exe").exists() {
            return PathBuf::from(candidate);
        }
    }
    panic!(
        "MSYS2 UCRT64 not found. Install it (pacman -S mingw-w64-ucrt-x86_64-gcc \
         mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja) or set UCRT64_ROOT."
    );
}

/// CMake parses `-D` values as strings, where a backslash starts an
/// escape — so every path handed to it goes in with forward slashes.
fn cmake_path(path: &Path) -> String {
    path.display().to_string().replace('\\', "/")
}

fn run(what: &str, cmd: &mut Command) {
    let status = cmd.status().unwrap_or_else(|e| panic!("failed to spawn {what}: {e}"));
    assert!(status.success(), "{what} failed with {status}");
}

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let melonds_dir = manifest_dir.join("melonDS");

    // The JIT is why this crate is built with UCRT64 at all, but it does
    // not survive initialization yet (see below), so it is opt-in until
    // it does: MELONDS_JIT=1.
    let jit = std::env::var("MELONDS_JIT").map(|v| v != "0").unwrap_or(false);
    println!("cargo:rerun-if-env-changed=MELONDS_JIT");

    let ucrt = ucrt64_root();
    let bin = ucrt.join("bin");
    let (gcc, gpp) = (bin.join("gcc.exe"), bin.join("g++.exe"));
    let cmake = bin.join("cmake.exe");
    let path_with_ucrt = format!("{};{}", bin.display(), std::env::var("PATH").unwrap_or_default());

    // The core static lib, as headless as it goes: no frontend, no
    // OpenGL, no GDB stub — and the JIT on, which is the whole reason
    // for this toolchain.
    let build_dir = out_dir.join("core-build");
    run(
        "cmake configure",
        Command::new(&cmake)
            .arg("-S")
            .arg(&melonds_dir)
            .arg("-B")
            .arg(&build_dir)
            .arg("-G")
            .arg("Ninja")
            .arg("-DCMAKE_BUILD_TYPE=Release")
            .arg("-DBUILD_QT_SDL=OFF")
            .arg("-DENABLE_OGLRENDERER=OFF")
            .arg("-DENABLE_GDBSTUB=OFF")
            .arg(if jit { "-DENABLE_JIT=ON" } else { "-DENABLE_JIT=OFF" })
            .arg(format!("-DCMAKE_C_COMPILER={}", cmake_path(&gcc)))
            .arg(format!("-DCMAKE_CXX_COMPILER={}", cmake_path(&gpp)))
            .arg(format!("-DCMAKE_MAKE_PROGRAM={}", cmake_path(&bin.join("ninja.exe"))))
            .env("PATH", &path_with_ucrt),
    );
    run(
        "cmake build",
        Command::new(&cmake)
            .arg("--build")
            .arg(&build_dir)
            .arg("--target")
            .arg("core")
            .env("PATH", &path_with_ucrt),
    );

    // The shim plus the core, linked into one DLL. libgcc/libstdc++ go
    // in statically so running it needs nothing from MSYS2 on PATH.
    let dll = out_dir.join("melonds_shim.dll");
    let def = out_dir.join("melonds_shim.def");
    run(
        "link shim dll",
        Command::new(&gpp)
            .arg("-O2")
            .arg("-std=c++20")
            .arg("-shared")
            .arg("-o")
            .arg(&dll)
            .arg(manifest_dir.join("shim/shim.cpp"))
            .arg("-I")
            .arg(melonds_dir.join("src"))
            .arg("-I")
            .arg(manifest_dir.join("shim"))
            .arg(build_dir.join("src/libcore.a"))
            .arg(build_dir.join("src/teakra/src/libteakra.a"))
            // The JIT's memory backend wants the Windows 8 mapping APIs,
            // which CMake asks for as MSVC's `onecore`; MinGW ships the
            // same import set as `mincore`.
            .args(["-lws2_32", "-lmincore", "-lbcrypt"])
            .args(["-static-libgcc", "-static-libstdc++"])
            // MinGW's --out-implib is a GNU-format archive the MSVC
            // linker cannot read, so export a .def instead and let
            // lib.exe turn it into a real import library below.
            .arg(format!("-Wl,--output-def,{}", def.display()))
            .env("PATH", &path_with_ucrt),
    );

    // The import library, built by MSVC from the DLL's own export list
    // so an x86_64-pc-windows-msvc link accepts it. This also overwrites
    // any stale static lib left in OUT_DIR by an earlier build.
    let cl = cc::Build::new().get_compiler();
    let lib_exe = cl.path().parent().expect("compiler has no directory").join("lib.exe");
    run(
        "lib.exe import library",
        Command::new(&lib_exe)
            .arg("/NOLOGO")
            .arg("/MACHINE:X64")
            .arg(format!("/DEF:{}", def.display()))
            .arg(format!("/OUT:{}", out_dir.join("melonds_shim.lib").display())),
    );

    // Cargo links against the import library; the DLL has to sit beside
    // the executable at runtime, which for cargo's layout means the
    // profile directory a few levels above OUT_DIR (plus its examples/
    // and deps/ subdirectories).
    let profile_dir = out_dir
        .ancestors()
        .nth(3)
        .expect("OUT_DIR is deeper than cargo's layout")
        .to_path_buf();
    for dir in [
        profile_dir.clone(),
        profile_dir.join("examples"),
        profile_dir.join("deps"),
    ] {
        if dir.exists() || std::fs::create_dir_all(&dir).is_ok() {
            let _ = std::fs::copy(&dll, dir.join("melonds_shim.dll"));
        }
    }

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=dylib=melonds_shim");

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
