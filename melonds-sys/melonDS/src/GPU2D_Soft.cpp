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

#include "GPU_Soft.h"
#include "GPU_ColorOp.h"

namespace melonDS
{

SoftRenderer2D::SoftRenderer2D(melonDS::GPU2D& gpu2D, SoftRenderer& parent)
    : Renderer2D(gpu2D), Parent(parent)
{
    // mosaic table is initialized at compile-time
}

SoftRenderer2D::~SoftRenderer2D()
{
}

void SoftRenderer2D::Reset()
{
    memset(BGOBJLine, 0, sizeof(BGOBJLine));
    memset(WindowMask, 0, sizeof(WindowMask));
    memset(OBJLine, 0, sizeof(OBJLine));
    memset(OBJWindow, 0, sizeof(OBJWindow));

    NumSprites = 0;
    SpritePrioOnLine = 0;
    SpriteBlendOnLine = false;
    SpriteXMin = 256;
    SpriteXMax = 0;
}

u32 SoftRenderer2D::ColorComposite(int i, u32 val1, u32 val2) const
{
    u32 coloreffect = 0;
    u32 eva, evb;

    u32 flag1 = val1 >> 24;
    u32 flag2 = val2 >> 24;

    u32 blendCnt = GPU2D.BlendCnt;

    u32 target2;
    if      (flag2 & 0x80) target2 = 0x1000;
    else if (flag2 & 0x40) target2 = 0x0100;
    else                   target2 = flag2 << 8;

    if ((flag1 & 0x80) && (blendCnt & target2))
    {
        // sprite blending

        coloreffect = 1;

        if (flag1 & 0x40)
        {
            eva = flag1 & 0x1F;
            evb = 16 - eva;
        }
        else
        {
            eva = GPU2D.EVA;
            evb = GPU2D.EVB;
        }
    }
    else if ((flag1 & 0x40) && (blendCnt & target2))
    {
        // 3D layer blending

        coloreffect = 4;
    }
    else
    {
        if      (flag1 & 0x80) flag1 = 0x10;
        else if (flag1 & 0x40) flag1 = 0x01;

        if ((blendCnt & flag1) && (WindowMask[i] & 0x20))
        {
            coloreffect = (blendCnt >> 6) & 0x3;

            if (coloreffect == 1)
            {
                if (blendCnt & target2)
                {
                    eva = GPU2D.EVA;
                    evb = GPU2D.EVB;
                }
                else
                    coloreffect = 0;
            }
        }
    }

    switch (coloreffect)
    {
        case 0: return val1;
        case 1: return ColorBlend4(val1, val2, eva, evb);
        case 2: return ColorBrightnessUp(val1, GPU2D.EVY, 0x8);
        case 3: return ColorBrightnessDown(val1, GPU2D.EVY, 0x7);
        case 4: return ColorBlend5(val1, val2);
    }

    return val1;
}

void SoftRenderer2D::DrawScanline(u32 line)
{
    u32* dst = Parent.Output2D[GPU2D.Num];

    if (!GPU2D.Enabled)
    {
        // if this 2D unit is disabled in POWCNT, the output is a fixed color
        // (black for unit A, white for unit B)
        u32 fillcolor = (GPU2D.Num == 0) ? 0xFF000000 : 0xFF3F3F3F;
        for (int i = 0; i < 256; i++)
            dst[i] = fillcolor;

        return;
    }

    if (GPU2D.ForcedBlank)
    {
        // forced blank
        for (int i = 0; i < 256; i++)
            dst[i] = 0xFF3F3F3F;

        return;
    }

    if (GPU2D.Num == 0)
    {
        auto bgDirty = GPU.VRAMDirty_ABG.DeriveState(GPU.VRAMMap_ABG, GPU);
        GPU.MakeVRAMFlat_ABGCoherent(bgDirty);
        auto bgExtPalDirty = GPU.VRAMDirty_ABGExtPal.DeriveState(GPU.VRAMMap_ABGExtPal, GPU);
        GPU.MakeVRAMFlat_ABGExtPalCoherent(bgExtPalDirty);
        auto objExtPalDirty = GPU.VRAMDirty_AOBJExtPal.DeriveState(&GPU.VRAMMap_AOBJExtPal, GPU);
        GPU.MakeVRAMFlat_AOBJExtPalCoherent(objExtPalDirty);
    }
    else
    {
        auto bgDirty = GPU.VRAMDirty_BBG.DeriveState(GPU.VRAMMap_BBG, GPU);
        GPU.MakeVRAMFlat_BBGCoherent(bgDirty);
        auto bgExtPalDirty = GPU.VRAMDirty_BBGExtPal.DeriveState(GPU.VRAMMap_BBGExtPal, GPU);
        GPU.MakeVRAMFlat_BBGExtPalCoherent(bgExtPalDirty);
        auto objExtPalDirty = GPU.VRAMDirty_BOBJExtPal.DeriveState(&GPU.VRAMMap_BOBJExtPal, GPU);
        GPU.MakeVRAMFlat_BOBJExtPalCoherent(objExtPalDirty);
    }

    // render BG layers and sprites
    DrawScanline_BGOBJ(line, dst);
}

#define DoDrawBG(type, line, num) \
    do \
    { \
        if ((bgCnt[num] & (1<<6)) && (GPU2D.BGMosaicSize[0] > 0)) \
        { \
            DrawBG_##type<true>(line, num); \
        } \
        else \
        { \
            DrawBG_##type<false>(line, num); \
        } \
    } while (false)

#define DoDrawBG_Large(line) \
    do \
    { \
        if ((bgCnt[2] & (1<<6)) && (GPU2D.BGMosaicSize[0] > 0)) \
        { \
            DrawBG_Large<true>(line); \
        } \
        else \
        { \
            DrawBG_Large<false>(line); \
        } \
    } while (false)

template<u32 bgmode>
void SoftRenderer2D::DrawScanlineBGMode(u32 line)
{
    u32 dispCnt = GPU2D.DispCnt;
    u16* bgCnt = GPU2D.BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[3] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<3))
            {
                if (bgmode >= 3)
                    DoDrawBG(Extended, line, 3);
                else if (bgmode >= 1)
                    DoDrawBG(Affine, line, 3);
                else
                    DoDrawBG(Text, line, 3);
            }
        }
        if ((bgCnt[2] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<2))
            {
                if (bgmode == 5)
                    DoDrawBG(Extended, line, 2);
                else if (bgmode == 4 || bgmode == 2)
                    DoDrawBG(Affine, line, 2);
                else
                    DoDrawBG(Text, line, 2);
            }
        }
        if ((bgCnt[1] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<1))
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<0))
            {
                if (!GPU2D.Num && (dispCnt & 0x8))
                    DrawBG_3D();
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((GPU2D.LayerEnable & (1<<4)) && (SpritePrioOnLine & (1<<i)))
        {
            InterleaveSprites(i);
        }

    }
}

