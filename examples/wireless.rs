//! The decisive experiment: two BN5DTDS instances walking to the
//! NetBattle wireless lobby on the deterministic airwaves, one
//! designating itself host, the other scanning — do they see each other?
//!
//!     cargo run --release --example wireless -- <rom.nds> <save.sav> <script0> <script1> <outdir>
//!
//! Scripts use the explore example's step grammar: `<frames>x<keys>`,
//! `<frames>xT<x>:<y>` (stylus hold), `@tag` dumps both instances'
//! screens at that step boundary.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Condvar, Mutex};

const WIFI_REPLY_WINDOW_US: u64 = 2000;

#[derive(Clone, Debug)]
enum Mp {
    Packet { ts: u64, data: Vec<u8> },
    Cmd { ts: u64, data: Vec<u8> },
    Reply { ts: u64, aid: u16, data: Vec<u8> },
    Ack { ts: u64, data: Vec<u8> },
}

#[derive(Default)]
struct Seat {
    incoming: VecDeque<Mp>,
    replies: VecDeque<Mp>,
    progress: u64,
    frames_done: u64,
    parked_at: Option<u64>,
    attached: bool,
}

#[derive(Default)]
struct AirwavesState {
    seats: [Seat; 2],
}

struct Airwaves {
    state: Mutex<AirwavesState>,
    cv: Condvar,
    // Traffic counters, for the experiment's verdict.
    sent: [AtomicU64; 2],
    received: [AtomicU64; 2],
}

impl Airwaves {
    fn new() -> Self {
        Airwaves {
            state: Mutex::new(AirwavesState::default()),
            cv: Condvar::new(),
            sent: [AtomicU64::new(0), AtomicU64::new(0)],
            received: [AtomicU64::new(0), AtomicU64::new(0)],
        }
    }

    fn seat_of(inst: melonds::InstanceId) -> usize {
        inst.0 as usize
    }

