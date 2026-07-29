//! Safe bindings to the melonDS core, shaped for tango-style embedding:
//! instances are independent values, all I/O is in-memory, and the
//! wireless airwaves are whatever the host's [`Mp`] implementation says
//! they are — which is what makes a deterministic in-process link (and
//! therefore rollback over it) possible at all.
//!
//! The core resolves its multiplayer and save callbacks through one
//! process-global table ([`install_host`]), with each callback receiving
//! the instance's `userdata`-like [`InstanceId`] so a host can tell its
//! cores apart.

use std::sync::OnceLock;

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

/// Which instance a host callback is about. This is the `token` passed
/// to [`Nds::new`] — the host's own routing handle, distinct from the
/// wireless identity: a host juggling several consoles over time (say,
/// a new link created while an old one winds down) needs callbacks it
/// can attribute to the right one, while the MAC-forming id has to stay
/// identical across every peer simulating the same pair.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub struct InstanceId(pub usize);

/// The host half of the platform: save persistence and the wireless
/// airwaves. One per process (melonDS's platform layer is link-time
/// global); install it before creating any [`Nds`].
///
/// MP semantics mirror melonDS `Platform::MP_*`: `timestamp` is the
/// sender's emulated wifi microsecond clock; receive methods fill `data`
/// (up to 2048 bytes) and return the packet length, `0` for nothing
/// available, or `None` for not-connected; `recv_replies` writes each
/// responding client's reply at `aid * 1024` into `data` and returns the
/// bitmask of aids that replied.
#[allow(unused_variables)]
pub trait Host: Send + Sync {
    fn log(&self, level: i32, msg: &str) {}
    fn write_save(&self, inst: InstanceId, data: &[u8], writeoffset: u32, writelen: u32) {}
    fn signal_stop(&self, inst: InstanceId, reason: i32) {}

    fn mp_begin(&self, inst: InstanceId) {}
    fn mp_end(&self, inst: InstanceId) {}
    fn mp_send_packet(&self, inst: InstanceId, data: &[u8], timestamp: u64) -> i32 {
        data.len() as i32
    }
    fn mp_recv_packet(&self, inst: InstanceId, data: &mut [u8], now: u64, timestamp: &mut u64) -> Option<i32> {
        Some(0)
    }
    fn mp_send_cmd(&self, inst: InstanceId, data: &[u8], timestamp: u64) -> i32 {
        data.len() as i32
    }
    fn mp_send_reply(&self, inst: InstanceId, data: &[u8], timestamp: u64, aid: u16) -> i32 {
        data.len() as i32
    }
    fn mp_send_ack(&self, inst: InstanceId, data: &[u8], timestamp: u64) -> i32 {
        data.len() as i32
    }
    fn mp_recv_host_packet(&self, inst: InstanceId, data: &mut [u8], now: u64, timestamp: &mut u64) -> Option<i32> {
        None
    }
    fn mp_recv_replies(&self, inst: InstanceId, data: &mut [u8], now: u64, timestamp: u64, aidmask: u16) -> u16 {
        0
    }

    /// The instance's wifi clock advanced through `now`; every MP frame
    /// it sends from here on is stamped strictly later. Receives also
    /// carry their own `now` — together these let a host gate frame
    /// delivery on emulated time alone, so the two consoles of a pair
    /// can run concurrently without losing determinism.
    fn mp_clock(&self, inst: InstanceId, now: u64) {}
}

static HOST: OnceLock<Box<dyn Host + Send + Sync>> = OnceLock::new();

/// Receive buffers are sized for the biggest frame the wifi hardware
/// moves (see melonDS `kMaxFrameSize` = 0x948); recv_replies packs up to
/// 16 aid slots of 1024 bytes.
const RECV_BUF: usize = 16 * 1024;

fn with_host<R>(f: impl FnOnce(&dyn Host) -> R) -> Option<R> {
    Some(f(&**HOST.get()?))
}

/// Install the process-global [`Host`]. May be called once; later calls
/// return the rejected host as an error.
pub fn install_host(host: Box<dyn Host + Send + Sync>) -> Result<(), Box<dyn Host + Send + Sync>> {
    let mut candidate = Some(host);
    HOST.get_or_init(|| candidate.take().unwrap());
    match candidate {
        None => {
            unsafe {
                melonds_sys::mds_set_host_vtable(&VTABLE);
            }
            Ok(())
        }
        Some(rejected) => Err(rejected),
    }
}

unsafe fn inst_of(userdata: *mut std::ffi::c_void) -> InstanceId {
    InstanceId(userdata as usize)
}

