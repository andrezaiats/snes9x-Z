/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _GFX_H_
#define _GFX_H_

#include "port.h"
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

struct SGFX
{
	const uint32 Pitch = sizeof(uint16) * MAX_SNES_WIDTH;
	const uint32 RealPPL = MAX_SNES_WIDTH; // true PPL of Screen buffer
	const uint32 ScreenSize =  MAX_SNES_WIDTH * MAX_SNES_HEIGHT;
	std::vector<uint16> ScreenBuffer;
	uint16	*Screen;
	uint16	*SubScreen;
	uint8	*ZBuffer;
	uint8	*SubZBuffer;
	uint16	*S;
	uint8	*DB;
	uint16	*ZERO;
	uint32	PPL;				
	uint32	LinesPerTile;		
	uint16	*ScreenColors;		
	uint16	*RealScreenColors;	
	uint8	Z1;					
	uint8	Z2;					
	uint32	FixedColour;
	uint8	DoInterlace;
	uint32	StartY;
	uint32	EndY;
	bool8	ClipColors;
	uint8	OBJWidths[128];
	uint8	OBJVisibleTiles[128];

	struct ClipData	*Clip;

	struct
	{
		uint8	RTOFlags;
		int16	Tiles;

		struct
		{
			int8	Sprite;
			uint8	Line;
		}	OBJ[128];
	}	OBJLines[SNES_HEIGHT_EXTENDED];

	void	(*DrawBackdropMath) (uint32, uint32, uint32);
	void	(*DrawBackdropNomath) (uint32, uint32, uint32);
	void	(*DrawTileMath) (uint32, uint32, uint32, uint32);
	void	(*DrawTileNomath) (uint32, uint32, uint32, uint32);
	void	(*DrawClippedTileMath) (uint32, uint32, uint32, uint32, uint32, uint32);
	void	(*DrawClippedTileNomath) (uint32, uint32, uint32, uint32, uint32, uint32);
	void	(*DrawMosaicPixelMath) (uint32, uint32, uint32, uint32, uint32, uint32);
	void	(*DrawMosaicPixelNomath) (uint32, uint32, uint32, uint32, uint32, uint32);
	void	(*DrawMode7BG1Math) (uint32, uint32, int);
	void	(*DrawMode7BG1Nomath) (uint32, uint32, int);
	void	(*DrawMode7BG2Math) (uint32, uint32, int);
	void	(*DrawMode7BG2Nomath) (uint32, uint32, int);

	std::string InfoString;
	uint32	InfoStringTimeout;
	char	FrameDisplayString[256];
};

struct SBG
{
	uint8	(*ConvertTile) (uint8 *, uint32, uint32);
	uint8	(*ConvertTileFlip) (uint8 *, uint32, uint32);

	uint32	TileSizeH;
	uint32	TileSizeV;
	uint32	OffsetSizeH;
	uint32	OffsetSizeV;
	uint32	TileShift;
	uint32	TileAddress;
	uint32	NameSelect;
	uint32	SCBase;

	uint32	StartPalette;
	uint32	PaletteShift;
	uint32	PaletteMask;
	uint8	EnableMath;
	uint8	InterlaceLine;

	uint8	*Buffer;
	uint8	*BufferFlip;
	uint8	*Buffered;
	uint8	*BufferedFlip;
	bool8	DirectColourMode;
};

struct SLineData
{
	struct
	{
		uint16	VOffset;
		uint16	HOffset;
	}	BG[4];
};

struct SLineMatrixData
{
	short	MatrixA;
	short	MatrixB;
	short	MatrixC;
	short	MatrixD;
	short	CentreX;
	short	CentreY;
	short	M7HOFS;
	short	M7VOFS;
};











struct SRenderRegs
{
	
	struct
	{
		uint16	SCBase;
		uint8	BGSize;
		uint16	NameBase;
		uint16	SCSize;
	}	BG[4];

	uint8	BGMode;
	uint8	BG3Priority;

	
	uint8	Mosaic;
	uint8	MosaicStart;
	bool8	BGMosaic[4];

	
	uint8	Window1Left;
	uint8	Window1Right;
	uint8	Window2Left;
	uint8	Window2Right;

	
	uint8	ClipCounts[6];
	uint8	ClipWindowOverlapLogic[6];
	uint8	ClipWindow1Enable[6];
	uint8	ClipWindow2Enable[6];
	bool8	ClipWindow1Inside[6];
	bool8	ClipWindow2Inside[6];

