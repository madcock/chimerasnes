#include "chisnes.h"
#include "memmap.h"
#include "apu.h"
#include "sa1.h"
#include "spc7110.h"
#include "cpuexec.h"
#include "cpuaddr.h"
#include "cpuops.h"
#include "cpumacro.h"

#include <retro_inline.h>

#ifdef SA1_OPCODES
	#define NOT_SA1(...)
#else
	#define NOT_SA1(...) __VA_ARGS__
#endif

#define rOp8Body(Addr, Wrap, Func)                        \
	{                                                     \
		uint8_t val = ICPU.OpenBus = GetByte(Addr(READ)); \
		Func##8(val);                                     \
	}

#define rOp16Body(Addr, Wrap, Func)               \
	{                                             \
		uint16_t val = GetWord(Addr(READ), Wrap); \
		ICPU.OpenBus = (uint8_t) (val >> 8);      \
		Func##16(val);                            \
	}

#define wmOp8Body(Addr, Wrap, Func, Mode) \
	{                                     \
		Func##8(Addr(Mode));              \
	}

#define wmOp16Body(Addr, Wrap, Func, Mode) \
	{                                      \
		Func##16(Addr(Mode), Wrap);        \
	}

#define wmOpC(Name, Cond, Addr, Wrap, Func, Mode)  \
	static void Op##Name()                         \
	{                                              \
		if (Check##Cond())                         \
			wmOp8Body(Addr, Wrap, Func, Mode)      \
		else                                       \
			wmOp16Body(Addr, Wrap, Func, Mode)     \
	}

#define rOp8(Name, Addr, Wrap, Func) \
	static void Op##Name() rOp8Body(Addr, Wrap, Func)

#define rOp16(Name, Addr, Wrap, Func) \
	static void Op##Name() rOp16Body(Addr, Wrap, Func)

#define rOpE1(Name, Addr, Wrap, Func) \
	NOT_SA1(rOp8(Name##E1, Addr, Wrap, Func))

#define rOpC(Name, Cond, Addr, Wrap, Func) \
	static void Op##Name()                 \
	{                                      \
		if (Check##Cond())                 \
			rOp8Body(Addr, Wrap, Func)     \
		else                               \
			rOp16Body(Addr, Wrap, Func)    \
	}

#define rOpM(Name, Addr, Wrap, Func) \
	rOpC(Name, Mem, Addr, Wrap, Func)

#define rOpX(Name, Addr, Wrap, Func) \
	rOpC(Name, Index, Addr, Wrap, Func)

#define wOp8(Name, Addr, Wrap, Func) \
	static void Op##Name() wmOp8Body(Addr, Wrap, Func, WRITE)

#define wOp16(Name, Addr, Wrap, Func) \
	static void Op##Name() wmOp16Body(Addr, Wrap, Func, WRITE)

#define wOpE1(Name, Addr, Wrap, Func) \
	NOT_SA1(wOp8(Name##E1, Addr, Wrap, Func))

#define wOpC(Name, Cond, Addr, Wrap, Func) \
	wmOpC(Name, Cond, Addr, Wrap, Func, WRITE)

#define wOpM(Name, Addr, Wrap, Func) \
	wOpC(Name, Mem, Addr, Wrap, Func)

#define wOpX(Name, Addr, Wrap, Func) \
	wOpC(Name, Index, Addr, Wrap, Func)

#define mOp8(Name, Addr, Wrap, Func) \
	static void Op##Name() wmOp8Body(Addr, Wrap, Func, MODIFY)

#define mOp16(Name, Addr, Wrap, Func) \
	static void Op##Name() wmOp16Body(Addr, Wrap, Func, MODIFY)

#define mOpE1(Name, Addr, Wrap, Func) \
	NOT_SA1(mOp8(Name##E1, Addr, Wrap, Func))

#define mOpC(Name, Cond, Addr, Wrap, Func) \
	wmOpC(Name, Cond, Addr, Wrap, Func, MODIFY)

#define mOpM(Name, Addr, Wrap, Func) \
	mOpC(Name, Mem, Addr, Wrap, Func)

#define BranchCheck(OpAddress)                   \
	NOT_SA1(                                     \
		if (CPU.BranchSkip)                      \
		{                                        \
			CPU.BranchSkip = false;              \
				                                 \
			if (ICPU.Registers.PCw > OpAddress)  \
				return;                          \
		}                                        \
	)

#define NoCheck(OpAddress) /* Nothing */

#define bOpBody(WVal, Check, Cond, E)                                            \
	{                                                                            \
		pair newPC;                                                              \
		newPC.W = WVal;                                                          \
		Check(newPC.W)                                                           \
		                                                                         \
		if (Cond)                                                                \
		{                                                                        \
			AddCycles(Settings.OneCycle);                                        \
			                                                                     \
			if (E && ICPU.Registers.PCh != newPC.B.h)                            \
				AddCycles(Settings.OneCycle);                                    \
			                                                                     \
			if ((ICPU.Registers.PCw & ~MEMMAP_MASK) != (newPC.W & ~MEMMAP_MASK)) \
				SetPCBase(ICPU.ShiftedPB | newPC.W);                             \
			else                                                                 \
				ICPU.Registers.PCw = newPC.W;                                    \
			                                                                     \
			CPUShutdown();                                                       \
		}                                                                        \
	}

#define bOps(Name, Check, Cond) \
	NOT_SA1(static void Op##Name##E1()   bOpBody(Relative    (JUMP), Check, Cond, true)) \
	        static void Op##Name##E0()   bOpBody(Relative    (JUMP), Check, Cond, false) \
	        static void Op##Name##Slow() bOpBody(RelativeSlow(JUMP), Check, Cond, CheckEmulation())

static INLINE void ForceShutdown() /* From the speed-hacks branch of CatSFC */
{
#ifdef SA1_OPCODES
	SA1.Executing = false;
#else
	CPU.WaitPC = 0;
	CPU.Cycles = CPU.NextEvent;
#endif
}

static INLINE void CPUShutdown()
{
	if (!Settings.Shutdown || ICPU.Registers.PCw != CPU.WaitPC)
		return;

#ifdef SA1_OPCODES
	if (SA1.WaitCounter >= 1)
		ForceShutdown();
	else
		SA1.WaitCounter++;
#else
	/* Don't skip cycles with a pending NMI or IRQ - could cause delayed
	 * interrupt. Interrupts are delayed for a few cycles already, but
	 * the delay could allow the shutdown code to cycle skip again.
	 * Was causing screen flashing on Top Gear 3000. */
	if (CPU.WaitCounter == 0 && !(CPU.Flags & (IRQ_FLAG | NMI_FLAG)))
		ForceShutdown();
	else if (CPU.WaitCounter >= 2)
		CPU.WaitCounter = 1;
	else
		CPU.WaitCounter--;
#endif
}

/* ADC */
static void Op69M1()
{
	ADC8(Immediate8(READ));
}

static void Op69M0()
{
	ADC16(Immediate16(READ));
}

static void Op69Slow()
{
	if (CheckMem())
		ADC8(Immediate8Slow(READ));
	else
		ADC16(Immediate16Slow(READ));
}

rOp8 (65M1,   Direct,                           WRAP_BANK, ADC)
rOp16(65M0,   Direct,                           WRAP_BANK, ADC)
rOpM (65Slow, DirectSlow,                       WRAP_BANK, ADC)

rOpE1(75,     DirectIndexedXE1,                 WRAP_BANK, ADC)
rOp8 (75E0M1, DirectIndexedXE0,                 WRAP_BANK, ADC)
rOp16(75E0M0, DirectIndexedXE0,                 WRAP_BANK, ADC)
rOpM (75Slow, DirectIndexedXSlow,               WRAP_BANK, ADC)

rOpE1(72,     DirectIndirectE1,                 WRAP_NONE, ADC)
rOp8 (72E0M1, DirectIndirectE0,                 WRAP_NONE, ADC)
rOp16(72E0M0, DirectIndirectE0,                 WRAP_NONE, ADC)
rOpM (72Slow, DirectIndirectSlow,               WRAP_NONE, ADC)

rOpE1(61,     DirectIndexedIndirectE1,          WRAP_NONE, ADC)
rOp8 (61E0M1, DirectIndexedIndirectE0,          WRAP_NONE, ADC)
rOp16(61E0M0, DirectIndexedIndirectE0,          WRAP_NONE, ADC)
rOpM (61Slow, DirectIndexedIndirectSlow,        WRAP_NONE, ADC)

rOpE1(71,     DirectIndirectIndexedE1,          WRAP_NONE, ADC)
rOp8 (71E0M1, DirectIndirectIndexedE0,          WRAP_NONE, ADC)
rOp16(71E0M0, DirectIndirectIndexedE0,          WRAP_NONE, ADC)
rOpM (71Slow, DirectIndirectIndexedSlow,        WRAP_NONE, ADC)

rOp8 (67M1,   DirectIndirectLong,               WRAP_NONE, ADC)
rOp16(67M0,   DirectIndirectLong,               WRAP_NONE, ADC)
rOpM (67Slow, DirectIndirectLongSlow,           WRAP_NONE, ADC)

rOp8 (77M1,   DirectIndirectIndexedLong,        WRAP_NONE, ADC)
rOp16(77M0,   DirectIndirectIndexedLong,        WRAP_NONE, ADC)
rOpM (77Slow, DirectIndirectIndexedLongSlow,    WRAP_NONE, ADC)

rOp8 (6DM1,   Absolute,                         WRAP_NONE, ADC)
rOp16(6DM0,   Absolute,                         WRAP_NONE, ADC)
rOpM (6DSlow, AbsoluteSlow,                     WRAP_NONE, ADC)

rOp8 (7DM1X1, AbsoluteIndexedXX1,               WRAP_NONE, ADC)
rOp16(7DM0X1, AbsoluteIndexedXX1,               WRAP_NONE, ADC)
rOp8 (7DM1X0, AbsoluteIndexedXX0,               WRAP_NONE, ADC)
rOp16(7DM0X0, AbsoluteIndexedXX0,               WRAP_NONE, ADC)
rOpM (7DSlow, AbsoluteIndexedXSlow,             WRAP_NONE, ADC)

rOp8 (79M1X1, AbsoluteIndexedYX1,               WRAP_NONE, ADC)
rOp16(79M0X1, AbsoluteIndexedYX1,               WRAP_NONE, ADC)
rOp8 (79M1X0, AbsoluteIndexedYX0,               WRAP_NONE, ADC)
rOp16(79M0X0, AbsoluteIndexedYX0,               WRAP_NONE, ADC)
rOpM (79Slow, AbsoluteIndexedYSlow,             WRAP_NONE, ADC)

rOp8 (6FM1,   AbsoluteLong,                     WRAP_NONE, ADC)
rOp16(6FM0,   AbsoluteLong,                     WRAP_NONE, ADC)
rOpM (6FSlow, AbsoluteLongSlow,                 WRAP_NONE, ADC)

rOp8 (7FM1,   AbsoluteLongIndexedX,             WRAP_NONE, ADC)
rOp16(7FM0,   AbsoluteLongIndexedX,             WRAP_NONE, ADC)
rOpM (7FSlow, AbsoluteLongIndexedXSlow,         WRAP_NONE, ADC)

rOp8 (63M1,   StackRelative,                    WRAP_NONE, ADC)
rOp16(63M0,   StackRelative,                    WRAP_NONE, ADC)
rOpM (63Slow, StackRelativeSlow,                WRAP_NONE, ADC)

rOp8 (73M1,   StackRelativeIndirectIndexed,     WRAP_NONE, ADC)
rOp16(73M0,   StackRelativeIndirectIndexed,     WRAP_NONE, ADC)
rOpM (73Slow, StackRelativeIndirectIndexedSlow, WRAP_NONE, ADC)

/* AND */
static void Op29M1()
{
	ICPU.Registers.AL &= Immediate8(READ);
	SetZN8(ICPU.Registers.AL);
}

static void Op29M0()
{
	ICPU.Registers.A.W &= Immediate16(READ);
	SetZN16(ICPU.Registers.A.W);
}

static void Op29Slow()
{
	if (CheckMem())
	{
		ICPU.Registers.AL &= Immediate8Slow(READ);
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Registers.A.W &= Immediate16Slow(READ);
		SetZN16(ICPU.Registers.A.W);
	}
}

rOp8 (25M1,   Direct,                           WRAP_BANK, AND)
rOp16(25M0,   Direct,                           WRAP_BANK, AND)
rOpM (25Slow, DirectSlow,                       WRAP_BANK, AND)

rOpE1(35,     DirectIndexedXE1,                 WRAP_BANK, AND)
rOp8 (35E0M1, DirectIndexedXE0,                 WRAP_BANK, AND)
rOp16(35E0M0, DirectIndexedXE0,                 WRAP_BANK, AND)
rOpM (35Slow, DirectIndexedXSlow,               WRAP_BANK, AND)

rOpE1(32,     DirectIndirectE1,                 WRAP_NONE, AND)
rOp8 (32E0M1, DirectIndirectE0,                 WRAP_NONE, AND)
rOp16(32E0M0, DirectIndirectE0,                 WRAP_NONE, AND)
rOpM (32Slow, DirectIndirectSlow,               WRAP_NONE, AND)

rOpE1(21,     DirectIndexedIndirectE1,          WRAP_NONE, AND)
rOp8 (21E0M1, DirectIndexedIndirectE0,          WRAP_NONE, AND)
rOp16(21E0M0, DirectIndexedIndirectE0,          WRAP_NONE, AND)
rOpM (21Slow, DirectIndexedIndirectSlow,        WRAP_NONE, AND)

rOpE1(31,     DirectIndirectIndexedE1,          WRAP_NONE, AND)
rOp8 (31E0M1, DirectIndirectIndexedE0,          WRAP_NONE, AND)
rOp16(31E0M0, DirectIndirectIndexedE0,          WRAP_NONE, AND)
rOpM (31Slow, DirectIndirectIndexedSlow,        WRAP_NONE, AND)

rOp8 (27M1,   DirectIndirectLong,               WRAP_NONE, AND)
rOp16(27M0,   DirectIndirectLong,               WRAP_NONE, AND)
rOpM (27Slow, DirectIndirectLongSlow,           WRAP_NONE, AND)

rOp8 (37M1,   DirectIndirectIndexedLong,        WRAP_NONE, AND)
rOp16(37M0,   DirectIndirectIndexedLong,        WRAP_NONE, AND)
rOpM (37Slow, DirectIndirectIndexedLongSlow,    WRAP_NONE, AND)

rOp8 (2DM1,   Absolute,                         WRAP_NONE, AND)
rOp16(2DM0,   Absolute,                         WRAP_NONE, AND)
rOpM (2DSlow, AbsoluteSlow,                     WRAP_NONE, AND)

rOp8 (3DM1X1, AbsoluteIndexedXX1,               WRAP_NONE, AND)
rOp16(3DM0X1, AbsoluteIndexedXX1,               WRAP_NONE, AND)
rOp8 (3DM1X0, AbsoluteIndexedXX0,               WRAP_NONE, AND)
rOp16(3DM0X0, AbsoluteIndexedXX0,               WRAP_NONE, AND)
rOpM (3DSlow, AbsoluteIndexedXSlow,             WRAP_NONE, AND)

rOp8 (39M1X1, AbsoluteIndexedYX1,               WRAP_NONE, AND)
rOp16(39M0X1, AbsoluteIndexedYX1,               WRAP_NONE, AND)
rOp8 (39M1X0, AbsoluteIndexedYX0,               WRAP_NONE, AND)
rOp16(39M0X0, AbsoluteIndexedYX0,               WRAP_NONE, AND)
rOpM (39Slow, AbsoluteIndexedYSlow,             WRAP_NONE, AND)

rOp8 (2FM1,   AbsoluteLong,                     WRAP_NONE, AND)
rOp16(2FM0,   AbsoluteLong,                     WRAP_NONE, AND)
rOpM (2FSlow, AbsoluteLongSlow,                 WRAP_NONE, AND)

rOp8 (3FM1,   AbsoluteLongIndexedX,             WRAP_NONE, AND)
rOp16(3FM0,   AbsoluteLongIndexedX,             WRAP_NONE, AND)
rOpM (3FSlow, AbsoluteLongIndexedXSlow,         WRAP_NONE, AND)

rOp8 (23M1,   StackRelative,                    WRAP_NONE, AND)
rOp16(23M0,   StackRelative,                    WRAP_NONE, AND)
rOpM (23Slow, StackRelativeSlow,                WRAP_NONE, AND)

rOp8 (33M1,   StackRelativeIndirectIndexed,     WRAP_NONE, AND)
rOp16(33M0,   StackRelativeIndirectIndexed,     WRAP_NONE, AND)
rOpM (33Slow, StackRelativeIndirectIndexedSlow, WRAP_NONE, AND)

/* ASL */
static void Op0AM1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Carry = (bool) (ICPU.Registers.AL & 0x80);
	ICPU.Registers.AL <<= 1;
	SetZN8(ICPU.Registers.AL);
}

static void Op0AM0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Carry = (bool) (ICPU.Registers.AH & 0x80);
	ICPU.Registers.A.W <<= 1;
	SetZN16(ICPU.Registers.A.W);
}

static void Op0ASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckMem())
	{
		ICPU.Carry = (bool) (ICPU.Registers.AL & 0x80);
		ICPU.Registers.AL <<= 1;
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Carry = (bool) (ICPU.Registers.AH & 0x80);
		ICPU.Registers.A.W <<= 1;
		SetZN16(ICPU.Registers.A.W);
	}
}

mOp8 (06M1,   Direct,                           WRAP_BANK, ASL)
mOp16(06M0,   Direct,                           WRAP_BANK, ASL)
mOpM (06Slow, DirectSlow,                       WRAP_BANK, ASL)

mOpE1(16,     DirectIndexedXE1,                 WRAP_BANK, ASL)
mOp8 (16E0M1, DirectIndexedXE0,                 WRAP_BANK, ASL)
mOp16(16E0M0, DirectIndexedXE0,                 WRAP_BANK, ASL)
mOpM (16Slow, DirectIndexedXSlow,               WRAP_BANK, ASL)

mOp8 (0EM1,   Absolute,                         WRAP_NONE, ASL)
mOp16(0EM0,   Absolute,                         WRAP_NONE, ASL)
mOpM (0ESlow, AbsoluteSlow,                     WRAP_NONE, ASL)

mOp8 (1EM1X1, AbsoluteIndexedXX1,               WRAP_NONE, ASL)
mOp16(1EM0X1, AbsoluteIndexedXX1,               WRAP_NONE, ASL)
mOp8 (1EM1X0, AbsoluteIndexedXX0,               WRAP_NONE, ASL)
mOp16(1EM0X0, AbsoluteIndexedXX0,               WRAP_NONE, ASL)
mOpM (1ESlow, AbsoluteIndexedXSlow,             WRAP_NONE, ASL)

/* BIT */
static void Op89M1()
{
	ICPU.Zero = (bool) (ICPU.Registers.AL & Immediate8(READ));
}

static void Op89M0()
{
	ICPU.Zero = (bool) (ICPU.Registers.A.W & Immediate16(READ));
}

static void Op89Slow()
{
	if (CheckMem())
		ICPU.Zero = (bool) (ICPU.Registers.AL & Immediate8Slow(READ));
	else
		ICPU.Zero = (bool) (ICPU.Registers.A.W & Immediate16Slow(READ));
}

rOp8 (24M1,   Direct,                           WRAP_BANK, BIT)
rOp16(24M0,   Direct,                           WRAP_BANK, BIT)
rOpM (24Slow, DirectSlow,                       WRAP_BANK, BIT)

rOpE1(34,     DirectIndexedXE1,                 WRAP_BANK, BIT)
rOp8 (34E0M1, DirectIndexedXE0,                 WRAP_BANK, BIT)
rOp16(34E0M0, DirectIndexedXE0,                 WRAP_BANK, BIT)
rOpM (34Slow, DirectIndexedXSlow,               WRAP_BANK, BIT)

rOp8 (2CM1,   Absolute,                         WRAP_NONE, BIT)
rOp16(2CM0,   Absolute,                         WRAP_NONE, BIT)
rOpM (2CSlow, AbsoluteSlow,                     WRAP_NONE, BIT)

rOp8 (3CM1X1, AbsoluteIndexedXX1,               WRAP_NONE, BIT)
rOp16(3CM0X1, AbsoluteIndexedXX1,               WRAP_NONE, BIT)
rOp8 (3CM1X0, AbsoluteIndexedXX0,               WRAP_NONE, BIT)
rOp16(3CM0X0, AbsoluteIndexedXX0,               WRAP_NONE, BIT)
rOpM (3CSlow, AbsoluteIndexedXSlow,             WRAP_NONE, BIT)

/* CMP */
static void OpC9M1()
{
	int16_t Int16 = (int16_t) ICPU.Registers.AL - (int16_t) Immediate8(READ);
	ICPU.Carry = (Int16 >= 0);
	SetZN8((uint8_t) Int16);
}

static void OpC9M0()
{
	int32_t Int32 = (int32_t) ICPU.Registers.A.W - (int32_t) Immediate16(READ);
	ICPU.Carry = (Int32 >= 0);
	SetZN16((uint16_t) Int32);
}

static void OpC9Slow()
{
	if (CheckMem())
	{
		int16_t Int16 = (int16_t) ICPU.Registers.AL - (int16_t) Immediate8Slow(READ);
		ICPU.Carry = (Int16 >= 0);
		SetZN8((uint8_t) Int16);
	}
	else
	{
		int32_t Int32 = (int32_t) ICPU.Registers.A.W - (int32_t) Immediate16Slow(READ);
		ICPU.Carry = (Int32 >= 0);
		SetZN16((uint16_t) Int32);
	}
}

rOp8 (C5M1,   Direct,                           WRAP_BANK, CMP)
rOp16(C5M0,   Direct,                           WRAP_BANK, CMP)
rOpM (C5Slow, DirectSlow,                       WRAP_BANK, CMP)

rOpE1(D5,     DirectIndexedXE1,                 WRAP_BANK, CMP)
rOp8 (D5E0M1, DirectIndexedXE0,                 WRAP_BANK, CMP)
rOp16(D5E0M0, DirectIndexedXE0,                 WRAP_BANK, CMP)
rOpM (D5Slow, DirectIndexedXSlow,               WRAP_BANK, CMP)

rOpE1(D2,     DirectIndirectE1,                 WRAP_NONE, CMP)
rOp8 (D2E0M1, DirectIndirectE0,                 WRAP_NONE, CMP)
rOp16(D2E0M0, DirectIndirectE0,                 WRAP_NONE, CMP)
rOpM (D2Slow, DirectIndirectSlow,               WRAP_NONE, CMP)

rOpE1(C1,     DirectIndexedIndirectE1,          WRAP_NONE, CMP)
rOp8 (C1E0M1, DirectIndexedIndirectE0,          WRAP_NONE, CMP)
rOp16(C1E0M0, DirectIndexedIndirectE0,          WRAP_NONE, CMP)
rOpM (C1Slow, DirectIndexedIndirectSlow,        WRAP_NONE, CMP)

rOpE1(D1,     DirectIndirectIndexedE1,          WRAP_NONE, CMP)
rOp8 (D1E0M1, DirectIndirectIndexedE0,          WRAP_NONE, CMP)
rOp16(D1E0M0, DirectIndirectIndexedE0,          WRAP_NONE, CMP)
rOpM (D1Slow, DirectIndirectIndexedSlow,        WRAP_NONE, CMP)

rOp8 (C7M1,   DirectIndirectLong,               WRAP_NONE, CMP)
rOp16(C7M0,   DirectIndirectLong,               WRAP_NONE, CMP)
rOpM (C7Slow, DirectIndirectLongSlow,           WRAP_NONE, CMP)

rOp8 (D7M1,   DirectIndirectIndexedLong,        WRAP_NONE, CMP)
rOp16(D7M0,   DirectIndirectIndexedLong,        WRAP_NONE, CMP)
rOpM (D7Slow, DirectIndirectIndexedLongSlow,    WRAP_NONE, CMP)

rOp8 (CDM1,   Absolute,                         WRAP_NONE, CMP)
rOp16(CDM0,   Absolute,                         WRAP_NONE, CMP)
rOpM (CDSlow, AbsoluteSlow,                     WRAP_NONE, CMP)

rOp8 (DDM1X1, AbsoluteIndexedXX1,               WRAP_NONE, CMP)
rOp16(DDM0X1, AbsoluteIndexedXX1,               WRAP_NONE, CMP)
rOp8 (DDM1X0, AbsoluteIndexedXX0,               WRAP_NONE, CMP)
rOp16(DDM0X0, AbsoluteIndexedXX0,               WRAP_NONE, CMP)
rOpM (DDSlow, AbsoluteIndexedXSlow,             WRAP_NONE, CMP)

rOp8 (D9M1X1, AbsoluteIndexedYX1,               WRAP_NONE, CMP)
rOp16(D9M0X1, AbsoluteIndexedYX1,               WRAP_NONE, CMP)
rOp8 (D9M1X0, AbsoluteIndexedYX0,               WRAP_NONE, CMP)
rOp16(D9M0X0, AbsoluteIndexedYX0,               WRAP_NONE, CMP)
rOpM (D9Slow, AbsoluteIndexedYSlow,             WRAP_NONE, CMP)

rOp8 (CFM1,   AbsoluteLong,                     WRAP_NONE, CMP)
rOp16(CFM0,   AbsoluteLong,                     WRAP_NONE, CMP)
rOpM (CFSlow, AbsoluteLongSlow,                 WRAP_NONE, CMP)

rOp8 (DFM1,   AbsoluteLongIndexedX,             WRAP_NONE, CMP)
rOp16(DFM0,   AbsoluteLongIndexedX,             WRAP_NONE, CMP)
rOpM (DFSlow, AbsoluteLongIndexedXSlow,         WRAP_NONE, CMP)

rOp8 (C3M1,   StackRelative,                    WRAP_NONE, CMP)
rOp16(C3M0,   StackRelative,                    WRAP_NONE, CMP)
rOpM (C3Slow, StackRelativeSlow,                WRAP_NONE, CMP)

rOp8 (D3M1,   StackRelativeIndirectIndexed,     WRAP_NONE, CMP)
rOp16(D3M0,   StackRelativeIndirectIndexed,     WRAP_NONE, CMP)
rOpM (D3Slow, StackRelativeIndirectIndexedSlow, WRAP_NONE, CMP)

/* CPX */
static void OpE0X1()
{
	int16_t Int16 = (int16_t) ICPU.Registers.XL - (int16_t) Immediate8(READ);
	ICPU.Carry = (Int16 >= 0);
	SetZN8((uint8_t) Int16);
}

static void OpE0X0()
{
	int32_t Int32 = (int32_t) ICPU.Registers.X.W - (int32_t) Immediate16(READ);
	ICPU.Carry = (Int32 >= 0);
	SetZN16((uint16_t) Int32);
}

static void OpE0Slow()
{
	if (CheckIndex())
	{
		int16_t Int16 = (int16_t) ICPU.Registers.XL - (int16_t) Immediate8Slow(READ);
		ICPU.Carry = (Int16 >= 0);
		SetZN8((uint8_t) Int16);
	}
	else
	{
		int32_t Int32 = (int32_t) ICPU.Registers.X.W - (int32_t) Immediate16Slow(READ);
		ICPU.Carry = (Int32 >= 0);
		SetZN16((uint16_t) Int32);
	}
}

rOp8 (E4X1,   Direct,                           WRAP_BANK, CPX)
rOp16(E4X0,   Direct,                           WRAP_BANK, CPX)
rOpX (E4Slow, DirectSlow,                       WRAP_BANK, CPX)

rOp8 (ECX1,   Absolute,                         WRAP_NONE, CPX)
rOp16(ECX0,   Absolute,                         WRAP_NONE, CPX)
rOpX (ECSlow, AbsoluteSlow,                     WRAP_NONE, CPX)

/* CPY */
static void OpC0X1()
{
	int16_t Int16 = (int16_t) ICPU.Registers.YL - (int16_t) Immediate8(READ);
	ICPU.Carry = (Int16 >= 0);
	SetZN8((uint8_t) Int16);
}

static void OpC0X0()
{
	int32_t Int32 = (int32_t) ICPU.Registers.Y.W - (int32_t) Immediate16(READ);
	ICPU.Carry = (Int32 >= 0);
	SetZN16((uint16_t) Int32);
}

static void OpC0Slow()
{
	if (CheckIndex())
	{
		int16_t Int16 = (int16_t) ICPU.Registers.YL - (int16_t) Immediate8Slow(READ);
		ICPU.Carry = (Int16 >= 0);
		SetZN8((uint8_t) Int16);
	}
	else
	{
		int32_t Int32 = (int32_t) ICPU.Registers.Y.W - (int32_t) Immediate16Slow(READ);
		ICPU.Carry = (Int32 >= 0);
		SetZN16((uint16_t) Int32);
	}
}

rOp8 (C4X1,   Direct,                           WRAP_BANK, CPY)
rOp16(C4X0,   Direct,                           WRAP_BANK, CPY)
rOpX (C4Slow, DirectSlow,                       WRAP_BANK, CPY)

rOp8 (CCX1,   Absolute,                         WRAP_NONE, CPY)
rOp16(CCX0,   Absolute,                         WRAP_NONE, CPY)
rOpX (CCSlow, AbsoluteSlow,                     WRAP_NONE, CPY)

/* DEC */
static void Op3AM1()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.AL--;
	SetZN8(ICPU.Registers.AL);
}

static void Op3AM0()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.A.W--;
	SetZN16(ICPU.Registers.A.W);
}

static void Op3ASlow()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;

	if (CheckMem())
	{
		ICPU.Registers.AL--;
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Registers.A.W--;
		SetZN16(ICPU.Registers.A.W);
	}
}

mOp8 (C6M1,   Direct,                           WRAP_BANK, DEC)
mOp16(C6M0,   Direct,                           WRAP_BANK, DEC)
mOpM (C6Slow, DirectSlow,                       WRAP_BANK, DEC)

mOpE1(D6,     DirectIndexedXE1,                 WRAP_BANK, DEC)
mOp8 (D6E0M1, DirectIndexedXE0,                 WRAP_BANK, DEC)
mOp16(D6E0M0, DirectIndexedXE0,                 WRAP_BANK, DEC)
mOpM (D6Slow, DirectIndexedXSlow,               WRAP_BANK, DEC)

mOp8 (CEM1,   Absolute,                         WRAP_NONE, DEC)
mOp16(CEM0,   Absolute,                         WRAP_NONE, DEC)
mOpM (CESlow, AbsoluteSlow,                     WRAP_NONE, DEC)

mOp8 (DEM1X1, AbsoluteIndexedXX1,               WRAP_NONE, DEC)
mOp16(DEM0X1, AbsoluteIndexedXX1,               WRAP_NONE, DEC)
mOp8 (DEM1X0, AbsoluteIndexedXX0,               WRAP_NONE, DEC)
mOp16(DEM0X0, AbsoluteIndexedXX0,               WRAP_NONE, DEC)
mOpM (DESlow, AbsoluteIndexedXSlow,             WRAP_NONE, DEC)

/* EOR */
static void Op49M1()
{
	ICPU.Registers.AL ^= Immediate8(READ);
	SetZN8(ICPU.Registers.AL);
}

static void Op49M0()
{
	ICPU.Registers.A.W ^= Immediate16(READ);
	SetZN16(ICPU.Registers.A.W);
}

static void Op49Slow()
{
	if (CheckMem())
	{
		ICPU.Registers.AL ^= Immediate8Slow(READ);
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Registers.A.W ^= Immediate16Slow(READ);
		SetZN16(ICPU.Registers.A.W);
	}
}

rOp8 (45M1,   Direct,                           WRAP_BANK, EOR)
rOp16(45M0,   Direct,                           WRAP_BANK, EOR)
rOpM (45Slow, DirectSlow,                       WRAP_BANK, EOR)

rOpE1(55,     DirectIndexedXE1,                 WRAP_BANK, EOR)
rOp8 (55E0M1, DirectIndexedXE0,                 WRAP_BANK, EOR)
rOp16(55E0M0, DirectIndexedXE0,                 WRAP_BANK, EOR)
rOpM (55Slow, DirectIndexedXSlow,               WRAP_BANK, EOR)

rOpE1(52,     DirectIndirectE1,                 WRAP_NONE, EOR)
rOp8 (52E0M1, DirectIndirectE0,                 WRAP_NONE, EOR)
rOp16(52E0M0, DirectIndirectE0,                 WRAP_NONE, EOR)
rOpM (52Slow, DirectIndirectSlow,               WRAP_NONE, EOR)

rOpE1(41,     DirectIndexedIndirectE1,          WRAP_NONE, EOR)
rOp8 (41E0M1, DirectIndexedIndirectE0,          WRAP_NONE, EOR)
rOp16(41E0M0, DirectIndexedIndirectE0,          WRAP_NONE, EOR)
rOpM (41Slow, DirectIndexedIndirectSlow,        WRAP_NONE, EOR)

rOpE1(51,     DirectIndirectIndexedE1,          WRAP_NONE, EOR)
rOp8 (51E0M1, DirectIndirectIndexedE0,          WRAP_NONE, EOR)
rOp16(51E0M0, DirectIndirectIndexedE0,          WRAP_NONE, EOR)
rOpM (51Slow, DirectIndirectIndexedSlow,        WRAP_NONE, EOR)

rOp8 (47M1,   DirectIndirectLong,               WRAP_NONE, EOR)
rOp16(47M0,   DirectIndirectLong,               WRAP_NONE, EOR)
rOpM (47Slow, DirectIndirectLongSlow,           WRAP_NONE, EOR)

rOp8 (57M1,   DirectIndirectIndexedLong,        WRAP_NONE, EOR)
rOp16(57M0,   DirectIndirectIndexedLong,        WRAP_NONE, EOR)
rOpM (57Slow, DirectIndirectIndexedLongSlow,    WRAP_NONE, EOR)

rOp8 (4DM1,   Absolute,                         WRAP_NONE, EOR)
rOp16(4DM0,   Absolute,                         WRAP_NONE, EOR)
rOpM (4DSlow, AbsoluteSlow,                     WRAP_NONE, EOR)

rOp8 (5DM1X1, AbsoluteIndexedXX1,               WRAP_NONE, EOR)
rOp16(5DM0X1, AbsoluteIndexedXX1,               WRAP_NONE, EOR)
rOp8 (5DM1X0, AbsoluteIndexedXX0,               WRAP_NONE, EOR)
rOp16(5DM0X0, AbsoluteIndexedXX0,               WRAP_NONE, EOR)
rOpM (5DSlow, AbsoluteIndexedXSlow,             WRAP_NONE, EOR)

rOp8 (59M1X1, AbsoluteIndexedYX1,               WRAP_NONE, EOR)
rOp16(59M0X1, AbsoluteIndexedYX1,               WRAP_NONE, EOR)
rOp8 (59M1X0, AbsoluteIndexedYX0,               WRAP_NONE, EOR)
rOp16(59M0X0, AbsoluteIndexedYX0,               WRAP_NONE, EOR)
rOpM (59Slow, AbsoluteIndexedYSlow,             WRAP_NONE, EOR)

rOp8 (4FM1,   AbsoluteLong,                     WRAP_NONE, EOR)
rOp16(4FM0,   AbsoluteLong,                     WRAP_NONE, EOR)
rOpM (4FSlow, AbsoluteLongSlow,                 WRAP_NONE, EOR)

rOp8 (5FM1,   AbsoluteLongIndexedX,             WRAP_NONE, EOR)
rOp16(5FM0,   AbsoluteLongIndexedX,             WRAP_NONE, EOR)
rOpM (5FSlow, AbsoluteLongIndexedXSlow,         WRAP_NONE, EOR)

rOp8 (43M1,   StackRelative,                    WRAP_NONE, EOR)
rOp16(43M0,   StackRelative,                    WRAP_NONE, EOR)
rOpM (43Slow, StackRelativeSlow,                WRAP_NONE, EOR)

rOp8 (53M1,   StackRelativeIndirectIndexed,     WRAP_NONE, EOR)
rOp16(53M0,   StackRelativeIndirectIndexed,     WRAP_NONE, EOR)
rOpM (53Slow, StackRelativeIndirectIndexedSlow, WRAP_NONE, EOR)

/* INC */
static void Op1AM1()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.AL++;
	SetZN8(ICPU.Registers.AL);
}

static void Op1AM0()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.A.W++;
	SetZN16(ICPU.Registers.A.W);
}

