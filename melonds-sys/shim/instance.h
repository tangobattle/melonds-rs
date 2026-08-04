// What the two halves of the embedder share.
//
// `shim.cpp` is the C ABI melonds-sys calls; `platform.cpp` is the
// Platform:: implementation melonDS resolves at link time, in place of
// the Qt frontend's. Between them there is only this: what an instance
// is, and where the host's hooks live.
#ifndef MELONDS_SHIM_INSTANCE_H
#define MELONDS_SHIM_INSTANCE_H

#include "shim.h"

// Must precede every melonDS header (it de-GCCs them under MSVC).
#include "msvc_compat.h"

#include "ConsoleMemory.h"
#include "NDS.h"

// One emulated DS. The core carries a pointer to this through every
// Platform call it makes as `userdata`, which is how a process-global
// hook table reaches the one host that asked for the call.
struct MdsNds
{
    // Owns the console and the record of which of its pages have moved.
    melonDS::ConsoleMemory memory;
    // The console itself, for the forwarders; `memory` outlives it.
    melonDS::NDS* nds = nullptr;
    // The embedder's pointer for this instance, handed to every hook.
    void* userdata = nullptr;
};

// The host hooks, installed once by mds_set_host_vtable.
extern MdsHostVtable g_host;

#endif // MELONDS_SHIM_INSTANCE_H