	bool8	RecomputeClipWindows;

	bool8	ForcedBlanking;

	
	uint8	FixedColourRed;
	uint8	FixedColourGreen;
	uint8	FixedColourBlue;
	uint8	Brightness;
	uint16	ScreenHeight;

	
	bool8	Mode7HFlip;
	bool8	Mode7VFlip;
	uint8	Mode7Repeat;

	
	uint8	OBJSizeSelect;
	uint16	OBJNameBase;
	uint16	OBJNameSelect;
	uint16	OAMAddr;
	uint8	OAMFlip;
	uint8	OAMPriorityRotation;
	uint8	FirstSprite;

	
	bool8	Interlace;
	bool8	InterlaceOBJ;
	
	
	
	
	bool8	InterlaceField;
	bool8	PseudoHires;
	uint8	MaxBrightness;
	int	RenderedScreenWidth;
	int	RenderedScreenHeight;
	bool8	DoubleWidthPixels;
	bool8	DoubleHeightPixels;
	bool8	RenderThisFrame;

	
	
	
	
	uint8	FillRAM[0x34];
};


enum SRenderTag
{
	RTAG_SPAN       = 0,
	RTAG_CGRAM      = 1,
	RTAG_OBJ        = 2,
	RTAG_OAMHI      = 3,
	RTAG_FRAME_START= 4,
	RTAG_FRAME_END  = 5,
	RTAG_RESYNC     = 6,
	RTAG_CLEAR_RTO  = 7,
	RTAG_SHUTDOWN   = 8,		
	RTAG_RTO_ACCUM  = 9		
};





struct SRenderCommand
{
	uint8	tag;	

	union
	{
		
		struct
		{
			uint32		StartY;
			uint32		EndY;
			uint8		flags;		
			SRenderRegs	regs;
		}	span;

		
		
		
		struct
		{
			uint32		line;
		}	rto_accum;

		
		struct
		{
			uint8	addr;
			uint16	value;
		}	cgram;

		
		struct
		{
			uint8	index;
			
			int16	HPos;
			uint16	VPos;
			uint8	HFlip;
			uint8	VFlip;
			uint16	Name;
			uint8	Priority;
			uint8	Palette;
			uint8	Size;
		}	obj;

		
		struct
		{
			uint8	index;	
			uint8	byte;
		}	oamhi;

		
		
		struct
		{
			uint8	dummy;
		}	frame_start;

		
		
		
		
		
		
		struct
		{
			uint8	dummy;
		}	frame_end;

		
		
		
		
		
		struct
		{
			SRenderRegs	regs;
		}	resync;

		
		struct
		{
			uint8	dummy;
		}	clear_rto;
	};
};




struct SRenderState
{
	uint16	CGDATA[256];		
	struct SOBJ	OBJ_data[128];	
					
	struct SOBJ	*OBJ;		
					

	
	uint8	*XB;
	uint32	Red[256];
	uint32	Green[256];
	uint32	Blue[256];
	uint16	ScreenColors[256];

	
	
	
	
	
	uint32	cgram_version;
	uint32	obj_version;
	uint32	cgram_applied;		
	uint32	obj_applied;		
	uint8	last_brightness;	

	
	
	
	
	int	last_obj_first_sprite;
	int	last_obj_size_select;

	bool8	RecomputeClipWindows;
	uint8	MosaicStart;

	
	
	
	
	
	
	
	
	
	
	
	
	
	
	bool8	HiresWidened;
	bool8	HeightDoubled;
	uint16	DoubledScreenHeight;	
	uint8	RangeTimeOver;		
};

extern struct SRenderState	RS;





extern const struct SRenderRegs	*S9xCurRenderRegs;






#define S9X_SPAN_NO_RTO		0x01




#define S9X_RENDER_RING_CAPACITY	8192

struct SRenderLog
{
	SRenderCommand	ring[S9X_RENDER_RING_CAPACITY];
	
	
	
	uint32	produced;	
	uint32	consumed;	
};

extern struct SRenderLog	RenderLog;




struct SResyncPayload
{
	uint16		CGDATA[256];
	struct SOBJ	OBJ[128];
};

extern struct SResyncPayload	S9xResyncPayload;












alignas(64) extern std::atomic<uint64_t>	RenderProduced;	
alignas(64) extern std::atomic<uint64_t>	RenderConsumed;	




