static void Op1ASlow()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;

	if (CheckMem())
	{
		ICPU.Registers.AL++;
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Registers.A.W++;
		SetZN16(ICPU.Registers.A.W);
	}
}

mOp8 (E6M1,   Direct,                           WRAP_BANK, INC)
mOp16(E6M0,   Direct,                           WRAP_BANK, INC)
mOpM (E6Slow, DirectSlow,                       WRAP_BANK, INC)

mOpE1(F6,     DirectIndexedXE1,                 WRAP_BANK, INC)
mOp8 (F6E0M1, DirectIndexedXE0,                 WRAP_BANK, INC)
mOp16(F6E0M0, DirectIndexedXE0,                 WRAP_BANK, INC)
mOpM (F6Slow, DirectIndexedXSlow,               WRAP_BANK, INC)

mOp8 (EEM1,   Absolute,                         WRAP_NONE, INC)
mOp16(EEM0,   Absolute,                         WRAP_NONE, INC)
mOpM (EESlow, AbsoluteSlow,                     WRAP_NONE, INC)

mOp8 (FEM1X1, AbsoluteIndexedXX1,               WRAP_NONE, INC)
mOp16(FEM0X1, AbsoluteIndexedXX1,               WRAP_NONE, INC)
mOp8 (FEM1X0, AbsoluteIndexedXX0,               WRAP_NONE, INC)
mOp16(FEM0X0, AbsoluteIndexedXX0,               WRAP_NONE, INC)
mOpM (FESlow, AbsoluteIndexedXSlow,             WRAP_NONE, INC)

