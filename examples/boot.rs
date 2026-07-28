//! Boot a cart headless and poke at it: run frames, report speed,
//! round-trip a savestate and prove the timeline replays bit-identically.
//!
//!     cargo run --release --example boot -- <rom.nds> [frames]

use sha2::Digest;

fn fb_digest(nds: &mut melonds::Nds) -> String {
    let mut hasher = sha2::Sha256::new();
    if let Some((top, bottom)) = nds.framebuffers() {
        hasher.update(bytemuck_cast(top));
        hasher.update(bytemuck_cast(bottom));
    }
    hex(&hasher.finalize()[..8])
}

fn bytemuck_cast(words: &[u32]) -> &[u8] {
    unsafe { std::slice::from_raw_parts(words.as_ptr() as *const u8, std::mem::size_of_val(words)) }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn main() {
    let mut args = std::env::args().skip(1);
    let rom_path = args.next().expect("usage: boot <rom.nds> [frames]");
    let frames: u32 = args.next().map(|s| s.parse().unwrap()).unwrap_or(600);

    let rom = std::fs::read(&rom_path).expect("failed to read rom");
    println!("rom: {} ({} MiB)", rom_path, rom.len() >> 20);

    let mut nds = melonds::Nds::new(&rom, None, 0).expect("cart rejected");
    nds.set_rtc(2026, 1, 1, 0, 0, 0);
    nds.boot();

    let start = std::time::Instant::now();
    for frame in 0..frames {
        nds.run_frame();
        if frame % 100 == 99 {
            println!(
                "frame {:5}  pc={:08x}  sys={:12}  fb={}",
                frame + 1,
                nds.pc(),
                nds.sys_timestamp(),
                fb_digest(&mut nds),
            );
        }
    }
    let elapsed = start.elapsed();
    println!(
        "{} frames in {:.2?} = {:.1} fps ({:.2}x realtime)",
        frames,
        elapsed,
        frames as f64 / elapsed.as_secs_f64(),
        frames as f64 / elapsed.as_secs_f64() / 59.8261,
    );

    // Savestate round-trip: the 60 frames after a restore must replay to
    // exactly the same framebuffer as the first time through.
    let mut state = Vec::new();
    let t = std::time::Instant::now();
    nds.save_state(&mut state).expect("save_state");
    println!("savestate: {} MiB in {:.2?}", state.len() >> 20, t.elapsed());

    for _ in 0..60 {
        nds.run_frame();
    }
    let first = fb_digest(&mut nds);

    let t = std::time::Instant::now();
    nds.load_state(&state).expect("load_state");
    println!("loadstate: {:.2?}", t.elapsed());
    for _ in 0..60 {
        nds.run_frame();
    }
    let second = fb_digest(&mut nds);

    println!("replay determinism: {} vs {} -> {}", first, second, if first == second { "OK" } else { "MISMATCH" });
}