#if defined(__linux__)
#define S9X_EC_FUTEX		1
#elif defined(__cpp_lib_atomic_wait) && __cpp_lib_atomic_wait >= 201907L
#define S9X_EC_ATOMIC_WAIT	1
#else
#define S9X_EC_CONDVAR		1
#endif





struct alignas(64) EventCount
{
#if defined(S9X_EC_CONDVAR)
	std::mutex		mtx;
	std::condition_variable	cv;

	EventCount() {}

	void notify()
	{
		
		
		
		
		
		
		{
			std::lock_guard<std::mutex> lk(mtx);
		}
		cv.notify_all();
	}

	template<typename Pred>
	void waitPred(Pred pred)
	{
		std::unique_lock<std::mutex> lk(mtx);
		cv.wait(lk, pred);
	}
#else
	
	std::atomic<uint32_t>	epoch;
	std::atomic<uint32_t>	waiters;

	EventCount() : epoch(0), waiters(0) {}

	void notify()
	{
		
		
		
		
		
		
		
		
		epoch.fetch_add(1, std::memory_order_seq_cst);
		if (waiters.load(std::memory_order_seq_cst) != 0)
			wakeAll();
	}

	template<typename Pred>
	void waitPred(Pred pred)
	{
		for (;;)
		{
			waiters.fetch_add(1, std::memory_order_seq_cst);
			const uint32_t	key = epoch.load(std::memory_order_seq_cst);

			if (pred())
			{
				waiters.fetch_sub(1, std::memory_order_seq_cst);
				return;
			}

			
			
			
			
			waitOn(key);
			waiters.fetch_sub(1, std::memory_order_seq_cst);
		}
	}

private:
	void wakeAll();
	void waitOn(uint32_t key);
#endif

public:
	
	
	EventCount(const EventCount &) = delete;
	EventCount &operator=(const EventCount &) = delete;
};



extern EventCount	WorkEC;
extern EventCount	DrainEC;





















extern uint32	S9xRenderSpinUs;



uint32 S9xPickDefaultSpinUs (void);