/* LDA */
static void OpA9M1()
{
	ICPU.Registers.AL = Immediate8(READ);
	SetZN8(ICPU.Registers.AL);
}

static void OpA9M0()
{
	ICPU.Registers.A.W = Immediate16(READ);
	SetZN16(ICPU.Registers.A.W);
}

static void OpA9Slow()
{
	if (CheckMem())
	{
		ICPU.Registers.AL = Immediate8Slow(READ);
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Registers.A.W = Immediate16Slow(READ);
		SetZN16(ICPU.Registers.A.W);
	}
}

rOp8 (A5M1,   Direct,                           WRAP_BANK, LDA)
rOp16(A5M0,   Direct,                           WRAP_BANK, LDA)
rOpM (A5Slow, DirectSlow,                       WRAP_BANK, LDA)

rOpE1(B5,     DirectIndexedXE1,                 WRAP_BANK, LDA)
rOp8 (B5E0M1, DirectIndexedXE0,                 WRAP_BANK, LDA)
rOp16(B5E0M0, DirectIndexedXE0,                 WRAP_BANK, LDA)
rOpM (B5Slow, DirectIndexedXSlow,               WRAP_BANK, LDA)

rOpE1(B2,     DirectIndirectE1,                 WRAP_NONE, LDA)
rOp8 (B2E0M1, DirectIndirectE0,                 WRAP_NONE, LDA)
rOp16(B2E0M0, DirectIndirectE0,                 WRAP_NONE, LDA)
rOpM (B2Slow, DirectIndirectSlow,               WRAP_NONE, LDA)

rOpE1(A1,     DirectIndexedIndirectE1,          WRAP_NONE, LDA)
rOp8 (A1E0M1, DirectIndexedIndirectE0,          WRAP_NONE, LDA)
rOp16(A1E0M0, DirectIndexedIndirectE0,          WRAP_NONE, LDA)
rOpM (A1Slow, DirectIndexedIndirectSlow,        WRAP_NONE, LDA)

rOpE1(B1,     DirectIndirectIndexedE1,          WRAP_NONE, LDA)
rOp8 (B1E0M1, DirectIndirectIndexedE0,          WRAP_NONE, LDA)
rOp16(B1E0M0, DirectIndirectIndexedE0,          WRAP_NONE, LDA)
rOpM (B1Slow, DirectIndirectIndexedSlow,        WRAP_NONE, LDA)

rOp8 (A7M1,   DirectIndirectLong,               WRAP_NONE, LDA)
rOp16(A7M0,   DirectIndirectLong,               WRAP_NONE, LDA)
rOpM (A7Slow, DirectIndirectLongSlow,           WRAP_NONE, LDA)

rOp8 (B7M1,   DirectIndirectIndexedLong,        WRAP_NONE, LDA)
rOp16(B7M0,   DirectIndirectIndexedLong,        WRAP_NONE, LDA)
rOpM (B7Slow, DirectIndirectIndexedLongSlow,    WRAP_NONE, LDA)

rOp8 (ADM1,   Absolute,                         WRAP_NONE, LDA)
rOp16(ADM0,   Absolute,                         WRAP_NONE, LDA)
rOpM (ADSlow, AbsoluteSlow,                     WRAP_NONE, LDA)

rOp8 (BDM1X1, AbsoluteIndexedXX1,               WRAP_NONE, LDA)
rOp16(BDM0X1, AbsoluteIndexedXX1,               WRAP_NONE, LDA)
rOp8 (BDM1X0, AbsoluteIndexedXX0,               WRAP_NONE, LDA)
rOp16(BDM0X0, AbsoluteIndexedXX0,               WRAP_NONE, LDA)
rOpM (BDSlow, AbsoluteIndexedXSlow,             WRAP_NONE, LDA)

rOp8 (B9M1X1, AbsoluteIndexedYX1,               WRAP_NONE, LDA)
rOp16(B9M0X1, AbsoluteIndexedYX1,               WRAP_NONE, LDA)
rOp8 (B9M1X0, AbsoluteIndexedYX0,               WRAP_NONE, LDA)
rOp16(B9M0X0, AbsoluteIndexedYX0,               WRAP_NONE, LDA)
rOpM (B9Slow, AbsoluteIndexedYSlow,             WRAP_NONE, LDA)

rOp8 (AFM1,   AbsoluteLong,                     WRAP_NONE, LDA)
rOp16(AFM0,   AbsoluteLong,                     WRAP_NONE, LDA)
rOpM (AFSlow, AbsoluteLongSlow,                 WRAP_NONE, LDA)

rOp8 (BFM1,   AbsoluteLongIndexedX,             WRAP_NONE, LDA)
rOp16(BFM0,   AbsoluteLongIndexedX,             WRAP_NONE, LDA)
rOpM (BFSlow, AbsoluteLongIndexedXSlow,         WRAP_NONE, LDA)

rOp8 (A3M1,   StackRelative,                    WRAP_NONE, LDA)
rOp16(A3M0,   StackRelative,                    WRAP_NONE, LDA)
rOpM (A3Slow, StackRelativeSlow,                WRAP_NONE, LDA)

rOp8 (B3M1,   StackRelativeIndirectIndexed,     WRAP_NONE, LDA)
rOp16(B3M0,   StackRelativeIndirectIndexed,     WRAP_NONE, LDA)
rOpM (B3Slow, StackRelativeIndirectIndexedSlow, WRAP_NONE, LDA)

/* LDX */
static void OpA2X1()
{
	ICPU.Registers.XL = Immediate8(READ);
	SetZN8(ICPU.Registers.XL);
}

static void OpA2X0()
{
	ICPU.Registers.X.W = Immediate16(READ);
	SetZN16(ICPU.Registers.X.W);
}

static void OpA2Slow()
{
	if (CheckIndex())
	{
		ICPU.Registers.XL = Immediate8Slow(READ);
		SetZN8(ICPU.Registers.XL);
	}
	else
	{
		ICPU.Registers.X.W = Immediate16Slow(READ);
		SetZN16(ICPU.Registers.X.W);
	}
}

rOp8 (A6X1,   Direct,                           WRAP_BANK, LDX)
rOp16(A6X0,   Direct,                           WRAP_BANK, LDX)
rOpX (A6Slow, DirectSlow,                       WRAP_BANK, LDX)

rOpE1(B6,     DirectIndexedYE1,                 WRAP_BANK, LDX)
rOp8 (B6E0X1, DirectIndexedYE0,                 WRAP_BANK, LDX)
rOp16(B6E0X0, DirectIndexedYE0,                 WRAP_BANK, LDX)
rOpX (B6Slow, DirectIndexedYSlow,               WRAP_BANK, LDX)

rOp8 (AEX1,   Absolute,                         WRAP_BANK, LDX)
rOp16(AEX0,   Absolute,                         WRAP_BANK, LDX)
rOpX (AESlow, AbsoluteSlow,                     WRAP_BANK, LDX)

rOp8 (BEX1,   AbsoluteIndexedYX1,               WRAP_BANK, LDX)
rOp16(BEX0,   AbsoluteIndexedYX0,               WRAP_BANK, LDX)
rOpX (BESlow, AbsoluteIndexedYSlow,             WRAP_BANK, LDX)

/* LDY */
static void OpA0X1()
{
	ICPU.Registers.YL = Immediate8(READ);
	SetZN8(ICPU.Registers.YL);
}

static void OpA0X0()
{
	ICPU.Registers.Y.W = Immediate16(READ);
	SetZN16(ICPU.Registers.Y.W);
}

static void OpA0Slow()
{
	if (CheckIndex())
	{
		ICPU.Registers.YL = Immediate8Slow(READ);
		SetZN8(ICPU.Registers.YL);
	}
	else
	{
		ICPU.Registers.Y.W = Immediate16Slow(READ);
		SetZN16(ICPU.Registers.Y.W);
	}
}

rOp8 (A4X1,   Direct,                           WRAP_BANK, LDY)
rOp16(A4X0,   Direct,                           WRAP_BANK, LDY)
rOpX (A4Slow, DirectSlow,                       WRAP_BANK, LDY)

rOpE1(B4,     DirectIndexedXE1,                 WRAP_BANK, LDY)
rOp8 (B4E0X1, DirectIndexedXE0,                 WRAP_BANK, LDY)
rOp16(B4E0X0, DirectIndexedXE0,                 WRAP_BANK, LDY)
rOpX (B4Slow, DirectIndexedXSlow,               WRAP_BANK, LDY)

rOp8 (ACX1,   Absolute,                         WRAP_BANK, LDY)
rOp16(ACX0,   Absolute,                         WRAP_BANK, LDY)
rOpX (ACSlow, AbsoluteSlow,                     WRAP_BANK, LDY)

rOp8 (BCX1,   AbsoluteIndexedXX1,               WRAP_BANK, LDY)
rOp16(BCX0,   AbsoluteIndexedXX0,               WRAP_BANK, LDY)
rOpX (BCSlow, AbsoluteIndexedXSlow,             WRAP_BANK, LDY)

/* LSR */
static void Op4AM1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Carry = ICPU.Registers.AL & 1;
	ICPU.Registers.AL >>= 1;
	SetZN8(ICPU.Registers.AL);
}

static void Op4AM0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Carry = ICPU.Registers.A.W & 1;
	ICPU.Registers.A.W >>= 1;
	SetZN16(ICPU.Registers.A.W);
}

static void Op4ASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckMem())
	{
		ICPU.Carry = ICPU.Registers.AL & 1;
		ICPU.Registers.AL >>= 1;
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Carry = ICPU.Registers.A.W & 1;
		ICPU.Registers.A.W >>= 1;
		SetZN16(ICPU.Registers.A.W);
	}
}

mOp8 (46M1,   Direct,                           WRAP_BANK, LSR)
mOp16(46M0,   Direct,                           WRAP_BANK, LSR)
mOpM (46Slow, DirectSlow,                       WRAP_BANK, LSR)

mOpE1(56,     DirectIndexedXE1,                 WRAP_BANK, LSR)
mOp8 (56E0M1, DirectIndexedXE0,                 WRAP_BANK, LSR)
mOp16(56E0M0, DirectIndexedXE0,                 WRAP_BANK, LSR)
mOpM (56Slow, DirectIndexedXSlow,               WRAP_BANK, LSR)

mOp8 (4EM1,   Absolute,                         WRAP_NONE, LSR)
mOp16(4EM0,   Absolute,                         WRAP_NONE, LSR)
mOpM (4ESlow, AbsoluteSlow,                     WRAP_NONE, LSR)

mOp8 (5EM1X1, AbsoluteIndexedXX1,               WRAP_NONE, LSR)
mOp16(5EM0X1, AbsoluteIndexedXX1,               WRAP_NONE, LSR)
mOp8 (5EM1X0, AbsoluteIndexedXX0,               WRAP_NONE, LSR)
mOp16(5EM0X0, AbsoluteIndexedXX0,               WRAP_NONE, LSR)
mOpM (5ESlow, AbsoluteIndexedXSlow,             WRAP_NONE, LSR)

/* ORA */
static void Op09M1()
{
	ICPU.Registers.AL |= Immediate8(READ);
	SetZN8(ICPU.Registers.AL);
}

static void Op09M0()
{
	ICPU.Registers.A.W |= Immediate16(READ);
	SetZN16(ICPU.Registers.A.W);
}

static void Op09Slow()
{
	if (CheckMem())
	{
		ICPU.Registers.AL |= Immediate8Slow(READ);
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Registers.A.W |= Immediate16Slow(READ);
		SetZN16(ICPU.Registers.A.W);
	}
}

rOp8 (05M1,   Direct,                           WRAP_BANK, ORA)
rOp16(05M0,   Direct,                           WRAP_BANK, ORA)
rOpM (05Slow, DirectSlow,                       WRAP_BANK, ORA)

rOpE1(15,     DirectIndexedXE1,                 WRAP_BANK, ORA)
rOp8 (15E0M1, DirectIndexedXE0,                 WRAP_BANK, ORA)
rOp16(15E0M0, DirectIndexedXE0,                 WRAP_BANK, ORA)
rOpM (15Slow, DirectIndexedXSlow,               WRAP_BANK, ORA)

rOpE1(12,     DirectIndirectE1,                 WRAP_NONE, ORA)
rOp8 (12E0M1, DirectIndirectE0,                 WRAP_NONE, ORA)
rOp16(12E0M0, DirectIndirectE0,                 WRAP_NONE, ORA)
rOpM (12Slow, DirectIndirectSlow,               WRAP_NONE, ORA)

rOpE1(01,     DirectIndexedIndirectE1,          WRAP_NONE, ORA)
rOp8 (01E0M1, DirectIndexedIndirectE0,          WRAP_NONE, ORA)
rOp16(01E0M0, DirectIndexedIndirectE0,          WRAP_NONE, ORA)
rOpM (01Slow, DirectIndexedIndirectSlow,        WRAP_NONE, ORA)

rOpE1(11,     DirectIndirectIndexedE1,          WRAP_NONE, ORA)
rOp8 (11E0M1, DirectIndirectIndexedE0,          WRAP_NONE, ORA)
rOp16(11E0M0, DirectIndirectIndexedE0,          WRAP_NONE, ORA)
rOpM (11Slow, DirectIndirectIndexedSlow,        WRAP_NONE, ORA)

rOp8 (07M1,   DirectIndirectLong,               WRAP_NONE, ORA)
rOp16(07M0,   DirectIndirectLong,               WRAP_NONE, ORA)
rOpM (07Slow, DirectIndirectLongSlow,           WRAP_NONE, ORA)

rOp8 (17M1,   DirectIndirectIndexedLong,        WRAP_NONE, ORA)
rOp16(17M0,   DirectIndirectIndexedLong,        WRAP_NONE, ORA)
rOpM (17Slow, DirectIndirectIndexedLongSlow,    WRAP_NONE, ORA)

rOp8 (0DM1,   Absolute,                         WRAP_NONE, ORA)
rOp16(0DM0,   Absolute,                         WRAP_NONE, ORA)
rOpM (0DSlow, AbsoluteSlow,                     WRAP_NONE, ORA)

rOp8 (1DM1X1, AbsoluteIndexedXX1,               WRAP_NONE, ORA)
rOp16(1DM0X1, AbsoluteIndexedXX1,               WRAP_NONE, ORA)
rOp8 (1DM1X0, AbsoluteIndexedXX0,               WRAP_NONE, ORA)
rOp16(1DM0X0, AbsoluteIndexedXX0,               WRAP_NONE, ORA)
rOpM (1DSlow, AbsoluteIndexedXSlow,             WRAP_NONE, ORA)

rOp8 (19M1X1, AbsoluteIndexedYX1,               WRAP_NONE, ORA)
rOp16(19M0X1, AbsoluteIndexedYX1,               WRAP_NONE, ORA)
rOp8 (19M1X0, AbsoluteIndexedYX0,               WRAP_NONE, ORA)
rOp16(19M0X0, AbsoluteIndexedYX0,               WRAP_NONE, ORA)
rOpM (19Slow, AbsoluteIndexedYSlow,             WRAP_NONE, ORA)

rOp8 (0FM1,   AbsoluteLong,                     WRAP_NONE, ORA)
rOp16(0FM0,   AbsoluteLong,                     WRAP_NONE, ORA)
rOpM (0FSlow, AbsoluteLongSlow,                 WRAP_NONE, ORA)

rOp8 (1FM1,   AbsoluteLongIndexedX,             WRAP_NONE, ORA)
rOp16(1FM0,   AbsoluteLongIndexedX,             WRAP_NONE, ORA)
rOpM (1FSlow, AbsoluteLongIndexedXSlow,         WRAP_NONE, ORA)

rOp8 (03M1,   StackRelative,                    WRAP_NONE, ORA)
rOp16(03M0,   StackRelative,                    WRAP_NONE, ORA)
rOpM (03Slow, StackRelativeSlow,                WRAP_NONE, ORA)

rOp8 (13M1,   StackRelativeIndirectIndexed,     WRAP_NONE, ORA)
rOp16(13M0,   StackRelativeIndirectIndexed,     WRAP_NONE, ORA)
rOpM (13Slow, StackRelativeIndirectIndexedSlow, WRAP_NONE, ORA)

/* ROL */
static void Op2AM1()
{
	uint16_t w;
	AddCycles(Settings.OneCycle);
	w = (((uint16_t) ICPU.Registers.AL) << 1) | CheckCarry();
	ICPU.Carry = (w > 0xff);
	ICPU.Registers.AL = (uint8_t) w;
	SetZN8(ICPU.Registers.AL);
}

static void Op2AM0()
{
	uint32_t w;
	AddCycles(Settings.OneCycle);
	w = (((uint32_t) ICPU.Registers.A.W) << 1) | CheckCarry();
	ICPU.Carry = (w > 0xffff);
	ICPU.Registers.A.W = (uint16_t) w;
	SetZN16(ICPU.Registers.A.W);
}

static void Op2ASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckMem())
	{
		uint16_t w = (((uint16_t) ICPU.Registers.AL) << 1) | CheckCarry();
		ICPU.Carry = (w > 0xff);
		ICPU.Registers.AL = (uint8_t) w;
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		uint32_t w = (((uint32_t) ICPU.Registers.A.W) << 1) | CheckCarry();
		ICPU.Carry = (w > 0xffff);
		ICPU.Registers.A.W = (uint16_t) w;
		SetZN16(ICPU.Registers.A.W);
	}
}

mOp8 (26M1,   Direct,                           WRAP_BANK, ROL)
mOp16(26M0,   Direct,                           WRAP_BANK, ROL)
mOpM (26Slow, DirectSlow,                       WRAP_BANK, ROL)

mOpE1(36,     DirectIndexedXE1,                 WRAP_BANK, ROL)
mOp8 (36E0M1, DirectIndexedXE0,                 WRAP_BANK, ROL)
mOp16(36E0M0, DirectIndexedXE0,                 WRAP_BANK, ROL)
mOpM (36Slow, DirectIndexedXSlow,               WRAP_BANK, ROL)

mOp8 (2EM1,   Absolute,                         WRAP_NONE, ROL)
mOp16(2EM0,   Absolute,                         WRAP_NONE, ROL)
mOpM (2ESlow, AbsoluteSlow,                     WRAP_NONE, ROL)

mOp8 (3EM1X1, AbsoluteIndexedXX1,               WRAP_NONE, ROL)
mOp16(3EM0X1, AbsoluteIndexedXX1,               WRAP_NONE, ROL)
mOp8 (3EM1X0, AbsoluteIndexedXX0,               WRAP_NONE, ROL)
mOp16(3EM0X0, AbsoluteIndexedXX0,               WRAP_NONE, ROL)
mOpM (3ESlow, AbsoluteIndexedXSlow,             WRAP_NONE, ROL)

/* ROR */
static void Op6AM1()
{
	uint16_t w;
	AddCycles(Settings.OneCycle);
	w = ((uint16_t) ICPU.Registers.AL) | (((uint16_t) CheckCarry()) << 8);
	ICPU.Carry = (bool) (w & 1);
	w >>= 1;
	ICPU.Registers.AL = (uint8_t) w;
	SetZN8(ICPU.Registers.AL);
}

