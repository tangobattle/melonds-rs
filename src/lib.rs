//! Safe bindings to the melonDS core, shaped for tango-style embedding:
//! instances are independent values, all I/O is in-memory, and the
//! wireless airwaves are whatever the host's [`Mp`] implementation says
//! they are — which is what makes a deterministic in-process link (and
//! therefore rollback over it) possible at all.
//!
//! Each instance carries its own [`Host`] — the callbacks for its save
//! writes and its wireless airwaves — handed over at [`Nds::new`] and
//! owned by the instance, the way its trap table is. The core's
//! platform layer is still link-time global underneath, but its
//! `userdata` pointer is per instance, so nothing above the FFI shim
//! is: the one process-global hook left is [`install_default_logger`],
//! because the core's log callback is the one that carries no instance.

/// Active-high key bits, DS KEYINPUT/KEYXY layout.
pub mod keys {
    pub const A: u32 = 1 << 0;
    pub const B: u32 = 1 << 1;
    pub const SELECT: u32 = 1 << 2;
    pub const START: u32 = 1 << 3;
    pub const RIGHT: u32 = 1 << 4;
    pub const LEFT: u32 = 1 << 5;
    pub const UP: u32 = 1 << 6;
    pub const DOWN: u32 = 1 << 7;
    pub const R: u32 = 1 << 8;
    pub const L: u32 = 1 << 9;
    pub const X: u32 = 1 << 10;
    pub const Y: u32 = 1 << 11;
}

/// Screen dimensions of one DS screen, in pixels.
pub const SCREEN_WIDTH: usize = 256;
pub const SCREEN_HEIGHT: usize = 192;

/// One instance's half of the platform: save persistence and the
/// wireless airwaves, owned by the [`Nds`] it answers for — handed over
/// at [`Nds::new`] and dropped with the instance, so there is no
/// registration to keep in sync and no callback that can outlive what
/// it routes to. The defaults are a console with nothing attached:
/// sends vanish (claiming success), receives report not-connected.
///
/// MP semantics mirror melonDS `Platform::MP_*`: `timestamp` is the
/// sender's emulated wifi microsecond clock; receive methods fill `data`
/// (up to 2048 bytes) and return the packet length, `0` for nothing
/// available, or `None` for not-connected; `recv_replies` writes each
/// responding client's reply at `aid * 1024` into `data` and returns the
/// bitmask of aids that replied.
#[allow(unused_variables)]
pub trait Host: Send {
    fn write_save(&self, data: &[u8], writeoffset: u32, writelen: u32) {}
    fn signal_stop(&self, reason: i32) {}

    fn mp_begin(&self) {}
    fn mp_end(&self) {}
    fn mp_send_packet(&self, data: &[u8], timestamp: u64) -> i32 {
        data.len() as i32
    }
    fn mp_recv_packet(&self, data: &mut [u8], now: u64, timestamp: &mut u64) -> Option<i32> {
        Some(0)
    }
    fn mp_send_cmd(&self, data: &[u8], timestamp: u64) -> i32 {
        data.len() as i32
    }
    fn mp_send_reply(&self, data: &[u8], timestamp: u64, aid: u16) -> i32 {
        data.len() as i32
    }
    fn mp_send_ack(&self, data: &[u8], timestamp: u64) -> i32 {
        data.len() as i32
    }
    fn mp_recv_host_packet(&self, data: &mut [u8], now: u64, timestamp: &mut u64) -> Option<i32> {
        None
    }
    fn mp_recv_replies(&self, data: &mut [u8], now: u64, timestamp: u64, aidmask: u16) -> u16 {
        0
    }

    /// The instance's wifi clock advanced through `now`; every MP frame
    /// it sends from here on is stamped strictly later. Receives also
    /// carry their own `now` — together these let a host gate frame
    /// delivery on emulated time alone, so the two consoles of a pair
    /// can run concurrently without losing determinism.
    fn mp_clock(&self, now: u64) {}
}

