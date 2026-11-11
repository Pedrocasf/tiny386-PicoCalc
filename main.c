// "headless" tiny386
// for SDL port, see `sdl/main.c`
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "i386.h"
#include "pc.h"

/* pc_step() calls between checks for an idle guest, and the instruction rate
 * below which it counts as idle -- a working guest runs orders of magnitude
 * faster than a halt/timer-interrupt loop. */
#define IDLE_CHECK_INTERVAL 1000
#define IDLE_RATE 200000

// platform HAL implementation
#include <time.h>
uint32_t get_uticks()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint32_t) ts.tv_sec * 1000000 +
		(uint32_t) ts.tv_nsec / 1000);
}

#ifndef _WIN32
#include <sys/mman.h>
void *bigmalloc(size_t size)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}
#else
void *bigmalloc(size_t size)
{
	return malloc(size);
}
#endif

int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	FILE *fp = fopen(file, "rb");
	if (fp == NULL) {
		fprintf(stderr, "load_rom: open %s failed: %s\n", file, strerror(errno));
		abort();
	}

	fseek(fp, 0, SEEK_END);
	int len = ftell(fp);
	fprintf(stderr, "load_rom: %s, len %d\n", file, len);
	rewind(fp);
	if (backward)
		fread(phys_mem + addr - len, 1, len, fp);
	else
		fread(phys_mem + addr, 1, len, fp);
	fclose(fp);
	return len;
}

//
static void redraw(void *opaque,
		   int x, int y, int w, int h)
{
}

/*
 * Interpreter benchmark: -bench runs fixed instruction loops instead of
 * booting, so the cost of decode and dispatch can be told apart from the cost
 * of guest memory.  Each loop is a few bytes planted at the reset vector and
 * runs with no data working set to speak of, so what it measures is the
 * interpreter itself rather than the host's caches.
 *
 * Differences between the lines localise the work: nop is fetch and dispatch
 * alone, mov adds operand decode, add adds the lazy-flag bookkeeping, the
 * [bx] forms add modsib() and the memory path, and the direct-address form
 * skips modsib() while still touching memory.
 */
#define BENCH_BODY 0xf0000	/* f000:0000, reached from the reset vector */
#define BENCH_REPS 16		/* copies of the instruction under test */
#define BENCH_US 1500000

struct bench {
	const char *name;
	int len;
	/*
	 * Somewhere the code should have left a known value, so a mistake in
	 * the hand assembled bytes below shows up as a failure rather than as
	 * a plausible looking number.  A guest that faults here vectors
	 * through a zeroed ivt into a field of zeroes and runs forever
	 * without complaining.  Zero address means no check.
	 */
	uint32_t chk_addr;
	uint16_t chk_val;
	const uint8_t code[48];
	/* run in flat 32-bit protected mode with paging on, which is where
	 * guest memory goes through the TLB (or, under USE_FASTMEM, the host
	 * MMU) */
	bool paged;
};