static void Op6AM0()
{
	uint32_t w;
	AddCycles(Settings.OneCycle);
	w = ((uint32_t) ICPU.Registers.A.W) | (((uint32_t) CheckCarry()) << 16);
	ICPU.Carry = (bool) (w & 1);
	w >>= 1;
	ICPU.Registers.A.W = (uint16_t) w;
	SetZN16(ICPU.Registers.A.W);
}

static void Op6ASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckMem())
	{
		uint16_t w = ((uint16_t) ICPU.Registers.AL) | (((uint16_t) CheckCarry()) << 8);
		ICPU.Carry = (bool) (w & 1);
		w >>= 1;
		ICPU.Registers.AL = (uint8_t) w;
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		uint32_t w = ((uint32_t) ICPU.Registers.A.W) | (((uint32_t) CheckCarry()) << 16);
		ICPU.Carry = (bool) (w & 1);
		w >>= 1;
		ICPU.Registers.A.W = (uint16_t) w;
		SetZN16(ICPU.Registers.A.W);
	}
}

mOp8 (66M1,   Direct,                           WRAP_BANK, ROR)
mOp16(66M0,   Direct,                           WRAP_BANK, ROR)
mOpM (66Slow, DirectSlow,                       WRAP_BANK, ROR)

mOpE1(76,     DirectIndexedXE1,                 WRAP_BANK, ROR)
mOp8 (76E0M1, DirectIndexedXE0,                 WRAP_BANK, ROR)
mOp16(76E0M0, DirectIndexedXE0,                 WRAP_BANK, ROR)
mOpM (76Slow, DirectIndexedXSlow,               WRAP_BANK, ROR)

mOp8 (6EM1,   Absolute,                         WRAP_NONE, ROR)
mOp16(6EM0,   Absolute,                         WRAP_NONE, ROR)
mOpM (6ESlow, AbsoluteSlow,                     WRAP_NONE, ROR)

mOp8 (7EM1X1, AbsoluteIndexedXX1,               WRAP_NONE, ROR)
mOp16(7EM0X1, AbsoluteIndexedXX1,               WRAP_NONE, ROR)
mOp8 (7EM1X0, AbsoluteIndexedXX0,               WRAP_NONE, ROR)
mOp16(7EM0X0, AbsoluteIndexedXX0,               WRAP_NONE, ROR)
mOpM (7ESlow, AbsoluteIndexedXSlow,             WRAP_NONE, ROR)

/* SBC */
static void OpE9M1()
{
	SBC8(Immediate8(READ));
}

static void OpE9M0()
{
	SBC16(Immediate16(READ));
}

static void OpE9Slow()
{
	if (CheckMem())
		SBC8(Immediate8Slow(READ));
	else
		SBC16(Immediate16Slow(READ));
}

rOp8 (E5M1,   Direct,                           WRAP_BANK, SBC)
rOp16(E5M0,   Direct,                           WRAP_BANK, SBC)
rOpM (E5Slow, DirectSlow,                       WRAP_BANK, SBC)

rOpE1(F5,     DirectIndexedXE1,                 WRAP_BANK, SBC)
rOp8 (F5E0M1, DirectIndexedXE0,                 WRAP_BANK, SBC)
rOp16(F5E0M0, DirectIndexedXE0,                 WRAP_BANK, SBC)
rOpM (F5Slow, DirectIndexedXSlow,               WRAP_BANK, SBC)

rOpE1(F2,     DirectIndirectE1,                 WRAP_NONE, SBC)
rOp8 (F2E0M1, DirectIndirectE0,                 WRAP_NONE, SBC)
rOp16(F2E0M0, DirectIndirectE0,                 WRAP_NONE, SBC)
rOpM (F2Slow, DirectIndirectSlow,               WRAP_NONE, SBC)

rOpE1(E1,     DirectIndexedIndirectE1,          WRAP_NONE, SBC)
rOp8 (E1E0M1, DirectIndexedIndirectE0,          WRAP_NONE, SBC)
rOp16(E1E0M0, DirectIndexedIndirectE0,          WRAP_NONE, SBC)
rOpM (E1Slow, DirectIndexedIndirectSlow,        WRAP_NONE, SBC)

rOpE1(F1,     DirectIndirectIndexedE1,          WRAP_NONE, SBC)
rOp8 (F1E0M1, DirectIndirectIndexedE0,          WRAP_NONE, SBC)
rOp16(F1E0M0, DirectIndirectIndexedE0,          WRAP_NONE, SBC)
rOpM (F1Slow, DirectIndirectIndexedSlow,        WRAP_NONE, SBC)

rOp8 (E7M1,   DirectIndirectLong,               WRAP_NONE, SBC)
rOp16(E7M0,   DirectIndirectLong,               WRAP_NONE, SBC)
rOpM (E7Slow, DirectIndirectLongSlow,           WRAP_NONE, SBC)

rOp8 (F7M1,   DirectIndirectIndexedLong,        WRAP_NONE, SBC)
rOp16(F7M0,   DirectIndirectIndexedLong,        WRAP_NONE, SBC)
rOpM (F7Slow, DirectIndirectIndexedLongSlow,    WRAP_NONE, SBC)

rOp8 (EDM1,   Absolute,                         WRAP_NONE, SBC)
rOp16(EDM0,   Absolute,                         WRAP_NONE, SBC)
rOpM (EDSlow, AbsoluteSlow,                     WRAP_NONE, SBC)

rOp8 (FDM1X1, AbsoluteIndexedXX1,               WRAP_NONE, SBC)
rOp16(FDM0X1, AbsoluteIndexedXX1,               WRAP_NONE, SBC)
rOp8 (FDM1X0, AbsoluteIndexedXX0,               WRAP_NONE, SBC)
rOp16(FDM0X0, AbsoluteIndexedXX0,               WRAP_NONE, SBC)
rOpM (FDSlow, AbsoluteIndexedXSlow,             WRAP_NONE, SBC)

rOp8 (F9M1X1, AbsoluteIndexedYX1,               WRAP_NONE, SBC)
rOp16(F9M0X1, AbsoluteIndexedYX1,               WRAP_NONE, SBC)
rOp8 (F9M1X0, AbsoluteIndexedYX0,               WRAP_NONE, SBC)
rOp16(F9M0X0, AbsoluteIndexedYX0,               WRAP_NONE, SBC)
rOpM (F9Slow, AbsoluteIndexedYSlow,             WRAP_NONE, SBC)

rOp8 (EFM1,   AbsoluteLong,                     WRAP_NONE, SBC)
rOp16(EFM0,   AbsoluteLong,                     WRAP_NONE, SBC)
rOpM (EFSlow, AbsoluteLongSlow,                 WRAP_NONE, SBC)

rOp8 (FFM1,   AbsoluteLongIndexedX,             WRAP_NONE, SBC)
rOp16(FFM0,   AbsoluteLongIndexedX,             WRAP_NONE, SBC)
rOpM (FFSlow, AbsoluteLongIndexedXSlow,         WRAP_NONE, SBC)

rOp8 (E3M1,   StackRelative,                    WRAP_NONE, SBC)
rOp16(E3M0,   StackRelative,                    WRAP_NONE, SBC)
rOpM (E3Slow, StackRelativeSlow,                WRAP_NONE, SBC)

rOp8 (F3M1,   StackRelativeIndirectIndexed,     WRAP_NONE, SBC)
rOp16(F3M0,   StackRelativeIndirectIndexed,     WRAP_NONE, SBC)
rOpM (F3Slow, StackRelativeIndirectIndexedSlow, WRAP_NONE, SBC)

/* STA */
wOp8 (85M1,   Direct,                           WRAP_BANK, STA)
wOp16(85M0,   Direct,                           WRAP_BANK, STA)
wOpM (85Slow, DirectSlow,                       WRAP_BANK, STA)

wOpE1(95,     DirectIndexedXE1,                 WRAP_BANK, STA)
wOp8 (95E0M1, DirectIndexedXE0,                 WRAP_BANK, STA)
wOp16(95E0M0, DirectIndexedXE0,                 WRAP_BANK, STA)
wOpM (95Slow, DirectIndexedXSlow,               WRAP_BANK, STA)

wOpE1(92,     DirectIndirectE1,                 WRAP_NONE, STA)
wOp8 (92E0M1, DirectIndirectE0,                 WRAP_NONE, STA)
wOp16(92E0M0, DirectIndirectE0,                 WRAP_NONE, STA)
wOpM (92Slow, DirectIndirectSlow,               WRAP_NONE, STA)

wOpE1(81,     DirectIndexedIndirectE1,          WRAP_NONE, STA)
wOp8 (81E0M1, DirectIndexedIndirectE0,          WRAP_NONE, STA)
wOp16(81E0M0, DirectIndexedIndirectE0,          WRAP_NONE, STA)
wOpM (81Slow, DirectIndexedIndirectSlow,        WRAP_NONE, STA)

wOpE1(91,     DirectIndirectIndexedE1,          WRAP_NONE, STA)
wOp8 (91E0M1, DirectIndirectIndexedE0,          WRAP_NONE, STA)
wOp16(91E0M0, DirectIndirectIndexedE0,          WRAP_NONE, STA)
wOpM (91Slow, DirectIndirectIndexedSlow,        WRAP_NONE, STA)

wOp8 (87M1,   DirectIndirectLong,               WRAP_NONE, STA)
wOp16(87M0,   DirectIndirectLong,               WRAP_NONE, STA)
wOpM (87Slow, DirectIndirectLongSlow,           WRAP_NONE, STA)

wOp8 (97M1,   DirectIndirectIndexedLong,        WRAP_NONE, STA)
wOp16(97M0,   DirectIndirectIndexedLong,        WRAP_NONE, STA)
wOpM (97Slow, DirectIndirectIndexedLongSlow,    WRAP_NONE, STA)

wOp8 (8DM1,   Absolute,                         WRAP_NONE, STA)
wOp16(8DM0,   Absolute,                         WRAP_NONE, STA)
wOpM (8DSlow, AbsoluteSlow,                     WRAP_NONE, STA)

wOp8 (9DM1X1, AbsoluteIndexedXX1,               WRAP_NONE, STA)
wOp16(9DM0X1, AbsoluteIndexedXX1,               WRAP_NONE, STA)
wOp8 (9DM1X0, AbsoluteIndexedXX0,               WRAP_NONE, STA)
wOp16(9DM0X0, AbsoluteIndexedXX0,               WRAP_NONE, STA)
wOpM (9DSlow, AbsoluteIndexedXSlow,             WRAP_NONE, STA)

wOp8 (99M1X1, AbsoluteIndexedYX1,               WRAP_NONE, STA)
wOp16(99M0X1, AbsoluteIndexedYX1,               WRAP_NONE, STA)
wOp8 (99M1X0, AbsoluteIndexedYX0,               WRAP_NONE, STA)
wOp16(99M0X0, AbsoluteIndexedYX0,               WRAP_NONE, STA)
wOpM (99Slow, AbsoluteIndexedYSlow,             WRAP_NONE, STA)

wOp8 (8FM1,   AbsoluteLong,                     WRAP_NONE, STA)
wOp16(8FM0,   AbsoluteLong,                     WRAP_NONE, STA)
wOpM (8FSlow, AbsoluteLongSlow,                 WRAP_NONE, STA)

wOp8 (9FM1,   AbsoluteLongIndexedX,             WRAP_NONE, STA)
wOp16(9FM0,   AbsoluteLongIndexedX,             WRAP_NONE, STA)
wOpM (9FSlow, AbsoluteLongIndexedXSlow,         WRAP_NONE, STA)

wOp8 (83M1,   StackRelative,                    WRAP_NONE, STA)
wOp16(83M0,   StackRelative,                    WRAP_NONE, STA)
wOpM (83Slow, StackRelativeSlow,                WRAP_NONE, STA)

wOp8 (93M1,   StackRelativeIndirectIndexed,     WRAP_NONE, STA)
wOp16(93M0,   StackRelativeIndirectIndexed,     WRAP_NONE, STA)
wOpM (93Slow, StackRelativeIndirectIndexedSlow, WRAP_NONE, STA)

/* STX */
wOp8 (86X1,   Direct,                           WRAP_BANK, STX)
wOp16(86X0,   Direct,                           WRAP_BANK, STX)
wOpX (86Slow, DirectSlow,                       WRAP_BANK, STX)

wOpE1(96,     DirectIndexedYE1,                 WRAP_BANK, STX)
wOp8 (96E0X1, DirectIndexedYE0,                 WRAP_BANK, STX)
wOp16(96E0X0, DirectIndexedYE0,                 WRAP_BANK, STX)
wOpX (96Slow, DirectIndexedYSlow,               WRAP_BANK, STX)

wOp8 (8EX1,   Absolute,                         WRAP_BANK, STX)
wOp16(8EX0,   Absolute,                         WRAP_BANK, STX)
wOpX (8ESlow, AbsoluteSlow,                     WRAP_BANK, STX)

/* STY */
wOp8 (84X1,   Direct,                           WRAP_BANK, STY)
wOp16(84X0,   Direct,                           WRAP_BANK, STY)
wOpX (84Slow, DirectSlow,                       WRAP_BANK, STY)

wOpE1(94,     DirectIndexedXE1,                 WRAP_BANK, STY)
wOp8 (94E0X1, DirectIndexedXE0,                 WRAP_BANK, STY)
wOp16(94E0X0, DirectIndexedXE0,                 WRAP_BANK, STY)
wOpX (94Slow, DirectIndexedXSlow,               WRAP_BANK, STY)

wOp8 (8CX1,   Absolute,                         WRAP_BANK, STY)
wOp16(8CX0,   Absolute,                         WRAP_BANK, STY)
wOpX (8CSlow, AbsoluteSlow,                     WRAP_BANK, STY)

/* STZ */
wOp8 (64M1,   Direct,                           WRAP_BANK, STZ)
wOp16(64M0,   Direct,                           WRAP_BANK, STZ)
wOpM (64Slow, DirectSlow,                       WRAP_BANK, STZ)

wOpE1(74,     DirectIndexedXE1,                 WRAP_BANK, STZ)
wOp8 (74E0M1, DirectIndexedXE0,                 WRAP_BANK, STZ)
wOp16(74E0M0, DirectIndexedXE0,                 WRAP_BANK, STZ)
wOpM (74Slow, DirectIndexedXSlow,               WRAP_BANK, STZ)

wOp8 (9CM1,   Absolute,                         WRAP_NONE, STZ)
wOp16(9CM0,   Absolute,                         WRAP_NONE, STZ)
wOpM (9CSlow, AbsoluteSlow,                     WRAP_NONE, STZ)

wOp8 (9EM1X1, AbsoluteIndexedXX1,               WRAP_NONE, STZ)
wOp16(9EM0X1, AbsoluteIndexedXX1,               WRAP_NONE, STZ)
wOp8 (9EM1X0, AbsoluteIndexedXX0,               WRAP_NONE, STZ)
wOp16(9EM0X0, AbsoluteIndexedXX0,               WRAP_NONE, STZ)
wOpM (9ESlow, AbsoluteIndexedXSlow,             WRAP_NONE, STZ)

/* TRB */
mOp8 (14M1,   Direct,                           WRAP_BANK, TRB)
mOp16(14M0,   Direct,                           WRAP_BANK, TRB)
mOpM (14Slow, DirectSlow,                       WRAP_BANK, TRB)

mOp8 (1CM1,   Absolute,                         WRAP_BANK, TRB)
mOp16(1CM0,   Absolute,                         WRAP_BANK, TRB)
mOpM (1CSlow, AbsoluteSlow,                     WRAP_BANK, TRB)

/* TSB */
mOp8 (04M1,   Direct,                           WRAP_BANK, TSB)
mOp16(04M0,   Direct,                           WRAP_BANK, TSB)
mOpM (04Slow, DirectSlow,                       WRAP_BANK, TSB)

mOp8 (0CM1,   Absolute,                         WRAP_BANK, TSB)
mOp16(0CM0,   Absolute,                         WRAP_BANK, TSB)
mOpM (0CSlow, AbsoluteSlow,                     WRAP_BANK, TSB)

/* Branch Instructions */
bOps(90, BranchCheck, !CheckCarry())     /* BCC */
bOps(B0, BranchCheck,  CheckCarry())     /* BCS */
bOps(F0, BranchCheck,  CheckZero())      /* BEQ */
bOps(D0, BranchCheck, !CheckZero())      /* BNE */
bOps(30, BranchCheck,  CheckNegative())  /* BMI */
bOps(10, BranchCheck, !CheckNegative())  /* BPL */
bOps(50, BranchCheck, !CheckOverflow())  /* BVC */
bOps(70, BranchCheck,  CheckOverflow())  /* BVS */
bOps(80, BranchCheck,  true)             /* BRA */

/* BRL */
static void Op82()
{
	SetPCBase(ICPU.ShiftedPB + RelativeLong(JUMP));
	AddCycles(Settings.OneCycle);
}

static void Op82Slow()
{
	SetPCBase(ICPU.ShiftedPB + RelativeLongSlow(JUMP));
	AddCycles(Settings.OneCycle);
}

/* ClearFlag / SetFlag Instructions */
static void Op18() /* CLC */
{
	ClearCarry();
	AddCycles(Settings.OneCycle);
}

static void Op38() /* SEC */
{
	SetCarry();
	AddCycles(Settings.OneCycle);
}

static void OpD8() /* CLD */
{
	ClearDecimal();
	AddCycles(Settings.OneCycle);
}

static void OpF8() /* SED */
{
	SetDecimal();
	AddCycles(Settings.OneCycle);
}

static void Op58() /* CLI */
{
	AddCycles(Settings.OneCycle);
	ClearIRQ();
}

static void Op78() /* SEI */
{
	AddCycles(Settings.OneCycle);
	SetIRQ();
}

static void OpB8() /* CLV */
{
	ClearOverflow();
	AddCycles(Settings.OneCycle);
}

/* DEX/DEY */
static void OpCAX1()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.XL--;
	SetZN8(ICPU.Registers.XL);
}

static void OpCAX0()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.X.W--;
	SetZN16(ICPU.Registers.X.W);
}

static void OpCASlow()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;

	if (CheckIndex())
	{
		ICPU.Registers.XL--;
		SetZN8(ICPU.Registers.XL);
	}
	else
	{
		ICPU.Registers.X.W--;
		SetZN16(ICPU.Registers.X.W);
	}
}

static void Op88X1()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.YL--;
	SetZN8(ICPU.Registers.YL);
}

static void Op88X0()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.Y.W--;
	SetZN16(ICPU.Registers.Y.W);
}

static void Op88Slow()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;

	if (CheckIndex())
	{
		ICPU.Registers.YL--;
		SetZN8(ICPU.Registers.YL);
	}
	else
	{
		ICPU.Registers.Y.W--;
		SetZN16(ICPU.Registers.Y.W);
	}
}

/* INX/INY */
static void OpE8X1()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.XL++;
	SetZN8(ICPU.Registers.XL);
}

static void OpE8X0()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.X.W++;
	SetZN16(ICPU.Registers.X.W);
}

