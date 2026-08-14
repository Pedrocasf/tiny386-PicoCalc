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
	if (argc == 2) {
		argv1 = argv[1];
	} else if (argc == 3) {
		if (strcmp(argv[1], "-kvm") == 0)
			enable_kvm = true;
		argv1 = argv[2];
	} else {
		return 1;
	}

	int err = ini_parse(argv1, parse_conf_ini, &conf);
	if (err) {
		fprintf(stderr, "error %d\n", err);
		return err;
	}
	if (enable_kvm)
		conf.cpu_gen = -1;

	void *fb = bigmalloc(conf.width * conf.height * 4);
	PC *pc = pc_new(redraw, NULL, fb, &conf);
	load_bios_and_reset(pc);

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

	for (; pc->shutdown_state != 8;) {
		pc_step(pc);
		pc_vga_step(pc);

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
