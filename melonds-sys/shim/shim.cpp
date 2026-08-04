// The embedder half of melonDS: the Platform:: implementation (melonDS
// resolves it at link time — this file takes the place of the Qt
// frontend's) plus the C ABI the Rust side calls.
//
// Platform choices, all in service of determinism:
//  - No file access at all. BIOS is FreeBIOS, firmware is generated,
//    the cart image and save come in as buffers, save writes go out
//    through the host vtable. Every file function fails cleanly.
//  - No wall clock. GetMSCount/GetUSCount are only referenced by
//    melonDS's own frontends (never by the core); they return 0 so any
//    accidental future use is loud rather than sneaky-nondeterministic.
//  - MP goes through the host vtable with the instance userdata, so the
//    Rust side owns packet transport and ordering entirely.

#include "shim.h"

// Must precede every melonDS header (it de-GCCs them under MSVC).
#include "msvc_compat.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <string>
#include <vector>

#include "NDS.h"
#include "NDSCart.h"
#include "SPU.h"
#include "GPU.h"
#include "RTC.h"
#include "Savestate.h"
#include "SPI_Firmware.h"
#include "FreeBIOS.h"
#include "Args.h"
#include "Platform.h"

using namespace melonDS;

static MdsHostVtable g_host = {};

void mds_set_host_vtable(const MdsHostVtable* vt)
{
    if (vt)
        g_host = *vt;
    else
        g_host = {};
}

// ---------------------------------------------------------------------
// The instance.

// A console lives in its own write-watched reservation, so that a
// snapshot can ask the operating system which of its pages the frames
// since the last one actually touched — the hardware's own dirty bits,
// at no cost to the emulation. A rollback session snapshots every tick
// and a battle moves ~3% of a console's state per tick, so copying only
// what moved is most of a snapshot's cost.
//
// Where the reservation cannot be write-watched (any platform but
// Windows), `watch_base` stays null and every save and load copies
// everything, exactly as before.
struct ConsoleMemory
{
    void* base = nullptr;
    size_t size = 0;
    // Per page, the generation at the end of which it was last written,
    // indexed by (address - base) >> 12. A page outside every watched
    // range keeps NEVER_CLEAN, so nothing is ever skipped on the word
    // of a record that is not being kept.
    static constexpr u32 NEVER_CLEAN = 0xFFFFFFFF;
    std::vector<u32> page_gen;
    u32 generation = 0;
    // The page ranges worth asking the kernel about: the bulk arrays a
    // savestate actually moves, learned from the first one. Empty until
    // then, which means "the whole reservation".
    std::vector<std::pair<size_t, size_t>> watched;
    // GetWriteWatch's output buffer, kept rather than reallocated: it
    // is one page pointer per dirty page and this runs every tick.
    std::vector<void*> written;
};

struct MdsNds
{
    ConsoleMemory memory;
    std::unique_ptr<NDS, void (*)(NDS*)> nds { nullptr, nullptr };
    void* userdata;
};

// One console's worth of write-watched pages. Windows tracks writes in
// the page tables and hands the list back on request, which is exactly
// the question a snapshot has; nothing else here needs to know a write
// happened.
static bool watched_alloc(ConsoleMemory& mem, size_t size)
{
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (p)
    {
        mem.base = p;
        mem.size = size;
        mem.page_gen.assign((size + 4095) / 4096, ConsoleMemory::NEVER_CLEAN);
        mem.written.resize((size + 4095) / 4096);
        return true;
    }
#endif
    (void)size;
    return false;
}

static void watched_free(ConsoleMemory& mem)
{
#ifdef _WIN32
    if (mem.base)
        VirtualFree(mem.base, 0, MEM_RELEASE);
#endif
    mem.base = nullptr;
}