/// Receive buffers are sized for the biggest frame the wifi hardware
/// moves (see melonDS `kMaxFrameSize` = 0x948); recv_replies packs up to
/// 16 aid slots of 1024 bytes.
const RECV_BUF: usize = 16 * 1024;

static LOGGER_INSTALLED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Route the core's log lines through the `log` crate, under target
/// `melonds`. Process-global because the core's log callback is the one
/// platform hook that carries no instance — melonDS logs from before
/// any instance exists. Safe to call from any thread, and installing
/// repeatedly is fine. Uninstalled, log lines are dropped.
pub fn install_default_logger() {
    LOGGER_INSTALLED.store(true, std::sync::atomic::Ordering::Relaxed);
}

/// The instance's own [`Host`], back out of the `userdata` pointer the
/// core threads through every platform call. The pointee is the boxed
/// trait object [`Nds::new`] parked in the instance, alive for as long
/// as the instance is — and the core only calls while inside an `mds_*`
/// entry point on that instance.
unsafe fn host_of<'a>(userdata: *mut std::ffi::c_void) -> &'a dyn Host {
    &**(userdata as *const Box<dyn Host>)
}

unsafe extern "C" fn host_log(level: i32, msg: *const std::ffi::c_char) {
    if !LOGGER_INSTALLED.load(std::sync::atomic::Ordering::Relaxed) {
        return;
    }
    // melonDS Platform::LogLevel: Debug=0, Info=1, Warn=2, Error=3.
    let level = match level {
        0 => log::Level::Debug,
        1 => log::Level::Info,
        2 => log::Level::Warn,
        _ => log::Level::Error,
    };
    if !log::log_enabled!(level) {
        return;
    }
    let msg = std::ffi::CStr::from_ptr(msg).to_string_lossy();
    log::log!(level, "{}", msg.trim_end());
}

unsafe extern "C" fn host_write_save(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: u32,
    writeoffset: u32,
    writelen: u32,
) {
    let data = std::slice::from_raw_parts(data, len as usize);
    host_of(userdata).write_save(data, writeoffset, writelen);
}

unsafe extern "C" fn host_signal_stop(userdata: *mut std::ffi::c_void, reason: i32) {
    host_of(userdata).signal_stop(reason);
}

unsafe extern "C" fn host_mp_begin(userdata: *mut std::ffi::c_void) {
    host_of(userdata).mp_begin();
}

unsafe extern "C" fn host_mp_end(userdata: *mut std::ffi::c_void) {
    host_of(userdata).mp_end();
}

unsafe extern "C" fn host_mp_send_packet(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: i32,
    timestamp: u64,
) -> i32 {
    let data = std::slice::from_raw_parts(data, len as usize);
    host_of(userdata).mp_send_packet(data, timestamp)
}

unsafe extern "C" fn host_mp_recv_packet(
    userdata: *mut std::ffi::c_void,
    data: *mut u8,
    now: u64,
    timestamp: *mut u64,
) -> i32 {
    let data = std::slice::from_raw_parts_mut(data, RECV_BUF);
    let mut ts = 0u64;
    let r = host_of(userdata).mp_recv_packet(data, now, &mut ts).unwrap_or(0);
    if !timestamp.is_null() {
        *timestamp = ts;
    }
    r
}

unsafe extern "C" fn host_mp_send_cmd(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: i32,
    timestamp: u64,
) -> i32 {
    let data = std::slice::from_raw_parts(data, len as usize);
    host_of(userdata).mp_send_cmd(data, timestamp)
}

unsafe extern "C" fn host_mp_send_reply(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: i32,
    timestamp: u64,
    aid: u16,
) -> i32 {
    let data = std::slice::from_raw_parts(data, len as usize);
    host_of(userdata).mp_send_reply(data, timestamp, aid)
}

unsafe extern "C" fn host_mp_send_ack(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: i32,
    timestamp: u64,
) -> i32 {
    let data = std::slice::from_raw_parts(data, len as usize);
    host_of(userdata).mp_send_ack(data, timestamp)
}

