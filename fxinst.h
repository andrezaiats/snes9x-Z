/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#ifndef _FXINST_H_
#define _FXINST_H_




#define FX_RAM_BANKS	4


#define FX_DO_ROMBUFFER




struct FxRegs_s
{
	
	uint32	avReg[16];					
	uint32	vColorReg;					
	uint32	vPlotOptionReg;				
	uint32	vStatusReg;					
	uint32	vPrgBankReg;				
	uint32	vRomBankReg;				
	uint32	vRamBankReg;				
	uint32	vCacheBaseReg;				
	uint32	vCacheFlags;				
	uint32	vLastRamAdr;				
	uint32	*pvDreg;					
	uint32	*pvSreg;					
	uint8	vRomBuffer;					
	uint8	vPipe;						
	uint32	vPipeAdr;					

	
	uint32	vSign;						
	uint32	vZero;						
	uint32	vCarry;						
	int32	vOverflow;					

	
	int32	vErrorCode;
	uint32	vIllegalAddress;

	uint8	bBreakPoint;
	uint32	vBreakPoint;
	uint32	vStepPoint;

	uint8	*pvRegisters;				
	uint32	nRamBanks;					
	uint8	*pvRam;						
	uint32	nRomBanks;					
	uint8	*pvRom;						

	uint32	vMode;						
	uint32	vPrevMode;					
	uint8	*pvScreenBase;
	uint8	*apvScreen[32];				
	int32	x[32];
	uint32	vScreenHeight;				
	uint32	vScreenRealHeight;			
	uint32	vPrevScreenHeight;
	uint32	vScreenSize;
	void	(*pfPlot) (void);
	void	(*pfRpix) (void);

	uint8	*pvRamBank;					
	uint8	*pvRomBank;					
	uint8	*pvPrgBank;					

	uint8	*apvRamBank[FX_RAM_BANKS];	
	uint8	*apvRomBank[256];			

	uint8	bCacheActive;
	uint8	*pvCache;					
	uint8	avCacheBackup[512];			
	uint32	vCounter;
	uint32	vInstCount;
	uint32	vSCBRDirty;					
	
	uint8	*avRegAddr;					

    // Cycle-accurate GSU timing (costs in SNES master-clock cycles, derived from
    // Mesen2's GSU model). When GSU.bCycleMode is set (default), fx_run() consumes
    // a master-cycle budget instead of a flat instruction count: cached fetches
    // cost 1 (2 at 10MHz), uncached fetches and RAM/ROM data accesses 5 (6),
    // 16-byte cache-line fills 16x that, multiplies per CFGR MS0. This makes the
    // GSU's throughput distribution match hardware: cache-hot loops run at full
    // clock while plot/RAM/ROM-heavy work is paced by real access latency.
    uint32	vCycles;		    // cycles consumed in the current fx_run
    uint32	vCacheMask;		// cache lines treated as loaded (timing only)
    uint32	vCostCache;		// fetch, cache hit:      CLSR ? 1 : 2
    uint32	vCostMem;		// fetch/data, uncached:  CLSR ? 5 : 6
    uint32	vCostFmult;		// fmult/lmult: (MS0 ? 3 : 7) * (CLSR ? 1 : 2)
    uint32	vCostMult;		// mult/umult:   MS0 ? 1 : 2
    uint8	bCycleMode;		// 1 = cycle budget (default), 0 = legacy
};

extern struct FxRegs_s	GSU;



#define FX_CYC(n)	{ GSU.vCycles += (uint32) (n); }


#define GSU_R0			0x000
#define GSU_R1			0x002
#define GSU_R2			0x004
#define GSU_R3			0x006
#define GSU_R4			0x008
#define GSU_R5			0x00a
#define GSU_R6			0x00c
#define GSU_R7			0x00e
#define GSU_R8			0x010
#define GSU_R9			0x012
#define GSU_R10			0x014
#define GSU_R11			0x016
#define GSU_R12			0x018
#define GSU_R13			0x01a
#define GSU_R14			0x01c
#define GSU_R15			0x01e
#define GSU_SFR			0x030
#define GSU_BRAMR		0x033
#define GSU_PBR			0x034
#define GSU_ROMBR		0x036
#define GSU_CFGR		0x037
#define GSU_SCBR		0x038
#define GSU_CLSR		0x039
#define GSU_SCMR		0x03a
#define GSU_VCR			0x03b
#define GSU_RAMBR		0x03c
#define GSU_CBR			0x03e
#define GSU_CACHERAM	0x100


#define FLG_Z			(1 <<  1)
#define FLG_CY			(1 <<  2)
#define FLG_S			(1 <<  3)
#define FLG_OV			(1 <<  4)
#define FLG_G			(1 <<  5)
#define FLG_R			(1 <<  6)
#define FLG_ALT1		(1 <<  8)
#define FLG_ALT2		(1 <<  9)
#define FLG_IL			(1 << 10)
#define FLG_IH			(1 << 11)
#define FLG_B			(1 << 12)
#define FLG_IRQ			(1 << 15)


