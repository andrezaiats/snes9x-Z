/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "snes9x.h"
#include "memmap.h"
#include "fxinst.h"
#include "fxemu.h"
#include "cpuexec.h"

static void FxReset (struct FxInfo_s *);
static void fx_readRegisterSpace (void);
static void fx_writeRegisterSpace (void);
static void fx_updateRamBank (uint8);
static void fx_dirtySCBR (void);
static bool8 fx_checkStartAddress (void);
static uint32 FxEmulate (uint32);
static void FxCacheWriteAccess (uint16);
static void FxFlushCache (void);


void S9xInitSuperFX (void)
{
	memset((uint8 *) &GSU, 0, sizeof(struct FxRegs_s));
}

void S9xResetSuperFX (void)
{
	// FIXME: Snes9x only runs the SuperFX at the end of every line.
	// 5823405 is a magic number that seems to work for most games.
    // only used when GSU Cycle Mode is disabled (see below)
	SuperFX.speedPerLine = (uint32) (5823405 * ((1.0 / (float) Memory.ROMFramesPerSecond) / ((float) (Timings.V_Max))));
	SuperFX.oneLineDone = FALSE;
	SuperFX.vFlags = 0;
	CPU.IRQExternal = FALSE;

	FxReset(&SuperFX);
}

void S9xSetSuperFX (uint8 byte, uint16 address)
{

	switch (address)
	{
		case 0x3030:
			if ((Memory.FillRAM[0x3030] ^ byte) & FLG_G)
			{
				Memory.FillRAM[0x3030] = byte;
				if (byte & FLG_G)
				{
					if (!SuperFX.oneLineDone)
					{
						S9xSuperFXExec();
						SuperFX.oneLineDone = TRUE;
					}
				}
				else
					FxFlushCache();
			}
			else
				Memory.FillRAM[0x3030] = byte;

			break;

		case 0x3031:
			Memory.FillRAM[0x3031] = byte;
			break;

		case 0x3033:
			Memory.FillRAM[0x3033] = byte;
			break;

		case 0x3034:
			Memory.FillRAM[0x3034] = byte & 0x7f;
			break;

		case 0x3036:
			Memory.FillRAM[0x3036] = byte & 0x7f;
			break;

		case 0x3037:
			Memory.FillRAM[0x3037] = byte;
			break;

		case 0x3038:
			Memory.FillRAM[0x3038] = byte;
			fx_dirtySCBR();
			break;

		case 0x3039:
			Memory.FillRAM[0x3039] = byte;
			break;

		case 0x303a:
			Memory.FillRAM[0x303a] = byte;
			break;

		case 0x303b:
			break;

		case 0x303c:
			Memory.FillRAM[0x303c] = byte;
			fx_updateRamBank(byte);
			break;

		case 0x303f:
			Memory.FillRAM[0x303f] = byte;
			break;

		case 0x301f:
			Memory.FillRAM[0x301f] = byte;
			Memory.FillRAM[0x3000 + GSU_SFR] |= FLG_G;
			if (!SuperFX.oneLineDone)
			{
				S9xSuperFXExec();
				SuperFX.oneLineDone = TRUE;
			}

			break;

		default:
			Memory.FillRAM[address] = byte;
			if (address >= 0x3100)
				FxCacheWriteAccess(address);

			break;
	}
}

uint8 S9xGetSuperFX (uint16 address)
{
	uint8	byte;


	byte = Memory.FillRAM[address];

	if (address == 0x3031)
	{
		CPU.IRQExternal = FALSE;
		Memory.FillRAM[0x3031] = byte & 0x7f;
	}

	return (byte);
}