unsafe extern "C" fn host_mp_recv_host_packet(
    userdata: *mut std::ffi::c_void,
    data: *mut u8,
    now: u64,
    timestamp: *mut u64,
) -> i32 {
    let data = std::slice::from_raw_parts_mut(data, RECV_BUF);
    let mut ts = 0u64;
    let r = host_of(userdata).mp_recv_host_packet(data, now, &mut ts).unwrap_or(-1);
    if !timestamp.is_null() {
        *timestamp = ts;
    }
    r
}

unsafe extern "C" fn host_mp_recv_replies(
    userdata: *mut std::ffi::c_void,
    data: *mut u8,
    now: u64,
    timestamp: u64,
    aidmask: u16,
) -> u16 {
    let data = std::slice::from_raw_parts_mut(data, RECV_BUF);
    host_of(userdata).mp_recv_replies(data, now, timestamp, aidmask)
}

unsafe extern "C" fn host_mp_clock(userdata: *mut std::ffi::c_void, now: u64) {
    host_of(userdata).mp_clock(now);
}

static VTABLE: melonds_sys::MdsHostVtable = melonds_sys::MdsHostVtable {
    log: Some(host_log),
    write_save: Some(host_write_save),
    signal_stop: Some(host_signal_stop),
    mp_begin: Some(host_mp_begin),
    mp_end: Some(host_mp_end),
    mp_send_packet: Some(host_mp_send_packet),
    mp_recv_packet: Some(host_mp_recv_packet),
    mp_send_cmd: Some(host_mp_send_cmd),
    mp_send_reply: Some(host_mp_send_reply),
    mp_send_ack: Some(host_mp_send_ack),
    mp_recv_host_packet: Some(host_mp_recv_host_packet),
    mp_recv_replies: Some(host_mp_recv_replies),
    mp_clock: Some(host_mp_clock),
};

/// One emulated DS.
pub struct Nds {
    ptr: *mut melonds_sys::MdsNds,
    state_buf_hint: usize,
    /// The instance's [`Host`], double-boxed so the core's `userdata`
    /// can be a thin pointer to it. Kept alive for as long as the core
    /// holds that pointer — the same lifetime contract as the trap
    /// tables. `None` only on the borrowed wrapper a trap handler gets.
    ///
    /// Underscored because it is never read: the core reaches the host
    /// through `userdata`, not through here, so this field exists only
    /// to own the allocation and to drop it after [`Drop`] has freed the
    /// instance.
    _host: Option<Box<Box<dyn Host>>>,
    /// Kept alive for as long as the core holds a pointer to it; see
    /// [`Nds::set_traps`].
    traps: Option<Box<TrapTable>>,
    /// The ARM7's table, same lifetime contract; see [`Nds::set_traps7`].
    traps7: Option<Box<TrapTable>>,
    /// The data-read watch tables, same lifetime contract; see
    /// [`Nds::set_watches`]. Watches dispatch through the trap
    /// trampoline — a handler keyed by address is the same job either
    /// way — so they carry the same table type.
    watches: Option<Box<TrapTable>>,
    watches7: Option<Box<TrapTable>>,
}

/// What a trap trampoline needs: the handlers, by address, and a way
/// back to the instance so a handler can be handed one.
struct TrapTable {
    handlers: std::collections::HashMap<u32, Box<dyn FnMut(&mut Nds)>>,
    ptr: *mut melonds_sys::MdsNds,
}

unsafe extern "C" fn trap_trampoline(userdata: *mut std::ffi::c_void, addr: u32) {
    let table = &mut *(userdata as *mut TrapTable);
    // The core's address filter is approximate and may offer an address
    // that was never registered, so an unknown one is simply not ours.
    let Some(handler) = table.handlers.get_mut(&addr) else {
        return;
    };
    // A borrow of the instance for the length of the handler. The core
    // is inside `run_frame` on this same instance, so this is the one
    // place an `&mut Nds` is manufactured rather than passed down; the
    // handler may do anything a caller could except run another frame.
    let mut nds = Nds {
        ptr: table.ptr,
        state_buf_hint: 0,
        _host: None,
        traps: None,
        traps7: None,
        watches: None,
        watches7: None,
    };
    handler(&mut nds);
    // The borrowed wrapper must not free the instance or disarm the
    // traps it was called from.
    std::mem::forget(nds);
}

