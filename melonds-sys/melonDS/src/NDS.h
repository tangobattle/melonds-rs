/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef NDS_H
#define NDS_H

#include <memory>
#include <string>
#include <optional>
#include <functional>

#include "Platform.h"
#include "Savestate.h"
#include "types.h"
#include "NDSCart.h"
#include "GBACart.h"
#include "SPU.h"
#include "Mic.h"
#include "SPI.h"
#include "RTC.h"
#include "Wifi.h"
#include "AREngine.h"
#include "GPU.h"
#include "ARMJIT.h"
#include "MemRegion.h"
#include "ARMJIT_Memory.h"
#include "ARM.h"
#include "CRC32.h"
#include "DMA.h"
#include "FreeBIOS.h"

// when touching the main loop/timing code, pls test a lot of shit
// with this enabled, to make sure it doesn't desync
//#define DEBUG_CHECK_DESYNC

namespace melonDS
{
struct NDSArgs;
class Firmware;
enum
{
    Event_LCD = 0,
    Event_SPU,
    Event_Wifi,
    Event_RTC,

    Event_DisplayFIFO,
    Event_CartROMTransfer9,
    Event_CartSPITransfer9,
    Event_CartROMTransfer7,
    Event_CartSPITransfer7,
    Event_SPITransfer,
    Event_Div,
    Event_Sqrt,

    // DSi
    Event_DSi_SDMMCTransfer,
    Event_DSi_SDIOTransfer,
    Event_DSi_NWifi,
    Event_DSi_CamIRQ,
    Event_DSi_CamTransfer,
    Event_DSi_DSP,
    Event_DSi_DSPHLE, // TODO use same event for both flavors of DSP?
    Event_DSi_Cart2ROMTransfer9,
    Event_DSi_Cart2SPITransfer9,
    Event_DSi_Cart2ROMTransfer7,
    Event_DSi_Cart2SPITransfer7,
    Event_DSi_Cart1Power,
    Event_DSi_Cart2Power,

    Event_MAX
};

static constexpr u32 MaxEventFunctions = 3;

typedef void (*EventFunc)(void* that, u32 param);
typedef std::initializer_list<EventFunc> EventFuncList;
#define MakeEventThunk(class, func) [](void* that, u32 param) { static_cast<class*>(that)->func(param); }

struct SchedEvent
{
    std::array<EventFunc, MaxEventFunctions> Funcs;
    void* That;
    u64 Timestamp;
    u32 FuncID;
    u32 Param;
};
enum
{
    IRQ_VBlank = 0,
    IRQ_HBlank,
    IRQ_VCount,
    IRQ_Timer0,
    IRQ_Timer1,
    IRQ_Timer2,
    IRQ_Timer3,
    IRQ_RTC,
    IRQ_DMA0,
    IRQ_DMA1,
    IRQ_DMA2,
    IRQ_DMA3,
    IRQ_Keypad,
    IRQ_GBASlot,
    IRQ_Unused14,
    IRQ_Unused15,
    IRQ_IPCSync,
    IRQ_IPCSendDone,
    IRQ_IPCRecv,
    IRQ_CartXferDone,
    IRQ_CartIREQMC,   // IRQ triggered by game cart (example: Pokémon Typing Adventure, BT controller)
    IRQ_GXFIFO,
    IRQ_LidOpen,
    IRQ_SPI,
    IRQ_Wifi,

    // DSi IRQs
    IRQ_DSi_DSP = 24,
    IRQ_DSi_Camera,
    IRQ_DSi_Cart2XferDone,
    IRQ_DSi_Cart2IREQMC,
    IRQ_DSi_NDMA0,
    IRQ_DSi_NDMA1,
    IRQ_DSi_NDMA2,
    IRQ_DSi_NDMA3,
};

enum
{
    // DSi ARM7-side IE2/IF2
    IRQ2_DSi_GPIO18_0 = 0,
    IRQ2_DSi_GPIO18_1,
    IRQ2_DSi_GPIO18_2,
    IRQ2_DSi_Unused3,
    IRQ2_DSi_GPIO33_0,
    IRQ2_DSi_Headphone,
    IRQ2_DSi_BPTWL,
    IRQ2_DSi_GPIO33_3, // "sound enable input"
    IRQ2_DSi_SDMMC,
    IRQ2_DSi_SD_Data1,
    IRQ2_DSi_SDIO,
    IRQ2_DSi_SDIO_Data1,
    IRQ2_DSi_AES,
    IRQ2_DSi_I2C,
    IRQ2_DSi_MicExt
};

enum
{
    CPUStop_DMA9_0 = (1<<0),
    CPUStop_DMA9_1 = (1<<1),
    CPUStop_DMA9_2 = (1<<2),
    CPUStop_DMA9_3 = (1<<3),
    CPUStop_NDMA9_0 = (1<<4),
    CPUStop_NDMA9_1 = (1<<5),
    CPUStop_NDMA9_2 = (1<<6),
    CPUStop_NDMA9_3 = (1<<7),
    CPUStop_DMA9 = 0xFFF,

    CPUStop_DMA7_0 = (1<<16),
    CPUStop_DMA7_1 = (1<<17),
    CPUStop_DMA7_2 = (1<<18),
    CPUStop_DMA7_3 = (1<<19),
    CPUStop_NDMA7_0 = (1<<20),
    CPUStop_NDMA7_1 = (1<<21),
    CPUStop_NDMA7_2 = (1<<22),
    CPUStop_NDMA7_3 = (1<<23),
    CPUStop_DMA7 = (0xFFF<<16),

