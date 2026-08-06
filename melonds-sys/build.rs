//! Builds melonDS and the embedding shim, then links them.
//!
//! One shape on every platform: the core, teakra and the shim are static
//! archives that rustc links straight into the binary, so nothing has to
//! be placed beside the executable at runtime.
//!
//! Windows needs one thing arranged for it: the core is built by
//! **clang driving the MSVC target**, not `cl.exe`. melonDS is GCC/Clang
//! code in places `cl.exe` rejects outright — variable-length arrays,
//! `__attribute__((packed))`, and (with the JIT built in) a GAS-syntax
//! linkage stub and `__asm__("cpuid")`. Clang on the MSVC target accepts
//! all of it, its integrated assembler handles the `.S`, and it emits
//! MSVC-ABI COFF with the MSVC STL and SEH unwind, which `link.exe`
//! reads natively. The JIT is off by default (see `main`), but the
//! compiler choice is not conditional on that: the same toolchain has to
//! build both configurations.
//!
//! Note clang, not clang-cl: CMake sets `MSVC` for clang-cl, and
//! melonDS's `if (NOT MSVC)` block is what applies `-fwrapv` — PUBLIC on
//! the core target. Losing it is a silent signed-overflow change, which
//! for an emulator means a determinism change, not a build error.

use std::path::{Path, PathBuf};
use std::process::Command;

/// The embedder's two halves: the C ABI the Rust side calls, and the
/// Platform:: implementation melonDS resolves at link time.
const SHIM_SOURCES: [&str; 2] = ["shim.cpp", "platform.cpp"];

fn main() {
    let manifest_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").unwrap());
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let melonds_dir = manifest_dir.join("melonDS");

    let target_os = std::env::var("CARGO_CFG_TARGET_OS").unwrap();
    let target_arch = std::env::var("CARGO_CFG_TARGET_ARCH").unwrap();

    // The JIT is off. It is faster — interpreting four ARM cores is a
    // large multiple of the work — but its compiled blocks are a second
    // copy of the console's timing behaviour that everything else here
    // has to keep coherent: savestates have to carry it, rollback has to
    // restore it, and a trap or watch set has to re-form it. Every one
    // of those seams has produced a divergence, and each cost a protocol
    // version to fix. The interpreter has no such state, so a console is
    // a function of its inputs and nothing else. MELONDS_JIT=1 opts back
    // in, for profiling the difference.
    //
    // The support matrix mirrors melonDS's own CMake: x86_64/aarch64
    // only, and force-off on x86_64 macOS. Its cmake_dependent_option
    // would silently override a `-DENABLE_JIT=ON` there, and the shim
    // must agree with the core on JIT_ENABLED (it changes the NDS
    // object's size), so the decision is made here and handed to CMake
    // explicitly.
    let jit_supported = matches!(target_arch.as_str(), "x86_64" | "aarch64")
        && !(target_os == "macos" && target_arch == "x86_64");
    let jit = jit_supported && std::env::var("MELONDS_JIT").map(|v| v == "1").unwrap_or(false);
    println!("cargo:rerun-if-env-changed=MELONDS_JIT");

    let wasm = target_arch == "wasm32";
    if wasm {
        build_wasm(&manifest_dir, &melonds_dir, &out_dir);
        return;
    }

    // The compiler that builds the core also builds the shim: they share
    // NDS.h, and an ABI split across that header is not a link error but
    // a silent layout disagreement.
    let clang = (target_os == "windows").then(windows_clang);

    // Every object in the link has to agree on the C runtime, rustc's
    // included. rustc takes the dynamic UCRT unless the crt-static
    // target feature is set, and neither of the two compilers here
    // defaults to matching it: CMake asks for `/MD` while clang's
    // GNU driver defaults to the static CRT. Both get told explicitly.
    // A mismatch is at best LNK2038 and at worst two heaps.
    let crt_static = std::env::var("CARGO_CFG_TARGET_FEATURE")
        .map(|features| features.split(',').any(|f| f == "crt-static"))
        .unwrap_or(false);

    let build_dir = if let Some(clang) = &clang {
        build_core_windows(&out_dir, &melonds_dir, jit, clang, crt_static)
    } else {
        build_core_unix(&melonds_dir, jit)
    };

    compile_shim(&manifest_dir, &melonds_dir, jit, clang.as_ref(), crt_static);
    emit_link_directives(&build_dir, &target_os);

    bindgen::Builder::default()
        .header(manifest_dir.join("shim").join("shim.h").to_str().unwrap().to_owned())
        .allowlist_item("Mds.*|mds_.*")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("failed to generate bindings")
        .write_to_file(out_dir.join("bindings.rs"))
        .expect("failed to write bindings");

    println!("cargo:rerun-if-changed=shim");
    println!("cargo:rerun-if-changed=melonDS/src");
}