// The core's only cross-instance state is a thread_local re-pinned on
// every entry, so moving an instance between threads is sound as long as
// calls aren't concurrent — which &mut enforces.
unsafe impl Send for Nds {}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("cart rejected (not a parseable NDS ROM)")]
    BadRom,
    #[error("savestate failed")]
    Savestate,
}

impl Nds {
    /// Boot a retail cart with FreeBIOS + generated firmware. The
    /// firmware MAC is uniquified by `instance_id` — part of the
    /// simulation, so a linked pair uses 0 and 1 on every peer. `host`
    /// receives this instance's callbacks and nothing else's; whatever
    /// tells one seat from another lives in it.
    pub fn new(rom: &[u8], save: Option<&[u8]>, instance_id: u32, host: Box<dyn Host>) -> Result<Self, Error> {
        // The vtable is static and identical for every instance; what
        // varies per instance rides in `userdata`.
        static VTABLE_INSTALLED: std::sync::Once = std::sync::Once::new();
        VTABLE_INSTALLED.call_once(|| unsafe {
            melonds_sys::mds_set_host_vtable(&VTABLE);
        });
        let host = Box::new(host);
        let (save_ptr, save_len) = match save {
            Some(s) => (s.as_ptr(), s.len() as u32),
            None => (std::ptr::null(), 0),
        };
        let ptr = unsafe {
            melonds_sys::mds_nds_new(
                rom.as_ptr(),
                rom.len() as u32,
                save_ptr,
                save_len,
                instance_id as i32,
                &*host as *const Box<dyn Host> as *mut std::ffi::c_void,
            )
        };
        if ptr.is_null() {
            return Err(Error::BadRom);
        }
        Ok(Nds {
            ptr,
            state_buf_hint: 0,
            _host: Some(host),
            traps: None,
            traps7: None,
            watches: None,
            watches7: None,
        })
    }

    /// Install execution traps: `handler` runs just before the ARM9
    /// executes any of `addrs`, and is told which address it stopped
    /// at. A handler may read and write memory and may
    /// [`jump`](Self::jump) to redirect execution, which is how a
    /// caller walks the game through its own code instead of pressing
    /// its buttons.
    ///
    /// Traps run under the JIT: a trapped address always starts its own
    /// compiled block, and the dispatcher runs the handler just before
    /// that block — the same "just before the instruction" the
    /// interpreter delivers, at full JIT speed for everything between.
    /// Changing the trap set re-forms the block cache, so installs are
    /// for scripted stretches, not per-frame toggling.
    ///
    /// Traps are host state: they are not written to savestates, so a
    /// [`load_state`](Self::load_state) keeps whatever is installed now.
    pub fn set_traps(&mut self, traps: Vec<(u32, Box<dyn FnMut(&mut Nds)>)>) {
        let addrs: Vec<u32> = traps.iter().map(|(addr, _)| *addr).collect();
        let table = Box::new(TrapTable {
            handlers: traps.into_iter().collect(),
            ptr: self.ptr,
        });
        unsafe {
            melonds_sys::mds_set_traps(
                self.ptr,
                addrs.as_ptr(),
                addrs.len() as u32,
                if addrs.is_empty() { None } else { Some(trap_trampoline) },
                &*table as *const TrapTable as *mut std::ffi::c_void,
            )
        };
        // Dropped only once the core is no longer pointing at it.
        self.traps = (!addrs.is_empty()).then_some(table);
    }