static void OpE8Slow()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;

	if (CheckIndex())
	{
		ICPU.Registers.XL++;
		SetZN8(ICPU.Registers.XL);
	}
	else
	{
		ICPU.Registers.X.W++;
		SetZN16(ICPU.Registers.X.W);
	}
}

static void OpC8X1()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.YL++;
	SetZN8(ICPU.Registers.YL);
}

static void OpC8X0()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;
	ICPU.Registers.Y.W++;
	SetZN16(ICPU.Registers.Y.W);
}

static void OpC8Slow()
{
	AddCycles(Settings.OneCycle);
	CPU.WaitPC = 0;

	if (CheckIndex())
	{
		ICPU.Registers.YL++;
		SetZN8(ICPU.Registers.YL);
	}
	else
	{
		ICPU.Registers.Y.W++;
		SetZN16(ICPU.Registers.Y.W);
	}
}

static void OpEA() /* NOP */
{
	AddCycles(Settings.OneCycle);
}

/* PUSH Instructions */
static INLINE void PushB(uint8_t b)
{
	SetByte(b, ICPU.Registers.S.W--);
}

static INLINE void PushBE(uint8_t b)
{
	SetByte(b, ICPU.Registers.S.W);
	ICPU.Registers.SL--;
}

static INLINE void PushW(uint16_t w)
{
	SetWord(w, ICPU.Registers.S.W - 1, WRAP_BANK, WRITE_10);
	ICPU.Registers.S.W -= 2;
}

static INLINE void PushWE(uint16_t w)
{
	ICPU.Registers.SL--;
	SetWord(w, ICPU.Registers.S.W, WRAP_PAGE, WRITE_10);
	ICPU.Registers.SL--;
}

/* PEA */
NOT_SA1(
static void OpF4E1()
{
	/* Note: PEA is a new instruction,
	 * and so doesn't respect the emu-mode stack bounds. */
	uint16_t val = (uint16_t) Absolute(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;
	ICPU.Registers.SH = 1;
}
)

static void OpF4E0()
{
	uint16_t val = (uint16_t) Absolute(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;
}

static void OpF4Slow()
{
	uint16_t val = (uint16_t) AbsoluteSlow(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;

	if (CheckEmulation())
		ICPU.Registers.SH = 1;
}

/* PEI */
NOT_SA1(
static void OpD4E1()
{
	/* Note: PEI is a new instruction,
	 * and so doesn't respect the emu-mode stack bounds. */
	uint16_t val = (uint16_t) DirectIndirectE1(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;
	ICPU.Registers.SH = 1;
}
)

static void OpD4E0()
{
	uint16_t val = (uint16_t) DirectIndirectE0(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;
}

static void OpD4Slow()
{
	uint16_t val = (uint16_t) DirectIndirectSlow(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;

	if (CheckEmulation())
		ICPU.Registers.SH = 1;
}

/* PER */
NOT_SA1(
static void Op62E1()
{
	/* Note: PER is a new instruction,
	 * and so doesn't respect the emu-mode stack bounds. */
	uint16_t val = (uint16_t) RelativeLong(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;
	ICPU.Registers.SH = 1;
}
)

static void Op62E0()
{
	uint16_t val = (uint16_t) RelativeLong(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;
}

static void Op62Slow()
{
	uint16_t val = (uint16_t) RelativeLongSlow(NONE);
	PushW(val);
	ICPU.OpenBus = val & 0xff;

	if (CheckEmulation())
		ICPU.Registers.SH = 1;
}

/* PHA */
NOT_SA1(
static void Op48E1()
{
	AddCycles(Settings.OneCycle);
	PushBE(ICPU.Registers.AL);
	ICPU.OpenBus = ICPU.Registers.AL;
}
)

static void Op48E0M1()
{
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.AL);
	ICPU.OpenBus = ICPU.Registers.AL;
}

static void Op48E0M0()
{
	AddCycles(Settings.OneCycle);
	PushW(ICPU.Registers.A.W);
	ICPU.OpenBus = ICPU.Registers.AL;
}

static void Op48Slow()
{
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		PushBE(ICPU.Registers.AL);
	else if (CheckMem())
		PushB(ICPU.Registers.AL);
	else
		PushW(ICPU.Registers.A.W);

	ICPU.OpenBus = ICPU.Registers.AL;
}

/* PHB */
NOT_SA1(
static void Op8BE1()
{
	AddCycles(Settings.OneCycle);
	PushBE(ICPU.Registers.DB);
	ICPU.OpenBus = ICPU.Registers.DB;
}
)

static void Op8BE0()
{
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.DB);
	ICPU.OpenBus = ICPU.Registers.DB;
}

static void Op8BSlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		PushBE(ICPU.Registers.DB);
	else
		PushB(ICPU.Registers.DB);

	ICPU.OpenBus = ICPU.Registers.DB;
}

/* PHD */
NOT_SA1(
static void Op0BE1()
{
	/* Note: PHD is a new instruction,
	 * and so doesn't respect the emu-mode stack bounds. */
	AddCycles(Settings.OneCycle);
	PushW(ICPU.Registers.D.W);
	ICPU.OpenBus = ICPU.Registers.DL;
	ICPU.Registers.SH = 1;
}
)

static void Op0BE0()
{
	AddCycles(Settings.OneCycle);
	PushW(ICPU.Registers.D.W);
	ICPU.OpenBus = ICPU.Registers.DL;
}

static void Op0BSlow()
{
	AddCycles(Settings.OneCycle);
	PushW(ICPU.Registers.D.W);
	ICPU.OpenBus = ICPU.Registers.DL;

	if (CheckEmulation())
		ICPU.Registers.SH = 1;
}

/* PHK */
NOT_SA1(
static void Op4BE1()
{
	AddCycles(Settings.OneCycle);
	PushBE(ICPU.Registers.PB);
	ICPU.OpenBus = ICPU.Registers.PB;
}
)

static void Op4BE0()
{
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.PB);
	ICPU.OpenBus = ICPU.Registers.PB;
}

static void Op4BSlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		PushBE(ICPU.Registers.PB);
	else
		PushB(ICPU.Registers.PB);

	ICPU.OpenBus = ICPU.Registers.PB;
}

/* PHP */
NOT_SA1(
static void Op08E1()
{
	PackStatus();
	AddCycles(Settings.OneCycle);
	PushBE(ICPU.Registers.PL);
	ICPU.OpenBus = ICPU.Registers.PL;
}
)

static void Op08E0()
{
	PackStatus();
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.PL);
	ICPU.OpenBus = ICPU.Registers.PL;
}

static void Op08Slow()
{
	PackStatus();
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		PushBE(ICPU.Registers.PL);
	else
		PushB(ICPU.Registers.PL);

	ICPU.OpenBus = ICPU.Registers.PL;
}

/* PHX */
NOT_SA1(
static void OpDAE1()
{
	AddCycles(Settings.OneCycle);
	PushBE(ICPU.Registers.XL);
	ICPU.OpenBus = ICPU.Registers.XL;
}
)

static void OpDAE0X1()
{
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.XL);
	ICPU.OpenBus = ICPU.Registers.XL;
}

static void OpDAE0X0()
{
	AddCycles(Settings.OneCycle);
	PushW(ICPU.Registers.X.W);
	ICPU.OpenBus = ICPU.Registers.XL;
}

static void OpDASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		PushBE(ICPU.Registers.XL);
	else if (CheckIndex())
		PushB(ICPU.Registers.XL);
	else
		PushW(ICPU.Registers.X.W);

	ICPU.OpenBus = ICPU.Registers.XL;
}

/* PHY */
NOT_SA1(
static void Op5AE1()
{
	AddCycles(Settings.OneCycle);
	PushBE(ICPU.Registers.YL);
	ICPU.OpenBus = ICPU.Registers.YL;
}
)

static void Op5AE0X1()
{
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.YL);
	ICPU.OpenBus = ICPU.Registers.YL;
}

static void Op5AE0X0()
{
	AddCycles(Settings.OneCycle);
	PushW(ICPU.Registers.Y.W);
	ICPU.OpenBus = ICPU.Registers.YL;
}

static void Op5ASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		PushBE(ICPU.Registers.YL);
	else if (CheckIndex())
		PushB(ICPU.Registers.YL);
	else
		PushW(ICPU.Registers.Y.W);

	ICPU.OpenBus = ICPU.Registers.YL;
}

/* PULL Instructions */
static INLINE uint8_t PullB()
{
	return GetByte(++ICPU.Registers.S.W);
}

static INLINE uint8_t PullBE()
{
	ICPU.Registers.SL++;
	return GetByte(ICPU.Registers.S.W);
}

static INLINE uint16_t PullW()
{
	uint16_t w = GetWord(ICPU.Registers.S.W + 1, WRAP_BANK);
	ICPU.Registers.S.W += 2;
	return w;
}

static INLINE uint16_t PullWE()
{
	ICPU.Registers.SL++;
	uint16_t w = GetWord(ICPU.Registers.S.W, WRAP_PAGE);
	ICPU.Registers.SL++;
	return w;
}

/* PLA */
NOT_SA1(
static void Op68E1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.AL = PullBE();
	SetZN8(ICPU.Registers.AL);
	ICPU.OpenBus = ICPU.Registers.AL;
}
)

static void Op68E0M1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.AL = PullB();
	SetZN8(ICPU.Registers.AL);
	ICPU.OpenBus = ICPU.Registers.AL;
}

static void Op68E0M0()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.A.W = PullW();
	SetZN16(ICPU.Registers.A.W);
	ICPU.OpenBus = ICPU.Registers.AH;
}

static void Op68Slow()
{
	AddCycles(Settings.TwoCycles);

	if (CheckEmulation())
	{
		ICPU.Registers.AL = PullBE();
		SetZN8(ICPU.Registers.AL);
		ICPU.OpenBus = ICPU.Registers.AL;
	}
	else if (CheckMem())
	{
		ICPU.Registers.AL = PullB();
		SetZN8(ICPU.Registers.AL);
		ICPU.OpenBus = ICPU.Registers.AL;
	}
	else
	{
		ICPU.Registers.A.W = PullW();
		SetZN16(ICPU.Registers.A.W);
		ICPU.OpenBus = ICPU.Registers.AH;
	}
}

/* PLB */
NOT_SA1(
static void OpABE1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.DB = PullBE();
	SetZN8(ICPU.Registers.DB);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = ICPU.Registers.DB;
}
)

static void OpABE0()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.DB = PullB();
	SetZN8(ICPU.Registers.DB);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = ICPU.Registers.DB;
}

static void OpABSlow()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.DB = (CheckEmulation() ? PullBE() : PullB());
	SetZN8(ICPU.Registers.DB);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = ICPU.Registers.DB;
}

/* PLD */
NOT_SA1(
static void Op2BE1()
{
	/* Note: PLD is a new instruction,
	 * and so doesn't respect the emu-mode stack bounds. */
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.D.W = PullW();
	SetZN16(ICPU.Registers.D.W);
	ICPU.OpenBus = ICPU.Registers.DH;
	ICPU.Registers.SH = 1;
}
)

static void Op2BE0()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.D.W = PullW();
	SetZN16(ICPU.Registers.D.W);
	ICPU.OpenBus = ICPU.Registers.DH;
}

static void Op2BSlow()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.D.W = PullW();
	SetZN16(ICPU.Registers.D.W);
	ICPU.OpenBus = ICPU.Registers.DH;

	if (CheckEmulation())
		ICPU.Registers.SH = 1;
}

/* PLP */
NOT_SA1(
static void Op28E1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.PL = PullBE();
	ICPU.OpenBus = ICPU.Registers.PL;
	SetFlags(MEMORY_FLAG | INDEX_FLAG);
	UnpackStatus();
	FixCycles();
	CHECK_FOR_IRQ();
}
)

static void Op28E0()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.PL = PullB();
	ICPU.OpenBus = ICPU.Registers.PL;
	UnpackStatus();

	if (CheckIndex())
	{
		ICPU.Registers.XH = 0;
		ICPU.Registers.YH = 0;
	}

	FixCycles();
	CHECK_FOR_IRQ();
}

static void Op28Slow()
{
	AddCycles(Settings.TwoCycles);

	if (CheckEmulation())
	{
		ICPU.Registers.PL = PullBE();
		ICPU.OpenBus = ICPU.Registers.PL;
		SetFlags(MEMORY_FLAG | INDEX_FLAG);
	}
	else
	{
		ICPU.Registers.PL = PullB();
		ICPU.OpenBus = ICPU.Registers.PL;
	}

	UnpackStatus();

	if (CheckIndex())
	{
		ICPU.Registers.XH = 0;
		ICPU.Registers.YH = 0;
	}

	FixCycles();
	CHECK_FOR_IRQ();
}

/* PLX */
NOT_SA1(
static void OpFAE1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.XL = PullBE();
	SetZN8(ICPU.Registers.XL);
	ICPU.OpenBus = ICPU.Registers.XL;
}
)

static void OpFAE0X1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.XL = PullB();
	SetZN8(ICPU.Registers.XL);
	ICPU.OpenBus = ICPU.Registers.XL;
}

static void OpFAE0X0()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.X.W = PullW();
	SetZN16(ICPU.Registers.X.W);
	ICPU.OpenBus = ICPU.Registers.XH;
}

static void OpFASlow()
{
	AddCycles(Settings.TwoCycles);

	if (CheckEmulation())
	{
		ICPU.Registers.XL = PullBE();
		SetZN8(ICPU.Registers.XL);
		ICPU.OpenBus = ICPU.Registers.XL;
	}
	else if (CheckIndex())
	{
		ICPU.Registers.XL = PullB();
		SetZN8(ICPU.Registers.XL);
		ICPU.OpenBus = ICPU.Registers.XL;
	}
	else
	{
		ICPU.Registers.X.W = PullW();
		SetZN16(ICPU.Registers.X.W);
		ICPU.OpenBus = ICPU.Registers.XH;
	}
}

/* PLY */
NOT_SA1(
static void Op7AE1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.YL = PullBE();
	SetZN8(ICPU.Registers.YL);
	ICPU.OpenBus = ICPU.Registers.YL;
}
)

static void Op7AE0X1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.YL = PullB();
	SetZN8(ICPU.Registers.YL);
	ICPU.OpenBus = ICPU.Registers.YL;
}

static void Op7AE0X0()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.Y.W = PullW();
	SetZN16(ICPU.Registers.Y.W);
	ICPU.OpenBus = ICPU.Registers.YH;
}

static void Op7ASlow()
{
	AddCycles(Settings.TwoCycles);

	if (CheckEmulation())
	{
		ICPU.Registers.YL = PullBE();
		SetZN8(ICPU.Registers.YL);
		ICPU.OpenBus = ICPU.Registers.YL;
	}
	else if (CheckIndex())
	{
		ICPU.Registers.YL = PullB();
		SetZN8(ICPU.Registers.YL);
		ICPU.OpenBus = ICPU.Registers.YL;
	}
	else
	{
		ICPU.Registers.Y.W = PullW();
		SetZN16(ICPU.Registers.Y.W);
		ICPU.OpenBus = ICPU.Registers.YH;
	}
}

/* TAX */
static void OpAAX1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.XL = ICPU.Registers.AL;
	SetZN8(ICPU.Registers.XL);
}

static void OpAAX0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.X.W = ICPU.Registers.A.W;
	SetZN16(ICPU.Registers.X.W);
}

static void OpAASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckIndex())
	{
		ICPU.Registers.XL = ICPU.Registers.AL;
		SetZN8(ICPU.Registers.XL);
	}
	else
	{
		ICPU.Registers.X.W = ICPU.Registers.A.W;
		SetZN16(ICPU.Registers.X.W);
	}
}

/* TAY */
static void OpA8X1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.YL = ICPU.Registers.AL;
	SetZN8(ICPU.Registers.YL);
}

static void OpA8X0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.Y.W = ICPU.Registers.A.W;
	SetZN16(ICPU.Registers.Y.W);
}

static void OpA8Slow()
{
	AddCycles(Settings.OneCycle);

	if (CheckIndex())
	{
		ICPU.Registers.YL = ICPU.Registers.AL;
		SetZN8(ICPU.Registers.YL);
	}
	else
	{
		ICPU.Registers.Y.W = ICPU.Registers.A.W;
		SetZN16(ICPU.Registers.Y.W);
	}
}

static void Op5B() /* TCD */
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.D.W = ICPU.Registers.A.W;
	SetZN16(ICPU.Registers.D.W);
}

static void Op1B() /* TCS */
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.S.W = ICPU.Registers.A.W;

	if (CheckEmulation())
		ICPU.Registers.SH = 1;
}

static void Op7B() /* TDC */
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.A.W = ICPU.Registers.D.W;
	SetZN16(ICPU.Registers.A.W);
}

static void Op3B() /* TSC */
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.A.W = ICPU.Registers.S.W;
	SetZN16(ICPU.Registers.A.W);
}

/* TSX */
static void OpBAX1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.XL = ICPU.Registers.SL;
	SetZN8(ICPU.Registers.XL);
}

static void OpBAX0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.X.W = ICPU.Registers.S.W;
	SetZN16(ICPU.Registers.X.W);
}

static void OpBASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckIndex())
	{
		ICPU.Registers.XL = ICPU.Registers.SL;
		SetZN8(ICPU.Registers.XL);
	}
	else
	{
		ICPU.Registers.X.W = ICPU.Registers.S.W;
		SetZN16(ICPU.Registers.X.W);
	}
}

/* TXA */
static void Op8AM1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.AL = ICPU.Registers.XL;
	SetZN8(ICPU.Registers.AL);
}

static void Op8AM0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.A.W = ICPU.Registers.X.W;
	SetZN16(ICPU.Registers.A.W);
}

static void Op8ASlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckMem())
	{
		ICPU.Registers.AL = ICPU.Registers.XL;
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Registers.A.W = ICPU.Registers.X.W;
		SetZN16(ICPU.Registers.A.W);
	}
}

static void Op9A() /* TXS */
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.S.W = ICPU.Registers.X.W;

	if (CheckEmulation())
		ICPU.Registers.SH = 1;
}

/* TXY */
static void Op9BX1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.YL = ICPU.Registers.XL;
	SetZN8(ICPU.Registers.YL);
}

static void Op9BX0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.Y.W = ICPU.Registers.X.W;
	SetZN16(ICPU.Registers.Y.W);
}

static void Op9BSlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckIndex())
	{
		ICPU.Registers.YL = ICPU.Registers.XL;
		SetZN8(ICPU.Registers.YL);
	}
	else
	{
		ICPU.Registers.Y.W = ICPU.Registers.X.W;
		SetZN16(ICPU.Registers.Y.W);
	}
}

/* TYA */
static void Op98M1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.AL = ICPU.Registers.YL;
	SetZN8(ICPU.Registers.AL);
}

static void Op98M0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.A.W = ICPU.Registers.Y.W;
	SetZN16(ICPU.Registers.A.W);
}

static void Op98Slow()
{
	AddCycles(Settings.OneCycle);

	if (CheckMem())
	{
		ICPU.Registers.AL = ICPU.Registers.YL;
		SetZN8(ICPU.Registers.AL);
	}
	else
	{
		ICPU.Registers.A.W = ICPU.Registers.Y.W;
		SetZN16(ICPU.Registers.A.W);
	}
}

