//! Raw FFI to the melonDS core, generated from `shim/shim.h`.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(rustdoc::broken_intra_doc_links)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

/// Run the C++ side's static constructors. wasm32 only, and required
/// exactly once before any other symbol in this crate is touched.
///
/// Everywhere else the platform's loader runs `.init_array`; on
/// wasm32-unknown-unknown nothing does — wasm-ld's only mechanism is
/// WASI command-export wrappers, which re-run the constructors around
/// *every* export call and are deliberately kept out of the link (see
/// shim/wasi-shim.c on `exit`). So the embedder calls the linker's
/// synthesized initializer itself, through here. The safe wrapper's
/// vtable-install `Once` is the natural place.
#[cfg(target_arch = "wasm32")]
pub fn run_static_ctors() {
    extern "C" {
        fn __wasm_call_ctors();
    }
    unsafe { __wasm_call_ctors() };
}