    /// [`set_traps`](Self::set_traps), but for the ARM7 — where the
    /// platform code the game leans on actually runs: the sound engine,
    /// the wireless stack, the cartridge backup server. A walk that has
    /// to answer one of those waits needs its feet on this side too.
    ///
    /// Same contracts throughout: handlers may read and write memory and
    /// may [`arm7_jump`](Self::arm7_jump); this CPU's trapped addresses
    /// become its own block boundaries, exactly as the ARM9's do.
    pub fn set_traps7(&mut self, traps: Vec<(u32, Box<dyn FnMut(&mut Nds)>)>) {
        let addrs: Vec<u32> = traps.iter().map(|(addr, _)| *addr).collect();
        let table = Box::new(TrapTable {
            handlers: traps.into_iter().collect(),
            ptr: self.ptr,
        });
        unsafe {
            melonds_sys::mds_set_traps7(
                self.ptr,
                addrs.as_ptr(),
                addrs.len() as u32,
                if addrs.is_empty() { None } else { Some(trap_trampoline) },
                &*table as *const TrapTable as *mut std::ffi::c_void,
            )
        };
        self.traps7 = (!addrs.is_empty()).then_some(table);
    }

    /// Install ARM9 data-read watches: `handler` runs when the CPU reads
    /// any of `addrs`, from inside the load and before the value reaches
    /// its register — so [`pc`](Self::pc) names the reading instruction
    /// and [`reg`](Self::reg) reads that instruction's registers.
    ///
    /// This answers the question a trap cannot. A trap finds code by its
    /// address, which is no help when you know a variable matters but
    /// not who reads it: if nothing branches on the value there is no
    /// coverage difference to diff, and if the access is computed there
    /// is no literal to search the binary for. A watch finds the reader
    /// by what it touches.
    ///
    /// **Observation only.** Do not [`jump`](Self::jump) from a watch
    /// handler: the load it interrupted still has to complete, and the
    /// instruction is mid-execution rather than about to start.
    ///
    /// Byte reads report their own address; wider ones report the
    /// aligned address the load goes to — so watch the word a field sits
    /// in, not only the field's own byte. As with the traps, the address
    /// the handler receives is authoritative: the core's filter is
    /// approximate and unregistered addresses are dropped here.
    ///
    /// **A watch takes the console off the JIT** for as long as one is
    /// installed, because compiled code reads memory without asking.
    /// That costs a large multiple of the emulator's speed and shifts
    /// emulated timing, so this is an instrument for scouting a single
    /// run — never something to leave installed in a session. Pass an
    /// empty vec to remove the watches and hand the console back.
    pub fn set_watches(&mut self, watches: Vec<(u32, Box<dyn FnMut(&mut Nds)>)>) {
        let addrs: Vec<u32> = watches.iter().map(|(addr, _)| *addr).collect();
        let table = Box::new(TrapTable {
            handlers: watches.into_iter().collect(),
            ptr: self.ptr,
        });
        unsafe {
            melonds_sys::mds_set_watches(
                self.ptr,
                addrs.as_ptr(),
                addrs.len() as u32,
                if addrs.is_empty() { None } else { Some(trap_trampoline) },
                &*table as *const TrapTable as *mut std::ffi::c_void,
            )
        };
        self.watches = (!addrs.is_empty()).then_some(table);
    }

    /// [`set_watches`](Self::set_watches), but for the ARM7 — same
    /// contracts, including that either processor's watches take the
    /// whole console off the JIT.
    pub fn set_watches7(&mut self, watches: Vec<(u32, Box<dyn FnMut(&mut Nds)>)>) {
        let addrs: Vec<u32> = watches.iter().map(|(addr, _)| *addr).collect();
        let table = Box::new(TrapTable {
            handlers: watches.into_iter().collect(),
            ptr: self.ptr,
        });
        unsafe {
            melonds_sys::mds_set_watches7(
                self.ptr,
                addrs.as_ptr(),
                addrs.len() as u32,
                if addrs.is_empty() { None } else { Some(trap_trampoline) },
                &*table as *const TrapTable as *mut std::ffi::c_void,
            )
        };
        self.watches7 = (!addrs.is_empty()).then_some(table);
    }

