// The C ABI over the melonDS core: one C function per thing the Rust
// side can ask a console to do, and nothing else. Every entry point here
// is a translation — C types to C++ ones, a handle to an instance — and
// anything that has to decide something belongs on one side or the other
// of this file, not in it.
//
// The embedder's other half, the Platform:: implementation melonDS
// resolves at link time, is in platform.cpp.

#include "instance.h"

#include <cstring>
#include <memory>

#include "Args.h"
#include "NDSCart.h"
#include "SPI_Firmware.h"
#include "Savestate.h"

using namespace melonDS;

MdsHostVtable g_host = {};

void mds_set_host_vtable(const MdsHostVtable* vt)
{
    if (vt)
        g_host = *vt;
    else
        g_host = {};
}

MdsNds* mds_nds_new(const uint8_t* rom, uint32_t rom_len, const uint8_t* save, uint32_t save_len, int instance_id,
                    void* userdata)
{
    auto wrapper = std::make_unique<MdsNds>();
    wrapper->userdata = userdata;

    NDSArgs args {};
    // Defaults from Args.h already give FreeBIOS images and JITArgs(),
    // and we keep the JIT: interpreting four ARM cores (two per console)
    // cannot hold 60 fps for a link. A savestate load flushes the block
    // cache, so rollback pays a recompile, but that is far cheaper than
    // interpreting every tick.
    args.Firmware = Firmware(0);
    // The SPU mixes one sample every 1,024 master-clock cycles. Feeding
    // that exact rate to melonDS makes blip_buf's ratio 512:1 (its clock
    // is the 16.756991 MHz half-rate), so the embedder receives the
    // console's native samples instead of melonDS first converting them
    // to its frontend-oriented 48 kHz default.
    args.OutputSampleRate = 33513982.0 / 1024.0;

    // Uniquify the generated firmware's MAC per instance, mirroring
    // EmuInstance::customizeFirmware — distinct MACs are what let two
    // instances associate over the emulated airwaves.
    if (instance_id > 0)
    {
        auto& header = args.Firmware.GetHeader();
        MacAddress mac;
        memcpy(&mac, header.MacAddr.data(), sizeof(MacAddress));
        mac[3] += instance_id;
        mac[4] += instance_id * 0x44;
        mac[5] += instance_id * 0x10;
        mac[0] &= 0xFC;
        header.MacAddr = mac;
        header.UpdateChecksum();
        args.Firmware.UpdateChecksums();
    }

    NDSCart::NDSCartArgs cart_args {};
    if (save && save_len > 0)
    {
        cart_args.SRAM = std::make_unique<u8[]>(save_len);
        memcpy(cart_args.SRAM.get(), save, save_len);
        cart_args.SRAMLength = save_len;
    }
    auto cart = NDSCart::ParseROM(rom, rom_len, wrapper.get(), std::move(cart_args));
    if (!cart)
        return nullptr;

    wrapper->nds = wrapper->memory.Create(std::move(args), wrapper.get());
    wrapper->nds->SetNDSCart(std::move(cart));
    return wrapper.release();
}

void mds_nds_free(MdsNds* w)
{
    // ConsoleMemory takes the console with it, in the right order.
    delete w;
}

void mds_rtc_set(MdsNds* w, int year, int month, int day, int hour, int minute, int second)
{
    w->nds->RTC.SetDateTime(year, month, day, hour, minute, second);
}

void mds_boot(MdsNds* w)
{
    w->nds->Reset();
    w->nds->SetupDirectBoot("rom.nds");
    w->nds->Start();
}

uint32_t mds_run_frame(MdsNds* w)
{
    return w->nds->RunFrame();
}

void mds_set_keys(MdsNds* w, uint32_t keys)
{
    // SetKeyMask wants the raw register view: bit set = key released.
    w->nds->SetKeyMask(~keys & 0xFFF);
}

void mds_touch(MdsNds* w, uint16_t x, uint16_t y)
{
    w->nds->TouchScreen(x, y);
}

void mds_release_screen(MdsNds* w)
{
    w->nds->ReleaseScreen();
}

void mds_set_mic_static(MdsNds* w, int on)
{
    w->nds->Mic.SetStaticInput(on != 0);
}

void mds_set_render(MdsNds* w, int enabled)
{
    w->nds->GPU.RenderEnabled = enabled != 0;
}

