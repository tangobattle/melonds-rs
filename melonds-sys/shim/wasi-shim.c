// Syscall shims for the wasm32-unknown-unknown build.
//
// The core's libc surface is almost entirely pure compute (string/
// memory, snprintf, math, dlmalloc-on-memory.grow), which wasi-libc
// provides without any imports. The handful of syscall-backed symbols
// below are defined here instead, so the wasi-libc objects that would
// import wasi_snapshot_preview1 are never pulled into the link and the
// final module needs no WASI runtime at all. Same story as mgba-sys's
// shim.c, with one addition for the *threads* sysroot this crate
// builds against: __wasi_thread_spawn (see the bottom).
//
// _REENTRANT so wasi/api.h declares __wasi_thread_spawn — it is the
// threads sysroot's marker for "compiling with -pthread", which the
// build script passes anyway; saying it here keeps the header honest
// even if a future refactor drops the flag from the C files.
#ifndef _REENTRANT
#define _REENTRANT 1
#endif

#include <stddef.h>
#include <time.h>
#include <wasi/api.h>

// Every definition in this file (and in the generated wasi-stubs.c) is
// weak: an app can link more than one emulator core, and mgba-sys's
// wasm shim defines much of the same syscall floor. Weak here means
// whichever crate's floor loads first satisfies the link and a
// stronger, behavior-bearing definition (mgba's real clock_gettime,
// say) always wins without an order-dependent duplicate-symbol error.
#define WEAK __attribute__((weak))

// No wall clock: the console's own clock is host state, handed in at
// boot and pinned (determinism is the whole product). The only code
// that could reach these is libc++'s condvar-timeout machinery under
// the optional threaded renderer, which this build keeps off. A zero
// answer is a well-defined "clock stands still" — not entropy for the
// simulation to drink.
WEAK int clock_gettime(clockid_t clock, struct timespec *ts) {
    (void)clock;
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 0;
    }
    return 0;
}

WEAK time_t time(time_t *out) {
    if (out) {
        *out = 0;
    }
    return 0;
}

// Trap instead of proc_exit.
WEAK _Noreturn void abort(void) {
    __builtin_trap();
}

// wasi-libc routes all stdio through these three FILE backends; stdout/
// stderr's FILE globals reference them. Swallowing writes (and stubbing
// seek/close) makes printf-family calls no-ops without fd_write/fd_seek/
// fd_close imports. The core's own logging never comes this way — it
// goes through the host vtable's log callback.
WEAK size_t __stdio_write(void *f, const unsigned char *buf, size_t len) {
    (void)f;
    (void)buf;
    return len;
}

WEAK long long __stdio_seek(void *f, long long off, int whence) {
    (void)f;
    (void)off;
    (void)whence;
    return -1;
}

WEAK int __stdio_close(void *f) {
    (void)f;
    return 0;
}

// Zero-fill "entropy": nothing in the emulator core wants randomness
// for anything security-relevant, and a deterministic core shouldn't
// get any.
WEAK int getentropy(void *buffer, size_t len) {
    unsigned char *p = buffer;
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
    return 0;
}

// The syscall floor: wasi-libc's implementations of the __wasi_*
// functions are thin wrappers over wasi_snapshot_preview1 imports, all
// living in ONE archive member (__wasilibc_real.o) — so referencing any
// single one pulls every import along. This file + wasi-stubs.c
// (generated, see tools/gen-wasi-stubs.py) together define the complete
// set, keeping that object out of the link entirely. The
// behavior-bearing ones live here; the pure "not supported" remainder
// is generated.

WEAK __wasi_errno_t __wasi_environ_get(uint8_t **environ_ptrs, uint8_t *environ_buf) {
    (void)environ_ptrs;
    (void)environ_buf;
    return 0;
}

WEAK __wasi_errno_t __wasi_environ_sizes_get(__wasi_size_t *count, __wasi_size_t *buf_size) {
    *count = 0;
    *buf_size = 0;
    return 0;
}

// BADF is the "no more preopens" sentinel that ends libc's preopen scan.
WEAK __wasi_errno_t __wasi_fd_prestat_get(__wasi_fd_t fd, __wasi_prestat_t *prestat) {
    (void)fd;
    (void)prestat;
    return __WASI_ERRNO_BADF;
}

WEAK __wasi_errno_t __wasi_random_get(uint8_t *buf, __wasi_size_t buf_len) {
    for (__wasi_size_t i = 0; i < buf_len; i++) {
        buf[i] = 0;
    }
    return 0;
}

WEAK __wasi_errno_t __wasi_sched_yield(void) {
    return 0;
}

WEAK _Noreturn void __wasi_proc_exit(__wasi_exitcode_t code) {
    (void)code;
    __builtin_trap();
}

// Process exit, defined here so wasi-libc's own exit.o is never
// loaded. That object is not just dead weight: it references
// __wasm_call_dtors, and the moment that symbol resolves, wasm-ld
// wraps EVERY export in a `ctors; f; dtors` command wrapper — WASI
// command semantics, where each export call is a fresh program run.
// For a live emulator instance that re-runs every C++ static
// initializer on every call, which is state loss, not hygiene. With
// exit unreachable and no dtors symbol in the link, no wrappers are
// generated; static ctors instead run exactly once, from the Rust
// side's run_static_ctors (see src/lib.rs).
WEAK _Noreturn void exit(int code) {
    (void)code;
    __builtin_trap();
}

WEAK _Noreturn void _Exit(int code) {
    (void)code;
    __builtin_trap();
}

// Static-destructor registration, refused: the module lives exactly as
// long as its page, so nothing ever runs destructors, and letting
// libc++abi register them would pull wasi-libc's atexit machinery in
// just to walk a list at a time that never comes.
WEAK int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
    (void)func;
    (void)arg;
    (void)dso;
    return 0;
}

WEAK int atexit(void (*func)(void)) {
    (void)func;
    return 0;
}

// C++ exception machinery, referenced by throw-sites in libc++ and the
// core's error paths even though nothing can catch on this target
// (wasi-sdk builds libc++ without exceptions, so lld would otherwise
// leave these as dangling `env` imports for the embedder to satisfy).
// A throw is a trap, exactly what -fno-exceptions would have compiled.
WEAK void *__cxa_allocate_exception(size_t size) {
    (void)size;
    __builtin_trap();
}

WEAK _Noreturn void __cxa_throw(void *exception, void *tinfo, void (*dest)(void *)) {
    (void)exception;
    (void)tinfo;
    (void)dest;
    __builtin_trap();
}

// The threads sysroot's one extra import: pthread_create asks the host
// to instantiate the module again on a new thread. C++-level threads in
// this build exist only for the optional threaded soft renderer, which
// stays off — the consoles of a linked pair are RUST threads, spawned
// by the embedder, and never come through here. Refusing keeps the
// contract visible: if something does ask, pthread_create fails cleanly
// with EAGAIN instead of the module growing a wasi import the browser
// can't satisfy.
WEAK int32_t __wasi_thread_spawn(void *start_arg) {
    (void)start_arg;
    return -1;
}