    /// One ARM9 general register, 0-15. Inside a trap this is how a
    /// handler reaches the object the trapped function was working on —
    /// reading `r4` to find a menu's state block, say, so the selection
    /// its confirm is about to read can be written first.
    ///
    /// Register 15 is the raw prefetch pointer, two instructions ahead;
    /// [`pc`](Self::pc) is the instruction's own address.
    pub fn reg(&mut self, i: u32) -> u32 {
        unsafe { melonds_sys::mds_arm9_reg(self.ptr, i) }
    }

    pub fn set_reg(&mut self, i: u32, val: u32) {
        unsafe { melonds_sys::mds_arm9_set_reg(self.ptr, i, val) }
    }

    /// Whether the ARM9 is executing Thumb, which is what decides bit 0
    /// of a [`jump`](Self::jump) target.
    pub fn thumb(&mut self) -> bool {
        unsafe { melonds_sys::mds_arm9_thumb(self.ptr) != 0 }
    }

    /// Redirect the ARM9. `addr` is an interworking address — bit 0 set
    /// means Thumb, exactly as `BX` reads it. Called from a trap
    /// handler, this replaces the trapped instruction with a jump.
    pub fn jump(&mut self, addr: u32) {
        unsafe { melonds_sys::mds_arm9_jump(self.ptr, addr) }
    }

    /// Redirect the ARM9 within the instruction set it is already
    /// running — the common case for a handler steering the game
    /// through its own code.
    pub fn jump_here(&mut self, addr: u32) {
        let thumb = self.thumb();
        self.jump(addr | u32::from(thumb));
    }

    /// One ARM7 general register, 0-15; the mirror of [`reg`](Self::reg).
    pub fn arm7_reg(&mut self, i: u32) -> u32 {
        unsafe { melonds_sys::mds_arm7_reg(self.ptr, i) }
    }

    pub fn arm7_set_reg(&mut self, i: u32, val: u32) {
        unsafe { melonds_sys::mds_arm7_set_reg(self.ptr, i, val) }
    }

    /// Whether the ARM7 is executing Thumb.
    pub fn arm7_thumb(&mut self) -> bool {
        unsafe { melonds_sys::mds_arm7_thumb(self.ptr) != 0 }
    }

    /// Redirect the ARM7; `addr` is an interworking address, as
    /// [`jump`](Self::jump) is for the ARM9.
    pub fn arm7_jump(&mut self, addr: u32) {
        unsafe { melonds_sys::mds_arm7_jump(self.ptr, addr) }
    }

    /// Redirect the ARM7 within its current instruction set.
    pub fn arm7_jump_here(&mut self, addr: u32) {
        let thumb = self.arm7_thumb();
        self.arm7_jump(addr | u32::from(thumb));
    }

    /// The address of the instruction the ARM7 is about to execute.
    pub fn arm7_pc(&mut self) -> u32 {
        unsafe { melonds_sys::mds_arm7_pc(self.ptr) }
    }

    /// Reads and writes through the ARM7's bus — the only way to see
    /// ARM7-private WRAM, which is where the platform code keeps its
    /// state.
    pub fn arm7_read32(&mut self, addr: u32) -> u32 {
        unsafe { melonds_sys::mds_arm7_read32(self.ptr, addr) }
    }

    pub fn arm7_read16(&mut self, addr: u32) -> u16 {
        unsafe { melonds_sys::mds_arm7_read16(self.ptr, addr) }
    }

    pub fn arm7_read8(&mut self, addr: u32) -> u8 {
        unsafe { melonds_sys::mds_arm7_read8(self.ptr, addr) }
    }

    pub fn arm7_write32(&mut self, addr: u32, val: u32) {
        unsafe { melonds_sys::mds_arm7_write32(self.ptr, addr, val) }
    }

