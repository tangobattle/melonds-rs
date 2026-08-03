// C ABI over the melonDS core for the Rust binding.
//
// One MdsNds is one emulated DS. Instances are independent (the core's
// only cross-instance state is thread_local) — a link of two lives in
// one process and exchanges wireless frames through the host vtable's
// MP hooks, which receive the per-instance `userdata` passed to
// mds_nds_new. All hooks are process-global function pointers; install
// them once before creating any instance.
#ifndef MELONDS_SHIM_H
#define MELONDS_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MdsNds MdsNds;

// Wireless + save hooks into the embedder. Any pointer may be null:
// null MP hooks behave as empty airwaves (sends succeed into the void,
// receives report nothing), a null write_save drops save writes, a null
// log goes to stderr. MP semantics mirror melonDS Platform::MP_*:
// timestamps are the sender's emulated wifi microsecond clock, receives
// return the packet length (0 = nothing available, -1 = not connected),
// recv_replies returns the aid bitmask of replies written.
typedef struct MdsHostVtable {
    void (*log)(int level, const char* msg);
    void (*write_save)(void* userdata, const uint8_t* data, uint32_t len, uint32_t writeoffset, uint32_t writelen);
    void (*signal_stop)(void* userdata, int reason);

    void (*mp_begin)(void* userdata);
    void (*mp_end)(void* userdata);
    int (*mp_send_packet)(void* userdata, const uint8_t* data, int len, uint64_t timestamp);
    int (*mp_recv_packet)(void* userdata, uint8_t* data, uint64_t now, uint64_t* timestamp);
    int (*mp_send_cmd)(void* userdata, const uint8_t* data, int len, uint64_t timestamp);
    int (*mp_send_reply)(void* userdata, const uint8_t* data, int len, uint64_t timestamp, uint16_t aid);
    int (*mp_send_ack)(void* userdata, const uint8_t* data, int len, uint64_t timestamp);
    int (*mp_recv_host_packet)(void* userdata, uint8_t* data, uint64_t now, uint64_t* timestamp);
    uint16_t (*mp_recv_replies)(void* userdata, uint8_t* data, uint64_t now, uint64_t timestamp, uint16_t aidmask);
    // The instance's wifi clock advanced through `now`: every future MP
    // send from it is strictly later than `now`.
    void (*mp_clock)(void* userdata, uint64_t now);
} MdsHostVtable;

// Install the process-global host hooks. Copies the struct.
void mds_set_host_vtable(const MdsHostVtable* vt);

// Build a DS with FreeBIOS + generated firmware (MAC uniquified by
// instance_id the same way melonDS's frontend does), interpreter CPU,
// non-threaded software renderer, and the given retail cart. `save` may
// be null for a blank save. Returns null on cart parse failure.
MdsNds* mds_nds_new(const uint8_t* rom, uint32_t rom_len, const uint8_t* save, uint32_t save_len, int instance_id,
                    void* userdata);
void mds_nds_free(MdsNds* nds);

// Pin the cart RTC. Call before mds_boot.
void mds_rtc_set(MdsNds* nds, int year, int month, int day, int hour, int minute, int second);

// Reset and direct-boot the cart (skips the DS menu; required with
// FreeBIOS). The emulator is running after this returns.
void mds_boot(MdsNds* nds);

// Run one video frame. Returns the number of scanlines emulated (0 if
// the core is stopped).
uint32_t mds_run_frame(MdsNds* nds);

// Active-high key bits, matching the DS KEYINPUT/KEYXY layout:
// 0=A 1=B 2=Select 3=Start 4=Right 5=Left 6=Up 7=Down 8=R 9=L 10=X 11=Y.
void mds_set_keys(MdsNds* nds, uint32_t keys);
void mds_touch(MdsNds* nds, uint16_t x, uint16_t y);
void mds_release_screen(MdsNds* nds);

// Hold full-scale white noise on the microphone. This build's Platform
// hooks have no host mic behind them, so this is the only mic input a
// console here can get — and it is an input like the keys above, set
// per frame and carried by whoever replays the frame.
void mds_set_mic_static(MdsNds* nds, int on);

// Borrow the current front framebuffers, 32-bit BGRA, 256x192 each.
// Valid until the next mds_run_frame / mds_state_load. Returns 0 and
// nulls both on failure (no frame rendered yet).
// Toggle framebuffer production for this console. Off saves the 2D
// compositing cost for a console nobody displays; emulation (including
// display capture into VRAM) is bit-identical either way.
void mds_set_render(MdsNds* nds, int enabled);
// Which framebuffers the host shows: bit 0 top, bit 1 bottom. An engine
// whose screen is not shown does not compose it.
void mds_set_displayed_screens(MdsNds* nds, uint8_t screens);

int mds_framebuffers(MdsNds* nds, const uint32_t** top, const uint32_t** bottom);

// Drain up to max_frames stereo sample pairs into out (interleaved L/R,
// so out must hold 2*max_frames i16). Returns pairs written.
int mds_audio_read(MdsNds* nds, int16_t* out, int max_frames);
// Sample frames the SPU is currently holding for the frontend.
int mds_audio_queued(MdsNds* nds);

// Direct main-RAM aperture (4 MB in DS mode, mask 0x3FFFFF).
uint8_t* mds_main_ram(MdsNds* nds, uint32_t* mask_out);