void S9xSuperFXExec (void)
{
	if ((Memory.FillRAM[0x3000 + GSU_SFR] & FLG_G) && (Memory.FillRAM[0x3000 + GSU_SCMR] & 0x18) != 0)
	{
        int	cs = Memory.FillRAM[0x3000 + GSU_CLSR] & 1;

		if (GSU.bCycleMode)
		{
			// Real cycle costs: the GSU runs at the master clock (21.4MHz,
			// CLSR=1) or half of it (10.7MHz, CLSR=0). Costs carry the CLSR
			// scaling, so the per-line budget is a flat master-cycle slice.

			int	ms0 = Memory.FillRAM[0x3000 + GSU_CFGR] & 0x20;

			GSU.vCostCache = cs ? 1 : 2;
			GSU.vCostMem   = cs ? 5 : 6;
			GSU.vCostFmult = (ms0 ? 3 : 7) * (cs ? 1 : 2);
			GSU.vCostMult  = ms0 ? 1 : 2;

			uint32	budget = (uint32) (Timings.H_Max > 0 ? Timings.H_Max : 1364);
			FxEmulate(budget * Settings.SuperFXClockMultiplier / 100);
		}
        else
        {
            FxEmulate((cs ? (SuperFX.speedPerLine * 5 / 2) : SuperFX.speedPerLine) * Settings.SuperFXClockMultiplier / 100);
        }

		uint16 GSUStatus = Memory.FillRAM[0x3000 + GSU_SFR] | (Memory.FillRAM[0x3000 + GSU_SFR + 1] << 8);
		if ((GSUStatus & (FLG_G | FLG_IRQ)) == FLG_IRQ)
			CPU.IRQExternal = TRUE;
	}

	// Winter Gold (#533): at the race-start pose switch, the game polls the
	// GSU-published pose cel at $70:EBC0 once per frame (V~159) and commits the
	// skier arrangement from it. On hardware the heavy course-load render keeps
	// the GSU busy one frame longer than the batch model, so that poll still
	// reads 0 ("not ready" -> skier parked, one blank frame) where snes9x already
	// shows the published cel ("ready" -> a stale arrangement is committed over
	// the new tiles = the garbled skier). Delay CPU visibility of the 0->nonzero
	// publish transition by one frame (312 lines), matching measured hardware
	// behavior (verified frame-by-frame against Mesen).
	if (Timings.GSUCelDelay > 0)
	{
		static int32	holdLines = 0;
		static uint8	prevCel = 0;
		uint8			*cel = &GSU.pvRam[0xEBC0];

		if (holdLines > 0)
		{
			if (*cel != 0)  // capture any republish during the hold
				prevCel = *cel;
			*cel = 0;
			if (--holdLines == 0)
				*cel = prevCel;
		}
		else if (prevCel == 0 && *cel != 0)
		{
			prevCel = *cel;
			*cel = 0;
			holdLines = Timings.GSUCelDelay;
		}
		else
			prevCel = *cel;
	}
}

static void FxReset (struct FxInfo_s *psFxInfo)
{
	
	memset((uint8 *) &GSU, 0, sizeof(struct FxRegs_s));

	
	GSU.pvSreg = GSU.pvDreg = &R0;

	
	GSU.pvRegisters       = psFxInfo->pvRegisters;
	GSU.nRamBanks         = psFxInfo->nRamBanks;
	GSU.pvRam             = psFxInfo->pvRam;
	GSU.nRomBanks         = psFxInfo->nRomBanks;
	GSU.pvRom             = psFxInfo->pvRom;
	GSU.vPrevScreenHeight = ~0;
	GSU.vPrevMode         = ~0;

	
	if (GSU.nRomBanks > 0x20)
		GSU.nRomBanks = 0x20;

	
	memset(GSU.pvRegisters, 0, 0x300);

	
	GSU.pvRegisters[0x3b] = 0;

	
	for (int i = 0; i < 256; i++)
	{
		uint32	b = i & 0x7f;

		if (b >= 0x40)
		{
			if (GSU.nRomBanks > 1)
				b %= GSU.nRomBanks;
			else
				b &= 1;

			GSU.apvRomBank[i] = &GSU.pvRom[b << 16];
		}
		else
		{
			b %= GSU.nRomBanks * 2;
			GSU.apvRomBank[i] = &GSU.pvRom[(b << 16) + FX_MEMORY_32K_MIRRORS];
		}
	}

	
	for (int i = 0; i < 4; i++)
	{
		GSU.apvRamBank[i] = &GSU.pvRam[(i % GSU.nRamBanks) << 16];
		GSU.apvRomBank[0x70 + i] = GSU.apvRamBank[i];
	}

	
	GSU.vPipe = 0x01;

	
	GSU.pvCache = &GSU.pvRegisters[0x100];

    // Cycle-accurate GSU timing state (see fxinst.h)
    GSU.vCycles = 0;
    GSU.vCacheMask = 0;
    GSU.vCostCache = 1;
    GSU.vCostMem = 5;
    GSU.vCostFmult = 7;
    GSU.vCostMult = 2;
    GSU.bCycleMode = Settings.DisableGSUCycleMode ? 0 : 1;

	fx_readRegisterSpace();
}

