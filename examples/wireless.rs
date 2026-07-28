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

    /// Whose turn it is to execute. Exactly one instance runs at any

    /// moment; every handoff is a function of emulated state, so the

    /// interleave - and therefore the whole session - is reproducible.

    turn: usize,

    /// Per-frame: this seat has finished its frame and will send

    /// nothing more until the next one.

    frame_done: [bool; 2],

}



struct Airwaves {

    /// Free-running mode: the instances are NOT frame-barriered, and

    /// blocking receives use a wall-clock timeout the way LocalMP does.

    /// Non-deterministic by construction - it exists to separate "does

    /// this game's wireless work under melonDS at all" from "is our

    /// lockstep timing model right".

    free: bool,

    state: Mutex<AirwavesState>,

    cv: Condvar,

    // Traffic counters, for the experiment's verdict.

    sent: [AtomicU64; 2],

    received: [AtomicU64; 2],

}



impl Airwaves {

    fn new(free: bool) -> Self {

        Airwaves {

            free,

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

        let _ = peer;

    }



    /// Block until this seat can be answered, the way LocalMP's

    /// semaphore wait does: return as soon as traffic is actually

    /// queued for us. Progress heuristics are only the *give-up*

    /// conditions — a receive that returns empty while the peer was

    /// still going to send this round is what collapses an MP session.

    ///

    /// `want_reply` picks which queue counts as data. The give-up set

    /// is deterministic and deadlock-free: the peer detached, the peer

    /// finished its frame (so it sends nothing more this round), or the

    /// peer is parked too — nobody can produce data, so waiting longer

    /// would hang the pair.

    fn wait_for_traffic<'a>(&'a self, me: usize, ts: u64, want_reply: bool) -> std::sync::MutexGuard<'a, AirwavesState> {

        let peer = 1 - me;

        let mut st = self.state.lock().unwrap();

        if ts > st.seats[me].progress {

            st.seats[me].progress = ts;

        }

        let have = |st: &AirwavesState| {

            if want_reply {

                !st.seats[me].replies.is_empty()

            } else {

                !st.seats[me].incoming.is_empty()

            }

        };



        if self.free {

            // Wall-clock mode keeps LocalMP's timeout semantics.

            let deadline = std::time::Instant::now() + std::time::Duration::from_millis(25);

            while !have(&st) && std::time::Instant::now() < deadline {

                let remaining = deadline.saturating_duration_since(std::time::Instant::now());

                st = self.cv.wait_timeout(st, remaining).unwrap().0;

            }

            return st;

        }



        // Nothing more can arrive this frame if the peer is finished or

        // off the air: give up without yielding, since handing the token

        // to a seat that will never hand it back would wedge the pair.

        if have(&st) || st.frame_done[peer] || !st.seats[peer].attached {

            return st;

        }



        // Yield: the peer runs until it produces something for us,

        // blocks itself, or finishes its frame.

        st.seats[me].parked_at = Some(ts);

        st.turn = peer;

        self.cv.notify_all();

        loop {

            let both_parked = st.seats[peer].parked_at.is_some();

            let done = have(&st) || st.frame_done[peer] || !st.seats[peer].attached || both_parked;

            // Only act while holding the token - that is what makes the

            // handoff order, and so the session, deterministic.

            if done && st.turn == me {

                st.seats[me].parked_at = None;

                return st;

            }

            st = self.cv.wait(st).unwrap();

        }

    }



    /// Take the run token for `me`, blocking until it is this seat's

    /// turn. Called before each frame.

    fn acquire<'a>(&'a self, me: usize) -> std::sync::MutexGuard<'a, AirwavesState> {

        let mut st = self.state.lock().unwrap();

        while st.turn != me {

            st = self.cv.wait(st).unwrap();

        }

        st

    }

}



/// Everything about the airwaves that a pair snapshot must carry. The

/// in-flight queues are session state as much as the instances' wifi

/// registers are: restore without them and the peers disagree about

/// what was on the air.

#[derive(Clone)]

struct AirwavesSnapshot {

    incoming: [Vec<Mp>; 2],

