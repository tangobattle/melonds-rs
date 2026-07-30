//! Builds melonDS and the embedding shim, then links them.
//!
//! Two shapes, one seam (the C ABI in shim.h):
//!
//! - **Unix (macOS/Linux):** the system toolchain compiles the core, the
//!   shim, and the final link alike, so everything is linked statically
//!   into the Rust binary — no shared library to place at runtime.
//!
//! - **Windows:** the core is compiled with **MSYS2 UCRT64** (melonDS's
//!   own recommended Windows toolchain) rather than MSVC, because the
//!   JIT is GCC/Clang code — GAS-syntax linkage stub, variable-length
//!   arrays, GNU declarations — that MSVC rejects outright. Mixing
//!   toolchains is safe because all C++ lives inside a DLL,
//!   libgcc/libstdc++ are linked into it statically, and the Rust side
//!   (an `x86_64-pc-windows-msvc` build) sees plain C functions through
//!   an import library. That keeps this crate usable from a normal
//!   MSVC-target workspace with no toolchain migration.
//!
//! Two interpreted ARM cores per console cannot hold 60 fps for a link,
//! so the JIT is not optional here.

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let melonds_dir = manifest_dir.join("melonDS");

    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap();

    // The JIT is on: interpreting four ARM cores cannot hold 60 fps for
    // a link, and with the block-cache flush removed from the savestate
    // load path (see the vendored NDS.cpp) a link still replays
    // bit-identically across a restore. MELONDS_JIT=0 falls back to the
    // interpreter.
    //
    // The support matrix mirrors melonDS's own CMake: x86_64/aarch64
    // only, and force-off on x86_64 macOS. Its cmake_dependent_option
    // would silently override a `-DENABLE_JIT=ON` there, and the shim
    // must agree with the core on JIT_ENABLED (it changes the NDS
    // object's size), so the decision is made here and handed to CMake
    // explicitly.
    let jit_supported = matches!(target_arch.as_str(), "x86_64" | "aarch64")
        && !(target_os == "macos" && target_arch == "x86_64");
    let jit = jit_supported && std::env::var("MELONDS_JIT").map(|v| v != "0").unwrap_or(true);
    println!("cargo:rerun-if-env-changed=MELONDS_JIT");

    if target_os == "windows" {
        build_windows(&manifest_dir, &out_dir, &melonds_dir, jit);
    } else {
        build_unix(&manifest_dir, &melonds_dir, &target_os, jit);
    }

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

// ---------------------------------------------------------------------
// Unix: everything static, one toolchain end to end.

fn build_unix(manifest_dir: &Path, melonds_dir: &Path, target_os: &str, jit: bool) {
    // The core static lib, as headless as it goes: no frontend, no
    // OpenGL, no GDB stub. LTO'd archives only survive a link driven by
    // the compiler that made them, and ours is driven by rustc — so
    // plain objects.
    let build_dir = cmake::Config::new(melonds_dir)
        .profile("Release")
        .define("BUILD_QT_SDL", "OFF")
        .define("ENABLE_OGLRENDERER", "OFF")
        .define("ENABLE_GDBSTUB", "OFF")
        .define("ENABLE_JIT", if jit { "ON" } else { "OFF" })
        .define("ENABLE_LTO_RELEASE", "OFF")
        .define("ENABLE_LTO", "OFF")
        .build_target("core")
        .build()
        .join("build");

    // The shim, compiled here rather than through CMake. JIT_ENABLED is
    // a PUBLIC compile definition on CMake's `core` target, so it has
    // to be repeated by hand: without it the shim sees an NDS without
    // its JIT members, allocates that smaller object, and the core's
    // constructor writes past the end of it. That corruption presents
    // as a SIGSEGV deep inside the ARMJIT constructor and looks nothing
    // like an ABI mismatch. -fwrapv likewise is PUBLIC on `core`.
    let mut shim = cc::Build::new();
    shim.cpp(true)
        .std("c++20")
        .file(manifest_dir.join("shim/shim.cpp"))
        .include(melonds_dir.join("src"))
        .include(manifest_dir.join("shim"))
        .flag("-fwrapv");
    if jit {
        shim.define("JIT_ENABLED", None);
    }
    // Emits the link-search + `static=melonds_shim` + C++ stdlib lines;
    // the shim archive must precede the core's for single-pass linkers,
    // so this comes first.
    shim.compile("melonds_shim");

    println!("cargo:rustc-link-search=native={}", build_dir.join("src").display());
    println!("cargo:rustc-link-search=native={}", build_dir.join("src/teakra/src").display());
    println!("cargo:rustc-link-lib=static=core");
    println!("cargo:rustc-link-lib=static=teakra");
    // The JIT's memory backend maps its fastmem arena with shm_open,
    // which glibc < 2.34 keeps in librt.
    if target_os == "linux" && std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("gnu") {
        println!("cargo:rustc-link-lib=dylib=rt");
    }
}