#define TF(a)			(GSU.vStatusReg &   FLG_##a)
#define CF(a)			(GSU.vStatusReg &= ~FLG_##a)
#define SF(a)			(GSU.vStatusReg |=  FLG_##a)


#define TS(a, b)		GSU.vStatusReg = ((GSU.vStatusReg & (~FLG_##a)) | ((!!(##b)) * FLG_##a))


#define ALT0			(!TF(ALT1) && !TF(ALT2))
#define ALT1			( TF(ALT1) && !TF(ALT2))
#define ALT2			(!TF(ALT1) &&  TF(ALT2))
#define ALT3			( TF(ALT1) &&  TF(ALT2))


#define SEX8(a)			((int32)  ((int8)   (a)))
#define SEX16(a)		((int32)  ((int16)  (a)))


#define USEX8(a)		((uint32) ((uint8)  (a)))
#define USEX16(a)		((uint32) ((uint16) (a)))
#define SUSEX16(a)		((int32)  ((uint16) (a)))


#define TSZ(num)		TS(S, ((num) & 0x8000)); TS(Z, (!USEX16(num)))


#define CLRFLAGS		GSU.vStatusReg &= ~(FLG_ALT1 | FLG_ALT2 | FLG_B); GSU.pvDreg = GSU.pvSreg = &R0


#define RAM(adr)		GSU.pvRamBank[USEX16(adr)]


#define ROM(idx)		GSU.pvRomBank[USEX16(idx)]


#define PIPE			GSU.vPipe


#define PRGBANK(idx)	GSU.pvPrgBank[USEX16(idx)]

// Update pipe from ROM, charging the per-byte fetch cost: 1 cycle from the
// GSU cache (with a one-time 16-byte line-fill charge), 5-6 from ROM/RAM.
#define FETCHPIPE \
{ \
	PIPE = PRGBANK(R15); \
	uint32 _fca = USEX16(R15 - GSU.vCacheBaseReg); \
	if (GSU.bCacheActive && _fca < 512) \
	{ \
		uint32 _flb = 1U << (_fca >> 4); \
		if (!(GSU.vCacheMask & _flb)) \
		{ \
			GSU.vCacheMask |= _flb; \
			GSU.vCycles += GSU.vCostMem << 4; \
		} \
		GSU.vCycles += GSU.vCostCache; \
	} \
	else \
		GSU.vCycles += GSU.vCostMem; \
}


#define ABS(x)			((x) < 0 ? -(x) : (x))


#define SREG			(*GSU.pvSreg)


#define DREG			(*GSU.pvDreg)

#ifndef FX_DO_ROMBUFFER


#define READR14


#define TESTR14

#else


#define READR14			GSU.vRomBuffer = ROM(R14)


#define TESTR14			if (GSU.pvDreg == &R14) READR14

#endif


#define R0				GSU.avReg[0]
#define R1				GSU.avReg[1]
#define R2				GSU.avReg[2]
#define R3				GSU.avReg[3]
#define R4				GSU.avReg[4]
#define R5				GSU.avReg[5]
#define R6				GSU.avReg[6]
#define R7				GSU.avReg[7]
#define R8				GSU.avReg[8]
#define R9				GSU.avReg[9]
#define R10				GSU.avReg[10]
#define R11				GSU.avReg[11]
#define R12				GSU.avReg[12]
#define R13				GSU.avReg[13]
#define R14				GSU.avReg[14]
#define R15				GSU.avReg[15]
#define SFR				GSU.vStatusReg
#define PBR				GSU.vPrgBankReg
#define ROMBR			GSU.vRomBankReg
#define RAMBR			GSU.vRamBankReg
#define CBR				GSU.vCacheBaseReg
#define SCBR			USEX8(GSU.pvRegisters[GSU_SCBR])
#define SCMR			USEX8(GSU.pvRegisters[GSU_SCMR])
#define COLR			GSU.vColorReg
#define POR				GSU.vPlotOptionReg
#define BRAMR			USEX8(GSU.pvRegisters[GSU_BRAMR])
#define VCR				USEX8(GSU.pvRegisters[GSU_VCR])
#define CFGR			USEX8(GSU.pvRegisters[GSU_CFGR])
#define CLSR			USEX8(GSU.pvRegisters[GSU_CLSR])








#define S9X_GSU_OPCODE_HIT(idx) ((void) 0)





















#define FX_STEP \
{ \
	uint32	vOpcode = (uint32) PIPE; \
	FETCHPIPE; \
	uint32	vDispatchIdx = (GSU.vStatusReg & 0x300) | vOpcode; \
	S9X_GSU_OPCODE_HIT(vDispatchIdx); \
	(*fx_OpcodeTable[vDispatchIdx])(); \
}

extern void (*fx_PlotTable[]) (void);
extern void (*fx_OpcodeTable[]) (void);


#define BRANCH_DELAY_RELATIVE

#endif