    replies: [Vec<Mp>; 2],

    progress: [u64; 2],

    attached: [bool; 2],

    turn: usize,

}



impl Airwaves {

    fn snapshot(&self) -> AirwavesSnapshot {

        let st = self.state.lock().unwrap();

        AirwavesSnapshot {

            incoming: [st.seats[0].incoming.iter().cloned().collect(), st.seats[1].incoming.iter().cloned().collect()],

            replies: [st.seats[0].replies.iter().cloned().collect(), st.seats[1].replies.iter().cloned().collect()],

            progress: [st.seats[0].progress, st.seats[1].progress],

            attached: [st.seats[0].attached, st.seats[1].attached],

            turn: st.turn,

        }

    }



    fn restore(&self, snap: &AirwavesSnapshot) {

        let mut st = self.state.lock().unwrap();

        for i in 0..2 {

            st.seats[i].incoming = snap.incoming[i].iter().cloned().collect();

            st.seats[i].replies = snap.replies[i].iter().cloned().collect();

            st.seats[i].progress = snap.progress[i];

            st.seats[i].attached = snap.attached[i];

            st.seats[i].parked_at = None;

        }

        st.turn = snap.turn;

        self.cv.notify_all();

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

        let mut st = self.0.wait_for_traffic(me, my_ts, false);

        match st.seats[me].incoming.pop_front() {

            Some(msg) => {

                self.0.received[me].fetch_add(1, Ordering::Relaxed);

                Some(deliver(msg, data, ts_out))

            }

            // Nothing on the air is `0` — a timed-out wait, which the

            // client retries. `-1` (None) means the HOST IS GONE and

            // tears the session down with a communication error, so it

            // is reserved for a peer that has actually detached.

            None => Some(if st.seats[1 - me].attached { 0 } else { -1 }),

        }

    }