static const struct bench benches[] = {
	{ "nop",           1, 0, 0, {0x90} },
	{ "mov ax,bx",     2, 0, 0, {0x89, 0xd8} },
	{ "add ax,bx",     2, 0, 0, {0x01, 0xd8} },
	{ "mov al,[bx]",   2, 0, 0, {0x8a, 0x07} },
	{ "add [bx],ax",   2, 0, 0, {0x01, 0x07} },
	{ "mov al,[1234]", 3, 0, 0, {0xa0, 0x34, 0x12} },
	/*
	 * A blend rather than one instruction repeated: six memory operands,
	 * three branches and eight register ops across twelve distinct
	 * opcodes.  The single-instruction rows above each keep one opcode
	 * handler hot and the rest of the interpreter cold, which flatters
	 * any change that grows code -- inlining looks free when nothing else
	 * is competing for the instruction cache.  This row keeps a dozen
	 * handlers live at once, so it prices that competition in.
	 *
	 *   mov bx,8000     bb 00 80    a fixed base, so nothing drifts
	 *   mov al,[bx]     8a 07       and every access stays in plain ram
	 *   add al,cl       00 c8
	 *   mov [bx+2],al   88 47 02
	 *   mov dx,[bx+4]   8b 57 04
	 *   add dx,1        83 c2 01
	 *   mov [bx+6],dx   89 57 06
	 *   jz  +0          74 00       one of these two is taken and the
	 *   jnz +0          75 00       other is not, whatever the flags say
	 *   mov cx,[bx+8]   8b 4f 08
	 *   shl cx,1        d1 e1
	 *   mov ax,cx       89 c8
	 *   sub ax,dx       29 d0
	 *   mov [bx+10],ax  89 47 0a
	 *   jmp +0          eb 00
	 *   nop             90
	 *
	 * Nothing here writes what it later reads, so the state settles after
	 * the first pass: dx stays 1 and ax stays 0-1, which is what the
	 * check below looks for at 0x800a.
	 */
	{ "mixed",        38, 0x800a, 0xffff, {
		0xbb, 0x00, 0x80,
		0x8a, 0x07,
		0x00, 0xc8,
		0x88, 0x47, 0x02,
		0x8b, 0x57, 0x04,
		0x83, 0xc2, 0x01,
		0x89, 0x57, 0x06,
		0x74, 0x00,
		0x75, 0x00,
		0x8b, 0x4f, 0x08,
		0xd1, 0xe1,
		0x89, 0xc8,
		0x29, 0xd0,
		0x89, 0x47, 0x0a,
		0xeb, 0x00,
		0x90,
	} },
	/*
	 * The same under paging, with the first 4 MiB identity mapped (see
	 * bench_paging), so the memory operands take the paging path instead
	 * of the paging-off segment fast path.
	 */
	{ "pg mov eax,[ebx]", 2, 0, 0, {0x8b, 0x03}, true },
	{ "pg add [ebx],eax", 2, 0, 0, {0x01, 0x03}, true },
	/*
	 * mixed again, with dword operands spaced out so that, again, nothing
	 * read is ever written: edx settles at 1 and eax at 0-1, at 0x8010
	 *
	 *   mov ebx,8000      bb 00 80 00 00
	 *   mov al,[ebx]      8a 03
	 *   add al,cl         00 c8
	 *   mov [ebx+20],al   88 43 14
	 *   mov edx,[ebx+4]   8b 53 04
	 *   add edx,1         83 c2 01
	 *   mov [ebx+8],edx   89 53 08
	 *   jz  +0            74 00
	 *   jnz +0            75 00
	 *   mov ecx,[ebx+12]  8b 4b 0c
	 *   shl ecx,1         d1 e1
	 *   mov eax,ecx       89 c8
	 *   sub eax,edx       29 d0
	 *   mov [ebx+16],eax  89 43 10
	 *   jmp +0            eb 00
	 *   nop               90
	 */
	{ "pg mixed",     40, 0x8010, 0xffff, {
		0xbb, 0x00, 0x80, 0x00, 0x00,
		0x8a, 0x03,
		0x00, 0xc8,
		0x88, 0x43, 0x14,
		0x8b, 0x53, 0x04,
		0x83, 0xc2, 0x01,
		0x89, 0x53, 0x08,
		0x74, 0x00,
		0x75, 0x00,
		0x8b, 0x4b, 0x0c,
		0xd1, 0xe1,
		0x89, 0xc8,
		0x29, 0xd0,
		0x89, 0x43, 0x10,
		0xeb, 0x00,
		0x90,
	}, true },
};

#define BENCH_PD 0x10000	/* a page directory, then its one page table */

/*
 * For the paged rows: identity map the first 4 MiB through one page table,
 * and plant the prologue that turns paging on from flat protected mode.
 * Returns the prologue's length.
 */
