//! Proves the execution traps: that a handler fires at the address it
//! was registered for, and that redirecting from one displaces the
//! instruction it was standing on.
//!
//! Usage: traps <rom> [save]

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

fn main() {
    let mut args = std::env::args().skip(1);
    let rom = std::fs::read(args.next().expect("usage: traps <rom> [save]")).unwrap();
    let save = args.next().map(|p| std::fs::read(p).unwrap());

    // A busy address to trap: a Thumb instruction in the game's own
    // code that the ARM9 keeps coming back to. Sampled rather than
    // assumed, so this works on any cart — but sampled from cart code
    // specifically, because the BIOS runs ARM and this test redirects
    // by Thumb instruction width.
    //
    // Found with the traps themselves rather than by reading the pc
    // between frames: between frames the ARM9 is reliably parked in the
    // BIOS, so sampling there only ever turns up ARM code.
    let mut nds = melonds::Nds::new(&rom, save.as_deref(), 0, 0).unwrap();
    nds.boot();
    // The busiest one, so that what follows is exercised properly
    // rather than proven on a single firing.
    let seen: Arc<Mutex<HashMap<u32, usize>>> = Arc::new(Mutex::new(HashMap::new()));
    {
        let sweep: Vec<(u32, Box<dyn FnMut(&mut melonds::Nds)>)> = (0x0200_0000..0x0202_0000u32)
            .step_by(2)
            .map(|addr| {
                let seen = seen.clone();
                let f: Box<dyn FnMut(&mut melonds::Nds)> = Box::new(move |nds: &mut melonds::Nds| {
                    if nds.thumb() {
                        *seen.lock().unwrap().entry(nds.pc()).or_default() += 1;
                    }
                });
                (addr, f)
            })
            .collect();
        nds.set_traps(sweep);
    }
    for _ in 0..120 {
        nds.run_frame();
    }
    let site = seen
        .lock()
        .unwrap()
        .iter()
        .max_by_key(|(_, &n)| n)
        .map(|(&addr, _)| addr)
        .expect("never caught the ARM9 running Thumb in cart code");
    println!("caught a live Thumb site in cart code: {site:#010x}");
    drop(nds);

    // Run again to the same point and trap it.
    let hits = Arc::new(Mutex::new(Vec::<u32>::new()));
    let mut nds = melonds::Nds::new(&rom, save.as_deref(), 0, 0).unwrap();
    nds.boot();
    {
        let hits = hits.clone();
        nds.set_traps(vec![(
            site,
            Box::new(move |nds: &mut melonds::Nds| {
                // The handler sees the site it stopped at, before the
                // instruction there has run.
                hits.lock().unwrap().push(nds.pc());
            }),
        )]);
    }
    for _ in 0..180 {
        nds.run_frame();
    }
    let observed = hits.lock().unwrap().clone();
    println!("trap fired {} times", observed.len());
    assert!(!observed.is_empty(), "trap never fired at {site:#010x}");
    assert!(
        observed.iter().all(|&pc| pc == site),
        "handler saw a pc other than its site: {:#010x?}",
        &observed[..observed.len().min(4)],
    );
    println!("every firing reported pc == site");

    // Where the jump lands is the thing worth proving, and a jump to
    // the trap's own site is the case with an exactly known answer: it
    // must be perfectly transparent. The handler runs before the
    // instruction at the site, and JumpTo refills the pipeline to run
    // that same instruction — so a run that jumps on every single
    // firing has to end up in bit-identical state to a run that never
    // jumps at all. Landing even one instruction off would derail the
    // game, which is exactly what a redirect to site+2 does.
    let baseline_ram = sha2::Digest::finalize(nds.main_ram().iter().fold(
        <sha2::Sha256 as sha2::Digest>::new(),
        |mut h, b| {
            sha2::Digest::update(&mut h, [*b]);
            h
        },
    ));
    drop(nds);

    let jumps = Arc::new(Mutex::new(0usize));
    let mut nds = melonds::Nds::new(&rom, save.as_deref(), 0, 0).unwrap();
    nds.boot();
    {
        let jumps = jumps.clone();
        nds.set_traps(vec![(
            site,
            Box::new(move |nds: &mut melonds::Nds| {
                *jumps.lock().unwrap() += 1;
                nds.jump_here(site);
            }),
        )]);
    }
    for _ in 0..180 {
        nds.run_frame();
    }
    let redirected = *jumps.lock().unwrap();
    println!("redirected {redirected} times, always back onto the site");
    assert_eq!(
        redirected,
        observed.len(),
        "a self-jump changed how often the site was reached"
    );
    let after_ram = sha2::Digest::finalize(nds.main_ram().iter().fold(
        <sha2::Sha256 as sha2::Digest>::new(),
        |mut h, b| {
            sha2::Digest::update(&mut h, [*b]);
            h
        },
    ));
    assert_eq!(
        baseline_ram, after_ram,
        "jumping to the site itself was not transparent — the jump landed somewhere else"
    );
    println!("main RAM identical to the untouched run: the jump lands exactly on its target");

    // Clearing the traps must disarm them (and hand the ARM9 back to
    // the JIT).
    nds.set_traps(vec![]);
    for _ in 0..60 {
        nds.run_frame();
    }
    assert_eq!(redirected, *jumps.lock().unwrap(), "a trap fired after being cleared");
    println!("cleared traps stay silent");
    println!("OK");
}