void mds_set_displayed_screens(MdsNds* w, uint8_t screens)
{
    w->nds->GPU.DisplayedScreens = screens;
}

int mds_framebuffers(MdsNds* w, const uint32_t** top, const uint32_t** bottom)
{
    void* t = nullptr;
    void* b = nullptr;
    bool ok = w->nds->GPU.GetFramebuffers(&t, &b);
    *top = static_cast<const uint32_t*>(t);
    *bottom = static_cast<const uint32_t*>(b);
    return (ok && t && b) ? 1 : 0;
}

int mds_audio_read(MdsNds* w, int16_t* out, int max_frames)
{
    return w->nds->SPU.ReadOutput(out, max_frames);
}

int mds_audio_queued(MdsNds* w)
{
    return w->nds->SPU.GetOutputSize();
}

uint8_t* mds_main_ram(MdsNds* w, uint32_t* mask_out)
{
    if (mask_out)
        *mask_out = w->nds->MainRAMMask;
    return w->nds->MainRAM;
}

// ---------------------------------------------------------------------
// The processors. Everything below is mirrored for the ARM7, which is
// where the platform code the game leans on actually runs — the sound
// engine, the wireless stack, the cartridge backup server — so a walk
// that has to answer *those* waits needs its feet on that side too.

uint32_t mds_arm9_read32(MdsNds* w, uint32_t addr) { return w->nds->ARM9Read32(addr); }
uint16_t mds_arm9_read16(MdsNds* w, uint32_t addr) { return w->nds->ARM9Read16(addr); }
uint8_t mds_arm9_read8(MdsNds* w, uint32_t addr) { return w->nds->ARM9Read8(addr); }
void mds_arm9_write32(MdsNds* w, uint32_t addr, uint32_t val) { w->nds->ARM9Write32(addr, val); }
void mds_arm9_write16(MdsNds* w, uint32_t addr, uint16_t val) { w->nds->ARM9Write16(addr, val); }
void mds_arm9_write8(MdsNds* w, uint32_t addr, uint8_t val) { w->nds->ARM9Write8(addr, val); }

// R[15] is the prefetch pointer, two instructions ahead of the one about
// to run. Back it off so this reports the instruction's own address —
// which is what a trap site is, so a handler asking where it is gets the
// address it registered.
static uint32_t pc_of(const melonDS::ARM& cpu)
{
    return cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
}

uint32_t mds_arm9_pc(MdsNds* w)
{
    return pc_of(w->nds->ARM9);
}

uint32_t mds_arm9_reg(MdsNds* w, uint32_t i)
{
    return i < 16 ? w->nds->ARM9.R[i] : 0;
}

void mds_arm9_set_reg(MdsNds* w, uint32_t i, uint32_t val)
{
    if (i < 16)
        w->nds->ARM9.R[i] = val;
}

int mds_arm9_thumb(MdsNds* w)
{
    return (w->nds->ARM9.CPSR & 0x20) ? 1 : 0;
}

void mds_arm9_jump(MdsNds* w, uint32_t addr)
{
    w->nds->ARM9.JumpTo(addr);
}

uint32_t mds_arm7_read32(MdsNds* w, uint32_t addr) { return w->nds->ARM7Read32(addr); }
uint16_t mds_arm7_read16(MdsNds* w, uint32_t addr) { return w->nds->ARM7Read16(addr); }
uint8_t mds_arm7_read8(MdsNds* w, uint32_t addr) { return w->nds->ARM7Read8(addr); }
void mds_arm7_write32(MdsNds* w, uint32_t addr, uint32_t val) { w->nds->ARM7Write32(addr, val); }
void mds_arm7_write16(MdsNds* w, uint32_t addr, uint16_t val) { w->nds->ARM7Write16(addr, val); }
void mds_arm7_write8(MdsNds* w, uint32_t addr, uint8_t val) { w->nds->ARM7Write8(addr, val); }

uint32_t mds_arm7_pc(MdsNds* w)
{
    return pc_of(w->nds->ARM7);
}

uint32_t mds_arm7_reg(MdsNds* w, uint32_t i)
{
    return i < 16 ? w->nds->ARM7.R[i] : 0;
}