// ---------------------------------------------------------------------
// Windows: UCRT64-built DLL behind an MSVC import library.

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

fn build_windows(manifest_dir: &Path, out_dir: &Path, melonds_dir: &Path, jit: bool) {
    let ucrt = ucrt64_root();
    let bin = ucrt.join("bin");
    let (gcc, gpp) = (bin.join("gcc.exe"), bin.join("g++.exe"));
    let cmake = bin.join("cmake.exe");
    let path_with_ucrt = format!("{};{}", bin.display(), std::env::var("PATH").unwrap_or_default());

    // The core static lib, as headless as it goes: no frontend, no
    // OpenGL, no GDB stub — and the JIT on, which is the whole reason
    // for this toolchain.
    let build_dir = out_dir.join("core-build");

    // Cargo reuses OUT_DIR across revisions of a git dependency, but
    // each revision checks out under its own path, and CMake refuses a
    // cache whose source directory moved. A cache pointing at some
    // other checkout is stale by definition — start over. (The cmake
    // crate does the same for the Unix path.)
    if let Ok(cache) = std::fs::read_to_string(build_dir.join("CMakeCache.txt")) {
        let ours = format!("CMAKE_HOME_DIRECTORY:INTERNAL={}", cmake_path(melonds_dir));
        if !cache.lines().any(|line| line == ours) {
            std::fs::remove_dir_all(&build_dir).expect("failed to clear stale core-build");
        }
    }

    run(
        "cmake configure",
        Command::new(&cmake)
            .arg("-S")
            .arg(melonds_dir)
            .arg("-B")
            .arg(&build_dir)
            .arg("-G")
            .arg("Ninja")
            .arg("-DCMAKE_BUILD_TYPE=Release")
            // DWARF for the whole core, so a crash offset in a user's
            // minidump resolves to file:line. It rides in a separate
            // .dbg next to the DLL (below), not in what ships.
            .arg("-DCMAKE_C_FLAGS=-g")
            .arg("-DCMAKE_CXX_FLAGS=-g")
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

    // The shim plus the core, linked into one DLL. The whole GNU
    // runtime goes in statically so running it needs nothing from
    // MSYS2 on PATH.
    let dll = out_dir.join("melonds_shim.dll");
    let def = out_dir.join("melonds_shim.def");
    run(
        "link shim dll",
        Command::new(&gpp)
            .arg("-O2")
            .arg("-g")
            .arg("-std=c++20")
            .arg("-shared")
            .arg("-o")
            .arg(&dll)
            .arg(manifest_dir.join("shim/shim.cpp"))
            .arg("-I")
            .arg(melonds_dir.join("src"))
            .arg("-I")
            .arg(manifest_dir.join("shim"))
            // JIT_ENABLED is a PUBLIC compile definition on CMake's
            // `core` target, so every translation unit that includes
            // NDS.h must have it. The shim is compiled here rather than
            // through CMake, so it has to be repeated by hand: without
            // it the shim sees an NDS without its JIT members, allocates
            // that smaller object, and the core's constructor writes
            // past the end of it. That corruption presents as a SIGSEGV
            // deep inside the ARMJIT constructor and looks nothing like
            // an ABI mismatch.
            .args(if jit { &["-DJIT_ENABLED"][..] } else { &[][..] })
            .arg(build_dir.join("src/libcore.a"))
            .arg(build_dir.join("src/teakra/src/libteakra.a"))
            // The JIT's memory backend wants the Windows 8 mapping APIs,
            // which CMake asks for as MSVC's `onecore`; MinGW ships the
            // same import set as `mincore`.
            .args(["-lws2_32", "-lmincore", "-lbcrypt"])
            // -static-libgcc/-static-libstdc++ alone leave winpthreads
            // dynamic: the DLL then imports libwinpthread-1.dll — under
            // newer mingw-w64 by its 64-bit-time_t clock_gettime64
            // symbol — and fails to load on any machine that doesn't
            // have a matching MSYS2 on PATH. -static folds it in.
            .args(["-static", "-static-libgcc", "-static-libstdc++"])
            // MinGW's --out-implib is a GNU-format archive the MSVC
            // linker cannot read, so export a .def instead and let
            // lib.exe turn it into a real import library below.
            .arg(format!("-Wl,--output-def,{}", def.display()))
            .env("PATH", &path_with_ucrt),
    );

    // The DWARF moves out into melonds_shim.dbg: a crash offset from a
    // user's minidump still resolves to file:line through the side file
    // (addr2line -e melonds_shim.dbg), while the DLL that actually
    // ships stays lean.
    let dbg = out_dir.join("melonds_shim.dbg");
    let objcopy = bin.join("objcopy.exe");
    run(
        "objcopy split debug info",
        Command::new(&objcopy).arg("--only-keep-debug").arg(&dll).arg(&dbg),
    );
    run(
        "objcopy strip debug info",
        Command::new(&objcopy)
            .arg("--strip-debug")
            .arg(format!("--add-gnu-debuglink={}", dbg.display()))
            .arg(&dll),
    );

    // The import library, built by MSVC so an x86_64-pc-windows-msvc
    // link accepts it — but not from the linker's own def: ld exports
    // every symbol in the image and numbers them, and those ordinals
    // shift with any change to the symbol population. lib.exe binds
    // imports to the ordinals, welding the executable to the exact DLL
    // build it linked against; run it beside any other build and every
    // call lands on whatever now holds the ordinal — observed as an
    // mds_* call executing Teakra typeinfo. Rewriting the def down to
    // the mds_* names, ordinal-free, binds imports by name instead: a
    // mismatched pair then either works (the C ABI is the stable part)
    // or refuses to load with the missing name spelled out. This also
    // overwrites any stale static lib left in OUT_DIR by an earlier
    // build.
    let import_def = out_dir.join("melonds_shim_import.def");
    let mds_names: String = std::fs::read_to_string(&def)
        .expect("failed to read linker-generated def")
        .lines()
        .filter_map(|line| line.split_whitespace().next())
        .filter(|name| name.starts_with("mds_"))
        .map(|name| format!("    {name}\n"))
        .collect();
    assert!(!mds_names.is_empty(), "no mds_* exports in linker-generated def");
    std::fs::write(&import_def, format!("LIBRARY melonds_shim.dll\nEXPORTS\n{mds_names}"))
        .expect("failed to write import def");
    let cl = cc::Build::new().get_compiler();
    let lib_exe = cl.path().parent().expect("compiler has no directory").join("lib.exe");
    run(
        "lib.exe import library",
        Command::new(&lib_exe)
            .arg("/NOLOGO")
            .arg("/MACHINE:X64")
            .arg(format!("/DEF:{}", import_def.display()))
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
            let _ = std::fs::copy(&dbg, dir.join("melonds_shim.dbg"));
        }
    }

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=dylib=melonds_shim");
}
