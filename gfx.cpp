/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "ppu.h"
#include "tile.h"
#include "controls.h"
#include "crosshairs.h"
#include "cheats.h"
#include "movie.h"
#include "screenshot.h"
#include "display.h"
#include <cstdlib>

#if defined(S9X_EC_FUTEX)
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sched.h>
#endif


extern struct SCheatData		Cheat;
extern struct SLineData			LineData[240];
extern struct SLineMatrixData	LineMatrixData[240];

void S9xComputeClipWindows (void);

void (*S9xCustomDisplayString) (const char *, int, int, bool, int) = NULL;

static void SetupOBJ (const SRenderRegs *R);
static void DrawOBJS (const SRenderRegs *R, int D);
static void DisplayTime (void);
static void DisplayFrameRate (void);
static void DisplayPressedKeys (void);
static void DisplayWatchedAddresses (void);
static void DisplayStringFromBottom (const char *, int, int, bool);
static void DrawBackground (const SRenderRegs *R, int, uint8, uint8);
static void DrawBackgroundMosaic (const SRenderRegs *R, int, uint8, uint8);
static void DrawBackgroundOffset (const SRenderRegs *R, int, uint8, uint8, int);
static void DrawBackgroundOffsetMosaic (const SRenderRegs *R, int, uint8, uint8, int);
static inline void DrawBackgroundMode7 (const SRenderRegs *R, int, void (*DrawMath) (uint32, uint32, int), void (*DrawNomath) (uint32, uint32, int), int);
static inline void DrawBackdrop (const SRenderRegs *R);
static inline void RenderScreen (const SRenderRegs *R, bool8);
static void DoRenderSpan (const SRenderRegs *R, uint32 StartY, uint32 EndY, uint8 SpanFlags);
static uint16 get_crosshair_color (uint8);
static void S9xDisplayStringType (const char *, int, int, bool, int);

#define TILE_PLUS(t, x)	(((t) & 0xfc00) | ((t + x) & 0x3ff))





struct SRenderLog	RenderLog;
struct SRenderState	RS;
struct SResyncPayload	S9xResyncPayload;





static_assert(sizeof(SRenderCommand) <= 512,
	"SRenderCommand grew: a union member is setting an expensive ring stride");





alignas(64) std::atomic<uint64_t>	RenderProduced(0);
alignas(64) std::atomic<uint64_t>	RenderConsumed(0);

EventCount		WorkEC;
EventCount		DrainEC;

#if !defined(S9X_EC_CONDVAR)




static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t),
	"EventCount::epoch must be a bare 32-bit word to be waitable");
static_assert(ATOMIC_INT_LOCK_FREE == 2,
	"EventCount::epoch must be lock-free");

