
#include "cputest.h"
#include "cputbl_test.h"
#include "readcpu.h"
#include "disasm.h"
#include "ini.h"
#include "fpp.h"

#include "options.h"

#define MAX_REGISTERS 16

static uae_u32 registers[] =
{
	0x00000010,
	0x00000000,
	0xffffffff,
	0xffffff00,
	0xffff0000,
	0x80008080,
	0x7fff7fff,
	0xaaaaaaaa,

	0x00000000,
	0x00000080,
	0x00008000,
	0x00007fff,
	0xfffffffe,
	0xffffff00,
	0x00000000, // replaced with opcode memory
	0x00000000  // replaced with stack
};

static floatx80 fpuregisters[8];
static uae_u32 fpu_fpiar, fpu_fpcr, fpu_fpsr;

const int areg_byteinc[] = { 1, 1, 1, 1, 1, 1, 1, 2 };
const int imm8_table[] = { 8, 1, 2, 3, 4, 5, 6, 7 };

int movem_index1[256];
int movem_index2[256];
int movem_next[256];
int bus_error_offset;
int cpu_bus_error;

struct mmufixup mmufixup[2];
cpuop_func *cpufunctbl[65536];
struct cputbl_data
{
	uae_s16 length;
	uae_s8 disp020[2];
	uae_u8 branch;
};
static struct cputbl_data cpudatatbl[65536];

struct regstruct regs;
struct flag_struct regflags;

static int verbose = 1;
static int feature_exception3_data = 0;
static int feature_exception3_instruction = 0;
static int feature_sr_mask = 0;
static int feature_min_interrupt_mask = 0;
static int feature_loop_mode = 0;
static int feature_loop_mode_register = -1;
static int feature_full_extension_format = 0;
static int feature_test_rounds = 2;
static int feature_flag_mode = 0;
static TCHAR *feature_instruction_size = NULL;
static uae_u32 feature_addressing_modes[2];
static int ad8r[2], pc8r[2];
static int multi_mode;
#define MAX_TARGET_EA 8
static uae_u32 feature_target_ea[MAX_TARGET_EA][2];
static int target_ea_src_cnt, target_ea_dst_cnt;
static int target_ea_src_max, target_ea_dst_max;
static uae_u32 target_ea[2];

#define HIGH_MEMORY_START (addressing_mask == 0xffffffff ? 0xffff8000 : 0x00ff8000)

// large enough for RTD
#define STACK_SIZE (0x8000 + 8)
#define RESERVED_SUPERSTACK 1024
// space for extra exception, not part of test region
#define EXTRA_RESERVED_SPACE 1024

static uae_u32 test_low_memory_start;
static uae_u32 test_low_memory_end;
static uae_u32 test_high_memory_start;
static uae_u32 test_high_memory_end;
static uae_u32 low_memory_size = 32768;
static uae_u32 high_memory_size = 32768;
static uae_u32 safe_memory_start;
static uae_u32 safe_memory_end;

static uae_u8 *low_memory, *high_memory, *test_memory;
static uae_u8 *low_memory_temp, *high_memory_temp, *test_memory_temp;
static uae_u8 dummy_memory[4];
static uaecptr test_memory_start, test_memory_end, opcode_memory_start;
static uae_u32 test_memory_size;
static int hmem_rom, lmem_rom;
static uae_u8 *opcode_memory;
static uae_u8 *storage_buffer;
static char inst_name[16+1];
static int storage_buffer_watermark;
static int max_storage_buffer = 1000000;

static bool out_of_test_space;
static uaecptr out_of_test_space_addr;
static int forced_immediate_mode;
static int test_exception;
static int exception_stack_frame_size;
static uaecptr test_exception_addr;
static int test_exception_3_w;
static int test_exception_3_fc;
static int test_exception_opcode;

static uae_u8 imm8_cnt;
static uae_u16 imm16_cnt;
static uae_u32 imm32_cnt;
static uae_u32 immabsl_cnt;
static uae_u32 addressing_mask;
static int opcodecnt;
static int cpu_stopped;
static int cpu_halted;
static int cpu_lvl = 0;
static int test_count;
static int testing_active;
static uae_u16 testing_active_opcode;
static time_t starttime;
static int filecount;
static uae_u16 sr_undefined_mask;
static int low_memory_accessed;
static int high_memory_accessed;
static int test_memory_accessed;
static uae_u16 extra_or, extra_and;
static uae_u32 cur_registers[MAX_REGISTERS];

struct uae_prefs currprefs;

struct accesshistory
{
	uaecptr addr;
	uae_u32 val;
	uae_u32 oldval;
	int size;
};
static int ahcnt, ahcnt2;
static int noaccesshistory = 0;

#define MAX_ACCESSHIST 48
static struct accesshistory ahist[MAX_ACCESSHIST];
static struct accesshistory ahist2[MAX_ACCESSHIST];

static int is_superstack_use_required(void)
{
	switch (testing_active_opcode)
	{
	case 0x4e73: // RTE
		return 1;
	}
	return 0;
}

#define OPCODE_AREA 32

static bool valid_address(uaecptr addr, int size, int w)
{
	addr &= addressing_mask;
	size--;
	if (addr + size < low_memory_size) {
		if (addr < test_low_memory_start || test_low_memory_start == 0xffffffff)
			goto oob;
		// exception vectors needed during tests
		if ((addr + size >= 0x08 && addr < 0x30 || (addr + size >= 0x80 && addr < 0xc0)) && regs.vbr == 0)
			goto oob;
		if (addr + size >= test_low_memory_end)
			goto oob;
		if (w && lmem_rom)
			goto oob;
		low_memory_accessed = w ? -1 : 1;
		return 1;
	}
	if (addr >= HIGH_MEMORY_START && addr <= HIGH_MEMORY_START + 0x7fff) {
		if (addr < test_high_memory_start || test_high_memory_start == 0xffffffff)
			goto oob;
		if (addr + size >= test_high_memory_end)
			goto oob;
		if (w && hmem_rom)
			goto oob;
		high_memory_accessed = w ? -1 : 1;
		return 1;
	}
	if (addr >= test_memory_end && addr + size < test_memory_end + EXTRA_RESERVED_SPACE) {
		if (testing_active < 0)
			return 1;
	}
	if (addr >= test_memory_start && addr + size < test_memory_end - RESERVED_SUPERSTACK) {
		// make sure we don't modify our test instruction
		if (testing_active && w) {
			if (addr >= opcode_memory_start && addr + size < opcode_memory_start + OPCODE_AREA)
				goto oob;
		}
		test_memory_accessed = w ? -1 : 1;
		return 1;
	}
	if (addr >= test_memory_end - RESERVED_SUPERSTACK && addr + size < test_memory_end) {
		// allow only instructions that have to access super stack, for example RTE
		// read-only
		if (w)
			goto oob;
		if (testing_active) {
			if (is_superstack_use_required()) {
				test_memory_accessed = 1;
				return 1;
			}
		}
	}
oob:
	return 0;
}

static bool is_nowrite_address(uaecptr addr, int size)
{
	return addr + size >= safe_memory_start && addr < safe_memory_end;
}

static void validate_addr(uaecptr addr, int size)
{
	if (valid_address(addr, size, 0))
		return;
	wprintf(_T("Trying to store invalid memory address %08x!?\n"), addr);
	abort();
}


static uae_u8 *get_addr(uaecptr addr, int size, int w)
{
	// allow debug output to read memory even if oob condition
	if (w >= 0 && out_of_test_space)
		goto oob;
	if (!valid_address(addr, 1, w))
		goto oob;
	if (size > 1) {
		if (!valid_address(addr + size - 1, 1, w))
			goto oob;
	}
	addr &= addressing_mask;
	size--;
	if (addr + size < low_memory_size) {
		return low_memory + addr;
	} else if (addr >= HIGH_MEMORY_START && addr <= HIGH_MEMORY_START + 0x7fff) {
		return high_memory + (addr - HIGH_MEMORY_START);
	} else if (addr >= test_memory_start && addr + size < test_memory_end + EXTRA_RESERVED_SPACE) {
		return test_memory + (addr - test_memory_start);
	}
oob:
	if (w >= 0) {
		if (!out_of_test_space) {
			out_of_test_space = true;
			out_of_test_space_addr = addr;
		}
	}
	dummy_memory[0] = 0;
	dummy_memory[1] = 0;
	dummy_memory[2] = 0;
	dummy_memory[3] = 0;
	return dummy_memory;
}

static void check_bus_error(uaecptr addr, int write, int fc)
{
	if (!testing_active)
		return;
	if (safe_memory_start == 0xffffffff && safe_memory_end == 0xffffffff)
		return;
	if (addr >= safe_memory_start && addr < safe_memory_end) {
		cpu_bus_error = 1;
	}
}

static uae_u16 get_iword_test(uaecptr addr)
{
	check_bus_error(addr, 0, regs.s ? 6 : 2);
	if (addr & 1) {
		return (get_byte_test(addr + 0) << 8) | (get_byte_test(addr + 1) << 0);
	} else {
		uae_u8 *p = get_addr(addr, 2, 0);
		return (p[0] << 8) | (p[1]);
	}
}

uae_u16 get_word_test_prefetch(int o)
{
	// no real prefetch
	if (cpu_lvl < 2)
		o -= 2;
	regs.irc = get_iword_test(m68k_getpci() + o + 2);
	return get_iword_test(m68k_getpci() + o);
}

// Move from SR does two writes to same address:
// ignore the first one
static void previoussame(uaecptr addr, int size)
{
	if (!ahcnt)
		return;
	struct accesshistory *ah = &ahist[ahcnt - 1];
	if (ah->addr == addr && ah->size == size) {
		ahcnt--;
	}
}