    CPUStop_Wakeup = (1<<29),
    CPUStop_Sleep = (1<<30),
    CPUStop_GXStall = (1<<31),
};

struct Timer
{
    u16 Reload;
    u16 Cnt;
    u32 Counter;
    u32 CycleShift;
};

enum
{
    Mem9_ITCM       = 0x00000001,
    Mem9_DTCM       = 0x00000002,
    Mem9_BIOS       = 0x00000004,
    Mem9_MainRAM    = 0x00000008,
    Mem9_WRAM       = 0x00000010,
    Mem9_IO         = 0x00000020,
    Mem9_Pal        = 0x00000040,
    Mem9_OAM        = 0x00000080,
    Mem9_VRAM       = 0x00000100,
    Mem9_GBAROM     = 0x00020000,
    Mem9_GBARAM     = 0x00040000,

    Mem7_BIOS       = 0x00000001,
    Mem7_MainRAM    = 0x00000002,
    Mem7_WRAM       = 0x00000004,
    Mem7_IO         = 0x00000008,
    Mem7_Wifi0      = 0x00000010,
    Mem7_Wifi1      = 0x00000020,
    Mem7_VRAM       = 0x00000040,
    Mem7_GBAROM     = 0x00000100,
    Mem7_GBARAM     = 0x00000200,

    // TODO: add DSi regions!
};

// supported GBA slot addon types
enum
{
    GBAAddon_RAMExpansion = 1,
    GBAAddon_RumblePak = 2,
    // Each game in the GBA Boktai trilogy uses the same solar sensor,
    // but Lunar Knights (the only NDS game to use the solar sensor)
    // applies slightly different effects depending on the game.
    GBAAddon_SolarSensorBoktai1 = 3,
    GBAAddon_SolarSensorBoktai2 = 4,
    GBAAddon_SolarSensorBoktai3 = 5,
    GBAAddon_MotionPakHomebrew = 6,
    GBAAddon_MotionPakRetail = 7,
    GBAAddon_GuitarGrip = 8,
};

class SPU;
class SPIHost;
class RTC;
class Wifi;

class AREngine;
class GPU;
class ARMJIT;

class NDS
{
private:
#ifdef JIT_ENABLED
    bool EnableJIT;
#endif
#ifdef GDBSTUB_ENABLED
    bool EnableGDBStub = false;
#endif

public: // TODO: Encapsulate the rest of these members
    void* UserData;

    int ConsoleType;
    // Which instance this is, for telling a savestate that came from
    // *here* from one that came from somewhere else. Never loaded from
    // a state — see NDS::DoSavestate.
    u64 InstanceCookie;

    // Set while loading a state this console did not take, so the
    // derived tables further down the load — CP15's included — know to
    // rebuild rather than trust what they are holding.
    bool LoadingForeignState = false;

    int CurCPU;

    SchedEvent SchedList[Event_MAX] {};
    u8 ARM9MemTimings[0x40000][8];
    u32 ARM9Regions[0x40000];
    u8 ARM7MemTimings[0x20000][4];
    u32 ARM7Regions[0x20000];

    u32 NumFrames;
    u32 NumLagFrames = 0;
    bool LagFrameFlag = false;

    // no need to worry about those overflowing, they can keep going for atleast 4350 years
    u64 ARM9Timestamp, ARM9Target;
    u64 ARM7Timestamp, ARM7Target;
    u32 ARM9ClockShift;

    u32 IME[2];
    u32 IE[2];
    u32 IF[2];
    u32 IE2;
    u32 IF2;
    Timer Timers[8];

    u32 CPUStop;

    u16 PowerControl9;

    u16 ExMemCnt[2];

protected:
    // These BIOS arrays should be declared *before* the component objects (JIT, SPI, etc.)
    // so that they're initialized before the component objects' constructors run.
    std::array<u8, ARM9BIOSSize> ARM9BIOS;
    std::array<u8, ARM7BIOSSize> ARM7BIOS;
    bool ARM9BIOSNative;
    bool ARM7BIOSNative;
public: // TODO: Encapsulate the rest of these members
    u16 ARM7BIOSProt;

    u8* MainRAM;
    u32 MainRAMMask;

    const u32 MainRAMMaxSize = 0x1000000;

    const u32 SharedWRAMSize = 0x8000;
    u8* SharedWRAM;

    MemRegion SWRAM_ARM9;
    MemRegion SWRAM_ARM7;

    u32 KeyInput;
    u16 RCnt;

    // JIT MUST be declared before all other component objects,
    // as they'll need the memory that it allocates in its constructor!
    // (Reminder: C++ fields are initialized in the order they're declared,
    // regardless of what the constructor's initializer list says.)
    melonDS::ARMJIT JIT;
    ARMv5 ARM9;
    ARMv4 ARM7;
    melonDS::SPU SPU;
    melonDS::Mic Mic;
    SPIHost SPI;
    melonDS::RTC RTC;
    melonDS::Wifi Wifi;
    NDSCart::NDSCartSlot NDSCartSlot;
    GBACart::GBACartSlot GBACartSlot;
    melonDS::GPU GPU;
    melonDS::AREngine AREngine;