    pub fn arm7_write16(&mut self, addr: u32, val: u16) {
        unsafe { melonds_sys::mds_arm7_write16(self.ptr, addr, val) }
    }

    pub fn arm7_write8(&mut self, addr: u32, val: u8) {
        unsafe { melonds_sys::mds_arm7_write8(self.ptr, addr, val) }
    }

    /// Pin the cart RTC to a fixed date/time. Call before [`boot`](Self::boot).
    pub fn set_rtc(&mut self, year: i32, month: i32, day: i32, hour: i32, minute: i32, second: i32) {
        unsafe { melonds_sys::mds_rtc_set(self.ptr, year, month, day, hour, minute, second) }
    }

    /// Reset and direct-boot into the cart.
    pub fn boot(&mut self) {
        unsafe { melonds_sys::mds_boot(self.ptr) }
    }

    /// Run one video frame. Returns emulated scanlines (0 = core stopped).
    pub fn run_frame(&mut self) -> u32 {
        unsafe { melonds_sys::mds_run_frame(self.ptr) }
    }

    /// Set the held keys (active-high, see [`keys`]).
    pub fn set_keys(&mut self, keys: u32) {
        unsafe { melonds_sys::mds_set_keys(self.ptr, keys) }
    }

    pub fn touch(&mut self, x: u16, y: u16) {
        unsafe { melonds_sys::mds_touch(self.ptr, x, y) }
    }

    pub fn release_screen(&mut self) {
        unsafe { melonds_sys::mds_release_screen(self.ptr) }
    }

    /// Hold full-scale white noise on the microphone.
    ///
    /// This build has no host mic behind melonDS's platform hooks, so
    /// static is the only mic input a console here can hear — it is what
    /// a cart that wants a breath or a shout gets. Like the keys it is
    /// an input, set per frame: the generator behind it is savestated,
    /// so a console rewound and re-run over the same inputs hears the
    /// same noise it did the first time.
    pub fn set_mic_static(&mut self, on: bool) {
        unsafe { melonds_sys::mds_set_mic_static(self.ptr, on as i32) }
    }

    /// Toggle framebuffer production. Off skips the 2D compositing for
    /// this console — for an instance nobody displays, or ticks whose
    /// output nobody will look at. Emulation (including display capture
    /// into VRAM) is bit-identical either way; only the framebuffer
    /// goes stale while off.
    pub fn set_render(&mut self, enabled: bool) {
        unsafe { melonds_sys::mds_set_render(self.ptr, enabled as i32) }
    }

    /// The current front framebuffers (top, bottom), BGRA8888,
    /// 256x192 each. `None` until a frame has rendered.
    pub fn framebuffers(&mut self) -> Option<(&[u32], &[u32])> {
        let mut top = std::ptr::null();
        let mut bottom = std::ptr::null();
        let ok = unsafe { melonds_sys::mds_framebuffers(self.ptr, &mut top, &mut bottom) };
        if ok == 0 {
            return None;
        }
        let n = SCREEN_WIDTH * SCREEN_HEIGHT;
        unsafe { Some((std::slice::from_raw_parts(top, n), std::slice::from_raw_parts(bottom, n))) }
    }

    /// Drain buffered audio into `out` (interleaved stereo i16).
    /// Returns sample *pairs* written.
    pub fn read_audio(&mut self, out: &mut [i16]) -> usize {
        let max_frames = (out.len() / 2) as i32;
        unsafe { melonds_sys::mds_audio_read(self.ptr, out.as_mut_ptr(), max_frames) as usize }
    }

    /// Sample frames the SPU is holding for the frontend — what
    /// [`read_audio`](Self::read_audio) would hand over right now.
    ///
    /// A frontend that can ask this can leave its audio backlog in the
    /// SPU and take only what it is about to play, rather than pulling
    /// the buffer dry and holding it itself.
    pub fn audio_queued(&mut self) -> usize {
        unsafe { melonds_sys::mds_audio_queued(self.ptr).max(0) as usize }
    }