/* TYX */
static void OpBBX1()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.XL = ICPU.Registers.YL;
	SetZN8(ICPU.Registers.XL);
}

static void OpBBX0()
{
	AddCycles(Settings.OneCycle);
	ICPU.Registers.X.W = ICPU.Registers.Y.W;
	SetZN16(ICPU.Registers.X.W);
}

static void OpBBSlow()
{
	AddCycles(Settings.OneCycle);

	if (CheckIndex())
	{
		ICPU.Registers.XL = ICPU.Registers.YL;
		SetZN8(ICPU.Registers.XL);
	}
	else
	{
		ICPU.Registers.X.W = ICPU.Registers.Y.W;
		SetZN16(ICPU.Registers.X.W);
	}
}

static void OpFB() /* XCE */
{
	uint8_t A1, A2;
	AddCycles(Settings.OneCycle);
	A1                = ICPU.Carry;
	A2                = ICPU.Registers.PH;
	ICPU.Carry        = A2 & 1;
	ICPU.Registers.PH = A1;

	if (CheckEmulation())
	{
		SetFlags(MEMORY_FLAG | INDEX_FLAG);
		ICPU.Registers.SH = 1;
	}

	if (CheckIndex())
	{
		ICPU.Registers.XH = 0;
		ICPU.Registers.YH = 0;
	}

	FixCycles();
}

static void Op00() /* BRK */
{
	bool emulation;
	uint16_t addr;
	AddCycles(CPU.MemSpeed);
	emulation = CheckEmulation();

	if (emulation)
	{
		PushWE(ICPU.Registers.PCw + 1);
		PackStatus();
		PushBE(ICPU.Registers.PL);
	}
	else
	{
		PushB(ICPU.Registers.PB);
		PushW(ICPU.Registers.PCw + 1);
		PackStatus();
		PushB(ICPU.Registers.PL);
	}

	ICPU.OpenBus = ICPU.Registers.PL;
	ClearDecimal();
	SetIRQ();
	addr = GetWord(emulation ? 0xFFFE : 0xFFE6, WRAP_NONE);
	SetPCBase(addr);
	ICPU.OpenBus = addr >> 8;
}

void OpcodeIRQ() /* IRQ */
{
	bool emulation;
	AddCycles(CPU.MemSpeed + Settings.OneCycle); /* IRQ and NMI do an opcode fetch as their first "IO" cycle. */
	emulation = CheckEmulation();

	if (emulation)
	{
		PushWE(ICPU.Registers.PCw);
		PackStatus();
		PushBE(ICPU.Registers.PL);
	}
	else
	{
		PushB(ICPU.Registers.PB);
		PushW(ICPU.Registers.PCw);
		PackStatus();
		PushB(ICPU.Registers.PL);
	}

	ICPU.OpenBus = ICPU.Registers.PL;
	ClearDecimal();
	SetIRQ();

#ifdef SA1_OPCODES
	SA1.OpenBus = Memory.FillRAM[0x2208];
	SA1SetPCBase(Memory.FillRAM[0x2207] | (Memory.FillRAM[0x2208] << 8));
#else
	if (Settings.Chip == SA_1 && (Memory.FillRAM[0x2209] & 0x40))
	{
		ICPU.OpenBus = Memory.FillRAM[0x220f];
		AddCycles(2 * Settings.OneCycle);
		SetPCBase(Memory.FillRAM[0x220e] | (Memory.FillRAM[0x220f] << 8));
	}
	else
	{
		uint16_t addr = GetWord(emulation ? 0xFFFE : 0xFFEE, WRAP_NONE);
		ICPU.OpenBus = addr >> 8;
		SetPCBase(addr);
	}
#endif
}

void OpcodeNMI() /* NMI */
{
	bool emulation;
	AddCycles(CPU.MemSpeed + Settings.OneCycle); /* IRQ and NMI do an opcode fetch as their first "IO" cycle. */
	emulation = CheckEmulation();

	if (emulation)
	{
		PushWE(ICPU.Registers.PCw);
		PackStatus();
		PushBE(ICPU.Registers.PL);
	}
	else
	{
		PushB(ICPU.Registers.PB);
		PushW(ICPU.Registers.PCw);
		PackStatus();
		PushB(ICPU.Registers.PL);
	}

	ICPU.OpenBus = ICPU.Registers.PL;
	ClearDecimal();
	SetIRQ();

#ifdef SA1_OPCODES
	ICPU.OpenBus = Memory.FillRAM[0x2206];
	SA1SetPCBase(Memory.FillRAM[0x2205] | (Memory.FillRAM[0x2206] << 8));
#else
	if (Settings.Chip == SA_1 && (Memory.FillRAM[0x2209] & 0x10))
	{
		ICPU.OpenBus = Memory.FillRAM[0x220d];
		AddCycles(2 * Settings.OneCycle);
		SetPCBase(Memory.FillRAM[0x220c] | (Memory.FillRAM[0x220d] << 8));
	}
	else
	{
		uint16_t addr = GetWord(emulation ? 0xFFFA : 0xFFEA, WRAP_NONE);
		ICPU.OpenBus = addr >> 8;
		SetPCBase(addr);
	}
#endif
}

static void Op02() /* COP */
{
	bool emulation;
	uint16_t addr;
	AddCycles(CPU.MemSpeed);
	emulation = CheckEmulation();

	if (emulation)
	{
		PushWE(ICPU.Registers.PCw + 1);
		PackStatus();
		PushBE(ICPU.Registers.PL);
	}
	else
	{
		PushB(ICPU.Registers.PB);
		PushW(ICPU.Registers.PCw + 1);
		PackStatus();
		PushB(ICPU.Registers.PL);
	}

	ICPU.OpenBus = ICPU.Registers.PL;
	ClearDecimal();
	SetIRQ();
	addr = GetWord(emulation ? 0xFFF4 : 0xFFE4, WRAP_NONE);
	SetPCBase(addr);
	ICPU.OpenBus = addr >> 8;
}

/* JML */
static void OpDC()
{
	SetPCBase(AbsoluteIndirectLong(JUMP));
}

static void OpDCSlow()
{
	SetPCBase(AbsoluteIndirectLongSlow(JUMP));
}

static void Op5C()
{
	SetPCBase(AbsoluteLong(JUMP));
}

static void Op5CSlow()
{
	SetPCBase(AbsoluteLongSlow(JUMP));
}

/* JMP */
static void Op4C()
{
	SetPCBase(ICPU.ShiftedPB + ((uint16_t) Absolute(JUMP)));

#ifdef SA1_OPCODES
	CPUShutdown();
#endif
}

static void Op4CSlow()
{
	SetPCBase(ICPU.ShiftedPB + ((uint16_t) AbsoluteSlow(JUMP)));

#ifdef SA1_OPCODES
	CPUShutdown();
#endif
}

static void Op6C()
{
	SetPCBase(ICPU.ShiftedPB + ((uint16_t) AbsoluteIndirect(JUMP)));
}

static void Op6CSlow()
{
	SetPCBase(ICPU.ShiftedPB + ((uint16_t) AbsoluteIndirectSlow(JUMP)));
}

static void Op7C()
{
	SetPCBase(ICPU.ShiftedPB + ((uint16_t) AbsoluteIndexedIndirect(JUMP)));
}

static void Op7CSlow()
{
	SetPCBase(ICPU.ShiftedPB + ((uint16_t) AbsoluteIndexedIndirectSlow(JUMP)));
}

/* JSL/RTL */
NOT_SA1(
static void Op22E1()
{
	/* Note: JSL is a new instruction,
	 * and so doesn't respect the emu-mode stack bounds. */
	uint32_t addr = AbsoluteLong(JSR);
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.PB);
	PushW(ICPU.Registers.PCw - 1);
	ICPU.Registers.SH = 1;
	SetPCBase(addr);
}
)

static void Op22E0()
{
	uint32_t addr = AbsoluteLong(JSR);
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.PB);
	PushW(ICPU.Registers.PCw - 1);
	SetPCBase(addr);
}

static void Op22Slow()
{
	uint32_t addr = AbsoluteLongSlow(JSR);
	AddCycles(Settings.OneCycle);
	PushB(ICPU.Registers.PB);
	PushW(ICPU.Registers.PCw - 1);

	if (CheckEmulation())
		ICPU.Registers.SH = 1;

	SetPCBase(addr);
}

NOT_SA1(
static void Op6BE1()
{
	/* Note: RTL is a new instruction,
	 * and so doesn't respect the emu-mode stack bounds. */
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.PCw = PullW();
	ICPU.Registers.PB = PullB();
	ICPU.Registers.SH = 1;
	ICPU.Registers.PCw++;
	SetPCBase(ICPU.Registers.PBPC);
	AddCycles(Settings.OneCycle);
}
)

static void Op6BE0()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.PCw = PullW();
	ICPU.Registers.PB = PullB();
	ICPU.Registers.PCw++;
	SetPCBase(ICPU.Registers.PBPC);
	AddCycles(Settings.OneCycle);
}

static void Op6BSlow()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.PCw = PullW();
	ICPU.Registers.PB = PullB();

	if (CheckEmulation())
		ICPU.Registers.SH = 1;

	ICPU.Registers.PCw++;
	SetPCBase(ICPU.Registers.PBPC);
	AddCycles(Settings.OneCycle);
}

/* JSR/RTS */
NOT_SA1(
static void Op20E1()
{
	uint16_t addr = Absolute(JSR);
	AddCycles(Settings.OneCycle);
	PushWE(ICPU.Registers.PCw - 1);
	SetPCBase(ICPU.ShiftedPB + addr);
}
)

static void Op20E0()
{
	uint16_t addr = Absolute(JSR);
	AddCycles(Settings.OneCycle);
	PushW(ICPU.Registers.PCw - 1);
	SetPCBase(ICPU.ShiftedPB + addr);
}

static void Op20Slow()
{
	uint16_t addr = AbsoluteSlow(JSR);
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		PushWE(ICPU.Registers.PCw - 1);
	else
		PushW(ICPU.Registers.PCw - 1);

	SetPCBase(ICPU.ShiftedPB + addr);
}

/* JSR (a,X) */
NOT_SA1(
static void OpFCE1()
{
	/* Note: JSR (a,X) is a new instruction,
	 * and so doesn't respect the emu-mode stack bounds. */
	uint16_t addr = AbsoluteIndexedIndirect(JSR);
	PushW(ICPU.Registers.PCw - 1);
	ICPU.Registers.SH = 1;
	SetPCBase(ICPU.ShiftedPB + addr);
}
)

static void OpFCE0()
{
	uint16_t addr = AbsoluteIndexedIndirect(JSR);
	PushW(ICPU.Registers.PCw - 1);
	SetPCBase(ICPU.ShiftedPB + addr);
}

static void OpFCSlow()
{
	uint16_t addr = AbsoluteIndexedIndirectSlow(JSR);
	PushW(ICPU.Registers.PCw - 1);

	if (CheckEmulation())
		ICPU.Registers.SH = 1;

	SetPCBase(ICPU.ShiftedPB + addr);
}

NOT_SA1(
static void Op60E1()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.PCw = PullWE();
	AddCycles(Settings.OneCycle);
	ICPU.Registers.PCw++;
	SetPCBase(ICPU.Registers.PBPC);
}
)

static void Op60E0()
{
	AddCycles(Settings.TwoCycles);
	ICPU.Registers.PCw = PullW();
	AddCycles(Settings.OneCycle);
	ICPU.Registers.PCw++;
	SetPCBase(ICPU.Registers.PBPC);
}

static void Op60Slow()
{
	AddCycles(Settings.TwoCycles);

	if (CheckEmulation())
		ICPU.Registers.PCw = PullWE();
	else
		ICPU.Registers.PCw = PullW();

	AddCycles(Settings.OneCycle);
	ICPU.Registers.PCw++;
	SetPCBase(ICPU.Registers.PBPC);
}

/* MVN/MVP */
static void Op54X1()
{
	uint32_t tmp;
	ICPU.Registers.DB = Immediate8(NONE);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = tmp = Immediate8(NONE);
	ICPU.OpenBus = tmp = GetByte((tmp << 16) + ICPU.Registers.X.W);
	SetByte(tmp, ICPU.ShiftedDB + ICPU.Registers.Y.W);

	ICPU.Registers.XL++;
	ICPU.Registers.YL++;
	ICPU.Registers.A.W--;

	if (ICPU.Registers.A.W != 0xffff)
		ICPU.Registers.PCw -= 3;

	AddCycles(Settings.TwoCycles);
}

static void Op54X0()
{
	uint32_t tmp;
	ICPU.Registers.DB = Immediate8(NONE);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = tmp = Immediate8(NONE);
	ICPU.OpenBus = tmp = GetByte((tmp << 16) + ICPU.Registers.X.W);
	SetByte(tmp, ICPU.ShiftedDB + ICPU.Registers.Y.W);

	ICPU.Registers.X.W++;
	ICPU.Registers.Y.W++;
	ICPU.Registers.A.W--;

	if (ICPU.Registers.A.W != 0xffff)
		ICPU.Registers.PCw -= 3;

	AddCycles(Settings.TwoCycles);
}

static void Op54Slow()
{
	uint32_t tmp;
	ICPU.OpenBus = ICPU.Registers.DB = Immediate8Slow(NONE);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = tmp = Immediate8Slow(NONE);
	ICPU.OpenBus = tmp = GetByte((tmp << 16) + ICPU.Registers.X.W);
	SetByte(tmp, ICPU.ShiftedDB + ICPU.Registers.Y.W);

	if (CheckIndex())
	{
		ICPU.Registers.XL++;
		ICPU.Registers.YL++;
	}
	else
	{
		ICPU.Registers.X.W++;
		ICPU.Registers.Y.W++;
	}

	ICPU.Registers.A.W--;

	if (ICPU.Registers.A.W != 0xffff)
		ICPU.Registers.PCw -= 3;

	AddCycles(Settings.TwoCycles);
}

static void Op44X1()
{
	uint32_t tmp;
	ICPU.Registers.DB = Immediate8(NONE);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = tmp = Immediate8(NONE);
	ICPU.OpenBus = tmp = GetByte((tmp << 16) + ICPU.Registers.X.W);
	SetByte(tmp, ICPU.ShiftedDB + ICPU.Registers.Y.W);
	ICPU.Registers.XL--;
	ICPU.Registers.YL--;
	ICPU.Registers.A.W--;

	if (ICPU.Registers.A.W != 0xffff)
		ICPU.Registers.PCw -= 3;

	AddCycles(Settings.TwoCycles);
}

static void Op44X0()
{
	uint32_t tmp;
	ICPU.Registers.DB = Immediate8(NONE);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = tmp = Immediate8(NONE);
	ICPU.OpenBus = tmp = GetByte((tmp << 16) + ICPU.Registers.X.W);
	SetByte(tmp, ICPU.ShiftedDB + ICPU.Registers.Y.W);
	ICPU.Registers.X.W--;
	ICPU.Registers.Y.W--;
	ICPU.Registers.A.W--;

	if (ICPU.Registers.A.W != 0xffff)
		ICPU.Registers.PCw -= 3;

	AddCycles(Settings.TwoCycles);
}

static void Op44Slow()
{
	uint32_t tmp;
	ICPU.OpenBus = ICPU.Registers.DB = Immediate8Slow(NONE);
	ICPU.ShiftedDB = ICPU.Registers.DB << 16;
	ICPU.OpenBus = tmp = Immediate8Slow(NONE);
	ICPU.OpenBus = tmp = GetByte((tmp << 16) + ICPU.Registers.X.W);
	SetByte(tmp, ICPU.ShiftedDB + ICPU.Registers.Y.W);

	if (CheckIndex())
	{
		ICPU.Registers.XL--;
		ICPU.Registers.YL--;
	}
	else
	{
		ICPU.Registers.X.W--;
		ICPU.Registers.Y.W--;
	}

	ICPU.Registers.A.W--;

	if (ICPU.Registers.A.W != 0xffff)
		ICPU.Registers.PCw -= 3;

	AddCycles(Settings.TwoCycles);
}

/* REP/SEP */
static void OpC2()
{
	uint8_t Work8 = ~Immediate8(READ);
	ICPU.Registers.PL &= Work8;
	ICPU.Carry &= Work8;
	ICPU.Overflow &= (Work8 >> 6);
	ICPU.Negative &= Work8;
	ICPU.Zero |= ~Work8 & ZERO;
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		SetFlags(MEMORY_FLAG | INDEX_FLAG);

	if (CheckIndex())
	{
		ICPU.Registers.XH = 0;
		ICPU.Registers.YH = 0;
	}

	FixCycles();
	CHECK_FOR_IRQ();
}

static void OpC2Slow()
{
	uint8_t Work8 = ~Immediate8Slow(READ);
	ICPU.Registers.PL &= Work8;
	ICPU.Carry &= Work8;
	ICPU.Overflow &= (Work8 >> 6);
	ICPU.Negative &= Work8;
	ICPU.Zero |= ~Work8 & ZERO;
	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		SetFlags(MEMORY_FLAG | INDEX_FLAG);

	if (CheckIndex())
	{
		ICPU.Registers.XH = 0;
		ICPU.Registers.YH = 0;
	}

	FixCycles();
	CHECK_FOR_IRQ();
}

static void OpE2()
{
	uint8_t Work8 = Immediate8(READ);
	ICPU.Registers.PL |= Work8;
	ICPU.Carry        |= Work8 & 1;
	ICPU.Overflow     |= (Work8 >> 6) & 1;
	ICPU.Negative     |= Work8;

	if (Work8 & ZERO)
		ICPU.Zero = 0;

	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		SetFlags(MEMORY_FLAG | INDEX_FLAG);

	if (CheckIndex())
	{
		ICPU.Registers.XH = 0;
		ICPU.Registers.YH = 0;
	}

	FixCycles();
}

static void OpE2Slow()
{
	uint8_t Work8 = Immediate8Slow(READ);
	ICPU.Registers.PL |= Work8;
	ICPU.Carry |= Work8 & 1;
	ICPU.Overflow |= (Work8 >> 6) & 1;
	ICPU.Negative |= Work8;

	if (Work8 & ZERO)
		ICPU.Zero = 0;

	AddCycles(Settings.OneCycle);

	if (CheckEmulation())
		SetFlags(MEMORY_FLAG | INDEX_FLAG);

	if (CheckIndex())
	{
		ICPU.Registers.XH = 0;
		ICPU.Registers.YH = 0;
	}

	FixCycles();
}

static void OpEB() /* XBA */
{
	uint8_t Work8     = ICPU.Registers.AL;
	ICPU.Registers.AL = ICPU.Registers.AH;
	ICPU.Registers.AH = Work8;
	SetZN8(ICPU.Registers.AL);
	AddCycles(Settings.TwoCycles);
}