    fn mp_recv_replies(&self, inst: melonds::InstanceId, data: &mut [u8], ts: u64, aidmask: u16) -> u16 {

        let me = Airwaves::seat_of(inst);

        let mut st = self.0.wait_for_traffic(me, ts + WIFI_REPLY_WINDOW_US, true);

        let mut mask = 0u16;

        while let Some(msg) = st.seats[me].replies.pop_front() {

            if let Mp::Reply { ts: rts, aid, data: d } = msg {

                // No stale-reply horizon here (LocalMP drops replies

                // older than the cmd by 32us). Our two instances are

                // frame-locked rather than wall-clock concurrent, so

                // their wifi clocks sit up to a frame apart and that

                // filter silently eats most of a round's replies.

                let _ = rts;

                // LocalMP packs reply payloads at (aid-1)*1024 — aid 0

                // is the host, clients start at 1 — while the returned

                // mask uses the raw aid bit. Writing at aid*1024 hands

                // the host a zeroed slot for its first client, which

                // reads as "communication error" one round later.

                if aid == 0 {

                    continue;

                }

                let off = (aid as usize - 1) * 1024;

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



fn pair_digest(pair: &mut [melonds::Nds; 2]) -> String {

    use sha2::Digest;

    let mut hasher = sha2::Sha256::new();

    for nds in pair {

        hasher.update(&*nds.main_ram());

    }

    hasher.finalize()[..8].iter().map(|b| format!("{b:02x}")).collect()

}



fn main() {

    let mut args: Vec<String> = std::env::args().skip(1).collect();

    let free = args.iter().any(|a| a == "--free");

    args.retain(|a| a != "--free");

    // --rollback <frame>: mid-session snapshot/restore/replay check.

    let rollback_at = args.iter().position(|a| a == "--rollback").map(|i| {

        let n: usize = args[i + 1].parse().expect("--rollback wants a frame number");

        args.drain(i..=i + 1);

        n

    });

    let rom = std::fs::read(&args[0]).expect("failed to read rom");

    let save = std::fs::read(&args[1]).expect("failed to read save");

    let (frames0, tags0) = parse_script(&args[2]);

    let (frames1, tags1) = parse_script(&args[3]);

    let outdir = std::path::PathBuf::from(&args[4]);

    std::fs::create_dir_all(&outdir).unwrap();



    let air: &'static Airwaves = Box::leak(Box::new(Airwaves::new(free)));

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



    if free {

        // Each instance runs its whole script on its own thread with no

        // barrier, the way melonDS's own frontend runs local MP.

        let tagsets = [&tags0, &tags1];

        std::thread::scope(|s| {

            for (i, nds) in pair.iter_mut().enumerate() {

                let outdir = &outdir;

                s.spawn(move || {

                    for (frame, input) in scripts[i].iter().enumerate() {

                        match input.touch {

                            Some((x, y)) => nds.touch(x, y),

                            None => nds.release_screen(),

                        }

                        nds.set_keys(input.keys);

                        nds.run_frame();

                        {

                            let mut st = air.state.lock().unwrap();

                            st.seats[i].frames_done += 1;

                            air.cv.notify_all();

                        }

                        for (at, tag) in tagsets[i].iter().filter(|(at, _)| *at == frame) {

                            let _ = at;

                            dump(nds, &outdir.join(format!("i{i}_{tag}.png")));

                        }

                    }

                });

            }

        });

        let elapsed = start.elapsed();

        println!(

            "FREE-RUN {} frames in {:.2?}; traffic: sent {}/{} received {}/{}",

            total,

            elapsed,

            air.sent[0].load(Ordering::Relaxed),

            air.sent[1].load(Ordering::Relaxed),

            air.received[0].load(Ordering::Relaxed),

            air.received[1].load(Ordering::Relaxed),

        );

        return;

    }



    // Captured at the rollback frame: both instances plus the airwaves.

    let mut pair_state = [Vec::new(), Vec::new()];

    let mut air_state = None;

    let mut first_digest = None;

    let mut frame = 0usize;

    let mut replaying = false;

    while frame < total {

        let inputs = [0, 1].map(|i| scripts[i].get(frame).copied().unwrap_or_default());

        {

            let mut st = air.state.lock().unwrap();

            st.frame_done = [false, false];

            st.turn = 0;

        }

        std::thread::scope(|s| {

            for (i, nds) in pair.iter_mut().enumerate() {

                s.spawn(move || {

                    // Seat 0 opens every frame; a seat runs only while

                    // it holds the token, so the two instances

                    // interleave in one reproducible order.

                    drop(air.acquire(i));

                    match inputs[i].touch {

                        Some((x, y)) => nds.touch(x, y),

                        None => nds.release_screen(),

                    }

                    nds.set_keys(inputs[i].keys);

                    nds.run_frame();

                    let mut st = air.state.lock().unwrap();

                    st.seats[i].frames_done += 1;

                    st.frame_done[i] = true;

                    // Hand off: to the peer if it still has a frame to

                    // run, otherwise keep it so a peer blocked on us can

                    // observe that we are done.

                    st.turn = if st.frame_done[1 - i] { i } else { 1 - i };

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

        if let Some(at) = rollback_at {

            // Snapshot at `at`, run 120 frames and digest, restore,

            // replay the same 120 and digest again. A live MP session

            // must come back bit-identical or rollback netplay is off

            // the table for this core.

            if frame == at && !replaying {

                for (nds, buf) in pair.iter_mut().zip(pair_state.iter_mut()) {

                    nds.save_state(buf).expect("save_state");

                }

                air_state = Some(air.snapshot());

                println!("rollback: captured pair + airwaves at frame {frame}");

            }

            if frame == at + 120 {

                let digest = pair_digest(&mut pair);

                match first_digest {

                    None => {

                        println!("rollback: first pass digest {digest}");

                        first_digest = Some(digest);

                        for (nds, buf) in pair.iter_mut().zip(pair_state.iter()) {

                            nds.load_state(buf).expect("load_state");

                        }

                        air.restore(air_state.as_ref().unwrap());

                        replaying = true;

                        frame = at;

                        continue;

                    }

                    Some(ref first) => {

                        println!(

                            "rollback: replay digest {digest} -> {}",

                            if *first == digest { "OK (bit-identical)" } else { "MISMATCH" }

                        );

                    }

                }

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

        frame += 1;

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