// Close the current generation: every page written since the last call
// is stamped with it. A buffer filled right after this carries the
// generation as its own, and a page written later gets a higher one —
// which is the whole test a save or a load then makes.
static void watch_advance(ConsoleMemory& mem)
{
    if (!mem.base)
        return;
#ifdef _WIN32
    mem.generation++;
    ULONG granularity = 0;

    // Ask only about the ranges the bulk copies read: the reservation
    // is a whole console and most of it — timing tables, framebuffers,
    // the renderer's scratch — is never serialized, but the kernel
    // still walks the page tables of whatever it is asked about, which
    // costs more than the copy the answer saves.
    const bool learned = !mem.watched.empty();
    const size_t queries = learned ? mem.watched.size() : 1;
    for (size_t q = 0; q < queries; q++)
    {
        u8* from = (u8*)mem.base;
        size_t bytes = mem.size;
        if (learned)
        {
            from = (u8*)mem.base + (mem.watched[q].first << 12);
            bytes = (mem.watched[q].second - mem.watched[q].first) << 12;
        }
        ULONG_PTR count = mem.written.size();
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, from, bytes, mem.written.data(), &count, &granularity) != 0)
        {
            // The kernel refused: treat the range as written, which
            // costs a full copy and stays correct.
            const size_t first = (from - (u8*)mem.base) >> 12;
            std::fill(mem.page_gen.begin() + first, mem.page_gen.begin() + first + (bytes >> 12), mem.generation);
            continue;
        }
        for (ULONG_PTR i = 0; i < count; i++)
        {
            size_t page = ((u8*)mem.written[i] - (u8*)mem.base) >> 12;
            if (page < mem.page_gen.size())
                mem.page_gen[page] = mem.generation;
        }
    }
#endif
}

// Adopt the bulk arrays a full save just reported as the ranges worth
// watching. Their pages start clean as of the generation that save
// carries — it moved every one of them — and every other page keeps
// NEVER_CLEAN, so nothing outside is ever skipped.
static void watch_learn(ConsoleMemory& mem, std::vector<std::pair<const void*, uint32_t>>& arrays)
{
    if (!mem.base || !mem.watched.empty() || arrays.empty())
        return;

    std::vector<std::pair<size_t, size_t>> pages;
    for (auto& entry : arrays)
    {
        const u8* p = (const u8*)entry.first;
        if (p < (u8*)mem.base || p + entry.second > (u8*)mem.base + mem.size)
            continue; // outside the reservation: never eligible anyway
        pages.emplace_back((size_t)(p - (u8*)mem.base) >> 12,
                           ((size_t)(p + entry.second - 1 - (u8*)mem.base) >> 12) + 1);
    }
    if (pages.empty())
        return;

    std::sort(pages.begin(), pages.end());
    for (auto& range : pages)
    {
        if (!mem.watched.empty() && range.first <= mem.watched.back().second)
            mem.watched.back().second = std::max(mem.watched.back().second, range.second);
        else
            mem.watched.push_back(range);
    }

    // Put back every sentinel the whole-reservation pass overwrote.
    // Until the ranges were known that pass stamped a generation onto
    // every page it found written, unwatched ones included — and an
    // unwatched page is never asked about again, so it would have kept
    // that generation and read as clean forever after.
    std::fill(mem.page_gen.begin(), mem.page_gen.end(), ConsoleMemory::NEVER_CLEAN);
    for (auto& range : mem.watched)
        std::fill(mem.page_gen.begin() + range.first, mem.page_gen.begin() + range.second, mem.generation);
}

// Traps run under the JIT: a trapped address always starts its own
// block (CompileBlock cuts blocks in front of one) and the dispatch
// loop runs its handler just before that block, which is the same
// "just before the instruction" the interpreter delivers. What a new
// trap set changes is where those boundaries fall, so installing or
// clearing traps re-forms the block cache.
static void mds_apply_jit_gate(MdsNds* w)
{
#ifdef JIT_ENABLED
    w->nds->JIT.ResetBlockCache();
#endif
}

// Watches, unlike traps, cannot run under the JIT: they live in the
// interpreter's load paths, and compiled code reads memory without
// going through them. So a watch on either processor puts the whole
// console back on the interpreter, and taking the last one off hands it
// back to the JIT. Both directions reset the block cache — the code
// compiled either side of the switch was compiled for the other regime.
static void mds_apply_watch_gate(MdsNds* w)
{
#ifdef JIT_ENABLED
    bool watching = w->nds->ARM9.WatchHandler || w->nds->ARM7.WatchHandler;
    w->nds->SetJITArgs(watching ? std::nullopt : std::optional<JITArgs>(JITArgs()));
    w->nds->JIT.ResetBlockCache();
#endif
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

    // Placement-new into the write-watched reservation when there is
    // one; a plain heap console otherwise, which simply never reports
    // any page clean.
    if (watched_alloc(wrapper->memory, sizeof(NDS)))
    {
        NDS* console = new (wrapper->memory.base) NDS(std::move(args), wrapper.get());
        wrapper->nds = { console, [](NDS* p) { p->~NDS(); } };
    }
    else
    {
        wrapper->nds = { new NDS(std::move(args), wrapper.get()), [](NDS* p) { delete p; } };
    }
    wrapper->nds->SetNDSCart(std::move(cart));
    return wrapper.release();
}