static void Op40Slow() /* RTI */
{
	AddCycles(Settings.TwoCycles);

	if (!CheckEmulation())
	{
		ICPU.Registers.PL = PullB();
		UnpackStatus();
		ICPU.Registers.PCw = PullW();
		ICPU.Registers.PB = PullB();
		ICPU.OpenBus = ICPU.Registers.PB;
		ICPU.ShiftedPB = ICPU.Registers.PB << 16;
	}
	else
	{
		ICPU.Registers.PL = PullBE();
		UnpackStatus();
		ICPU.Registers.PCw = PullWE();
		ICPU.OpenBus = ICPU.Registers.PCh;
		SetFlags(MEMORY_FLAG | INDEX_FLAG);
	}

	SetPCBase(ICPU.Registers.PBPC);

	if (CheckIndex())
	{
		ICPU.Registers.XH = 0;
		ICPU.Registers.YH = 0;
	}

	FixCycles();
	CHECK_FOR_IRQ();
}

static void OpCB() /* WAI */
{
	CPU.WaitingForInterrupt = true;
	ICPU.Registers.PCw--;

NOT_SA1(
	if (Settings.Shutdown)
		CPU.Cycles = CPU.NextEvent;
	else
		AddCycles(Settings.OneCycle);
)
}

static void OpDB() /* Usually an STP opcode; SNESAdvance speed hack, not implemented in Snes9xTYL | Snes9x-Euphoria (from the speed-hacks branch of CatSFC) */
{
	int8_t  BranchOffset;
	uint32_t OpAddress;
	uint8_t NextByte = CPU.PCBase[ICPU.Registers.PCw++];
	ForceShutdown();
	BranchOffset = (NextByte & 0x7F) | ((NextByte & 0x40) << 1);
	OpAddress = ((int32_t) ICPU.Registers.PCw + BranchOffset) & 0xffff;

	switch (NextByte & 0x80)
	{
		case 0x00: /* BNE */
			bOpBody(OpAddress, BranchCheck, !CheckZero(), CheckEmulation())
			return;
		case 0x80: /* BEQ */
			bOpBody(OpAddress, BranchCheck,  CheckZero(), CheckEmulation())
			return;
	}
}

static void Op42() /* SNESAdvance speed hack, as implemented in Snes9xTYL / Snes9x-Euphoria (from the speed-hacks branch of CatSFC) */
{
	int8_t  BranchOffset;
	uint32_t OpAddress;
	uint8_t NextByte = CPU.PCBase[ICPU.Registers.PCw++];
	ForceShutdown();
	BranchOffset = 0xF0 | (NextByte & 0xF); /* always negative */
	OpAddress    = ((int32_t) ICPU.Registers.PCw + BranchOffset) & 0xffff;

	switch (NextByte & 0xF0)
	{
		case 0x10: /* BPL */
			bOpBody(OpAddress, BranchCheck, !CheckNegative(), CheckEmulation())
			return;
		case 0x30: /* BMI */
			bOpBody(OpAddress, BranchCheck,  CheckNegative(), CheckEmulation())
			return;
		case 0x50: /* BVC */
			bOpBody(OpAddress, BranchCheck, !CheckOverflow(), CheckEmulation())
			return;
		case 0x70: /* BVS */
			bOpBody(OpAddress, BranchCheck,  CheckOverflow(), CheckEmulation())
			return;
		case 0x80: /* BRA */
			bOpBody(OpAddress, NoCheck,      true,            CheckEmulation())
			return;
		case 0x90: /* BCC */
			bOpBody(OpAddress, BranchCheck, !CheckCarry(),    CheckEmulation())
			return;
		case 0xB0: /* BCS */
			bOpBody(OpAddress, BranchCheck,  CheckCarry(),    CheckEmulation())
			return;
		case 0xD0: /* BNE */
			bOpBody(OpAddress, BranchCheck, !CheckZero(),     CheckEmulation())
			return;
		case 0xF0: /* BEQ */
			bOpBody(OpAddress, BranchCheck,  CheckZero(),     CheckEmulation())
			return;
	}
}

/* CPU Opcode Definitions */
NOT_SA1(
SOpcodes OpcodesE1[256] =
{
	{Op00},     {Op01E1},   {Op02},   {Op03M1}, {Op04M1},   {Op05M1},   {Op06M1},   {Op07M1},
	{Op08E1},   {Op09M1},   {Op0AM1}, {Op0BE1}, {Op0CM1},   {Op0DM1},   {Op0EM1},   {Op0FM1},
	{Op10E1},   {Op11E1},   {Op12E1}, {Op13M1}, {Op14M1},   {Op15E1},   {Op16E1},   {Op17M1},
	{Op18},     {Op19M1X1}, {Op1AM1}, {Op1B},   {Op1CM1},   {Op1DM1X1}, {Op1EM1X1}, {Op1FM1},
	{Op20E1},   {Op21E1},   {Op22E1}, {Op23M1}, {Op24M1},   {Op25M1},   {Op26M1},   {Op27M1},
	{Op28E1},   {Op29M1},   {Op2AM1}, {Op2BE1}, {Op2CM1},   {Op2DM1},   {Op2EM1},   {Op2FM1},
	{Op30E1},   {Op31E1},   {Op32E1}, {Op33M1}, {Op34E1},   {Op35E1},   {Op36E1},   {Op37M1},
	{Op38},     {Op39M1X1}, {Op3AM1}, {Op3B},   {Op3CM1X1}, {Op3DM1X1}, {Op3EM1X1}, {Op3FM1},
	{Op40Slow}, {Op41E1},   {Op42},   {Op43M1}, {Op44X1},   {Op45M1},   {Op46M1},   {Op47M1},
	{Op48E1},   {Op49M1},   {Op4AM1}, {Op4BE1}, {Op4C},     {Op4DM1},   {Op4EM1},   {Op4FM1},
	{Op50E1},   {Op51E1},   {Op52E1}, {Op53M1}, {Op54X1},   {Op55E1},   {Op56E1},   {Op57M1},
	{Op58},     {Op59M1X1}, {Op5AE1}, {Op5B},   {Op5C},     {Op5DM1X1}, {Op5EM1X1}, {Op5FM1},
	{Op60E1},   {Op61E1},   {Op62E1}, {Op63M1}, {Op64M1},   {Op65M1},   {Op66M1},   {Op67M1},
	{Op68E1},   {Op69M1},   {Op6AM1}, {Op6BE1}, {Op6C},     {Op6DM1},   {Op6EM1},   {Op6FM1},
	{Op70E1},   {Op71E1},   {Op72E1}, {Op73M1}, {Op74E1},   {Op75E1},   {Op76E1},   {Op77M1},
	{Op78},     {Op79M1X1}, {Op7AE1}, {Op7B},   {Op7C},     {Op7DM1X1}, {Op7EM1X1}, {Op7FM1},
	{Op80E1},   {Op81E1},   {Op82},   {Op83M1}, {Op84X1},   {Op85M1},   {Op86X1},   {Op87M1},
	{Op88X1},   {Op89M1},   {Op8AM1}, {Op8BE1}, {Op8CX1},   {Op8DM1},   {Op8EX1},   {Op8FM1},
	{Op90E1},   {Op91E1},   {Op92E1}, {Op93M1}, {Op94E1},   {Op95E1},   {Op96E1},   {Op97M1},
	{Op98M1},   {Op99M1X1}, {Op9A},   {Op9BX1}, {Op9CM1},   {Op9DM1X1}, {Op9EM1X1}, {Op9FM1},
	{OpA0X1},   {OpA1E1},   {OpA2X1}, {OpA3M1}, {OpA4X1},   {OpA5M1},   {OpA6X1},   {OpA7M1},
	{OpA8X1},   {OpA9M1},   {OpAAX1}, {OpABE1}, {OpACX1},   {OpADM1},   {OpAEX1},   {OpAFM1},
	{OpB0E1},   {OpB1E1},   {OpB2E1}, {OpB3M1}, {OpB4E1},   {OpB5E1},   {OpB6E1},   {OpB7M1},
	{OpB8},     {OpB9M1X1}, {OpBAX1}, {OpBBX1}, {OpBCX1},   {OpBDM1X1}, {OpBEX1},   {OpBFM1},
	{OpC0X1},   {OpC1E1},   {OpC2},   {OpC3M1}, {OpC4X1},   {OpC5M1},   {OpC6M1},   {OpC7M1},
	{OpC8X1},   {OpC9M1},   {OpCAX1}, {OpCB},   {OpCCX1},   {OpCDM1},   {OpCEM1},   {OpCFM1},
	{OpD0E1},   {OpD1E1},   {OpD2E1}, {OpD3M1}, {OpD4E1},   {OpD5E1},   {OpD6E1},   {OpD7M1},
	{OpD8},     {OpD9M1X1}, {OpDAE1}, {OpDB},   {OpDC},     {OpDDM1X1}, {OpDEM1X1}, {OpDFM1},
	{OpE0X1},   {OpE1E1},   {OpE2},   {OpE3M1}, {OpE4X1},   {OpE5M1},   {OpE6M1},   {OpE7M1},
	{OpE8X1},   {OpE9M1},   {OpEA},   {OpEB},   {OpECX1},   {OpEDM1},   {OpEEM1},   {OpEFM1},
	{OpF0E1},   {OpF1E1},   {OpF2E1}, {OpF3M1}, {OpF4E1},   {OpF5E1},   {OpF6E1},   {OpF7M1},
	{OpF8},     {OpF9M1X1}, {OpFAE1}, {OpFB},   {OpFCE1},   {OpFDM1X1}, {OpFEM1X1}, {OpFFM1}
};
)

SOpcodes OpcodesM1X1[256] =
{
	{Op00},     {Op01E0M1}, {Op02},     {Op03M1}, {Op04M1},   {Op05M1},   {Op06M1},   {Op07M1},
	{Op08E0},   {Op09M1},   {Op0AM1},   {Op0BE0}, {Op0CM1},   {Op0DM1},   {Op0EM1},   {Op0FM1},
	{Op10E0},   {Op11E0M1}, {Op12E0M1}, {Op13M1}, {Op14M1},   {Op15E0M1}, {Op16E0M1}, {Op17M1},
	{Op18},     {Op19M1X1}, {Op1AM1},   {Op1B},   {Op1CM1},   {Op1DM1X1}, {Op1EM1X1}, {Op1FM1},
	{Op20E0},   {Op21E0M1}, {Op22E0},   {Op23M1}, {Op24M1},   {Op25M1},   {Op26M1},   {Op27M1},
	{Op28E0},   {Op29M1},   {Op2AM1},   {Op2BE0}, {Op2CM1},   {Op2DM1},   {Op2EM1},   {Op2FM1},
	{Op30E0},   {Op31E0M1}, {Op32E0M1}, {Op33M1}, {Op34E0M1}, {Op35E0M1}, {Op36E0M1}, {Op37M1},
	{Op38},     {Op39M1X1}, {Op3AM1},   {Op3B},   {Op3CM1X1}, {Op3DM1X1}, {Op3EM1X1}, {Op3FM1},
	{Op40Slow}, {Op41E0M1}, {Op42},     {Op43M1}, {Op44X1},   {Op45M1},   {Op46M1},   {Op47M1},
	{Op48E0M1}, {Op49M1},   {Op4AM1},   {Op4BE0}, {Op4C},     {Op4DM1},   {Op4EM1},   {Op4FM1},
	{Op50E0},   {Op51E0M1}, {Op52E0M1}, {Op53M1}, {Op54X1},   {Op55E0M1}, {Op56E0M1}, {Op57M1},
	{Op58},     {Op59M1X1}, {Op5AE0X1}, {Op5B},   {Op5C},     {Op5DM1X1}, {Op5EM1X1}, {Op5FM1},
	{Op60E0},   {Op61E0M1}, {Op62E0},   {Op63M1}, {Op64M1},   {Op65M1},   {Op66M1},   {Op67M1},
	{Op68E0M1}, {Op69M1},   {Op6AM1},   {Op6BE0}, {Op6C},     {Op6DM1},   {Op6EM1},   {Op6FM1},
	{Op70E0},   {Op71E0M1}, {Op72E0M1}, {Op73M1}, {Op74E0M1}, {Op75E0M1}, {Op76E0M1}, {Op77M1},
	{Op78},     {Op79M1X1}, {Op7AE0X1}, {Op7B},   {Op7C},     {Op7DM1X1}, {Op7EM1X1}, {Op7FM1},
	{Op80E0},   {Op81E0M1}, {Op82},     {Op83M1}, {Op84X1},   {Op85M1},   {Op86X1},   {Op87M1},
	{Op88X1},   {Op89M1},   {Op8AM1},   {Op8BE0}, {Op8CX1},   {Op8DM1},   {Op8EX1},   {Op8FM1},
	{Op90E0},   {Op91E0M1}, {Op92E0M1}, {Op93M1}, {Op94E0X1}, {Op95E0M1}, {Op96E0X1}, {Op97M1},
	{Op98M1},   {Op99M1X1}, {Op9A},     {Op9BX1}, {Op9CM1},   {Op9DM1X1}, {Op9EM1X1}, {Op9FM1},
	{OpA0X1},   {OpA1E0M1}, {OpA2X1},   {OpA3M1}, {OpA4X1},   {OpA5M1},   {OpA6X1},   {OpA7M1},
	{OpA8X1},   {OpA9M1},   {OpAAX1},   {OpABE0}, {OpACX1},   {OpADM1},   {OpAEX1},   {OpAFM1},
	{OpB0E0},   {OpB1E0M1}, {OpB2E0M1}, {OpB3M1}, {OpB4E0X1}, {OpB5E0M1}, {OpB6E0X1}, {OpB7M1},
	{OpB8},     {OpB9M1X1}, {OpBAX1},   {OpBBX1}, {OpBCX1},   {OpBDM1X1}, {OpBEX1},   {OpBFM1},
	{OpC0X1},   {OpC1E0M1}, {OpC2},     {OpC3M1}, {OpC4X1},   {OpC5M1},   {OpC6M1},   {OpC7M1},
	{OpC8X1},   {OpC9M1},   {OpCAX1},   {OpCB},   {OpCCX1},   {OpCDM1},   {OpCEM1},   {OpCFM1},
	{OpD0E0},   {OpD1E0M1}, {OpD2E0M1}, {OpD3M1}, {OpD4E0},   {OpD5E0M1}, {OpD6E0M1}, {OpD7M1},
	{OpD8},     {OpD9M1X1}, {OpDAE0X1}, {OpDB},   {OpDC},     {OpDDM1X1}, {OpDEM1X1}, {OpDFM1},
	{OpE0X1},   {OpE1E0M1}, {OpE2},     {OpE3M1}, {OpE4X1},   {OpE5M1},   {OpE6M1},   {OpE7M1},
	{OpE8X1},   {OpE9M1},   {OpEA},     {OpEB},   {OpECX1},   {OpEDM1},   {OpEEM1},   {OpEFM1},
	{OpF0E0},   {OpF1E0M1}, {OpF2E0M1}, {OpF3M1}, {OpF4E0},   {OpF5E0M1}, {OpF6E0M1}, {OpF7M1},
	{OpF8},     {OpF9M1X1}, {OpFAE0X1}, {OpFB},   {OpFCE0},   {OpFDM1X1}, {OpFEM1X1}, {OpFFM1}
};

SOpcodes OpcodesM1X0[256] =
{
	{Op00},     {Op01E0M1}, {Op02},     {Op03M1}, {Op04M1},   {Op05M1},   {Op06M1},   {Op07M1},
	{Op08E0},   {Op09M1},   {Op0AM1},   {Op0BE0}, {Op0CM1},   {Op0DM1},   {Op0EM1},   {Op0FM1},
	{Op10E0},   {Op11E0M1}, {Op12E0M1}, {Op13M1}, {Op14M1},   {Op15E0M1}, {Op16E0M1}, {Op17M1},
	{Op18},     {Op19M1X0}, {Op1AM1},   {Op1B},   {Op1CM1},   {Op1DM1X0}, {Op1EM1X0}, {Op1FM1},
	{Op20E0},   {Op21E0M1}, {Op22E0},   {Op23M1}, {Op24M1},   {Op25M1},   {Op26M1},   {Op27M1},
	{Op28E0},   {Op29M1},   {Op2AM1},   {Op2BE0}, {Op2CM1},   {Op2DM1},   {Op2EM1},   {Op2FM1},
	{Op30E0},   {Op31E0M1}, {Op32E0M1}, {Op33M1}, {Op34E0M1}, {Op35E0M1}, {Op36E0M1}, {Op37M1},
	{Op38},     {Op39M1X0}, {Op3AM1},   {Op3B},   {Op3CM1X0}, {Op3DM1X0}, {Op3EM1X0}, {Op3FM1},
	{Op40Slow}, {Op41E0M1}, {Op42},     {Op43M1}, {Op44X0},   {Op45M1},   {Op46M1},   {Op47M1},
	{Op48E0M1}, {Op49M1},   {Op4AM1},   {Op4BE0}, {Op4C},     {Op4DM1},   {Op4EM1},   {Op4FM1},
	{Op50E0},   {Op51E0M1}, {Op52E0M1}, {Op53M1}, {Op54X0},   {Op55E0M1}, {Op56E0M1}, {Op57M1},
	{Op58},     {Op59M1X0}, {Op5AE0X0}, {Op5B},   {Op5C},     {Op5DM1X0}, {Op5EM1X0}, {Op5FM1},
	{Op60E0},   {Op61E0M1}, {Op62E0},   {Op63M1}, {Op64M1},   {Op65M1},   {Op66M1},   {Op67M1},
	{Op68E0M1}, {Op69M1},   {Op6AM1},   {Op6BE0}, {Op6C},     {Op6DM1},   {Op6EM1},   {Op6FM1},
	{Op70E0},   {Op71E0M1}, {Op72E0M1}, {Op73M1}, {Op74E0M1}, {Op75E0M1}, {Op76E0M1}, {Op77M1},
	{Op78},     {Op79M1X0}, {Op7AE0X0}, {Op7B},   {Op7C},     {Op7DM1X0}, {Op7EM1X0}, {Op7FM1},
	{Op80E0},   {Op81E0M1}, {Op82},     {Op83M1}, {Op84X0},   {Op85M1},   {Op86X0},   {Op87M1},
	{Op88X0},   {Op89M1},   {Op8AM1},   {Op8BE0}, {Op8CX0},   {Op8DM1},   {Op8EX0},   {Op8FM1},
	{Op90E0},   {Op91E0M1}, {Op92E0M1}, {Op93M1}, {Op94E0X0}, {Op95E0M1}, {Op96E0X0}, {Op97M1},
	{Op98M1},   {Op99M1X0}, {Op9A},     {Op9BX0}, {Op9CM1},   {Op9DM1X0}, {Op9EM1X0}, {Op9FM1},
	{OpA0X0},   {OpA1E0M1}, {OpA2X0},   {OpA3M1}, {OpA4X0},   {OpA5M1},   {OpA6X0},   {OpA7M1},
	{OpA8X0},   {OpA9M1},   {OpAAX0},   {OpABE0}, {OpACX0},   {OpADM1},   {OpAEX0},   {OpAFM1},
	{OpB0E0},   {OpB1E0M1}, {OpB2E0M1}, {OpB3M1}, {OpB4E0X0}, {OpB5E0M1}, {OpB6E0X0}, {OpB7M1},
	{OpB8},     {OpB9M1X0}, {OpBAX0},   {OpBBX0}, {OpBCX0},   {OpBDM1X0}, {OpBEX0},   {OpBFM1},
	{OpC0X0},   {OpC1E0M1}, {OpC2},     {OpC3M1}, {OpC4X0},   {OpC5M1},   {OpC6M1},   {OpC7M1},
	{OpC8X0},   {OpC9M1},   {OpCAX0},   {OpCB},   {OpCCX0},   {OpCDM1},   {OpCEM1},   {OpCFM1},
	{OpD0E0},   {OpD1E0M1}, {OpD2E0M1}, {OpD3M1}, {OpD4E0},   {OpD5E0M1}, {OpD6E0M1}, {OpD7M1},
	{OpD8},     {OpD9M1X0}, {OpDAE0X0}, {OpDB},   {OpDC},     {OpDDM1X0}, {OpDEM1X0}, {OpDFM1},
	{OpE0X0},   {OpE1E0M1}, {OpE2},     {OpE3M1}, {OpE4X0},   {OpE5M1},   {OpE6M1},   {OpE7M1},
	{OpE8X0},   {OpE9M1},   {OpEA},     {OpEB},   {OpECX0},   {OpEDM1},   {OpEEM1},   {OpEFM1},
	{OpF0E0},   {OpF1E0M1}, {OpF2E0M1}, {OpF3M1}, {OpF4E0},   {OpF5E0M1}, {OpF6E0M1}, {OpF7M1},
	{OpF8},     {OpF9M1X0}, {OpFAE0X0}, {OpFB},   {OpFCE0},   {OpFDM1X0}, {OpFEM1X0}, {OpFFM1}
};