// Bus accessors (IO/VRAM included), ARM9-visible address space. Writes
// go through the bus so JIT/dirty tracking stays coherent.
uint32_t mds_arm9_read32(MdsNds* nds, uint32_t addr);
uint16_t mds_arm9_read16(MdsNds* nds, uint32_t addr);
uint8_t mds_arm9_read8(MdsNds* nds, uint32_t addr);
void mds_arm9_write32(MdsNds* nds, uint32_t addr, uint32_t val);
void mds_arm9_write16(MdsNds* nds, uint32_t addr, uint16_t val);
void mds_arm9_write8(MdsNds* nds, uint32_t addr, uint8_t val);

// The address of the instruction the ARM9 is about to execute — for
// trap-anchor scouting, and what a trap handler sees as its site.
uint32_t mds_arm9_pc(MdsNds* nds);

// One ARM9 general register, 0-15 (15 is the raw prefetch pointer, not
// the instruction address — see mds_arm9_pc). Reading r4 inside a trap
// is how a handler finds the object the trapped function was working
// on, which is what lets it write the selection an organic confirm
// would have read.
uint32_t mds_arm9_reg(MdsNds* nds, uint32_t i);
void mds_arm9_set_reg(MdsNds* nds, uint32_t i, uint32_t val);

// Whether the ARM9 is executing Thumb — which is what decides bit 0 of
// a mds_arm9_jump target.
int mds_arm9_thumb(MdsNds* nds);

// Redirect the ARM9. `addr` is an interworking address: bit 0 set means
// Thumb, exactly as BX reads it. Called from inside a trap handler this
// replaces the trapped instruction with a jump; called outside one it
// takes effect immediately, mid-frame.
void mds_arm9_jump(MdsNds* nds, uint32_t addr);

// Fire `fn` just before the ARM9 executes any of `count` addresses.
// Handlers may read and write memory and may mds_arm9_jump. The address
// the handler receives is authoritative: the core filters cheaply and
// may pass an address that was never registered, so dispatch on it
// exactly. Passing count 0 removes the traps. Not saved in savestates:
// what the host installs now survives a state load, which is what
// rollback re-simulation wants.
typedef void (*MdsTrapFn)(void* userdata, uint32_t addr);
void mds_set_traps(MdsNds* nds, const uint32_t* addrs, uint32_t count, MdsTrapFn fn, void* userdata);

// The ARM7 mirror of the mds_arm9_* surface, same contracts throughout.
// The platform code the game leans on — sound, wireless, the cartridge
// backup server — runs on this processor, so a walk that must answer
// those waits needs traps here too. Traps on either CPU run under the
// JIT: trapped addresses become block boundaries, checked at dispatch.
uint32_t mds_arm7_read32(MdsNds* nds, uint32_t addr);
uint16_t mds_arm7_read16(MdsNds* nds, uint32_t addr);
uint8_t mds_arm7_read8(MdsNds* nds, uint32_t addr);
void mds_arm7_write32(MdsNds* nds, uint32_t addr, uint32_t val);
void mds_arm7_write16(MdsNds* nds, uint32_t addr, uint16_t val);
void mds_arm7_write8(MdsNds* nds, uint32_t addr, uint8_t val);
uint32_t mds_arm7_pc(MdsNds* nds);
uint32_t mds_arm7_reg(MdsNds* nds, uint32_t i);
void mds_arm7_set_reg(MdsNds* nds, uint32_t i, uint32_t val);
int mds_arm7_thumb(MdsNds* nds);
void mds_arm7_jump(MdsNds* nds, uint32_t addr);
void mds_set_traps7(MdsNds* nds, const uint32_t* addrs, uint32_t count, MdsTrapFn fn, void* userdata);

// Fire `fn` when either processor READS any of `count` data addresses —
// the question a trap cannot answer. A trap finds code by its address;
// a watch finds it by what it touches, which is the only way left when
// a variable is read without anything branching on it (nothing for a
// coverage diff to see) through a computed address (nothing for a
// search of the binary to find).
//
// The handler fires from inside the load, before the value reaches its
// register, so mds_armX_pc names the reading instruction and the
// registers are that instruction's. Observation only — do NOT jump from
// a watch handler, the interrupted load still has to complete. Address
// filtering is the same deal as the traps': cheap and approximate in
// the core, exact on the host. Byte reads report their own address;
// wider ones report the aligned address the load goes to, so watch the
// word a field sits in, not just the field.
//
// Arming a watch takes the console OFF THE JIT for as long as any watch
// is installed, because compiled code reads memory without asking. That
// costs a large multiple of the emulator's speed and shifts emulated
// timing, so this is an instrument for scouting a single run, never
// something to leave in a session.
typedef void (*MdsWatchFn)(void* userdata, uint32_t addr);
void mds_set_watches(MdsNds* nds, const uint32_t* addrs, uint32_t count, MdsWatchFn fn, void* userdata);
void mds_set_watches7(MdsNds* nds, const uint32_t* addrs, uint32_t count, MdsWatchFn fn, void* userdata);

// Emulated system-clock cycle count (33.513982 MHz domain) — the
// lockstep coordinator's notion of instance progress.
uint64_t mds_sys_timestamp(MdsNds* nds);

// Savestate round-trip, in memory. mds_state_save returns the number of
// bytes written, or -1 if cap was too small (call again with a bigger
// buffer). mds_state_load returns 1 on success.
int32_t mds_state_save(MdsNds* nds, uint8_t* buf, uint32_t cap);
int32_t mds_state_load(MdsNds* nds, const uint8_t* buf, uint32_t len);

// Copy of the cart's current save memory. Returns length (0 if none);
// out may be null to query the length.
uint32_t mds_save_read(MdsNds* nds, uint8_t* out, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif
