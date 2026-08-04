// The Platform:: implementation melonDS resolves at link time — this
// file takes the place of the Qt frontend's.
//
// The choices here are all in service of determinism:
//  - No file access at all. BIOS is FreeBIOS, firmware is generated,
//    the cart image and save come in as buffers, save writes go out
//    through the host vtable. Every file function fails cleanly.
//  - No wall clock. GetMSCount/GetUSCount are only referenced by
//    melonDS's own frontends (never by the core); they return 0 so any
//    accidental future use is loud rather than sneaky-nondeterministic.
//  - MP goes through the host vtable with the instance userdata, so the
//    Rust side owns packet transport and ordering entirely.

#include "instance.h"

#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "Platform.h"

using namespace melonDS;

namespace melonDS::Platform
{

// The embedder's pointer for whichever instance the core is calling
// about, out of the one it threads through every hook.
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

// The instance's own wifi clock, which is the "now" a host needs to
// decide whether a frame in the air has reached this console yet.
static u64 Now(void* userdata)
{
    return static_cast<MdsNds*>(userdata)->nds->Wifi.GetUSTimestamp();
}

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
    return g_host.mp_recv_packet(UD(userdata), data, Now(userdata), timestamp);
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
    return g_host.mp_recv_host_packet(UD(userdata), data, Now(userdata), timestamp);
}

u16 MP_RecvReplies(u8* data, u64 timestamp, u16 aidmask, void* userdata)
{
    if (!g_host.mp_recv_replies)
        return 0;
    return g_host.mp_recv_replies(UD(userdata), data, Now(userdata), timestamp, aidmask);
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