SOpcodes OpcodesM0X0[256] =
{
	{Op00},     {Op01E0M0}, {Op02},     {Op03M0}, {Op04M0},   {Op05M0},   {Op06M0},   {Op07M0},
	{Op08E0},   {Op09M0},   {Op0AM0},   {Op0BE0}, {Op0CM0},   {Op0DM0},   {Op0EM0},   {Op0FM0},
	{Op10E0},   {Op11E0M0}, {Op12E0M0}, {Op13M0}, {Op14M0},   {Op15E0M0}, {Op16E0M0}, {Op17M0},
	{Op18},     {Op19M0X0}, {Op1AM0},   {Op1B},   {Op1CM0},   {Op1DM0X0}, {Op1EM0X0}, {Op1FM0},
	{Op20E0},   {Op21E0M0}, {Op22E0},   {Op23M0}, {Op24M0},   {Op25M0},   {Op26M0},   {Op27M0},
	{Op28E0},   {Op29M0},   {Op2AM0},   {Op2BE0}, {Op2CM0},   {Op2DM0},   {Op2EM0},   {Op2FM0},
	{Op30E0},   {Op31E0M0}, {Op32E0M0}, {Op33M0}, {Op34E0M0}, {Op35E0M0}, {Op36E0M0}, {Op37M0},
	{Op38},     {Op39M0X0}, {Op3AM0},   {Op3B},   {Op3CM0X0}, {Op3DM0X0}, {Op3EM0X0}, {Op3FM0},
	{Op40Slow}, {Op41E0M0}, {Op42},     {Op43M0}, {Op44X0},   {Op45M0},   {Op46M0},   {Op47M0},
	{Op48E0M0}, {Op49M0},   {Op4AM0},   {Op4BE0}, {Op4C},     {Op4DM0},   {Op4EM0},   {Op4FM0},
	{Op50E0},   {Op51E0M0}, {Op52E0M0}, {Op53M0}, {Op54X0},   {Op55E0M0}, {Op56E0M0}, {Op57M0},
	{Op58},     {Op59M0X0}, {Op5AE0X0}, {Op5B},   {Op5C},     {Op5DM0X0}, {Op5EM0X0}, {Op5FM0},
	{Op60E0},   {Op61E0M0}, {Op62E0},   {Op63M0}, {Op64M0},   {Op65M0},   {Op66M0},   {Op67M0},
	{Op68E0M0}, {Op69M0},   {Op6AM0},   {Op6BE0}, {Op6C},     {Op6DM0},   {Op6EM0},   {Op6FM0},
	{Op70E0},   {Op71E0M0}, {Op72E0M0}, {Op73M0}, {Op74E0M0}, {Op75E0M0}, {Op76E0M0}, {Op77M0},
	{Op78},     {Op79M0X0}, {Op7AE0X0}, {Op7B},   {Op7C},     {Op7DM0X0}, {Op7EM0X0}, {Op7FM0},
	{Op80E0},   {Op81E0M0}, {Op82},     {Op83M0}, {Op84X0},   {Op85M0},   {Op86X0},   {Op87M0},
	{Op88X0},   {Op89M0},   {Op8AM0},   {Op8BE0}, {Op8CX0},   {Op8DM0},   {Op8EX0},   {Op8FM0},
	{Op90E0},   {Op91E0M0}, {Op92E0M0}, {Op93M0}, {Op94E0X0}, {Op95E0M0}, {Op96E0X0}, {Op97M0},
	{Op98M0},   {Op99M0X0}, {Op9A},     {Op9BX0}, {Op9CM0},   {Op9DM0X0}, {Op9EM0X0}, {Op9FM0},
	{OpA0X0},   {OpA1E0M0}, {OpA2X0},   {OpA3M0}, {OpA4X0},   {OpA5M0},   {OpA6X0},   {OpA7M0},
	{OpA8X0},   {OpA9M0},   {OpAAX0},   {OpABE0}, {OpACX0},   {OpADM0},   {OpAEX0},   {OpAFM0},
	{OpB0E0},   {OpB1E0M0}, {OpB2E0M0}, {OpB3M0}, {OpB4E0X0}, {OpB5E0M0}, {OpB6E0X0}, {OpB7M0},
	{OpB8},     {OpB9M0X0}, {OpBAX0},   {OpBBX0}, {OpBCX0},   {OpBDM0X0}, {OpBEX0},   {OpBFM0},
	{OpC0X0},   {OpC1E0M0}, {OpC2},     {OpC3M0}, {OpC4X0},   {OpC5M0},   {OpC6M0},   {OpC7M0},
	{OpC8X0},   {OpC9M0},   {OpCAX0},   {OpCB},   {OpCCX0},   {OpCDM0},   {OpCEM0},   {OpCFM0},
	{OpD0E0},   {OpD1E0M0}, {OpD2E0M0}, {OpD3M0}, {OpD4E0},   {OpD5E0M0}, {OpD6E0M0}, {OpD7M0},
	{OpD8},     {OpD9M0X0}, {OpDAE0X0}, {OpDB},   {OpDC},     {OpDDM0X0}, {OpDEM0X0}, {OpDFM0},
	{OpE0X0},   {OpE1E0M0}, {OpE2},     {OpE3M0}, {OpE4X0},   {OpE5M0},   {OpE6M0},   {OpE7M0},
	{OpE8X0},   {OpE9M0},   {OpEA},     {OpEB},   {OpECX0},   {OpEDM0},   {OpEEM0},   {OpEFM0},
	{OpF0E0},   {OpF1E0M0}, {OpF2E0M0}, {OpF3M0}, {OpF4E0},   {OpF5E0M0}, {OpF6E0M0}, {OpF7M0},
	{OpF8},     {OpF9M0X0}, {OpFAE0X0}, {OpFB},   {OpFCE0},   {OpFDM0X0}, {OpFEM0X0}, {OpFFM0}
};

SOpcodes OpcodesM0X1[256] =
{
	{Op00},     {Op01E0M0}, {Op02},     {Op03M0}, {Op04M0},   {Op05M0},   {Op06M0},   {Op07M0},
	{Op08E0},   {Op09M0},   {Op0AM0},   {Op0BE0}, {Op0CM0},   {Op0DM0},   {Op0EM0},   {Op0FM0},
	{Op10E0},   {Op11E0M0}, {Op12E0M0}, {Op13M0}, {Op14M0},   {Op15E0M0}, {Op16E0M0}, {Op17M0},
	{Op18},     {Op19M0X1}, {Op1AM0},   {Op1B},   {Op1CM0},   {Op1DM0X1}, {Op1EM0X1}, {Op1FM0},
	{Op20E0},   {Op21E0M0}, {Op22E0},   {Op23M0}, {Op24M0},   {Op25M0},   {Op26M0},   {Op27M0},
	{Op28E0},   {Op29M0},   {Op2AM0},   {Op2BE0}, {Op2CM0},   {Op2DM0},   {Op2EM0},   {Op2FM0},
	{Op30E0},   {Op31E0M0}, {Op32E0M0}, {Op33M0}, {Op34E0M0}, {Op35E0M0}, {Op36E0M0}, {Op37M0},
	{Op38},     {Op39M0X1}, {Op3AM0},   {Op3B},   {Op3CM0X1}, {Op3DM0X1}, {Op3EM0X1}, {Op3FM0},
	{Op40Slow}, {Op41E0M0}, {Op42},     {Op43M0}, {Op44X1},   {Op45M0},   {Op46M0},   {Op47M0},
	{Op48E0M0}, {Op49M0},   {Op4AM0},   {Op4BE0}, {Op4C},     {Op4DM0},   {Op4EM0},   {Op4FM0},
	{Op50E0},   {Op51E0M0}, {Op52E0M0}, {Op53M0}, {Op54X1},   {Op55E0M0}, {Op56E0M0}, {Op57M0},
	{Op58},     {Op59M0X1}, {Op5AE0X1}, {Op5B},   {Op5C},     {Op5DM0X1}, {Op5EM0X1}, {Op5FM0},
	{Op60E0},   {Op61E0M0}, {Op62E0},   {Op63M0}, {Op64M0},   {Op65M0},   {Op66M0},   {Op67M0},
	{Op68E0M0}, {Op69M0},   {Op6AM0},   {Op6BE0}, {Op6C},     {Op6DM0},   {Op6EM0},   {Op6FM0},
	{Op70E0},   {Op71E0M0}, {Op72E0M0}, {Op73M0}, {Op74E0M0}, {Op75E0M0}, {Op76E0M0}, {Op77M0},
	{Op78},     {Op79M0X1}, {Op7AE0X1}, {Op7B},   {Op7C},     {Op7DM0X1}, {Op7EM0X1}, {Op7FM0},
	{Op80E0},   {Op81E0M0}, {Op82},     {Op83M0}, {Op84X1},   {Op85M0},   {Op86X1},   {Op87M0},
	{Op88X1},   {Op89M0},   {Op8AM0},   {Op8BE0}, {Op8CX1},   {Op8DM0},   {Op8EX1},   {Op8FM0},
	{Op90E0},   {Op91E0M0}, {Op92E0M0}, {Op93M0}, {Op94E0X1}, {Op95E0M0}, {Op96E0X1}, {Op97M0},
	{Op98M0},   {Op99M0X1}, {Op9A},     {Op9BX1}, {Op9CM0},   {Op9DM0X1}, {Op9EM0X1}, {Op9FM0},
	{OpA0X1},   {OpA1E0M0}, {OpA2X1},   {OpA3M0}, {OpA4X1},   {OpA5M0},   {OpA6X1},   {OpA7M0},
	{OpA8X1},   {OpA9M0},   {OpAAX1},   {OpABE0}, {OpACX1},   {OpADM0},   {OpAEX1},   {OpAFM0},
	{OpB0E0},   {OpB1E0M0}, {OpB2E0M0}, {OpB3M0}, {OpB4E0X1}, {OpB5E0M0}, {OpB6E0X1}, {OpB7M0},
	{OpB8},     {OpB9M0X1}, {OpBAX1},   {OpBBX1}, {OpBCX1},   {OpBDM0X1}, {OpBEX1},   {OpBFM0},
	{OpC0X1},   {OpC1E0M0}, {OpC2},     {OpC3M0}, {OpC4X1},   {OpC5M0},   {OpC6M0},   {OpC7M0},
	{OpC8X1},   {OpC9M0},   {OpCAX1},   {OpCB},   {OpCCX1},   {OpCDM0},   {OpCEM0},   {OpCFM0},
	{OpD0E0},   {OpD1E0M0}, {OpD2E0M0}, {OpD3M0}, {OpD4E0},   {OpD5E0M0}, {OpD6E0M0}, {OpD7M0},
	{OpD8},     {OpD9M0X1}, {OpDAE0X1}, {OpDB},   {OpDC},     {OpDDM0X1}, {OpDEM0X1}, {OpDFM0},
	{OpE0X1},   {OpE1E0M0}, {OpE2},     {OpE3M0}, {OpE4X1},   {OpE5M0},   {OpE6M0},   {OpE7M0},
	{OpE8X1},   {OpE9M0},   {OpEA},     {OpEB},   {OpECX1},   {OpEDM0},   {OpEEM0},   {OpEFM0},
	{OpF0E0},   {OpF1E0M0}, {OpF2E0M0}, {OpF3M0}, {OpF4E0},   {OpF5E0M0}, {OpF6E0M0}, {OpF7M0},
	{OpF8},     {OpF9M0X1}, {OpFAE0X1}, {OpFB},   {OpFCE0},   {OpFDM0X1}, {OpFEM0X1}, {OpFFM0}
};

SOpcodes OpcodesSlow[256] =
{
	{Op00},     {Op01Slow}, {Op02},     {Op03Slow}, {Op04Slow}, {Op05Slow}, {Op06Slow}, {Op07Slow},
	{Op08Slow}, {Op09Slow}, {Op0ASlow}, {Op0BSlow}, {Op0CSlow}, {Op0DSlow}, {Op0ESlow}, {Op0FSlow},
	{Op10Slow}, {Op11Slow}, {Op12Slow}, {Op13Slow}, {Op14Slow}, {Op15Slow}, {Op16Slow}, {Op17Slow},
	{Op18},     {Op19Slow}, {Op1ASlow}, {Op1B},     {Op1CSlow}, {Op1DSlow}, {Op1ESlow}, {Op1FSlow},
	{Op20Slow}, {Op21Slow}, {Op22Slow}, {Op23Slow}, {Op24Slow}, {Op25Slow}, {Op26Slow}, {Op27Slow},
	{Op28Slow}, {Op29Slow}, {Op2ASlow}, {Op2BSlow}, {Op2CSlow}, {Op2DSlow}, {Op2ESlow}, {Op2FSlow},
	{Op30Slow}, {Op31Slow}, {Op32Slow}, {Op33Slow}, {Op34Slow}, {Op35Slow}, {Op36Slow}, {Op37Slow},
	{Op38},     {Op39Slow}, {Op3ASlow}, {Op3B},     {Op3CSlow}, {Op3DSlow}, {Op3ESlow}, {Op3FSlow},
	{Op40Slow}, {Op41Slow}, {Op42},     {Op43Slow}, {Op44Slow}, {Op45Slow}, {Op46Slow}, {Op47Slow},
	{Op48Slow}, {Op49Slow}, {Op4ASlow}, {Op4BSlow}, {Op4CSlow}, {Op4DSlow}, {Op4ESlow}, {Op4FSlow},
	{Op50Slow}, {Op51Slow}, {Op52Slow}, {Op53Slow}, {Op54Slow}, {Op55Slow}, {Op56Slow}, {Op57Slow},
	{Op58},     {Op59Slow}, {Op5ASlow}, {Op5B},     {Op5CSlow}, {Op5DSlow}, {Op5ESlow}, {Op5FSlow},
	{Op60Slow}, {Op61Slow}, {Op62Slow}, {Op63Slow}, {Op64Slow}, {Op65Slow}, {Op66Slow}, {Op67Slow},
	{Op68Slow}, {Op69Slow}, {Op6ASlow}, {Op6BSlow}, {Op6CSlow}, {Op6DSlow}, {Op6ESlow}, {Op6FSlow},
	{Op70Slow}, {Op71Slow}, {Op72Slow}, {Op73Slow}, {Op74Slow}, {Op75Slow}, {Op76Slow}, {Op77Slow},
	{Op78},     {Op79Slow}, {Op7ASlow}, {Op7B},     {Op7CSlow}, {Op7DSlow}, {Op7ESlow}, {Op7FSlow},
	{Op80Slow}, {Op81Slow}, {Op82Slow}, {Op83Slow}, {Op84Slow}, {Op85Slow}, {Op86Slow}, {Op87Slow},
	{Op88Slow}, {Op89Slow}, {Op8ASlow}, {Op8BSlow}, {Op8CSlow}, {Op8DSlow}, {Op8ESlow}, {Op8FSlow},
	{Op90Slow}, {Op91Slow}, {Op92Slow}, {Op93Slow}, {Op94Slow}, {Op95Slow}, {Op96Slow}, {Op97Slow},
	{Op98Slow}, {Op99Slow}, {Op9A},     {Op9BSlow}, {Op9CSlow}, {Op9DSlow}, {Op9ESlow}, {Op9FSlow},
	{OpA0Slow}, {OpA1Slow}, {OpA2Slow}, {OpA3Slow}, {OpA4Slow}, {OpA5Slow}, {OpA6Slow}, {OpA7Slow},
	{OpA8Slow}, {OpA9Slow}, {OpAASlow}, {OpABSlow}, {OpACSlow}, {OpADSlow}, {OpAESlow}, {OpAFSlow},
	{OpB0Slow}, {OpB1Slow}, {OpB2Slow}, {OpB3Slow}, {OpB4Slow}, {OpB5Slow}, {OpB6Slow}, {OpB7Slow},
	{OpB8},     {OpB9Slow}, {OpBASlow}, {OpBBSlow}, {OpBCSlow}, {OpBDSlow}, {OpBESlow}, {OpBFSlow},
	{OpC0Slow}, {OpC1Slow}, {OpC2Slow}, {OpC3Slow}, {OpC4Slow}, {OpC5Slow}, {OpC6Slow}, {OpC7Slow},
	{OpC8Slow}, {OpC9Slow}, {OpCASlow}, {OpCB},     {OpCCSlow}, {OpCDSlow}, {OpCESlow}, {OpCFSlow},
	{OpD0Slow}, {OpD1Slow}, {OpD2Slow}, {OpD3Slow}, {OpD4Slow}, {OpD5Slow}, {OpD6Slow}, {OpD7Slow},
	{OpD8},     {OpD9Slow}, {OpDASlow}, {OpDB},     {OpDCSlow}, {OpDDSlow}, {OpDESlow}, {OpDFSlow},
	{OpE0Slow}, {OpE1Slow}, {OpE2Slow}, {OpE3Slow}, {OpE4Slow}, {OpE5Slow}, {OpE6Slow}, {OpE7Slow},
	{OpE8Slow}, {OpE9Slow}, {OpEA},     {OpEB},     {OpECSlow}, {OpEDSlow}, {OpEESlow}, {OpEFSlow},
	{OpF0Slow}, {OpF1Slow}, {OpF2Slow}, {OpF3Slow}, {OpF4Slow}, {OpF5Slow}, {OpF6Slow}, {OpF7Slow},
	{OpF8},     {OpF9Slow}, {OpFASlow}, {OpFB},     {OpFCSlow}, {OpFDSlow}, {OpFESlow}, {OpFFSlow}
};
