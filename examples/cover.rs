//! Instruction-coverage tracer, for finding the code behind a screen.
//!
//! Registering enough trap addresses to saturate the core's address
//! filter makes every executed instruction report itself, so this is a
//! full ARM9 trace without the core having to know what a trace is.
//!
//! The point is the difference between two windows: cover the frames
//! where a menu sits idle, cover the frames where it acts on a press,
//! and what only appears in the second is the code that handled the
//! press.
//!
//! Usage: cover <rom> --save FILE [options]
//!   --script FILE     lines "<frame> <keys|T:x,y|->", held from that frame
//!   --frames N        run N frames
//!   --cover A-B:FILE  record every address executed in frames A..=B
//!   --range LO-HI     address range to record (default the cart code)
//!   --watch ADDR:LEN  print hex when a main-RAM range changes
//!   --redirect S:T    whenever the ARM9 reaches S, jump it to T instead
//!                     (repeatable) — for trying out a priming anchor
//!   --redirect-once S:T  the same, but only the first time S is reached —
//!                     for an anchor that must fire once and then let the
//!                     game carry on normally
//!   --probe ADDR      print r0-r7 the first few times ADDR is reached —
//!                     for finding the object a handler is working on
//!   --shot-at F,F,..  write console 0's screens as a PNG at these frames
//!   --dump-dir DIR    where PNGs go (default .)

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

fn parse_keys(s: &str) -> (u32, Option<(u16, u16)>) {
    if s == "-" {
        return (0, None);
    }
    if let Some(xy) = s.strip_prefix("T:") {
        let (x, y) = xy.split_once(',').unwrap();
        return (0, Some((x.parse().unwrap(), y.parse().unwrap())));
    }
    let keys = s
        .split('+')
        .map(|k| match k {
            "A" => 1 << 0,
            "B" => 1 << 1,
            "SEL" => 1 << 2,
            "ST" => 1 << 3,
            "RI" => 1 << 4,
            "LE" => 1 << 5,
            "UP" => 1 << 6,
            "DO" => 1 << 7,
            "RB" => 1 << 8,
            "LB" => 1 << 9,
            "X" => 1 << 10,
            "Y" => 1 << 11,
            k => panic!("unknown key {k:?}"),
        })
        .fold(0, |a, b| a | b);
    (keys, None)
}