    /// The 4 MB main-RAM aperture.
    pub fn main_ram(&mut self) -> &mut [u8] {
        let mut mask = 0u32;
        let ptr = unsafe { melonds_sys::mds_main_ram(self.ptr, &mut mask) };
        unsafe { std::slice::from_raw_parts_mut(ptr, mask as usize + 1) }
    }

    pub fn read32(&mut self, addr: u32) -> u32 {
        unsafe { melonds_sys::mds_arm9_read32(self.ptr, addr) }
    }

    pub fn read16(&mut self, addr: u32) -> u16 {
        unsafe { melonds_sys::mds_arm9_read16(self.ptr, addr) }
    }

    pub fn read8(&mut self, addr: u32) -> u8 {
        unsafe { melonds_sys::mds_arm9_read8(self.ptr, addr) }
    }

    pub fn write32(&mut self, addr: u32, val: u32) {
        unsafe { melonds_sys::mds_arm9_write32(self.ptr, addr, val) }
    }

    pub fn write16(&mut self, addr: u32, val: u16) {
        unsafe { melonds_sys::mds_arm9_write16(self.ptr, addr, val) }
    }

    pub fn write8(&mut self, addr: u32, val: u8) {
        unsafe { melonds_sys::mds_arm9_write8(self.ptr, addr, val) }
    }

    /// ARM9 program counter.
    pub fn pc(&mut self) -> u32 {
        unsafe { melonds_sys::mds_arm9_pc(self.ptr) }
    }

    /// Emulated system-clock cycles since boot (33.513982 MHz domain).
    pub fn sys_timestamp(&mut self) -> u64 {
        unsafe { melonds_sys::mds_sys_timestamp(self.ptr) }
    }

    /// Serialize the full instance state into `buf` (cleared first).
    /// Serialize the full instance state into `buf`.
    ///
    /// Rollback calls this every tick, so the buffer is grown rather
    /// than cleared and refilled: handing back the `Vec` from a previous
    /// save reuses its allocation and skips re-zeroing bytes the core is
    /// about to overwrite anyway.
    pub fn save_state(&mut self, buf: &mut Vec<u8>) -> Result<(), Error> {
        // The first save probes with a generous ceiling; after that the
        // measured size (plus slack) is the size, and a DS state runs
        // ~6 MB rather than the 20 MB the probe reserves.
        let mut cap = if self.state_buf_hint > 0 {
            self.state_buf_hint
        } else {
            20 << 20
        };
        loop {
            if buf.len() < cap {
                buf.resize(cap, 0);
            }
            let n = unsafe { melonds_sys::mds_state_save(self.ptr, buf.as_mut_ptr(), buf.len() as u32) };
            if n > 0 {
                buf.truncate(n as usize);
                self.state_buf_hint = n as usize + (64 << 10);
                return Ok(());
            }
            if cap >= 256 << 20 {
                return Err(Error::Savestate);
            }
            cap *= 2;
        }
    }

    pub fn load_state(&mut self, buf: &[u8]) -> Result<(), Error> {
        let ok = unsafe { melonds_sys::mds_state_load(self.ptr, buf.as_ptr(), buf.len() as u32) };
        if ok == 1 {
            Ok(())
        } else {
            Err(Error::Savestate)
        }
    }

    /// Snapshot of the cart's save memory.
    pub fn save_memory(&mut self) -> Vec<u8> {
        let len = unsafe { melonds_sys::mds_save_read(self.ptr, std::ptr::null_mut(), 0) };
        if len == 0 {
            return Vec::new();
        }
        let mut out = vec![0u8; len as usize];
        unsafe { melonds_sys::mds_save_read(self.ptr, out.as_mut_ptr(), len) };
        out
    }
}

impl Drop for Nds {
    fn drop(&mut self) {
        // The host box (a field) drops after this body, so the core is
        // gone before the pointer it held goes stale.
        unsafe { melonds_sys::mds_nds_free(self.ptr) }
    }
}