static inline void S9xSpinHint (void)
{
#if defined(__x86_64__) || defined(__i386__)
	__asm__ volatile("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
	__asm__ volatile("yield" ::: "memory");
#else
	
	
	__asm__ volatile("" ::: "memory");
#endif
}



extern bool8	S9xRenderLagMode;


































#if defined(_WIN32)
#define S9X_RENDER_SPAN_LINES_DEFAULT	8
#else
#define S9X_RENDER_SPAN_LINES_DEFAULT	16
#endif

extern uint32	S9xRenderSpanLines;







extern int32	S9xPendingRTOLine;

void S9xRecordPeriodicSpan (void);
void S9xRecordPendingRTO (void);




void S9xRecordSpan (void);
void S9xRecordCGRAM (uint8 addr, uint16 value);
void S9xRecordOBJ (int index);
void S9xRecordOAMHi (int index, uint8 byte);
void S9xRecordFrameStart (void);
void S9xRecordFrameEnd (void);
void S9xRecordClearRTO (void);



void S9xRenderDrain (void);



void S9xVRAMWriteBarrier (void);



void S9xRenderResync (void);


uint8 S9xRenderRTO (void);












void S9xRenderPublishGeometry (void);



extern uint16		BlackColourMap[256];
extern uint16		DirectColourMaps[8][256];
extern uint8		mul_brightness[16][32];
extern uint8		brightness_cap[64];
extern struct SBG	BG;
extern struct SGFX	GFX;

#define H_FLIP		0x4000
#define V_FLIP		0x8000
#define BLANK_TILE	2

struct COLOR_ADD
{
	static alwaysinline uint16 fn(uint16 C1, uint16 C2)
	{
		const int RED_MASK = 0x1F << RED_SHIFT_BITS;
		const int GREEN_MASK = 0x1F << GREEN_SHIFT_BITS;
		const int BLUE_MASK = 0x1F;

		int rb = C1 & (RED_MASK | BLUE_MASK);
		rb += C2 & (RED_MASK | BLUE_MASK);
		int rbcarry = rb & ((0x20 << RED_SHIFT_BITS) | (0x20 << 0));
		int g = (C1 & (GREEN_MASK)) + (C2 & (GREEN_MASK));
		int rgbsaturate = (((g & (0x20 << GREEN_SHIFT_BITS)) | rbcarry) >> 5) * 0x1f;
		uint16 retval = (rb & (RED_MASK | BLUE_MASK)) | (g & GREEN_MASK) | rgbsaturate;
#if GREEN_SHIFT_BITS == 6
		retval |= (retval & 0x0400) >> 5;
#endif
		return retval;
	}

	static alwaysinline uint16 fn1_2(uint16 C1, uint16 C2)
	{
		return ((((C1 & RGB_REMOVE_LOW_BITS_MASK) +
			(C2 & RGB_REMOVE_LOW_BITS_MASK)) >> 1) +
			(C1 & C2 & RGB_LOW_BITS_MASK)) | ALPHA_BITS_MASK;
	}
};

struct COLOR_ADD_BRIGHTNESS
{
	static alwaysinline uint16 fn(uint16 C1, uint16 C2)
	{
		return ((brightness_cap[ (C1 >> RED_SHIFT_BITS)           +  (C2 >> RED_SHIFT_BITS)          ] << RED_SHIFT_BITS)   |
				(brightness_cap[((C1 >> GREEN_SHIFT_BITS) & 0x1f) + ((C2 >> GREEN_SHIFT_BITS) & 0x1f)] << GREEN_SHIFT_BITS) |
	// Proper 15->16bit color conversion moves the high bit of green into the low bit.
	#if GREEN_SHIFT_BITS == 6
			   ((brightness_cap[((C1 >> 6) & 0x1f) + ((C2 >> 6) & 0x1f)] & 0x10) << 1) |
	#endif
				(brightness_cap[ (C1                      & 0x1f) +  (C2                      & 0x1f)]      ));
	}

	static alwaysinline uint16 fn1_2(uint16 C1, uint16 C2)
	{
		return COLOR_ADD::fn1_2(C1, C2);
	}
};


struct COLOR_SUB
{
	static alwaysinline uint16 fn(uint16 C1, uint16 C2)
	{
		int rb1 = (C1 & (THIRD_COLOR_MASK | FIRST_COLOR_MASK)) | ((0x20 << 0) | (0x20 << RED_SHIFT_BITS));
		int rb2 = C2 & (THIRD_COLOR_MASK | FIRST_COLOR_MASK);
		int rb = rb1 - rb2;
		int rbcarry = rb & ((0x20 << RED_SHIFT_BITS) | (0x20 << 0));
		int g = ((C1 & (SECOND_COLOR_MASK)) | (0x20 << GREEN_SHIFT_BITS)) - (C2 & (SECOND_COLOR_MASK));
		int rgbsaturate = (((g & (0x20 << GREEN_SHIFT_BITS)) | rbcarry) >> 5) * 0x1f;
		uint16 retval = ((rb & (THIRD_COLOR_MASK | FIRST_COLOR_MASK)) | (g & SECOND_COLOR_MASK)) & rgbsaturate;
#if GREEN_SHIFT_BITS == 6
		retval |= (retval & 0x0400) >> 5;
#endif
		return retval;
	}

	static alwaysinline uint16 fn1_2(uint16 C1, uint16 C2)
	{
		return GFX.ZERO[((C1 | RGB_HI_BITS_MASKx2) -
			(C2 & RGB_REMOVE_LOW_BITS_MASK)) >> 1];
	}
};

void S9xStartScreenRefresh (void);
void S9xEndScreenRefresh (void);
void S9xRecordMidLineBrightness (int line, int x, uint8 oldBright, uint8 newBright);
void S9xRecordMidLineWindowSel (int reg, uint8 oldVal, uint8 newVal);
void S9xRecordMidLineScroll (int reg, uint16 oldVal, uint16 newVal);
void S9xInvalidateDirectColourMaps (void);
void S9xBuildDirectColourMaps (void);
void RenderLine (uint8);
void S9xComputeClipWindows (void);
void S9xDisplayChar (uint16 *, uint8);
void S9xGraphicsScreenResize (void);

void S9xDisplayMessages (uint16 *, int, int, int, int);


bool8 S9xGraphicsInit (void);
void S9xGraphicsDeinit (void);
bool8 S9xInitUpdate (void);
bool8 S9xDeinitUpdate (int, int);
bool8 S9xContinueUpdate (int, int);
void S9xReRefresh (void);
void S9xSyncSpeed (void);


extern void (*S9xCustomDisplayString) (const char *, int, int, bool, int type);
void S9xVariableDisplayString(const char* string, int linesFromBottom, int pixelsFromLeft, bool allowWrap, int type);

#endif