unsafe extern "C" fn host_log(level: i32, msg: *const std::ffi::c_char) {
    let msg = std::ffi::CStr::from_ptr(msg).to_string_lossy();
    with_host(|h| h.log(level, msg.trim_end()));
}

unsafe extern "C" fn host_write_save(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: u32,
    writeoffset: u32,
    writelen: u32,
) {
    let data = std::slice::from_raw_parts(data, len as usize);
    with_host(|h| h.write_save(inst_of(userdata), data, writeoffset, writelen));
}

unsafe extern "C" fn host_signal_stop(userdata: *mut std::ffi::c_void, reason: i32) {
    with_host(|h| h.signal_stop(inst_of(userdata), reason));
}

unsafe extern "C" fn host_mp_begin(userdata: *mut std::ffi::c_void) {
    with_host(|h| h.mp_begin(inst_of(userdata)));
}

unsafe extern "C" fn host_mp_end(userdata: *mut std::ffi::c_void) {
    with_host(|h| h.mp_end(inst_of(userdata)));
}

unsafe extern "C" fn host_mp_send_packet(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: i32,
    timestamp: u64,
) -> i32 {
    let data = std::slice::from_raw_parts(data, len as usize);
    with_host(|h| h.mp_send_packet(inst_of(userdata), data, timestamp)).unwrap_or(len)
}

unsafe extern "C" fn host_mp_recv_packet(
    userdata: *mut std::ffi::c_void,
    data: *mut u8,
    now: u64,
    timestamp: *mut u64,
) -> i32 {
    let data = std::slice::from_raw_parts_mut(data, RECV_BUF);
    let mut ts = 0u64;
    let r = with_host(|h| h.mp_recv_packet(inst_of(userdata), data, now, &mut ts))
        .flatten()
        .unwrap_or(0);
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
    with_host(|h| h.mp_send_cmd(inst_of(userdata), data, timestamp)).unwrap_or(len)
}

unsafe extern "C" fn host_mp_send_reply(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: i32,
    timestamp: u64,
    aid: u16,
) -> i32 {
    let data = std::slice::from_raw_parts(data, len as usize);
    with_host(|h| h.mp_send_reply(inst_of(userdata), data, timestamp, aid)).unwrap_or(len)
}

unsafe extern "C" fn host_mp_send_ack(
    userdata: *mut std::ffi::c_void,
    data: *const u8,
    len: i32,
    timestamp: u64,
) -> i32 {
    let data = std::slice::from_raw_parts(data, len as usize);
    with_host(|h| h.mp_send_ack(inst_of(userdata), data, timestamp)).unwrap_or(len)
}

unsafe extern "C" fn host_mp_recv_host_packet(
    userdata: *mut std::ffi::c_void,
    data: *mut u8,
    now: u64,
    timestamp: *mut u64,
) -> i32 {
    let data = std::slice::from_raw_parts_mut(data, RECV_BUF);
    let mut ts = 0u64;
    let r = with_host(|h| h.mp_recv_host_packet(inst_of(userdata), data, now, &mut ts))
        .flatten()
        .unwrap_or(-1);
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
    with_host(|h| h.mp_recv_replies(inst_of(userdata), data, now, timestamp, aidmask)).unwrap_or(0)
}

unsafe extern "C" fn host_mp_clock(userdata: *mut std::ffi::c_void, now: u64) {
    with_host(|h| h.mp_clock(inst_of(userdata), now));
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
    /// Kept alive for as long as the core holds a pointer to it; see
    /// [`Nds::set_traps`].
    traps: Option<Box<TrapTable>>,
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
        traps: None,
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
    /// simulation, so a linked pair uses 0 and 1 on every peer — while
    /// `token` is the value host callbacks carry as [`InstanceId`],
    /// free for the host to make process-unique.
    pub fn new(rom: &[u8], save: Option<&[u8]>, instance_id: u32, token: usize) -> Result<Self, Error> {
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
                token as *mut std::ffi::c_void,
            )
        };
        if ptr.is_null() {
            return Err(Error::BadRom);
        }
        Ok(Nds {
            ptr,
            state_buf_hint: 0,
            traps: None,
        })
    }

    /// Install execution traps: `handler` runs just before the ARM9
    /// executes any of `addrs`, and is told which address it stopped
    /// at. A handler may read and write memory and may
    /// [`jump`](Self::jump) to redirect execution, which is how a
    /// caller walks the game through its own code instead of pressing
    /// its buttons.
    ///
    /// Traps run the ARM9 interpreted — the JIT would run straight past
    /// them — so this is a tool for short scripted stretches like
    /// priming, not for a whole match. Passing an empty list removes
    /// them and hands the ARM9 back to the JIT.
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
        unsafe { melonds_sys::mds_nds_free(self.ptr) }
    }
}

