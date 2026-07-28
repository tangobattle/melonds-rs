# melonds-rs

Rust bindings to the [melonDS](https://github.com/melonDS-emu/melonDS) Nintendo
DS core, shaped for deterministic multi-instance embedding (tango's DS netplay
engine).

- `melonds-sys`: vendored melonDS core (patched: MSVC support, headless build)
  + the embedding shim (`shim/shim.cpp` replaces the Qt frontend's Platform
  implementation: in-memory everything, no wall clock, MP routed to the host)
  + bindgen FFI.
- `melonds`: the safe wrapper. Instances are values; the process-global
  `Host` trait owns save persistence and the wireless airwaves.

Pairing two consoles over emulated wireless and the rollback netcode on top
live in [melonds-rollback](https://github.com/tangobattle/melonds-rollback),
mirroring how `mgba-rollback` sits above `mgba-rs`.

License: GPL-3.0-or-later (the melonDS core's license governs the combined
work).