void SoftRenderer2D::DrawScanlineBGMode6(u32 line)
{
    u32 dispCnt = GPU2D.DispCnt;
    u16* bgCnt = GPU2D.BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[2] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<2))
            {
                DoDrawBG_Large(line);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<0))
            {
                if ((!GPU2D.Num) && (dispCnt & 0x8))
                    DrawBG_3D();
            }
        }
        if ((GPU2D.LayerEnable & (1<<4)) && (SpritePrioOnLine & (1<<i)))
        {
            InterleaveSprites(i);
        }
    }
}

void SoftRenderer2D::DrawScanlineBGMode7(u32 line)
{
    u32 dispCnt = GPU2D.DispCnt;
    u16* bgCnt = GPU2D.BGCnt;
    // mode 7 only has text-mode BG0 and BG1

    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[1] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<1))
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<0))
            {
                if (!GPU2D.Num && (dispCnt & 0x8))
                    DrawBG_3D();
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((GPU2D.LayerEnable & (1<<4)) && (SpritePrioOnLine & (1<<i)))
        {
            InterleaveSprites(i);
        }
    }
}

    // Whether the color-effect pass can touch any pixel on this line.
    // BLDCNT alone decides for the effect modes; sprite-forced and
    // 3D-forced blending additionally need such a top pixel to exist,
    // which the sprite pass has already ruled on for this line. When
    // nothing can fire, the composite is a straight copy of the top
    // layer and the bottom layer is never even read.
static bool CompositeIsIdentity(u32 blendCnt, bool spriteBlend, bool has3D)
{
    u32 mode = (blendCnt >> 6) & 0x3;
    bool modesIdle = (mode == 0)
        || (mode == 1 && (blendCnt & 0x3F00) == 0)
        || (mode >= 2 && (blendCnt & 0x3F) == 0);
    return modesIdle && (((blendCnt & 0x3F00) == 0) || (!spriteBlend && !has3D));
}

void SoftRenderer2D::DrawScanline_BGOBJ(u32 line, u32* dst)
{
    bool has3d = (GPU2D.Num == 0) && (GPU2D.DispCnt & 0x8) && (GPU2D.LayerEnable & 0x1);
    bool identity = CompositeIsIdentity(GPU2D.BlendCnt, SpriteBlendOnLine, has3d);

    u64 backdrop;
    if (GPU2D.Num)
        backdrop = *(u16*)&GPU.Palette[0x400];
    else
        backdrop = *(u16*)&GPU.Palette[0];

    {
        u8 r = (backdrop & 0x001F) << 1;
        u8 g = ((backdrop & 0x03E0) >> 4) | ((backdrop & 0x8000) >> 15);
        u8 b = (backdrop & 0x7C00) >> 9;

        backdrop = r | (g << 8) | (b << 16) | 0x20000000;
        backdrop |= (backdrop << 32);

        for (int i = 0; i < 256; i+=2)
            *(u64*)&BGOBJLine[i] = backdrop;
        if (!identity)
            for (int i = 256; i < 512; i+=2)
                *(u64*)&BGOBJLine[i] = 0;
    }

    if (GPU2D.DispCnt & 0xE000)
        GPU2D.CalculateWindowMask(WindowMask, OBJWindow);
    else
        memset(WindowMask, 0xFF, 256);

    ApplySpriteMosaicX();
    CurBGXMosaicTable = MosaicTable[GPU2D.BGMosaicSize[0]].data();

    switch (GPU2D.DispCnt & 0x7)
    {
        case 0: DrawScanlineBGMode<0>(line); break;
        case 1: DrawScanlineBGMode<1>(line); break;
        case 2: DrawScanlineBGMode<2>(line); break;
        case 3: DrawScanlineBGMode<3>(line); break;
        case 4: DrawScanlineBGMode<4>(line); break;
        case 5: DrawScanlineBGMode<5>(line); break;
        case 6: DrawScanlineBGMode6(line); break;
        case 7: DrawScanlineBGMode7(line); break;
    }

    // color special effects
    if (identity)
    {
        memcpy(dst, BGOBJLine, 256 * sizeof(u32));
        return;
    }

    for (int i = 0; i < 256; i++)
    {
        u32 val1 = BGOBJLine[i];
        u32 val2 = BGOBJLine[256+i];

        dst[i] = ColorComposite(i, val1, val2);
    }
}


void SoftRenderer2D::DrawPixel(u32* dst, u16 color, u32 flag)
{
    u8 r = (color & 0x001F) << 1;
    u8 g = ((color & 0x03E0) >> 4) | ((color & 0x8000) >> 15);
    u8 b = (color & 0x7C00) >> 9;

    *(dst+256) = *dst;
    *dst = r | (g << 8) | (b << 16) | flag;
}

void SoftRenderer2D::DrawBG_3D()
{
    for (int i = 0; i < 256; i++)
    {
        u32 c = Parent.Output3D[i];

        if ((c >> 24) == 0) continue;
        if (!(WindowMask[i] & 0x01)) continue;

        BGOBJLine[i+256] = BGOBJLine[i];
        BGOBJLine[i] = c | 0x40000000;
    }
}

