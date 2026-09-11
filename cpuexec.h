/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _CPUEXEC_H_
#define _CPUEXEC_H_

#include "ppu.h"
#ifdef DEBUGGER
#include "debug.h"
#endif

struct SOpcodes
{
	void (*S9xOpcode) (void);
};

struct SICPU
{
	struct SOpcodes	*S9xOpcodes;
	uint8	*S9xOpLengths;
	uint8	_Carry;
	uint8	_Zero;
	uint8	_Negative;
	uint8	_Overflow;
	uint32	ShiftedPB;
	uint32	ShiftedDB;
	uint32	Frame;
	uint32	FrameAdvanceCount;
};

extern struct SICPU		ICPU;



extern uint64				S9xInstructionCounter;







extern uint64				S9xEventCount;
#define S9X_BLOCK_HISTOGRAM_BUCKETS 8
extern uint64				S9xBlockLenHistogram[S9X_BLOCK_HISTOGRAM_BUCKETS];





void S9xResetBenchCounters (void);











extern uint64	S9xAPUTouchesThisLine;
extern uint64	S9xGSUTouchesThisLine;
extern uint64	S9xAPUTotalTouches;
extern uint64	S9xGSUTotalTouches;
extern uint64	S9xTouchScanlineCount;
#define S9X_TOUCH_HISTOGRAM_BUCKETS 8
extern uint64	S9xAPUTouchHistogram[S9X_TOUCH_HISTOGRAM_BUCKETS];
extern uint64	S9xGSUTouchHistogram[S9X_TOUCH_HISTOGRAM_BUCKETS];







#define S9X_GSU_OPCODE_BUCKETS 1024
extern uint64	S9xGSUOpcodeHistogram[S9X_GSU_OPCODE_BUCKETS];
extern uint64	S9xGSUOpcodeTotal;





















extern uint64	S9xSTAT77TouchesThisLine;
extern uint64	S9xSTAT77TotalTouches;
extern uint64	S9xSTAT77TouchHistogram[S9X_TOUCH_HISTOGRAM_BUCKETS];

extern uint64	S9xUpdateScreenCallsThisLine;
extern uint64	S9xUpdateScreenTotalCalls;
extern uint64	S9xUpdateScreenCallHistogram[S9X_TOUCH_HISTOGRAM_BUCKETS];

extern uint64	S9xUpdateScreenTotalSpanLines;
extern uint64	S9xUpdateScreenSpanHistogram[S9X_TOUCH_HISTOGRAM_BUCKETS];





void S9xRecordUpdateScreenSpan (uint32 spanLines);

extern struct SOpcodes	S9xOpcodesE1[256];
extern struct SOpcodes	S9xOpcodesM1X1[256];
extern struct SOpcodes	S9xOpcodesM1X0[256];
extern struct SOpcodes	S9xOpcodesM0X1[256];
extern struct SOpcodes	S9xOpcodesM0X0[256];
extern struct SOpcodes	S9xOpcodesSlow[256];
extern uint8			S9xOpLengthsM1X1[256];
extern uint8			S9xOpLengthsM1X0[256];
extern uint8			S9xOpLengthsM0X1[256];
extern uint8			S9xOpLengthsM0X0[256];

void S9xMainLoop (void);
void S9xReset (void);
void S9xSoftReset (void);
void S9xDoHEventProcessing (void);

static inline void S9xUnpackStatus (void)
{
	ICPU._Zero = (Registers.PL & Zero) == 0;
	ICPU._Negative = (Registers.PL & Negative);
	ICPU._Carry = (Registers.PL & Carry);
	ICPU._Overflow = (Registers.PL & Overflow) >> 6;
}

static inline void S9xPackStatus (void)
{
	Registers.PL &= ~(Zero | Negative | Carry | Overflow);
	Registers.PL |= ICPU._Carry | ((ICPU._Zero == 0) << 1) | (ICPU._Negative & 0x80) | (ICPU._Overflow << 6);
}

static inline void S9xFixCycles (void)
{
	if (CheckEmulation())
	{
		ICPU.S9xOpcodes = S9xOpcodesE1;
		ICPU.S9xOpLengths = S9xOpLengthsM1X1;
	}
	else
	if (CheckMemory())
	{
		if (CheckIndex())
		{
			ICPU.S9xOpcodes = S9xOpcodesM1X1;
			ICPU.S9xOpLengths = S9xOpLengthsM1X1;
		}
		else
		{
			ICPU.S9xOpcodes = S9xOpcodesM1X0;
			ICPU.S9xOpLengths = S9xOpLengthsM1X0;
		}
	}
	else
	{
		if (CheckIndex())
		{
			ICPU.S9xOpcodes = S9xOpcodesM0X1;
			ICPU.S9xOpLengths = S9xOpLengthsM0X1;
		}
		else
		{
			ICPU.S9xOpcodes = S9xOpcodesM0X0;
			ICPU.S9xOpLengths = S9xOpLengthsM0X0;
		}
	}
}

#endif