static void fx_readRegisterSpace (void)
{
	static const uint32	avHeight[] = { 128, 160, 192, 256 };
	static const uint32	avMult[]   = {  16,  32,  32,  64 };

	uint8	*p;
	int		n;

	GSU.vErrorCode = 0;

	
	p = GSU.pvRegisters;
	for (int i = 0; i < 16; i++, p += 2)
		GSU.avReg[i] = (uint32) READ_WORD(p);

	
	p = GSU.pvRegisters;
	GSU.vStatusReg     =  (uint32) READ_WORD(&p[GSU_SFR]);
	GSU.vPrgBankReg    =  (uint32) p[GSU_PBR];
	GSU.vRomBankReg    =  (uint32) p[GSU_ROMBR];
	GSU.vRamBankReg    = ((uint32) p[GSU_RAMBR]) & (FX_RAM_BANKS - 1);
	GSU.vCacheBaseReg  =  (uint32) p[GSU_CBR];
	GSU.vCacheBaseReg |= ((uint32) p[GSU_CBR + 1]) << 8;

	
	GSU.vZero     = !(GSU.vStatusReg & FLG_Z);
	GSU.vSign     =  (GSU.vStatusReg & FLG_S)  << 12;
	GSU.vOverflow =  (GSU.vStatusReg & FLG_OV) << 16;
	GSU.vCarry    =  (GSU.vStatusReg & FLG_CY) >> 2;

	
	GSU.pvRamBank = GSU.apvRamBank[GSU.vRamBankReg & 0x3];
	GSU.pvRomBank = GSU.apvRomBank[GSU.vRomBankReg];
	GSU.pvPrgBank = GSU.apvRomBank[GSU.vPrgBankReg];

	
	GSU.pvScreenBase = &GSU.pvRam[USEX8(p[GSU_SCBR]) << 10];
	n  =  (int) (!!(p[GSU_SCMR] & 0x04));
	n |= ((int) (!!(p[GSU_SCMR] & 0x20))) << 1;
	GSU.vScreenHeight = GSU.vScreenRealHeight = avHeight[n];
	GSU.vMode = p[GSU_SCMR] & 0x03;

	if (n == 3)
		GSU.vScreenSize = (256 / 8) * (256 / 8) * 32;
	else
		GSU.vScreenSize = (GSU.vScreenHeight / 8) * (256 / 8) * avMult[GSU.vMode];

	if (GSU.vPlotOptionReg & 0x10) 
		GSU.vScreenHeight = 256;

	if (GSU.pvScreenBase + GSU.vScreenSize > GSU.pvRam + (GSU.nRamBanks * 65536))
		GSU.pvScreenBase = GSU.pvRam + (GSU.nRamBanks * 65536) - GSU.vScreenSize;

	GSU.pfPlot = fx_PlotTable[GSU.vMode];
	GSU.pfRpix = fx_PlotTable[GSU.vMode + 5];

	fx_OpcodeTable[0x04c] = GSU.pfPlot;
	fx_OpcodeTable[0x14c] = GSU.pfRpix;
	fx_OpcodeTable[0x24c] = GSU.pfPlot;
	fx_OpcodeTable[0x34c] = GSU.pfRpix;

	fx_computeScreenPointers();

	
}

static void fx_writeRegisterSpace (void)
{
	uint8	*p;

	p = GSU.pvRegisters;
	for (int i = 0; i < 16; i++, p += 2)
		WRITE_WORD(p, GSU.avReg[i]);

	
	if (USEX16(GSU.vZero) == 0)
		SF(Z);
	else
		CF(Z);

	if (GSU.vSign & 0x8000)
		SF(S);
	else
		CF(S);

	if (GSU.vOverflow >= 0x8000 || GSU.vOverflow < -0x8000)
		SF(OV);
	else
		CF(OV);

	if (GSU.vCarry)
		SF(CY);
	else
		CF(CY);

	p = GSU.pvRegisters;
	WRITE_WORD(&p[GSU_SFR], GSU.vStatusReg);
	p[GSU_PBR]     = (uint8)  GSU.vPrgBankReg;
	p[GSU_ROMBR]   = (uint8)  GSU.vRomBankReg;
	p[GSU_RAMBR]   = (uint8)  GSU.vRamBankReg;
	WRITE_WORD(&p[GSU_CBR], GSU.vCacheBaseReg);

	
}


