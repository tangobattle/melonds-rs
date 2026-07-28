//! Drive a cart with a key script and dump the screens — the eyes for
//! menu-flow reconnaissance.
//!
//!     cargo run --release --example explore -- <rom.nds> <script> <out.png> [save.bin]
//!
//! Script: comma-separated `<frames>x<keys>` steps, where keys is a
//! `+`-joined list of A B X Y L R START SELECT UP DOWN LEFT RIGHT (or
//! empty for idle), e.g. `240x,10xSTART,60x,10xA,300x`. A trailing
//! `@<file>` after the frame count on any step dumps the screens at the
//! end of that step, e.g. `240x@title,10xSTART,120x@menu` writes
//! title.png / menu.png next to <out.png>. The final state always dumps
//! to <out.png> itself.

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

fn dump(nds: &mut melonds::Nds, path: &std::path::Path) {
    let (w, h) = (melonds::SCREEN_WIDTH as u32, melonds::SCREEN_HEIGHT as u32);
    let mut img = image::RgbaImage::new(w, h * 2);
    if let Some((top, bottom)) = nds.framebuffers() {
        for (i, screen) in [top, bottom].into_iter().enumerate() {
            for y in 0..h {
                for x in 0..w {
                    let px = screen[(y * w + x) as usize];
                    // BGRA word -> RGBA bytes
                    let [b, g, r, _] = px.to_le_bytes();
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
    let rom_path = &args[0];
    let script = &args[1];
    let out_path = std::path::PathBuf::from(&args[2]);
    let save = args.get(3).map(|p| std::fs::read(p).expect("failed to read save"));

    let rom = std::fs::read(rom_path).expect("failed to read rom");
    let mut nds = melonds::Nds::new(&rom, save.as_deref(), 0).expect("cart rejected");
    nds.set_rtc(2026, 1, 1, 0, 0, 0);
    nds.boot();

    let mut total = 0u32;
    for step in script.split(',') {
        let (frames, rest) = step.split_once('x').expect("step must be <frames>x<keys>");
        let (frames, tag) = match frames.split_once('@') {
            Some((f, tag)) => (f, Some(tag)),
            None => (frames, None),
        };
        // allow the dump tag on either side of the x
        let (keys_str, tag) = match rest.split_once('@') {
            Some((k, t)) => (k, Some(t)),
            None => (rest, tag),
        };
        let frames: u32 = frames.parse().expect("bad frame count");
        let keys = parse_keys(keys_str);
        nds.set_keys(keys);
        for _ in 0..frames {
            nds.run_frame();
        }
        total += frames;
        if let Some(tag) = tag {
            dump(&mut nds, &out_path.with_file_name(format!("{tag}.png")));
        }
    }
    println!("ran {total} frames, pc={:08x}", nds.pc());
    dump(&mut nds, &out_path);
}