void put_byte_test(uaecptr addr, uae_u32 v)
{
	check_bus_error(addr, 1, regs.s ? 5 : 1);
	uae_u8 *p = get_addr(addr, 1, 1);
	if (!out_of_test_space && !noaccesshistory && !cpu_bus_error) {
		previoussame(addr, sz_byte);
		if (ahcnt >= MAX_ACCESSHIST) {
			wprintf(_T("ahist overflow!"));
			abort();
		}
		struct accesshistory *ah = &ahist[ahcnt++];
		ah->addr = addr;
		ah->val = v & 0xff;
		ah->oldval = *p;
		ah->size = sz_byte;
	}
	*p = v;
}
void put_word_test(uaecptr addr, uae_u32 v)
{
	check_bus_error(addr, 1, regs.s ? 5 : 1);
	if (addr & 1) {
		put_byte_test(addr + 0, v >> 8);
		put_byte_test(addr + 1, v >> 0);
	} else {
		uae_u8 *p = get_addr(addr, 2, 1);
		if (!out_of_test_space && !noaccesshistory && !cpu_bus_error) {
			previoussame(addr, sz_word);
			if (ahcnt >= MAX_ACCESSHIST) {
				wprintf(_T("ahist overflow!"));
				abort();
			}
			struct accesshistory *ah = &ahist[ahcnt++];
			ah->addr = addr;
			ah->val = v & 0xffff;
			ah->oldval = (p[0] << 8) | p[1];
			ah->size = sz_word;
		}
		p[0] = v >> 8;
		p[1] = v & 0xff;
	}
}
void put_long_test(uaecptr addr, uae_u32 v)
{
	check_bus_error(addr, 1, regs.s ? 5 : 1);
	if (addr & 1) {
		put_byte_test(addr + 0, v >> 24);
		put_word_test(addr + 1, v >> 8);
		put_byte_test(addr + 3, v >> 0);
	} else if (addr & 2) {
		put_word_test(addr + 0, v >> 16);
		put_word_test(addr + 2, v >> 0);
	} else {
		uae_u8 *p = get_addr(addr, 4, 1);
		if (!out_of_test_space && !noaccesshistory && !cpu_bus_error) {
			previoussame(addr, sz_long);
			if (ahcnt >= MAX_ACCESSHIST) {
				wprintf(_T("ahist overflow!"));
				abort();
			}
			struct accesshistory *ah = &ahist[ahcnt++];
			ah->addr = addr;
			ah->val = v;
			ah->oldval = (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
			ah->size = sz_long;
		}
		p[0] = v >> 24;
		p[1] = v >> 16;
		p[2] = v >> 8;
		p[3] = v >> 0;
	}
}


static void undo_memory(struct accesshistory *ahp, int  *cntp)
{
	out_of_test_space = 0;
	int cnt = *cntp;
	noaccesshistory = 1;
	for (int i = cnt - 1; i >= 0; i--) {
		struct accesshistory *ah = &ahp[i];
		switch (ah->size)
		{
		case sz_byte:
			put_byte_test(ah->addr, ah->oldval);
			break;
		case sz_word:
			put_word_test(ah->addr, ah->oldval);
			break;
		case sz_long:
			put_long_test(ah->addr, ah->oldval);
			break;
		}
	}
	noaccesshistory = 0;
	if (out_of_test_space) {
		wprintf(_T("undo_memory out of test space fault!?\n"));
		abort();
	}
}


uae_u32 get_byte_test(uaecptr addr)
{
	check_bus_error(addr, 0, regs.s ? 5 : 1);
	uae_u8 *p = get_addr(addr, 1, 0);
	return *p;
}
uae_u32 get_word_test(uaecptr addr)
{
	check_bus_error(addr, 0, regs.s ? 5 : 1);
	if (addr & 1) {
		return (get_byte_test(addr + 0) << 8) | (get_byte_test(addr + 1) << 0);
	} else {
		uae_u8 *p = get_addr(addr, 2, 0);
		return (p[0] << 8) | (p[1]);
	}
}
uae_u32 get_long_test(uaecptr addr)
{
	check_bus_error(addr, 0, regs.s ? 5 : 1);
	if (addr & 1) {
		uae_u8 v0 = get_byte_test(addr + 0);
		uae_u16 v1 = get_word_test(addr + 1);
		uae_u8 v3 = get_byte_test(addr + 3);
		return (v0 << 24) | (v1 << 8) | (v3 << 0);
	} else if (addr & 2) {
		uae_u16 v0 = get_word_test(addr + 0);
		uae_u16 v1 = get_word_test(addr + 2);
		return (v0 << 16) | (v1 << 0);
	} else {
		uae_u8 *p = get_addr(addr, 4, 0);
		return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | (p[3]);
	}
}

uae_u32 get_byte_debug(uaecptr addr)
{
	uae_u8 *p = get_addr(addr, 1, -1);
	return *p;
}
uae_u32 get_word_debug(uaecptr addr)
{
	uae_u8 *p = get_addr(addr, 2, -1);
	return (p[0] << 8) | (p[1]);
}
uae_u32 get_long_debug(uaecptr addr)
{
	uae_u8 *p = get_addr(addr, 4, -1);
	return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | (p[3]);
}
uae_u32 get_iword_debug(uaecptr addr)
{
	return get_word_debug(addr);
}
uae_u32 get_ilong_debug(uaecptr addr)
{
	return get_long_debug(addr);
}

uae_u32 get_byte_cache_debug(uaecptr addr, bool *cached)
{
	*cached = false;
	return get_byte_test(addr);
}
uae_u32 get_word_cache_debug(uaecptr addr, bool *cached)
{
	*cached = false;
	return get_word_test(addr);
}
uae_u32 get_long_cache_debug(uaecptr addr, bool *cached)
{
	*cached = false;
	return get_long_test(addr);
}

uae_u32 sfc_nommu_get_byte(uaecptr addr)
{
	return get_byte_test(addr);
}
uae_u32 sfc_nommu_get_word(uaecptr addr)
{
	return get_word_test(addr);
}
uae_u32 sfc_nommu_get_long(uaecptr addr)
{
	return get_long_test(addr);
}

void dfc_nommu_put_byte(uaecptr addr, uae_u32 v)
{
	put_byte_test(addr, v);
}
void dfc_nommu_put_word(uaecptr addr, uae_u32 v)
{
	put_word_test(addr, v);
}
void dfc_nommu_put_long(uaecptr addr, uae_u32 v)
{
	put_long_test(addr, v);
}

uae_u16 get_wordi_test(int o)
{
	uae_u32 v = get_word_test_prefetch(o);
	regs.pc += 2;
	return v;
}

uae_u32 memory_get_byte(uaecptr addr)
{
	return get_byte_test(addr);
}
uae_u32 memory_get_word(uaecptr addr)
{
	return get_word_test(addr);
}
uae_u32 memory_get_wordi(uaecptr addr)
{
	return get_word_test(addr);
}
uae_u32 memory_get_long(uaecptr addr)
{
	return get_long_test(addr);
}
uae_u32 memory_get_longi(uaecptr addr)
{
	return get_long_test(addr);
}

void memory_put_long(uaecptr addr, uae_u32 v)
{
	put_long_test(addr, v);
}
void memory_put_word(uaecptr addr, uae_u32 v)
{
	put_word_test(addr, v);
}
void memory_put_byte(uaecptr addr, uae_u32 v)
{
	put_byte_test(addr, v);
}

uae_u8 *memory_get_real_address(uaecptr addr)
{
	return NULL;
}

uae_u32 next_iword_test(void)
{
	uae_u32 v = get_word_test_prefetch(0);
	regs.pc += 2;
	return v;
}

uae_u32 next_ilong_test(void)
{
	uae_u32 v = get_word_test_prefetch(0) << 16;
	v |= get_word_test_prefetch(2);
	regs.pc += 4;
	return v;
}

bool mmu_op30(uaecptr pc, uae_u32 opcode, uae_u16 extra, uaecptr extraa)
{
	m68k_setpc(pc);
	op_illg(opcode);
	return true;
}

uae_u32(*x_get_long)(uaecptr);
uae_u32(*x_get_word)(uaecptr);
void (*x_put_long)(uaecptr, uae_u32);
void (*x_put_word)(uaecptr, uae_u32);

uae_u32(*x_cp_get_long)(uaecptr);
uae_u32(*x_cp_get_word)(uaecptr);
uae_u32(*x_cp_get_byte)(uaecptr);
void (*x_cp_put_long)(uaecptr, uae_u32);
void (*x_cp_put_word)(uaecptr, uae_u32);
void (*x_cp_put_byte)(uaecptr, uae_u32);
uae_u32(*x_cp_next_iword)(void);
uae_u32(*x_cp_next_ilong)(void);

uae_u32(*x_next_iword)(void);
uae_u32(*x_next_ilong)(void);
void (*x_do_cycles)(unsigned long);

uae_u32(REGPARAM3 *x_cp_get_disp_ea_020)(uae_u32 base, int idx) REGPARAM;

static int SPCFLAG_TRACE, SPCFLAG_DOTRACE;

uae_u32 get_disp_ea_test(uae_u32 base, uae_u32 dp)
{
	int reg = (dp >> 12) & 15;
	uae_s32 regd = regs.regs[reg];
	if ((dp & 0x800) == 0)
		regd = (uae_s32)(uae_s16)regd;
	return base + (uae_s8)dp + regd;
}

static void activate_trace(void)
{
	SPCFLAG_TRACE = 0;
	SPCFLAG_DOTRACE = 1;
}

static void do_trace(void)
{
	regs.trace_pc = regs.pc;
	if (regs.t0 && !regs.t1 && currprefs.cpu_model >= 68020) {
		// this is obsolete
		return;
	}
	if (regs.t1) {
		activate_trace();
	}
}

void REGPARAM2 MakeSR(void)
{
	regs.sr = ((regs.t1 << 15) | (regs.t0 << 14)
		| (regs.s << 13) | (regs.m << 12) | (regs.intmask << 8)
		| (GET_XFLG() << 4) | (GET_NFLG() << 3)
		| (GET_ZFLG() << 2) | (GET_VFLG() << 1)
		| GET_CFLG());
}
void MakeFromSR_x(int t0trace)
{
	int oldm = regs.m;
	int olds = regs.s;
	int oldt0 = regs.t0;
	int oldt1 = regs.t1;

	SET_XFLG((regs.sr >> 4) & 1);
	SET_NFLG((regs.sr >> 3) & 1);
	SET_ZFLG((regs.sr >> 2) & 1);
	SET_VFLG((regs.sr >> 1) & 1);
	SET_CFLG(regs.sr & 1);

	if (regs.t1 == ((regs.sr >> 15) & 1) &&
		regs.t0 == ((regs.sr >> 14) & 1) &&
		regs.s == ((regs.sr >> 13) & 1) &&
		regs.m == ((regs.sr >> 12) & 1) &&
		regs.intmask == ((regs.sr >> 8) & 7))
		return;

	regs.t1 = (regs.sr >> 15) & 1;
	regs.t0 = (regs.sr >> 14) & 1;
	regs.s = (regs.sr >> 13) & 1;
	regs.m = (regs.sr >> 12) & 1;
	regs.intmask = (regs.sr >> 8) & 7;

	if (currprefs.cpu_model >= 68020) {
		/* 68060 does not have MSP but does have M-bit.. */
		if (currprefs.cpu_model >= 68060)
			regs.msp = regs.isp;
		if (olds != regs.s) {
			if (olds) {
				if (oldm)
					regs.msp = m68k_areg (regs, 7);
				else
					regs.isp = m68k_areg (regs, 7);
				m68k_areg (regs, 7) = regs.usp;
			} else {
				regs.usp = m68k_areg (regs, 7);
				m68k_areg (regs, 7) = regs.m ? regs.msp : regs.isp;
			}
		} else if (olds && oldm != regs.m) {
			if (oldm) {
				regs.msp = m68k_areg (regs, 7);
				m68k_areg (regs, 7) = regs.isp;
			} else {
				regs.isp = m68k_areg (regs, 7);
				m68k_areg (regs, 7) = regs.msp;
			}
		}
		if (currprefs.cpu_model >= 68060)
			regs.t0 = 0;
	} else {
		regs.t0 = regs.m = 0;
		if (olds != regs.s) {
			if (olds) {
				regs.isp = m68k_areg (regs, 7);
				m68k_areg (regs, 7) = regs.usp;
			} else {
				regs.usp = m68k_areg (regs, 7);
				m68k_areg (regs, 7) = regs.isp;
			}
		}
	}

	if (regs.t1 || regs.t0) {
		SPCFLAG_TRACE = 1;
	} else {
		/* Keep SPCFLAG_DOTRACE, we still want a trace exception for
		SR-modifying instructions (including STOP).  */
		SPCFLAG_TRACE = 0;
	}
	// STOP SR-modification does not generate T0
	// If this SR modification set Tx bit, no trace until next instruction.
	if ((oldt0 && t0trace && currprefs.cpu_model >= 68020) || oldt1) {
		// Always trace if Tx bits were already set, even if this SR modification cleared them.
		activate_trace();
	}

}

void REGPARAM2 MakeFromSR_T0(void)
{
	MakeFromSR_x(1);
}
void REGPARAM2 MakeFromSR(void)
{
	MakeFromSR_x(0);
}

void cpu_halt(int halt)
{
	cpu_halted = halt;
}

static void exception_check_trace(int nr)
{
	SPCFLAG_TRACE = 0;
	SPCFLAG_DOTRACE = 0;
	if (regs.t1) {
		/* trace stays pending if exception is div by zero, chk,
		* trapv or trap #x
		*/
		if (nr == 5 || nr == 6 || nr == 7 || (nr >= 32 && nr <= 47))
			SPCFLAG_DOTRACE = 1;
	}
	regs.t1 = regs.t0 = 0;
}

void m68k_setstopped(void)
{
	/* A traced STOP instruction drops through immediately without
	actually stopping.  */
	if (SPCFLAG_DOTRACE == 0) {
		cpu_stopped = 1;
	} else {
		cpu_stopped = 0;
	}
}

void check_t0_trace(void)
{
	if (regs.t0 && !regs.t1 && currprefs.cpu_model >= 68020) {
		SPCFLAG_TRACE = 0;
		SPCFLAG_DOTRACE = 1;
	}
}

void cpureset(void)
{
	cpu_halted = -1;
}

static void doexcstack(void)
{
	// generate exception but don't store it with test results

	int noac = noaccesshistory;
	int ta = testing_active;
	noaccesshistory = 1;
	testing_active = -1;

	int opcode = (opcode_memory[0] << 8) | (opcode_memory[1]);
	if (test_exception_opcode >= 0)
		opcode = test_exception_opcode;

	int sv = regs.s;
	uaecptr tmp = m68k_areg(regs, 7);
	m68k_areg(regs, 7) = test_memory_end + EXTRA_RESERVED_SPACE;
	if (cpu_lvl == 0) {
		if (test_exception == 2 || test_exception == 3) {
			uae_u16 mode = (sv ? 4 : 0) | test_exception_3_fc;
			mode |= test_exception_3_w ? 0 : 16;
			Exception_build_68000_address_error_stack_frame(mode, opcode, test_exception_addr, regs.pc);
		}
	} else if (cpu_lvl == 1) {
		if (test_exception == 3) {
			uae_u16 ssw = (sv ? 4 : 0) | test_exception_3_fc;
			ssw |= test_exception_3_w ? 0 : 0x100;
			ssw |= (test_exception_3_fc & 2) ? 0 : 0x2000;
			regs.mmu_fault_addr = test_exception_addr;
			Exception_build_stack_frame(regs.instruction_pc, regs.pc, ssw, 3, 0x08);
		} else {
			Exception_build_stack_frame_common(regs.instruction_pc, regs.pc, 0, test_exception);
		}
	} else if (cpu_lvl == 2 || cpu_lvl == 3) {
		if (test_exception == 3) {
			uae_u16 ssw = (sv ? 4 : 0) | test_exception_3_fc;
			ssw |= 0x20;
			regs.mmu_fault_addr = test_exception_addr;
			Exception_build_stack_frame(regs.instruction_pc, regs.pc, ssw, 3, 0x0b);
		} else {
			Exception_build_stack_frame_common(regs.instruction_pc, regs.pc, 0, test_exception);
		}
	} else {
		if (test_exception == 3) {
			if (currprefs.cpu_model >= 68040)
				test_exception_addr &= ~1;
			Exception_build_stack_frame(test_exception_addr, regs.pc, 0, 3, 0x02);
		} else {
			Exception_build_stack_frame_common(regs.instruction_pc, regs.pc, 0, test_exception);
		}
	}
	exception_stack_frame_size = test_memory_end + EXTRA_RESERVED_SPACE - m68k_areg(regs, 7);

	m68k_areg(regs, 7) = tmp;
	testing_active = ta;
	noaccesshistory = noac;
}

uae_u32 REGPARAM2 op_illg_1(uae_u32 opcode)
{
	if ((opcode & 0xf000) == 0xf000)
		test_exception = 11;
	else if ((opcode & 0xf000) == 0xa000)
		test_exception = 10;
	else
		test_exception = 4;
	doexcstack();
	return 0;
}
void REGPARAM2 op_unimpl(uae_u32 opcode)
{
	test_exception = 61;
	doexcstack();
}
uae_u32 REGPARAM2 op_unimpl_1(uae_u32 opcode)
{
	if ((opcode & 0xf000) == 0xf000 || currprefs.cpu_model < 68060)
		op_illg(opcode);
	else
		op_unimpl(opcode);
	return 0;
}
uae_u32 REGPARAM2 op_illg(uae_u32 opcode)
{
	return op_illg_1(opcode);
}

void exception2_read(uae_u32 opcode, uaecptr addr, int fc)
{
	test_exception = 2;
	test_exception_3_w = 0;
	test_exception_addr = addr;
	test_exception_opcode = opcode;
	test_exception_3_fc = fc;
	doexcstack();
}

void exception2_write(uae_u32 opcode, uaecptr addr, int fc)
{
	test_exception = 2;
	test_exception_3_w = 1;
	test_exception_addr = addr;
	test_exception_opcode = opcode;
	test_exception_3_fc = fc;
	doexcstack();
}


void exception3_read(uae_u32 opcode, uae_u32 addr, int fc)
{
	test_exception = 3;
	test_exception_3_w = 0;
	test_exception_addr = addr;
	test_exception_opcode = opcode;
	test_exception_3_fc = fc;
	doexcstack();
}
void exception3_write(uae_u32 opcode, uae_u32 addr, int fc)
{
	test_exception = 3;
	test_exception_3_w = 1;
	test_exception_addr = addr;
	test_exception_opcode = opcode;
	test_exception_3_fc = fc;
	doexcstack();
}
void REGPARAM2 Exception(int n)
{
	test_exception = n;
	test_exception_addr = m68k_getpci();
	test_exception_opcode = -1;
	doexcstack();
}
void REGPARAM2 Exception_cpu(int n)
{
	test_exception = n;
	test_exception_addr = m68k_getpci();
	test_exception_opcode = -1;

	bool t0 = currprefs.cpu_model >= 68020 && regs.t0 && !regs.t1;
	// check T0 trace
	if (t0) {
		activate_trace();
	}
	doexcstack();
}
void exception3i(uae_u32 opcode, uaecptr addr)
{
	test_exception = 3;
	test_exception_3_fc = 2;
	test_exception_3_w = 0;
	test_exception_addr = addr;
	test_exception_opcode = opcode;
	doexcstack();
}
void exception3b(uae_u32 opcode, uaecptr addr, bool w, bool i, uaecptr pc)
{
	test_exception = 3;
	test_exception_3_fc = i ? 2 : 1;
	test_exception_3_w = w;
	test_exception_addr = addr;
	test_exception_opcode = opcode;
	doexcstack();
}

int cctrue(int cc)
{
	uae_u32 cznv = regflags.cznv;

	switch (cc) {
	case 0:  return 1;											/*					T  */
	case 1:  return 0;											/*					F  */
	case 2:  return (cznv & (FLAGVAL_C | FLAGVAL_Z)) == 0;		/* !CFLG && !ZFLG	HI */
	case 3:  return (cznv & (FLAGVAL_C | FLAGVAL_Z)) != 0;		/*  CFLG || ZFLG	LS */
	case 4:  return (cznv & FLAGVAL_C) == 0;					/* !CFLG			CC */
	case 5:  return (cznv & FLAGVAL_C) != 0;					/*  CFLG			CS */
	case 6:  return (cznv & FLAGVAL_Z) == 0;					/* !ZFLG			NE */
	case 7:  return (cznv & FLAGVAL_Z) != 0;					/*  ZFLG			EQ */
	case 8:  return (cznv & FLAGVAL_V) == 0;					/* !VFLG			VC */
	case 9:  return (cznv & FLAGVAL_V) != 0;					/*  VFLG			VS */
	case 10: return (cznv & FLAGVAL_N) == 0;					/* !NFLG			PL */
	case 11: return (cznv & FLAGVAL_N) != 0;					/*  NFLG			MI */

	case 12: /*  NFLG == VFLG		GE */
		return ((cznv >> FLAGBIT_N) & 1) == ((cznv >> FLAGBIT_V) & 1);
	case 13: /*  NFLG != VFLG		LT */
		return ((cznv >> FLAGBIT_N) & 1) != ((cznv >> FLAGBIT_V) & 1);
	case 14: /* !GET_ZFLG && (GET_NFLG == GET_VFLG);  GT */
		return !(cznv & FLAGVAL_Z) && (((cznv >> FLAGBIT_N) & 1) == ((cznv >> FLAGBIT_V) & 1));
	case 15: /* GET_ZFLG || (GET_NFLG != GET_VFLG);   LE */
		return (cznv & FLAGVAL_Z) || (((cznv >> FLAGBIT_N) & 1) != ((cznv >> FLAGBIT_V) & 1));
	}
	return 0;
}

static uae_u32 xorshiftstate;
static uae_u32 xorshift32(void)
{
	uae_u32 x = xorshiftstate;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	xorshiftstate = x;
	return xorshiftstate;
}

static int rand16_cnt;
static uae_u16 rand16(void)
{
	int cnt = rand16_cnt & 15;
	uae_u16 v = 0;
	rand16_cnt++;
	if (cnt < 15) {
		v = (uae_u16)xorshift32();
		if (cnt <= 6)
			v &= ~0x8000;
		else
			v |= 0x8000;
	}
	if (forced_immediate_mode == 1)
		v &= ~1;
	if (forced_immediate_mode == 2)
		v |= 1;
	return v;
}
static int rand32_cnt;
static uae_u32 rand32(void)
{
	int cnt = rand32_cnt & 31;
	uae_u32 v = 0;
	rand32_cnt++;
	if (cnt < 31) {
		v = xorshift32();
		if (cnt <= 14)
			v &= ~0x80000000;
		else
			v |= 0x80000000;
	}
	if (forced_immediate_mode == 1)
		v &= ~1;
	if (forced_immediate_mode == 2)
		v |= 1;
	return v;
}

// first 3 values: positive
// next 4 values: negative
// last: zero
static int rand8_cnt;
static uae_u8 rand8(void)
{
	int cnt = rand8_cnt & 7;
	uae_u8 v = 0;
	rand8_cnt++;
	if (cnt < 7) {
		v = (uae_u8)xorshift32();
		if (cnt <= 2)
			v &= ~0x80;
		else
			v |= 0x80;
	}
	if (forced_immediate_mode == 1)
		v &= ~1;
	if (forced_immediate_mode == 2)
		v |= 1;
	return v;
}

static uae_u8 frand8(void)
{
	return (uae_u8)xorshift32();
}

static void fill_memory_buffer(uae_u8 *p, int size)
{
	for (int i = 0; i < size; i++) {
		p[i] = frand8();
	}
	// fill extra zeros
	for (int i = 0; i < size; i++) {
		if (frand8() < 0x70)
			p[i] = 0x00;
	}
}

static void fill_memory(void)
{
	fill_memory_buffer(low_memory_temp, low_memory_size);
	fill_memory_buffer(high_memory_temp, high_memory_size);
	fill_memory_buffer(test_memory_temp, test_memory_size);
}

static void save_memory(const TCHAR *path, const TCHAR *name, uae_u8 *p, int size)
{
	TCHAR fname[1000];
	_stprintf(fname, _T("%s%s"), path, name);
	FILE *f = _tfopen(fname, _T("wb"));
	if (!f) {
		wprintf(_T("Couldn't open '%s'\n"), fname);
		abort();
	}
	fwrite(p, 1, size, f);
	fclose(f);
}

static uae_u8 *store_rel(uae_u8 *dst, uae_u8 mode, uae_u32 s, uae_u32 d, int ordered)
{
	int diff = (uae_s32)d - (uae_s32)s;
	if (diff == 0) {
		if (!ordered)
			return dst;
		*dst++ = CT_EMPTY;
		return dst;
	}
	if (diff >= -128 && diff < 128) {
		*dst++ = mode | CT_RELATIVE_START_BYTE;
		*dst++ = diff & 0xff;
	} else if (diff >= -32768 && diff < 32767) {
		*dst++ = mode | CT_RELATIVE_START_WORD;
		*dst++ = (diff >> 8) & 0xff;
		*dst++ = diff & 0xff;
	} else if (d < 0x8000) {
		*dst++ = mode | CT_ABSOLUTE_WORD;
		*dst++ = (d >> 8) & 0xff;
		*dst++ = d & 0xff;
	} else if ((d & ~addressing_mask) == ~addressing_mask && (d & addressing_mask) >= (HIGH_MEMORY_START & addressing_mask)) {
		*dst++ = mode | CT_ABSOLUTE_WORD;
		*dst++ = (d >> 8) & 0xff;
		*dst++ = d & 0xff;
	} else {
		*dst++ = mode | CT_ABSOLUTE_LONG;
		*dst++ = (d >> 24) & 0xff;
		*dst++ = (d >> 16) & 0xff;
		*dst++ = (d >> 8) & 0xff;
		*dst++ = d & 0xff;
	}
	return dst;
}

static uae_u8 *store_fpureg(uae_u8 *dst, uae_u8 mode, floatx80 d)
{
	*dst++ = mode | CT_SIZE_FPU;
	*dst++ = (d.high >> 8) & 0xff;
	*dst++ = (d.high >> 0) & 0xff;
	*dst++ = (d.low >> 56) & 0xff;
	*dst++ = (d.low >> 48) & 0xff;
	*dst++ = (d.low >> 40) & 0xff;
	*dst++ = (d.low >> 32) & 0xff;
	*dst++ = (d.low >> 24) & 0xff;
	*dst++ = (d.low >> 16) & 0xff;
	*dst++ = (d.low >>  8) & 0xff;
	*dst++ = (d.low >>  0) & 0xff;
	return dst;
}

static uae_u8 *store_reg(uae_u8 *dst, uae_u8 mode, uae_u32 s, uae_u32 d, int size)
{
	if (s == d && size < 0)
		return dst;
	if (((s & 0xffffff00) == (d & 0xffffff00) && size < 0) || size == sz_byte) {
		*dst++ = mode | CT_SIZE_BYTE;
		*dst++ = d & 0xff;
	} else if (((s & 0xffff0000) == (d & 0xffff0000) && size < 0) || size == sz_word) {
		*dst++ = mode | CT_SIZE_WORD;
		*dst++ = (d >> 8) & 0xff;
		*dst++ = d & 0xff;
	} else {
		*dst++ = mode | CT_SIZE_LONG;
		*dst++ = (d >> 24) & 0xff;
		*dst++ = (d >> 16) & 0xff;
		*dst++ = (d >> 8) & 0xff;
		*dst++ = d & 0xff;
	}
	return dst;
}

static uae_u8 *store_mem_bytes(uae_u8 *dst, uaecptr start, int len, uae_u8 *old)
{
	if (!len)
		return dst;
	if (len > 32) {
		wprintf(_T("too long byte count!\n"));
		abort();
	}
	uaecptr oldstart = start;
	uae_u8 offset = 0;
	// start
	for (int i = 0; i < len; i++) {
		uae_u8 v = get_byte_test(start);
		if (v != *old)
			break;
		start++;
		old++;
	}
	// end
	offset = start - oldstart;
	if (offset > 7) {
		start -= (offset - 7);
		offset = 7;
	}
	len -= offset;
	for (int i = len - 1; i >= 0; i--) {
		uae_u8 v = get_byte_test(start + i);
		if (v != old[i])
			break;
		len--;
	}
	if (!len)
		return dst;
	*dst++ = CT_MEMWRITES | CT_PC_BYTES;
	*dst++ = (offset << 5) | (uae_u8)(len == 32 ? 0 : len);
	for (int i = 0; i < len; i++) {
		*dst++ = get_byte_test(start);
		start++;
	}
	return dst;
}

static uae_u8 *store_mem(uae_u8 *dst, int storealways)
{
	if (ahcnt == 0)
		return dst;
	for (int i = 0; i < ahcnt; i++) {
		struct accesshistory *ah = &ahist[i];
		if (ah->oldval == ah->val && !storealways)
 			continue;
		validate_addr(ah->addr, 1 << ah->size);
		uaecptr addr = ah->addr;
		addr &= addressing_mask;
 		if (addr < 0x8000) {
			*dst++ = CT_MEMWRITE | CT_ABSOLUTE_WORD;
			*dst++ = (addr >> 8) & 0xff;
			*dst++ = addr & 0xff;
		} else if (addr >= HIGH_MEMORY_START) {
			*dst++ = CT_MEMWRITE | CT_ABSOLUTE_WORD;
			*dst++ = (addr >> 8) & 0xff;
			*dst++ = addr & 0xff;
		} else if (addr >= opcode_memory_start && addr < opcode_memory_start + 32768) {
			*dst++ = CT_MEMWRITE | CT_RELATIVE_START_WORD;
			uae_u16 diff = addr - opcode_memory_start;
			*dst++ = (diff >> 8) & 0xff;
			*dst++ = diff & 0xff;
		} else if (addr < opcode_memory_start && addr >= opcode_memory_start - 32768) {
			*dst++ = CT_MEMWRITE | CT_RELATIVE_START_WORD;
			uae_u16 diff = addr - opcode_memory_start;
			*dst++ = (diff >> 8) & 0xff;
			*dst++ = diff & 0xff;
		} else {
			*dst++ = CT_MEMWRITE | CT_ABSOLUTE_LONG;
			*dst++ = (addr >> 24) & 0xff;
			*dst++ = (addr >> 16) & 0xff;
			*dst++ = (addr >> 8) & 0xff;
			*dst++ = addr & 0xff;
		}
		dst = store_reg(dst, CT_MEMWRITE, 0, ah->oldval, ah->size);
		dst = store_reg(dst, CT_MEMWRITE, 0, ah->val, ah->size);
	}
	return dst;
}

static void pl(uae_u8 *p, uae_u32 v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v >> 0;
}
static uae_u32 gl(uae_u8 *p)
{
	return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | (p[3] << 0);
}
static uae_u32 gw(uae_u8 *p)
{
	return (p[0] << 8) | (p[1] << 0);
}

static bool load_file(const TCHAR *path, const TCHAR *file, uae_u8 *p, int size, int offset)
{
	TCHAR fname[1000];
	if (path) {
		_stprintf(fname, _T("%s%s"), path, file);
	} else {
		_tcscpy(fname, file);
	}
	FILE *f = _tfopen(fname, _T("rb"));
	if (!f)
		return false;
	if (offset < 0) {
		fseek(f, -size, SEEK_END);
	} else {
		fseek(f, offset, SEEK_SET);
	}
	int lsize = fread(p, 1, size, f);
	if (lsize != size) {
		wprintf(_T("Couldn't read file '%s'\n"), fname);
		exit(0);
	}
	fclose(f);
	return true;
}

static void save_data(uae_u8 *dst, const TCHAR *dir)
{
	TCHAR path[1000];

	if (dst == storage_buffer)
		return;

	if (dst - storage_buffer > max_storage_buffer) {
		wprintf(_T("data buffer overrun!\n"));
		abort();
	}

	_wmkdir(dir);
	_stprintf(path, _T("%s/%04d.dat"), dir, filecount);
	FILE *f = _tfopen(path, _T("wb"));
	if (!f) {
		wprintf(_T("couldn't open '%s'\n"), path);
		abort();
	}
	if (filecount == 0) {
		uae_u8 data[4];
		pl(data, DATA_VERSION);
		fwrite(data, 1, 4, f);
		pl(data, (uae_u32)starttime);
		fwrite(data, 1, 4, f);
		pl(data, ((hmem_rom | (test_high_memory_start == 0xffffffff ? 0x8000 : 0x0000)) << 16) | (lmem_rom | (test_low_memory_start == 0xffffffff ? 0x8000 : 0x0000)));
		fwrite(data, 1, 4, f);
		pl(data, test_memory_start);
		fwrite(data, 1, 4, f);
		pl(data, test_memory_size);
		fwrite(data, 1, 4, f);
		pl(data, opcode_memory_start - test_memory_start);
		fwrite(data, 1, 4, f);
		pl(data, (cpu_lvl << 16) | sr_undefined_mask | (addressing_mask == 0xffffffff ? 0x80000000 : 0) | ((feature_flag_mode & 1) << 30) | (feature_min_interrupt_mask << 20));
		fwrite(data, 1, 4, f);
		pl(data, currprefs.fpu_model);
		fwrite(data, 1, 4, f);
		pl(data, test_low_memory_start);
		fwrite(data, 1, 4, f);
		pl(data, test_low_memory_end);
		fwrite(data, 1, 4, f);
		pl(data, test_high_memory_start);
		fwrite(data, 1, 4, f);
		pl(data, test_high_memory_end);
		fwrite(data, 1, 4, f);
		pl(data, safe_memory_start);
		fwrite(data, 1, 4, f);
		pl(data, safe_memory_end);
		fwrite(data, 1, 4, f);
		pl(data, 0);
		fwrite(data, 1, 4, f);
		pl(data, 0);
		fwrite(data, 1, 4, f);
		fwrite(inst_name, 1, sizeof(inst_name) - 1, f);
		fclose(f);
		filecount++;
		save_data(dst, dir);
	} else {
		uae_u8 data[4];
		pl(data, DATA_VERSION);
		fwrite(data, 1, 4, f);
		pl(data, (uae_u32)starttime);
		fwrite(data, 1, 4, f);
		pl(data, 0);
		fwrite(data, 1, 4, f);
		fwrite(data, 1, 4, f);
		*dst++ = CT_END_FINISH;
		fwrite(storage_buffer, 1, dst - storage_buffer, f);
		fclose(f);
		filecount++;
	}
}

static int full_format_cnt;

static uaecptr putfpuimm(uaecptr pc, int opcodesize, int *isconstant)
{
	// TODO: generate sane FPU immediates
	switch (opcodesize)
	{
	case 0: // L
		put_long_test(pc, rand32());
		pc += 4;
		break;
	case 4: // W
		put_word_test(pc, rand16());
		pc += 2;
		break;
	case 6: // B
		put_word_test(pc, rand16());
		pc += 2;
		break;
	case 1: // S
		put_long(pc, rand32());
		pc += 4;
		break;
	case 2: // X
		put_long(pc, rand32());
		put_long(pc + 4, rand32());
		put_long(pc + 8, rand32());
		pc += 12;
		break;
	case 3: // P
		put_long(pc, rand32());
		put_long(pc + 4, rand32());
		put_long(pc + 8, rand32());
		pc += 12;
		break;
	case 5: // D
		put_long(pc, rand32());
		put_long(pc + 4, rand32());
		pc += 8;
		break;
	}
	return pc;
}

// generate mostly random EA.
static int create_ea_random(uae_u16 *opcodep, uaecptr pc, int mode, int reg, struct instr *dp, int *isconstant, int srcdst, int fpuopcode, int opcodesize, uae_u32 *eap)
{
	uaecptr old_pc = pc;
	uae_u16 opcode = *opcodep;

	switch (mode)
	{
	case Dreg:
		if (reg == feature_loop_mode_register) {
			if (((dp->sduse & 0x20) && !srcdst) || ((dp->sduse & 0x02) && srcdst)) {
				int pos = srcdst ? dp->dpos : dp->spos;
				opcode &= ~(7 << pos);
				opcode |= ((reg + 1) & 7) << pos;
			}
		}
		break;
	case Areg:
	case Aind:
	case Aipi:
	case Apdi:
		*eap = 1;
		break;
	case Ad16:
	case PC16:
		put_word_test(pc, rand16());
		*isconstant = 16;
		pc += 2;
		*eap = 1;
		break;
	case Ad8r:
	case PC8r:
	{
		uae_u16 v = rand16();
		if (!feature_full_extension_format)
			v &= ~0x100;
		if (mode == Ad8r) {
			if ((ad8r[srcdst] & 3) == 1)
				v &= ~0x100;
			if ((ad8r[srcdst] & 3) == 2)
				v |= 0x100;
		} else if (mode == PC8r) {
			if ((pc8r[srcdst] & 3) == 1)
				v &= ~0x100;
			if ((pc8r[srcdst] & 3) == 2)
				v |= 0x100;
		}
		if (currprefs.cpu_model < 68020 || (v & 0x100) == 0) {
			*isconstant = 16;
			put_word_test(pc, v);
			pc += 2;
		} else {
			// full format extension
			// attempt to generate every possible combination,
			// one by one.
			for (;;) {
				v &= 0xff00;
				v |= full_format_cnt & 0xff;
				// skip reserved combinations for now
				int is = (v >> 6) & 1;
				int iis = v & 7;
				int sz = (v >> 4) & 3;
				int bit3 = (v >> 3) & 1;
				if ((is && iis >= 4) || iis == 4 || sz == 0 || bit3 == 1) {
					full_format_cnt++;
					continue;
				}
				break;
			}
			put_word_test(pc, v);
			uaecptr pce = pc;
			pc += 2;
			// calculate lenght of extension
			ShowEA_disp(&pce, mode == Ad8r ? regs.regs[reg + 8] : pce, NULL, NULL);
			while (pc < pce) {
				v = rand16();
				put_word_test(pc, v);
				pc += 2;
			}
			*isconstant = 32;
		}
		*eap = 1;
		break;
	}
	case absw:
		put_word_test(pc, rand16());
		*isconstant = 16;
		pc += 2;
		*eap = 1;
		break;
	case absl:
		{
			uae_u32 v = rand32();
			if ((immabsl_cnt & 7) == 0) {
				v &= 0x0000ffff;
			} else if ((immabsl_cnt & 7) >= 4) {
				int offset = 0;
				for (;;) {
					offset = (uae_s16)rand16();
					if (offset < -OPCODE_AREA || offset > OPCODE_AREA)
						break;
				}
				v = opcode_memory_start + offset;
			}
			immabsl_cnt++;
			put_long_test(pc, v);
			*isconstant = 32;
			pc += 4;
		}
		*eap = 1;
		break;
	case imm:
		if (fpuopcode >= 0 && opcodesize < 8) {
			pc = putfpuimm(pc, opcodesize, isconstant);
		} else {
			if (dp->size == sz_long) {
				put_long_test(pc, rand32());
				*isconstant = 32;
				pc += 4;
			}
			else {
				put_word_test(pc, rand16());
				*isconstant = 16;
				pc += 2;
			}
		}
		break;
	case imm0:
	{
		// byte immediate but randomly fill also upper byte
		uae_u16 v = rand16();
		if ((imm8_cnt & 3) == 0)
			v &= 0xff;
		imm8_cnt++;
		put_word_test(pc, v);
		*isconstant = 16;
		pc += 2;
		break;
	}
	case imm1:
	{
		// word immediate
		if (fpuopcode >= 0) {
			uae_u16 v = 0;
			if (opcodesize == 7) {
				// FMOVECR
				v = 0x4000;
				v |= opcodesize << 10;
				v |= imm16_cnt & 0x3ff;
				imm16_cnt++;
				if ((opcode & 0x3f) != 0)
					return -1;
				*isconstant = 0;
				if (imm16_cnt < 0x400)
					*isconstant = -1;
			} else if (opcodesize == 8) {
				// FMOVEM/FMOVE to/from control registers
				v |= 0x8000;
				v |= (imm16_cnt & 15) << 11;
				v |= rand16() & 0x07ff;
				imm16_cnt++;
				if (imm16_cnt >= 32)
					*isconstant = 0;
				else
					*isconstant = -1;
			} else {
				v |= fpuopcode;
				if (imm16_cnt & 1) {
					// EA to FP reg
					v |= 0x4000;
					v |= opcodesize << 10;
					imm16_cnt++;
				} else {
					// FP reg to FP reg
					v |= ((imm16_cnt >> 1) & 7) << 10;
					// clear mode/reg field
					opcode &= ~0x3f;
					imm16_cnt++;
					if (opcodesize != 2) {
						// not X: skip
						return -2;
					}
				}
				*isconstant = 16;
			}
			put_word_test(pc, v);
			pc += 2;
		} else {
			if (opcodecnt == 1) {
				// STOP #xxxx: test all combinations
				// (also includes RTD)
				if (dp->mnemo == i_LPSTOP) {
					uae_u16 lp = 0x01c0;
					if (imm16_cnt & (0x40 | 0x80)) {
						for (;;) {
							lp = rand16();
							if (lp != 0x01c0)
								break;
						}
					}
					put_word_test(pc, lp);
					put_word_test(pc + 2, imm16_cnt++);
					pc += 2;
					if (imm16_cnt == 0)
						*isconstant = 0;
					else
						*isconstant = -1;
				} else if (dp->mnemo == i_MOVE2C || dp->mnemo == i_MOVEC2) {
					uae_u16 c = (imm16_cnt >> 1) & 0xfff;
					opcode = 0x4e7a;
					put_word_test(pc, 0x1000 | c); // movec rc,d1
					pc += 2;
					uae_u32 val = (imm16_cnt & 1) ? 0xffffffff : 0x00000000;
					switch (c)
					{
					case 0x003: // TC
					case 0x004: // ITT0
					case 0x005: // ITT1
					case 0x006: // DTT0
					case 0x007: // DTT1
						val &= ~0x8000;
						break;
					case 0x808: // PCR
						val &= ~(0x80 | 0x40 | 0x20 | 0x10 | 0x08 | 0x04);
						break;
					}
					put_word_test(pc, 0x203c); // move.l #x,d0
					pc += 2;
					put_long_test(pc, val);
					pc += 4;
					put_long_test(pc, 0x4e7b0000 | c); // movec d0,rc
					pc += 4;
					put_long_test(pc, 0x4e7a2000 | c); // movec rc,d2
					pc += 4;
					put_long_test(pc, 0x4e7b1000 | c); // movec d1,rc
					pc += 4;
					put_word_test(pc, 0x7200); // moveq #0,d0
					pc += 2;
					imm16_cnt++;
					multi_mode = 1;
					pc -= 2;
					if (imm16_cnt == 0x1000)
						*isconstant = 0;
					else
						*isconstant = -1;
				} else {
					put_word_test(pc, imm16_cnt++);
					if (imm16_cnt == 0)
						*isconstant = 0;
					else
						*isconstant = -1;
				}
			} else {
				uae_u16 v = rand16();
				if ((imm16_cnt & 7) == 0)
					v &= 0x00ff;
				if ((imm16_cnt & 15) == 0)
					v &= 0x000f;
				imm16_cnt++;
				put_word_test(pc, v);
				*isconstant = 16;
			}
			pc += 2;
		}
		break;
	}
	case imm2:
	{
		// long immediate
		uae_u32 v = rand32();
		if ((imm32_cnt & 7) == 0) {
			v &= 0x0000ffff;
		}
		imm32_cnt++;
		put_long_test(pc, v);
		if (imm32_cnt < 256)
			*isconstant = -1;
		else
			*isconstant = 32;
		pc += 4;
		break;
	}
	}
	*opcodep = opcode;
	return pc - old_pc;
}

static int ea_exact_cnt;

// generate exact EA (for bus error test)
static int create_ea_exact(uae_u16 *opcodep, uaecptr pc, int mode, int reg, struct instr *dp, int *isconstant, int srcdst, int fpuopcode, int opcodesize, uae_u32 *eap)
{
	uae_u32 target = target_ea[srcdst];
	ea_exact_cnt++;

	switch (mode)
	{
		// always allow modes that don't have EA
	case Areg:
	case Dreg:
		return 0;
	case Aind:
	case Aipi:
	{
		if (cur_registers[reg + 8] == target) {
			*eap = target;
			return  0;
		}
		return -2;
	}
	case Apdi:
	{
		if (cur_registers[reg + 8] == target + (1 << dp->size)) {
			*eap = target;
			return  0;
		}
		return -2;
	}
	case Ad16:
	{
		uae_u32 v = cur_registers[reg + 8];
		if (target <= v + 0x7ffe && (target >= v - 0x8000 || v < 0x8000)) {
			put_word_test(pc, target - v);
			*eap = target;
			return 2;
		}
		return -2;
	}
	case PC16:
	{
		uae_u32 pct = pc + 2 - 2;
		if (target <= pct + 0x7ffe && target >= pct - 0x8000) {
			put_word_test(pc, target - pct);
			*eap = target;
			return 2;
		}
		return -2;
	}
	case Ad8r:
	{
		for (int r = 0; r < 16; r++) {
			uae_u32 aval = cur_registers[reg + 8];
			int rn = ((ea_exact_cnt >> 1) + r) & 15;
			for (int i = 0; i < 2; i++) {
				if ((ea_exact_cnt & 1) == 0 || i == 1) {
					uae_s32 val32 = cur_registers[rn];
					uae_u32 addr = aval + val32;
					if (target <= addr + 0x7f && target >= addr - 0x80) {
						put_word_test(pc, (rn << 12) | 0x0800 | ((target - addr) & 0xff));
						*eap = target;
						return 2;
					}
				} else {
					uae_s16 val16 = (uae_s16)cur_registers[rn];
					uae_u32 addr = aval + val16;
					if (target <= addr + 0x7f && target >= addr - 0x80) {
						put_word_test(pc, (rn << 12) | 0x0000 | ((target - addr) & 0xff));
						*eap = target;
						return 2;
					}
				}
			}
		}
		return -2;
	}
	case PC8r:
	{
		for (int r = 0; r < 16; r++) {
			uae_u32 aval = pc + 2 - 2;
			int rn = ((ea_exact_cnt >> 1) + r) & 15;
			for (int i = 0; i < 2; i++) {
				if ((ea_exact_cnt & 1) == 0 || i == 1) {
					uae_s32 val32 = cur_registers[rn];
					uae_u32 addr = aval + val32;
					if (target <= addr + 0x7f && target >= addr - 0x80) {
						put_word_test(pc, (rn << 12) | 0x0800 | ((target - addr) & 0xff));
						*eap = target;
						return 2;
					}
				} else {
					uae_s16 val16 = (uae_s16)cur_registers[rn];
					uae_u32 addr = aval + val16;
					if (target <= addr + 0x7f && target >= addr - 0x80) {
						put_word_test(pc, (rn << 12) | 0x0000 | ((target - addr) & 0xff));
						*eap = target;
						return 2;
					}
				}
			}
		}
		return -2;
	}
	case absw:
	{
		if (target >= test_low_memory_start && target < test_low_memory_end) {
			put_word_test(pc, target);
			*eap = target;
			return 2;
		}
		return -2;
	}
	case absl:
	{
		if (target >= test_low_memory_start && target < test_low_memory_end) {
			put_long_test(pc, target);
			*eap = target;
			return 4;
		}
		if (target >= test_high_memory_start && target < test_high_memory_end) {
			put_long_test(pc, target);
			*eap = target;
			return 4;
		}
		if (target >= test_memory_start && target < test_memory_end) {
			put_long_test(pc, target);
			*eap = target;
			return 4;
		}
		return -2;
	}
	case imm:
		if (srcdst)
			return -2;
		if (dp->size == sz_long) {
			put_long_test(pc, rand32());
			return 4;
		} else {
			put_word_test(pc, rand16());
			return 2;
		}
		break;
	case imm0:
	{
		if (srcdst)
			return -2;
		uae_u16 v = rand16();
		if ((imm8_cnt & 3) == 0)
			v &= 0xff;
		imm8_cnt++;
		put_word_test(pc, v);
		return 2;
	}
	case imm1:
	{
		if (srcdst)
			return -2;
		uae_u16 v = rand16();
		put_word_test(pc, v);
		return 2;
	}
	case imm2:
	{
		if (srcdst)
			return -2;
		uae_u32 v = rand32();
		put_long_test(pc, v);
		return 4;
	}
	}
	return -2;
}

static int create_ea(uae_u16 *opcodep, uaecptr pc, int mode, int reg, struct instr *dp, int *isconstant, int srcdst, int fpuopcode, int opcodesize, uae_u32 *ea)
{
	int am = mode >= imm ? imm : mode;

	if (!((1 << am) & feature_addressing_modes[srcdst]))
		return -1;

	if (target_ea[srcdst] == 0xffffffff) {
		return create_ea_random(opcodep, pc, mode, reg, dp, isconstant, srcdst, fpuopcode, opcodesize, ea);
	} else {
		return create_ea_exact(opcodep, pc, mode, reg, dp, isconstant, srcdst, fpuopcode, opcodesize, ea);
	}
}

static int imm_special;

static int handle_specials_preea(uae_u16 opcode, uaecptr pc, struct instr *dp)
{
	if (dp->mnemo == i_FTRAPcc) {
		uae_u16 v = rand16();
		v &= ~7;
		v |= imm_special;
		put_word_test(pc, v);
		imm_special++;
		return 2;
	}
	if (dp->mnemo == i_MOVE16) {
		if (opcode & 0x20) {
			uae_u16 v = 0;
			v |= imm_special << 12;
			put_word_test(pc, v);
			imm_special++;
			return 2;
		}
	}
	return 0;
}

static int handle_specials_branch(uae_u16 opcode, uaecptr pc, struct instr *dp, int *isconstant)
{
	// 68020 BCC.L is BCC.B to odd address if 68000/010
	if ((opcode & 0xf0ff) == 0x60ff) {
		if (currprefs.cpu_model >= 68020) {
			return 0;
		}
		return -2;
	} else if (dp->mnemo == i_FDBcc) {
		// FDBcc jump offset
		uae_u16 v = rand16();
		put_word_test(pc, v);
		*isconstant = 16;
		return 2;
	}
	return 0;
}

static int handle_specials_pack(uae_u16 opcode, uaecptr pc, struct instr *dp, int *isconstant)
{
	// PACK and UNPK has third parameter
	if (dp->mnemo == i_PACK || dp->mnemo == i_UNPK) {
		uae_u16 v = rand16();
		put_word_test(pc, v);
		*isconstant = 16;
		return 2;
	}
	return 0;
}

static uaecptr handle_specials_extra(uae_u16 opcode, uaecptr pc, struct instr *dp)
{
	// cas undocumented (marked as zero in document) fields do something weird, for example
	// setting bit 9 will make "Du" address register but results are not correct.
	// so lets make sure unused zero bits are zeroed.
	switch (dp->mnemo)
	{
	case i_CAS:
	{
		uae_u16 extra = get_word_test(opcode_memory_start + 2);
		uae_u16 extra2 = extra;
		extra &= (7 << 6) | (7 << 0);
		if (extra != extra2) {
			put_word_test(opcode_memory_start + 2, extra);
		}
		break;
	}
	case i_CAS2:
	{
		uae_u16 extra = get_word_test(opcode_memory_start + 2);
		uae_u16 extra2 = extra;
		extra &= (7 << 6) | (7 << 0) | (15 << 12);
		if (extra != extra2) {
			put_word_test(opcode_memory_start + 2, extra);
		}
		extra = get_word_test(opcode_memory_start + 4);
		extra2 = extra;
		extra &= (7 << 6) | (7 << 0) | (15 << 12);
		if (extra != extra2) {
			put_word_test(opcode_memory_start + 4, extra);
		}
		break;
	}
	case i_CHK2: // also CMP2
	{
		uae_u16 extra = get_word_test(opcode_memory_start + 2);
		uae_u16 extra2 = extra;
		extra &= (31 << 11);
		if (extra != extra2) {
			put_word_test(opcode_memory_start + 2, extra);
		}
		break;
	}
	case i_BFINS:
	case i_BFFFO:
	case i_BFEXTS:
	case i_BFEXTU:
	{
		if (cpu_lvl >= 4) {
			// 68040+ and extra word bit 15 not zero (hidden A/D field):
			// REGISTER field becomes address register in some internal
			// operations, results are also wrong. So clear it here..
			uae_u16 extra = get_word_test(opcode_memory_start + 2);
			if (extra & 0x8000) {
				extra &= ~0x8000;
				put_word_test(opcode_memory_start + 2, extra);
			}
		}
		break;
	}
	case i_DIVL:
	case i_MULL:
	{
		if (cpu_lvl >= 4) {
			// same as BF instructions but also other bits need clearing
			// or results are unexplained..
			uae_u16 extra = get_word_test(opcode_memory_start + 2);
			if (extra & 0x83f8) {
				extra &= ~0x83f8;
				put_word_test(opcode_memory_start + 2, extra);
			}
		}
		break;
	}
	}
	return pc;
}

static uae_u32 generate_stack_return(int cnt)
{
	uae_u32 v = rand32();
	switch (cnt & 3)
	{
	case 0:
	case 3:
		v = opcode_memory_start + 128;
		break;
	case 1:
		v &= 0xffff;
		if (test_low_memory_start == 0xffffffff)
			v |= 0x8000;
		if (test_high_memory_start == 0xffffffff)
			v &= 0x7fff;
		break;
	case 2:
		v = opcode_memory_start + (uae_s16)v;
		break;
	}
	if (!feature_exception3_instruction)
		v &= ~1;
	return v;
}

static int handle_rte(uae_u16 opcode, uaecptr pc, struct instr *dp, int *isconstant, uaecptr addr)
{
	// skip bus error/address error frames because internal fields can't be simply randomized
	int offset = 2;
	int frame, v;

	imm_special++;
	for (;;) {
		frame = (imm_special >> 2) & 15;
		// 68010 bus/address error
		if (cpu_lvl == 1 && (frame == 8)) {
			imm_special += 4;
			continue;
		}
		if ((cpu_lvl == 2 || cpu_lvl == 3) && (frame == 9 || frame == 10 || frame == 11)) {
			imm_special += 4;
			continue;
		}
		// 68040 FP post-instruction, FP unimplemented, Address error
		if (cpu_lvl == 4 && (frame == 3 || frame == 4 || frame == 7)) {
			imm_special += 4;
			continue;
		}
		// 68060 access fault/FP disabled, FP post-instruction
		if (cpu_lvl == 5 && (frame == 3 || frame == 4)) {
			imm_special += 4;
			continue;
		}
		// throwaway frame
		if (frame == 1) {
			imm_special += 4;
			continue;
		}
		break;	
	}
	v = imm_special >> 6;
	uae_u16 sr = v & 31;
	sr |= (v >> 5) << 12;
	put_word_test(addr, sr);
	addr += 2 + 4;
	// frame + vector offset
	put_word_test(addr, (frame << 12) | (rand16() & 0x0fff));
	addr += 2;
#if 0
	if (frame == 1) {
		int imm_special_tmp = imm_special;
		imm_special &= ~(15 << 2);
		if (rand8() & 1)
			imm_special |= 2 << 2;
		handle_rte(opcode, addr, dp, isconstant, addr);
		imm_special = imm_special_tmp;
		v = generate_stack_return(imm_special);
		put_long_test(addr + 2, v);
		offset += 8 + 2;
#endif
	if (frame == 2) {
		put_long_test(addr, rand32());
	}
	return offset;
}

static int handle_specials_stack(uae_u16 opcode, uaecptr pc, struct instr *dp, int *isconstant)
{
	int offset = 0;
	if (dp->mnemo == i_RTE || dp->mnemo == i_RTD || dp->mnemo == i_RTS || dp->mnemo == i_RTR || dp->mnemo == i_UNLK) {
		uae_u32 v;
		uaecptr addr = regs.regs[8 + 7];
		// RTE, RTD, RTS and RTR
		if (dp->mnemo == i_RTR) {
			// RTR
			v = imm_special++;
			uae_u16 ccr = v & 31;
			ccr |= rand16() & ~31;
			put_word_test(addr, ccr);
			addr += 2;
			offset += 2;
			*isconstant = imm_special >= (1 << (0 + 5)) * 4 ? 0 : -1;
		} else if (dp->mnemo == i_RTE) {
			// RTE
			if (currprefs.cpu_model == 68000) {
				imm_special++;
				v = imm_special >> 2;
				uae_u16 sr = v & 31;
				sr |= (v >> 5) << 12;
				put_word_test(addr, sr);
				addr += 2;
				offset += 2;
			} else {
				offset += handle_rte(opcode, pc, dp, isconstant, addr);
				addr += 2;
			}
			*isconstant = imm_special >= (1 << (4 + 5)) * 4 ? 0 : -1;
		} else if (dp->mnemo == i_RTS) {
			// RTS
			imm_special++;
			*isconstant = imm_special >= 256 ? 0 : -1;
		}
		v = generate_stack_return(imm_special);
		put_long_test(addr, v);
		if (out_of_test_space) {
			wprintf(_T("handle_specials out of bounds access!?"));
			abort();
		}
	}
	return offset;
}

static void execute_ins(uae_u16 opc, uaecptr endpc, uaecptr targetpc, struct instr *dp)
{
	uae_u16 opw1 = (opcode_memory[2] << 8) | (opcode_memory[3] << 0);
	uae_u16 opw2 = (opcode_memory[4] << 8) | (opcode_memory[5] << 0);
	if (opc == 0x89c2
		//&& opw1 == 0xcf19
		//&& opw2 == 0x504e
		)
		printf("");
	if (regs.sr & 0x2000)
		printf("");

	// execute instruction
	SPCFLAG_TRACE = 0;
	SPCFLAG_DOTRACE = 0;
	mmufixup[0].reg = -1;
	mmufixup[1].reg = -1;

	MakeFromSR();

	low_memory_accessed = 0;
	high_memory_accessed = 0;
	test_memory_accessed = 0;
	testing_active = 1;
	testing_active_opcode = opc;
	cpu_bus_error = 0;

	int cnt = feature_loop_mode * 2;
	if (multi_mode)
		cnt = 100;

	for (;;) {

		if (SPCFLAG_TRACE)
			do_trace();

		regs.instruction_pc = regs.pc;
		uaecptr a7 = regs.regs[15];
		int s = regs.s;

		(*cpufunctbl[opc])(opc);

		// Supervisor mode and A7 was modified: skip this test round.
		if (s && regs.regs[15] != a7) {
			// but not if RTE
			if (!is_superstack_use_required()) {
				test_exception = -1;
				break;
			}
		}

		// skip test if SR interrupt mask got too low
		if (regs.intmask < feature_min_interrupt_mask) {
			test_exception = -1;
			break;
		}

		if (!test_exception) {
			if (SPCFLAG_DOTRACE)
				Exception(9);

			if (cpu_stopped && regs.s == 0 && currprefs.cpu_model <= 68010) {
				// 68000/68010 undocumented special case:
				// if STOP clears S-bit and T was not set:
				// cause privilege violation exception, PC pointing to following instruction.
				// If T was set before STOP: STOP works as documented.
				cpu_stopped = 0;
				Exception(8);
			}
		}

		// Amiga Chip ram does not support TAS or MOVE16
		if ((dp->mnemo == i_TAS || dp->mnemo == i_MOVE16) && low_memory_accessed) {
			test_exception = -1;
			break;
		}

		if (regs.pc == endpc || regs.pc == targetpc)
			break;

		if (test_exception)
			break;

		if (!valid_address(regs.pc, 2, 0))
			break;

		if (!feature_loop_mode && !multi_mode) {
			wprintf(_T("Test instruction didn't finish in single step in non-loop mode!?\n"));
			abort();
		}

		if (cpu_lvl < 2) {
			opc = get_word_test_prefetch(2);
		} else {
			opc = get_word_test_prefetch(0);
		}

		cnt--;
		if (cnt <= 0) {
			wprintf(_T("Loop mode didn't end!?\n"));
			abort();
		}
	}

	testing_active = 0;

	if (regs.s) {
		regs.regs[15] = regs.usp;
	}
}

// any instruction that can branch execution
static int isbranchinst(struct instr *dp)
{
	switch (dp->mnemo)
	{
	case i_Bcc:
	case i_BSR:
	case i_JMP:
	case i_JSR:
		return 1;
	case i_RTS:
	case i_RTR:
	case i_RTD:
	case i_RTE:
		return 2;
	case i_DBcc:
	case i_FBcc:
	case i_FDBcc:
		return -1;
	}
	return 0;
}

// only test instructions that can have
// special trace behavior
static int is_test_trace(struct instr *dp)
{
	if (isbranchinst(dp))
		return 1;
	switch (dp->mnemo)
	{
	case i_STOP:
	case i_MV2SR:
		return 1;
	}
	return 0;
}

static int isunsupported(struct instr *dp)
{
	switch (dp->mnemo)
	{
	case i_MOVE2C:
	case i_FSAVE:
	case i_FRESTORE:
	case i_PFLUSH:
	case i_PFLUSHA:
	case i_PFLUSHAN:
	case i_PFLUSHN:
	case i_PTESTR:
	case i_PTESTW:
	case i_CPUSHA:
	case i_CPUSHL:
	case i_CPUSHP:
	case i_CINVA:
	case i_CINVL:
	case i_CINVP:
	case i_PLPAR:
	case i_PLPAW:
		return 1;
	}
	return 0;
}

static int noregistercheck(struct instr *dp)
{
	return 0;
}

static uae_u8 last_exception[256];
static int last_exception_len;

static uae_u8 *save_exception(uae_u8 *p, struct instr *dp)
{
	uae_u8 *op = p;
	p++;
	uae_u8 *sf = test_memory + test_memory_size + EXTRA_RESERVED_SPACE - exception_stack_frame_size;
	// parse exception and store fields that are unique
	// SR and PC was already saved with non-exception data
	if (cpu_lvl == 0) {
		if (test_exception == 2 || test_exception == 3) {
			// status
			*p++ = sf[1];
			// opcode (which is not necessarily current opcode!)
			*p++ = sf[6];
			*p++ = sf[7];
			// access address
			p = store_rel(p, 0, opcode_memory_start, gl(sf + 2), 1);
		}
	} else if (cpu_lvl > 0) {
		uae_u8 ccrmask = 0;
		uae_u16 frame = (sf[6] << 8) | sf[7];
		// frame + vector offset
		*p++ = sf[6];
		*p++ = sf[7];
		switch (frame >> 12)
		{
		case 0:
			break;
		case 2:
			// instruction address
			p = store_rel(p, 0, opcode_memory_start, gl(sf + 8), 1);
			break;
		case 3:
			// effective address
			p = store_rel(p, 0, opcode_memory_start, gl(sf + 8), 1);
			break;
		case 4: // floating point unimplemented (68040/060)
			// fault/effective address
			p = store_rel(p, 0, opcode_memory_start, gl(sf + 8), 1);
			// FSLW or PC of faulted instruction
			p = store_rel(p, 0, opcode_memory_start, gl(sf + 12), 1);
			break;
		case 0x0a: // 68020/030 address error.
		case 0x0b: // Don't save anything extra, too many undefined fields and bits..
			exception_stack_frame_size = 0x08;
			break;
		default:
			wprintf(_T("Unknown frame %04x!\n"), frame);
			abort();
		}
	}
	if (last_exception_len > 0 && last_exception_len == exception_stack_frame_size && !memcmp(sf, last_exception, exception_stack_frame_size)) {
		// stack frame was identical to previous
		p = op;
		*p++ = 0xff;
	} else {
		int datalen = (p - op) - 1;
		last_exception_len = exception_stack_frame_size;
		*op = (uae_u8)datalen;
		memcpy(last_exception, sf, exception_stack_frame_size);
	}
	return p;
}

static uae_u16 get_ccr_ignore(struct instr *dp, uae_u16 extra)
{
	uae_u16 ccrignoremask = 0;
	if ((cpu_lvl == 2 || cpu_lvl == 3) && test_exception == 5) {
		if ((dp->mnemo == i_DIVS) || (dp->mnemo == i_DIVL && (extra & 0x0800) && !(extra & 0x0400))) {
			// 68020/030 DIVS.W/.L + Divide by Zero: V state is not stable.
			ccrignoremask |= 2; // mask CCR=V
		}
	}
	return ccrignoremask;
}

static int isfpp(int mnemo)
{
	switch (mnemo)
	{
	case i_FPP:
	case i_FBcc:
	case i_FDBcc:
	case i_FTRAPcc:
	case i_FScc:
	case i_FRESTORE:
	case i_FSAVE:
		return 1;
	}
	return 0;
}


static void generate_target_registers(uae_u32 target_address, uae_u32 *out)
{
	uae_u32 *a = out + 8;
	a[0] = target_address;
	a[1] = target_address + 1;
	a[2] = target_address + 2;
	a[3] = target_address + 4;
	a[4] = target_address - 6;
	a[5] = target_address + 6;
	for (int i = 0; i < 7; i++) {
		out[i] = i - 4;
	}
	out[7] = target_address - opcode_memory_start;
}

static const TCHAR *sizes[] = { _T("B"), _T("W"), _T("L") };

static void test_mnemo(const TCHAR *path, const TCHAR *mnemo, const TCHAR *ovrfilename, int opcodesize, int fpuopcode)
{
	TCHAR dir[1000];
	uae_u8 *dst = NULL;
	int mn;
	int size;
	bool fpumode = 0;

	uae_u32 test_cnt = 0;

	if (mnemo == NULL || mnemo[0] == 0)
		return;

	size = 3;
	if (fpuopcode < 0) {
		if (opcodesize == 0)
			size = 2;
		else if (opcodesize == 4)
			size = 1;
		else if (opcodesize == 6)
			size = 0;
	}

	filecount = 0;

	struct mnemolookup *lookup = NULL;
	for (int i = 0; lookuptab[i].name; i++) {
		lookup = &lookuptab[i];
		if (!_tcsicmp(lookup->name, mnemo) || (lookup->friendlyname && !_tcsicmp(lookup->friendlyname, mnemo)))
			break;
	}
	if (!lookup) {
		wprintf(_T("'%s' not found.\n"), mnemo);
		return;
	}

	mn = lookup->mnemo;
	xorshiftstate = lookup - lookuptab;
	const TCHAR *mns = lookup->name;
	if (fpuopcode >= 0) {
		xorshiftstate = 128 + fpuopcode;
		mns = fpuopcodes[fpuopcode];
		if (opcodesize == 7) {
			mns = _T("FMOVECR");
			xorshiftstate = 128 + 64;
		} else if (opcodesize == 8) {
			mns = _T("FMOVEM");
			xorshiftstate = 128 + 64 + 1;
		}
	}
	xorshiftstate += 256 * opcodesize;
	if (ovrfilename) {
		mns = ovrfilename;
		for (int i = 0; i < _tcslen(ovrfilename); i++) {
			xorshiftstate <<= 1;
			xorshiftstate ^= ovrfilename[i];
		}
	}

	int pathlen = _tcslen(path);
	_stprintf(dir, _T("%s%s"), path, mns);
	if (fpuopcode < 0) {
		if (size < 3) {
			_tcscat(dir, _T("."));
			_tcscat(dir, sizes[size]);
		}
	} else {
		_tcscat(dir, _T("."));
		_tcscat(dir, fpsizes[opcodesize < 7 ? opcodesize : 2]);
	}
	memset(inst_name, 0, sizeof(inst_name));
	ua_copy(inst_name, sizeof(inst_name), dir + pathlen);

	opcodecnt = 0;
	for (int opcode = 0; opcode < 65536; opcode++) {
		struct instr *dp = table68k + opcode;
		// match requested mnemonic
		if (dp->mnemo != lookup->mnemo)
			continue;
		// match requested size
		if ((size >= 0 && size <= 2) && (size != dp->size || dp->unsized))
			continue;
		if (size == 3 && !dp->unsized)
			continue;
		// skip all unsupported instructions if not specifically testing i_ILLG
		if (dp->clev > cpu_lvl && lookup->mnemo != i_ILLG)
			continue;
		opcodecnt++;
		if (isunsupported(dp))
			return;
		if (isfpp(lookup->mnemo) && !currprefs.fpu_model)
			return;
		fpumode = currprefs.fpu_model && isfpp(lookup->mnemo);
	}

	if (!opcodecnt)
		return;

	wprintf(_T("%s\n"), dir);

	int quick = 0;
	int rounds = feature_test_rounds;
	int subtest_count = 0;

	int count = 0;

	registers[8 + 6] = opcode_memory_start - 0x100;
	registers[8 + 7] = test_memory_end - STACK_SIZE;

	uae_u32 target_address = 0xffffffff;
	target_ea[0] = 0xffffffff;
	target_ea[1] = 0xffffffff;
	if (feature_target_ea[0][0] != 0xffffffff) {
		target_address = feature_target_ea[0][0];
		target_ea[0] = target_address;
	} else if (feature_target_ea[0][1] != 0xffffffff) {
		target_address = feature_target_ea[0][1];
		target_ea[1] = target_address;
	}
	target_ea_src_cnt = 0;
	target_ea_dst_cnt = 0;

	// 1.0
	fpuregisters[0].high = 0x3fff;
	fpuregisters[0].low = 0x8000000000000000;
	// -1.0
	fpuregisters[1].high = 0xbfff;
	fpuregisters[1].low = 0x8000000000000000;
	// 0.0
	fpuregisters[2].high = 0x0000;
	fpuregisters[2].low = 0x0000000000000000;
	// NaN
	fpuregisters[3].high = 0x7fff;
	fpuregisters[3].low = 0xffffffffffffffff;
	// inf+
	fpuregisters[4].high = 0x7fff;
	fpuregisters[4].low = 0x0000000000000000;
	// inf-
	fpuregisters[5].high = 0xffff;
	fpuregisters[5].low = 0x0000000000000000;

	for (int i = 6; i < 8; i++) {
		fpuregisters[i].high = rand16();
		fpuregisters[i].low = (((uae_u64)rand32()) << 32) | (rand32());
	}

	for (int i = 0; i < MAX_REGISTERS; i++) {
		cur_registers[i] = registers[i];
	}
	floatx80 cur_fpuregisters[8];
	for (int i = 0; i < 8; i++) {
		cur_fpuregisters[i] = fpuregisters[i];
	}

	if (target_address != 0xffffffff) {
		generate_target_registers(target_address, cur_registers);
	}

	dst = storage_buffer;

	memcpy(low_memory, low_memory_temp, low_memory_size);
	memcpy(high_memory, high_memory_temp, high_memory_size);
	memcpy(test_memory, test_memory_temp, test_memory_size);

	full_format_cnt = 0;
	last_exception_len = -1;

	int sr_override = 0;

	for (;;) {

		if (quick)
			break;

		if (feature_loop_mode) {
			regs.regs[feature_loop_mode_register] &= 0xffff0000;
			regs.regs[feature_loop_mode_register] |= feature_loop_mode - 1;
			cur_registers[feature_loop_mode_register] = regs.regs[feature_loop_mode_register];
		}

		for (int i = 0; i < MAX_REGISTERS; i++) {
			dst = store_reg(dst, CT_DREG + i, 0, cur_registers[i], -1);
			regs.regs[i] = cur_registers[i];
		}
		for (int i = 0; i < 8; i++) {
			dst = store_fpureg(dst, CT_FPREG + i, cur_fpuregisters[i]);
			regs.fp[i].fpx = cur_fpuregisters[i];
		}
		regs.sr = feature_min_interrupt_mask << 8;

		uae_u32 srcaddr_old = 0xffffffff;
		uae_u32 dstaddr_old = 0xffffffff;
		uae_u32 srcaddr = 0xffffffff;
		uae_u32 dstaddr = 0xffffffff;

		for (int opcode = 0; opcode < 65536; opcode++) {

			struct instr *dp = table68k + opcode;
			// match requested mnemonic
			if (dp->mnemo != lookup->mnemo)
				continue;

			// match requested size
			if ((size >= 0 && size <= 2) && (size != dp->size || dp->unsized))
				continue;
			if (size == 3 && !dp->unsized)
				continue;
			// skip all unsupported instructions if not specifically testing i_ILLG
			if (dp->clev > cpu_lvl && lookup->mnemo != i_ILLG)
				continue;

			// not supported yet in target mode
			if (target_address != 0xffffffff) {
				if (dp->mnemo == i_MVMEL || dp->mnemo == i_MVMLE)
					continue;
			}

			int extra_loops = 3;
			while (extra_loops-- > 0) {

				// force both odd and even immediate values
				// for better address error testing
				forced_immediate_mode = 0;
				if ((currprefs.cpu_model == 68000 || currprefs.cpu_model == 68010) && (feature_exception3_data == 1 || feature_exception3_instruction == 1)) {
					if (dp->size > 0) {
						if (extra_loops == 1)
							forced_immediate_mode = 1;
						if (extra_loops == 0)
							forced_immediate_mode = 2;
					} else {
						extra_loops = 0;
					}
				} else if (currprefs.cpu_model == 68020 && ((ad8r[0] & 3) == 2 || (pc8r[0] & 3) == 2 || (ad8r[1] & 3) == 2 || (pc8r[1] & 3) == 2)) {
					; // if only 68020+ addressing modes: do extra loops
				} else {
					extra_loops = 0;
				}

				imm8_cnt = 0;
				imm16_cnt = 0;
				imm32_cnt = 0;
				immabsl_cnt = 0;
				imm_special = 0;

				// retry few times if out of bounds access
				int oob_retries = 10;
				// if instruction has immediate(s), repeat instruction test multiple times
				// each round generates new random immediate(s)
				int constant_loops = 32;
				while (constant_loops-- > 0) {
					uae_u8 oldbytes[OPCODE_AREA];
					memcpy(oldbytes, opcode_memory, sizeof(oldbytes));

					uae_u16 opc = opcode;
					int isconstant_src = 0;
					int isconstant_dst = 0;
					int did_out_of_bounds = 0;
					uae_u8 *prev_dst2 = dst;

					out_of_test_space = 0;
					ahcnt = 0;
					multi_mode = 0;

					if (opc == 0x0156)
						printf("");
					if (subtest_count == 416)
						printf("");


					uaecptr pc = opcode_memory_start + 2;

					pc += handle_specials_preea(opc, pc, dp);


					uae_u32 srcea = 0xffffffff;
					uae_u32 dstea = 0xffffffff;

					// create source addressing mode
					if (dp->suse) {
						int o = create_ea(&opc, pc, dp->smode, dp->sreg, dp, &isconstant_src, 0, fpuopcode, opcodesize, &srcea);
						if (o < 0) {
							memcpy(opcode_memory, oldbytes, sizeof(oldbytes));
							if (o == -1)
								goto nextopcode;
							continue;
						}
						pc += o;
					}

					uae_u8 *ao = opcode_memory + 2;
					uae_u16 apw1 = (ao[0] << 8) | (ao[1] << 0);
					uae_u16 apw2 = (ao[2] << 8) | (ao[3] << 0);
					if (opc == 0x3fb2
						&& apw1 == 0xa190
						&& apw2 == 0x2770
						)
						printf("");

					// if source EA modified opcode
					dp = table68k + opc;

					// create destination addressing mode
					if (dp->duse) {
						int o = create_ea(&opc, pc, dp->dmode, dp->dreg, dp, &isconstant_dst, 1, fpuopcode, opcodesize, &dstea);
						if (o < 0) {
							memcpy(opcode_memory, oldbytes, sizeof(oldbytes));
							if (o == -1)
								goto nextopcode;
							continue;
						}
						pc += o;
					}

					// requested target address but no EA? skip
					if (target_address != 0xffffffff) {
						if (srcea != target_address && dstea != target_address) {
							memcpy(opcode_memory, oldbytes, sizeof(oldbytes));
							continue;
						}
					}


					pc = handle_specials_extra(opc, pc, dp);

					// if destination EA modified opcode
					dp = table68k + opc;

					uae_u8 *bo = opcode_memory + 2;
					uae_u16 bopw1 = (bo[0] << 8) | (bo[1] << 0);
					uae_u16 bopw2 = (bo[2] << 8) | (bo[3] << 0);
					if (opc == 0xf228
						&& bopw1 == 0x003a
						//&& bopw2 == 0x2770
						)
						printf("");

					// bcc.x
					pc += handle_specials_branch(opc, pc, dp, &isconstant_src);
					// pack
					pc += handle_specials_pack(opc, pc, dp, &isconstant_src);

					put_word_test(opcode_memory_start, opc);

					if (extra_or || extra_and) {
						uae_u16 ew = get_word_test(opcode_memory_start + 2);
						uae_u16 ew2 = (ew | extra_or) & ~extra_and;
						if (ew2 != ew) {
							put_word_test(opcode_memory_start + 2, ew2);
						}
					}

					// loop mode
					if (feature_loop_mode) {
						// dbf dn, opcode_memory_start
						put_long_test(pc, ((0x51c8 | feature_loop_mode_register) << 16) | ((opcode_memory_start - pc - 2) & 0xffff));
						pc += 4;
					}

					// end word, needed to detect if instruction finished normally when
					// running on real hardware.
					put_word_test(pc, 0x4afc); // illegal instruction
					pc += 2;

					if (isconstant_src < 0 || isconstant_dst < 0) {
						constant_loops++;
						quick = 1;
					} else {
						// the smaller the immediate, the less test loops
						if (constant_loops > isconstant_src && constant_loops > isconstant_dst)
							constant_loops = isconstant_dst > isconstant_src ? isconstant_dst : isconstant_src;

						// don't do extra tests if no immediates
						if (!isconstant_dst && !isconstant_src)
							extra_loops = 0;
					}

					if (out_of_test_space) {
						wprintf(_T("Setting up test instruction generated out of bounds error!?\n"));
						abort();
					}

					dst = store_mem_bytes(dst, opcode_memory_start, pc - opcode_memory_start, oldbytes);
					ahcnt = 0;


					TCHAR out[256];
					memset(out, 0, sizeof(out));
					// disassemble and output generated instruction
					for (int i = 0; i < MAX_REGISTERS; i++) {
						regs.regs[i] = cur_registers[i];
					}
					for (int i = 0; i < 8; i++) {
						regs.fp[i].fpx = cur_fpuregisters[i];
					}
					uaecptr nextpc;
					srcaddr = 0xffffffff;
					dstaddr = 0xffffffff;
					uae_u32 dflags = m68k_disasm_2(out, sizeof(out) / sizeof(TCHAR), opcode_memory_start, &nextpc, 1, &srcaddr, &dstaddr, 0xffffffff, 0);
					if (verbose) {
						my_trim(out);
						wprintf(_T("%08u %s"), subtest_count, out);
					}

					if ((dflags & 1) && target_ea[0] != 0xffffffff && srcaddr != 0xffffffff && srcaddr != target_ea[0]) {
						wprintf(_T("\nSource address mismatch %08x <> %08x\n"), target_ea[0], srcaddr);
						abort();
					}
					if ((dflags & 2) && target_ea[1] != 0xffffffff && dstaddr != target_ea[1]) {
						wprintf(_T("\nDestination address mismatch %08x <> %08x\n"), target_ea[1], dstaddr);
						abort();
					}

					if ((dflags & 1) && target_ea[0] == 0xffffffff && (srcaddr & addressing_mask) >= safe_memory_start - 4 && (srcaddr & addressing_mask) < safe_memory_end + 4) {
						// random generated EA must never be inside safe memory
						continue;
					}
					if ((dflags & 2) && target_ea[1] == 0xffffffff && (dstaddr & addressing_mask) >= safe_memory_start - 4 && (dstaddr & addressing_mask) < safe_memory_end + 4) {
						// random generated EA must never be inside safe memory
						continue;
					}

#if 0
					// can't test because dp may be empty if instruction is invalid
					if (nextpc != pc - 2) {
						wprintf(_T("Disassembler/generator instruction length mismatch!\n"));
						abort();
					}
#endif
					uaecptr branch_target = 0xffffffff;
					int bc = isbranchinst(dp);
					if (bc) {
						if (bc < 0) {
							srcaddr = dstaddr;
						}
						if (bc == 2) {
							// RTS and friends
							int stackoffset = handle_specials_stack(opc, pc, dp, &isconstant_src);
							if (isconstant_src < 0 || isconstant_dst < 0) {
								constant_loops++;
								quick = 0;
							}
							srcaddr = get_long_test(regs.regs[8 + 7] + stackoffset);
						}
						// branch target is not accessible? skip.
						if ((srcaddr >= cur_registers[15] - 16 && srcaddr <= cur_registers[15] + 16) || ((srcaddr & 1) && !feature_exception3_instruction)) {
							// lets not jump directly to stack..
							prev_dst2 = dst;
							if (verbose) {
								if (srcaddr & 1)
									wprintf(_T(" Branch target is odd\n"));
								else
									wprintf(_T(" Branch target is stack\n"));
							}
							continue;
						}				
						testing_active = 1;
						if (!valid_address(srcaddr, 2, 1)) {
							if (verbose) {
								wprintf(_T(" Branch target inaccessible\n"));
							}
							testing_active = 0;
							prev_dst2 = dst;
							continue;
						} else {
							// branch target = generate exception
							if (!is_nowrite_address(srcaddr, 2)) {
								put_word_test(srcaddr, 0x4afc);
							}
							branch_target = srcaddr;
							dst = store_mem(dst, 1);
							memcpy(&ahist2, &ahist, sizeof(struct accesshistory) *MAX_ACCESSHIST);
							ahcnt2 = ahcnt;
						}
						testing_active = 0;
					}

					if (srcaddr != srcaddr_old && (dflags & 1)) {
						dst = store_reg(dst, CT_SRCADDR, srcaddr_old, srcaddr, -1);
						srcaddr_old = srcaddr;
					}
					if (dstaddr != dstaddr_old && (dflags & 2)) {
						dst = store_reg(dst, CT_DSTADDR, dstaddr_old, dstaddr, -1);
						dstaddr_old = dstaddr;
					}

					*dst++ = CT_END_INIT;

					int exception_array[256] = { 0 };
					int ok = 0;
					int cnt_stopped = 0;

					uae_u32 last_sr = 0;
					uae_u32 last_pc = 0;
					uae_u32 last_registers[MAX_REGISTERS];
					floatx80 last_fpuregisters[8];
					uae_u32 last_fpiar = 0;
					uae_u32 last_fpsr = 0;
					uae_u32 last_fpcr = 0;

					int ccr_done = 0;
					int prev_s_cnt = 0;
					int s_cnt = 0;
					int t_cnt = 0;

					int extraccr = 0;

					// extra loops for supervisor and trace
					uae_u16 sr_allowed_mask = feature_sr_mask & 0xf000;
					uae_u16 sr_mask_request = feature_sr_mask & 0xf000;

					for (;;) {
						uae_u16 sr_mask = 0;

						int maxflag = fpumode ? 256 : 32;
						if (feature_flag_mode == 1) {
							maxflag = fpumode ? 256 / 8 : 2;
						}

						if (extraccr & 1)
							sr_mask |= 0x2000; // S
						if (extraccr & 2)
							sr_mask |= 0x4000; // T0
						if (extraccr & 4)
							sr_mask |= 0x8000; // T1
						if (extraccr & 8)
							sr_mask |= 0x1000; // M

						if((sr_mask & ~sr_allowed_mask) || (sr_mask & ~sr_mask_request))
							goto nextextra;

						if (extraccr) {
							*dst++ = (uae_u8)extraccr;
						}

						// Test every CPU CCR or FPU SR/rounding/precision combination
						for (int ccr = 0; ccr < maxflag; ccr++) {

							bool skipped = false;

							out_of_test_space = 0;
							test_exception = 0;
							cpu_stopped = 0;
							cpu_halted = 0;
							ahcnt = 0;

							memset(&regs, 0, sizeof(regs));

							regs.pc = opcode_memory_start;
							regs.irc = get_word_test(regs.pc + 2);

							// set up registers
							for (int i = 0; i < MAX_REGISTERS; i++) {
								regs.regs[i] = cur_registers[i];
							}
							if (fpumode) {
								for (int i = 0; i < 8; i++) {
									regs.fp[i].fpx = cur_fpuregisters[i];
								}
								regs.fpiar = regs.pc;
								// condition codes
								if (feature_flag_mode == 0) {
									fpp_set_fpsr((ccr & 15) << 24);
									// precision and rounding
									fpp_set_fpcr((ccr >> 4) << 4);
								} else {
									fpp_set_fpsr(((ccr & 1) ? 15 : 0) << 24);
									// precision and rounding
									fpp_set_fpcr((ccr >> 1) << 4);
								}
							}
							if (feature_flag_mode == 0) {
								regs.sr = ccr | sr_mask;
							} else {
								regs.sr = ((ccr & 1) ? 31 : 0) | sr_mask;
							}
							regs.sr |= feature_min_interrupt_mask << 8;
							regs.usp = regs.regs[8 + 7];
							regs.isp = test_memory_end - 0x80;
							// copy user stack to super stack, for RTE etc support
							memcpy(regs.isp - test_memory_start + test_memory, regs.usp - test_memory_start + test_memory, 0x20);
							regs.msp = test_memory_end;

							// data size optimization, only store data
							// if it is different than in previous round
							if (!ccr && !extraccr) {
								last_sr = regs.sr;
								last_pc = regs.pc;
								for (int i = 0; i < 16; i++) {
									last_registers[i] = regs.regs[i];
								}
								for (int i = 0; i < 8; i++) {
									last_fpuregisters[i] = regs.fp[i].fpx;
								}
								last_fpiar = regs.fpiar;
								last_fpcr = regs.fpcr;
								last_fpsr = regs.fpsr;
							}

							if (regs.sr & 0x2000)
								prev_s_cnt++;

							if (subtest_count == 23360)
								printf("");

							execute_ins(opc, pc - 2, branch_target, dp);

							if (regs.s)
								s_cnt++;

							// validate PC
							uae_u8 *pcaddr = get_addr(regs.pc, 2, 0);

							// examine results
							if (cpu_stopped) {
								cnt_stopped++;
								// CPU stopped, skip test
								skipped = 1;
							} else if (cpu_halted) {
								// CPU halted or reset, skip test
								skipped = 1;
							} else if (out_of_test_space) {
								exception_array[0]++;
								// instruction accessed memory out of test address space bounds
								skipped = 1;
								did_out_of_bounds = true;
							} else if (test_exception < 0) {
								// something happened that requires test skip
								skipped = 1;
							} else if (test_exception) {
								// generated exception
								exception_array[test_exception]++;
								if (test_exception == 8 && !(sr_mask & 0x2000)) {
									// Privilege violation exception? Switch to super mode in next round.
									sr_mask_request |= 0x2000;
									sr_allowed_mask |= 0x2000;
								}
								if (test_exception == 3) {
									if (!feature_exception3_data && !(test_exception_3_fc & 2)) {
										skipped = 1;
									}
									if (!feature_exception3_instruction && (test_exception_3_fc & 2)) {
										skipped = 1;
									}
								} else {
									if (feature_exception3_data == 2) {
										skipped = 1;
									}
									if (feature_exception3_instruction == 2) {
										skipped = 1;
									}
								}
								if (test_exception == 9) {
									t_cnt++;
								}
							} else {
								// instruction executed successfully
								ok++;
								// validate branch instructions
								if (isbranchinst(dp)) {
									if ((regs.pc != srcaddr && regs.pc != pc - 2) || pcaddr[0] != 0x4a && pcaddr[1] != 0xfc) {
										wprintf(_T("Branch instruction target fault\n"));
										abort();
									}
								}
							}
							MakeSR();
							if (!skipped) {
								bool storeregs = true;
								if (noregistercheck(dp)) {
									*dst++ = CT_SKIP_REGS;
									storeregs = false;
								}
								// save modified registers
								for (int i = 0; i < MAX_REGISTERS; i++) {
									uae_u32 s = last_registers[i];
									uae_u32 d = regs.regs[i];
									if (s != d) {
										if (storeregs) {
											dst = store_reg(dst, CT_DREG + i, s, d, -1);
										}
										last_registers[i] = d;
									}
								}
								uae_u32 ccrignoremask = get_ccr_ignore(dp, ((pcaddr[2] << 8) | pcaddr[3])) << 16;
								if ((regs.sr | ccrignoremask) != last_sr) {
									dst = store_reg(dst, CT_SR, last_sr, regs.sr | ccrignoremask, -1);
									last_sr = regs.sr | ccrignoremask;
								}
								if (regs.pc != last_pc) {
									dst = store_rel(dst, CT_PC, last_pc, regs.pc, 0);
									last_pc = regs.pc;
								}
								if (currprefs.fpu_model) {
									for (int i = 0; i < 8; i++) {
										floatx80 s = last_fpuregisters[i];
										floatx80 d = regs.fp[i].fpx;
										if (s.high != d.high || s.low != d.low) {
											dst = store_fpureg(dst, CT_FPREG + i, d);
											last_fpuregisters[i] = d;
										}
									}
									if (regs.fpiar != last_fpiar) {
										dst = store_rel(dst, CT_FPIAR, last_fpiar, regs.fpiar, 0);
										last_fpiar = regs.fpiar;
									}
									if (regs.fpsr != last_fpsr) {
										dst = store_reg(dst, CT_FPSR, last_fpsr, regs.fpsr, -1);
										last_fpsr = regs.fpsr;
									}
									if (regs.fpcr != last_fpcr) {
										dst = store_reg(dst, CT_FPCR, last_fpcr, regs.fpcr, -1);
										last_fpcr = regs.fpcr;
									}
								}
								dst = store_mem(dst, 0);
								if (test_exception) {
									*dst++ = CT_END | test_exception;
									dst = save_exception(dst, dp);
								} else {
									*dst++ = CT_END;
								}
								test_count++;
								subtest_count++;
								ccr_done++;
							} else {
								*dst++ = CT_END_SKIP;
							}
							undo_memory(ahist, &ahcnt);
						}
					nextextra:
						extraccr++;
						if (extraccr >= 16)
							break;
					}
					*dst++ = CT_END;

					undo_memory(ahist2, &ahcnt2);

					if (!ccr_done) {
						// undo whole previous ccr/extra loop if all tests were skipped
						dst = prev_dst2;
						//*dst++ = CT_END_INIT;
						memcpy(opcode_memory, oldbytes, sizeof(oldbytes));
					} else {
						full_format_cnt++;
					}
					if (verbose) {
						wprintf(_T(" OK=%d OB=%d S=%d/%d T=%d STP=%d"), ok, exception_array[0], prev_s_cnt, s_cnt, t_cnt, cnt_stopped);
						for (int i = 2; i < 128; i++) {
							if (exception_array[i])
								wprintf(_T(" E%d=%d"), i, exception_array[i]);
						}
					}
					if (dst - storage_buffer >= storage_buffer_watermark) {
						if (subtest_count > 0) {
							save_data(dst, dir);
						}
						dst = storage_buffer;
						for (int i = 0; i < MAX_REGISTERS; i++) {
							dst = store_reg(dst, CT_DREG + i, 0, cur_registers[i], -1);
							regs.regs[i] = cur_registers[i];
						}
						if (currprefs.fpu_model) {
							for (int i = 0; i < 8; i++) {
								dst = store_fpureg(dst, CT_FPREG + i, cur_fpuregisters[i]);
								regs.fp[i].fpx = cur_fpuregisters[i];
							}
						}
					}
					if (verbose) {
						wprintf(_T("\n"));
					}
					if (did_out_of_bounds) {
						if (oob_retries) {
							oob_retries--;
							constant_loops++;
						} else {
							full_format_cnt++;
						}
					}
				}
			}
		nextopcode:;
		}

		if (subtest_count > 0) {
			save_data(dst, dir);
		}
		dst = storage_buffer;

		if (opcodecnt == 1 && target_address == 0xffffffff)
			break;
		if (lookup->mnemo == i_ILLG)
			break;

		bool nextround = false;
		if (target_address != 0xffffffff) {
			target_ea_src_cnt++;
			if (target_ea_src_cnt >= target_ea_src_max) {
				target_ea_src_cnt = 0;
				if (target_ea_src_max > 0)
					nextround = true;
			}
			target_ea_dst_cnt++;
			if (target_ea_dst_cnt >= target_ea_dst_max) {
				target_ea_dst_cnt = 0;
				if (target_ea_dst_max > 0)
					nextround = true;
			}
			target_ea[0] = 0xffffffff;
			target_ea[1] = 0xffffffff;
			if (feature_target_ea[target_ea_src_cnt][0] != 0xffffffff) {
				target_address = feature_target_ea[target_ea_src_cnt][0];
				target_ea[0] = target_address;
			} else if (feature_target_ea[target_ea_dst_cnt][1] != 0xffffffff) {
				target_address = feature_target_ea[target_ea_dst_cnt][1];
				target_ea[1] = target_address;
			}
			generate_target_registers(target_address, cur_registers);
		} else {
			// randomize registers
			for (int i = 0; i < 16 - 2; i++) {
				cur_registers[i] = rand32();
			}
			nextround = true;
		}

		if (nextround) {
			rounds--;
			if (rounds < 0)
				break;
		}

		cur_registers[0] &= 0xffff;
		cur_registers[8] &= 0xffff;
		cur_registers[8 + 6]--;
		cur_registers[8 + 7] -= 2;

		for (int i = 0; i < 8; i++) {
			cur_fpuregisters[i].high = rand16();
			cur_fpuregisters[i].low = (((uae_u64)rand32()) << 32) | (rand32());
			if (rand16() & 1) {
				cur_fpuregisters[i].low |= 0x8000000000000000;
			}
		}
	}

	wprintf(_T("- %d tests\n"), subtest_count);
}

static void test_mnemo_text(const TCHAR *path, const TCHAR *mode)
{
	TCHAR modetxt[100];
	int mnemo = -1;
	int fpuopcode = -1;
	int sizes = -1;

	extra_and = 0;
	extra_or = 0;

	_tcscpy(modetxt, mode);
	my_trim(modetxt);
	TCHAR *s = _tcschr(modetxt, '.');
	if (s || feature_instruction_size != NULL) {
		TCHAR c = 0;
		if (s) {
			*s = 0;
			c = _totlower(s[1]);
		}
		if (!c && feature_instruction_size) {
			c = feature_instruction_size[0];
		}
		if (c == 'b' || c == 'B')
			sizes = 6;
		if (c == 'w' || c == 'W')
			sizes = 4;
		if (c == 'l' || c == 'L')
			sizes = 0;
		if (c == 'u' || c == 'U')
			sizes = 8;
		if (c == 's' || c == 'S')
			sizes = 1;
		if (c == 'x' || c == 'X')
			sizes = 2;
		if (c == 'p' || c == 'P')
			sizes = 3;
		if (c == 'd' || c == 'D')
			sizes = 5;
	}

	const TCHAR *ovrname = NULL;
	if (!_tcsicmp(modetxt, _T("DIVUL"))) {
		_tcscpy(modetxt, _T("DIVL"));
		extra_and = 0x0800;
		extra_and |= 0x0400;
		ovrname = _T("DIVUL");
	} else if (!_tcsicmp(modetxt, _T("DIVSL"))) {
		_tcscpy(modetxt, _T("DIVL"));
		extra_or = 0x0800;
		extra_and = 0x0400;
		ovrname = _T("DIVSL");
	} else if (!_tcsicmp(modetxt, _T("DIVS")) && sizes == 0) {
		_tcscpy(modetxt, _T("DIVL"));
		extra_or = 0x0800;
		extra_or |= 0x0400;
		ovrname = _T("DIVS");
	} else if (!_tcsicmp(modetxt, _T("DIVU")) && sizes == 0) {
		_tcscpy(modetxt, _T("DIVL"));
		extra_and = 0x0800;
		extra_or |= 0x0400;
		ovrname = _T("DIVU");
	} else if (!_tcsicmp(modetxt, _T("CHK2"))) {
		_tcscpy(modetxt, _T("CHK2"));
		extra_or = 0x0800;
		ovrname = _T("CHK2");
	} else if (!_tcsicmp(modetxt, _T("CMP2"))) {
		_tcscpy(modetxt, _T("CHK2"));
		extra_and = 0x0800;
		ovrname = _T("CMP2");
	} else if (!_tcsicmp(modetxt, _T("MULS")) && sizes == 0) {
		_tcscpy(modetxt, _T("MULL"));
		extra_or = 0x0800;
		ovrname = _T("MULS");
	} else if (!_tcsicmp(modetxt, _T("MULU")) && sizes == 0) {
		_tcscpy(modetxt, _T("MULL"));
		extra_and = 0x0800;
		ovrname = _T("MULU");
	} else if (!_tcsicmp(modetxt, _T("MOVEC"))) {
		_tcscpy(modetxt, _T("MOVEC2"));
		ovrname = _T("MOVEC");
	}

	for (int j = 0; lookuptab[j].name; j++) {
		if (!_tcsicmp(modetxt, lookuptab[j].name)) {
			mnemo = j;
			break;
		}
	}
	if (mnemo < 0) {
		if (_totlower(modetxt[0]) == 'f') {
			if (!_tcsicmp(modetxt, _T("fmovecr"))) {
				mnemo = i_FPP;
				sizes = 7;
				fpuopcode = 0;
			} else if (!_tcsicmp(modetxt, _T("fmovem"))) {
				mnemo = i_FPP;
				sizes = 8;
				fpuopcode = 0;
			} else {
				for (int i = 0; i < 64; i++) {
					if (fpuopcodes[i] && !_tcsicmp(modetxt, fpuopcodes[i])) {
						mnemo = i_FPP;
						fpuopcode = i;
						break;
					}
				}
			}
		}

		if (mnemo < 0) {
			wprintf(_T("Couldn't find '%s'\n"), modetxt);
			return;
		}
	}

	if (mnemo >= 0 && sizes < 0) {
		if (fpuopcode >= 0) {
			for (int i = 0; i < 7; i++) {
				test_mnemo(path, lookuptab[mnemo].name, ovrname, i, fpuopcode);
			}
		} else {
			test_mnemo(path, lookuptab[mnemo].name, ovrname, 0, -1);
			test_mnemo(path, lookuptab[mnemo].name, ovrname, 4, -1);
			test_mnemo(path, lookuptab[mnemo].name, ovrname, 6, -1);
			test_mnemo(path, lookuptab[mnemo].name, ovrname, -1, -1);
		}
	} else {
		test_mnemo(path, lookuptab[mnemo].name, ovrname, sizes, fpuopcode);
	}
}

static void my_trim(TCHAR *s)
{
	int len;
	while (_tcslen(s) > 0 && _tcscspn(s, _T("\t \r\n")) == 0)
		memmove(s, s + 1, (_tcslen(s + 1) + 1) * sizeof(TCHAR));
	len = _tcslen(s);
	while (len > 0 && _tcscspn(s + len - 1, _T("\t \r\n")) == 0)
		s[--len] = '\0';
}

static const TCHAR *addrmodes[] =
{
	_T("Dreg"), _T("Areg"), _T("Aind"), _T("Aipi"), _T("Apdi"), _T("Ad16"), _T("Ad8r"),
	_T("absw"), _T("absl"), _T("PC16"), _T("PC8r"), _T("imm"), _T("Ad8rf"), _T("PC8rf"),
	NULL
};

#define INISECTION _T("cputest")

int __cdecl main(int argc, char *argv[])
{
	const struct cputbl *tbl = NULL;
	TCHAR path[1000], *vs;
	int v;

	struct ini_data *ini = ini_load(_T("cputestgen.ini"), false);
	if (!ini) {
		wprintf(_T("Couldn't open cputestgen.ini"));
		return 0;
	}

	currprefs.cpu_model = 68000;
	ini_getval(ini, INISECTION, _T("cpu"), &currprefs.cpu_model);
	if (currprefs.cpu_model != 68000 && currprefs.cpu_model != 68010 && currprefs.cpu_model != 68020 &&
		currprefs.cpu_model != 68030 && currprefs.cpu_model != 68040 && currprefs.cpu_model != 68060) {
		wprintf(_T("Unsupported CPU model.\n"));
		return 0;
	}

	currprefs.address_space_24 = 1;
	addressing_mask = 0x00ffffff;
	v = 24;
	ini_getval(ini, INISECTION, _T("cpu_address_space"), &v);
	if (v == 32 || currprefs.cpu_model >= 68030) {
		currprefs.address_space_24 = 0;
		addressing_mask = 0xffffffff;
	}

	currprefs.fpu_model = 0;
	currprefs.fpu_mode = 1;
	ini_getval(ini, INISECTION, _T("fpu"), &currprefs.fpu_model);
	if (currprefs.fpu_model && currprefs.cpu_model < 68020) {
		wprintf(_T("FPU requires 68020 or 68040 CPU.\n"));
		return 0;
	}
	if (currprefs.fpu_model != 0 && currprefs.fpu_model != 68881 && currprefs.fpu_model != 68882 && currprefs.fpu_model != 68040 && currprefs.fpu_model != 68060) {
		wprintf(_T("Unsupported FPU model.\n"));
		return 0;
	}

	verbose = 1;
	ini_getval(ini, INISECTION, _T("verbose"), &verbose);

	feature_addressing_modes[0] = 0xffffffff;
	feature_addressing_modes[1] = 0xffffffff;
	ad8r[0] = ad8r[1] = 1;
	pc8r[0] = pc8r[1] = 1;

	feature_exception3_data = 0;
	ini_getval(ini, INISECTION, _T("feature_exception3_data"), &feature_exception3_data);
	feature_exception3_instruction = 0;
	ini_getval(ini, INISECTION, _T("feature_exception3_instruction"), &feature_exception3_instruction);

	for (int i = 0; i < MAX_TARGET_EA; i++) {
		feature_target_ea[i][0] = 0xffffffff;
		feature_target_ea[i][1] = 0xffffffff;
	}
	for (int i = 0; i < 2; i++) {
		if (ini_getstring(ini, INISECTION, i ? _T("feature_target_dst_ea") : _T("feature_target_src_ea"), &vs)) {
			int cnt = 0;
			TCHAR *p = vs;
			while (p && *p) {
				if (cnt >= MAX_TARGET_EA)
					break;
				TCHAR *pp = _tcschr(p, ',');
				if (pp) {
					*pp++ = 0;
				}
				TCHAR *endptr;
				if (_tcslen(p) > 2 && p[0] == '0' && _totupper(p[1]) == 'X') {
					p += 2;
				}
				feature_target_ea[cnt][i] = _tcstol(p, &endptr, 16);
				if (feature_target_ea[cnt][i] & 1) {
					feature_exception3_data = 3;
					feature_exception3_instruction = 3;
				}
				p = pp;
				cnt++;
			}
			if (i) {
				target_ea_dst_max = cnt;
			} else {
				target_ea_src_max = cnt;
			}
			xfree(vs);
		}
	}

	safe_memory_start = 0xffffffff;
	if (ini_getval(ini, INISECTION, _T("feature_safe_memory_start"), &v))
		safe_memory_start = v;
	safe_memory_end = 0xffffffff;
	if (ini_getval(ini, INISECTION, _T("feature_safe_memory_size"), &v))
		safe_memory_end = safe_memory_start + v;

	feature_sr_mask = 0;
	ini_getval(ini, INISECTION, _T("feature_sr_mask"), &feature_sr_mask);
	feature_min_interrupt_mask = 0;
	ini_getval(ini, INISECTION, _T("feature_min_interrupt_mask"), &feature_min_interrupt_mask);

	feature_loop_mode = 0;
	ini_getval(ini, INISECTION, _T("feature_loop_mode"), &feature_loop_mode);
	if (feature_loop_mode) {
		feature_loop_mode_register = 7;
	}
	feature_flag_mode = 0;
	ini_getval(ini, INISECTION, _T("feature_flags_mode"), &feature_flag_mode);

	feature_full_extension_format = 0;
	if (currprefs.cpu_model >= 68020) {
		ini_getval(ini, INISECTION, _T("feature_full_extension_format"), &feature_full_extension_format);
		if (feature_full_extension_format) {
			ad8r[0] |= 2;
			ad8r[1] |= 2;
			pc8r[0] |= 2;
			pc8r[1] |= 2;
		}
	}

	for (int j = 0; j < 2; j++) {
		TCHAR *am = NULL;
		if (ini_getstring(ini, INISECTION, j ? _T("feature_addressing_modes_dst") : _T("feature_addressing_modes_src"), &am)) {
			if (_tcslen(am) > 0) {
				feature_addressing_modes[j] = 0;
				ad8r[j] = 0;
				pc8r[j] = 0;
				TCHAR *p = am;
				while (p && *p) {
					TCHAR *pp = _tcschr(p, ',');
					if (pp) {
						*pp++ = 0;
					}
					TCHAR amtext[256];
					_tcscpy(amtext, p);
					my_trim(amtext);
					for (int i = 0; addrmodes[i]; i++) {
						if (!_tcsicmp(addrmodes[i], amtext)) {
							feature_addressing_modes[j] |= 1 << i;
							break;
						}
					}
					p = pp;
				}
				if (feature_addressing_modes[j] & (1 << Ad8r))
					ad8r[j] |= 1;
				if (feature_addressing_modes[j] & (1 << imm0)) // Ad8rf
					ad8r[j] |= 2;
				if (feature_addressing_modes[j] & (1 << PC8r))
					pc8r[j] |= 1;
				if (feature_addressing_modes[j] & (1 << imm1)) // PC8rf
					pc8r[j] |= 2;
				if (ad8r[j])
					feature_addressing_modes[j] |= 1 << Ad8r;
				if (pc8r[j])
					feature_addressing_modes[j] |= 1 << PC8r;
			}
			xfree(am);
		}
	}


	TCHAR *mode = NULL;
	ini_getstring(ini, INISECTION, _T("mode"), &mode);

	TCHAR *ipath = NULL;
	ini_getstring(ini, INISECTION, _T("path"), &ipath);
	if (!ipath) {
		_tcscpy(path, _T("data/"));
	} else {
		_tcscpy(path, ipath);
	}
	free(ipath);

	_stprintf(path + _tcslen(path), _T("%lu/"), currprefs.cpu_model);
	_wmkdir(path);

	xorshiftstate = 1;

	feature_test_rounds = 2;
	ini_getval(ini, INISECTION, _T("test_rounds"), &feature_test_rounds);

	feature_instruction_size = NULL;
	ini_getstring(ini, INISECTION, _T("feature_instruction_size"), &feature_instruction_size);

	ini_getval(ini, INISECTION, _T("feature_instruction_size"), &feature_test_rounds);

	v = 0;
	ini_getval(ini, INISECTION, _T("test_memory_start"), &v);
	if (!v) {
		wprintf(_T("test_memory_start is required\n"));
		return 0;
	}
	test_memory_start = v;

	v = 0;
	ini_getval(ini, INISECTION, _T("test_memory_size"), &v);
	if (!v) {
		wprintf(_T("test_memory_start is required\n"));
		return 0;
	}
	test_memory_size = v;
	test_memory_end = test_memory_start + test_memory_size;

	test_low_memory_start = 0xffffffff;
	test_low_memory_end = 0xffffffff;
	v = 0;
	if (ini_getval(ini, INISECTION, _T("test_low_memory_start"), &v))
		test_low_memory_start = v;
	v = 0;
	if (ini_getval(ini, INISECTION, _T("test_low_memory_end"), &v))
		test_low_memory_end = v;

	test_high_memory_start = 0xffffffff;
	test_high_memory_end = 0xffffffff;
	v = 0;
	if (ini_getval(ini, INISECTION, _T("test_high_memory_start"), &v))
		test_high_memory_start = v;
	v = 0;
	if (ini_getval(ini, INISECTION, _T("test_high_memory_end"), &v))
		test_high_memory_end = v;

	test_memory = (uae_u8 *)calloc(1, test_memory_size + EXTRA_RESERVED_SPACE);
	test_memory_temp = (uae_u8 *)calloc(1, test_memory_size);
	if (!test_memory || !test_memory_temp) {
		wprintf(_T("Couldn't allocate test memory\n"));
		return 0;
	}

	opcode_memory = test_memory + test_memory_size / 2;
	opcode_memory_start = test_memory_start + test_memory_size / 2;

	low_memory_size = test_low_memory_end;
	if (low_memory_size < 0x8000)
		low_memory_size = 0x8000;

	low_memory = (uae_u8 *)calloc(1, low_memory_size);
	low_memory_temp = (uae_u8 *)calloc(1, low_memory_size);
	high_memory = (uae_u8 *)calloc(1, high_memory_size);
	high_memory_temp = (uae_u8 *)calloc(1, high_memory_size);

	fill_memory();

	if (test_low_memory_start != 0xffffffff) {
		TCHAR *lmem_rom_name = NULL;
		ini_getstring(ini, INISECTION, _T("low_rom"), &lmem_rom_name);
		if (lmem_rom_name) {
			if (load_file(NULL, lmem_rom_name, low_memory_temp, low_memory_size, 0)) {
				wprintf(_T("Low test memory ROM loaded\n"));
				lmem_rom = 1;
			}
		}
		free(lmem_rom_name);
		save_memory(path, _T("lmem.dat"), low_memory_temp, low_memory_size);
	}

	if (test_high_memory_start != 0xffffffff) {
		TCHAR *hmem_rom_name = NULL;
		ini_getstring(ini, INISECTION, _T("high_rom"), &hmem_rom_name);
		if (hmem_rom_name) {
			if (load_file(NULL, hmem_rom_name, high_memory_temp, high_memory_size, -1)) {
				wprintf(_T("High test memory ROM loaded\n"));
				hmem_rom = 1;
			}
		}
		free(hmem_rom_name);
		save_memory(path, _T("hmem.dat"), high_memory_temp, high_memory_size);
	}

	save_memory(path, _T("tmem.dat"), test_memory_temp, test_memory_size);

	storage_buffer = (uae_u8 *)calloc(max_storage_buffer, 1);
	// FMOVEM stores can use lots of memory
	storage_buffer_watermark = max_storage_buffer - 70000;

	for (int i = 0; i < 256; i++) {
		int j;
		for (j = 0; j < 8; j++) {
			if (i & (1 << j)) break;
		}
		movem_index1[i] = j;
		movem_index2[i] = 7 - j;
		movem_next[i] = i & (~(1 << j));
	}

	read_table68k();
	do_merges();

	if (currprefs.cpu_model == 68000) {
		tbl = op_smalltbl_90_test_ff;
		cpu_lvl = 0;
	} else if (currprefs.cpu_model == 68010) {
		tbl = op_smalltbl_91_test_ff;
		cpu_lvl = 1;
	} else if (currprefs.cpu_model == 68020) {
		tbl = op_smalltbl_92_test_ff;
		cpu_lvl = 2;
	} else if (currprefs.cpu_model == 68030) {
		tbl = op_smalltbl_93_test_ff;
		cpu_lvl = 3;
	} else if (currprefs.cpu_model == 68040 ) {
		tbl = op_smalltbl_94_test_ff;
		cpu_lvl = 4;
	} else if (currprefs.cpu_model == 68060) {
		tbl = op_smalltbl_95_test_ff;
		cpu_lvl = 5;
	} else {
		wprintf(_T("Unsupported CPU model.\n"));
		abort();
	}

	for (int opcode = 0; opcode < 65536; opcode++) {
		cpufunctbl[opcode] = op_illg_1;
	}

	for (int i = 0; tbl[i].handler != NULL; i++) {
		int opcode = tbl[i].opcode;
		cpufunctbl[opcode] = tbl[i].handler;
		cpudatatbl[opcode].length = tbl[i].length;
		cpudatatbl[opcode].disp020[0] = tbl[i].disp020[0];
		cpudatatbl[opcode].disp020[1] = tbl[i].disp020[1];
		cpudatatbl[opcode].branch = tbl[i].branch;
	}

	currprefs.int_no_unimplemented = true;
	currprefs.fpu_no_unimplemented = true;

	for (int opcode = 0; opcode < 65536; opcode++) {
		cpuop_func *f;
		instr *table = &table68k[opcode];

		if (table->mnemo == i_ILLG)
			continue;

		/* unimplemented opcode? */
		if (table->unimpclev > 0 && cpu_lvl >= table->unimpclev) {
			if (currprefs.cpu_model == 68060) {
				// remove unimplemented integer instructions
				// unimpclev == 5: not implemented in 68060,
				// generates unimplemented instruction exception.
				if (currprefs.int_no_unimplemented && table->unimpclev == 5) {
					cpufunctbl[opcode] = op_unimpl_1;
					continue;
				}
				// remove unimplemented instruction that were removed in previous models,
				// generates normal illegal instruction exception.
				// unimplclev < 5: instruction was removed in 68040 or previous model.
				// clev=4: implemented in 68040 or later. unimpclev=5: not in 68060
				if (table->unimpclev < 5 || (table->clev == 4 && table->unimpclev == 5)) {
					cpufunctbl[opcode] = op_illg_1;
					continue;
				}
			} else {
				cpufunctbl[opcode] = op_illg_1;
				continue;
			}
		}

		if (table->clev > cpu_lvl) {
			continue;
		}

		if (!currprefs.fpu_model) {
			int fppskip = isfpp(table->mnemo);
			if (fppskip)
				continue;
		}

		if (table->handler != -1) {
			int idx = table->handler;
			f = cpufunctbl[idx];
			if (f == op_illg_1)
				abort();
			cpufunctbl[opcode] = f;
			memcpy(&cpudatatbl[opcode], &cpudatatbl[idx], sizeof(struct cputbl_data));
		}
	}

	x_get_long = get_long_test;
	x_get_word = get_word_test;
	x_put_long = put_long_test;
	x_put_word = put_word_test;

	x_next_iword = next_iword_test;
	x_cp_next_iword = next_iword_test;
	x_next_ilong = next_ilong_test;
	x_cp_next_ilong = next_ilong_test;

	x_cp_get_disp_ea_020 = x_get_disp_ea_020;

	x_cp_get_long = get_long_test;
	x_cp_get_word = get_word_test;
	x_cp_get_byte = get_byte_test;
	x_cp_put_long = put_long_test;
	x_cp_put_word = put_word_test;
	x_cp_put_byte = put_byte_test;

	fpu_reset();

	starttime = time(0);

	if (!mode) {
		wprintf(_T("Mode must be 'all' or '<mnemonic>'\n"));
		return 0;
	}

	TCHAR *modep = mode;
	while(modep) {

		int all = 0;

		if (!_tcsicmp(mode, _T("all"))) {

			if (verbose == 1)
				verbose = 0;
			for (int j = 1; lookuptab[j].name; j++) {
				test_mnemo_text(path, lookuptab[j].name);
			}
			// Illegal instructions last. All currently selected CPU model's unsupported opcodes
			// (Generates illegal instruction, a-line or f-line exception)
			test_mnemo_text(path, _T("ILLEGAL"));
			break;
		}

		if (!_tcsicmp(mode, _T("fall"))) {

			if (verbose == 1)
				verbose = 0;
			test_mnemo_text(path, _T("FMOVECR"));
			const TCHAR *prev = _T("");
			for (int j = 0; j < 64; j++) {
				if (fpuopcodes[j] && _tcscmp(prev, fpuopcodes[j])) {
					test_mnemo_text(path, fpuopcodes[j]);
					prev = fpuopcodes[j];
				}
			}
			for (int j = 1; lookuptab[j].name; j++) {
				if (lookuptab[j].name[0] == 'F' && _tcscmp(lookuptab[j].name, _T("FPP"))) {
					test_mnemo_text(path, lookuptab[j].name);
				}
			}
			test_mnemo_text(path, _T("FMOVEM"));
			break;
		}


		TCHAR *sp = _tcschr(modep, ',');
		if (sp)
			*sp++ = 0;

		test_mnemo_text(path, modep);

		modep = sp;
	}

	wprintf(_T("%d total tests generated\n"), test_count);

	return 0;
}