void mds_nds_free(MdsNds* w)
{
    if (!w)
        return;
    // The console is destroyed before its reservation goes away: the
    // deleter runs the destructor in place, `watched_free` returns the
    // pages.
    w->nds.reset();
    watched_free(w->memory);
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

uint32_t mds_arm9_read32(MdsNds* w, uint32_t addr) { return w->nds->ARM9Read32(addr); }
uint16_t mds_arm9_read16(MdsNds* w, uint32_t addr) { return w->nds->ARM9Read16(addr); }
uint8_t mds_arm9_read8(MdsNds* w, uint32_t addr) { return w->nds->ARM9Read8(addr); }
void mds_arm9_write32(MdsNds* w, uint32_t addr, uint32_t val) { w->nds->ARM9Write32(addr, val); }
void mds_arm9_write16(MdsNds* w, uint32_t addr, uint16_t val) { w->nds->ARM9Write16(addr, val); }
void mds_arm9_write8(MdsNds* w, uint32_t addr, uint8_t val) { w->nds->ARM9Write8(addr, val); }

uint32_t mds_arm9_pc(MdsNds* w)
{
    // R[15] is the prefetch pointer, two instructions ahead of the one
    // about to run. Back it off so this reports the instruction's own
    // address — which is what a trap site is, so a handler asking where
    // it is gets the address it registered.
    auto& cpu = w->nds->ARM9;
    return cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
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

void mds_set_traps(MdsNds* w, const uint32_t* addrs, uint32_t count, MdsTrapFn fn, void* userdata)
{
    w->nds->ARM9.SetTraps(addrs, count, reinterpret_cast<melonDS::ARM::TrapFn>(fn), userdata);
    mds_apply_jit_gate(w);
}

// The ARM7 mirror of the mds_arm9_* surface. The other processor is
// where the platform code the game leans on actually runs — the sound
// engine, the wireless stack, the cartridge backup server — so a walk
// that has to answer *those* waits needs its feet on this side too.

uint32_t mds_arm7_read32(MdsNds* w, uint32_t addr) { return w->nds->ARM7Read32(addr); }
uint16_t mds_arm7_read16(MdsNds* w, uint32_t addr) { return w->nds->ARM7Read16(addr); }
uint8_t mds_arm7_read8(MdsNds* w, uint32_t addr) { return w->nds->ARM7Read8(addr); }
void mds_arm7_write32(MdsNds* w, uint32_t addr, uint32_t val) { w->nds->ARM7Write32(addr, val); }
void mds_arm7_write16(MdsNds* w, uint32_t addr, uint16_t val) { w->nds->ARM7Write16(addr, val); }
void mds_arm7_write8(MdsNds* w, uint32_t addr, uint8_t val) { w->nds->ARM7Write8(addr, val); }

uint32_t mds_arm7_pc(MdsNds* w)
{
    auto& cpu = w->nds->ARM7;
    return cpu.R[15] - ((cpu.CPSR & 0x20) ? 2 : 4);
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

void mds_set_traps7(MdsNds* w, const uint32_t* addrs, uint32_t count, MdsTrapFn fn, void* userdata)
{
    w->nds->ARM7.SetTraps(addrs, count, reinterpret_cast<melonDS::ARM::TrapFn>(fn), userdata);
    mds_apply_jit_gate(w);
}

void mds_set_watches(MdsNds* w, const uint32_t* addrs, uint32_t count, MdsWatchFn fn, void* userdata)
{
    w->nds->ARM9.SetWatches(addrs, count, reinterpret_cast<melonDS::ARM::WatchFn>(fn), userdata);
    mds_apply_watch_gate(w);
}

void mds_set_watches7(MdsNds* w, const uint32_t* addrs, uint32_t count, MdsWatchFn fn, void* userdata)
{
    w->nds->ARM7.SetWatches(addrs, count, reinterpret_cast<melonDS::ARM::WatchFn>(fn), userdata);
    mds_apply_watch_gate(w);
}

uint64_t mds_sys_timestamp(MdsNds* w)
{
    // SysTimestamp itself is protected; between frames the current-CPU
    // clock reading is the same value.
    return w->nds->GetSysClockCycles(0);
}

// `since` is the generation the buffer already holds a state from, or
// 0 for a buffer this console has never filled — see ConsoleMemory. It
// is closed here rather than by the caller so that the writes a save or
// a load itself makes land in the generation after the one it reports.
int32_t mds_state_save(MdsNds* w, uint8_t* buf, uint32_t cap, uint32_t since, uint32_t* gen_out)
{
    watch_advance(w->memory);
    Savestate state(buf, cap, true);
    if (state.Error)
        return -1;
    if (since != 0)
        state.SetDirtyPages(w->memory.base, (uint32_t)w->memory.size, w->memory.page_gen.data(), since);
    // The first full save is where the ranges to watch come from; it
    // moves every byte anyway, so recording costs it nothing.
    std::vector<std::pair<const void*, uint32_t>> arrays;
    const bool learning = w->memory.base && w->memory.watched.empty();
    if (learning)
        state.RecordBulkArrays(&arrays);
    if (!w->nds->DoSavestate(&state) || state.Error)
        return -1;
    if (learning)
        watch_learn(w->memory, arrays);
    if (gen_out)
        *gen_out = w->memory.base ? w->memory.generation : 0;
    return (int32_t)state.Length();
}

int32_t mds_state_load(MdsNds* w, const uint8_t* buf, uint32_t len, uint32_t since)
{
    watch_advance(w->memory);
    Savestate state(const_cast<uint8_t*>(buf), len, false);
    if (state.Error)
        return 0;
    if (since != 0)
        state.SetDirtyPages(w->memory.base, (uint32_t)w->memory.size, w->memory.page_gen.data(), since);
    return (w->nds->DoSavestate(&state) && !state.Error) ? 1 : 0;
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

// ---------------------------------------------------------------------
// Platform implementation.

namespace melonDS::Platform
{

static void* UD(void* userdata)
{
    return userdata ? static_cast<MdsNds*>(userdata)->userdata : nullptr;
}

void SignalStop(StopReason reason, void* userdata)
{
    if (g_host.signal_stop)
        g_host.signal_stop(UD(userdata), (int)reason);
}

// --- files: nothing to open, ever -------------------------------------

std::string GetLocalFilePath(const std::string& filename)
{
    return filename;
}

FileHandle* OpenFile(const std::string&, FileMode)
{
    return nullptr;
}

FileHandle* OpenLocalFile(const std::string&, FileMode)
{
    return nullptr;
}

bool FileExists(const std::string&) { return false; }
bool LocalFileExists(const std::string&) { return false; }
bool CheckFileWritable(const std::string&) { return false; }
bool CheckLocalFileWritable(const std::string&) { return false; }
bool CloseFile(FileHandle*) { return false; }
bool IsEndOfFile(FileHandle*) { return true; }
bool FileReadLine(char*, int, FileHandle*) { return false; }
u64 FilePosition(FileHandle*) { return 0; }
bool FileSeek(FileHandle*, s64, FileSeekOrigin) { return false; }
void FileRewind(FileHandle*) {}
u64 FileRead(void*, u64, u64, FileHandle*) { return 0; }
bool FileFlush(FileHandle*) { return false; }
u64 FileWrite(const void*, u64, u64, FileHandle*) { return 0; }
u64 FileWriteFormatted(FileHandle*, const char*, ...) { return 0; }
u64 FileLength(FileHandle*) { return 0; }

// --- logging -----------------------------------------------------------

void Log(LogLevel level, const char* fmt, ...)
{
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (g_host.log)
        g_host.log((int)level, buf);
    else
        fputs(buf, stderr);
}

// --- threading primitives ----------------------------------------------
// Only the optional threaded soft renderer uses these (we leave it off),
// but they're real implementations so nothing explodes if it's enabled.

struct Thread
{
    std::thread t;
};

Thread* Thread_Create(std::function<void()> func)
{
    return new Thread { std::thread(std::move(func)) };
}

void Thread_Free(Thread* thread)
{
    if (thread->t.joinable())
        thread->t.detach();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread->t.joinable())
        thread->t.join();
}

struct Semaphore
{
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
};

Semaphore* Semaphore_Create() { return new Semaphore(); }
void Semaphore_Free(Semaphore* sema) { delete sema; }

void Semaphore_Reset(Semaphore* sema)
{
    std::lock_guard<std::mutex> lock(sema->m);
    sema->count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    std::unique_lock<std::mutex> lock(sema->m);
    sema->cv.wait(lock, [&] { return sema->count > 0; });
    sema->count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(sema->m);
    if (!sema->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return sema->count > 0; }))
        return false;
    sema->count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    std::lock_guard<std::mutex> lock(sema->m);
    sema->count += count;
    sema->cv.notify_all();
}

struct Mutex
{
    std::mutex m;
};

Mutex* Mutex_Create() { return new Mutex(); }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { mutex->m.lock(); }
void Mutex_Unlock(Mutex* mutex) { mutex->m.unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return mutex->m.try_lock(); }

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

// Never used by the core (frontends only); keep them inert.
u64 GetMSCount() { return 0; }
u64 GetUSCount() { return 0; }

// --- saves / firmware / rtc ---------------------------------------------

void WriteNDSSave(const u8* savedata, u32 savelen, u32 writeoffset, u32 writelen, void* userdata)
{
    if (g_host.write_save)
        g_host.write_save(UD(userdata), savedata, savelen, writeoffset, writelen);
}

void WriteGBASave(const u8*, u32, u32, u32, void*) {}

void WriteFirmware(const Firmware&, u32, u32, void*) {}

void WriteDateTime(int, int, int, int, int, int, void*) {}

// --- local multiplayer ---------------------------------------------------

void MP_Begin(void* userdata)
{
    if (g_host.mp_begin)
        g_host.mp_begin(UD(userdata));
}

void MP_End(void* userdata)
{
    if (g_host.mp_end)
        g_host.mp_end(UD(userdata));
}

int MP_SendPacket(u8* data, int len, u64 timestamp, void* userdata)
{
    return g_host.mp_send_packet ? g_host.mp_send_packet(UD(userdata), data, len, timestamp) : len;
}

int MP_RecvPacket(u8* data, u64* timestamp, void* userdata)
{
    if (!g_host.mp_recv_packet)
        return 0;
    u64 now = static_cast<MdsNds*>(userdata)->nds->Wifi.GetUSTimestamp();
    return g_host.mp_recv_packet(UD(userdata), data, now, timestamp);
}

int MP_SendCmd(u8* data, int len, u64 timestamp, void* userdata)
{
    return g_host.mp_send_cmd ? g_host.mp_send_cmd(UD(userdata), data, len, timestamp) : len;
}

int MP_SendReply(u8* data, int len, u64 timestamp, u16 aid, void* userdata)
{
    return g_host.mp_send_reply ? g_host.mp_send_reply(UD(userdata), data, len, timestamp, aid) : len;
}

int MP_SendAck(u8* data, int len, u64 timestamp, void* userdata)
{
    return g_host.mp_send_ack ? g_host.mp_send_ack(UD(userdata), data, len, timestamp) : len;
}

int MP_RecvHostPacket(u8* data, u64* timestamp, void* userdata)
{
    if (!g_host.mp_recv_host_packet)
        return -1;
    u64 now = static_cast<MdsNds*>(userdata)->nds->Wifi.GetUSTimestamp();
    return g_host.mp_recv_host_packet(UD(userdata), data, now, timestamp);
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata)
{
    if (!g_host.mp_recv_replies)
        return 0;
    u64 now = static_cast<MdsNds*>(userdata)->nds->Wifi.GetUSTimestamp();
    return g_host.mp_recv_replies(UD(userdata), data, now, timestamp, aidmask);
}

void MP_USClock(u64 timestamp, void* userdata)
{
    if (g_host.mp_clock)
        g_host.mp_clock(UD(userdata), timestamp);
}

// --- internet (WifiAP) — no connectivity ---------------------------------

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

// --- camera / mic / addons — absent hardware ------------------------------

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}

void Camera_CaptureFrame(int, u32* frame, int width, int height, bool, void*)
{
    memset(frame, 0, (size_t)width * height * sizeof(u32));
}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}

int Mic_ReadInput(s16* data, int maxlength, void*)
{
    memset(data, 0, (size_t)maxlength * sizeof(s16));
    return 0;
}

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.0f; }

// --- DSi AAC HLE — not a DSi ----------------------------------------------

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

// --- dynamic libraries (pcap only, which we don't build) ------------------

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

} // namespace melonDS::Platform