// ---------------------------------------------------------------------
// wasm32.

/// The whole build for a browser: core, teakra and shim compiled by
/// wasi-sdk, then bindgen run against wasi-libc's headers.
///
/// **The C++ is compiled for `wasm32-wasip1-threads`, not plain
/// `wasm32-wasip1`.** teakra — melonDS's DSP, which the core links
/// whether or not a DS-mode game ever wakes it — takes `find_package
/// (Threads REQUIRED)` and uses `std::mutex`, and libc++ without threads
/// has no such type. The threaded sysroot also happens to be what a
/// linked pair needs anyway: two consoles have to run concurrently and
/// block on each other through the air, because a melonDS frame cannot
/// be suspended half way the way an mgba core can be parked between
/// timing slices.
///
/// That choice reaches the whole module. A threaded wasm build wants
/// shared memory and the atomics feature, so the Rust half has to be
/// built to match (`-Ctarget-feature=+atomics,+bulk-memory,+mutable-globals`
/// over a `-Zbuild-std` std), and a page that instantiates it has to be
/// served cross-origin-isolated. None of that is this file's business,
/// but a link failure about mismatched memory is where it shows up.
fn build_wasm(manifest_dir: &Path, melonds_dir: &Path, out_dir: &Path) {
    let sdk = wasi_sdk();
    let sysroot = sdk.join("share").join("wasi-sysroot");
    let build_dir = out_dir.join("core-build");

    // Same reasoning as the Windows path: a cache whose source directory
    // moved (a new checkout of this git dependency) is stale, and CMake
    // treats that as an error rather than a reconfigure.
    if let Ok(cache) = std::fs::read_to_string(build_dir.join("CMakeCache.txt")) {
        let home = cache.lines().find_map(|line| {
            let (name, value) = line.split_once('=')?;
            let name = name.split_once(':').map_or(name, |(name, _)| name);
            (name == "CMAKE_HOME_DIRECTORY").then_some(value)
        });
        if home != Some(cmake_path(melonds_dir).as_str()) {
            std::fs::remove_dir_all(&build_dir).expect("failed to clear stale core-build");
        }
    }

    run(
        "cmake configure",
        Command::new("cmake")
            .arg("-S")
            .arg(melonds_dir)
            .arg("-B")
            .arg(&build_dir)
            .arg("-G")
            .arg("Ninja")
            .arg(format!(
                "-DCMAKE_TOOLCHAIN_FILE={}",
                cmake_path(&sdk.join("share").join("cmake").join("wasi-sdk-pthread.cmake"))
            ))
            .arg("-DCMAKE_BUILD_TYPE=Release")
            .arg("-DBUILD_QT_SDL=OFF")
            .arg("-DENABLE_OGLRENDERER=OFF")
            .arg("-DENABLE_GDBSTUB=OFF")
            // There is no wasm dynarec, and the interpreter is what
            // every other target runs anyway.
            .arg("-DENABLE_JIT=OFF")
            .arg("-DENABLE_LTO=OFF")
            .arg("-DENABLE_LTO_RELEASE=OFF"),
    );
    run(
        "cmake build",
        Command::new("cmake").arg("--build").arg(&build_dir).arg("--target").arg("core"),
    );

    // The shim shares NDS.h with the core, so it is built by the same
    // compiler with the same target — an ABI split across that header is
    // a silent layout disagreement, not a link error.
    cc::Build::new()
        .cpp(true)
        .compiler(sdk.join("bin").join("clang++"))
        .archiver(sdk.join("bin").join("llvm-ar"))
        .target("wasm32-wasip1-threads")
        .std("c++20")
        // cc translates the triple it was given into `wasm32-wasi`,
        // which is the threadless one — `-pthread` then buys nothing and
        // libc++ comes up without `std::this_thread`. Its own flag lands
        // first, and clang takes the last `--target`, so saying it again
        // here is what actually selects the sysroot the core was built
        // against.
        .flag("--target=wasm32-wasip1-threads")
        .flag(format!("--sysroot={}", sysroot.display()))
        .flag("-pthread")
        // PUBLIC on CMake's `core` target, so it has to be repeated for
        // anything that shares the headers.
        .flag("-fwrapv")
        .files(SHIM_SOURCES.map(|src| manifest_dir.join("shim").join(src)))
        .include(melonds_dir.join("src"))
        .include(manifest_dir.join("shim"))
        .compile("melonds_shim");

    // The syscall floor, C rather than C++ — the __wasi_* names must
    // link unmangled to shadow wasi-libc's import-backed versions. See
    // the note at the top of wasi-shim.c.
    cc::Build::new()
        .compiler(sdk.join("bin").join("clang"))
        .archiver(sdk.join("bin").join("llvm-ar"))
        .target("wasm32-wasip1-threads")
        .flag("--target=wasm32-wasip1-threads")
        .flag(format!("--sysroot={}", sysroot.display()))
        .flag("-pthread")
        .file(manifest_dir.join("shim").join("wasi-shim.c"))
        .file(manifest_dir.join("shim").join("wasi-stubs.c"))
        .compile("melonds_wasi_shim");

    println!("cargo:rustc-link-search=native={}", build_dir.join("src").display());
    println!("cargo:rustc-link-search=native={}", build_dir.join("src/teakra/src").display());
    println!("cargo:rustc-link-lib=static=core");
    println!("cargo:rustc-link-lib=static=teakra");
    // wasi-libc and the C++ runtime, which rustc does not bring for a
    // target whose Rust half has no libc of its own.
    println!(
        "cargo:rustc-link-search=native={}",
        sysroot.join("lib").join("wasm32-wasip1-threads").display()
    );
    for lib in ["c", "c++", "c++abi"] {
        println!("cargo:rustc-link-lib=static={lib}");
    }
    // The i64/i128/float helpers the C++ objects call and Rust's own
    // compiler-builtins may not export.
    if let Some(dir) = wasm32_builtins_dir(&sdk) {
        println!("cargo:rustc-link-search=native={}", dir.display());
        println!("cargo:rustc-link-lib=static=clang_rt.builtins-wasm32");
    }

    bindgen::Builder::default()
        .header(manifest_dir.join("shim").join("shim.h").to_str().unwrap().to_owned())
        .allowlist_item("Mds.*|mds_.*")
        // Parse the header exactly as the wasi compile saw it: ILP32
        // layouts and wasi-libc's headers, not the host's. Needs a
        // wasm-aware libclang (LIBCLANG_PATH).
        .clang_args([
            "--target=wasm32-wasip1-threads".to_string(),
            format!("--sysroot={}", sysroot.display()),
            // clang defaults wasm symbols to hidden, and bindgen drops
            // every function it considers non-linkable.
            "-fvisibility=default".to_string(),
        ])
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("failed to generate bindings")
        .write_to_file(out_dir.join("bindings.rs"))
        .expect("failed to write bindings");

    println!("cargo:rerun-if-changed=shim");
    println!("cargo:rerun-if-changed=melonDS/src");
    println!("cargo:rerun-if-env-changed=WASI_SDK_PATH");
}