    const u32 ARM7WRAMSize = 0x10000;
    u8* ARM7WRAM;

    // provision for DSi second cart slot
    NDSCart::NDSCartSlot* NDSCartSlots[2];

    virtual void Reset();
    void Start();

    /// Stop the emulator.
    virtual void Stop(Platform::StopReason reason = Platform::StopReason::External);

    bool DoSavestate(Savestate* file);

    void SetARM9RegionTimings(u32 addrstart, u32 addrend, u32 region, int buswidth, int nonseq, int seq);
    void SetARM7RegionTimings(u32 addrstart, u32 addrend, u32 region, int buswidth, int nonseq, int seq);

    void LoadBIOS();

    /// @return \c true if the loaded ARM9 BIOS image is a known dump
    /// of a native DS-compatible ARM9 BIOS.
    [[nodiscard]] bool IsLoadedARM9BIOSKnownNative() const noexcept { return ARM9BIOSNative; }
    [[nodiscard]] const std::array<u8, ARM9BIOSSize>& GetARM9BIOS() const noexcept { return ARM9BIOS; }
    void SetARM9BIOS(const std::array<u8, ARM9BIOSSize>& bios) noexcept;

    [[nodiscard]] const std::array<u8, ARM7BIOSSize>& GetARM7BIOS() const noexcept { return ARM7BIOS; }
    void SetARM7BIOS(const std::array<u8, ARM7BIOSSize>& bios) noexcept;

    /// @return \c true if the loaded ARM7 BIOS image is a known dump
    /// of a native DS-compatible ARM9 BIOS.
    [[nodiscard]] bool IsLoadedARM7BIOSKnownNative() const noexcept { return ARM7BIOSNative; }

    [[nodiscard]] NDSCart::CartCommon* GetNDSCart() { return NDSCartSlot.GetCart(); }
    [[nodiscard]] const NDSCart::CartCommon* GetNDSCart() const { return NDSCartSlot.GetCart(); }
    virtual void SetNDSCart(std::unique_ptr<NDSCart::CartCommon>&& cart);
    [[nodiscard]] bool CartInserted() const noexcept { return NDSCartSlot.GetCart() != nullptr; }
    virtual std::unique_ptr<NDSCart::CartCommon> EjectCart() { return NDSCartSlot.EjectCart(); }

    [[nodiscard]] u8* GetNDSSave() { return NDSCartSlot.GetSaveMemory(); }
    [[nodiscard]] const u8* GetNDSSave() const { return NDSCartSlot.GetSaveMemory(); }
    [[nodiscard]] u32 GetNDSSaveLength() const { return NDSCartSlot.GetSaveMemoryLength(); }
    void SetNDSSave(const u8* savedata, u32 savelen);

    const Firmware& GetFirmware() const { return SPI.GetFirmwareMem()->GetFirmware(); }
    Firmware& GetFirmware() { return SPI.GetFirmwareMem()->GetFirmware(); }
    void SetFirmware(Firmware&& firmware) { SPI.GetFirmwareMem()->SetFirmware(std::move(firmware)); }

    const Renderer& GetRenderer() const noexcept { return GPU.GetRenderer(); }
    Renderer& GetRenderer() noexcept { return GPU.GetRenderer(); }
    void SetRenderer(std::unique_ptr<Renderer>&& renderer) noexcept
    {
        if (renderer != nullptr)
            GPU.SetRenderer(std::move(renderer));
    }

    virtual bool NeedsDirectBoot() const;
    void SetupDirectBoot(const std::string& romname);
    virtual void SetupDirectBoot();

    [[nodiscard]] GBACart::CartCommon* GetGBACart() { return (ConsoleType == 1) ? nullptr : GBACartSlot.GetCart(); }
    [[nodiscard]] const GBACart::CartCommon* GetGBACart() const {  return (ConsoleType == 1) ? nullptr : GBACartSlot.GetCart(); }

    /// Inserts a GBA cart into the emulated console's Slot-2.
    ///
    /// @param cart The GBA cart, most likely (but not necessarily) returned from GBACart::ParseROM.
    /// To insert an accessory that doesn't use a ROM image
    /// (e.g. the Expansion Pak), create it manually and pass it here.
    /// If \c nullptr, the existing cart is ejected.
    /// If this is a DSi, this method does nothing.
    ///
    /// @post \c cart is \c nullptr and this NDS takes ownership
    /// of the cart object it held, if any.
    void SetGBACart(std::unique_ptr<GBACart::CartCommon>&& cart) { if (ConsoleType == 0) GBACartSlot.SetCart(std::move(cart)); }

    u8* GetGBASave() { return GBACartSlot.GetSaveMemory(); }
    const u8* GetGBASave() const { return GBACartSlot.GetSaveMemory(); }
    u32 GetGBASaveLength() const { return GBACartSlot.GetSaveMemoryLength(); }
    void SetGBASave(const u8* savedata, u32 savelen);

    std::unique_ptr<GBACart::CartCommon> EjectGBACart() { return GBACartSlot.EjectCart(); }

    u32 RunFrame();

    bool IsRunning() const noexcept { return Running; }

    void TouchScreen(u16 x, u16 y);
    void ReleaseScreen();

    void SetKeyMask(u32 mask);

    bool IsLidClosed() const;
    void SetLidClosed(bool closed);

    void RegisterEventFuncs(u32 id, void* that, const EventFuncList& funcs);
    void UnregisterEventFuncs(u32 id);
    void ScheduleEvent(u32 id, bool periodic, s32 delay, u32 funcid, u32 param);
    void CancelEvent(u32 id);

    void debug(u32 p);

    void Halt();

    void MapSharedWRAM(u8 val);

    void UpdateIRQ(u32 cpu);
    void SetIRQ(u32 cpu, u32 irq);
    void ClearIRQ(u32 cpu, u32 irq);
    void SetIRQ2(u32 irq);
    void ClearIRQ2(u32 irq);
    bool HaltInterrupted(u32 cpu) const;
    void StopCPU(u32 cpu, u32 mask);
    void ResumeCPU(u32 cpu, u32 mask);
    void GXFIFOStall();
    void GXFIFOUnstall();

    u32 GetPC(u32 cpu) const;
    u64 GetSysClockCycles(int num);
    void NocashPrint(u32 cpu, u32 addr, bool appendNewline = true);

    void MonitorARM9Jump(u32 addr);

    virtual bool DMAsInMode(u32 cpu, u32 mode) const;
    virtual bool DMAsRunning(u32 cpu) const;
    virtual void CheckDMAs(u32 cpu, u32 mode);
    virtual void StopDMAs(u32 cpu, u32 mode);

    void RunTimers(u32 cpu);

    virtual u8 ARM9Read8(u32 addr);
    virtual u16 ARM9Read16(u32 addr);
    virtual u32 ARM9Read32(u32 addr);
    virtual void ARM9Write8(u32 addr, u8 val);
    virtual void ARM9Write16(u32 addr, u16 val);
    virtual void ARM9Write32(u32 addr, u32 val);

    virtual bool ARM9GetMemRegion(u32 addr, bool write, MemRegion* region);

    virtual u8 ARM7Read8(u32 addr);
    virtual u16 ARM7Read16(u32 addr);
    virtual u32 ARM7Read32(u32 addr);

    // The parts of the ARM7's address space that are plain memory, as
    // a base and a mask it can index directly. See the definition at
    // the end of this header — the ARM7's fetches go through it, and
    // they are the hottest reads in the machine.
    inline bool ARM7DirectRegion(u32 addr, const u8** mem, u32* mask) const;

    // The same question for the ARM9, whose code fetches already have
    // `CodeMem` but whose loads and stores did not. Writable, so a
    // store can use it too — which is why the JIT's invalidation is
    // reported alongside rather than left behind.
    inline bool ARM9DirectRegion(u32 addr, u8** mem, u32* mask, int* jitregion);
    virtual void ARM7Write8(u32 addr, u8 val);
    virtual void ARM7Write16(u32 addr, u16 val);
    virtual void ARM7Write32(u32 addr, u32 val);

    virtual bool ARM7GetMemRegion(u32 addr, bool write, MemRegion* region);

    virtual u8 ARM9IORead8(u32 addr);
    virtual u16 ARM9IORead16(u32 addr);
    virtual u32 ARM9IORead32(u32 addr);
    virtual void ARM9IOWrite8(u32 addr, u8 val);
    virtual void ARM9IOWrite16(u32 addr, u16 val);
    virtual void ARM9IOWrite32(u32 addr, u32 val);

    virtual u8 ARM7IORead8(u32 addr);
    virtual u16 ARM7IORead16(u32 addr);
    virtual u32 ARM7IORead32(u32 addr);
    virtual void ARM7IOWrite8(u32 addr, u8 val);
    virtual void ARM7IOWrite16(u32 addr, u16 val);
    virtual void ARM7IOWrite32(u32 addr, u32 val);

#ifdef JIT_ENABLED
    [[nodiscard]] bool IsJITEnabled() const noexcept { return EnableJIT; }
    void SetJITArgs(std::optional<JITArgs> args) noexcept;
#else
    [[nodiscard]] bool IsJITEnabled() const noexcept { return false; }
    void SetJITArgs(std::optional<JITArgs> args) noexcept {}
#endif

#ifdef GDBSTUB_ENABLED
    void SetGdbArgs(std::optional<GDBArgs> args) noexcept;
#else
    void SetGdbArgs(std::optional<GDBArgs> args) noexcept {}
#endif

protected:
    void InitTimings();
    u32 SchedListMask;
    u64 SysTimestamp;
    u8 WRAMCnt = 0;
    u8 PostFlag9;
    u8 PostFlag7;
    u16 PowerControl7;
    u16 WifiWaitCnt;
    u8 TimerCheckMask[2];
    u64 TimerTimestamp[2];
    DMA DMAs[8];
    u32 DMA9Fill[4];
    u16 IPCSync9, IPCSync7;
    u16 IPCFIFOCnt9, IPCFIFOCnt7;
    FIFO<u32, 16> IPCFIFO9; // FIFO in which the ARM9 writes
    FIFO<u32, 16> IPCFIFO7;
    u16 DivCnt;
    alignas(u64) u32 DivNumerator[2];
    alignas(u64) u32 DivDenominator[2];
    alignas(u64) u32 DivQuotient[2];
    alignas(u64) u32 DivRemainder[2];
    u16 SqrtCnt;
    alignas(u64) u32 SqrtVal[2];
    u32 SqrtRes;
    u16 KeyCnt[2];
    bool Running;
    bool RunningGame;
    u64 LastSysClockCycles;
    u64 FrameStartTimestamp = 0;

    // A tick advances a fixed span of emulated time — one video frame's
    // worth of system clock — rather than "however long the next video
    // frame takes". For a console whose frames are the hardware's 263
    // lines these are the same thing, exactly: 263 lines IS that span.
    // They differ only for a console whose game stretches its LCD, and
    // there the span is the honest one — such a console produces
    // slightly fewer frames per second, which is what the hardware
    // does, rather than running its clocks fast.
    //
    // That distinction is what a linked pair rests on. A DS wireless
    // client phase-locks by writing VCOUNT, so its frames change
    // length; advanced a frame at a time it would consume more emulated
    // microseconds per tick than the host, and the two wifi clocks
    // would diverge by the frame-length ratio — a drift the client then
    // tries to correct with the very knob that causes it, so it never
    // settles. Advanced a span at a time, both consoles cover the same
    // emulated time per tick whatever either game does to its display.
    //
    // Absolute rather than a per-call duration so that the tail of a
    // frame that overran carries into the next span instead of being
    // rounded away.
    u64 SliceEnd = 0;
    // Whether the last call stopped inside a video frame, so the next
    // must resume that frame rather than start another.
    bool MidSlice = false;
    u64 NextTarget();
    u64 NextTargetSleep();
    void CheckKeyIRQ(u32 cpu, u32 oldkey, u32 newkey);
    void Reschedule(u64 target);
    void RunSystemSleep(u64 timestamp);
    void RunSystem(u64 timestamp);
    void HandleTimerOverflow(u32 tid);
    u16 TimerGetCounter(u32 timer);
    void TimerStart(u32 id, u16 cnt);
    void StartDiv();
    void DivDone(u32 param);
    void SqrtDone(u32 param);
    void StartSqrt();
    void RunTimer(u32 tid, s32 cycles);
    void UpdateWifiTimings();
    void SetWifiWaitCnt(u16 val);
    void SetExMemCnt(u32 cpu, u16 val, u16 mask);
    void SetGBASlotTimings();
    void EnterSleepMode();
    template <CPUExecuteMode cpuMode>
    u32 RunFrame();

public:
    NDS(NDSArgs&& args, void* userdata = nullptr) noexcept : NDS(std::move(args), 0, userdata) {}
    NDS() noexcept;
    virtual ~NDS() noexcept;
    NDS(const NDS&) = delete;
    NDS& operator=(const NDS&) = delete;
    NDS(NDS&&) = delete;
    NDS& operator=(NDS&&) = delete;

    static thread_local NDS* Current;
protected:
    explicit NDS(NDSArgs&& args, int type, void* userdata) noexcept;
    virtual u32 GetSavestateConfig();
    virtual void DoSavestateExtra(Savestate* file) {}
};


// ---------------------------------------------------------------------
// The per-instruction CPU interface.
//
// These are the calls an interpreted instruction makes — fetch, load,
// store, charge cycles, branch — and they were virtual, which cost an
// indirect call apiece and put the body somewhere the caller could
// never see into. They dispatch on `ARM::Num` instead: 0 is always the
// ARMv5 and 1 always the ARMv4, on a DS and on a DSi alike, so the
// branch is exact and perfectly predicted (a run of `Execute` is all
// one CPU). Behaviour is unchanged — the same function runs, reached a
// cheaper way.
//
// They live here rather than in ARM.h because the ARM7's half reads
// the NDS: its cycle table, and the memory its instructions fetch
// from.

inline void ARM::JumpTo(u32 addr, bool restorecpsr)
{
    if (Num == 0) static_cast<ARMv5*>(this)->JumpTo(addr, restorecpsr);
    else          static_cast<ARMv4*>(this)->JumpTo(addr, restorecpsr);
}

#define MELONDS_ARM_DISPATCH(ret, name, params, args)     \
    inline ret ARM::name params                           \
    {                                                     \
        if (Num == 0) return static_cast<ARMv5*>(this)->name args; \
        else          return static_cast<ARMv4*>(this)->name args; \
    }

MELONDS_ARM_DISPATCH(void, DataRead8,   (u32 addr, u32* val), (addr, val))
MELONDS_ARM_DISPATCH(void, DataRead16,  (u32 addr, u32* val), (addr, val))
MELONDS_ARM_DISPATCH(void, DataRead32,  (u32 addr, u32* val), (addr, val))
MELONDS_ARM_DISPATCH(void, DataRead32S, (u32 addr, u32* val), (addr, val))
MELONDS_ARM_DISPATCH(void, DataWrite8,   (u32 addr, u8 val),  (addr, val))
MELONDS_ARM_DISPATCH(void, DataWrite16,  (u32 addr, u16 val), (addr, val))
MELONDS_ARM_DISPATCH(void, DataWrite32,  (u32 addr, u32 val), (addr, val))
MELONDS_ARM_DISPATCH(void, DataWrite32S, (u32 addr, u32 val), (addr, val))
MELONDS_ARM_DISPATCH(void, AddCycles_C,   (),          ())
MELONDS_ARM_DISPATCH(void, AddCycles_CI,  (s32 numI),  (numI))
MELONDS_ARM_DISPATCH(void, AddCycles_CDI, (),          ())
MELONDS_ARM_DISPATCH(void, AddCycles_CD,  (),          ())

#undef MELONDS_ARM_DISPATCH

// The ARM7's cycle accounting, moved out of ARM.cpp so it can inline
// into the instruction that charges it.

inline void ARMv4::AddCycles_C()
{
    // code only. this code fetch is sequential.
    Cycles += NDS.ARM7MemTimings[CodeCycles][(CPSR&0x20)?1:3];
}

inline void ARMv4::AddCycles_CI(s32 num)
{
    // code+internal. results in a nonseq code fetch.
    Cycles += NDS.ARM7MemTimings[CodeCycles][(CPSR&0x20)?0:2] + num;
}

inline void ARMv4::AddCycles_CDI()
{
    // LDR/LDM cycles.
    s32 numC = NDS.ARM7MemTimings[CodeCycles][(CPSR&0x20)?0:2];
    s32 numD = DataCycles;

    if ((DataRegion >> 24) == 0x02) // mainRAM
    {
        if (CodeRegion == 0x02)
            Cycles += numC + numD;
        else
        {
            numC++;
            Cycles += std::max(numC + numD - 3, std::max(numC, numD));
        }
    }
    else if (CodeRegion == 0x02)
    {
        numD++;
        Cycles += std::max(numC + numD - 3, std::max(numC, numD));
    }
    else
    {
        Cycles += numC + numD + 1;
    }
}

inline void ARMv4::AddCycles_CD()
{
    // TODO: max gain should be 5c when writing to mainRAM
    s32 numC = NDS.ARM7MemTimings[CodeCycles][(CPSR&0x20)?0:2];
    s32 numD = DataCycles;

    if ((DataRegion >> 24) == 0x02)
    {
        if (CodeRegion == 0x02)
            Cycles += numC + numD;
        else
            Cycles += std::max(numC + numD - 3, std::max(numC, numD));
    }
    else if (CodeRegion == 0x02)
    {
        Cycles += std::max(numC + numD - 3, std::max(numC, numD));
    }
    else
    {
        Cycles += numC + numD;
    }
}


// The ARM7 has no cached code region the way the ARM9 has `CodeMem`
// (its mapping moves under it, so melonDS never took the ARM9's
// shortcut here), and so every one of its instruction fetches used to
// leave the CPU for `NDS::ARM7Read32` — an out-of-line virtual call
// into a switch over the whole address space, per instruction, and
// again for every load. In practice it is reading one of three plain
// arrays; the switch below says which, and the read then happens in
// the caller.
//
// A DSi answers `false` to all of it: `DSi::ARM7Read32` overrides this
// path with a different bus, and this is not the place to reimplement
// it. `ConsoleType` is a member the caller already has hot.
inline bool NDS::ARM7DirectRegion(u32 addr, const u8** mem, u32* mask) const
{
    if (ConsoleType != 0)
        return false;

    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        *mem = MainRAM;
        *mask = MainRAMMask;
        return true;

    case 0x03000000:
        if (SWRAM_ARM7.Mem)
        {
            *mem = SWRAM_ARM7.Mem;
            *mask = SWRAM_ARM7.Mask;
            return true;
        }
        [[fallthrough]];

    case 0x03800000:
        *mem = ARM7WRAM;
        *mask = ARM7WRAMSize - 1;
        return true;
    }

    // The BIOS, with the protection its reads are subject to: a fetch
    // from outside it cannot read it, and code below the protection
    // boundary is hidden from code above it. Both of those answer
    // `false` and take the long way, which is where the 0xFF..-filled
    // result comes from.
    //
    // Last, not first: the ARM7 does run BIOS code (the SWI handlers,
    // the halt loop), but a battle's fetches are overwhelmingly RAM,
    // and asking this question ahead of the switch measured 3% slower
    // across the whole tick — two extra loads and a branch on every
    // access to save a call on a few.
    if (addr < 0x00004000)
    {
        u32 pc = ARM7.R[15];
        if (pc >= 0x00004000) return false;
        if (addr < ARM7BIOSProt && pc >= ARM7BIOSProt) return false;
        *mem = ARM7BIOS.data();
        *mask = ARM7BIOSSize - 1;
        return true;
    }

    return false;
}

// The ARM9's plain-memory regions. Its I/O, palette, VRAM and OAM all
// have side effects or per-bank routing, so only main RAM and its share
// of the WRAM qualify — but between them they are nearly every load an
// instruction makes that the TCMs did not already answer.
inline bool NDS::ARM9DirectRegion(u32 addr, u8** mem, u32* mask, int* jitregion)
{
    if (ConsoleType != 0)
        return false;

    switch (addr & 0xFF000000)
    {
    case 0x02000000:
        *mem = MainRAM;
        *mask = MainRAMMask;
        *jitregion = ARMJIT_Memory::memregion_MainRAM;
        return true;

    case 0x03000000:
        if (!SWRAM_ARM9.Mem)
            return false; // reads 0, writes are dropped: not this path's business
        *mem = SWRAM_ARM9.Mem;
        *mask = SWRAM_ARM9.Mask;
        *jitregion = ARMJIT_Memory::memregion_SharedWRAM;
        return true;
    }

    return false;
}

inline u8 ARMv4::BusRead8(u32 addr)
{
    const u8* mem; u32 mask;
    if (NDS.ARM7DirectRegion(addr, &mem, &mask)) return mem[addr & mask];
    return NDS.ARM7Read8(addr);
}

inline u16 ARMv4::BusRead16(u32 addr)
{
    // The alignment mask that `NDS::ARM7Read16` applies first: below
    // 0x4000 it cannot change which side of the BIOS boundary an
    // address falls on, so applying it early is the same read.
    addr &= ~1;
    const u8* mem; u32 mask;
    if (NDS.ARM7DirectRegion(addr, &mem, &mask)) return *(const u16*)&mem[addr & mask];
    return NDS.ARM7Read16(addr);
}

inline u32 ARMv4::BusRead32(u32 addr)
{
    addr &= ~3;
    const u8* mem; u32 mask;
    if (NDS.ARM7DirectRegion(addr, &mem, &mask)) return *(const u32*)&mem[addr & mask];
    return NDS.ARM7Read32(addr);
}

// Stores take the same shortcut, minus the BIOS (which is not
// writable and whose region answers `false` for a write anyway,
// because `ARM7DirectRegion` hands back a `const` pointer). The JIT's
// invalidation has to be repeated by hand: its region is a template
// argument, so it cannot ride along in a runtime `(base, mask)`.
inline void ARMv4::BusWrite8(u32 addr, u8 val)
{
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        if (NDS.ConsoleType != 0) break;
        NDS.JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_MainRAM>(addr);
        NDS.MainRAM[addr & NDS.MainRAMMask] = val;
        return;
    }
    NDS.ARM7Write8(addr, val);
}

inline void ARMv4::BusWrite16(u32 addr, u16 val)
{
    addr &= ~1;
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        if (NDS.ConsoleType != 0) break;
        NDS.JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_MainRAM>(addr);
        *(u16*)&NDS.MainRAM[addr & NDS.MainRAMMask] = val;
        return;
    }
    NDS.ARM7Write16(addr, val);
}

inline void ARMv4::BusWrite32(u32 addr, u32 val)
{
    addr &= ~3;
    switch (addr & 0xFF800000)
    {
    case 0x02000000:
    case 0x02800000:
        if (NDS.ConsoleType != 0) break;
        NDS.JIT.CheckAndInvalidate<1, ARMJIT_Memory::memregion_MainRAM>(addr);
        *(u32*)&NDS.MainRAM[addr & NDS.MainRAMMask] = val;
        return;
    }
    NDS.ARM7Write32(addr, val);
}

// The ARM9's side of the same shortcut.
inline u32 ARMv5::BusRead32(u32 addr)
{
    u8* mem; u32 mask; int region;
    if (NDS.ARM9DirectRegion(addr & ~3, &mem, &mask, &region))
        return *(u32*)&mem[(addr & ~3) & mask];
    return NDS.ARM9Read32(addr);
}

inline u16 ARMv5::BusRead16(u32 addr)
{
    u8* mem; u32 mask; int region;
    if (NDS.ARM9DirectRegion(addr & ~1, &mem, &mask, &region))
        return *(u16*)&mem[(addr & ~1) & mask];
    return NDS.ARM9Read16(addr);
}

inline void ARMv5::BusWrite32(u32 addr, u32 val)
{
    u8* mem; u32 mask; int region;
    if (NDS.ARM9DirectRegion(addr, &mem, &mask, &region))
    {
        if (region == ARMJIT_Memory::memregion_MainRAM)
            NDS.JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_MainRAM>(addr);
        else
            NDS.JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_SharedWRAM>(addr);
        *(u32*)&mem[addr & mask] = val;
        return;
    }
    NDS.ARM9Write32(addr, val);
}


// The two CPUs' data paths, moved out of CP15.cpp and ARM.cpp so an
// instruction's load or store happens where the instruction is rather
// than behind a call.

inline void ARMv5::DataRead8(u32 addr, u32* val)
{
    if (!(PU_Map[addr>>12] & 0x01))
    {
        DataAbort();
        return;
    }

    DataRegion = addr;

    CheckWatch(addr);

    if (addr < ITCMSize)
    {
        DataCycles = 1;
        *val = *(u8*)&ITCM[addr & (ITCMPhysicalSize - 1)];
        return;
    }
    if ((addr & DTCMMask) == DTCMBase)
    {
        DataCycles = 1;
        *val = *(u8*)&DTCM[addr & (DTCMPhysicalSize - 1)];
        return;
    }

    *val = BusRead8(addr);
    DataCycles = MemTimings[addr >> 12][1];
}

inline void ARMv5::DataRead16(u32 addr, u32* val)
{
    if (!(PU_Map[addr>>12] & 0x01))
    {
        DataAbort();
        return;
    }

    DataRegion = addr;

    addr &= ~1;

    CheckWatch(addr);

    if (addr < ITCMSize)
    {
        DataCycles = 1;
        *val = *(u16*)&ITCM[addr & (ITCMPhysicalSize - 1)];
        return;
    }
    if ((addr & DTCMMask) == DTCMBase)
    {
        DataCycles = 1;
        *val = *(u16*)&DTCM[addr & (DTCMPhysicalSize - 1)];
        return;
    }

    *val = BusRead16(addr);
    DataCycles = MemTimings[addr >> 12][1];
}

