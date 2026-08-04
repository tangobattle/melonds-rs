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

#ifndef SAVESTATE_H
#define SAVESTATE_H

#include <cstring>
#include <string>
#include <stdio.h>
#include <vector>
#include <utility>
#include "types.h"

#define SAVESTATE_MAJOR 14
// 14.1 carries the CPUs' fetch-timing scratch — see ARM::DoSavestate.
// 14.2 carries the divider and square-root registers — see NDS::DoSavestate.
// 14.3 carries the tick's span of emulated time — see NDS::SliceEnd.
// 14.4 carries the renderer's rasterized output — see Renderer::DoSavestate.
#define SAVESTATE_MINOR 5

// bitmask for the savestate config word
enum
{
    SC_Console_DSi      = (1<<0),
    SC_DSi_DSPHLE       = (1<<16),
};

namespace melonDS
{
class Savestate
{
public:
    static constexpr u32 DEFAULT_SIZE = 32 * 1024 * 1024; // 32 MB
    Savestate(void* buffer, u32 size, bool save);
    explicit Savestate(u32 initial_size = DEFAULT_SIZE);

    ~Savestate();

    bool Error;

    bool Saving;

    u32 CurSection;

    // Only move the pages of the console that have moved.
    //
    // A rollback session snapshots every tick and restores into the
    // same console it captured, so most of a state's bytes are already
    // equal on both sides of the copy. `SetDirtyPages` hands over the
    // embedder's per-page record of when each was last written and the
    // generation the buffer on the other side was filled at; a page
    // last written no later than that is identical in both, and
    // `VarArray` leaves it alone.
    //
    // Only arrays inside the watched block are eligible — anything
    // outside it (the cart's save memory, say) has no record and is
    // always copied. Set nothing and every byte moves, which is what
    // every caller but the session wants.
    void SetDirtyPages(const void* base, u32 size, const u32* pageGen, u32 sinceGen)
    {
        WatchBase = (const u8*)base;
        WatchSize = size;
        PageGen = pageGen;
        SinceGen = sinceGen;
    }

    void Section(const char* magic);

    void Var8(u8* var)
    {
        VarSmall(var, sizeof(*var));
    }

    void Var16(u16* var)
    {
        VarSmall(var, sizeof(*var));
    }

    void Var32(u32* var)
    {
        VarSmall(var, sizeof(*var));
    }

    void Var64(u64* var)
    {
        VarSmall(var, sizeof(*var));
    }

    // The inlined all-fits fast path of VarArray. Sections like the 3D
    // geometry state serialize hundreds of thousands of small fields;
    // taking the out-of-line general path for each of them dominates
    // whole-console savestate time.
    void VarSmall(void* data, u32 len)
    {
        if (Error || finished) return;
        if (buffer_offset + len > buffer_length)
        {
            // Doesn't fit: grow (saving) or fail (loading) — the
            // general path handles both.
            VarArray(data, len);
            return;
        }
        if (Saving)
            memcpy(buffer + buffer_offset, data, len);
        else
            memcpy(data, buffer + buffer_offset, len);
        buffer_offset += len;
    }

    void VarBool(bool* var);
    void Bool32(bool* var); // backwards compatibility (TODO remove)

    void VarArray(void* data, u32 len);

    // Declare a block worth watching that this state never hands to
    // `VarArray`. The 3D engine's polygon RAM is the one that matters:
    // it is packed a record at a time out of a struct full of pointers,
    // so the copy a dirty record could avoid has to be asked for by
    // name.
    void TrackBulk(const void* data, u32 len)
    {
        if (BulkArrays)
            BulkArrays->emplace_back(data, len);
    }

    // Whether [data, data+len) is untouched since the buffer on the
    // other side of this state was filled — so both hold the same bytes
    // and neither needs writing. Answers false without a record, which
    // is what makes every caller safe by default.
    bool Clean(const void* data, u32 len) const
    {
        const u8* at = (const u8*)data;
        if (!WatchBase || at < WatchBase || at + len > WatchBase + WatchSize)
            return false;
        const u32 last = (u32)(at + len - 1 - WatchBase) >> 12;
        for (u32 page = (u32)(at - WatchBase) >> 12; page <= last; page++)
            if (PageGen[page] > SinceGen)
                return false;
        return true;
    }

    // Step the stream over bytes that are already equal on both sides.
    // Only a caller that has asked `Clean` first may do this.
    void Skip(u32 len)
    {
        if (Error || finished) return;
        if (buffer_offset + len <= buffer_length)
            buffer_offset += len;
        else
            Error = true;
    }

    // The copy VarArray makes, minus the pages the dirty record proves
    // are already equal. See the definition.
    void MoveArray(void* dst, const void* src, const void* tracked, u32 len);

    // Collect the bulk arrays this state moves, so the embedder can
    // watch their pages and no others: asking the kernel about a
    // console's whole allocation costs more than the copy it saves,
    // and all but a few percent of that allocation is never serialized.
    // Only arrays worth watching are reported — the rest are a scalar
    // or two and always move.
    static constexpr u32 BULK_ARRAY = 64 * 1024;
    void RecordBulkArrays(std::vector<std::pair<const void*, u32>>* out) { BulkArrays = out; }

    void Finish();

    // TODO rewinds the stream
    void Rewind(bool save);

    bool IsAtLeastVersion(u32 major, u32 minor)
    {
        u16 major_version = MajorVersion();
        if (MajorVersion() > major) return true;
        if (major_version == major && MinorVersion() >= minor) return true;
        return false;
    }

    void* Buffer() { return buffer; }
    [[nodiscard]] const void* Buffer() const { return buffer; }

    [[nodiscard]] u32 BufferLength() const { return buffer_length; }

    [[nodiscard]] u32 Length() const { return buffer_offset; }

    [[nodiscard]] u16 MajorVersion() const
    {
        // major version is stored at offset 0x04
        u16 major = 0;
        memcpy(&major, buffer + 0x04, sizeof(major));
        return major;
    }

    [[nodiscard]] u16 MinorVersion() const
    {
        // minor version is stored at offset 0x06
        u16 minor = 0;
        memcpy(&minor, buffer + 0x06, sizeof(minor));
        return minor;
    }

private:
    // The dirty record `SetDirtyPages` installs; null base means every
    // byte moves.
    const u8* WatchBase = nullptr;
    u32 WatchSize = 0;
    const u32* PageGen = nullptr;
    u32 SinceGen = 0;
    std::vector<std::pair<const void*, u32>>* BulkArrays = nullptr;

    static constexpr u32 NO_SECTION = 0xffffffff;
    void CloseCurrentSection();
    bool Resize(u32 new_length);
    void WriteSavestateHeader();
    void WriteStateLength();
    u32 FindSection(const char* magic) const;
    u8* buffer;
    u32 buffer_offset;
    u32 buffer_length;
    bool buffer_owned;
    bool finished;
};
}

#endif // SAVESTATE_H