template<bool mosaic>
void SoftRenderer2D::DrawBG_Text(u32 line, u32 bgnum)
{
    if constexpr (!mosaic)
        return DrawBG_Text_Fast(line, bgnum);

    // workaround for backgrounds missing on aarch64 with lto build
#if defined(__GNUC__) || defined(__clang__)
    asm volatile ("" : : : "memory");
#endif

    u16 bgcnt = GPU2D.BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal, extpalslot;

    u16 xoff = GPU2D.BGXPos[bgnum];
    u16 yoff = GPU2D.BGYPos[bgnum];

    if (bgcnt & (1<<6))
        yoff += GPU2D.BGMosaicLine;
    else
        yoff += line;
    /*u16 yoff = GPU2D.BGYPos[bgnum] + line;

    if (bgcnt & (1<<6))
    {
        // vertical mosaic
        yoff -= GPU2D.BGMosaicY;
    }*/

    u32 widexmask = (bgcnt & (1<<14)) ? 0x100 : 0;

    extpal = (GPU2D.DispCnt & (1<<30));
    if (extpal) extpalslot = ((bgnum<2) && (bgcnt&0x2000)) ? (2+bgnum) : bgnum;

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);
    if (GPU2D.Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0x400];
    }
    else
    {
        tilesetaddr = ((GPU2D.DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((GPU2D.DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0];
    }

    // adjust Y position in tilemap
    if (bgcnt & (1<<15))
    {
        tilemapaddr += ((yoff & 0x1F8) << 3);
        if (bgcnt & (1<<14))
            tilemapaddr += ((yoff & 0x100) << 3);
    }
    else
        tilemapaddr += ((yoff & 0xF8) << 3);

    u16 curtile;
    u16* curpal;
    u32 pixelsaddr;
    u8 color;
    u32 lastxpos;

    if (bgcnt & (1<<7))
    {
        // 256-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3)) & bgvrammask];

            if (extpal) curpal = GPU2D.GetBGExtPal(extpalslot, curtile>>12);
            else        curpal = pal;

            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                     + (((curtile & (1<<11)) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];

                if (extpal) curpal = GPU2D.GetBGExtPal(extpalslot, curtile>>12);
                else        curpal = pal;

                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                         + (((curtile & (1<<11)) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & (1<<10)) ? (7-(xpos&0x7)) : (xpos&0x7);
                color = bgvram[(pixelsaddr + tilexoff) & bgvrammask];

                if (color)
                    DrawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
    else
    {
        // 16-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[((tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3))) & bgvrammask];
            curpal = pal + ((curtile & 0xF000) >> 8);
            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                     + (((curtile & (1<<11)) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];
                curpal = pal + ((curtile & 0xF000) >> 8);
                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                         + (((curtile & (1<<11)) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & (1<<10)) ? (7-(xpos&0x7)) : (xpos&0x7);
                if (tilexoff & 0x1)
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] >> 4;
                }
                else
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] & 0x0F;
                }

                if (color)
                    DrawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
}

// The non-mosaic text BG, drawn a tile row at a time: one load fetches
// the row's eight pixel indices, a fully transparent row is skipped
// outright — most cels on most layers — and an opaque one is walked
// out of a register instead of through eight masked VRAM reads. Pixel
// output is bit-identical to the per-pixel path above.
void SoftRenderer2D::DrawBG_Text_Fast(u32 line, u32 bgnum)
{
    u16 bgcnt = GPU2D.BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal, extpalslot;

    u16 xoff = GPU2D.BGXPos[bgnum];
    u16 yoff = GPU2D.BGYPos[bgnum];

    // Mosaic enabled with a zero mosaic size lands here too, and still
    // takes its Y from the latched mosaic line.
    if (bgcnt & (1<<6))
        yoff += GPU2D.BGMosaicLine;
    else
        yoff += line;

    u32 widexmask = (bgcnt & (1<<14)) ? 0x100 : 0;

    extpal = (GPU2D.DispCnt & (1<<30));
    if (extpal) extpalslot = ((bgnum<2) && (bgcnt&0x2000)) ? (2+bgnum) : bgnum;

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);
    if (GPU2D.Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0x400];
    }
    else
    {
        tilesetaddr = ((GPU2D.DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((GPU2D.DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0];
    }

    // adjust Y position in tilemap
    if (bgcnt & (1<<15))
    {
        tilemapaddr += ((yoff & 0x1F8) << 3);
        if (bgcnt & (1<<14))
            tilemapaddr += ((yoff & 0x100) << 3);
    }
    else
        tilemapaddr += ((yoff & 0xF8) << 3);

    u32 yrow = yoff & 0x7;
    u32 flag = 0x01000000 << bgnum;
    u8 winbit = 1 << bgnum;

    if (bgcnt & (1<<7))
    {
        // 256-color: a tile row is eight bytes. Both the tileset base
        // and the row offset are multiples of eight, so the masked
        // eight-byte load can't straddle the VRAM wrap the per-pixel
        // path's masking expresses.
        int i = 0;
        while (i < 256)
        {
            u32 xpos = xoff + (u32)i;
            u32 tx = xpos & 0x7;
            int len = 8 - (int)tx;
            if (i + len > 256) len = 256 - i;

            u16 curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];
            u32 pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                         + (((curtile & (1<<11)) ? (7-yrow) : yrow) << 3);
            u64 row = *(u64*)&bgvram[pixelsaddr & bgvrammask];
            if (row == 0)
            {
                i += len;
                continue;
            }
            if (curtile & (1<<10)) // xflip
                row = __builtin_bswap64(row);

            u16* curpal;
            if (extpal) curpal = GPU2D.GetBGExtPal(extpalslot, curtile>>12);
            else        curpal = pal;

            row >>= (tx << 3);
            u32* first = &BGOBJLine[i];
            u32* second = first + 256;
            for (int k = 0; k < len; k++, row >>= 8)
            {
                u8 color = (u8)row;
                bool draw = color && (WindowMask[i+k] & winbit);
                u16 c = curpal[color];
                u32 nf = ((c & 0x001F) << 1) | (((c & 0x03E0) >> 4) << 8)
                       | ((c & 0x8000) ? 0x100 : 0) | (((c & 0x7C00) >> 9) << 16) | flag;
                u32 f = first[k];
                second[k] = draw ? f : second[k];
                first[k] = draw ? nf : f;
            }
            i += len;
        }
    }
    else
    {
        // 16-color: a tile row is four bytes of nibbles, low nibble
        // leftmost; alignment argument as above, in fours.
        int i = 0;
        while (i < 256)
        {
            u32 xpos = xoff + (u32)i;
            u32 tx = xpos & 0x7;
            int len = 8 - (int)tx;
            if (i + len > 256) len = 256 - i;

            u16 curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];
            u32 pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                         + (((curtile & (1<<11)) ? (7-yrow) : yrow) << 2);
            u32 row = *(u32*)&bgvram[pixelsaddr & bgvrammask];
            if (row == 0)
            {
                i += len;
                continue;
            }
            if (curtile & (1<<10)) // xflip: reverse the eight nibbles
            {
                row = __builtin_bswap32(row);
                row = ((row & 0x0F0F0F0F) << 4) | ((row >> 4) & 0x0F0F0F0F);
            }

            u16* curpal = pal + ((curtile & 0xF000) >> 8);

            row >>= (tx << 2);
            u32* first = &BGOBJLine[i];
            u32* second = first + 256;
            for (int k = 0; k < len; k++, row >>= 4)
            {
                u8 color = row & 0xF;
                bool draw = color && (WindowMask[i+k] & winbit);
                u16 c = curpal[color];
                u32 nf = ((c & 0x001F) << 1) | (((c & 0x03E0) >> 4) << 8)
                       | ((c & 0x8000) ? 0x100 : 0) | (((c & 0x7C00) >> 9) << 16) | flag;
                u32 f = first[k];
                second[k] = draw ? f : second[k];
                first[k] = draw ? nf : f;
            }
            i += len;
        }
    }
}

template<bool mosaic>
void SoftRenderer2D::DrawBG_Affine(u32 line, u32 bgnum)
{
    u16 bgcnt = GPU2D.BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;

    u32 coordmask;
    u32 yshift;
    switch ((bgcnt >> 14) & 0x3)
    {
        case 0: coordmask = 0x07800; yshift = 7; break;
        case 1: coordmask = 0x0F800; yshift = 8; break;
        case 2: coordmask = 0x1F800; yshift = 9; break;
        case 3: coordmask = 0x3F800; yshift = 10; break;
    }

    u32 overflowmask;
    if (bgcnt & (1<<13)) overflowmask = 0;
    else                 overflowmask = ~(coordmask | 0x7FF);

    s16 rotA = GPU2D.BGRotA[bgnum-2];
    s16 rotC = GPU2D.BGRotC[bgnum-2];

    s32 rotX = GPU2D.BGXRefInternal[bgnum-2];
    s32 rotY = GPU2D.BGYRefInternal[bgnum-2];

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);

    if (GPU2D.Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0x400];
    }
    else
    {
        tilesetaddr = ((GPU2D.DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((GPU2D.DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0];
    }

    u16 curtile;
    u8 color;

    yshift -= 3;

    for (int i = 0; i < 256; i++)
    {
        if (WindowMask[i] & (1<<bgnum))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if ((!((finalX|finalY) & overflowmask)))
            {
                curtile = bgvram[(tilemapaddr + ((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11))) & bgvrammask];

                // draw pixel
                u32 tilexoff = (finalX >> 8) & 0x7;
                u32 tileyoff = (finalY >> 8) & 0x7;

                color = bgvram[(tilesetaddr + (curtile << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                if (color)
                    DrawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }
}

template<bool mosaic>
void SoftRenderer2D::DrawBG_Extended(u32 line, u32 bgnum)
{
    u16 bgcnt = GPU2D.BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal;

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);

    extpal = (GPU2D.DispCnt & (1<<30));

    s16 rotA = GPU2D.BGRotA[bgnum-2];
    s16 rotC = GPU2D.BGRotC[bgnum-2];

    s32 rotX = GPU2D.BGXRefInternal[bgnum-2];
    s32 rotY = GPU2D.BGYRefInternal[bgnum-2];

    if (bgcnt & (1<<7))
    {
        // bitmap modes

        u32 xmask, ymask;
        u32 yshift;
        switch ((bgcnt >> 14) & 0x3)
        {
            case 0: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;
            case 1: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;
            case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
            case 3: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
        }

        u32 ofxmask, ofymask;
        if (bgcnt & (1<<13))
        {
            ofxmask = 0;
            ofymask = 0;
        }
        else
        {
            ofxmask = ~xmask;
            ofymask = ~ymask;
        }

        tilemapaddr = ((bgcnt & 0x1F00) << 6);

        if (bgcnt & (1<<2))
        {
            // direct color bitmap

            u16 color;

            for (int i = 0; i < 256; i++)
            {
                if (WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = *(u16*)&bgvram[(tilemapaddr + (((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) << 1)) & bgvrammask];

                        if (color & 0x8000)
                            DrawPixel(&BGOBJLine[i], color & 0x7FFF, 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
        else
        {
            // 256-color bitmap

            if (GPU2D.Num) pal = (u16*)&GPU.Palette[0x400];
            else           pal = (u16*)&GPU.Palette[0];

            u8 color;

            for (int i = 0; i < 256; i++)
            {
                if (WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = bgvram[(tilemapaddr + (((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                        if (color)
                            DrawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
    }
    else
    {
        // mixed affine/text mode

        u32 coordmask;
        u32 yshift;
        switch ((bgcnt >> 14) & 0x3)
        {
            case 0: coordmask = 0x07800; yshift = 7; break;
            case 1: coordmask = 0x0F800; yshift = 8; break;
            case 2: coordmask = 0x1F800; yshift = 9; break;
            case 3: coordmask = 0x3F800; yshift = 10; break;
        }

        u32 overflowmask;
        if (bgcnt & (1<<13)) overflowmask = 0;
        else                 overflowmask = ~(coordmask | 0x7FF);

        if (GPU2D.Num)
        {
            tilesetaddr = ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((bgcnt & 0x1F00) << 3);

            pal = (u16*)&GPU.Palette[0x400];
        }
        else
        {
            tilesetaddr = ((GPU2D.DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((GPU2D.DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

            pal = (u16*)&GPU.Palette[0];
        }

        u16 curtile;
        u16* curpal;
        u8 color;

        yshift -= 3;

        for (int i = 0; i < 256; i++)
        {
            if (WindowMask[i] & (1<<bgnum))
            {
                s32 finalX, finalY;
                if (mosaic)
                {
                    int im = CurBGXMosaicTable[i];
                    finalX = rotX - (im * rotA);
                    finalY = rotY - (im * rotC);
                }
                else
                {
                    finalX = rotX;
                    finalY = rotY;
                }

                if ((!((finalX|finalY) & overflowmask)))
                {
                    curtile = *(u16*)&bgvram[(tilemapaddr + (((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11)) << 1)) & bgvrammask];

                    if (extpal) curpal = GPU2D.GetBGExtPal(bgnum, curtile>>12);
                    else        curpal = pal;

                    // draw pixel
                    u32 tilexoff = (finalX >> 8) & 0x7;
                    u32 tileyoff = (finalY >> 8) & 0x7;

                    if (curtile & (1<<10)) tilexoff = 7-tilexoff;
                    if (curtile & (1<<11)) tileyoff = 7-tileyoff;

                    color = bgvram[(tilesetaddr + ((curtile & 0x03FF) << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                    if (color)
                        DrawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
                }
            }

            rotX += rotA;
            rotY += rotC;
        }
    }
}

template<bool mosaic>
void SoftRenderer2D::DrawBG_Large(u32 line) // BG is always BG2
{
    u16 bgcnt = GPU2D.BGCnt[2];

    u16* pal;

    // large BG sizes:
    // 0: 512x1024
    // 1: 1024x512
    // 2: 512x256
    // 3: 512x512
    u32 xmask, ymask;
    u32 yshift;
    switch ((bgcnt >> 14) & 0x3)
    {
        case 0: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;
        case 1: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;
        case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
        case 3: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
    }

    u32 ofxmask, ofymask;
    if (bgcnt & (1<<13))
    {
        ofxmask = 0;
        ofymask = 0;
    }
    else
    {
        ofxmask = ~xmask;
        ofymask = ~ymask;
    }

    s16 rotA = GPU2D.BGRotA[0];
    s16 rotC = GPU2D.BGRotC[0];

    s32 rotX = GPU2D.BGXRefInternal[0];
    s32 rotY = GPU2D.BGYRefInternal[0];

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);

    // 256-color bitmap

    if (GPU2D.Num) pal = (u16*)&GPU.Palette[0x400];
    else           pal = (u16*)&GPU.Palette[0];

    u8 color;

    for (int i = 0; i < 256; i++)
    {
        if (WindowMask[i] & (1<<2))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if (!(finalX & ofxmask) && !(finalY & ofymask))
            {
                color = bgvram[((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                if (color)
                    DrawPixel(&BGOBJLine[i], pal[color], 0x01000000<<2);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }
}


void SoftRenderer2D::ApplySpriteMosaicX()
{
    /*
     * apply X mosaic if needed
     * X mosaic for sprites is applied after all sprites are rendered
     *
     * rules:
     * pixels are processed from left to right
     * current pixel value is latched if:
     * - the X mosaic counter is 0
     * - the current pixel doesn't receive sprite mosaic
     * - the current pixel receives sprite mosaic and the previous one didn't, or vice versa
     * - the current BG-relative priority value is lower than the previous one
     */

    u8 mosw = GPU2D.OBJMosaicSize[0];
    if (mosw == 0) return;
    // No sprite on this line can produce an opaque pixel — latching
    // would only shuffle values InterleaveSprites never reads.
    if (!SpritePrioOnLine) return;

    u8 mosx = 0;
    u32 latchcolor;
    for (int i = 0; i < 256; i++)
    {
        u32 curcolor = OBJLine[i];
        bool latch = false;

        if (mosx == 0)
            latch = true;
        else if (!(curcolor & OBJ_Mosaic))
            latch = true;
        else if (!(latchcolor & OBJ_Mosaic))
            latch = true;
        else if ((curcolor & OBJ_BGPrioMask) < (latchcolor & OBJ_BGPrioMask))
            latch = true;

        if (latch)
            latchcolor = curcolor;

        OBJLine[i] = latchcolor;

        if (mosx == mosw)
            mosx = 0;
        else
            mosx++;
    }
}

void SoftRenderer2D::InterleaveSprites(u32 prio)
{
    u32 attrmask = (prio << 16) | OBJ_IsOpaque;
    u16* pal = (u16*)&GPU.Palette[GPU2D.Num ? 0x600 : 0x200];
    u16* extpal = GPU2D.GetOBJExtPal();

    for (s32 i = SpriteXMin; i < SpriteXMax; i++)
    {
        if ((OBJLine[i] & OBJ_OpaPrioMask) != attrmask)
            continue;
        if (!(WindowMask[i] & 0x10))
            continue;

        u16 color;
        u32 pixel = OBJLine[i];

        if (pixel & OBJ_DirectColor)
            color = pixel & 0x7FFF;
        else if (pixel & OBJ_StandardPal)
            color = pal[pixel & 0xFF];
        else
            color = extpal[pixel & 0xFFF];

        DrawPixel(&BGOBJLine[i], color, pixel & 0xFF000000);
    }
}

#define DoDrawSprite(type, ...) \
    do \
    { \
        if (iswin) \
        { \
            DrawSprite_##type<true>(__VA_ARGS__); \
        } \
        else \
        { \
            DrawSprite_##type<false>(__VA_ARGS__); \
        } \
    } while (0)

static const s32 SpriteWidthTable[16] =
{
    8, 16, 8, 8,
    16, 32, 8, 8,
    32, 32, 16, 8,
    64, 64, 32, 8
};
static const s32 SpriteHeightTable[16] =
{
    8, 8, 16, 8,
    16, 8, 32, 8,
    32, 16, 32, 8,
    64, 32, 64, 8
};

// The per-line accept set — sprite enabled, horizontally on screen,
// vertically covering the line — as a bitmask per line, from one pass
// over the OAM half instead of a 128-entry scan every line.
void SoftRenderer2D::BuildSpriteLineMask()
{
    memset(SpriteLineMask, 0, sizeof(SpriteLineMask));

    u16* oam = (u16*)&GPU.OAM[GPU2D.Num ? 0x400 : 0];

    for (int sprnum = 0; sprnum < 128; sprnum++)
    {
        u16* attrib = &oam[sprnum*4];

        u16 sprtype = (attrib[0] >> 8) & 0x3;
        if (sprtype == 2) // disabled
            continue;

        u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
        s32 boundwidth = SpriteWidthTable[sizeparam];
        s32 boundheight = SpriteHeightTable[sizeparam];
        if (sprtype == 3) // double-size rotscale sprite
        {
            boundwidth <<= 1;
            boundheight <<= 1;
        }

        s32 xpos = (s32)(attrib[1] << 23) >> 23;
        if (xpos <= -boundwidth)
            continue;

        s32 ypos = attrib[0] & 0xFF;
        u64 bit = 1ull << (sprnum & 0x3F);
        int word = sprnum >> 6;
        for (s32 l = 0; l < boundheight; l++)
        {
            s32 y = (ypos + l) & 0xFF;
            if (y < 192)
                SpriteLineMask[y][word] |= bit;
        }
    }

    SpriteLineMaskValid = true;
}

void SoftRenderer2D::DrawSprites(u32 line)
{
    // the OBJ buffers don't get updated at all if the 2D engine is disabled
    if (!GPU2D.Enabled)
        return;

    if (GPU2D.Num == 0)
    {
        auto objDirty = GPU.VRAMDirty_AOBJ.DeriveState(GPU.VRAMMap_AOBJ, GPU);
        GPU.MakeVRAMFlat_AOBJCoherent(objDirty);
    }
    else
    {
        auto objDirty = GPU.VRAMDirty_BOBJ.DeriveState(GPU.VRAMMap_BOBJ, GPU);
        GPU.MakeVRAMFlat_BOBJCoherent(objDirty);
    }

    NumSprites = 0;
    SpritePrioOnLine = 0;
    SpriteBlendOnLine = false;
    SpriteXMin = 256;
    SpriteXMax = 0;
    // OBJLine is cleared lazily, when the first sprite on the line is
    // about to draw into it; OBJWindow is read by the window-mask
    // calculation whether or not any sprite drew, so it clears always.
    memset(OBJWindow, 0, sizeof(OBJWindow));

    if (!GPU2D.OBJEnable)
        return;

    u16* oam = (u16*)&GPU.OAM[GPU2D.Num ? 0x400 : 0];

    if (GPU.OAMDirty & (1u << GPU2D.Num))
    {
        GPU.OAMDirty &= ~(1u << GPU2D.Num);
        SpriteLineMaskValid = false;
    }
    if (!SpriteLineMaskValid)
        BuildSpriteLineMask();

    for (int word = 0; word < 2; word++)
    {
        u64 m = SpriteLineMask[line][word];
        while (m)
    {
        int sprnum = (word << 6) + __builtin_ctzll(m);
        m &= m - 1;

        u16* attrib = &oam[sprnum*4];

        u16 sprtype = (attrib[0] >> 8) & 0x3;
        if (sprtype == 2) // disabled
            continue;

        bool iswin = (((attrib[0] >> 10) & 0x3) == 2);

        u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
        s32 width = SpriteWidthTable[sizeparam];
        s32 height = SpriteHeightTable[sizeparam];
        s32 boundwidth = width;
        s32 boundheight = height;

        if (sprtype == 3) // double-size rotscale sprite
        {
            boundwidth <<= 1;
            boundheight <<= 1;
        }

        // TODO checkme (128-tall sprite overflow thing)
        s32 ypos = attrib[0] & 0xFF;
        if (((line - ypos) & 0xFF) >= boundheight)
            continue;

        s32 xpos = (s32)(attrib[1] << 23) >> 23;
        if (xpos <= -boundwidth)
            continue;

        if ((attrib[0] & (1<<12)) && (!iswin))
        {
            // adjust Y position for sprite mosaic
            // (sprite mosaic does not apply to OBJ-window sprites)
            // a ypos greater than the sprite height means we underflowed, due to OBJMosaicLine being
            // latched before the sprite's top, so we clamp it to 0
            ypos = (GPU2D.OBJMosaicLine - ypos) & 0xFF;
            if (ypos >= boundheight) ypos = 0;
        }
        else
            ypos = (line - ypos) & 0xFF;

        if (!NumSprites)
            memset(OBJLine, 0, sizeof(OBJLine));
        if (!iswin)
        {
            SpritePrioOnLine |= 1 << ((attrib[2] >> 10) & 0x3);

            u32 spritemode = (attrib[0] >> 10) & 0x3;
            if (spritemode == 1 || spritemode == 3)
                SpriteBlendOnLine = true;

            s32 lo = xpos < 0 ? 0 : xpos;
            s32 hi = xpos + boundwidth;
            if (hi > 256) hi = 256;
            if (lo < SpriteXMin) SpriteXMin = lo;
            if (hi > SpriteXMax) SpriteXMax = hi;
        }

        if (sprtype & 1)
            DoDrawSprite(Rotscale, sprnum, boundwidth, boundheight, width, height, xpos, ypos);
        else
            DoDrawSprite(Normal, sprnum, width, height, xpos, ypos);

        NumSprites++;
    }
    }
}

template<bool window>
void SoftRenderer2D::DrawSpritePixel(int color, u32 pixelattr, s32 xpos)
{
    if (window)
    {
        if (color != -1)
            OBJWindow[xpos] = 1;
    }
    else
    {
        u32 oldpixel = OBJLine[xpos];
        bool oldisopaque = !!(oldpixel & OBJ_IsOpaque);
        bool newisopaque = (color != -1);
        bool priocheck = (pixelattr & OBJ_BGPrioMask) < (oldpixel & OBJ_BGPrioMask);

        if (newisopaque && (!oldisopaque || priocheck))
        {
            OBJLine[xpos] = color | pixelattr;
        }
        else if (!newisopaque && !oldisopaque)
        {
            OBJLine[xpos] &= ~(OBJ_Mosaic | OBJ_BGPrioMask);
            OBJLine[xpos] |= (pixelattr & (OBJ_IsSprite | OBJ_Mosaic | OBJ_BGPrioMask));
        }
    }
}

// What DrawSpritePixel does for a run of transparent pixels: no color
// lands, but the attributes still merge into non-opaque OBJLine pixels
// (the sprite mosaic pass reads them off transparent pixels too).
void SoftRenderer2D::MergeTransparentSpritePixels(s32 xpos, u32 len, u32 pixelattr)
{
    u32 add = pixelattr & (OBJ_IsSprite | OBJ_Mosaic | OBJ_BGPrioMask);
    for (u32 k = 0; k < len; k++)
    {
        u32 old = OBJLine[xpos + (s32)k];
        if (!(old & OBJ_IsOpaque))
            OBJLine[xpos + (s32)k] = (old & ~(u32)(OBJ_Mosaic | OBJ_BGPrioMask)) | add;
    }
}

template<bool window>
void SoftRenderer2D::DrawSprite_Rotscale(u32 num, u32 boundwidth, u32 boundheight, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)&GPU.OAM[GPU2D.Num ? 0x400 : 0];
    u16* attrib = &oam[num * 4];
    u16* rotparams = &oam[(((attrib[1] >> 9) & 0x1F) * 16) + 3];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | OBJ_IsSprite | OBJ_IsOpaque;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 ytilefactor;

    u8* objvram;
    u32 objvrammask;
    GPU2D.GetOBJVRAM(objvram, objvrammask);

    s32 centerX = boundwidth >> 1;
    s32 centerY = boundheight >> 1;

    if ((attrib[0] & (1<<12)) && !window)
    {
        // apply Y mosaic
        pixelattr |= OBJ_Mosaic;
    }

    u32 xoff;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+boundwidth) > 256)
            boundwidth = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    s16 rotA = (s16)rotparams[0];
    s16 rotB = (s16)rotparams[4];
    s16 rotC = (s16)rotparams[8];
    s16 rotD = (s16)rotparams[12];

    s32 rotX = ((xoff-centerX) * rotA) + ((ypos-centerY) * rotB) + (width << 7);
    s32 rotY = ((xoff-centerX) * rotC) + ((ypos-centerY) * rotD) + (height << 7);

    width <<= 8;
    height <<= 8;

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr;
        if (GPU2D.DispCnt & 0x40)
        {
            if (GPU2D.DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr = tilenum << (7 + ((GPU2D.DispCnt >> 22) & 0x1));
                ytilefactor = ((width >> 8) * 2);
            }
        }
        else
        {
            if (GPU2D.DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                ytilefactor = (256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                ytilefactor = (128 * 2);
            }
        }

        for (; xoff < boundwidth;)
        {
            if ((u32)rotX < width && (u32)rotY < height)
            {
                color = *(u16*)&objvram[(pixelsaddr + ((rotY >> 8) * ytilefactor) + ((rotX >> 8) << 1)) & objvrammask];

                DrawSpritePixel<window>((color&0x8000) ? color : -1, pixelattr, xpos);
            }

            rotX += rotA;
            rotY += rotC;
            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (GPU2D.DispCnt & (1<<4))
        {
            pixelsaddr <<= ((GPU2D.DispCnt >> 20) & 0x3);
            ytilefactor = (width >> 11) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            ytilefactor = 0x20;
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        ytilefactor <<= 5;
        pixelsaddr <<= 5;

        if (attrib[0] & (1<<13))
        {
            // 256-color

            if (!window)
            {
                if (!(GPU2D.DispCnt & (1<<31)))
                    pixelattr |= OBJ_StandardPal;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>5) + ((rotX>>11)*64) + ((rotX&0x700)>>8)) & objvrammask];

                    DrawSpritePixel<window>(color ? color : -1, pixelattr, xpos);
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
        else
        {
            // 16-color
            if (!window)
            {
                pixelattr |= OBJ_StandardPal;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>6) + ((rotX>>11)*32) + ((rotX&0x700)>>9)) & objvrammask];
                    if (rotX & 0x100)
                        color >>= 4;
                    else
                        color &= 0x0F;

                    DrawSpritePixel<window>(color ? color : -1, pixelattr, xpos);
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
    }
}

template<bool window>
void SoftRenderer2D::DrawSprite_Normal(u32 num, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)&GPU.OAM[GPU2D.Num ? 0x400 : 0];
    u16* attrib = &oam[num * 4];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | OBJ_IsSprite | OBJ_IsOpaque;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 wmask = width - 8; // really ((width - 1) & ~0x7)

    if ((attrib[0] & (1<<12)) && !window)
    {
        // apply Y mosaic
        pixelattr |= OBJ_Mosaic;
    }

    u8* objvram;
    u32 objvrammask;
    GPU2D.GetOBJVRAM(objvram, objvrammask);

    // yflip
    if (attrib[1] & (1<<13))
        ypos = height-1 - ypos;

    u32 xoff;
    u32 xend = width;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+xend) > 256)
            xend = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        // bitmap sprite

        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr = tilenum;
        if (GPU2D.DispCnt & 0x40)
        {
            if (GPU2D.DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr <<= (7 + ((GPU2D.DispCnt >> 22) & 0x1));
                pixelsaddr += (ypos * width * 2);
            }
        }
        else
        {
            if (GPU2D.DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                pixelsaddr += (ypos * 256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                pixelsaddr += (ypos * 128 * 2);
            }
        }

        s32 pixelstride;

        if (attrib[1] & (1<<12)) // xflip
        {
            pixelsaddr += ((width-1) << 1);
            pixelsaddr -= (xoff << 1);
            pixelstride = -2;
        }
        else
        {
            pixelsaddr += (xoff << 1);
            pixelstride = 2;
        }

        for (; xoff < xend;)
        {
            color = *(u16*)&objvram[pixelsaddr & objvrammask];

            pixelsaddr += pixelstride;

            DrawSpritePixel<window>((color&0x8000) ? color : -1, pixelattr, xpos);

            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (GPU2D.DispCnt & (1<<4))
        {
            pixelsaddr <<= ((GPU2D.DispCnt >> 20) & 0x3);
            pixelsaddr += ((ypos >> 3) * (width >> 3)) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            pixelsaddr += ((ypos >> 3) * 0x20);
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        if (attrib[0] & (1<<13))
        {
            // 256-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 3);
            s32 pixelstride;

            if (!window)
            {
                if (!(GPU2D.DispCnt & (1<<31)))
                    pixelattr |= OBJ_StandardPal;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            if (attrib[1] & (1<<12)) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 3);
                pixelsaddr += ((width-1) & 0x7);
                pixelsaddr -= ((xoff & wmask) << 3);
                pixelsaddr -= (xoff & 0x7);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 3);
                pixelsaddr += (xoff & 0x7);
                pixelstride = 1;
            }

            // leading partial tile, pixel by pixel
            for (; (xoff & 0x7) && xoff < xend;)
            {
                color = objvram[pixelsaddr & objvrammask];

                pixelsaddr += pixelstride;

                DrawSpritePixel<window>(color ? color : -1, pixelattr, xpos);

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += (56 * pixelstride);
            }

            // then whole tile rows out of one aligned load each; a
            // fully transparent row — most of a sprite's bounding box —
            // takes no pixel work beyond the attribute merge
            while (xoff < xend)
            {
                u32 seglen = xend - xoff;
                if (seglen > 8) seglen = 8;

                u64 row;
                if (pixelstride > 0)
                    row = *(u64*)&objvram[pixelsaddr & objvrammask];
                else
                    row = __builtin_bswap64(*(u64*)&objvram[(pixelsaddr - 7) & objvrammask]);

                u64 used = (seglen == 8) ? row : (row & ((1ull << (seglen << 3)) - 1));
                if (used == 0)
                {
                    if constexpr (!window)
                        MergeTransparentSpritePixels(xpos, seglen, pixelattr);
                }
                else
                {
                    u64 r = row;
                    for (u32 k = 0; k < seglen; k++, r >>= 8)
                    {
                        u8 c = (u8)r;
                        DrawSpritePixel<window>(c ? c : -1, pixelattr, xpos + (s32)k);
                    }
                }

                xoff += seglen;
                xpos += (s32)seglen;
                pixelsaddr += (s32)seglen * pixelstride;
                if (!(xoff & 0x7)) pixelsaddr += (56 * pixelstride);
            }
        }
        else
        {
            // 16-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 2);
            s32 pixelstride;

            if (!window)
            {
                pixelattr |= OBJ_StandardPal;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            // TODO: optimize VRAM access!!
            // TODO: do xflip better? the 'two pixels per byte' thing makes it a bit shitty

            if (attrib[1] & (1<<12)) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 2);
                pixelsaddr += (((width-1) & 0x7) >> 1);
                pixelsaddr -= ((xoff & wmask) << 2);
                pixelsaddr -= ((xoff & 0x7) >> 1);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 2);
                pixelsaddr += ((xoff & 0x7) >> 1);
                pixelstride = 1;
            }

            // leading partial tile, pixel by pixel
            for (; (xoff & 0x7) && xoff < xend;)
            {
                if (attrib[1] & (1<<12))
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] & 0x0F; pixelsaddr--; }
                    else              color = objvram[pixelsaddr & objvrammask] >> 4;
                }
                else
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] >> 4; pixelsaddr++; }
                    else              color = objvram[pixelsaddr & objvrammask] & 0x0F;
                }

                DrawSpritePixel<window>(color ? color : -1, pixelattr, xpos);

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += ((attrib[1] & 0x1000) ? -28 : 28);
            }

            // then whole tile rows of nibbles out of one aligned load
            // each, as in the 256-color path
            while (xoff < xend)
            {
                u32 seglen = xend - xoff;
                if (seglen > 8) seglen = 8;

                u32 row;
                if (pixelstride > 0)
                    row = *(u32*)&objvram[pixelsaddr & objvrammask];
                else
                {
                    row = __builtin_bswap32(*(u32*)&objvram[(pixelsaddr - 3) & objvrammask]);
                    row = ((row & 0x0F0F0F0F) << 4) | ((row >> 4) & 0x0F0F0F0F);
                }

                u32 used = (seglen == 8) ? row : (row & ((1u << (seglen << 2)) - 1));
                if (used == 0)
                {
                    if constexpr (!window)
                        MergeTransparentSpritePixels(xpos, seglen, pixelattr);
                }
                else
                {
                    u32 r = row;
                    for (u32 k = 0; k < seglen; k++, r >>= 4)
                    {
                        u8 c = r & 0xF;
                        DrawSpritePixel<window>(c ? c : -1, pixelattr, xpos + (s32)k);
                    }
                }

                xoff += seglen;
                xpos += (s32)seglen;
                pixelsaddr += (s32)(seglen >> 1) * pixelstride;
                if (!(xoff & 0x7)) pixelsaddr += ((attrib[1] & 0x1000) ? -28 : 28);
            }
        }
    }
}

}