static int bench_paging(PC *pc, uint8_t *body)
{
	uint8_t *pd = (uint8_t *) pc->phys_mem + BENCH_PD;
	uint8_t *pt = pd + 4096;
	memset(pd, 0, 8192);
	uint32_t pde = (BENCH_PD + 4096) | 7;	/* present, writable, user */
	memcpy(pd, &pde, 4);
	for (uint32_t i = 0; i < 1024; i++) {
		uint32_t pte = (i << 12) | 7;
		memcpy(pt + i * 4, &pte, 4);
	}
	static const uint8_t prologue[] = {
		0xb8, BENCH_PD & 0xff, (BENCH_PD >> 8) & 0xff,
		(BENCH_PD >> 16) & 0xff, 0x00,	/* mov eax,BENCH_PD */
		0x0f, 0x22, 0xd8,		/* mov cr3,eax */
		0x0f, 0x20, 0xc0,		/* mov eax,cr0 */
		0x0d, 0x00, 0x00, 0x00, 0x80,	/* or eax,80000000 */
		0x0f, 0x22, 0xc0,		/* mov cr0,eax */
	};
	memcpy(body, prologue, sizeof(prologue));
	return sizeof(prologue);
}

static void bench_one(PC *pc, const struct bench *b)
{
	uint8_t *body = (uint8_t *) pc->phys_mem + BENCH_BODY;
	int top = b->paged ? bench_paging(pc, body) : 0;	/* loop start */
	int off = top;

	for (int i = 0; i < BENCH_REPS; i++, off += b->len)
		memcpy(body + off, b->code, b->len);
	if (off + 2 - top <= 128) {
		body[off] = 0xeb;		/* jmp rel8 back to the top */
		body[off + 1] = (uint8_t) -(off + 2 - top);
	} else if (!b->paged) {
		int rel = -(off + 3);		/* too far for rel8 */
		body[off] = 0xe9;
		body[off + 1] = rel & 0xff;
		body[off + 2] = (rel >> 8) & 0xff;
	} else {
		int32_t rel = -(off + 5 - top);	/* jmp rel32 in 32-bit code */
		body[off] = 0xe9;
		memcpy(body + off + 1, &rel, 4);
	}

	if (b->paged)
		memset((uint8_t *) pc->phys_mem + 0x8000, 0, 0x20);
	if (b->chk_addr)
		memset((uint8_t *) pc->phys_mem + b->chk_addr, 0, 2);

	if (b->paged)
		cpui386_reset_pm(pc->cpu, BENCH_BODY);	/* flat 32-bit, cpl 0 */
	else
		cpui386_reset(pc->cpu);		/* back to f000:fff0 */

	uint32_t t0 = get_uticks();
	long c0 = cpui386_get_cycle(pc->cpu);
	while (get_uticks() - t0 < BENCH_US)
		pc_step(pc);
	uint32_t dt = get_uticks() - t0;
	long dc = cpui386_get_cycle(pc->cpu) - c0;

	double mips = dc / (double) dt;
	printf("bench: %-14s %6.2f Mips  %6.1f ns/insn",
	       b->name, mips, 1000.0 / mips);

	if (b->chk_addr) {
		const uint8_t *p = (uint8_t *) pc->phys_mem + b->chk_addr;
		uint16_t got = p[0] | (p[1] << 8);
		if (got != b->chk_val)
			printf("  BAD: %04x at %05x, wanted %04x",
			       got, b->chk_addr, b->chk_val);
	}
	printf("\n");
	fflush(stdout);
}

static void cpu_bench(PC *pc)
{
	/* jmp from the reset vector down to f000:0000, where there is room */
	static const uint8_t tramp[] = {0xe9, 0x0d, 0x00};
	memcpy(pc->phys_mem + 0xffff0, tramp, sizeof(tramp));

	for (int i = 0; i < (int) (sizeof(benches) / sizeof(benches[0])); i++)
		bench_one(pc, &benches[i]);
}

#include "term.h"
static void put_key(void *o, unsigned char scan_code, int is_pressed)
{
	ps2_put_keycode(o, is_pressed, scan_code);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [-kvm] [-term] [-bench] inifile\n",
		argv0);
}

