/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#define _TILEIMPL_CPP_
#include "tileimpl.h"

namespace TileImpl {

	template<class MATH, class BPSTART>
	void Normal1x1Base<MATH, BPSTART>::Draw(int N, int M, uint32 Offset, uint32 OffsetInLine, uint8 Pix, uint8 Z1, uint8 Z2)
	{
		(void) OffsetInLine;
		if (Z1 > GFX.DB[Offset + N] && (M))
		{
			GFX.S[Offset + N] = MATH::Calc(GFX.ScreenColors[Pix], GFX.SubScreen[Offset + N], GFX.SubZBuffer[Offset + N]);
			GFX.DB[Offset + N] = Z2;
		}
	}

	
	
	
	
	
	
	
	
	
	template<class MATH, class BPSTART>
	void Normal1x1Base<MATH, BPSTART>::FillSpan(uint32 Offset, uint32 OffsetInLine, uint32 Left, uint32 Right, uint8 Pix, uint8 Z2)
	{
		(void) OffsetInLine;

		
		
		if (Right <= Left)
			return;

		const uint16	Main = GFX.ScreenColors[Pix];
		uint16			*s   = GFX.S + Offset;
		const uint16	*sub = GFX.SubScreen + Offset;
		const uint8		*sd  = GFX.SubZBuffer + Offset;

		for (uint32 x = Left; x < Right; x++)
			s[x] = MATH::Calc(Main, sub[x], sd[x]);

		memset(GFX.DB + Offset + Left, Z2, Right - Left);
	}

	
	
	
	
	
	
	
	template<class MATH, class BPSTART>
	template<int STEP>
	void Normal1x1Base<MATH, BPSTART>::DrawRow8(uint32 Offset, uint32 OffsetInLine, const uint8 *bp, uint8 Z1, uint8 Z2)
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

			if (Z1 > db[x] && Pix)
			{
				s[x] = MATH::Calc(colors[Pix], sub[x], sd[x]);
				db[x] = Z2;
			}
		}
	}

	
	
	
	
	
	
	
	
	
	
	
	template<class MATH, class BPSTART>
	template<int STEP>
	void Normal1x1Base<MATH, BPSTART>::DrawRowClipped(uint32 Offset, uint32 OffsetInLine, const uint8 *bp, uint32 StartPixel, uint32 Width, uint8 Z1, uint8 Z2)
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
			const uint32	idx = Offset + x;

			if (Z1 > db[idx] && Pix)
			{
				s[idx] = MATH::Calc(colors[Pix], sub[idx], sd[idx]);
				db[idx] = Z2;
			}
		}
	}


	// normal width
	template struct Renderers<DrawTile16, Normal1x1>;
	template struct Renderers<DrawClippedTile16, Normal1x1>;
	template struct Renderers<DrawMosaicPixel16, Normal1x1>;
	template struct Renderers<DrawBackdrop16, Normal1x1>;
	template struct Renderers<DrawMode7MosaicBG1, Normal1x1>;
	template struct Renderers<DrawMode7BG1, Normal1x1>;
	template struct Renderers<DrawMode7MosaicBG2, Normal1x1>;
	template struct Renderers<DrawMode7BG2, Normal1x1>;

} // namespace TileImpl