    fn send(&self, me: usize, msg: Mp) {
        self.sent[me].fetch_add(1, Ordering::Relaxed);
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

    /// Park until the peer can no longer produce traffic for emulated
    /// time <= `ts` this frame. Purely emulated-time decisions.
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
            let ok = peer.progress > ts
                || peer.frames_done > my_frame
                || !peer.attached
                || peer.parked_at.map(|p| p > ts || (p == ts && me == 0)).unwrap_or(false);
            if ok {
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
    fn log(&self, _level: i32, _msg: &str) {}

    fn mp_begin(&self, inst: melonds::InstanceId) {
        println!("[mp] instance {} attached to the airwaves", inst.0);
        let mut st = self.0.state.lock().unwrap();
        st.seats[Airwaves::seat_of(inst)].attached = true;
        self.0.cv.notify_all();
    }

    fn mp_end(&self, inst: melonds::InstanceId) {
        println!("[mp] instance {} detached", inst.0);
        let mut st = self.0.state.lock().unwrap();
        st.seats[Airwaves::seat_of(inst)].attached = false;
        self.0.cv.notify_all();
    }

    fn mp_send_packet(&self, inst: melonds::InstanceId, data: &[u8], ts: u64) -> i32 {
        self.0.send(Airwaves::seat_of(inst), Mp::Packet { ts, data: data.to_vec() });
        data.len() as i32
    }

    fn mp_send_cmd(&self, inst: melonds::InstanceId, data: &[u8], ts: u64) -> i32 {
        self.0.send(Airwaves::seat_of(inst), Mp::Cmd { ts, data: data.to_vec() });
        data.len() as i32
    }

    fn mp_send_reply(&self, inst: melonds::InstanceId, data: &[u8], ts: u64, aid: u16) -> i32 {
        self.0.send(Airwaves::seat_of(inst), Mp::Reply { ts, aid, data: data.to_vec() });
        data.len() as i32
    }

    fn mp_send_ack(&self, inst: melonds::InstanceId, data: &[u8], ts: u64) -> i32 {
        self.0.send(Airwaves::seat_of(inst), Mp::Ack { ts, data: data.to_vec() });
        data.len() as i32
    }

    fn mp_recv_packet(&self, inst: melonds::InstanceId, data: &mut [u8], ts_out: &mut u64) -> Option<i32> {
        // Non-blocking, and type-agnostic: melonDS's LocalMP keeps
        // regular/cmd/ack frames in ONE queue and pops whatever is at
        // the head (the wifi code filters by frame contents). Popping
        // only regular frames head-blocks the queue behind the first
        // cmd and the peer never hears a beacon again.
        let me = Airwaves::seat_of(inst);
        let mut st = self.0.state.lock().unwrap();
        match st.seats[me].incoming.pop_front() {
            Some(msg) => {
                self.0.received[me].fetch_add(1, Ordering::Relaxed);
                Some(deliver(msg, data, ts_out))
            }
            None => Some(0),
        }
    }

    fn mp_recv_host_packet(&self, inst: melonds::InstanceId, data: &mut [u8], ts_out: &mut u64) -> Option<i32> {
        let me = Airwaves::seat_of(inst);
        {
            let mut st = self.0.state.lock().unwrap();
            if let Some(msg) = st.seats[me].incoming.pop_front() {
                self.0.received[me].fetch_add(1, Ordering::Relaxed);
                return Some(deliver(msg, data, ts_out));
            }
        }
        let my_ts = self.0.state.lock().unwrap().seats[me].progress;
        let mut st = self.0.wait_peer_past(me, my_ts);
        match st.seats[me].incoming.pop_front() {
            Some(msg) => {
                self.0.received[me].fetch_add(1, Ordering::Relaxed);
                Some(deliver(msg, data, ts_out))
            }
            None => None,
        }
    }

    fn mp_recv_replies(&self, inst: melonds::InstanceId, data: &mut [u8], ts: u64, aidmask: u16) -> u16 {
        let me = Airwaves::seat_of(inst);
        let mut st = self.0.wait_peer_past(me, ts + WIFI_REPLY_WINDOW_US);
        let mut mask = 0u16;
        while let Some(msg) = st.seats[me].replies.pop_front() {
            if let Mp::Reply { ts: rts, aid, data: d } = msg {
                if rts + 32 < ts {
                    continue;
                }
                let off = aid as usize * 1024;
                data[off..off + d.len()].copy_from_slice(&d);
                mask |= 1 << aid;
                self.0.received[me].fetch_add(1, Ordering::Relaxed);
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

// --- script parsing (explore's grammar) ---------------------------------

#[derive(Clone, Copy, Default)]
struct Frame {
    keys: u32,
    touch: Option<(u16, u16)>,
}

fn parse_keys(s: &str) -> u32 {
    s.split('+')
        .filter(|k| !k.is_empty())
        .map(|k| match k {
            "A" => melonds::keys::A,
            "B" => melonds::keys::B,
            "X" => melonds::keys::X,
            "Y" => melonds::keys::Y,
            "L" => melonds::keys::L,
            "R" => melonds::keys::R,
            "START" => melonds::keys::START,
            "SELECT" => melonds::keys::SELECT,
            "UP" => melonds::keys::UP,
            "DOWN" => melonds::keys::DOWN,
            "LEFT" => melonds::keys::LEFT,
            "RIGHT" => melonds::keys::RIGHT,
            other => panic!("unknown key {other:?}"),
        })
        .fold(0, |a, b| a | b)
}

fn parse_script(script: &str) -> (Vec<Frame>, Vec<(usize, String)>) {
    let mut frames = Vec::new();
    let mut tags = Vec::new();
    for step in script.split(',') {
        let (count, rest) = step.split_once('x').expect("step must be <frames>x<keys>");
        let count: u32 = count.parse().expect("bad frame count");
        let (keys_str, tag) = match rest.split_once('@') {
            Some((k, t)) => (k, Some(t)),
            None => (rest, None),
        };
        let frame = if let Some(xy) = keys_str.strip_prefix('T') {
            let (x, y) = xy.split_once(':').expect("touch step must be T<x>:<y>");
            Frame {
                keys: 0,
                touch: Some((x.parse().unwrap(), y.parse().unwrap())),
            }
        } else {
            Frame {
                keys: parse_keys(keys_str),
                touch: None,
            }
        };
        frames.extend(std::iter::repeat(frame).take(count as usize));
        if let Some(tag) = tag {
            tags.push((frames.len() - 1, tag.to_owned()));
        }
    }
    (frames, tags)
}

fn dump(nds: &mut melonds::Nds, path: &std::path::Path) {
    let (w, h) = (melonds::SCREEN_WIDTH as u32, melonds::SCREEN_HEIGHT as u32);
    let mut img = image::RgbaImage::new(w, h * 2);
    if let Some((top, bottom)) = nds.framebuffers() {
        for (i, screen) in [top, bottom].into_iter().enumerate() {
            for y in 0..h {
                for x in 0..w {
                    let [b, g, r, _] = screen[(y * w + x) as usize].to_le_bytes();
                    img.put_pixel(x, y + i as u32 * h, image::Rgba([r, g, b, 0xff]));
                }
            }
        }
    }
    img.save(path).expect("failed to write png");
    println!("dumped {}", path.display());
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let rom = std::fs::read(&args[0]).expect("failed to read rom");
    let save = std::fs::read(&args[1]).expect("failed to read save");
    let (frames0, tags0) = parse_script(&args[2]);
    let (frames1, tags1) = parse_script(&args[3]);
    let outdir = std::path::PathBuf::from(&args[4]);
    std::fs::create_dir_all(&outdir).unwrap();

    let air: &'static Airwaves = Box::leak(Box::new(Airwaves::new()));
    melonds::install_host(Box::new(AirwavesHost(air))).ok().expect("host installed twice");

    let mut pair = [
        melonds::Nds::new(&rom, Some(&save), 0).expect("cart rejected"),
        melonds::Nds::new(&rom, Some(&save), 1).expect("cart rejected"),
    ];
    for nds in &mut pair {
        nds.set_rtc(2026, 1, 1, 0, 0, 0);
        nds.boot();
    }

    let scripts = [&frames0, &frames1];
    let total = frames0.len().max(frames1.len());
    let start = std::time::Instant::now();
    for frame in 0..total {
        let inputs = [0, 1].map(|i| scripts[i].get(frame).copied().unwrap_or_default());
        std::thread::scope(|s| {
            for (i, nds) in pair.iter_mut().enumerate() {
                s.spawn(move || {
                    match inputs[i].touch {
                        Some((x, y)) => nds.touch(x, y),
                        None => nds.release_screen(),
                    }
                    nds.set_keys(inputs[i].keys);
                    nds.run_frame();
                    let mut st = air.state.lock().unwrap();
                    st.seats[i].frames_done += 1;
                    air.cv.notify_all();
                });
            }
        });
        for (i, tags) in [&tags0, &tags1].into_iter().enumerate() {
            for (at, tag) in tags.iter().filter(|(at, _)| *at == frame) {
                let _ = at;
                dump(&mut pair[i], &outdir.join(format!("i{i}_{tag}.png")));
            }
        }
        if frame % 1200 == 1199 {
            println!(
                "frame {frame}: sent {}/{} received {}/{}",
                air.sent[0].load(Ordering::Relaxed),
                air.sent[1].load(Ordering::Relaxed),
                air.received[0].load(Ordering::Relaxed),
                air.received[1].load(Ordering::Relaxed),
            );
        }
    }
    let elapsed = start.elapsed();
    println!(
        "{} frames in {:.2?} ({:.2}x realtime/pair); traffic: sent {}/{} received {}/{}",
        total,
        elapsed,
        total as f64 / elapsed.as_secs_f64() / 59.8261,
        air.sent[0].load(Ordering::Relaxed),
        air.sent[1].load(Ordering::Relaxed),
        air.received[0].load(Ordering::Relaxed),
        air.received[1].load(Ordering::Relaxed),
    );
}
