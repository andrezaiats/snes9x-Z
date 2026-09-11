/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#define _TILEIMPL_CPP_
#include "tileimpl.h"

namespace TileImpl {

	template<class MATH, class BPSTART>
	void Normal2x1Base<MATH, BPSTART>::Draw(int N, int M, uint32 Offset, uint32 OffsetInLine, uint8 Pix, uint8 Z1, uint8 Z2)
	{
		(void) OffsetInLine;
		if (Z1 > GFX.DB[Offset + 2 * N] && (M))
		{
			GFX.S[Offset + 2 * N] = GFX.S[Offset + 2 * N + 1] = MATH::Calc(GFX.ScreenColors[Pix], GFX.SubScreen[Offset + 2 * N], GFX.SubZBuffer[Offset + 2 * N]);
			GFX.DB[Offset + 2 * N] = GFX.DB[Offset + 2 * N + 1] = Z2;
		}
	}

	
	template<class MATH, class BPSTART>
	void Normal2x1Base<MATH, BPSTART>::FillSpan(uint32 Offset, uint32 OffsetInLine, uint32 Left, uint32 Right, uint8 Pix, uint8 Z2)
	{
		(void) OffsetInLine;

		
		if (Right <= Left)
			return;

		const uint16	Main = GFX.ScreenColors[Pix];
		uint16			*s   = GFX.S + Offset;
		const uint16	*sub = GFX.SubScreen + Offset;
		const uint8		*sd  = GFX.SubZBuffer + Offset;

		for (uint32 x = Left; x < Right; x++)
			s[2 * x] = s[2 * x + 1] = MATH::Calc(Main, sub[2 * x], sd[2 * x]);

		memset(GFX.DB + Offset + 2 * Left, Z2, 2 * (Right - Left));
	}

	
	template<class MATH, class BPSTART>
	template<int STEP>
	void Normal2x1Base<MATH, BPSTART>::DrawRow8(uint32 Offset, uint32 OffsetInLine, const uint8 *bp, uint8 Z1, uint8 Z2)
	{
		(void) OffsetInLine;

		uint16			*s      = GFX.S + Offset;
		uint8			*db     = GFX.DB + Offset;
		const uint16	*colors = GFX.ScreenColors;
		const uint16	*sub    = GFX.SubScreen + Offset;
		const uint8		*sd     = GFX.SubZBuffer + Offset;

		for (int x = 0; x < 8; x++, bp += STEP)
		{
			const uint8	Pix = *bp;

			if (Z1 > db[2 * x] && Pix)
			{
				s[2 * x] = s[2 * x + 1] = MATH::Calc(colors[Pix], sub[2 * x], sd[2 * x]);
				db[2 * x] = db[2 * x + 1] = Z2;
			}
		}
	}

	
	
	
	template<class MATH, class BPSTART>
	template<int STEP>
	void Normal2x1Base<MATH, BPSTART>::DrawRowClipped(uint32 Offset, uint32 OffsetInLine, const uint8 *bp, uint32 StartPixel, uint32 Width, uint8 Z1, uint8 Z2)
	{
		(void) OffsetInLine;

		uint16			*s      = GFX.S;
		uint8			*db     = GFX.DB;
		const uint16	*colors = GFX.ScreenColors;
		const uint16	*sub    = GFX.SubScreen;
		const uint8		*sd     = GFX.SubZBuffer;

		uint32	end = StartPixel + Width;
		if (end > 8)
			end = 8;

		for (uint32 x = StartPixel; x < end; x++, bp += STEP)
		{
			const uint8		Pix = *bp;
			const uint32	idx = Offset + 2 * x;

			if (Z1 > db[idx] && Pix)
			{
				s[idx] = s[idx + 1] = MATH::Calc(colors[Pix], sub[idx], sd[idx]);
				db[idx] = db[idx + 1] = Z2;
			}
		}
	}


	// normal double width
	template struct Renderers<DrawTile16, Normal2x1>;
	template struct Renderers<DrawClippedTile16, Normal2x1>;
	template struct Renderers<DrawMosaicPixel16, Normal2x1>;
	template struct Renderers<DrawBackdrop16, Normal2x1>;
	template struct Renderers<DrawMode7MosaicBG1, Normal2x1>;
	template struct Renderers<DrawMode7BG1, Normal2x1>;
	template struct Renderers<DrawMode7MosaicBG2, Normal2x1>;
	template struct Renderers<DrawMode7BG2, Normal2x1>;

	// normal double width interlace
	template struct Renderers<DrawTile16, Interlace>;
	template struct Renderers<DrawClippedTile16, Interlace>;
	template struct Renderers<DrawMosaicPixel16, Interlace>;
	//template struct Renderers<DrawBackdrop16, Normal2x1>;
	//template struct Renderers<DrawMode7MosaicBG1, Normal2x1>;
	//template struct Renderers<DrawMode7BG1, Normal2x1>;
	//template struct Renderers<DrawMode7MosaicBG2, Normal2x1>;
	//template struct Renderers<DrawMode7BG2, Normal2x1>;

} // namespace TileImpl