/// The wasi-sdk root, from `WASI_SDK_PATH`.
fn wasi_sdk() -> PathBuf {
    println!("cargo:rerun-if-env-changed=WASI_SDK_PATH");
    PathBuf::from(
        std::env::var("WASI_SDK_PATH")
            .expect("set WASI_SDK_PATH to a wasi-sdk root to build melonDS for wasm32"),
    )
}

/// wasi-sdk's compiler-rt builtins archive for wasm32. The layout moved
/// across sdk releases, so this globs rather than pinning a clang
/// version.
fn wasm32_builtins_dir(sdk: &Path) -> Option<PathBuf> {
    for version in std::fs::read_dir(sdk.join("lib").join("clang")).ok()? {
        let lib = version.ok()?.path().join("lib");
        for flavor in std::fs::read_dir(lib).ok()? {
            let dir = flavor.ok()?.path();
            if dir.join("libclang_rt.builtins-wasm32.a").exists() {
                return Some(dir);
            }
        }
    }
    None
}

// ---------------------------------------------------------------------
// The core.

/// The core static lib, as headless as it goes: no frontend, no OpenGL,
/// no GDB stub. LTO'd archives only survive a link driven by the
/// compiler that made them, and ours is driven by rustc — so plain
/// objects. Returns the CMake binary directory.
fn build_core_unix(melonds_dir: &Path, jit: bool) -> PathBuf {
    cmake::Config::new(melonds_dir)
        .profile("Release")
        .define("BUILD_QT_SDL", "OFF")
        .define("ENABLE_OGLRENDERER", "OFF")
        .define("ENABLE_GDBSTUB", "OFF")
        .define("ENABLE_JIT", if jit { "ON" } else { "OFF" })
        .define("ENABLE_LTO_RELEASE", "OFF")
        .define("ENABLE_LTO", "OFF")
        .build_target("core")
        .build()
        .join("build")
}

