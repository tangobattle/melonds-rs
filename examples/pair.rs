//! Two instances in one process on a deterministic airwaves coordinator —
//! the seed of the DS link engine (mgba-rollback's `Link` analogue).
//!
//! Each instance runs its frames on its own thread; the coordinator
//! gates every wireless receive on *emulated* progress only (the peer
//! has simulated past the request's wifi timestamp, is parked at a later
//! one, or has finished the frame), never on the wall clock, so the
//! whole pair is a pure function of (roms, saves, rtc, key script) and
//! can be snapshot/restored as a unit.
//!
//!     cargo run --release --example pair -- <rom.nds> <frames> [save.bin]
//!
//! Runs the same boot-to-title key script on both instances, snapshots
//! the pair mid-run, replays the tail twice, and compares digests.

use std::collections::VecDeque;
use std::sync::{Condvar, Mutex};

use sha2::Digest;

const WIFI_REPLY_WINDOW_US: u64 = 2000;

#[derive(Clone, Debug, PartialEq)]
enum Mp {
    Packet { ts: u64, data: Vec<u8> },
    Cmd { ts: u64, data: Vec<u8> },
    Reply { ts: u64, aid: u16, data: Vec<u8> },
    Ack { ts: u64, data: Vec<u8> },
}

#[derive(Default)]
struct Seat {
    /// Regular + host->client traffic queued for this seat to receive.
    incoming: VecDeque<Mp>,
    /// Replies queued for this seat (it being the cmd sender).
    replies: VecDeque<Mp>,
    /// Newest wifi timestamp this seat has been seen at (any MP call).
    progress: u64,
    /// Frames this seat has fully finished.
    frames_done: u64,
    /// If parked in a receive, the timestamp it waits at.
    parked_at: Option<u64>,
    /// In MP mode (between mp_begin/mp_end).
    attached: bool,
}

#[derive(Default)]
struct AirwavesState {
    seats: [Seat; 2],
}

/// The deterministic airwaves. All decisions are functions of seat
/// progress in emulated time; the condvar only mediates who runs when.
struct Airwaves {
    state: Mutex<AirwavesState>,
    cv: Condvar,
}

impl Airwaves {
    fn new() -> Self {
        Airwaves {
            state: Mutex::new(AirwavesState::default()),
            cv: Condvar::new(),
        }
    }

    fn seat_of(inst: melonds::InstanceId) -> usize {
        inst.0 as usize
    }

    fn note_progress(&self, me: usize, ts: u64) {
        let mut st = self.state.lock().unwrap();
        if ts > st.seats[me].progress {
            st.seats[me].progress = ts;
        }
        self.cv.notify_all();
    }

    fn send(&self, me: usize, msg: Mp) {
        let mut st = self.state.lock().unwrap();
        let ts = match &msg {
            Mp::Packet { ts, .. } | Mp::Cmd { ts, .. } | Mp::Reply { ts, .. } | Mp::Ack { ts, .. } => *ts,
        };
        if ts > st.seats[me].progress {
            st.seats[me].progress = ts;
        }
        let peer = 1 - me;
        match msg {
            Mp::Reply { .. } => st.seats[peer].replies.push_back(msg),
            _ => st.seats[peer].incoming.push_back(msg),
        }
        self.cv.notify_all();
    }

    /// Wait until the peer can no longer produce traffic for emulated
    /// time <= `ts` this frame: it simulated past `ts`, parked at a
    /// later timestamp, detached, or finished the frame. Returns with
    /// the state lock held.
    fn wait_peer_past<'a>(&'a self, me: usize, ts: u64) -> std::sync::MutexGuard<'a, AirwavesState> {
        let mut st = self.state.lock().unwrap();
        st.seats[me].parked_at = Some(ts);
        if ts > st.seats[me].progress {
            st.seats[me].progress = ts;
        }
        self.cv.notify_all();
        loop {
            let peer = &st.seats[1 - me];
            let my_frame = st.seats[me].frames_done;
            let peer_done_frame = peer.frames_done > my_frame;
            let peer_past = peer.progress > ts;
            // Tiebreak equal park timestamps by seat index so exactly
            // one side yields, deterministically.
            let peer_parked_later = peer
                .parked_at
                .map(|p| p > ts || (p == ts && me == 0))
                .unwrap_or(false);
            if peer_past || peer_parked_later || peer_done_frame || !peer.attached {
                st.seats[me].parked_at = None;
                self.cv.notify_all();
                return st;
            }
            st = self.cv.wait(st).unwrap();
        }
    }
}

