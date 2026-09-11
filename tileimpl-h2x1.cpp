/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#define _TILEIMPL_CPP_
#include "tileimpl.h"

namespace TileImpl {

	template<class MATH, class BPSTART>
	void HiresBase<MATH, BPSTART>::Draw(int N, int M, uint32 Offset, uint32 OffsetInLine, uint8 Pix, uint8 Z1, uint8 Z2)
	{
		if (Z1 > GFX.DB[Offset + 2 * N] && (M))
		{
			GFX.S[Offset + 2 * N + 1] = MATH::Calc(GFX.ScreenColors[Pix], GFX.SubScreen[Offset + 2 * N], GFX.SubZBuffer[Offset + 2 * N]);
			if ((OffsetInLine + 2 * N ) != (SNES_WIDTH - 1) << 1)
				GFX.S[Offset + 2 * N + 2] = MATH::Calc((GFX.ClipColors ? 0 : GFX.SubScreen[Offset + 2 * N + 2]), GFX.RealScreenColors[Pix], GFX.SubZBuffer[Offset + 2 * N]);
			if ((OffsetInLine + 2 * N) == 0 || (OffsetInLine + 2 * N) == GFX.RealPPL)
				GFX.S[Offset + 2 * N] = MATH::Calc((GFX.ClipColors ? 0 : GFX.SubScreen[Offset + 2 * N]), GFX.RealScreenColors[Pix], GFX.SubZBuffer[Offset + 2 * N]);
			GFX.DB[Offset + 2 * N] = GFX.DB[Offset + 2 * N + 1] = Z2;
		}
	}

	
	
	
	template<class MATH, class BPSTART>
	void HiresBase<MATH, BPSTART>::FillSpan(uint32 Offset, uint32 OffsetInLine, uint32 Left, uint32 Right, uint8 Pix, uint8 Z2)
	{
		
		if (Right <= Left)
			return;

		const uint16	Main = GFX.ScreenColors[Pix];
		const uint16	Real = GFX.RealScreenColors[Pix];
		uint16			*s   = GFX.S + Offset;
		const uint16	*sub = GFX.SubScreen + Offset;
		const uint8		*sd  = GFX.SubZBuffer + Offset;

		for (uint32 x = Left; x < Right; x++)
		{
			s[2 * x + 1] = MATH::Calc(Main, sub[2 * x], sd[2 * x]);
			if ((OffsetInLine + 2 * x) != (SNES_WIDTH - 1) << 1)
				s[2 * x + 2] = MATH::Calc((GFX.ClipColors ? 0 : sub[2 * x + 2]), Real, sd[2 * x]);
			if ((OffsetInLine + 2 * x) == 0 || (OffsetInLine + 2 * x) == GFX.RealPPL)
				s[2 * x] = MATH::Calc((GFX.ClipColors ? 0 : sub[2 * x]), Real, sd[2 * x]);
		}

		memset(GFX.DB + Offset + 2 * Left, Z2, 2 * (Right - Left));
	}

	
	
	template<class MATH, class BPSTART>
	template<int STEP>
	void HiresBase<MATH, BPSTART>::DrawRow8(uint32 Offset, uint32 OffsetInLine, const uint8 *bp, uint8 Z1, uint8 Z2)
	{
		uint16			*s      = GFX.S + Offset;
		uint8			*db     = GFX.DB + Offset;
		const uint16	*colors = GFX.ScreenColors;
		const uint16	*real   = GFX.RealScreenColors;
		const uint16	*sub    = GFX.SubScreen + Offset;
		const uint8		*sd     = GFX.SubZBuffer + Offset;
		const bool8		clip    = GFX.ClipColors;
		const uint32	ppl     = GFX.RealPPL;

		for (int x = 0; x < 8; x++, bp += STEP)
		{
			const uint8	Pix = *bp;

			if (Z1 > db[2 * x] && Pix)
			{
				s[2 * x + 1] = MATH::Calc(colors[Pix], sub[2 * x], sd[2 * x]);
				if ((OffsetInLine + 2 * x) != (SNES_WIDTH - 1) << 1)
					s[2 * x + 2] = MATH::Calc((clip ? 0 : sub[2 * x + 2]), real[Pix], sd[2 * x]);
				if ((OffsetInLine + 2 * x) == 0 || (OffsetInLine + 2 * x) == ppl)
					s[2 * x] = MATH::Calc((clip ? 0 : sub[2 * x]), real[Pix], sd[2 * x]);
				db[2 * x] = db[2 * x + 1] = Z2;
			}
		}
	}

	
	
	
	template<class MATH, class BPSTART>
	template<int STEP>
	void HiresBase<MATH, BPSTART>::DrawRowClipped(uint32 Offset, uint32 OffsetInLine, const uint8 *bp, uint32 StartPixel, uint32 Width, uint8 Z1, uint8 Z2)
	{
		uint16			*s      = GFX.S;
		uint8			*db     = GFX.DB;
		const uint16	*colors = GFX.ScreenColors;
		const uint16	*real   = GFX.RealScreenColors;
		const uint16	*sub    = GFX.SubScreen;
		const uint8		*sd     = GFX.SubZBuffer;
		const bool8		clip    = GFX.ClipColors;
		const uint32	ppl     = GFX.RealPPL;

		uint32	end = StartPixel + Width;
		if (end > 8)
			end = 8;

		for (uint32 x = StartPixel; x < end; x++, bp += STEP)
		{
			const uint8		Pix = *bp;
			const uint32	idx = Offset + 2 * x;

			if (Z1 > db[idx] && Pix)
			{
				s[idx + 1] = MATH::Calc(colors[Pix], sub[idx], sd[idx]);
				if ((OffsetInLine + 2 * x) != (SNES_WIDTH - 1) << 1)
					s[idx + 2] = MATH::Calc((clip ? 0 : sub[idx + 2]), real[Pix], sd[idx]);
				if ((OffsetInLine + 2 * x) == 0 || (OffsetInLine + 2 * x) == ppl)
					s[idx] = MATH::Calc((clip ? 0 : sub[idx]), real[Pix], sd[idx]);
				db[idx] = db[idx + 1] = Z2;
			}
		}
	}


	// hires double width
	template struct Renderers<DrawTile16, Hires>;
	template struct Renderers<DrawClippedTile16, Hires>;
	template struct Renderers<DrawMosaicPixel16, Hires>;
	template struct Renderers<DrawBackdrop16, Hires>;
	template struct Renderers<DrawMode7MosaicBG1, Hires>;
	template struct Renderers<DrawMode7BG1, Hires>;
	template struct Renderers<DrawMode7MosaicBG2, Hires>;
	template struct Renderers<DrawMode7BG2, Hires>;

	// hires double width interlace
	template struct Renderers<DrawTile16, HiresInterlace>;
	template struct Renderers<DrawClippedTile16, HiresInterlace>;
	template struct Renderers<DrawMosaicPixel16, HiresInterlace>;
	//template struct Renderers<DrawBackdrop16, Hires>;
	//template struct Renderers<DrawMode7MosaicBG1, Hires>;
	//template struct Renderers<DrawMode7BG1, Hires>;
	//template struct Renderers<DrawMode7MosaicBG2, Hires>;
	//template struct Renderers<DrawMode7BG2, Hires>;

} // namespace TileImpl