/// The same core, configured by hand. The cmake crate derives its
/// generator and compiler flags from the target triple, which for
/// `windows-msvc` means a Visual Studio generator and `cl`-shaped flags
/// handed to a compiler that wants neither — so CMake is driven
/// directly instead.
fn build_core_windows(
    out_dir: &Path,
    melonds_dir: &Path,
    jit: bool,
    clang: &Clang,
    crt_static: bool,
) -> PathBuf {
    let build_dir = out_dir.join("core-build");

    // Cargo reuses OUT_DIR across revisions of a git dependency, but
    // each revision checks out under its own path, and CMake refuses a
    // cache whose source directory moved. A cache pointing at some
    // other checkout is stale by definition — start over. (The cmake
    // crate does the same for the Unix path.)
    //
    // The compiler is checked the same way and for a sharper reason:
    // this build used to run MSYS2 g++ into this very directory, and
    // CMake treats a changed CMAKE_C_COMPILER as a hard error rather
    // than a reconfigure. Any tree that built the DLL once still has
    // that cache sitting here.
    if let Ok(cache) = std::fs::read_to_string(build_dir.join("CMakeCache.txt")) {
        // Entries are `NAME:TYPE=VALUE`, and the type an entry ends up
        // with depends on how it was first set — so match the name and
        // compare the value, never the whole line.
        let cached = |key: &str| {
            cache.lines().find_map(|line| {
                let (name, value) = line.split_once('=')?;
                let name = name.split_once(':').map_or(name, |(name, _)| name);
                (name == key).then_some(value)
            })
        };
        let matches = cached("CMAKE_HOME_DIRECTORY") == Some(cmake_path(melonds_dir).as_str())
            && cached("CMAKE_C_COMPILER") == Some(cmake_path(&clang.cc).as_str());
        if !matches {
            std::fs::remove_dir_all(&build_dir).expect("failed to clear stale core-build");
        }
    }

    run(
        "cmake configure",
        Command::new("cmake")
            .arg("-S")
            .arg(melonds_dir)
            .arg("-B")
            .arg(&build_dir)
            .arg("-G")
            .arg("Ninja")
            .arg("-DCMAKE_BUILD_TYPE=Release")
            // CodeView, which is what clang emits for `-g` on an MSVC
            // target: it rides in the objects, link.exe folds it into
            // the binary's PDB, and a crash offset in a user's minidump
            // resolves to file:line with no side file to ship.
            .arg("-DCMAKE_C_FLAGS=-g")
            .arg("-DCMAKE_CXX_FLAGS=-g")
            .arg("-DBUILD_QT_SDL=OFF")
            .arg("-DENABLE_OGLRENDERER=OFF")
            .arg("-DENABLE_GDBSTUB=OFF")
            .arg(if jit { "-DENABLE_JIT=ON" } else { "-DENABLE_JIT=OFF" })
            .arg("-DENABLE_LTO=OFF")
            .arg("-DENABLE_LTO_RELEASE=OFF")
            .arg(format!("-DCMAKE_C_COMPILER={}", cmake_path(&clang.cc)))
            .arg(format!("-DCMAKE_CXX_COMPILER={}", cmake_path(&clang.cxx)))
            // The JIT's linkage stub is GAS-syntax `.S`; clang's
            // integrated assembler is what makes it buildable here, so
            // the ASM language gets pinned to clang too.
            .arg(format!("-DCMAKE_ASM_COMPILER={}", cmake_path(&clang.cc)))
            // CMake's Windows-Clang platform module enables the RC
            // language whether or not anything here compiles a resource,
            // and MSVC's rc.exe lives in the Windows SDK — off PATH
            // outside a developer prompt. LLVM ships its own.
            .arg(format!("-DCMAKE_RC_COMPILER={}", cmake_path(&clang.rc)))
            // find_path(dirent.h) fires whenever MINGW is unset, and
            // there is no dirent.h in the MSVC headers. melonDS vendors
            // one for exactly this case.
            .arg(format!(
                "-DDIRENT_INCLUDE_DIRS={}",
                cmake_path(&melonds_dir.join("msvc-include"))
            ))
            .arg(format!(
                "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded{}",
                if crt_static { "" } else { "DLL" }
            )),
    );
    run("cmake build", Command::new("cmake").arg("--build").arg(&build_dir).arg("--target").arg("core"));

    build_dir
}

// ---------------------------------------------------------------------
// The shim.