#ifndef _WIN32
#include <signal.h>
volatile int g_running = 1;
static void sig_handler(int _)
{
	g_running = 0;
}
static void set_sig_handler()
{
	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	/* closing the terminal: stop the same way, so disk images are closed */
	signal(SIGHUP, sig_handler);
}
#else
#define g_running 1
#define set_sig_handler()
#endif

int main(int argc, char *argv[])
{
	PCConfig conf;
	memset(&conf, 0, sizeof(conf));
	conf.mem_size = 8 * 1024 * 1024;
	conf.vga_mem_size = 256 * 1024;
	conf.width = 720;
	conf.height = 480;
	conf.cpu_gen = 4;
	conf.fpu = 0;

	const char *argv1;
	bool enable_kvm = false;
	bool use_term = false;
	bool bench = false;
	if (argc > 1) {
		for (int i = 1; i < argc - 1; i++) {
			if (strcmp(argv[i], "-kvm") == 0)
				enable_kvm = true;
			else if (strcmp(argv[i], "-term") == 0)
				use_term = true;
			else if (strcmp(argv[i], "-bench") == 0)
				bench = true;
			else {
				usage(argv[0]);
				return 1;
			}
		}
		argv1 = argv[argc - 1];
		ne2000_set_config_file(argv1);
	} else {
		usage(argv[0]);
		return 1;
	}

	int err = ini_parse(argv1, parse_conf_ini, &conf);
	if (err) {
		fprintf(stderr, "error %d\n", err);
		return err;
	}
	if (enable_kvm)
		conf.cpu_gen = -1;

	set_sig_handler();

	void *fb = bigmalloc(conf.width * conf.height * 4);
	PC *pc = pc_new(redraw, NULL, fb, &conf);
	Term *term = NULL;
	if (use_term)
		term = term_init(pc->vga, put_key, pc->kbd);
	load_bios_and_reset(pc);

	if (bench) {
		cpu_bench(pc);	/* overwrites the reset vector: no guest after */
		return 0;
	}

	pc->boot_start_time = get_uticks();

	/*
	 * A guest that halts waiting for a keypress looks exactly like a hang
	 * from out here: no more output, and the process sleeping in the
	 * usleep() the halt handler does.  Say so, since this build has neither
	 * a screen for it to draw on nor a keyboard to satisfy it.  Progress is
	 * measured with the emulated cpu's own cycle counter.
	 */
#ifndef USE_CPUABS
	long last_cycle = 0;
	uint32_t last_sample = get_uticks();
	bool idle_reported = false;
	int countdown = IDLE_CHECK_INTERVAL;
#endif

	for (; g_running && pc->shutdown_state != 8;) {
		pc_step(pc);
		pc_vga_step(pc);
		if (use_term)
			term_step(term);

#ifndef USE_CPUABS
		if (--countdown > 0)
			continue;
		countdown = IDLE_CHECK_INTERVAL;

		uint32_t now = get_uticks();
		if (now - last_sample < 2000000)
			continue;

		long cycle = cpui386_get_cycle(pc->cpu);
		long rate = (cycle - last_cycle) / 2;
		last_cycle = cycle;
		last_sample = now;

		/* A guest waiting on a keypress is not stopped: it halts, wakes
		 * on the timer interrupt, and halts again, so what collapses is
		 * the rate rather than progress itself. */
		if (rate < IDLE_RATE && !idle_reported) {
			fprintf(stderr, "guest is idle (%ld instructions/s): "
					"it is halting, most likely waiting for "
					"input this build cannot give it -- no "
					"screen, no keyboard (not a hang)\n",
				rate);
			idle_reported = true;
		} else if (rate >= IDLE_RATE && idle_reported) {
			fprintf(stderr, "guest is running again (%ld "
					"instructions/s)\n", rate);
			idle_reported = false;
		}
#endif
	}
	return 0;
}