void mds_arm7_set_reg(MdsNds* w, uint32_t i, uint32_t val)
{
    if (i < 16)
        w->nds->ARM7.R[i] = val;
}

int mds_arm7_thumb(MdsNds* w)
{
    return (w->nds->ARM7.CPSR & 0x20) ? 1 : 0;
}

void mds_arm7_jump(MdsNds* w, uint32_t addr)
{
    w->nds->ARM7.JumpTo(addr);
}

// ---------------------------------------------------------------------
// Traps and watches.

// Traps run under the JIT: a trapped address always starts its own block
// (CompileBlock cuts blocks in front of one) and the dispatch loop runs
// its handler just before that block, which is the same "just before the
// instruction" the interpreter delivers. What a new trap set changes is
// where those boundaries fall, so installing or clearing traps re-forms
// the block cache.
static void apply_trap_gate(MdsNds* w)
{
#ifdef JIT_ENABLED
    w->nds->JIT.ResetBlockCache();
#else
    (void)w;
#endif
}

// Watches, unlike traps, cannot run under the JIT: they live in the
// interpreter's load paths, and compiled code reads memory without going
// through them. So a watch on either processor puts the whole console
// back on the interpreter, and taking the last one off hands it back to
// the JIT. Both directions reset the block cache — the code compiled
// either side of the switch was compiled for the other regime.
static void apply_watch_gate(MdsNds* w)
{
#ifdef JIT_ENABLED
    bool watching = w->nds->ARM9.WatchHandler || w->nds->ARM7.WatchHandler;
    w->nds->SetJITArgs(watching ? std::nullopt : std::optional<JITArgs>(JITArgs()));
    w->nds->JIT.ResetBlockCache();
#else
    (void)w;
#endif
}

void mds_set_traps(MdsNds* w, const uint32_t* addrs, uint32_t count, MdsTrapFn fn, void* userdata)
{
    w->nds->ARM9.SetTraps(addrs, count, reinterpret_cast<melonDS::ARM::TrapFn>(fn), userdata);
    apply_trap_gate(w);
}

void mds_set_traps7(MdsNds* w, const uint32_t* addrs, uint32_t count, MdsTrapFn fn, void* userdata)
{
    w->nds->ARM7.SetTraps(addrs, count, reinterpret_cast<melonDS::ARM::TrapFn>(fn), userdata);
    apply_trap_gate(w);
}

void mds_set_watches(MdsNds* w, const uint32_t* addrs, uint32_t count, MdsWatchFn fn, void* userdata)
{
    w->nds->ARM9.SetWatches(addrs, count, reinterpret_cast<melonDS::ARM::WatchFn>(fn), userdata);
    apply_watch_gate(w);
}

void mds_set_watches7(MdsNds* w, const uint32_t* addrs, uint32_t count, MdsWatchFn fn, void* userdata)
{
    w->nds->ARM7.SetWatches(addrs, count, reinterpret_cast<melonDS::ARM::WatchFn>(fn), userdata);
    apply_watch_gate(w);
}

// ---------------------------------------------------------------------
// Time and state.

uint64_t mds_sys_timestamp(MdsNds* w)
{
    // SysTimestamp itself is protected; between frames the current-CPU
    // clock reading is the same value.
    return w->nds->GetSysClockCycles(0);
}

int32_t mds_state_save(MdsNds* w, uint8_t* buf, uint32_t cap, uint32_t since, uint32_t* gen_out)
{
    Savestate state(buf, cap, true);
    if (!w->memory.DoSavestate(state, since))
        return -1;
    if (gen_out)
        *gen_out = w->memory.Generation();
    return (int32_t)state.Length();
}

int32_t mds_state_load(MdsNds* w, const uint8_t* buf, uint32_t len, uint32_t since)
{
    Savestate state(const_cast<uint8_t*>(buf), len, false);
    return w->memory.DoSavestate(state, since) ? 1 : 0;
}

uint32_t mds_save_read(MdsNds* w, uint8_t* out, uint32_t cap)
{
    auto* cart = w->nds->NDSCartSlot.GetCart();
    if (!cart)
        return 0;
    const u8* mem = cart->GetSaveMemory();
    u32 len = cart->GetSaveMemoryLength();
    if (!mem || !len)
        return 0;
    if (out && cap >= len)
        memcpy(out, mem, len);
    return len;
}