/// JIT_ENABLED is a PUBLIC compile definition on CMake's `core` target,
/// so it has to be repeated by hand here: without it the shim sees an
/// NDS without its JIT members, allocates that smaller object, and the
/// core's constructor writes past the end of it. That corruption
/// presents as a SIGSEGV deep inside the ARMJIT constructor and looks
/// nothing like an ABI mismatch. `-fwrapv` likewise is PUBLIC on `core`.
fn compile_shim(
    manifest_dir: &Path,
    melonds_dir: &Path,
    jit: bool,
    clang: Option<&Clang>,
    crt_static: bool,
) {
    let mut shim = cc::Build::new();
    shim.cpp(true)
        .std("c++20")
        .files(SHIM_SOURCES.map(|src| manifest_dir.join("shim").join(src)))
        .include(melonds_dir.join("src"))
        .include(manifest_dir.join("shim"))
        .flag("-fwrapv");
    if jit {
        shim.define("JIT_ENABLED", None);
    }
    if let Some(clang) = clang {
        // cc would otherwise reach for `cl.exe` on this target. Pinning
        // the compiler also picks the GNU-driver flag syntax (cc reads
        // the tool family off the executable name), which is what the
        // flags above are written in. The archiver has to be pinned with
        // it: cc pairs a Clang-family compiler with `llvm-ar`, whose
        // GNU-format output link.exe cannot read, while `llvm-lib`
        // writes the COFF archive rustc expects to find as a `.lib`.
        shim.compiler(&clang.cxx)
            .archiver(&clang.lib)
            .flag(if crt_static { "-fms-runtime-lib=static" } else { "-fms-runtime-lib=dll" })
            // The core carries CodeView for the same reason, and this
            // is the one object cc compiles rather than CMake.
            .flag("-g");
    }
    // Emits the link-search + `static=melonds_shim` + C++ stdlib lines;
    // the shim archive must precede the core's for single-pass linkers,
    // so this comes first.
    shim.compile("melonds_shim");
}

fn emit_link_directives(build_dir: &Path, target_os: &str) {
    println!("cargo:rustc-link-search=native={}", build_dir.join("src").display());
    println!("cargo:rustc-link-search=native={}", build_dir.join("src/teakra/src").display());
    println!("cargo:rustc-link-lib=static=core");
    println!("cargo:rustc-link-lib=static=teakra");

    match target_os {
        // CMake attaches these to `core` PRIVATE, which for a static
        // library records them for a consumer that reads CMake's link
        // interface. rustc does not, so they are repeated here.
        // `onecore` is the Windows 8 memory-mapping set the JIT's
        // fastmem arena needs; `bcrypt` backs the RNG.
        "windows" => {
            for lib in ["onecore", "ole32", "comctl32", "ws2_32", "wsock32", "bcrypt"] {
                println!("cargo:rustc-link-lib=dylib={lib}");
            }
        }
        // The JIT's memory backend maps its fastmem arena with shm_open,
        // which glibc < 2.34 keeps in librt.
        "linux" if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("gnu") => {
            println!("cargo:rustc-link-lib=dylib=rt");
        }
        _ => {}
    }
}

// ---------------------------------------------------------------------
// Toolchain discovery.

/// The clang tools used for the Windows build: compiler drivers, the
/// lib.exe-compatible archiver, and the resource compiler.
struct Clang {
    cc: PathBuf,
    cxx: PathBuf,
    lib: PathBuf,
    rc: PathBuf,
}

/// LLVM's install root, from `LLVM_ROOT`, from `clang.exe` on PATH, or
/// from the location its Windows installer uses.
fn windows_clang() -> Clang {
    println!("cargo:rerun-if-env-changed=LLVM_ROOT");

    let bin = std::env::var("LLVM_ROOT")
        .map(|root| PathBuf::from(root).join("bin"))
        .ok()
        .or_else(|| {
            std::env::var_os("PATH")
                .iter()
                .flat_map(std::env::split_paths)
                .find(|dir| dir.join("clang.exe").exists())
        })
        .or_else(|| {
            let default = PathBuf::from("C:/Program Files/LLVM/bin");
            default.join("clang.exe").exists().then_some(default)
        })
        .expect(
            "clang not found. Install LLVM (winget install LLVM.LLVM), put it on PATH, \
             or set LLVM_ROOT. The melonDS JIT is GCC/Clang code that cl.exe cannot build.",
        );

    Clang {
        cc: bin.join("clang.exe"),
        cxx: bin.join("clang++.exe"),
        lib: bin.join("llvm-lib.exe"),
        rc: bin.join("llvm-rc.exe"),
    }
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