void EventCount::wakeAll()
{
#if defined(S9X_EC_FUTEX)
	syscall(SYS_futex, reinterpret_cast<uint32_t *>(&epoch),
		FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
#else
	epoch.notify_all();
#endif
}

void EventCount::waitOn(uint32_t key)
{
#if defined(S9X_EC_FUTEX)
	
	
	
	
	syscall(SYS_futex, reinterpret_cast<uint32_t *>(&epoch),
		FUTEX_WAIT_PRIVATE, key, NULL, NULL, 0);
#else
	epoch.wait(key);
#endif
}
#endif

static std::thread	RenderThread;

#define RT_STAT_ADD(i, n)	((void) 0)
#define RT_STAT_MAX(i, n)	((void) 0)
#define RT_STAT_RECORD(c)	((void) 0)
#define RT_STAT_FRAME_OPEN()	((void) 0)
#define RT_STAT_FRAME_CLOSE()	((void) 0)
#define RT_STAT_SPAN(s, e, l)	((void) 0)


static void RenderThreadMain(void);















static inline SRenderCommand *PushRecordBeginSync(void)
{
	uint32	prod = RenderLog.produced;
	uint32	next = (prod + 1) & (S9X_RENDER_RING_CAPACITY - 1);
	if (next == RenderLog.consumed)
		S9xRenderDrain();	
	return &RenderLog.ring[RenderLog.produced & (S9X_RENDER_RING_CAPACITY - 1)];
}

static inline void PushRecordEndSync(bool notify)
{
	RT_STAT_RECORD(RenderLog.ring[RenderLog.produced & (S9X_RENDER_RING_CAPACITY - 1)]);
	RenderLog.produced = (RenderLog.produced + 1) & (S9X_RENDER_RING_CAPACITY - 1);
	if (notify && !S9xRenderLagMode)
		S9xRenderDrain();
}

static inline SRenderCommand *PushRecordBeginThreaded(void)
{
	
	
	uint64_t	prod;
	for (;;)
	{
		prod = RenderProduced.load(std::memory_order_relaxed);
		uint64_t	cons = RenderConsumed.load(std::memory_order_acquire);
		if (prod - cons < S9X_RENDER_RING_CAPACITY - 1)
			break;
		
		RT_STAT_ADD(S9X_RTS_RING_FULL_YIELDS, 1);
		std::this_thread::yield();
	}
	return &RenderLog.ring[prod & (S9X_RENDER_RING_CAPACITY - 1)];
}

static inline void PushRecordEndThreaded(void)
{
	
	
	RT_STAT_RECORD(RenderLog.ring[RenderProduced.load(std::memory_order_relaxed) &
		(S9X_RENDER_RING_CAPACITY - 1)]);
	
	
	
	
	RenderProduced.fetch_add(1ULL, std::memory_order_seq_cst);
	WorkEC.notify();
}




















uint32			S9xRenderSpinUs = 0;



uint32			S9xRenderSpanLines = 0;
int32			S9xPendingRTOLine = -1;

















static unsigned S9xAvailableCPUs (void)
{
#if defined(__linux__)
	cpu_set_t	set;
	CPU_ZERO(&set);
	if (sched_getaffinity(0, sizeof(set), &set) == 0)
	{
		const int	n = CPU_COUNT(&set);
		if (n > 0)
			return (unsigned) n;
	}
#endif
	const unsigned	hc = std::thread::hardware_concurrency();
	return hc ? hc : 1;	
}



































uint32 S9xPickDefaultSpinUs (void)
{
#if defined(_WIN32)
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	
	const unsigned n = S9xAvailableCPUs();
	return (n <= 2) ? 0 : (n < 32) ? 80 : 640;
#else
	return (S9xAvailableCPUs() <= 1) ? 0 : 640;
#endif
}

bool8			S9xRenderLagMode = FALSE;
const SRenderRegs	*S9xCurRenderRegs = NULL;

bool8 S9xGraphicsInit (void)
{
	S9xInitTileRenderer();
	memset(BlackColourMap, 0, 256 * sizeof(uint16));

	IPPU.OBJChanged = TRUE;
	Settings.BG_Forced = 0;
	Settings.ForcedBackdrop = 0;

	
	RS.OBJ = RS.OBJ_data;
	memcpy(RS.OBJ_data, PPU.OBJ, sizeof(RS.OBJ_data));
	RS.cgram_version  = 1;
	RS.obj_version    = 1;
	RS.cgram_applied  = 0;
	RS.obj_applied    = 0;
	RS.last_obj_first_sprite = -1;	
	RS.last_obj_size_select  = -1;
	RS.last_brightness = 0xff; 
	RS.RecomputeClipWindows = TRUE;
	RS.MosaicStart  = 0;
	RS.RangeTimeOver = 0;
	memcpy(RS.CGDATA, PPU.CGDATA, sizeof(RS.CGDATA));

	
	
	S9xFixColourBrightness();
	RS.XB             = IPPU.XB;
	RS.last_brightness = PPU.Brightness;
	
	for (int i = 0; i < 256; i++)
	{
		RS.Red[i]          = IPPU.Red[i];
		RS.Green[i]        = IPPU.Green[i];
		RS.Blue[i]         = IPPU.Blue[i];
		RS.ScreenColors[i] = IPPU.ScreenColors[i];
	}
	RS.cgram_applied = RS.cgram_version;
	S9xInvalidateDirectColourMaps();

	GFX.ScreenBuffer.resize(MAX_SNES_WIDTH * (MAX_SNES_HEIGHT + 64));
	GFX.Screen = &GFX.ScreenBuffer[GFX.RealPPL * 32];
	GFX.ZERO = (uint16 *) malloc(sizeof(uint16) * 0x10000);
	GFX.SubScreen  = (uint16 *) malloc(GFX.ScreenSize * sizeof(uint16));
	GFX.ZBuffer    = (uint8 *)  malloc(GFX.ScreenSize);
	GFX.SubZBuffer = (uint8 *)  malloc(GFX.ScreenSize);

	if (!GFX.ZERO || !GFX.SubScreen || !GFX.ZBuffer || !GFX.SubZBuffer)
	{
		S9xGraphicsDeinit();
		return (FALSE);
	}

	memset(GFX.ZERO, 0, 0x10000 * sizeof(uint16));
	for (uint32 r = 0; r <= MAX_RED; r++)
	{
		uint32	r2 = r;
		if (r2 & 0x10)
			r2 &= ~0x10;
		else
			r2 = 0;

		for (uint32 g = 0; g <= MAX_GREEN; g++)
		{
			uint32	g2 = g;
			if (g2 & GREEN_HI_BIT)
				g2 &= ~GREEN_HI_BIT;
			else
				g2 = 0;

			for (uint32 b = 0; b <= MAX_BLUE; b++)
			{
				uint32	b2 = b;
				if (b2 & 0x10)
					b2 &= ~0x10;
				else
					b2 = 0;

				GFX.ZERO[BUILD_PIXEL2(r, g, b)] = BUILD_PIXEL2(r2, g2, b2);
				GFX.ZERO[BUILD_PIXEL2(r, g, b) & ~ALPHA_BITS_MASK] = BUILD_PIXEL2(r2, g2, b2);
			}
		}
	}

	
	
	
	if (Settings.ThreadedRender)
	{
		
		
		S9xRenderSpinUs = S9xPickDefaultSpinUs();


		
		
		
		S9xRenderSpanLines = S9X_RENDER_SPAN_LINES_DEFAULT;
		S9xPendingRTOLine = -1;

		RenderProduced.store(0, std::memory_order_relaxed);
		RenderConsumed.store(0, std::memory_order_relaxed);
		RenderThread = std::thread(RenderThreadMain);
	}

	return (TRUE);
}

void S9xGraphicsDeinit (void)
{
	
	if (Settings.ThreadedRender && RenderThread.joinable())
	{
		
		
		
		SRenderCommand *cmd = PushRecordBeginThreaded();
		cmd->tag = RTAG_SHUTDOWN;
		PushRecordEndThreaded();
		RenderThread.join();
	}

	if (GFX.ZERO)       { free(GFX.ZERO);       GFX.ZERO       = NULL; }
	if (GFX.SubScreen)  { free(GFX.SubScreen);  GFX.SubScreen  = NULL; }
	if (GFX.ZBuffer)    { free(GFX.ZBuffer);    GFX.ZBuffer    = NULL; }
	if (GFX.SubZBuffer) { free(GFX.SubZBuffer); GFX.SubZBuffer = NULL; }
}

void S9xGraphicsScreenResize (void)
{
	IPPU.MaxBrightness = PPU.Brightness;

	IPPU.Interlace    = Memory.FillRAM[0x2133] & 1;
	IPPU.InterlaceOBJ = Memory.FillRAM[0x2133] & 2;
	IPPU.PseudoHires = Memory.FillRAM[0x2133] & 8;

	if (PPU.BGMode == 5 || PPU.BGMode == 6 || IPPU.PseudoHires || IPPU.Interlace)
	{
		IPPU.DoubleWidthPixels = TRUE;
		IPPU.RenderedScreenWidth = SNES_WIDTH << 1;
	}
	else
	{
		IPPU.DoubleWidthPixels = FALSE;
		IPPU.RenderedScreenWidth = SNES_WIDTH;
	}

	if (IPPU.Interlace)
	{
		GFX.PPL = GFX.RealPPL << 1;
		IPPU.DoubleHeightPixels = TRUE;
		IPPU.RenderedScreenHeight = PPU.ScreenHeight << 1;
		GFX.DoInterlace++;
	}
	else
	{
		GFX.PPL = GFX.RealPPL;
		IPPU.DoubleHeightPixels = FALSE;
		IPPU.RenderedScreenHeight = PPU.ScreenHeight;
	}
}

static bool8	DirectColourMapsDirty = TRUE;

void S9xInvalidateDirectColourMaps (void)
{
	DirectColourMapsDirty = TRUE;
}

void S9xBuildDirectColourMaps (void)
{
	if (!DirectColourMapsDirty)
		return;
	DirectColourMapsDirty = FALSE;

	for (uint32 p = 0; p < 8; p++)
		for (uint32 c = 0; c < 256; c++)
			DirectColourMaps[p][c] = BUILD_PIXEL(RS.XB[((c & 7) << 2) | ((p & 1) << 1)], RS.XB[((c & 0x38) >> 1) | (p & 2)], RS.XB[((c & 0xc0) >> 3) | (p & 4)]);
}





#define MAX_BRIGHT_EVENTS 64
static struct
{
	uint8	line, x, oldB, newB;
}	bright_events[MAX_BRIGHT_EVENTS];
static int		bright_event_count = 0;
static uint8	line_brightness[240];

void S9xRecordMidLineBrightness (int line, int x, uint8 oldBright, uint8 newBright)
{
	if (bright_event_count >= MAX_BRIGHT_EVENTS || line > 239)
		return;
	bright_events[bright_event_count].line = (uint8) line;
	bright_events[bright_event_count].x    = (uint8) x;
	bright_events[bright_event_count].oldB = oldBright;
	bright_events[bright_event_count].newB = newBright;
	bright_event_count++;
}

static void S9xApplyMidLineBrightness (void);

#define MAX_RASTER_EVENTS 1536
static struct
{
	uint8	line, x, cls, reg;
	uint16	oldV, newV;
}	raster_events[MAX_RASTER_EVENTS];
static int	raster_event_count = 0;

static int		raster_span_count = 0;
static uint16	raster_span_l[2], raster_span_r[2];

static uint8	line_windows[240][4];
static uint16	line_backdrop[240];

static bool mid_line_event_pos (int &line, int &x)
{
	if (!IPPU.RenderThisFrame || raster_event_count >= MAX_RASTER_EVENTS)
		return (false);
	if (CPU.V_Counter < FIRST_VISIBLE_LINE || CPU.V_Counter >= PPU.ScreenHeight + FIRST_VISIBLE_LINE)
		return (false);
	if (CPU.Cycles >= Timings.HBlankStart)
		return (false);
	line = CPU.V_Counter - FIRST_VISIBLE_LINE;
	x = CPU.Cycles / ONE_DOT_CYCLE - 22;
	return (x >= 1 && x <= 255 && line <= 239);
}

void S9xRecordMidLineWindowSel (int reg, uint8 oldVal, uint8 newVal)
{
	int	line, x;
	if (oldVal == newVal || !mid_line_event_pos(line, x))
		return;
	raster_events[raster_event_count].line = (uint8) line;
	raster_events[raster_event_count].x    = (uint8) x;
	raster_events[raster_event_count].cls  = 0;
	raster_events[raster_event_count].reg  = (uint8) reg;
	raster_events[raster_event_count].oldV = oldVal;
	raster_events[raster_event_count].newV = newVal;
	raster_event_count++;
}

void S9xRecordMidLineScroll (int reg, uint16 oldVal, uint16 newVal)
{
	int	line, x;
	if (oldVal == newVal || PPU.BGMode == 7 || !mid_line_event_pos(line, x))
		return;
	raster_events[raster_event_count].line = (uint8) line;
	raster_events[raster_event_count].x    = (uint8) x;
	raster_events[raster_event_count].cls  = 1;
	raster_events[raster_event_count].reg  = (uint8) reg;
	raster_events[raster_event_count].oldV = oldVal;
	raster_events[raster_event_count].newV = newVal;
	raster_event_count++;
}






static void apply_byte_reg (int reg, uint8 byte)
{
	if (reg == 3)
	{
		PPU.Mosaic     = (byte >> 4) + 1;
		PPU.BGMosaic[0] = (byte & 1);
		PPU.BGMosaic[1] = (byte & 2);
		PPU.BGMosaic[2] = (byte & 4);
		PPU.BGMosaic[3] = (byte & 8);
		return;
	}

	const int	n = reg * 2;
	PPU.ClipWindow1Enable[n]     = !!(byte & 0x02);
	PPU.ClipWindow1Enable[n + 1] = !!(byte & 0x20);
	PPU.ClipWindow2Enable[n]     = !!(byte & 0x08);
	PPU.ClipWindow2Enable[n + 1] = !!(byte & 0x80);
	PPU.ClipWindow1Inside[n]     = !(byte & 0x01);
	PPU.ClipWindow1Inside[n + 1] = !(byte & 0x10);
	PPU.ClipWindow2Inside[n]     = !(byte & 0x04);
	PPU.ClipWindow2Inside[n + 1] = !(byte & 0x40);
}

static inline uint16 byte_reg_addr (int reg)
{
	return (reg == 3) ? 0x2106 : (0x2123 + reg);
}








static void SnapshotRenderRegs (SRenderRegs *out);

void S9xUpdateScreen (void)
{
	
	
	
	S9xPendingRTOLine = -1;

	uint32	StartY = (uint32) IPPU.PreviousLine;
	uint32	EndY   = (uint32) IPPU.CurrentLine - 1;
	if (EndY >= PPU.ScreenHeight)
		EndY = PPU.ScreenHeight - 1;

	
	
	
	
	
	
	
	
	RT_STAT_SPAN(StartY, EndY, (uint32) IPPU.CurrentLine);

	SRenderCommand *cmd;
	if (Settings.ThreadedRender)
	{
		cmd = PushRecordBeginThreaded();
		cmd->tag         = RTAG_SPAN;
		cmd->span.StartY = StartY;
		cmd->span.EndY   = EndY;
		cmd->span.flags  = 0;
		SnapshotRenderRegs(&cmd->span.regs);
		IPPU.PreviousLine = IPPU.CurrentLine;
		IPPU.OBJChanged   = FALSE;
		PushRecordEndThreaded();
	}
	else
	{
		cmd = PushRecordBeginSync();
		cmd->tag         = RTAG_SPAN;
		cmd->span.StartY = StartY;
		cmd->span.EndY   = EndY;
		cmd->span.flags  = 0;
		SnapshotRenderRegs(&cmd->span.regs);
		IPPU.PreviousLine = IPPU.CurrentLine;
		
		
		
		
		IPPU.OBJChanged = FALSE;
		PushRecordEndSync(true);
	}
}


void S9xRecordSpan (void)
{
	S9xUpdateScreen();
}
















void S9xRecordPeriodicSpan (void)
{
	uint32	StartY = (uint32) IPPU.PreviousLine;
	uint32	EndY   = (uint32) IPPU.CurrentLine - 1;
	if (EndY >= PPU.ScreenHeight)
		EndY = PPU.ScreenHeight - 1;
	if (EndY < StartY)
		return;

	RT_STAT_SPAN(StartY, EndY, (uint32) IPPU.CurrentLine);

	SRenderCommand *cmd = PushRecordBeginThreaded();
	cmd->tag         = RTAG_SPAN;
	cmd->span.StartY = StartY;
	cmd->span.EndY   = EndY;
	cmd->span.flags  = S9X_SPAN_NO_RTO;
	SnapshotRenderRegs(&cmd->span.regs);
	IPPU.PreviousLine = IPPU.CurrentLine;
	IPPU.OBJChanged   = FALSE;
	PushRecordEndThreaded();

	
	
	
	S9xPendingRTOLine = (int32) EndY;
}





void S9xRecordPendingRTO (void)
{
	const uint32	line = (uint32) S9xPendingRTOLine;
	S9xPendingRTOLine = -1;

	SRenderCommand *cmd = PushRecordBeginThreaded();
	cmd->tag = RTAG_RTO_ACCUM;
	cmd->rto_accum.line = line;
	PushRecordEndThreaded();
}





static void SnapshotRenderRegs (SRenderRegs *out)
{
	for (int i = 0; i < 4; i++)
	{
		out->BG[i].SCBase  = PPU.BG[i].SCBase;
		out->BG[i].BGSize  = PPU.BG[i].BGSize;
		out->BG[i].NameBase = PPU.BG[i].NameBase;
		out->BG[i].SCSize  = PPU.BG[i].SCSize;
	}
	out->BGMode        = PPU.BGMode;
	out->BG3Priority   = PPU.BG3Priority;
	out->Mosaic        = PPU.Mosaic;
	out->MosaicStart   = PPU.MosaicStart;
	for (int i = 0; i < 4; i++)
		out->BGMosaic[i] = PPU.BGMosaic[i];
	out->Window1Left   = PPU.Window1Left;
	out->Window1Right  = PPU.Window1Right;
	out->Window2Left   = PPU.Window2Left;
	out->Window2Right  = PPU.Window2Right;
	for (int i = 0; i < 6; i++)
	{
		out->ClipCounts[i]             = PPU.ClipCounts[i];
		out->ClipWindowOverlapLogic[i] = PPU.ClipWindowOverlapLogic[i];
		out->ClipWindow1Enable[i]      = PPU.ClipWindow1Enable[i];
		out->ClipWindow2Enable[i]      = PPU.ClipWindow2Enable[i];
		out->ClipWindow1Inside[i]      = PPU.ClipWindow1Inside[i];
		out->ClipWindow2Inside[i]      = PPU.ClipWindow2Inside[i];
	}
	out->RecomputeClipWindows = PPU.RecomputeClipWindows;
	out->ForcedBlanking       = PPU.ForcedBlanking;
	out->FixedColourRed       = PPU.FixedColourRed;
	out->FixedColourGreen     = PPU.FixedColourGreen;
	out->FixedColourBlue      = PPU.FixedColourBlue;
	out->Brightness           = PPU.Brightness;
	out->ScreenHeight         = PPU.ScreenHeight;
	out->Mode7HFlip           = PPU.Mode7HFlip;
	out->Mode7VFlip           = PPU.Mode7VFlip;
	out->Mode7Repeat          = PPU.Mode7Repeat;
	out->OBJSizeSelect        = PPU.OBJSizeSelect;
	out->OBJNameBase          = PPU.OBJNameBase;
	out->OBJNameSelect        = PPU.OBJNameSelect;
	out->OAMAddr              = PPU.OAMAddr;
	out->OAMFlip              = PPU.OAMFlip;
	out->OAMPriorityRotation  = PPU.OAMPriorityRotation;
	out->FirstSprite          = PPU.FirstSprite;
	out->Interlace            = IPPU.Interlace;
	out->InterlaceOBJ         = IPPU.InterlaceOBJ;
	out->InterlaceField       = S9xInterlaceField();
	out->PseudoHires          = IPPU.PseudoHires;
	out->MaxBrightness        = IPPU.MaxBrightness;
	out->RenderedScreenWidth  = IPPU.RenderedScreenWidth;
	out->RenderedScreenHeight = IPPU.RenderedScreenHeight;
	out->DoubleWidthPixels    = IPPU.DoubleWidthPixels;
	out->DoubleHeightPixels   = IPPU.DoubleHeightPixels;
	out->RenderThisFrame      = IPPU.RenderThisFrame;
	
	memcpy(out->FillRAM, &Memory.FillRAM[0x2100], 0x34);
}





void S9xRecordCGRAM (uint8 addr, uint16 value)
{
	SRenderCommand *cmd;
	if (Settings.ThreadedRender)
	{
		cmd = PushRecordBeginThreaded();
		cmd->tag        = RTAG_CGRAM;
		cmd->cgram.addr  = addr;
		cmd->cgram.value = value;
		PushRecordEndThreaded();
	}
	else
	{
		cmd = PushRecordBeginSync();
		cmd->tag        = RTAG_CGRAM;
		cmd->cgram.addr  = addr;
		cmd->cgram.value = value;
		PushRecordEndSync(true);
	}
}

void S9xRecordOBJ (int index)
{
	const SOBJ &o = PPU.OBJ[index];
	SRenderCommand *cmd;
	if (Settings.ThreadedRender)
	{
		cmd = PushRecordBeginThreaded();
		cmd->tag       = RTAG_OBJ;
		cmd->obj.index = (uint8) index;
		cmd->obj.HPos     = o.HPos;
		cmd->obj.VPos     = o.VPos;
		cmd->obj.HFlip    = o.HFlip;
		cmd->obj.VFlip    = o.VFlip;
		cmd->obj.Name     = o.Name;
		cmd->obj.Priority = o.Priority;
		cmd->obj.Palette  = o.Palette;
		cmd->obj.Size     = o.Size;
		PushRecordEndThreaded();
	}
	else
	{
		cmd = PushRecordBeginSync();
		cmd->tag       = RTAG_OBJ;
		cmd->obj.index = (uint8) index;
		cmd->obj.HPos     = o.HPos;
		cmd->obj.VPos     = o.VPos;
		cmd->obj.HFlip    = o.HFlip;
		cmd->obj.VFlip    = o.VFlip;
		cmd->obj.Name     = o.Name;
		cmd->obj.Priority = o.Priority;
		cmd->obj.Palette  = o.Palette;
		cmd->obj.Size     = o.Size;
		PushRecordEndSync(true);
	}
}

void S9xRecordOAMHi (int index, uint8 byte)
{
	SRenderCommand *cmd;
	if (Settings.ThreadedRender)
	{
		cmd = PushRecordBeginThreaded();
		cmd->tag          = RTAG_OAMHI;
		cmd->oamhi.index  = (uint8) index;
		cmd->oamhi.byte   = byte;
		PushRecordEndThreaded();
	}
	else
	{
		cmd = PushRecordBeginSync();
		cmd->tag          = RTAG_OAMHI;
		cmd->oamhi.index  = (uint8) index;
		cmd->oamhi.byte   = byte;
		PushRecordEndSync(true);
	}
}

void S9xRecordFrameStart (void)
{
	SRenderCommand *cmd;
	RT_STAT_FRAME_OPEN();
	S9xPendingRTOLine = -1;
	if (Settings.ThreadedRender)
	{
		cmd = PushRecordBeginThreaded();
		cmd->tag = RTAG_FRAME_START;
		cmd->frame_start.dummy = 0;
		PushRecordEndThreaded();
	}
	else
	{
		cmd = PushRecordBeginSync();
		cmd->tag = RTAG_FRAME_START;
		cmd->frame_start.dummy = 0;
		PushRecordEndSync(true);
	}
}

void S9xRecordFrameEnd (void)
{
	SRenderCommand *cmd;
	if (Settings.ThreadedRender)
		cmd = PushRecordBeginThreaded();
	else
		cmd = PushRecordBeginSync();

	
	
	
	
	
	
	cmd->tag = RTAG_FRAME_END;
	cmd->frame_end.dummy = 0;

	
	
	bright_event_count = 0;
	raster_event_count = 0;

	if (Settings.ThreadedRender)
		PushRecordEndThreaded();
	else
		PushRecordEndSync(false);	

	
	S9xRenderDrain();
}

void S9xRecordClearRTO (void)
{
	SRenderCommand *cmd;
	if (Settings.ThreadedRender)
	{
		cmd = PushRecordBeginThreaded();
		cmd->tag = RTAG_CLEAR_RTO;
		cmd->clear_rto.dummy = 0;
		PushRecordEndThreaded();
	}
	else
	{
		cmd = PushRecordBeginSync();
		cmd->tag = RTAG_CLEAR_RTO;
		cmd->clear_rto.dummy = 0;
		PushRecordEndSync(true);
	}
}





void S9xRenderResync (void)
{
	
	S9xRenderDrain();

	
	SRenderCommand *cmd;
	if (Settings.ThreadedRender)
		cmd = PushRecordBeginThreaded();
	else
		cmd = PushRecordBeginSync();

	cmd->tag = RTAG_RESYNC;
	SnapshotRenderRegs(&cmd->resync.regs);
	
	
	
	memcpy(S9xResyncPayload.CGDATA, PPU.CGDATA, sizeof(S9xResyncPayload.CGDATA));
	memcpy(S9xResyncPayload.OBJ, PPU.OBJ, sizeof(S9xResyncPayload.OBJ));

	if (Settings.ThreadedRender)
		PushRecordEndThreaded();
	else
		PushRecordEndSync(false);

	S9xRenderDrain();
}

uint8 S9xRenderRTO (void)
{
	S9xRenderDrain();
	return RS.RangeTimeOver;
}





void S9xRenderPublishGeometry (void)
{
	if (RS.HiresWidened)
	{
		IPPU.DoubleWidthPixels   = TRUE;
		IPPU.RenderedScreenWidth = 512;
	}
	if (RS.HeightDoubled)
	{
		IPPU.DoubleHeightPixels   = TRUE;
		IPPU.RenderedScreenHeight = RS.DoubledScreenHeight;
	}
}






static void rerender_line_span (int line, int x0, int x1)
{
	raster_span_l[0] = (uint16) x0;
	raster_span_r[0] = (uint16) x1;
	raster_span_count = 1;

	const uint8	savedWin[4] =
	{
		Memory.FillRAM[0x2126], Memory.FillRAM[0x2127],
		Memory.FillRAM[0x2128], Memory.FillRAM[0x2129],
	};
	PPU.Window1Left  = line_windows[line][0];
	PPU.Window1Right = line_windows[line][1];
	PPU.Window2Left  = line_windows[line][2];
	PPU.Window2Right = line_windows[line][3];
	
	Memory.FillRAM[0x2126] = line_windows[line][0];
	Memory.FillRAM[0x2127] = line_windows[line][1];
	Memory.FillRAM[0x2128] = line_windows[line][2];
	Memory.FillRAM[0x2129] = line_windows[line][3];

	const uint16	savedBackdrop = PPU.CGDATA[0];
	const uint16	savedScreen0  = RS.ScreenColors[0];
	const uint8	savedR0 = RS.Red[0], savedG0 = RS.Green[0], savedB0 = RS.Blue[0];
	{
		const uint16	c = line_backdrop[line];
		PPU.CGDATA[0] = c;
		RS.CGDATA[0]  = c;
		RS.Red[0]     = RS.XB[c & 0x1f];
		RS.Green[0]   = RS.XB[(c >> 5) & 0x1f];
		RS.Blue[0]    = RS.XB[(c >> 10) & 0x1f];
		RS.ScreenColors[0] = (uint16) BUILD_PIXEL(RS.Red[0], RS.Green[0], RS.Blue[0]);
	}

	uint32	zrow = line * GFX.PPL + ((GFX.DoInterlace && S9xInterlaceField()) ? GFX.RealPPL : 0);
	memset(GFX.ZBuffer + zrow, 0, IPPU.RenderedScreenWidth);
	memset(GFX.SubZBuffer + zrow, 0, IPPU.RenderedScreenWidth);

	IPPU.PreviousLine = line;
	IPPU.CurrentLine  = line + 1;
	PPU.RecomputeClipWindows = TRUE;
	S9xUpdateScreen();
	
	
	
	
	
	
	S9xRenderDrain();
	raster_span_count = 0;

	Memory.FillRAM[0x2126] = savedWin[0];
	Memory.FillRAM[0x2127] = savedWin[1];
	Memory.FillRAM[0x2128] = savedWin[2];
	Memory.FillRAM[0x2129] = savedWin[3];
	PPU.Window1Left  = savedWin[0];
	PPU.Window1Right = savedWin[1];
	PPU.Window2Left  = savedWin[2];
	PPU.Window2Right = savedWin[3];
	PPU.CGDATA[0]        = savedBackdrop;
	RS.CGDATA[0]         = savedBackdrop;
	RS.ScreenColors[0]   = savedScreen0;
	RS.Red[0]   = savedR0;
	RS.Green[0] = savedG0;
	RS.Blue[0]  = savedB0;
}

static void S9xApplyMidLineBrightness (void)
{
	if (!bright_event_count)
		return;

	const uint32	savedPrev = IPPU.PreviousLine;
	const uint32	savedCur  = IPPU.CurrentLine;
	const int	xscale = IPPU.DoubleWidthPixels ? 2 : 1;

	for (int i = 0; i < bright_event_count; )
	{
		const int	line = bright_events[i].line;
		int	end = i;
		while (end < bright_event_count && bright_events[end].line == line)
			end++;

		if (line >= (int) PPU.ScreenHeight)
			{ i = end; continue; }

		const uint8	base = bright_events[i].oldB;

		if (line_brightness[line] != base)
		{
			const uint8	savedBright = PPU.Brightness;
			PPU.Brightness = base;
			S9xFixColourBrightness();
			S9xInvalidateDirectColourMaps();
			rerender_line_span(line, 0, 256);
			PPU.Brightness = savedBright;
			S9xFixColourBrightness();
			S9xInvalidateDirectColourMaps();
		}

		for (int j = i; j < end; j++)
		{
			const int	trueB = bright_events[j].newB;
			const int	x0 = bright_events[j].x;
			const int	x1 = (j + 1 < end) ? bright_events[j + 1].x : 256;
			if (trueB == base || x1 <= x0)
				continue;

			const int	num = trueB + 1;
			const int	den = base + 1;

			uint16	*p = GFX.Screen + line * GFX.PPL;
			if (GFX.DoInterlace && S9xInterlaceField())
				p += GFX.RealPPL;

			for (int x = x0 * xscale; x < x1 * xscale; x++)
			{
				uint32	r, g, b;
				DECOMPOSE_PIXEL(p[x], r, g, b);
				r = r * num / den; if (r > 31) r = 31;
				g = g * num / den; if (g > 31) g = 31;
				b = b * num / den; if (b > 31) b = 31;
				p[x] = BUILD_PIXEL(r, g, b);
			}
		}

		i = end;
	}

	IPPU.PreviousLine = savedPrev;
	IPPU.CurrentLine  = savedCur;
}

static void RestrictClipWindows (void)
{
	for (int s = 0; s <= 1; s++)
	{
		for (int l = 0; l < 6; l++)
		{
			struct ClipData	*c = &IPPU.Clip[s][l];
			if (!c->Count)
				continue;

			uint16	L[6], R[6];
			uint8	M[6];
			int		n = 0;

			for (int sp = 0; sp < raster_span_count; sp++)
			{
				for (int k = 0; k < c->Count && n < 6; k++)
				{
					uint16	a = c->Left[k]  > raster_span_l[sp] ? c->Left[k]  : raster_span_l[sp];
					uint16	b = c->Right[k] < raster_span_r[sp] ? c->Right[k] : raster_span_r[sp];
					if (a < b)
					{
						L[n] = a;
						R[n] = b;
						M[n] = c->DrawMode[k];
						n++;
					}
				}
			}

			c->Count = (uint8) n;
			for (int k = 0; k < n; k++)
			{
				c->Left[k] = L[k];
				c->Right[k] = R[k];
				c->DrawMode[k] = M[k];
			}
		}
	}
}

static void S9xApplyMidLineRaster (void)
{
	if (!raster_event_count)
		return;

	const uint32	savedPrev = IPPU.PreviousLine;
	const uint32	savedCur  = IPPU.CurrentLine;

	for (int i = 0; i < raster_event_count; )
	{
		const int	line = raster_events[i].line;
		int	end = i;
		while (end < raster_event_count && raster_events[end].line == line)
			end++;

		for (int cls = 0; line < (int) PPU.ScreenHeight && cls < 2; cls++)
		{
			uint16	firstOld[8], lastNew[8], savedOfs[8];
			uint8	firstXPerReg[8], lastXPerReg[8];
			bool	touched[8] = { false };
			int		maxFirstX = 0, minFirstX = 256;
			bool	any = false;

			for (int j = i; j < end; j++)
			{
				if (raster_events[j].cls != cls)
					continue;
				const int	r = raster_events[j].reg;
				if (!touched[r])
				{
					touched[r] = true;
					firstOld[r] = raster_events[j].oldV;
					firstXPerReg[r] = raster_events[j].x;
					if (raster_events[j].x > maxFirstX)
						maxFirstX = raster_events[j].x;
					if (raster_events[j].x < minFirstX)
						minFirstX = raster_events[j].x;
				}
				lastNew[r] = raster_events[j].newV;
				lastXPerReg[r] = raster_events[j].x;
				any = true;
			}
			if (!any)
				continue;

			int	minLastX = 256;
			for (int r = 0; r < 8; r++)
				if (touched[r] && (cls == 0 || !(r & 1)) && lastXPerReg[r] < minLastX)
					minLastX = lastXPerReg[r];
			int	leftEnd = minFirstX + 2;
			if (cls == 0)
			{
				for (int r = 0; r < 3; r++)
					if (touched[r] && firstXPerReg[r] + 2 > leftEnd)
						leftEnd = firstXPerReg[r] + 2;
				if (touched[3])
				{
					const int	onset = firstXPerReg[3] + 14;
					if (onset > leftEnd)
						leftEnd = onset;
				}
			}
			int	rightStart = (minLastX >= 256) ? 256 : minLastX + (cls ? 8 : 2);
			if (rightStart < leftEnd)
				rightStart = leftEnd;

			for (int side = 0; side < 2; side++)
			{
				raster_span_count = 0;
				if (side == 0 && maxFirstX > 1 && leftEnd < 256)
					{ raster_span_l[0] = 0; raster_span_r[0] = leftEnd; raster_span_count = 1; }
				if (side == 1 && rightStart < 255)
					{ raster_span_l[0] = rightStart; raster_span_r[0] = 256; raster_span_count = 1; }
				if (!raster_span_count)
					continue;

				const uint16	*vals = side ? lastNew : firstOld;
				for (int r = 0; r < 8; r++)
				{
					if (!touched[r])
						continue;
					if (cls == 0)
						apply_byte_reg(r, (uint8) vals[r]);
					else if (r & 1)
					{
						savedOfs[r] = LineData[line].BG[r >> 1].VOffset;
						LineData[line].BG[r >> 1].VOffset = vals[r] + 1;
					}
					else
					{
						savedOfs[r] = LineData[line].BG[r >> 1].HOffset;
						LineData[line].BG[r >> 1].HOffset = vals[r];
					}
				}

				rerender_line_span(line, raster_span_l[0], raster_span_r[0]);

				for (int r = 0; r < 8; r++)
				{
					if (!touched[r])
						continue;
					if (cls == 0)
						apply_byte_reg(r, Memory.FillRAM[byte_reg_addr(r)]);
					else if (r & 1)
						LineData[line].BG[r >> 1].VOffset = savedOfs[r];
					else
						LineData[line].BG[r >> 1].HOffset = savedOfs[r];
				}
			}
			PPU.RecomputeClipWindows = TRUE;
		}

		i = end;
	}

	IPPU.PreviousLine = savedPrev;
	IPPU.CurrentLine  = savedCur;
}

void S9xStartScreenRefresh (void)
{
	if (GFX.DoInterlace)
		GFX.DoInterlace--;

	if (IPPU.RenderThisFrame)
	{
		if (!GFX.DoInterlace || !S9xInterlaceField())
		{
			if (!S9xInitUpdate())
			{
				IPPU.RenderThisFrame = FALSE;
				return;
			}

			S9xGraphicsScreenResize();

			IPPU.RenderedFramesCount++;
		}

		PPU.MosaicStart = 0;
		RS.MosaicStart  = 0;
		PPU.RecomputeClipWindows = TRUE;
		RS.RecomputeClipWindows  = TRUE;
		IPPU.PreviousLine = IPPU.CurrentLine = 0;
		bright_event_count = 0;
		raster_event_count = 0;

		
		
		S9xRecordFrameStart();
	}

	if (++IPPU.FrameCount == (uint32)Memory.ROMFramesPerSecond)
	{
		IPPU.DisplayedRenderedFrameCount = IPPU.RenderedFramesCount;
		IPPU.RenderedFramesCount = 0;
		IPPU.FrameCount = 0;
	}

	if (GFX.InfoStringTimeout > 0 && --GFX.InfoStringTimeout == 0)
		GFX.InfoString.clear();

	IPPU.TotalEmulatedFrames++;
}

void S9xEndScreenRefresh (void)
{
	if (IPPU.RenderThisFrame)
	{
		FLUSH_REDRAW();

		
		
		
		
		
		
		
		
		
		RT_STAT_FRAME_CLOSE();

		
		
		
		
		
		
		
		
		S9xRenderDrain();

		
		
		
		S9xRenderPublishGeometry();

		S9xApplyMidLineRaster();
		S9xApplyMidLineBrightness();

		
		
		
		S9xRecordFrameEnd();

		if (GFX.DoInterlace && S9xInterlaceField() == 0)
		{
			S9xControlEOF();
			S9xContinueUpdate(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
		}
		else
		{
			if (IPPU.ColorsChanged)
			{
				uint32 saved = PPU.CGDATA[0];
				IPPU.ColorsChanged = FALSE;
				PPU.CGDATA[0] = saved;
			}

			S9xControlEOF();

			if (Settings.TakeScreenshot)
				S9xDoScreenshot(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);

			if (Settings.AutoDisplayMessages)
				S9xDisplayMessages(GFX.Screen, GFX.RealPPL, IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight, 1);

			S9xDeinitUpdate(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
		}
	}
	else
		S9xControlEOF();

	S9xUpdateCheatsInMemory ();

#ifdef DEBUGGER
	if (CPU.Flags & FRAME_ADVANCE_FLAG)
	{
		if (ICPU.FrameAdvanceCount)
		{
			ICPU.FrameAdvanceCount--;
			IPPU.RenderThisFrame = TRUE;
			IPPU.FrameSkip = 0;
		}
		else
		{
			CPU.Flags &= ~FRAME_ADVANCE_FLAG;
			CPU.Flags |= DEBUG_MODE_FLAG;
		}
	}
#endif

	if (CPU.SRAMModified)
	{
		if (!CPU.AutoSaveTimer)
		{
			if (!(CPU.AutoSaveTimer = Settings.AutoSaveDelay * Memory.ROMFramesPerSecond))
				CPU.SRAMModified = FALSE;
		}
		else
		{
			if (!--CPU.AutoSaveTimer)
			{
				S9xAutoSaveSRAM();
				CPU.SRAMModified = FALSE;
			}
		}
	}
}

void RenderLine (uint8 C)
{
	if (IPPU.RenderThisFrame)
	{
		if (C < 240)
		{
			line_brightness[C] = PPU.Brightness;
			line_windows[C][0] = Memory.FillRAM[0x2126];
			line_windows[C][1] = Memory.FillRAM[0x2127];
			line_windows[C][2] = Memory.FillRAM[0x2128];
			line_windows[C][3] = Memory.FillRAM[0x2129];
			line_backdrop[C]   = PPU.CGDATA[0];
		}

		LineData[C].BG[0].VOffset = PPU.BG[0].VOffset + 1;
		LineData[C].BG[0].HOffset = PPU.BG[0].HOffset;
		LineData[C].BG[1].VOffset = PPU.BG[1].VOffset + 1;
		LineData[C].BG[1].HOffset = PPU.BG[1].HOffset;

		if (PPU.BGMode == 7)
		{
			struct SLineMatrixData *p = &LineMatrixData[C];
			p->MatrixA = PPU.MatrixA;
			p->MatrixB = PPU.MatrixB;
			p->MatrixC = PPU.MatrixC;
			p->MatrixD = PPU.MatrixD;
			p->CentreX = PPU.CentreX;
			p->CentreY = PPU.CentreY;
			p->M7HOFS  = PPU.M7HOFS;
			p->M7VOFS  = PPU.M7VOFS;
		}
		else
		{
			LineData[C].BG[2].VOffset = PPU.BG[2].VOffset + 1;
			LineData[C].BG[2].HOffset = PPU.BG[2].HOffset;
			LineData[C].BG[3].VOffset = PPU.BG[3].VOffset + 1;
			LineData[C].BG[3].HOffset = PPU.BG[3].HOffset;
		}

		IPPU.CurrentLine = C + 1;

		
		
		
		
		if (S9xRenderSpanLines != 0 && Settings.ThreadedRender &&
			(uint32) (IPPU.CurrentLine - IPPU.PreviousLine) >= S9xRenderSpanLines)
			S9xRecordPeriodicSpan();
	}
	else
	{
		if (!Settings.ThreadedRender)
		{
			
			if (IPPU.OBJChanged)
			{
				
				SRenderRegs tmp;
				SnapshotRenderRegs(&tmp);
				SetupOBJ(&tmp);
				IPPU.OBJChanged = FALSE;
			}
			RS.RangeTimeOver |= GFX.OBJLines[C].RTOFlags;
			PPU.RangeTimeOver = RS.RangeTimeOver;
		}
		
		
		
	}
}










static bool ProcessOneCommand (const SRenderCommand &cmd)
{
	switch (cmd.tag)
	{
		case RTAG_SPAN:
			DoRenderSpan(&cmd.span.regs, cmd.span.StartY, cmd.span.EndY, cmd.span.flags);
			break;

		case RTAG_CGRAM:
		{
			uint8	addr  = cmd.cgram.addr;
			uint16	value = cmd.cgram.value;
			RS.CGDATA[addr] = value;
			RS.Red[addr]   = RS.XB[value & 0x1f];
			RS.Green[addr] = RS.XB[(value >> 5) & 0x1f];
			RS.Blue[addr]  = RS.XB[(value >> 10) & 0x1f];
			RS.ScreenColors[addr] = (uint16) BUILD_PIXEL(RS.Red[addr], RS.Green[addr], RS.Blue[addr]);
			RS.cgram_version++;
			break;
		}

		case RTAG_OBJ:
		{
			int	i = cmd.obj.index;
			RS.OBJ[i].HPos     = cmd.obj.HPos;
			RS.OBJ[i].VPos     = cmd.obj.VPos;
			RS.OBJ[i].HFlip    = cmd.obj.HFlip;
			RS.OBJ[i].VFlip    = cmd.obj.VFlip;
			RS.OBJ[i].Name     = cmd.obj.Name;
			RS.OBJ[i].Priority = cmd.obj.Priority;
			RS.OBJ[i].Palette  = cmd.obj.Palette;
			RS.OBJ[i].Size     = cmd.obj.Size;
			RS.obj_version++;
			break;
		}

		case RTAG_OAMHI:
			RS.obj_version++;
			break;

		case RTAG_FRAME_START:
			RS.RangeTimeOver = 0;
			RS.HiresWidened = FALSE;
			RS.HeightDoubled = FALSE;
			break;

		case RTAG_FRAME_END:
			
			
			break;

		case RTAG_RESYNC:
		{
			const SRenderRegs &r = cmd.resync.regs;
			memcpy(RS.CGDATA, S9xResyncPayload.CGDATA, sizeof(RS.CGDATA));
			memcpy(RS.OBJ_data, S9xResyncPayload.OBJ, sizeof(RS.OBJ_data));
			
			uint8 b = r.Brightness;
			RS.XB = mul_brightness[b];
			RS.last_brightness = b;
			for (int i = 0; i < 256; i++)
			{
				RS.Red[i]   = RS.XB[RS.CGDATA[i] & 0x1f];
				RS.Green[i] = RS.XB[(RS.CGDATA[i] >> 5) & 0x1f];
				RS.Blue[i]  = RS.XB[(RS.CGDATA[i] >> 10) & 0x1f];
				RS.ScreenColors[i] = (uint16) BUILD_PIXEL(RS.Red[i], RS.Green[i], RS.Blue[i]);
			}
			RS.cgram_version++;
			RS.obj_version++;
			RS.RecomputeClipWindows = TRUE;
			RS.RangeTimeOver = 0;
			RS.HiresWidened = FALSE;
			RS.HeightDoubled = FALSE;
			DirectColourMapsDirty = TRUE;
			break;
		}

		case RTAG_CLEAR_RTO:
			RS.RangeTimeOver = 0;
			
			
			
			if (!Settings.ThreadedRender)
				PPU.RangeTimeOver = 0;
			break;

		case RTAG_RTO_ACCUM:
			
			
			
			
			
			
			
			RS.RangeTimeOver |= GFX.OBJLines[cmd.rto_accum.line].RTOFlags;
			break;

		case RTAG_SHUTDOWN:
			return false;	
	}
	return true;
}






#define PROCESS_ONE_COMMAND(c)	ProcessOneCommand(c)







void S9xVRAMWriteBarrier (void)
{
	if (Settings.ThreadedRender)
	{
		
		
		S9xRenderDrain();
	}
	else
	{
		if (RenderLog.produced != RenderLog.consumed)
			S9xRenderDrain();
	}
}









template<typename Pred>
static inline bool SpinUntil (Pred pred)
{
	if (pred())
		return true;

	const uint32	budget = S9xRenderSpinUs;
	if (budget == 0)
		return false;

	const auto	deadline = std::chrono::steady_clock::now() +
				std::chrono::microseconds(budget);

	for (;;)
	{
		for (int i = 0; i < 64; i++)
		{
			if (pred())
				return true;
			S9xSpinHint();
		}

		if (std::chrono::steady_clock::now() >= deadline)
			return false;
	}
}






static void DrainWaitForTarget (uint64_t target)
{
	
	
	
	
	
	WorkEC.notify();


	if (SpinUntil([target]() {
		return RenderConsumed.load(std::memory_order_seq_cst) >= target;
	}))
		return;

	RT_STAT_ADD(S9X_RTS_DRAIN_PARKS, 1);
	WorkEC.notify();  
	DrainEC.waitPred([target]() {
		return RenderConsumed.load(std::memory_order_seq_cst) >= target;
	});
}

void S9xRenderDrain (void)
{
	if (Settings.ThreadedRender)
	{
		
		
		
		uint64_t target = RenderProduced.load(std::memory_order_seq_cst);
		RT_STAT_ADD(S9X_RTS_DRAIN_CALLS, 1);
		if (RenderConsumed.load(std::memory_order_seq_cst) >= target)
		{
			RT_STAT_ADD(S9X_RTS_DRAIN_FASTPATH, 1);
			return;
		}
		DrainWaitForTarget(target);
	}
	else
	{
		while (RenderLog.consumed != RenderLog.produced)
		{
			const SRenderCommand &cmd =
				RenderLog.ring[RenderLog.consumed & (S9X_RENDER_RING_CAPACITY - 1)];
			PROCESS_ONE_COMMAND(cmd);
			RenderLog.consumed = (RenderLog.consumed + 1) & (S9X_RENDER_RING_CAPACITY - 1);
		}
	}
}






#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC poison PPU
#endif







static uint8 rclip_region_map[6][6] =
{
	{ 0, 0x01, 0x03, 0x07, 0x0f, 0x1f },
	{ 0,    0, 0x02, 0x06, 0x0e, 0x1e },
	{ 0,    0,    0, 0x04, 0x0c, 0x1c },
	{ 0,    0,    0,    0, 0x08, 0x18 },
	{ 0,    0,    0,    0,    0, 0x10 }
};

static inline uint8 RCalcWindowMask (const SRenderRegs *R, int i, uint8 W1, uint8 W2)
{
	if (!R->ClipWindow1Enable[i])
	{
		if (!R->ClipWindow2Enable[i])
			return (0);
		else
		{
			if (!R->ClipWindow2Inside[i])
				return (~W2);
			return (W2);
		}
	}
	else
	{
		if (!R->ClipWindow2Enable[i])
		{
			if (!R->ClipWindow1Inside[i])
				return (~W1);
			return (W1);
		}
		else
		{
			if (!R->ClipWindow1Inside[i])
				W1 = ~W1;
			if (!R->ClipWindow2Inside[i])
				W2 = ~W2;

			switch (R->ClipWindowOverlapLogic[i])
			{
				case 0: return (W1 | W2);
				case 1: return (W1 & W2);
				case 2: return (W1 ^ W2);
				case 3: return (~(W1 ^ W2));
			}
		}
	}
	return (0);
}

static inline void RStoreWindowRegions (uint8 Mask, struct ClipData *Clip, int n_regions, int16 *windows, uint8 *drawing_modes, bool8 sub, bool8 StoreMode0 = FALSE)
{
	int	ct = 0;
	for (int j = 0; j < n_regions; j++)
	{
		int	DrawMode = drawing_modes[j];
		if (sub)
			DrawMode |= 1;
		if (Mask & (1 << j))
			DrawMode = 0;
		if (!StoreMode0 && !DrawMode)
			continue;
		if (ct > 0 && Clip->Right[ct - 1] == windows[j] && Clip->DrawMode[ct - 1] == DrawMode)
			Clip->Right[ct - 1] = windows[j + 1];
		else
		{
			Clip->Left[ct]     = windows[j];
			Clip->Right[ct]    = windows[j + 1];
			Clip->DrawMode[ct] = DrawMode;
			ct++;
		}
	}
	Clip->Count = ct;
}

static void RComputeClipWindows (const SRenderRegs *R)
{
	int16	windows[6] = { 0, 256, 256, 256, 256, 256 };
	uint8	drawing_modes[5] = { 0, 0, 0, 0, 0 };
	int		n_regions = 1;
	int		i, j;

	if (R->Window1Left <= R->Window1Right)
	{
		if (R->Window1Left > 0)
		{
			windows[2] = 256;
			windows[1] = R->Window1Left;
			n_regions = 2;
		}
		if (R->Window1Right < 255)
		{
			windows[n_regions + 1] = 256;
			windows[n_regions] = R->Window1Right + 1;
			n_regions++;
		}
	}

	if (R->Window2Left <= R->Window2Right)
	{
		for (i = 0; i <= n_regions; i++)
		{
			if (R->Window2Left == windows[i])
				break;
			if (R->Window2Left < windows[i])
			{
				for (j = n_regions; j >= i; j--)
					windows[j + 1] = windows[j];
				windows[i] = R->Window2Left;
				n_regions++;
				break;
			}
		}
		for (; i <= n_regions; i++)
		{
			if (R->Window2Right + 1 == windows[i])
				break;
			if (R->Window2Right + 1 < windows[i])
			{
				for (j = n_regions; j >= i; j--)
					windows[j + 1] = windows[j];
				windows[i] = R->Window2Right + 1;
				n_regions++;
				break;
			}
		}
	}

	uint8	W1, W2;

	if (R->Window1Left <= R->Window1Right)
	{
		for (i = 0; windows[i] != R->Window1Left; i++) ;
		for (j = i; windows[j] != R->Window1Right + 1; j++) ;
		W1 = rclip_region_map[i][j];
	}
	else
		W1 = 0;

	if (R->Window2Left <= R->Window2Right)
	{
		for (i = 0; windows[i] != R->Window2Left; i++) ;
		for (j = i; windows[j] != R->Window2Right + 1; j++) ;
		W2 = rclip_region_map[i][j];
	}
	else
		W2 = 0;

	uint8	CW_color = 0, CW_math = 0;
	uint8	CW = RCalcWindowMask(R, 5, W1, W2);

	switch (R->FillRAM[0x30] & 0xc0)
	{
		case 0x00:	CW_color = 0;		break;
		case 0x40:	CW_color = ~CW;		break;
		case 0x80:	CW_color = CW;		break;
		case 0xc0:	CW_color = 0xff;	break;
	}

	switch (R->FillRAM[0x30] & 0x30)
	{
		case 0x00:	CW_math  = 0;		break;
		case 0x10:	CW_math  = ~CW;		break;
		case 0x20:	CW_math  = CW;		break;
		case 0x30:	CW_math  = 0xff;	break;
	}

	for (i = 0; i < n_regions; i++)
	{
		if (!(CW_color & (1 << i)))
			drawing_modes[i] |= 1;
		if (!(CW_math  & (1 << i)))
			drawing_modes[i] |= 2;
	}

	RStoreWindowRegions(0, &IPPU.Clip[0][5], n_regions, windows, drawing_modes, FALSE, TRUE);
	RStoreWindowRegions(0, &IPPU.Clip[1][5], n_regions, windows, drawing_modes, TRUE,  TRUE);

	for (j = 0; j < 5; j++)
	{
		uint8	W = Settings.DisableGraphicWindows ? 0 : RCalcWindowMask(R, j, W1, W2);
		for (int sub = 0; sub < 2; sub++)
		{
			if (R->FillRAM[0x2e + sub] & (1 << j))
				RStoreWindowRegions(W, &IPPU.Clip[sub][j], n_regions, windows, drawing_modes, sub);
			else
				RStoreWindowRegions(0, &IPPU.Clip[sub][j], n_regions, windows, drawing_modes, sub);
		}
	}
}




static void RFixColourBrightness (uint8 brightness)
{
	if (RS.last_brightness == brightness)
		return;
	RS.XB = mul_brightness[brightness];
	RS.last_brightness = brightness;
	for (int i = 0; i < 64; i++)
	{
		if (i > RS.XB[0x1f])
			brightness_cap[i] = RS.XB[0x1f];
		else
			brightness_cap[i] = i;
	}
	for (int i = 0; i < 256; i++)
	{
		RS.Red[i]   = RS.XB[RS.CGDATA[i] & 0x1f];
		RS.Green[i] = RS.XB[(RS.CGDATA[i] >> 5) & 0x1f];
		RS.Blue[i]  = RS.XB[(RS.CGDATA[i] >> 10) & 0x1f];
		RS.ScreenColors[i] = (uint16) BUILD_PIXEL(RS.Red[i], RS.Green[i], RS.Blue[i]);
	}
	RS.cgram_applied = RS.cgram_version; 
	DirectColourMapsDirty = TRUE;
}




static void SetupOBJ (const SRenderRegs *R)
{
	int	SmallWidth, SmallHeight, LargeWidth, LargeHeight;

	switch (R->OBJSizeSelect)
	{
		case 0:
			SmallWidth = SmallHeight = 8;
			LargeWidth = LargeHeight = 16;
			break;
		case 1:
			SmallWidth = SmallHeight = 8;
			LargeWidth = LargeHeight = 32;
			break;
		case 2:
			SmallWidth = SmallHeight = 8;
			LargeWidth = LargeHeight = 64;
			break;
		case 3:
			SmallWidth = SmallHeight = 16;
			LargeWidth = LargeHeight = 32;
			break;
		case 4:
			SmallWidth = SmallHeight = 16;
			LargeWidth = LargeHeight = 64;
			break;
		case 5:
		default:
			SmallWidth = SmallHeight = 32;
			LargeWidth = LargeHeight = 64;
			break;
		case 6:
			SmallWidth = 16; SmallHeight = 32;
			LargeWidth = 32; LargeHeight = 64;
			break;
		case 7:
			SmallWidth = 16; SmallHeight = 32;
			LargeWidth = LargeHeight = 32;
			break;
	}

	int	inc = R->InterlaceOBJ ? 2 : 1;
	int startline = (R->InterlaceOBJ && R->InterlaceField) ? 1 : 0;

	int		Height;
	uint8	S;
	int sprite_limit = (Settings.MaxSpriteTilesPerLine == 128) ? 128 : 32;



	if (!R->OAMPriorityRotation || !(R->OAMFlip & R->OAMAddr & 1))
	{
		uint8	LineOBJ[SNES_HEIGHT_EXTENDED];
		memset(LineOBJ, 0, sizeof(LineOBJ));

		for (int i = 0; i < SNES_HEIGHT_EXTENDED; i++)
		{
			GFX.OBJLines[i].RTOFlags = 0;
			GFX.OBJLines[i].Tiles = Settings.MaxSpriteTilesPerLine;
		}

		uint8	FirstSprite = R->FirstSprite;
		S = FirstSprite;

		do
		{
			if (RS.OBJ[S].Size)
			{
				GFX.OBJWidths[S] = LargeWidth;
				Height = LargeHeight;
			}
			else
			{
				GFX.OBJWidths[S] = SmallWidth;
				Height = SmallHeight;
			}

			int	HPos = RS.OBJ[S].HPos;
			if (HPos == -256)
				HPos = 0;

			if (HPos > -GFX.OBJWidths[S] && HPos <= 256)
			{
				if (HPos < 0)
					GFX.OBJVisibleTiles[S] = (GFX.OBJWidths[S] + HPos + 7) >> 3;
				else if (HPos + GFX.OBJWidths[S] > 255)
					GFX.OBJVisibleTiles[S] = (256 - HPos + 7) >> 3;
				else
					GFX.OBJVisibleTiles[S] = GFX.OBJWidths[S] >> 3;

				for (uint8 line = startline, Y = (uint8) (RS.OBJ[S].VPos & 0xff); line < Height; Y++, line += inc)
				{
					if (Y >= SNES_HEIGHT_EXTENDED)
						continue;

					if (LineOBJ[Y] >= sprite_limit)
					{
						GFX.OBJLines[Y].RTOFlags |= 0x40;
						continue;
					}

					GFX.OBJLines[Y].Tiles -= GFX.OBJVisibleTiles[S];
					if (GFX.OBJLines[Y].Tiles < 0)
						GFX.OBJLines[Y].RTOFlags |= 0x80;

					GFX.OBJLines[Y].OBJ[LineOBJ[Y]].Sprite = S;
					if (RS.OBJ[S].VFlip)
						GFX.OBJLines[Y].OBJ[LineOBJ[Y]].Line = line ^ (GFX.OBJWidths[S] - 1);
					else
						GFX.OBJLines[Y].OBJ[LineOBJ[Y]].Line = line;

					LineOBJ[Y]++;
				}
			}

			S = (S + 1) & 0x7f;
		} while (S != FirstSprite);

		for (int Y = 0; Y < SNES_HEIGHT_EXTENDED; Y++)
		{
			if (LineOBJ[Y] < sprite_limit)
				GFX.OBJLines[Y].OBJ[LineOBJ[Y]].Sprite = -1;
			if (Y > 0)
				GFX.OBJLines[Y].RTOFlags |= GFX.OBJLines[Y - 1].RTOFlags;
		}
	}
	else
	{
		uint8 OBJOnLine[SNES_HEIGHT_EXTENDED][128];
		bool8 AnyOBJOnLine[SNES_HEIGHT_EXTENDED];
		memset(AnyOBJOnLine, FALSE, sizeof(AnyOBJOnLine));

		for (S = 0; S < 128; S++)
		{
			if (RS.OBJ[S].Size)
			{
				GFX.OBJWidths[S] = LargeWidth;
				Height = LargeHeight;
			}
			else
			{
				GFX.OBJWidths[S] = SmallWidth;
				Height = SmallHeight;
			}

			int	HPos = RS.OBJ[S].HPos;
			if (HPos == -256)
				HPos = 256;

			if (HPos > -GFX.OBJWidths[S] && HPos <= 256)
			{
				if (HPos < 0)
					GFX.OBJVisibleTiles[S] = (GFX.OBJWidths[S] + HPos + 7) >> 3;
				else if (HPos + GFX.OBJWidths[S] >= 257)
					GFX.OBJVisibleTiles[S] = (257 - HPos + 7) >> 3;
				else
					GFX.OBJVisibleTiles[S] = GFX.OBJWidths[S] >> 3;

				for (uint8 line = startline, Y = (uint8) (RS.OBJ[S].VPos & 0xff); line < Height; Y++, line += inc)
				{
					if (Y >= SNES_HEIGHT_EXTENDED)
						continue;

					if (!AnyOBJOnLine[Y]) {
						memset(OBJOnLine[Y], 0, sizeof(OBJOnLine[Y]));
						AnyOBJOnLine[Y] = TRUE;
					}

					if (RS.OBJ[S].VFlip)
						OBJOnLine[Y][S] = (line ^ (GFX.OBJWidths[S] - 1)) | 0x80;
					else
						OBJOnLine[Y][S] = line | 0x80;
				}
			}
		}

		int	j;
		for (int Y = 0; Y < SNES_HEIGHT_EXTENDED; Y++)
		{
			GFX.OBJLines[Y].RTOFlags = Y ? GFX.OBJLines[Y - 1].RTOFlags : 0;
			GFX.OBJLines[Y].Tiles = Settings.MaxSpriteTilesPerLine;

			uint8	FirstSprite = (R->FirstSprite + Y) & 0x7f;
			S = FirstSprite;
			j = 0;

			if (AnyOBJOnLine[Y])
			{
				do
				{
					if (OBJOnLine[Y][S])
					{
						if (j >= sprite_limit)
						{
							GFX.OBJLines[Y].RTOFlags |= 0x40;
							break;
						}

						GFX.OBJLines[Y].Tiles -= GFX.OBJVisibleTiles[S];
						if (GFX.OBJLines[Y].Tiles < 0)
							GFX.OBJLines[Y].RTOFlags |= 0x80;
						GFX.OBJLines[Y].OBJ[j].Sprite = S;
						GFX.OBJLines[Y].OBJ[j++].Line = OBJOnLine[Y][S] & ~0x80;
					}

					S = (S + 1) & 0x7f;
				} while (S != FirstSprite);
			}

			if (j < sprite_limit)
				GFX.OBJLines[Y].OBJ[j].Sprite = -1;
		}
	}
}




#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize ("no-tree-vrp")
#endif
static void DrawOBJS (const SRenderRegs *R, int D)
{
	void (*DrawTile) (uint32, uint32, uint32, uint32) = NULL;
	void (*DrawClippedTile) (uint32, uint32, uint32, uint32, uint32, uint32) = NULL;

	int	PixWidth = R->DoubleWidthPixels ? 2 : 1;
	BG.InterlaceLine = R->InterlaceField ? 8 : 0;
	GFX.Z1 = 2;
	int sprite_limit = (Settings.MaxSpriteTilesPerLine == 128) ? 128 : 32;

	uint8	psl[SNES_HEIGHT_EXTENDED];
	int16	batch_end[128];

	for (int i = 0; i < sprite_limit; i++)
		batch_end[i] = -1;

	for (uint32 Y = GFX.StartY + 1; Y <= GFX.EndY; Y++)
	{
		int	n = 0;

		if (GFX.OBJLines[Y].Tiles >= 0 && GFX.OBJLines[Y - 1].Tiles >= 0)
		{
			while (n < sprite_limit)
			{
				int	S = GFX.OBJLines[Y].OBJ[n].Sprite;
				if (S < 0 || S != GFX.OBJLines[Y - 1].OBJ[n].Sprite)
					break;
				if ((int) GFX.OBJLines[Y].OBJ[n].Line != (int) GFX.OBJLines[Y - 1].OBJ[n].Line + (RS.OBJ[S].VFlip ? -1 : 1))
					break;
				n++;
			}
		}

		psl[Y] = (uint8) n;
	}

	for (uint32 Y = GFX.StartY, Offset = Y * GFX.PPL; Y <= GFX.EndY; Y++, Offset += GFX.PPL)
	{
		int	I = 0;
		int	tiles = GFX.OBJLines[Y].Tiles;

		for (int S = GFX.OBJLines[Y].OBJ[I].Sprite; S >= 0 && I < sprite_limit; S = GFX.OBJLines[Y].OBJ[++I].Sprite)
		{
			tiles += GFX.OBJVisibleTiles[S];
			if (tiles <= 0)
				continue;

			if (batch_end[I] >= (int) Y)
				continue;

			int		Line = GFX.OBJLines[Y].OBJ[I].Line;
			uint32	rows_left = RS.OBJ[S].VFlip ? (Line & 7) + 1 : 8 - (Line & 7);
			uint32	LineCount = 1;
			int		cover_end = (int) GFX.EndY;

			for (int i = 0; i < I; i++)
			{
				int	P = GFX.OBJLines[Y].OBJ[i].Sprite;
				if (RS.OBJ[P].HPos < RS.OBJ[S].HPos + GFX.OBJWidths[S] && RS.OBJ[S].HPos < RS.OBJ[P].HPos + GFX.OBJWidths[P] && batch_end[i] < cover_end)
					cover_end = batch_end[i];
			}

			while (LineCount < rows_left && (int) (Y + LineCount) <= cover_end && psl[Y + LineCount] > I)
				LineCount++;
			batch_end[I] = (int16) (Y + LineCount - 1);

			int	BaseTile = (((Line << 1) + (RS.OBJ[S].Name & 0xf0)) & 0xf0) | (RS.OBJ[S].Name & 0x100) | (RS.OBJ[S].Palette << 10);
			int	TileX = RS.OBJ[S].Name & 0x0f;
			int	TileLine = (Line & 7) * 8;
			int	TileInc = 1;

			if (RS.OBJ[S].VFlip)
			{
				TileLine = 56 - TileLine;
				BaseTile |= V_FLIP;
			}

			if (RS.OBJ[S].HFlip)
			{
				TileX = (TileX + (GFX.OBJWidths[S] >> 3) - 1) & 0x0f;
				BaseTile |= H_FLIP;
				TileInc = -1;
			}

			GFX.Z2 = D + RS.OBJ[S].Priority * 4;

			int	DrawMode = 3;
			int	clip = 0, next_clip = -1000;
			int	X = RS.OBJ[S].HPos;
			if (X == -256)
				X = 256;

			for (int t = tiles, O = Offset + X * PixWidth; X <= 256 && X < RS.OBJ[S].HPos + GFX.OBJWidths[S]; TileX = (TileX + TileInc) & 0x0f, X += 8, O += 8 * PixWidth)
			{
				if (X < -7 || --t < 0 || X == 256)
					continue;

				for (int x = X; x < X + 8;)
				{
					if (x >= next_clip)
					{
						for (; clip < GFX.Clip[4].Count && GFX.Clip[4].Left[clip] <= x; clip++) ;
						if (clip == 0 || x >= GFX.Clip[4].Right[clip - 1])
						{
							DrawMode = 0;
							next_clip = ((clip < GFX.Clip[4].Count) ? GFX.Clip[4].Left[clip] : 1000);
						}
						else
						{
							DrawMode = GFX.Clip[4].DrawMode[clip - 1];
							next_clip = GFX.Clip[4].Right[clip - 1];
							GFX.ClipColors = !(DrawMode & 1);

							if (BG.EnableMath && (RS.OBJ[S].Palette & 4) && (DrawMode & 2))
							{
								DrawTile = GFX.DrawTileMath;
								DrawClippedTile = GFX.DrawClippedTileMath;
							}
							else
							{
								DrawTile = GFX.DrawTileNomath;
								DrawClippedTile = GFX.DrawClippedTileNomath;
							}
						}
					}

					if (x == X && x + 8 < next_clip)
					{
						if (DrawMode)
							DrawTile(BaseTile | TileX, O, TileLine, LineCount);
						x += 8;
					}
					else
					{
						int	w = (next_clip <= X + 8) ? next_clip - x : X + 8 - x;
						if (DrawMode)
							DrawClippedTile(BaseTile | TileX, O, x - X, w, TileLine, LineCount);
						x += w;
					}
				}
			}
		}
	}
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif




static inline void RenderScreen (const SRenderRegs *R, bool8 sub)
{
	uint8	BGActive;
	int		D;

	if (!sub)
	{
		GFX.S = GFX.Screen;
		if (GFX.DoInterlace && R->InterlaceField)
			GFX.S += GFX.RealPPL;
		GFX.DB = GFX.ZBuffer;
		GFX.Clip = IPPU.Clip[0];
		BGActive = R->FillRAM[0x2c] & ~Settings.BG_Forced;
		D = 32;
	}
	else
	{
		GFX.S = GFX.SubScreen;
		GFX.DB = GFX.SubZBuffer;
		GFX.Clip = IPPU.Clip[1];
		BGActive = R->FillRAM[0x2d] & ~Settings.BG_Forced;
		D = (R->FillRAM[0x30] & 2) << 4;
	}

	BG.NameSelect = 0;
	S9xSelectTileRenderers(R->BGMode, sub, FALSE);

	BG.EnableMath = !sub && (R->FillRAM[0x31] & 0x20);
	DrawBackdrop(R);

	if (BGActive & 0x10)
	{
		BG.TileAddress = R->OBJNameBase;
		BG.NameSelect = R->OBJNameSelect;
		BG.EnableMath = !sub && (R->FillRAM[0x31] & 0x10);
		BG.StartPalette = 128;
		S9xSelectTileConverter(4, FALSE, sub, FALSE);
		S9xSelectTileRenderers(R->BGMode, sub, TRUE);
		DrawOBJS(R, D + 4);

		BG.NameSelect = 0;
		S9xSelectTileRenderers(R->BGMode, sub, FALSE);
	}

	#define DO_BG(n, pal, depth, hires, offset, Zh, Zl, voffoff) \
		if (BGActive & (1 << n)) \
		{ \
			BG.StartPalette = pal; \
			BG.EnableMath = !sub && (R->FillRAM[0x31] & (1 << n)); \
			BG.TileSizeH = (!hires && R->BG[n].BGSize) ? 16 : 8; \
			BG.TileSizeV = (R->BG[n].BGSize) ? 16 : 8; \
			S9xSelectTileConverter(depth, hires, sub, R->BGMosaic[n]); \
			\
			if (offset) \
			{ \
				BG.OffsetSizeH = (!hires && R->BG[2].BGSize) ? 16 : 8; \
				BG.OffsetSizeV = (R->BG[2].BGSize) ? 16 : 8; \
				\
				if (R->BGMosaic[n] && (hires || R->Mosaic > 1)) \
					DrawBackgroundOffsetMosaic(R, n, D + Zh, D + Zl, voffoff); \
				else \
					DrawBackgroundOffset(R, n, D + Zh, D + Zl, voffoff); \
			} \
			else \
			{ \
				if (R->BGMosaic[n] && (hires || R->Mosaic > 1)) \
					DrawBackgroundMosaic(R, n, D + Zh, D + Zl); \
				else \
					DrawBackground(R, n, D + Zh, D + Zl); \
			} \
		}

	switch (R->BGMode)
	{
		case 0:
			DO_BG(0,  0, 2, FALSE, FALSE, 15, 11, 0);
			DO_BG(1, 32, 2, FALSE, FALSE, 14, 10, 0);
			DO_BG(2, 64, 2, FALSE, FALSE,  7,  3, 0);
			DO_BG(3, 96, 2, FALSE, FALSE,  6,  2, 0);
			break;

		case 1:
			DO_BG(0,  0, 4, FALSE, FALSE, 15, 11, 0);
			DO_BG(1,  0, 4, FALSE, FALSE, 14, 10, 0);
			DO_BG(2,  0, 2, FALSE, FALSE, (R->BG3Priority ? 17 : 7), 3, 0);
			break;

		case 2:
			DO_BG(0,  0, 4, FALSE, TRUE,  15,  7, 8);
			DO_BG(1,  0, 4, FALSE, TRUE,  11,  3, 8);
			break;

		case 3:
			DO_BG(0,  0, 8, FALSE, FALSE, 15,  7, 0);
			DO_BG(1,  0, 4, FALSE, FALSE, 11,  3, 0);
			break;

		case 4:
			DO_BG(0,  0, 8, FALSE, TRUE,  15,  7, 0);
			DO_BG(1,  0, 2, FALSE, TRUE,  11,  3, 0);
			break;

		case 5:
			DO_BG(0,  0, 4, TRUE,  FALSE, 15,  7, 0);
			DO_BG(1,  0, 2, TRUE,  FALSE, 11,  3, 0);
			break;

		case 6:
			DO_BG(0,  0, 4, TRUE,  TRUE,  15,  7, 8);
			break;

		case 7:
			if (BGActive & 0x01)
			{
				BG.EnableMath = !sub && (R->FillRAM[0x31] & 1);
				DrawBackgroundMode7(R, 0, GFX.DrawMode7BG1Math, GFX.DrawMode7BG1Nomath, D);
			}

			if ((R->FillRAM[0x33] & 0x40) && (BGActive & 0x02))
			{
				BG.EnableMath = !sub && (R->FillRAM[0x31] & 2);
				DrawBackgroundMode7(R, 1, GFX.DrawMode7BG2Math, GFX.DrawMode7BG2Nomath, D);
			}

			break;
	}

	#undef DO_BG
}





static void DoRenderSpan (const SRenderRegs *R_in, uint32 StartY, uint32 EndY, uint8 SpanFlags)
{
	
	
	
	
	
	
	
	
	
	
	SRenderRegs		regsCopy = *R_in;
	const SRenderRegs	*R = &regsCopy;

	S9xCurRenderRegs = R;
	
	
	
	
	
	
	if (RS.obj_version != RS.obj_applied || R->InterlaceOBJ ||
		R->FirstSprite != RS.last_obj_first_sprite ||
		R->OBJSizeSelect != RS.last_obj_size_select)
	{
		SetupOBJ(R);
		RS.obj_applied = RS.obj_version;
		RS.last_obj_first_sprite = R->FirstSprite;
		RS.last_obj_size_select  = R->OBJSizeSelect;
	}

	
	
	
	if (!(SpanFlags & S9X_SPAN_NO_RTO))
		RS.RangeTimeOver |= GFX.OBJLines[EndY].RTOFlags;
	
	

	GFX.StartY = StartY;
	GFX.EndY   = EndY;

	
	RFixColourBrightness(R->Brightness);

	if (!R->ForcedBlanking)
	{
		
		
		if (R->RecomputeClipWindows || RS.RecomputeClipWindows)
		{
			RComputeClipWindows(R);
			RS.RecomputeClipWindows = FALSE;
			
			IPPU.Clip[0][0] = IPPU.Clip[0][0]; 
		}

		if (raster_span_count)
			RestrictClipWindows();

		
		
		
		
		
		if (RS.HiresWidened)
		{
			regsCopy.DoubleWidthPixels   = TRUE;
			regsCopy.RenderedScreenWidth = 512;
		}
		else
		if (!R->DoubleWidthPixels && (R->BGMode == 5 || R->BGMode == 6 || R->PseudoHires || R->Interlace))
		{
			for (uint32 y = 0; y < GFX.StartY; y++)
			{
				uint16	*p = GFX.Screen + y * GFX.PPL + 255;
				uint16	*q = GFX.Screen + y * GFX.PPL + 510;

				for (int x = 255; x >= 0; x--, p--, q -= 2)
					*q = *(q + 1) = *p;
			}

			
			regsCopy.DoubleWidthPixels   = TRUE;
			regsCopy.RenderedScreenWidth = 512;
			
			
			RS.HiresWidened = TRUE;
		}

		if (RS.HeightDoubled)
		{
			regsCopy.DoubleHeightPixels = TRUE;
		}
		else
		if (!R->DoubleHeightPixels && R->Interlace)
		{
			RS.HeightDoubled = TRUE;
			RS.DoubledScreenHeight = (uint16) (R->ScreenHeight << 1);
			regsCopy.DoubleHeightPixels = TRUE;
			GFX.PPL = GFX.RealPPL << 1;
			GFX.DoInterlace = 2;

			for (int32 y = (int32) GFX.StartY - 2; y >= 0; y--)
				memmove(GFX.Screen + (y + 1) * GFX.PPL, GFX.Screen + y * GFX.RealPPL, GFX.PPL * sizeof(uint16));
		}

		if ((R->FillRAM[0x30] & 0x30) != 0x30 && (R->FillRAM[0x31] & 0x3f))
			GFX.FixedColour = BUILD_PIXEL(RS.XB[R->FixedColourRed], RS.XB[R->FixedColourGreen], RS.XB[R->FixedColourBlue]);

		if (R->BGMode == 5 || R->BGMode == 6 || R->PseudoHires ||
			((R->FillRAM[0x30] & 0x30) != 0x30 && (R->FillRAM[0x30] & 2) && (R->FillRAM[0x31] & 0x3f) && (R->FillRAM[0x2d] & 0x1f)))
			RenderScreen(R, TRUE);
		else
		if (R->FillRAM[0x31] & 0x3f)
		{
			memset(GFX.SubZBuffer + GFX.StartY * GFX.PPL, 0, (GFX.EndY - GFX.StartY + 1) * GFX.PPL);
		}

		RenderScreen(R, FALSE);
	}
	else
	{
		
		
		
		const uint16	black = BUILD_PIXEL(0, 0, 0);

		GFX.S = GFX.Screen + GFX.StartY * GFX.PPL;
		if (GFX.DoInterlace && R->InterlaceField)
			GFX.S += GFX.RealPPL;

		for (uint32 l = GFX.StartY; l <= GFX.EndY; l++, GFX.S += GFX.PPL)
			for (int x = 0; x < R->RenderedScreenWidth; x++)
				GFX.S[x] = black;
	}
}




static inline void DrawBackdrop (const SRenderRegs *R)
{
	uint32	Offset = GFX.StartY * GFX.PPL;

	for (int clip = 0; clip < GFX.Clip[5].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[5].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[5].DrawMode[clip] & 2))
			GFX.DrawBackdropMath(Offset, GFX.Clip[5].Left[clip], GFX.Clip[5].Right[clip]);
		else
			GFX.DrawBackdropNomath(Offset, GFX.Clip[5].Left[clip], GFX.Clip[5].Right[clip]);
	}
}




static void DrawBackground (const SRenderRegs *R, int bg, uint8 Zh, uint8 Zl)
{
	BG.TileAddress = R->BG[bg].NameBase << 1;

	uint32	Tile;
	uint16	*SC0, *SC1, *SC2, *SC3;

	SC0 = (uint16 *) &Memory.VRAM[R->BG[bg].SCBase << 1];
	SC1 = (R->BG[bg].SCSize & 1) ? SC0 + 1024 : SC0;
	if (SC1 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC1 -= 0x8000;
	SC2 = (R->BG[bg].SCSize & 2) ? SC1 + 1024 : SC0;
	if (SC2 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC2 -= 0x8000;
	SC3 = (R->BG[bg].SCSize & 1) ? SC2 + 1024 : SC2;
	if (SC3 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC3 -= 0x8000;

	uint32	Lines;
	int		OffsetMask  = (BG.TileSizeH == 16) ? 0x3ff : 0x1ff;
	int		OffsetShift = (BG.TileSizeV == 16) ? 4 : 3;
	int		PixWidth = R->DoubleWidthPixels ? 2 : 1;
	bool8	HiresInterlace = R->Interlace && R->DoubleWidthPixels;

	void (*DrawTile) (uint32, uint32, uint32, uint32);
	void (*DrawClippedTile) (uint32, uint32, uint32, uint32, uint32, uint32);

	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
		{
			DrawTile = GFX.DrawTileMath;
			DrawClippedTile = GFX.DrawClippedTileMath;
		}
		else
		{
			DrawTile = GFX.DrawTileNomath;
			DrawClippedTile = GFX.DrawClippedTileNomath;
		}

		for (uint32 Y = GFX.StartY; Y <= GFX.EndY; Y += Lines)
		{
			uint32	Y2 = HiresInterlace ? Y * 2 + R->InterlaceField : Y;
			uint32	VOffset = LineData[Y].BG[bg].VOffset + (HiresInterlace ? 1 : 0);
			uint32	HOffset = LineData[Y].BG[bg].HOffset;
			int		VirtAlign = ((Y2 + VOffset) & 7) >> (HiresInterlace ? 1 : 0);

			
			
			
			
			
			
			
			
			
			
			
			
			
			
			uint32	LinesMax = GFX.LinesPerTile - VirtAlign;
			if (LinesMax > GFX.EndY - Y + 1)
				LinesMax = GFX.EndY - Y + 1;

			for (Lines = 1; Lines < LinesMax; Lines++)
			{
				if ((VOffset != LineData[Y + Lines].BG[bg].VOffset) || (HOffset != LineData[Y + Lines].BG[bg].HOffset))
					break;
			}

			VirtAlign <<= 3;

			uint32	t1, t2;
			uint32	TilemapRow = (VOffset + Y2) >> OffsetShift;
			BG.InterlaceLine = ((VOffset + Y2) & 1) << 3;

			if ((VOffset + Y2) & 8)
			{
				t1 = 16;
				t2 = 0;
			}
			else
			{
				t1 = 0;
				t2 = 16;
			}

			uint16	*b1, *b2;

			if (TilemapRow & 0x20)
			{
				b1 = SC2;
				b2 = SC3;
			}
			else
			{
				b1 = SC0;
				b2 = SC1;
			}

			b1 += (TilemapRow & 0x1f) << 5;
			b2 += (TilemapRow & 0x1f) << 5;

			uint32	Left   = GFX.Clip[bg].Left[clip];
			uint32	Right  = GFX.Clip[bg].Right[clip];
			uint32	Offset = Left * PixWidth + Y * GFX.PPL;
			uint32	HPos   = (HOffset + Left) & OffsetMask;
			uint32	HTile  = HPos >> 3;
			uint16	*t;

			if (BG.TileSizeH == 8)
			{
				if (HTile > 31)
					t = b2 + (HTile & 0x1f);
				else
					t = b1 + HTile;
			}
			else
			{
				if (HTile > 63)
					t = b2 + ((HTile >> 1) & 0x1f);
				else
					t = b1 + (HTile >> 1);
			}

			uint32	Width = Right - Left;

			if (HPos & 7)
			{
				uint32	l = HPos & 7;
				uint32	w = 8 - l;
				if (w > Width)
					w = Width;

				Offset -= l * PixWidth;
				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
				{
					DrawClippedTile(Tile, Offset, l, w, VirtAlign, Lines);
					t++;
					if (HTile == 31)
						t = b2;
					else
					if (HTile == 63)
						t = b1;
				}
				else
				{
					if (!(Tile & H_FLIP))
						DrawClippedTile(TILE_PLUS(Tile, (HTile & 1)), Offset, l, w, VirtAlign, Lines);
					else
						DrawClippedTile(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, l, w, VirtAlign, Lines);
					t += HTile & 1;
					if (HTile == 63)
						t = b2;
					else
					if (HTile == 127)
						t = b1;
				}

				HTile++;
				Offset += 8 * PixWidth;
				Width -= w;
			}

			while (Width >= 8)
			{
				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
				{
					DrawTile(Tile, Offset, VirtAlign, Lines);
					t++;
					if (HTile == 31)
						t = b2;
					else
					if (HTile == 63)
						t = b1;
				}
				else
				{
					if (!(Tile & H_FLIP))
						DrawTile(TILE_PLUS(Tile, (HTile & 1)), Offset, VirtAlign, Lines);
					else
						DrawTile(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, VirtAlign, Lines);
					t += HTile & 1;
					if (HTile == 63)
						t = b2;
					else
					if (HTile == 127)
						t = b1;
				}

				HTile++;
				Offset += 8 * PixWidth;
				Width -= 8;
			}

			if (Width)
			{
				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
					DrawClippedTile(Tile, Offset, 0, Width, VirtAlign, Lines);
				else
				{
					if (!(Tile & H_FLIP))
						DrawClippedTile(TILE_PLUS(Tile, (HTile & 1)), Offset, 0, Width, VirtAlign, Lines);
					else
						DrawClippedTile(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, 0, Width, VirtAlign, Lines);
				}
			}
		}
	}
}




static void DrawBackgroundMosaic (const SRenderRegs *R, int bg, uint8 Zh, uint8 Zl)
{
	BG.TileAddress = R->BG[bg].NameBase << 1;

	uint32	Tile;
	uint16	*SC0, *SC1, *SC2, *SC3;

	SC0 = (uint16 *) &Memory.VRAM[R->BG[bg].SCBase << 1];
	SC1 = (R->BG[bg].SCSize & 1) ? SC0 + 1024 : SC0;
	if (SC1 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC1 -= 0x8000;
	SC2 = (R->BG[bg].SCSize & 2) ? SC1 + 1024 : SC0;
	if (SC2 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC2 -= 0x8000;
	SC3 = (R->BG[bg].SCSize & 1) ? SC2 + 1024 : SC2;
	if (SC3 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC3 -= 0x8000;

	int	Lines;
	int	OffsetMask  = (BG.TileSizeH == 16) ? 0x3ff : 0x1ff;
	int	OffsetShift = (BG.TileSizeV == 16) ? 4 : 3;
	int	PixWidth = R->DoubleWidthPixels ? 2 : 1;
	bool8	HiresInterlace = R->Interlace && R->DoubleWidthPixels;

	void (*DrawPix) (uint32, uint32, uint32, uint32, uint32, uint32);

	int	MosaicStart = ((uint32) GFX.StartY - R->MosaicStart) % R->Mosaic;

	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
			DrawPix = GFX.DrawMosaicPixelMath;
		else
			DrawPix = GFX.DrawMosaicPixelNomath;

		for (uint32 Y = GFX.StartY - MosaicStart; Y <= GFX.EndY; Y += R->Mosaic)
		{
			uint32	Y2 = HiresInterlace ? Y * 2 : Y;
			uint32	VOffset = LineData[Y + MosaicStart].BG[bg].VOffset + (HiresInterlace ? 1 : 0);
			uint32	HOffset = LineData[Y + MosaicStart].BG[bg].HOffset;

			Lines = R->Mosaic - MosaicStart;
			if (Y + MosaicStart + Lines > GFX.EndY)
				Lines = GFX.EndY - Y - MosaicStart + 1;

			int	VirtAlign = (((Y2 + VOffset) & 7) >> (HiresInterlace ? 1 : 0)) << 3;

			uint32	t1, t2;
			uint32	TilemapRow = (VOffset + Y2) >> OffsetShift;
			BG.InterlaceLine = ((VOffset + Y2) & 1) << 3;

			if ((VOffset + Y2) & 8)
			{
				t1 = 16;
				t2 = 0;
			}
			else
			{
				t1 = 0;
				t2 = 16;
			}

			uint16	*b1, *b2;

			if (TilemapRow & 0x20)
			{
				b1 = SC2;
				b2 = SC3;
			}
			else
			{
				b1 = SC0;
				b2 = SC1;
			}

			b1 += (TilemapRow & 0x1f) << 5;
			b2 += (TilemapRow & 0x1f) << 5;

			uint32	Left   = GFX.Clip[bg].Left[clip];
			uint32	Right  = GFX.Clip[bg].Right[clip];
			uint32	Offset = Left * PixWidth + (Y + MosaicStart) * GFX.PPL;
			uint32	HPos   = (HOffset + Left - (Left % R->Mosaic)) & OffsetMask;
			uint32	HTile  = HPos >> 3;
			uint16	*t;

			if (BG.TileSizeH == 8)
			{
				if (HTile > 31)
					t = b2 + (HTile & 0x1f);
				else
					t = b1 + HTile;
			}
			else
			{
				if (HTile > 63)
					t = b2 + ((HTile >> 1) & 0x1f);
				else
					t = b1 + (HTile >> 1);
			}

			uint32	Width = Right - Left;

			HPos &= 7;

			while (Left < Right)
			{
				uint32	w = R->Mosaic - (Left % R->Mosaic);
				if (w > Width)
					w = Width;

				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
					DrawPix(Tile, Offset, VirtAlign, HPos & 7, w, Lines);
				else
				{
					if (!(Tile & H_FLIP))
						DrawPix(TILE_PLUS(Tile, (HTile & 1)), Offset, VirtAlign, HPos & 7, w, Lines);
					else
						DrawPix(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, VirtAlign, HPos & 7, w, Lines);
				}

				HPos += R->Mosaic;

				while (HPos >= 8)
				{
					HPos -= 8;

					if (BG.TileSizeH == 8)
					{
						t++;
						if (HTile == 31)
							t = b2;
						else
						if (HTile == 63)
							t = b1;
					}
					else
					{
						t += HTile & 1;
						if (HTile == 63)
							t = b2;
						else
						if (HTile == 127)
							t = b1;
					}

					HTile++;
				}

				Offset += w * PixWidth;
				Width -= w;
				Left += w;
			}

			MosaicStart = 0;
		}
	}
}




static void DrawBackgroundOffset (const SRenderRegs *R, int bg, uint8 Zh, uint8 Zl, int VOffOff)
{
	BG.TileAddress = R->BG[bg].NameBase << 1;

	uint32	Tile;
	uint16	*SC0, *SC1, *SC2, *SC3;
	uint16	*BPS0, *BPS1, *BPS2, *BPS3;

	BPS0 = (uint16 *) &Memory.VRAM[R->BG[2].SCBase << 1];
	BPS1 = (R->BG[2].SCSize & 1) ? BPS0 + 1024 : BPS0;
	if (BPS1 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS1 -= 0x8000;
	BPS2 = (R->BG[2].SCSize & 2) ? BPS1 + 1024 : BPS0;
	if (BPS2 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS2 -= 0x8000;
	BPS3 = (R->BG[2].SCSize & 1) ? BPS2 + 1024 : BPS2;
	if (BPS3 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS3 -= 0x8000;

	SC0 = (uint16 *) &Memory.VRAM[R->BG[bg].SCBase << 1];
	SC1 = (R->BG[bg].SCSize & 1) ? SC0 + 1024 : SC0;
	if (SC1 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC1 -= 0x8000;
	SC2 = (R->BG[bg].SCSize & 2) ? SC1 + 1024 : SC0;
	if (SC2 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC2 -= 0x8000;
	SC3 = (R->BG[bg].SCSize & 1) ? SC2 + 1024 : SC2;
	if (SC3 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC3 -= 0x8000;

	int	OffsetMask   = (BG.TileSizeH   == 16) ? 0x3ff : 0x1ff;
	int	OffsetShift  = (BG.TileSizeV   == 16) ? 4 : 3;
	int	Offset2Mask  = (BG.OffsetSizeH == 16) ? 0x3ff : 0x1ff;
	int	Offset2Shift = (BG.OffsetSizeV == 16) ? 4 : 3;
	int	OffsetEnableMask = 0x2000 << bg;
	int	PixWidth = R->DoubleWidthPixels ? 2 : 1;
	bool8	HiresInterlace = R->Interlace && R->DoubleWidthPixels;

	void (*DrawClippedTile) (uint32, uint32, uint32, uint32, uint32, uint32);

	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
			DrawClippedTile = GFX.DrawClippedTileMath;
		else
			DrawClippedTile = GFX.DrawClippedTileNomath;

		for (uint32 Y = GFX.StartY; Y <= GFX.EndY; Y++)
		{
			uint32	Y2 = HiresInterlace ? Y * 2 + R->InterlaceField : Y;
			uint32	VOff = LineData[Y].BG[2].VOffset - 1;
			uint32	HOff = LineData[Y].BG[2].HOffset;
			uint32	HOffsetRow = VOff >> Offset2Shift;
			uint32	VOffsetRow = (VOff + VOffOff) >> Offset2Shift;
			uint16	*s, *s1, *s2;

			if (HOffsetRow & 0x20)
			{
				s1 = BPS2;
				s2 = BPS3;
			}
			else
			{
				s1 = BPS0;
				s2 = BPS1;
			}

			s1 += (HOffsetRow & 0x1f) << 5;
			s2 += (HOffsetRow & 0x1f) << 5;
			s = ((VOffsetRow & 0x20) ? BPS2 : BPS0) + ((VOffsetRow & 0x1f) << 5);
			int32	VOffsetOffset = s - s1;

			uint32	Left  = GFX.Clip[bg].Left[clip];
			uint32	Right = GFX.Clip[bg].Right[clip];
			uint32	Offset = Left * PixWidth + Y * GFX.PPL;
			uint32	HScroll = LineData[Y].BG[bg].HOffset;
			bool8	left_edge = (Left < (8 - (HScroll & 7)));
			uint32	Width = Right - Left;

			while (Left < Right)
			{
				uint32	VOffset, HOffset;

				if (left_edge)
				{
					VOffset = LineData[Y].BG[bg].VOffset;
					HOffset = HScroll;
					left_edge = FALSE;
				}
				else
				{
					int HOffTile = ((HOff + Left - 1) & Offset2Mask) >> 3;

					if (BG.OffsetSizeH == 8)
					{
						if (HOffTile > 31)
							s = s2 + (HOffTile & 0x1f);
						else
							s = s1 + HOffTile;
					}
					else
					{
						if (HOffTile > 63)
							s = s2 + ((HOffTile >> 1) & 0x1f);
						else
							s = s1 + (HOffTile >> 1);
					}

					uint16	HCellOffset = READ_WORD(s);
					uint16	VCellOffset;

					if (VOffOff)
						VCellOffset = READ_WORD(s + VOffsetOffset);
					else
					{
						if (HCellOffset & 0x8000)
						{
							VCellOffset = HCellOffset;
							HCellOffset = 0;
						}
						else
							VCellOffset = 0;
					}

					if (VCellOffset & OffsetEnableMask)
						VOffset = VCellOffset + 1;
					else
						VOffset = LineData[Y].BG[bg].VOffset;

					if (HCellOffset & OffsetEnableMask)
						HOffset = (HCellOffset & ~7) | (HScroll & 7);
					else
						HOffset = HScroll;
				}

				if (HiresInterlace)
					VOffset++;

				uint32	t1, t2;
				int		VirtAlign = (((Y2 + VOffset) & 7) >> (HiresInterlace ? 1 : 0)) << 3;
				int		TilemapRow = (VOffset + Y2) >> OffsetShift;
				BG.InterlaceLine = ((VOffset + Y2) & 1) << 3;

				if ((VOffset + Y2) & 8)
				{
					t1 = 16;
					t2 = 0;
				}
				else
				{
					t1 = 0;
					t2 = 16;
				}

				uint16	*b1, *b2;

				if (TilemapRow & 0x20)
				{
					b1 = SC2;
					b2 = SC3;
				}
				else
				{
					b1 = SC0;
					b2 = SC1;
				}

				b1 += (TilemapRow & 0x1f) << 5;
				b2 += (TilemapRow & 0x1f) << 5;

				uint32	HPos = (HOffset + Left) & OffsetMask;
				uint32	HTile = HPos >> 3;
				uint16	*t;

				if (BG.TileSizeH == 8)
				{
					if (HTile > 31)
						t = b2 + (HTile & 0x1f);
					else
						t = b1 + HTile;
				}
				else
				{
					if (HTile > 63)
						t = b2 + ((HTile >> 1) & 0x1f);
					else
						t = b1 + (HTile >> 1);
				}

				uint32	l = HPos & 7;
				uint32	w = 8 - l;
				if (w > Width)
					w = Width;

				Offset -= l * PixWidth;
				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
					DrawClippedTile(Tile, Offset, l, w, VirtAlign, 1);
				else
				{
					if (!(Tile & H_FLIP))
						DrawClippedTile(TILE_PLUS(Tile, (HTile & 1)), Offset, l, w, VirtAlign, 1);
					else
						DrawClippedTile(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, l, w, VirtAlign, 1);
				}

				Left += w;
				Offset += 8 * PixWidth;
				Width -= w;
			}
		}
	}
}




static void DrawBackgroundOffsetMosaic (const SRenderRegs *R, int bg, uint8 Zh, uint8 Zl, int VOffOff)
{
	BG.TileAddress = R->BG[bg].NameBase << 1;

	uint32	Tile;
	uint16	*SC0, *SC1, *SC2, *SC3;
	uint16	*BPS0, *BPS1, *BPS2, *BPS3;

	BPS0 = (uint16 *) &Memory.VRAM[R->BG[2].SCBase << 1];
	BPS1 = (R->BG[2].SCSize & 1) ? BPS0 + 1024 : BPS0;
	if (BPS1 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS1 -= 0x8000;
	BPS2 = (R->BG[2].SCSize & 2) ? BPS1 + 1024 : BPS0;
	if (BPS2 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS2 -= 0x8000;
	BPS3 = (R->BG[2].SCSize & 1) ? BPS2 + 1024 : BPS2;
	if (BPS3 >= (uint16 *) (Memory.VRAM + 0x10000))
		BPS3 -= 0x8000;

	SC0 = (uint16 *) &Memory.VRAM[R->BG[bg].SCBase << 1];
	SC1 = (R->BG[bg].SCSize & 1) ? SC0 + 1024 : SC0;
	if (SC1 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC1 -= 0x8000;
	SC2 = (R->BG[bg].SCSize & 2) ? SC1 + 1024 : SC0;
	if (SC2 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC2 -= 0x8000;
	SC3 = (R->BG[bg].SCSize & 1) ? SC2 + 1024 : SC2;
	if (SC3 >= (uint16 *) (Memory.VRAM + 0x10000))
		SC3 -= 0x8000;

	int	Lines;
	int	OffsetMask   = (BG.TileSizeH   == 16) ? 0x3ff : 0x1ff;
	int	OffsetShift  = (BG.TileSizeV   == 16) ? 4 : 3;
	int	Offset2Shift = (BG.OffsetSizeV == 16) ? 4 : 3;
	int	OffsetEnableMask = 0x2000 << bg;
	int	PixWidth = R->DoubleWidthPixels ? 2 : 1;
	bool8	HiresInterlace = R->Interlace && R->DoubleWidthPixels;

	void (*DrawPix) (uint32, uint32, uint32, uint32, uint32, uint32);

	int	MosaicStart = ((uint32) GFX.StartY - R->MosaicStart) % R->Mosaic;

	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
			DrawPix = GFX.DrawMosaicPixelMath;
		else
			DrawPix = GFX.DrawMosaicPixelNomath;

		for (uint32 Y = GFX.StartY - MosaicStart; Y <= GFX.EndY; Y += R->Mosaic)
		{
			uint32	Y2 = HiresInterlace ? Y * 2 : Y;
			uint32	VOff = LineData[Y + MosaicStart].BG[2].VOffset - 1;
			uint32	HOff = LineData[Y + MosaicStart].BG[2].HOffset;

			Lines = R->Mosaic - MosaicStart;
			if (Y + MosaicStart + Lines > GFX.EndY)
				Lines = GFX.EndY - Y - MosaicStart + 1;

			uint32	HOffsetRow = VOff >> Offset2Shift;
			uint32	VOffsetRow = (VOff + VOffOff) >> Offset2Shift;
			uint16	*s, *s1, *s2;

			if (HOffsetRow & 0x20)
			{
				s1 = BPS2;
				s2 = BPS3;
			}
			else
			{
				s1 = BPS0;
				s2 = BPS1;
			}

			s1 += (HOffsetRow & 0x1f) << 5;
			s2 += (HOffsetRow & 0x1f) << 5;
			s = ((VOffsetRow & 0x20) ? BPS2 : BPS0) + ((VOffsetRow & 0x1f) << 5);
			int32	VOffsetOffset = s - s1;

			uint32	Left =  GFX.Clip[bg].Left[clip];
			uint32	Right = GFX.Clip[bg].Right[clip];
			uint32	Offset = Left * PixWidth + (Y + MosaicStart) * GFX.PPL;
			uint32	HScroll = LineData[Y + MosaicStart].BG[bg].HOffset;
			uint32	Width = Right - Left;

			while (Left < Right)
			{
				uint32	VOffset, HOffset;

				if (Left < (8 - (HScroll & 7)))
				{
					VOffset = LineData[Y + MosaicStart].BG[bg].VOffset;
					HOffset = HScroll;
				}
				else
				{
					int HOffTile = (((Left + (HScroll & 7)) - 8) + (HOff & ~7)) >> 3;

					if (BG.OffsetSizeH == 8)
					{
						if (HOffTile > 31)
							s = s2 + (HOffTile & 0x1f);
						else
							s = s1 + HOffTile;
					}
					else
					{
						if (HOffTile > 63)
							s = s2 + ((HOffTile >> 1) & 0x1f);
						else
							s = s1 + (HOffTile >> 1);
					}

					uint16	HCellOffset = READ_WORD(s);
					uint16	VCellOffset;

					if (VOffOff)
						VCellOffset = READ_WORD(s + VOffsetOffset);
					else
					{
						if (HCellOffset & 0x8000)
						{
							VCellOffset = HCellOffset;
							HCellOffset = 0;
						}
						else
							VCellOffset = 0;
					}

					if (VCellOffset & OffsetEnableMask)
						VOffset = VCellOffset + 1;
					else
						VOffset = LineData[Y + MosaicStart].BG[bg].VOffset;

					if (HCellOffset & OffsetEnableMask)
						HOffset = (HCellOffset & ~7) | (HScroll & 7);
					else
						HOffset = HScroll;
				}

				if (HiresInterlace)
					VOffset++;

				uint32	t1, t2;
				int		VirtAlign = (((Y2 + VOffset) & 7) >> (HiresInterlace ? 1 : 0)) << 3;
				int		TilemapRow = (VOffset + Y2) >> OffsetShift;
				BG.InterlaceLine = ((VOffset + Y2) & 1) << 3;

				if ((VOffset + Y2) & 8)
				{
					t1 = 16;
					t2 = 0;
				}
				else
				{
					t1 = 0;
					t2 = 16;
				}

				uint16	*b1, *b2;

				if (TilemapRow & 0x20)
				{
					b1 = SC2;
					b2 = SC3;
				}
				else
				{
					b1 = SC0;
					b2 = SC1;
				}

				b1 += (TilemapRow & 0x1f) << 5;
				b2 += (TilemapRow & 0x1f) << 5;

				uint32	HPos = (HOffset + Left - (Left % R->Mosaic)) & OffsetMask;
				uint32	HTile = HPos >> 3;
				uint16	*t;

				if (BG.TileSizeH == 8)
				{
					if (HTile > 31)
						t = b2 + (HTile & 0x1f);
					else
						t = b1 + HTile;
				}
				else
				{
					if (HTile > 63)
						t = b2 + ((HTile >> 1) & 0x1f);
					else
						t = b1 + (HTile >> 1);
				}

				uint32	w = R->Mosaic - (Left % R->Mosaic);
				if (w > Width)
					w = Width;

				Tile = READ_WORD(t);
				GFX.Z1 = GFX.Z2 = (Tile & 0x2000) ? Zh : Zl;

				if (BG.TileSizeV == 16)
					Tile = TILE_PLUS(Tile, ((Tile & V_FLIP) ? t2 : t1));

				if (BG.TileSizeH == 8)
					DrawPix(Tile, Offset, VirtAlign, HPos & 7, w, Lines);
				else
				{
					if (!(Tile & H_FLIP))
						DrawPix(TILE_PLUS(Tile, (HTile & 1)), Offset, VirtAlign, HPos & 7, w, Lines);
					else
					if (!(Tile & V_FLIP))
						DrawPix(TILE_PLUS(Tile, 1 - (HTile & 1)), Offset, VirtAlign, HPos & 7, w, Lines);
				}

				Left += w;
				Offset += w * PixWidth;
				Width -= w;
			}

			MosaicStart = 0;
		}
	}
}




static inline void DrawBackgroundMode7 (const SRenderRegs *R, int bg, void (*DrawMath) (uint32, uint32, int), void (*DrawNomath) (uint32, uint32, int), int D)
{
	for (int clip = 0; clip < GFX.Clip[bg].Count; clip++)
	{
		GFX.ClipColors = !(GFX.Clip[bg].DrawMode[clip] & 1);

		if (BG.EnableMath && (GFX.Clip[bg].DrawMode[clip] & 2))
			DrawMath(GFX.Clip[bg].Left[clip], GFX.Clip[bg].Right[clip], D);
		else
			DrawNomath(GFX.Clip[bg].Left[clip], GFX.Clip[bg].Right[clip], D);
	}
}








void S9xReRefresh (void)
{
	if (Settings.Paused)
		S9xDeinitUpdate(IPPU.RenderedScreenWidth, IPPU.RenderedScreenHeight);
}

void S9xSetInfoString (const char *string)
{
	if (Settings.InitialInfoStringTimeout > 0)
	{
		GFX.InfoString = string;
		GFX.InfoStringTimeout = Settings.InitialInfoStringTimeout;
		S9xReRefresh();
	}
}

#include "var8x10font.h"
static const int font_width = 8;
static const int font_height = 10;

static inline int CharWidth(uint8 c)
{
	return font_width - var8x10font_kern[c - 32][0] - var8x10font_kern[c - 32][1];
}

static int StringWidth(const char* str)
{
	int length = strlen(str);
	int pixcount = 0;

	if (length > 0)
		pixcount++;

	for (int i = 0; i < length; i++)
		pixcount += (CharWidth(str[i]) - 1);

	return pixcount;
}

static void VariableDisplayChar(int x, int y, uint8 c, bool monospace = false, int overlap = 0)
{
	int cindex = c - 32;
	int crow = cindex >> 4;
	int ccol = cindex & 15;
	int cwidth = font_width - (monospace ? 0 : (var8x10font_kern[cindex][0] + var8x10font_kern[cindex][1]));

	int	line = crow * font_height;
	int	offset = ccol * font_width + (monospace ? 0 : var8x10font_kern[cindex][0]);
	int scale = IPPU.RenderedScreenWidth / SNES_WIDTH;

	uint16* s = GFX.Screen + y * GFX.RealPPL + x * scale;

	for (int h = 0; h < font_height; h++, line++, s += GFX.RealPPL - cwidth * scale)
	{
		for (int w = 0; w < cwidth; w++, s++)
		{
			if (var8x10font[line][offset + w] == '#')
				*s = Settings.DisplayColor;
			else if (var8x10font[line][offset + w] == '.')
				*s = 0x0000;

			if (scale > 1)
			{
				s[1] = s[0];
				s++;
			}
		}
	}
}

void S9xVariableDisplayString(const char* string, int linesFromBottom, int pixelsFromLeft, bool allowWrap, int type)
{
	if (GFX.ScreenBuffer.empty() || IPPU.RenderedScreenWidth == 0)
		return;

	bool monospace = true;
	if (type == S9X_NO_INFO)
	{
		if (linesFromBottom <= 0)
			linesFromBottom = 1;

		if (linesFromBottom >= 5 && !Settings.DisplayPressedKeys)
		{
			if (!Settings.DisplayPressedKeys)
				linesFromBottom -= 3;
			else
				linesFromBottom -= 1;
		}

		if (pixelsFromLeft > 128)
			pixelsFromLeft = SNES_WIDTH - StringWidth(string);

		monospace = false;
	}

	int min_lines = 1;
	std::string msg(string);
	for (auto& c : msg)
		if (c == '\n')
			min_lines++;
	if (min_lines > linesFromBottom)
		linesFromBottom = min_lines;

	int dst_x = pixelsFromLeft;
	int dst_y = IPPU.RenderedScreenHeight - (font_height)*linesFromBottom;
	int len = strlen(string);

	if (IPPU.RenderedScreenHeight % 224 && !Settings.ShowOverscan)
		dst_y -= 8;
	else if (Settings.ShowOverscan)
		dst_y += 8;

	int overlap = 0;

	for (int i = 0; i < len; i++)
	{
		int cindex = (uint8)string[i] - 32;
		int char_width = font_width - (monospace ? 1 : (var8x10font_kern[cindex][0] + var8x10font_kern[cindex][1]));

		if (dst_x + char_width > SNES_WIDTH || string[i] == '\n')
		{
			if (!allowWrap)
				break;

			linesFromBottom--;
			dst_y = IPPU.RenderedScreenHeight - font_height * linesFromBottom;
			dst_x = pixelsFromLeft;

			if (dst_y >= IPPU.RenderedScreenHeight)
				break;
		}

		if (string[i] == '\n')
			continue;

		VariableDisplayChar(dst_x, dst_y, string[i], monospace, overlap);

		dst_x += char_width - 1;
		overlap = 1;
	}
}

static void DisplayStringFromBottom(const char* string, int linesFromBottom, int pixelsFromLeft, bool allowWrap)
{
	if (S9xCustomDisplayString)
	{
		S9xCustomDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, S9X_NO_INFO);
		return;
	}

	S9xVariableDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, S9X_NO_INFO);
}

static void S9xDisplayStringType(const char* string, int linesFromBottom, int pixelsFromLeft, bool allowWrap, int type)
{
	if (S9xCustomDisplayString)
	{
		S9xCustomDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, type);
		return;
	}

	S9xVariableDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, type);
}

static void DisplayTime (void)
{
	char string[10];

	time_t rawtime;
	struct tm *timeinfo;

	time (&rawtime);
	timeinfo = localtime(&rawtime);

	sprintf(string, "%02u:%02u", timeinfo->tm_hour, timeinfo->tm_min);
	S9xDisplayString(string, 0, 0, false);
}

static void DisplayFrameRate (void)
{
	char	string[10];
	static uint32 lastFrameCount = 0, calcFps = 0;
	static time_t lastTime = time(NULL);

	time_t currTime = time(NULL);
	if (lastTime != currTime) {
		if (lastFrameCount < IPPU.TotalEmulatedFrames) {
			calcFps = (IPPU.TotalEmulatedFrames - lastFrameCount) / (uint32)(currTime - lastTime);
		}
		lastTime = currTime;
		lastFrameCount = IPPU.TotalEmulatedFrames;
	}
	sprintf(string, "%u fps", calcFps);
	S9xDisplayString(string, 2, IPPU.RenderedScreenWidth - (font_width - 1) * strlen(string) - 1, false);

#ifdef DEBUGGER
	const int	len = 8;
	sprintf(string, "%02d/%02d %02d", (int) IPPU.DisplayedRenderedFrameCount, (int) Memory.ROMFramesPerSecond, (int) IPPU.FrameCount);
#else
	const int	len = 5;
	sprintf(string, "%02d/%02d",      (int) IPPU.DisplayedRenderedFrameCount, (int) Memory.ROMFramesPerSecond);
#endif

	S9xDisplayString(string, 1, IPPU.RenderedScreenWidth - (font_width - 1) * len - 1, false);
}

static void DisplayPressedKeys (void)
{
	static unsigned char	KeyMap[]   = { '0', '1', '2', 'R', 'L', 'X', 'A', 225, 224, 227, 226, 'S', 's', 'Y', 'B' };
	static int		KeyOrder[] = { 8, 10, 7, 9, 0, 6, 14, 13, 5, 1, 4, 3, 2, 11, 12 };

	enum controllers	controller;
	int					line = Settings.DisplayMovieFrame && S9xMovieActive() ? 2 : 1;
	int8				ids[4];
	char				string[255];

	for (int port = 0; port < 2; port++)
	{
		S9xGetController(port, &controller, &ids[0], &ids[1], &ids[2], &ids[3]);

		switch (controller)
		{
			case CTL_MOUSE:
			{
				uint8 buf[5];
				if (!MovieGetMouse(port, buf))
					break;
				int16 x = READ_WORD(buf);
				int16 y = READ_WORD(buf + 2);
				uint8 buttons = buf[4];
				sprintf(string, "#%d %d: (%03d,%03d) %c%c", port + 1, ids[0] + 1, x, y,
						(buttons & 0x40) ? 'L' : ' ', (buttons & 0x80) ? 'R' : ' ');
				S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
				break;
			}

			case CTL_SUPERSCOPE:
			{
				uint8 buf[6];
				if (!MovieGetScope(port, buf))
					break;
				int16 x = READ_WORD(buf);
				int16 y = READ_WORD(buf + 2);
				uint8 buttons = buf[4];
				sprintf(string, "#%d %d: (%03d,%03d) %c%c%c%c", port + 1, ids[0] + 1, x, y,
						(buttons & 0x80) ? 'F' : ' ', (buttons & 0x40) ? 'C' : ' ',
						(buttons & 0x20) ? 'T' : ' ', (buttons & 0x10) ? 'P' : ' ');
				S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
				break;
			}

			case CTL_JUSTIFIER:
			{
				uint8 buf[11];
				if (!MovieGetJustifier(port, buf))
					break;
				int16 x1 = READ_WORD(buf);
				int16 x2 = READ_WORD(buf + 2);
				int16 y1 = READ_WORD(buf + 4);
				int16 y2 = READ_WORD(buf + 6);
				uint8 buttons = buf[8];
				bool8 offscreen1 = buf[9];
				bool8 offscreen2 = buf[10];
				sprintf(string, "#%d %d: (%03d,%03d) %c%c%c / (%03d,%03d) %c%c%c", port + 1, ids[0] + 1,
						x1, y1, (buttons & 0x80) ? 'T' : ' ', (buttons & 0x20) ? 'S' : ' ', offscreen1 ? 'O' : ' ',
						x2, y2, (buttons & 0x40) ? 'T' : ' ', (buttons & 0x10) ? 'S' : ' ', offscreen2 ? 'O' : ' ');
				S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
				break;
			}

			case CTL_JOYPAD:
			{
				sprintf(string, "#%d %d:                  ", port + 1, ids[0] + 1);
				uint16 pad = MovieGetJoypad(ids[0]);
				for (int i = 0; i < 15; i++)
				{
					int j = KeyOrder[i];
					int mask = (1 << (j + 1));
					string[6 + i]= (pad & mask) ? KeyMap[j] : ' ';
				}

				S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
				break;
			}

			case CTL_MP5:
			{
				for (int n = 0; n < 4; n++)
				{
					if (ids[n] != -1)
					{
						sprintf(string, "#%d %d:                  ", port + 1, ids[n] + 1);
						uint16 pad = MovieGetJoypad(ids[n]);
						for (int i = 0; i < 15; i++)
						{
							int j = KeyOrder[i];
							int mask = (1 << (j + 1));
							string[6 + i]= (pad & mask) ? KeyMap[j] : ' ';
						}

						S9xDisplayStringType(string, line++, 1, false, S9X_PRESSED_KEYS_INFO);
					}
				}

				break;
			}

			case CTL_MACSRIFLE:
				break;

			case CTL_NONE:
				break;
		}
	}
}

static void DisplayWatchedAddresses (void)
{
	for (unsigned int i = 0; i < sizeof(watches) / sizeof(watches[0]); i++)
	{
		if (!watches[i].on)
			break;

		int32	displayNumber = 0;
		char	buf[64];

		for (int r = 0; r < watches[i].size; r++)
			displayNumber += (Cheat.CWatchRAM[(watches[i].address - 0x7E0000) + r]) << (8 * r);

		if (watches[i].format == 1)
			sprintf(buf, "%s,%du = %u", watches[i].desc, watches[i].size, (unsigned int) displayNumber);
		else
		if (watches[i].format == 3)
			sprintf(buf, "%s,%dx = %X", watches[i].desc, watches[i].size, (unsigned int) displayNumber);
		else
		{
			if (watches[i].size == 1)
				displayNumber = (int32) ((int8)  displayNumber);
			else if (watches[i].size == 2)
				displayNumber = (int32) ((int16) displayNumber);
			else if (watches[i].size == 3)
				if (displayNumber >= 8388608)
					displayNumber -= 16777216;

			sprintf(buf, "%s,%ds = %d", watches[i].desc, watches[i].size, (int) displayNumber);
		}

		S9xDisplayString(buf, 6 + i, 1, false);
	}
}

void S9xDisplayMessages (uint16 *screen, int ppl, int width, int height, int scale)
{
	if (Settings.DisplayTime)
		DisplayTime();

	if (Settings.DisplayFrameRate)
		DisplayFrameRate();

	if (Settings.DisplayWatchedAddresses)
		DisplayWatchedAddresses();

	if (Settings.DisplayPressedKeys)
		DisplayPressedKeys();

	if (Settings.DisplayMovieFrame && S9xMovieActive())
		S9xDisplayString(GFX.FrameDisplayString, 1, 1, false);

	if (!GFX.InfoString.empty())
		S9xDisplayString(GFX.InfoString.c_str(), 5, 1, true);
}

static uint16 get_crosshair_color (uint8 color)
{
	switch (color & 15)
	{
		case  0: return (BUILD_PIXEL( 0,  0,  0));
		case  1: return (BUILD_PIXEL( 0,  0,  0));
		case  2: return (BUILD_PIXEL( 8,  8,  8));
		case  3: return (BUILD_PIXEL(16, 16, 16));
		case  4: return (BUILD_PIXEL(23, 23, 23));
		case  5: return (BUILD_PIXEL(31, 31, 31));
		case  6: return (BUILD_PIXEL(31,  0,  0));
		case  7: return (BUILD_PIXEL(31, 16,  0));
		case  8: return (BUILD_PIXEL(31, 31,  0));
		case  9: return (BUILD_PIXEL( 0, 31,  0));
		case 10: return (BUILD_PIXEL( 0, 31, 31));
		case 11: return (BUILD_PIXEL( 0, 23, 31));
		case 12: return (BUILD_PIXEL( 0,  0, 31));
		case 13: return (BUILD_PIXEL(23,  0, 31));
		case 14: return (BUILD_PIXEL(31,  0, 31));
		case 15: return (BUILD_PIXEL(31,  0, 16));
	}

	return (0);
}

void S9xDrawCrosshair (const char *crosshair, uint8 fgcolor, uint8 bgcolor, int16 x, int16 y)
{
	if (!crosshair)
		return;

	int16	r, rx = 1, c, cx = 1, W = SNES_WIDTH, H = (int16) IPPU.RenderedScreenHeight;
	uint16	fg, bg;

	x -= 7;
	y -= 7;

	if (IPPU.DoubleWidthPixels)  { cx = 2; x *= 2; W *= 2; }
	if (IPPU.DoubleHeightPixels) { rx = 2; y *= 2; H *= 2; }

	fg = get_crosshair_color(fgcolor);
	bg = get_crosshair_color(bgcolor);

	uint16	*s = GFX.Screen + y * (int32)GFX.RealPPL + x;

	for (r = 0; r < 15 * rx; r++, s += GFX.RealPPL - 15 * cx)
	{
		if (y + r < 0)
		{
			s += 15 * cx;
			continue;
		}

		if (y + r >= H)
			break;

		for (c = 0; c < 15 * cx; c++, s++)
		{
			if (x + c < 0 || s < GFX.Screen)
				continue;

			if (x + c >= W)
			{
				s += 15 * cx - c;
				break;
			}

			uint8	p = crosshair[(r / rx) * 15 + (c / cx)];

			if (p == '#' && fgcolor)
				*s = (fgcolor & 0x10) ? COLOR_ADD::fn1_2(fg, *s) : fg;
			else
			if (p == '.' && bgcolor)
				*s = (bgcolor & 0x10) ? COLOR_ADD::fn1_2(*s, bg) : bg;
		}
	}
}












static void RenderThreadMain (void)
{

	uint64_t consumed_local = 0;

	for (;;)
	{
		
		uint64_t prod = RenderProduced.load(std::memory_order_seq_cst);
		if (consumed_local < prod)
		{
			const SRenderCommand &cmd =
				RenderLog.ring[consumed_local & (S9X_RENDER_RING_CAPACITY - 1)];
			bool keep_going = PROCESS_ONE_COMMAND(cmd);
			consumed_local++;
			RenderConsumed.store(consumed_local, std::memory_order_seq_cst);
			DrainEC.notify();
			if (!keep_going)
				return;		
			continue;
		}

		
		const auto	haveWork = [&consumed_local]() {
			return consumed_local < RenderProduced.load(std::memory_order_seq_cst);
		};

		if (SpinUntil(haveWork))
		{
			RT_STAT_ADD(S9X_RTS_RENDER_SPIN_HITS, 1);
			continue;
		}

		RT_STAT_ADD(S9X_RTS_RENDER_PARKS, 1);
		WorkEC.waitPred(haveWork);
	}
}











