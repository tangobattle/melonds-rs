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

#ifndef MIC_H
#define MIC_H

#include "Savestate.h"
#include "Platform.h"

namespace melonDS
{
class NDS;

enum MicSource
{
    Mic_NDS = 0,        // NDS mic (TSC AUX channel)
    Mic_DSi,            // DSi mic (0x04004600)
    Mic_DSi_DSP         // DSi mic (DSP BTDMP)
};

class Mic
{
public:
    explicit Mic(melonDS::NDS& nds);
    ~Mic();
    void Reset();
    void DoSavestate(Savestate* file);

    void Start(MicSource source);
    void Stop(MicSource source);
    void StopAll();

    // Hold full-scale white noise on the mic in place of the platform's
    // input, for as long as the frontend asks for it. Set per frame like
    // the key mask, and not savestated for the same reason: it is an
    // input, so a rewound console gets it handed back before it runs.
    void SetStaticInput(bool on) { StaticInput = on; }

    void Advance(u32 cycles);
    s16 ReadSample();

private:
    melonDS::NDS& NDS;

    static const u32 InputBufferSize = 2*1024;
    s16 InputBuffer[InputBufferSize] {};
    u32 InputBufferWritePos = 0;
    u32 InputBufferReadPos = 0;
    u32 InputBufferLevel = 0;

    u8 OpenMask;
    u32 CycleCount;
    s16 CurSample;
    u8 StopMask;
    u32 StopCount[3];

    bool StaticInput = false;
    // The static's generator. Unlike the platform's sample buffer this
    // *is* savestated: a console that hears static is reading it every
    // time it samples the AUX channel, so where the sequence had got to
    // is console state, and a restore that lost it would leave two
    // machines running the same inputs hearing different noise.
    u32 NoiseState;

    void DoStop(MicSource source);
    void FeedBuffer();
};

}

#endif // MIC_H