inline void ARMv5::DataRead32(u32 addr, u32* val)
{
    if (!(PU_Map[addr>>12] & 0x01))
    {
        DataAbort();
        return;
    }

    DataRegion = addr;

    addr &= ~3;

    CheckWatch(addr);

    if (addr < ITCMSize)
    {
        DataCycles = 1;
        *val = *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)];
        return;
    }
    if ((addr & DTCMMask) == DTCMBase)
    {
        DataCycles = 1;
        *val = *(u32*)&DTCM[addr & (DTCMPhysicalSize - 1)];
        return;
    }

    *val = BusRead32(addr);
    DataCycles = MemTimings[addr >> 12][2];
}

inline void ARMv5::DataRead32S(u32 addr, u32* val)
{
    addr &= ~3;

    CheckWatch(addr);

    if (addr < ITCMSize)
    {
        DataCycles += 1;
        *val = *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)];
        return;
    }
    if ((addr & DTCMMask) == DTCMBase)
    {
        DataCycles += 1;
        *val = *(u32*)&DTCM[addr & (DTCMPhysicalSize - 1)];
        return;
    }

    *val = BusRead32(addr);
    DataCycles += MemTimings[addr >> 12][3];
}

inline void ARMv5::DataWrite8(u32 addr, u8 val)
{
    if (!(PU_Map[addr>>12] & 0x02))
    {
        DataAbort();
        return;
    }

    DataRegion = addr;

    if (addr < ITCMSize)
    {
        DataCycles = 1;
        *(u8*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;
        NDS.JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_ITCM>(addr);
        return;
    }
    if ((addr & DTCMMask) == DTCMBase)
    {
        DataCycles = 1;
        *(u8*)&DTCM[addr & (DTCMPhysicalSize - 1)] = val;
        return;
    }

    BusWrite8(addr, val);
    DataCycles = MemTimings[addr >> 12][1];
}

inline void ARMv5::DataWrite16(u32 addr, u16 val)
{
    if (!(PU_Map[addr>>12] & 0x02))
    {
        DataAbort();
        return;
    }

    DataRegion = addr;

    addr &= ~1;

    if (addr < ITCMSize)
    {
        DataCycles = 1;
        *(u16*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;
        NDS.JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_ITCM>(addr);
        return;
    }
    if ((addr & DTCMMask) == DTCMBase)
    {
        DataCycles = 1;
        *(u16*)&DTCM[addr & (DTCMPhysicalSize - 1)] = val;
        return;
    }

    BusWrite16(addr, val);
    DataCycles = MemTimings[addr >> 12][1];
}

inline void ARMv5::DataWrite32(u32 addr, u32 val)
{
    if (!(PU_Map[addr>>12] & 0x02))
    {
        DataAbort();
        return;
    }

    DataRegion = addr;

    addr &= ~3;

    if (addr < ITCMSize)
    {
        DataCycles = 1;
        *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;
        NDS.JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_ITCM>(addr);
        return;
    }
    if ((addr & DTCMMask) == DTCMBase)
    {
        DataCycles = 1;
        *(u32*)&DTCM[addr & (DTCMPhysicalSize - 1)] = val;
        return;
    }

    BusWrite32(addr, val);
    DataCycles = MemTimings[addr >> 12][2];
}

inline void ARMv5::DataWrite32S(u32 addr, u32 val)
{
    addr &= ~3;

    if (addr < ITCMSize)
    {
        DataCycles += 1;
        *(u32*)&ITCM[addr & (ITCMPhysicalSize - 1)] = val;
#ifdef JIT_ENABLED
        NDS.JIT.CheckAndInvalidate<0, ARMJIT_Memory::memregion_ITCM>(addr);
#endif
        return;
    }
    if ((addr & DTCMMask) == DTCMBase)
    {
        DataCycles += 1;
        *(u32*)&DTCM[addr & (DTCMPhysicalSize - 1)] = val;
        return;
    }

    BusWrite32(addr, val);
    DataCycles += MemTimings[addr >> 12][3];
}


inline void ARMv4::DataRead8(u32 addr, u32* val)
{
    CheckWatch(addr);

    *val = BusRead8(addr);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][0];
}

inline void ARMv4::DataRead16(u32 addr, u32* val)
{
    addr &= ~1;

    CheckWatch(addr);

    *val = BusRead16(addr);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][0];
}

inline void ARMv4::DataRead32(u32 addr, u32* val)
{
    addr &= ~3;

    CheckWatch(addr);

    *val = BusRead32(addr);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][2];
}

inline void ARMv4::DataRead32S(u32 addr, u32* val)
{
    addr &= ~3;

    CheckWatch(addr);

    *val = BusRead32(addr);
    DataCycles += NDS.ARM7MemTimings[addr >> 15][3];
}

inline void ARMv4::DataWrite8(u32 addr, u8 val)
{
    BusWrite8(addr, val);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][0];
}

inline void ARMv4::DataWrite16(u32 addr, u16 val)
{
    addr &= ~1;

    BusWrite16(addr, val);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][0];
}

inline void ARMv4::DataWrite32(u32 addr, u32 val)
{
    addr &= ~3;

    BusWrite32(addr, val);
    DataRegion = addr;
    DataCycles = NDS.ARM7MemTimings[addr >> 15][2];
}

inline void ARMv4::DataWrite32S(u32 addr, u32 val)
{
    addr &= ~3;

    BusWrite32(addr, val);
    DataCycles += NDS.ARM7MemTimings[addr >> 15][3];
}


}
#endif // NDS_H