struct AirwavesHost(&'static Airwaves);

impl melonds::Host for AirwavesHost {
    fn log(&mut self, _level: i32, _msg: &str) {}

    fn mp_begin(&mut self, inst: melonds::InstanceId) {
        let mut st = self.0.state.lock().unwrap();
        st.seats[Airwaves::seat_of(inst)].attached = true;
        self.0.cv.notify_all();
    }

    fn mp_end(&mut self, inst: melonds::InstanceId) {
        let mut st = self.0.state.lock().unwrap();
        st.seats[Airwaves::seat_of(inst)].attached = false;
        self.0.cv.notify_all();
    }

    fn mp_send_packet(&mut self, inst: melonds::InstanceId, data: &[u8], ts: u64) -> i32 {
        self.0.send(Airwaves::seat_of(inst), Mp::Packet { ts, data: data.to_vec() });
        data.len() as i32
    }

    fn mp_send_cmd(&mut self, inst: melonds::InstanceId, data: &[u8], ts: u64) -> i32 {
        self.0.send(Airwaves::seat_of(inst), Mp::Cmd { ts, data: data.to_vec() });
        data.len() as i32
    }

    fn mp_send_reply(&mut self, inst: melonds::InstanceId, data: &[u8], ts: u64, aid: u16) -> i32 {
        self.0.send(Airwaves::seat_of(inst), Mp::Reply { ts, aid, data: data.to_vec() });
        data.len() as i32
    }

    fn mp_send_ack(&mut self, inst: melonds::InstanceId, data: &[u8], ts: u64) -> i32 {
        self.0.send(Airwaves::seat_of(inst), Mp::Ack { ts, data: data.to_vec() });
        data.len() as i32
    }

    fn mp_recv_packet(&mut self, inst: melonds::InstanceId, data: &mut [u8], ts_out: &mut u64) -> Option<i32> {
        // Non-blocking by contract (mirrors LocalMP): whatever regular
        // traffic is already queued, or nothing.
        let me = Airwaves::seat_of(inst);
        let mut st = self.0.state.lock().unwrap();
        while let Some(msg) = st.seats[me].incoming.pop_front() {
            if let Mp::Packet { ts, data: d } = msg {
                data[..d.len()].copy_from_slice(&d);
                *ts_out = ts;
                return Some(d.len() as i32);
            }
        }
        Some(0)
    }

    fn mp_recv_host_packet(&mut self, inst: melonds::InstanceId, data: &mut [u8], ts_out: &mut u64) -> Option<i32> {
        let me = Airwaves::seat_of(inst);
        {
            let mut st = self.0.state.lock().unwrap();
            if let Some(msg) = st.seats[me].incoming.pop_front() {
                return Some(deliver(msg, data, ts_out));
            }
        }
        let my_ts = self.0.state.lock().unwrap().seats[me].progress;
        let mut st = self.0.wait_peer_past(me, my_ts);
        match st.seats[me].incoming.pop_front() {
            Some(msg) => Some(deliver(msg, data, ts_out)),
            None => None, // nothing on the air for us
        }
    }

    fn mp_recv_replies(&mut self, inst: melonds::InstanceId, data: &mut [u8], ts: u64, aidmask: u16) -> u16 {
        let me = Airwaves::seat_of(inst);
        let mut st = self.0.wait_peer_past(me, ts + WIFI_REPLY_WINDOW_US);
        let mut mask = 0u16;
        // Replies for an older cmd are stale; melonDS's LocalMP uses a
        // 32us horizon, mirrored here.
        while let Some(msg) = st.seats[me].replies.pop_front() {
            if let Mp::Reply { ts: rts, aid, data: d } = msg {
                if rts + 32 < ts {
                    continue;
                }
                let off = aid as usize * 1024;
                data[off..off + d.len()].copy_from_slice(&d);
                mask |= 1 << aid;
                if mask & aidmask == aidmask {
                    break;
                }
            }
        }
        mask
    }
}

fn deliver(msg: Mp, data: &mut [u8], ts_out: &mut u64) -> i32 {
    let (ts, d) = match msg {
        Mp::Packet { ts, data } | Mp::Cmd { ts, data } | Mp::Ack { ts, data } => (ts, data),
        Mp::Reply { ts, data, .. } => (ts, data),
    };
    data[..d.len()].copy_from_slice(&d);
    *ts_out = ts;
    d.len() as i32
}

/// One frame of both instances, in lockstep. The scoped threads park in
/// the airwaves whenever the wifi protocol needs the peer.
fn tick(pair: &mut [melonds::Nds; 2], air: &Airwaves, keys: [u32; 2]) {
    std::thread::scope(|s| {
        for (i, nds) in pair.iter_mut().enumerate() {
            s.spawn(move || {
                nds.set_keys(keys[i]);
                nds.run_frame();
                let mut st = air.state.lock().unwrap();
                st.seats[i].frames_done += 1;
                air.cv.notify_all();
            });
        }
    });
}

fn digest(pair: &mut [melonds::Nds; 2]) -> String {
    let mut hasher = sha2::Sha256::new();
    for nds in pair {
        let ram = nds.main_ram();
        hasher.update(&*ram);
    }
    hex(&hasher.finalize()[..8])
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn boot_script(frame: u32) -> u32 {
    // Idle through the intro, then START x3 with settling gaps: title
    // screen, then the save-select screen.
    match frame {
        240..=249 | 380..=389 | 510..=519 => melonds::keys::START,
        _ => 0,
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let rom_path = args.next().expect("usage: pair <rom.nds> [frames] [save.bin]");
    let frames: u32 = args.next().map(|s| s.parse().unwrap()).unwrap_or(700);
    let save = args.next().map(|p| std::fs::read(p).expect("failed to read save"));

    let air: &'static Airwaves = Box::leak(Box::new(Airwaves::new()));
    melonds::install_host(Box::new(AirwavesHost(air))).ok().expect("host installed twice");

    let rom = std::fs::read(&rom_path).expect("failed to read rom");
    let mut pair = [
        melonds::Nds::new(&rom, save.as_deref(), 0).expect("cart rejected"),
        melonds::Nds::new(&rom, save.as_deref(), 1).expect("cart rejected"),
    ];
    for nds in &mut pair {
        nds.set_rtc(2026, 1, 1, 0, 0, 0);
        nds.boot();
    }

    let start = std::time::Instant::now();
    for frame in 0..frames {
        let k = boot_script(frame);
        tick(&mut pair, air, [k, k]);
    }
    let elapsed = start.elapsed();
    println!(
        "pair: {} frames in {:.2?} = {:.1} fps ({:.2}x realtime), digest {}",
        frames,
        elapsed,
        frames as f64 / elapsed.as_secs_f64(),
        frames as f64 / elapsed.as_secs_f64() / 59.8261,
        digest(&mut pair),
    );

    // Snapshot the pair (both instances; the airwaves queues are only
    // non-empty mid-connection — assert quiescence for now, the real
    // link serializes them too).
    {
        let st = air.state.lock().unwrap();
        for seat in &st.seats {
            assert!(seat.incoming.is_empty() && seat.replies.is_empty(), "airwaves not quiescent at snapshot");
        }
    }
    let mut states = [Vec::new(), Vec::new()];
    let t = std::time::Instant::now();
    for (nds, buf) in pair.iter_mut().zip(states.iter_mut()) {
        nds.save_state(buf).expect("save_state");
    }
    println!("pair snapshot: {} MiB in {:.2?}", (states[0].len() + states[1].len()) >> 20, t.elapsed());

    for frame in frames..frames + 120 {
        let k = boot_script(frame);
        tick(&mut pair, air, [k, k]);
    }
    let first = digest(&mut pair);

    let t = std::time::Instant::now();
    for (nds, buf) in pair.iter_mut().zip(states.iter()) {
        nds.load_state(buf).expect("load_state");
    }
    println!("pair restore: {:.2?}", t.elapsed());
    for frame in frames..frames + 120 {
        let k = boot_script(frame);
        tick(&mut pair, air, [k, k]);
    }
    let second = digest(&mut pair);

    println!(
        "pair replay determinism: {} vs {} -> {}",
        first,
        second,
        if first == second { "OK" } else { "MISMATCH" }
    );
}