static void fx_updateRamBank (uint8 byte)
{
	
	GSU.vRamBankReg = (uint32) byte & (FX_RAM_BANKS - 1);
	GSU.pvRamBank = GSU.apvRamBank[byte & 0x3];
}


static void fx_dirtySCBR (void)
{
	GSU.vSCBRDirty = TRUE;
}

static bool8 fx_checkStartAddress (void)
{
	
	if (GSU.bCacheActive && R15 >= GSU.vCacheBaseReg && R15 < (GSU.vCacheBaseReg + 512))
		return true;


	if (SCMR & (1 << 4))
	{
		if (GSU.vPrgBankReg <= 0x5f || GSU.vPrgBankReg >= 0x80)
			return true;
	}

	if (GSU.vPrgBankReg <= 0x7f && (SCMR & (1 << 3)))
		return true;

	return false;
}


static uint32 FxEmulate (uint32 nInstructions)
{
	uint32	vCount;

	
	fx_readRegisterSpace();

	
	if (!fx_checkStartAddress())
	{
		CF(G);
		fx_writeRegisterSpace();
		

		return (0);
	}

	
	CF(IRQ);

	
	vCount = fx_run(nInstructions);

	
	fx_writeRegisterSpace();

	
	if (GSU.vErrorCode)
		return (GSU.vErrorCode);
	else
		return (vCount);
}

void fx_computeScreenPointers (void)
{
	if (GSU.vMode != GSU.vPrevMode || GSU.vPrevScreenHeight != GSU.vScreenHeight || GSU.vSCBRDirty)
	{
		GSU.vSCBRDirty = FALSE;

		
		switch (GSU.vScreenHeight)
		{
			case 128:
				switch (GSU.vMode)
				{
					case 0:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 4);
							GSU.x[i] = i <<  8;
						}

						break;

					case 1:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 5);
							GSU.x[i] = i <<  9;
						}

						break;

					case 2:
					case 3:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 6);
							GSU.x[i] = i << 10;
						}

						break;
				}

				break;

			case 160:
				switch (GSU.vMode)
				{
					case 0:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 4);
							GSU.x[i] = (i <<  8) + (i << 6);
						}

						break;

					case 1:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 5);
							GSU.x[i] = (i <<  9) + (i << 7);
						}

						break;

					case 2:
					case 3:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 6);
							GSU.x[i] = (i << 10) + (i << 8);
						}

						break;
				}

				break;

			case 192:
				switch (GSU.vMode)
				{
					case 0:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 4);
							GSU.x[i] = (i <<  8) + (i << 7);
						}

						break;

					case 1:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 5);
							GSU.x[i] = (i <<  9) + (i << 8);
						}

						break;

					case 2:
					case 3:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + (i << 6);
							GSU.x[i] = (i << 10) + (i << 9);
						}

						break;
				}

				break;

			case 256:
				switch (GSU.vMode)
				{
					case 0:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + ((i & 0x10) <<  9) + ((i & 0xf) <<  8);
							GSU.x[i] = ((i & 0x10) <<  8) + ((i & 0xf) << 4);
						}

						break;

					case 1:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + ((i & 0x10) << 10) + ((i & 0xf) <<  9);
							GSU.x[i] = ((i & 0x10) <<  9) + ((i & 0xf) << 5);
						}

						break;

					case 2:
					case 3:
						for (int i = 0; i < 32; i++)
						{
							GSU.apvScreen[i] = GSU.pvScreenBase + ((i & 0x10) << 11) + ((i & 0xf) << 10);
							GSU.x[i] = ((i & 0x10) << 10) + ((i & 0xf) << 6);
						}

						break;
				}

				break;
		}

		GSU.vPrevMode = GSU.vMode;
		GSU.vPrevScreenHeight = GSU.vScreenHeight;
	}
}


static void FxCacheWriteAccess (uint16 vAddress)
{
	

	if ((vAddress & 0x00f) == 0x00f)
		GSU.vCacheFlags |= 1 << ((vAddress & 0x1f0) >> 4);
}

static void FxFlushCache (void)
{
	GSU.vCacheFlags = 0;
	GSU.vCacheBaseReg = 0;
	GSU.bCacheActive = FALSE;
	GSU.vCacheMask = 0;
	
}

void fx_flushCache (void)
{
	
	GSU.vCacheFlags = 0;
	GSU.bCacheActive = FALSE;
	GSU.vCacheMask = 0;
}