fn parse_hex(s: &str) -> u32 {
    u32::from_str_radix(s.trim_start_matches("0x"), 16).unwrap()
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let rom_path = args[0].clone();
    let mut opt: HashMap<String, Vec<String>> = HashMap::new();
    let mut i = 1;
    while i < args.len() {
        let key = args[i].trim_start_matches("--").to_string();
        opt.entry(key).or_default().push(args[i + 1].clone());
        i += 2;
    }
    let one = |k: &str| opt.get(k).map(|v| v[0].clone());

    let rom = std::fs::read(&rom_path).unwrap();
    let save = one("save").map(|p| std::fs::read(p).unwrap());
    let mut nds = melonds::Nds::new(&rom, save.as_deref(), 0, 0).unwrap();
    nds.boot();

    // (start, end, path)
    let windows: Vec<(u32, u32, String)> = opt
        .get("cover")
        .map(|v| {
            v.iter()
                .map(|w| {
                    let (range, path) = w.split_once(':').unwrap();
                    let (a, b) = range.split_once('-').unwrap();
                    (a.parse().unwrap(), b.parse().unwrap(), path.to_string())
                })
                .collect()
        })
        .unwrap_or_default();

    // Recording is gated per frame so the trace only costs what the
    // windows ask for.
    // An atomic, not a lock: this is read once per executed
    // instruction, so the gate has to be nearly free when it is shut.
    let recording = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let hits: Arc<Mutex<std::collections::HashSet<u32>>> = Arc::new(Mutex::new(Default::default()));

    // Redirects are the point of the exercise: an anchor found by
    // covering is tried by pointing it at the branch a press would have
    // taken, and the game should carry on as if the press happened.
    let redirects: Vec<(u32, u32)> = opt
        .get("redirect")
        .map(|v| {
            v.iter()
                .map(|w| {
                    let (s, t) = w.split_once(':').unwrap();
                    (parse_hex(s), parse_hex(t))
                })
                .collect()
        })
        .unwrap_or_default();
    let once: Vec<(u32, u32)> = opt
        .get("redirect-once")
        .map(|v| {
            v.iter()
                .map(|w| {
                    let (s, t) = w.split_once(':').unwrap();
                    (parse_hex(s), parse_hex(t))
                })
                .collect()
        })
        .unwrap_or_default();
    let probes: Vec<u32> = opt.get("probe").map(|v| v.iter().map(|a| parse_hex(a)).collect()).unwrap_or_default();
    let fired: Arc<Mutex<HashMap<u32, usize>>> = Arc::new(Mutex::new(HashMap::new()));

    if !windows.is_empty() || !redirects.is_empty() || !once.is_empty() || !probes.is_empty() {
        // Every halfword address in the range, because a trap only
        // reports for an address that was registered. The range is the
        // cart's code by default; widening it costs memory per address,
        // not speed, since the core's filter saturates either way.
        let (lo, hi) = one("range")
            .map(|r| {
                let (a, b) = r.split_once('-').unwrap();
                (parse_hex(a), parse_hex(b))
            })
            .unwrap_or((0x0200_0000, 0x0214_0000));
        let mut traps: Vec<(u32, Box<dyn FnMut(&mut melonds::Nds)>)> = Vec::new();
        if !windows.is_empty() {
            traps.extend((lo..hi).step_by(2).map(|addr| {
                let recording = recording.clone();
                let hits = hits.clone();
                let f: Box<dyn FnMut(&mut melonds::Nds)> = Box::new(move |nds: &mut melonds::Nds| {
                    if recording.load(std::sync::atomic::Ordering::Relaxed) {
                        hits.lock().unwrap().insert(nds.pc());
                    }
                });
                (addr, f)
            }));
        }
        // A redirect replaces any coverage handler on the same address:
        // one handler per address, and jumping is the more specific job.
        for &(site, target) in &redirects {
            traps.retain(|(addr, _)| *addr != site);
            let fired = fired.clone();
            traps.push((
                site,
                Box::new(move |nds: &mut melonds::Nds| {
                    *fired.lock().unwrap().entry(site).or_default() += 1;
                    nds.jump_here(target);
                }),
            ));
        }
        for &site in &probes {
            traps.retain(|(addr, _)| *addr != site);
            let mut seen = 0usize;
            traps.push((
                site,
                Box::new(move |nds: &mut melonds::Nds| {
                    if seen >= 4 {
                        return;
                    }
                    seen += 1;
                    let regs: Vec<String> = (0..8).map(|i| format!("r{i}={:08x}", nds.reg(i))).collect();
                    println!("probe {site:08x} #{seen}: {}", regs.join(" "));
                }),
            ));
        }
        for &(site, target) in &once {
            traps.retain(|(addr, _)| *addr != site);
            let fired = fired.clone();
            let mut spent = false;
            traps.push((
                site,
                Box::new(move |nds: &mut melonds::Nds| {
                    if spent {
                        return;
                    }
                    spent = true;
                    *fired.lock().unwrap().entry(site).or_default() += 1;
                    nds.jump_here(target);
                }),
            ));
        }
        nds.set_traps(traps);
    }

    let mut script: Vec<(u32, u32, Option<(u16, u16)>)> = Vec::new();
    if let Some(path) = one("script") {
        for line in std::fs::read_to_string(path).unwrap().lines() {
            let line = line.split('#').next().unwrap().trim();
            if line.is_empty() {
                continue;
            }
            let mut it = line.split_whitespace();
            let frame: u32 = it.next().unwrap().parse().unwrap();
            let (keys, touch) = parse_keys(it.next().unwrap());
            script.push((frame, keys, touch));
        }
        script.sort_by_key(|e| e.0);
    }

    let watches: Vec<(u32, usize)> = opt
        .get("watch")
        .map(|v| {
            v.iter()
                .map(|w| {
                    let (a, l) = w.split_once(':').unwrap();
                    (parse_hex(a), parse_hex(l) as usize)
                })
                .collect()
        })
        .unwrap_or_default();
    let mut watch_prev: Vec<Vec<u8>> = watches.iter().map(|&(_, l)| vec![0; l]).collect();

    let frames: u32 = one("frames").map(|v| v.parse().unwrap()).unwrap_or(600);
    let shot_at: Vec<u32> = one("shot-at")
        .map(|v| v.split(',').map(|x| x.parse().unwrap()).collect())
        .unwrap_or_default();
    let dump_dir = one("dump-dir").unwrap_or_else(|| ".".into());
    let mut si = 0usize;
    let mut cur = (0u32, None);

    for f in 0..frames {
        while si < script.len() && script[si].0 <= f {
            cur = (script[si].1, script[si].2);
            si += 1;
        }
        match cur.1 {
            Some((x, y)) => nds.touch(x, y),
            None => nds.release_screen(),
        }
        nds.set_keys(cur.0);

        let active = windows.iter().any(|&(a, b, _)| f >= a && f <= b);
        recording.store(active, std::sync::atomic::Ordering::Relaxed);
        nds.run_frame();
        recording.store(false, std::sync::atomic::Ordering::Relaxed);

        for (wi, &(addr, len)) in watches.iter().enumerate() {
            let ram = nds.main_ram();
            let mask = ram.len() - 1;
            let base = (addr as usize - 0x0200_0000) & mask;
            let buf = ram[base..base + len].to_vec();
            if buf != watch_prev[wi] {
                let hex: Vec<String> = buf.iter().map(|b| format!("{b:02x}")).collect();
                println!("[{f:5}] {addr:08x}: {}", hex.join(" "));
                watch_prev[wi] = buf;
            }
        }

        if shot_at.contains(&f) {
            if let Some((top, bottom)) = nds.framebuffers() {
                let mut img = image::RgbImage::new(256, 384);
                for (half, screen) in [top, bottom].into_iter().enumerate() {
                    for (i, &pixel) in screen.iter().enumerate() {
                        let [b, g, r, _] = pixel.to_le_bytes();
                        img.put_pixel((i % 256) as u32, (half * 192 + i / 256) as u32, image::Rgb([r, g, b]));
                    }
                }
                img.save(format!("{dump_dir}/f{f:06}.png")).unwrap();
            }
        }

        // A window closes on its last frame; write it out and reset.
        for (a, b, path) in &windows {
            if f == *b {
                let mut sorted: Vec<u32> = hits.lock().unwrap().iter().copied().collect();
                sorted.sort_unstable();
                let text: String = sorted.iter().map(|a| format!("{a:08x}\n")).collect();
                std::fs::write(path, text).unwrap();
                println!("[{f:5}] cover {a}-{b}: {} addresses -> {path}", sorted.len());
                hits.lock().unwrap().clear();
            }
        }
    }
    for (site, count) in fired.lock().unwrap().iter() {
        println!("redirect {site:08x} fired {count} times");
    }
    println!("done at frame {frames}, pc={:08x}", nds.pc());
}
