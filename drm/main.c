/*
 * Linux KMS/DRM frontend: runs full-screen from a text console (VT), with no
 * X11, Wayland or SDL.
 *
 * Display: libdrm, a connected connector in its preferred mode, two dumb
 * buffers flipped with drmModePageFlip (or one buffer and drmModeDirtyFB
 * where page flips are not available).  The guest's frame is centred,
 * scaled by the largest integer factor that fits, or shrunk to fit when the
 * panel is smaller than the guest's frame.
 *
 * Input: evdev keyboards and mice from /dev/input/event*, grabbed so that
 * nothing reaches the console underneath.  Linux KEY_* codes are what
 * ps2_put_keycode() takes, so keys go to the guest as they are.
 *
 * VT: the console is put in graphics mode, and VT switches (ctrl + alt + Fn)
 * are handled with VT_PROCESS: the DRM master and the input grabs are given
 * up while another VT is shown.
 *
 * Keys: ctrl + ] quits, ctrl + [ shows/hides the OSD.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <dirent.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "pc.h"
#include "osd/osd.h"

#ifdef DRM_AUDIO
#define CNFA_IMPLEMENTATION
#include "rawdraw/CNFA.h"
#endif

uint32_t get_uticks()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ((uint32_t) ts.tv_sec * 1000000 +
		(uint32_t) ts.tv_nsec / 1000);
}

void *bigmalloc(size_t size)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

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

/* --- display --- */

typedef struct {
	uint32_t handle, fb_id, pitch;
	uint64_t size;
	uint8_t *map;
} DumbBuf;

#define MAX_INPUTS 32
typedef struct {
	int fd;
	char path[32];
	bool is_kbd, is_mouse;
	bool grabbed;
} InputDev;

typedef struct {
	int width, height;      /* the guest's frame */
	uint32_t *fb;
	PC *pc;
	OSD *osd;
	bool osd_enabled;

	/* DRM */
	int drm_fd;
	uint32_t conn_id, crtc_id;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	DumbBuf buf[2];
	int nbufs;
	int front;
	bool flip_pending;
	bool dirty;
	bool active;            /* our VT is shown and we are DRM master */
	int dst_x, dst_y, dst_w, dst_h;
	int *xmap;

	/* VT */
	int tty_fd;
	int saved_kd_mode;
	bool vt_process;

	/* input */
	InputDev in[MAX_INPUTS];
	int nin;
	uint32_t last_scan;
	int mx, my;             /* OSD pointer, in guest frame pixels */
	int mbtn;
	int rel_x, rel_y, rel_z;
	bool rel_pending;
} Console;

static Console *g_console;
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_vt_release, g_vt_acquire;

#define KEYCODE_MAX 127
static uint8_t key_pressed[KEYCODE_MAX + 1];

static void die(const char *msg)
{
	fprintf(stderr, "tiny386_drm: %s\n", msg);
	exit(1);
}

static bool dumb_create(Console *s, DumbBuf *b)
{
	struct drm_mode_create_dumb creq = {0};
	creq.width = s->mode.hdisplay;
	creq.height = s->mode.vdisplay;
	creq.bpp = 32;
	if (drmIoctl(s->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return false;
	b->handle = creq.handle;
	b->pitch = creq.pitch;
	b->size = creq.size;
	if (drmModeAddFB(s->drm_fd, creq.width, creq.height, 24, 32,
			 b->pitch, b->handle, &b->fb_id)) {
		struct drm_mode_destroy_dumb dreq = { .handle = b->handle };
		drmIoctl(s->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		return false;
	}
	struct drm_mode_map_dumb mreq = { .handle = b->handle };
	void *map = MAP_FAILED;
	if (drmIoctl(s->drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) == 0)
		map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			   s->drm_fd, mreq.offset);
	if (map == MAP_FAILED) {
		struct drm_mode_destroy_dumb dreq = { .handle = b->handle };
		drmModeRmFB(s->drm_fd, b->fb_id);
		drmIoctl(s->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
		return false;
	}
	b->map = map;
	memset(b->map, 0, b->size);
	return true;
}

static void dumb_destroy(Console *s, DumbBuf *b)
{
	struct drm_mode_destroy_dumb dreq = { .handle = b->handle };
	munmap(b->map, b->size);
	drmModeRmFB(s->drm_fd, b->fb_id);
	drmIoctl(s->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

/* the CRTC for a connector: the one it drives now, else a free one */
static uint32_t find_crtc(int fd, drmModeRes *res, drmModeConnector *conn)
{
	if (conn->encoder_id) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
		if (enc) {
			uint32_t crtc = enc->crtc_id;
			drmModeFreeEncoder(enc);
			if (crtc)
				return crtc;
		}
	}
	for (int i = 0; i < conn->count_encoders; i++) {
		drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc)
			continue;
		for (int j = 0; j < res->count_crtcs; j++) {
			if (enc->possible_crtcs & (1u << j)) {
				drmModeFreeEncoder(enc);
				return res->crtcs[j];
			}
		}
		drmModeFreeEncoder(enc);
	}
	return 0;
}

/*
 * Pick a connected connector and a mode on the device at path.  want_w and
 * want_h (from TINY386_DRM_MODE=WxH) choose a mode other than the preferred.
 */
static bool drm_try_device(Console *s, const char *path,
			   int want_w, int want_h)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return false;
	uint64_t cap = 0;
	drmModeRes *res = NULL;
	if (drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &cap) < 0 || !cap ||
	    !(res = drmModeGetResources(fd))) {
		close(fd);
		return false;
	}
	bool found = false;
	for (int i = 0; i < res->count_connectors && !found; i++) {
		drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0) {
			int m = 0;
			for (int j = 0; j < conn->count_modes; j++) {
				if (conn->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
					m = j;
					break;
				}
			}
			if (want_w > 0) {
				for (int j = 0; j < conn->count_modes; j++) {
					if (conn->modes[j].hdisplay == want_w &&
					    conn->modes[j].vdisplay == want_h) {
						m = j;
						break;
					}
				}
			}
			uint32_t crtc = find_crtc(fd, res, conn);
			if (crtc) {
				s->drm_fd = fd;
				s->conn_id = conn->connector_id;
				s->crtc_id = crtc;
				s->mode = conn->modes[m];
				found = true;
			}
		}
		drmModeFreeConnector(conn);
	}
	drmModeFreeResources(res);
	if (!found)
		close(fd);
	return found;
}

static void drm_layout(Console *s)
{
	int pw = s->mode.hdisplay, ph = s->mode.vdisplay;
	int k = pw / s->width < ph / s->height ? pw / s->width : ph / s->height;
	if (k >= 1) {
		s->dst_w = s->width * k;
		s->dst_h = s->height * k;
	} else {
		/* the panel is smaller than the guest's frame: shrink to fit */
		if ((long) pw * s->height <= (long) ph * s->width) {
			s->dst_w = pw;
			s->dst_h = (long) s->height * pw / s->width;
		} else {
			s->dst_h = ph;
			s->dst_w = (long) s->width * ph / s->height;
		}
		if (s->dst_w < 1) s->dst_w = 1;
		if (s->dst_h < 1) s->dst_h = 1;
	}
	s->dst_x = (pw - s->dst_w) / 2;
	s->dst_y = (ph - s->dst_h) / 2;
	s->xmap = malloc(sizeof(int) * s->dst_w);
	for (int x = 0; x < s->dst_w; x++)
		s->xmap[x] = (long) x * s->width / s->dst_w;
}

static void drm_init(Console *s)
{
	int want_w = 0, want_h = 0;
	const char *m = getenv("TINY386_DRM_MODE");
	if (m && sscanf(m, "%dx%d", &want_w, &want_h) != 2)
		want_w = want_h = 0;

	const char *dev = getenv("TINY386_DRM_DEVICE");
	bool ok = false;
	if (dev) {
		ok = drm_try_device(s, dev, want_w, want_h);
	} else {
		for (int i = 0; i < 16 && !ok; i++) {
			char path[32];
			snprintf(path, sizeof(path), "/dev/dri/card%d", i);
			ok = drm_try_device(s, path, want_w, want_h);
		}
	}
	if (!ok)
		die("no DRM device with a connected display "
		    "(set TINY386_DRM_DEVICE=/dev/dri/cardN?)");

	if (drmSetMaster(s->drm_fd) < 0 && errno != EINVAL)
		fprintf(stderr, "tiny386_drm: drmSetMaster: %s "
			"(is a display server running on this device?)\n",
			strerror(errno));

	s->saved_crtc = drmModeGetCrtc(s->drm_fd, s->crtc_id);

	s->nbufs = 0;
	for (int i = 0; i < 2; i++)
		if (dumb_create(s, &s->buf[i]))
			s->nbufs++;
		else
			break;
	if (s->nbufs == 0)
		die("cannot create a dumb buffer");
	s->front = 0;
	if (drmModeSetCrtc(s->drm_fd, s->crtc_id, s->buf[0].fb_id, 0, 0,
			   &s->conn_id, 1, &s->mode)) {
		fprintf(stderr, "tiny386_drm: drmModeSetCrtc: %s\n",
			strerror(errno));
		exit(1);
	}
	s->active = true;
	drm_layout(s);
	fprintf(stderr, "tiny386_drm: %dx%d@%d, guest %dx%d drawn as %dx%d, "
		"%d buffer%s\n",
		s->mode.hdisplay, s->mode.vdisplay, s->mode.vrefresh,
		s->width, s->height, s->dst_w, s->dst_h,
		s->nbufs, s->nbufs > 1 ? "s" : "");
}

static void blit(Console *s, DumbBuf *b)
{
	const uint32_t *src = s->fb;
	uint8_t *row0 = b->map + (size_t) s->dst_y * b->pitch + s->dst_x * 4;
	int last_sy = -1;
	uint8_t *last_row = NULL;
	for (int y = 0; y < s->dst_h; y++) {
		uint8_t *row = row0 + (size_t) y * b->pitch;
		int sy = (long) y * s->height / s->dst_h;
		if (sy == last_sy) {
			memcpy(row, last_row, s->dst_w * 4);
		} else if (s->dst_w == s->width) {
			memcpy(row, src + (size_t) sy * s->width, s->width * 4);
		} else {
			const uint32_t *sl = src + (size_t) sy * s->width;
			uint32_t *d = (uint32_t *) row;
			for (int x = 0; x < s->dst_w; x++)
				d[x] = sl[s->xmap[x]];
		}
		last_sy = sy;
		last_row = row;
	}
}

static void present(Console *s)
{
	if (!s->active)
		return;
	if (s->flip_pending) {
		/* shown when the pending flip completes */
		s->dirty = true;
		return;
	}
	s->dirty = false;
	if (s->nbufs > 1) {
		int back = s->front ^ 1;
		blit(s, &s->buf[back]);
		if (drmModePageFlip(s->drm_fd, s->crtc_id, s->buf[back].fb_id,
				    DRM_MODE_PAGE_FLIP_EVENT, s) == 0) {
			s->front = back;
			s->flip_pending = true;
			return;
		}
		/* no page flips here: one buffer, set once, from now on */
		fprintf(stderr, "tiny386_drm: drmModePageFlip: %s, "
			"using a single buffer\n", strerror(errno));
		dumb_destroy(s, &s->buf[back]);
		s->nbufs = 1;
		if (s->front != 0) {
			s->buf[0] = s->buf[1];
			s->front = 0;
		}
	}
	DumbBuf *b = &s->buf[s->front];
	blit(s, b);
	drmModeClip clip = { s->dst_x, s->dst_y,
			     s->dst_x + s->dst_w, s->dst_y + s->dst_h };
	drmModeDirtyFB(s->drm_fd, b->fb_id, &clip, 1);
}

static void page_flip_handler(int fd, unsigned int seq, unsigned int sec,
			      unsigned int usec, void *data)
{
	Console *s = data;
	s->flip_pending = false;
	if (s->dirty)
		present(s);
}

static void drm_handle_events(Console *s, int timeout)
{
	struct pollfd pfd = { s->drm_fd, POLLIN, 0 };
	drmEventContext ev = {
		.version = 2,
		.page_flip_handler = page_flip_handler,
	};
	while (poll(&pfd, 1, timeout) > 0 && (pfd.revents & POLLIN)) {
		drmHandleEvent(s->drm_fd, &ev);
		timeout = 0;
	}
}

static void redraw(void *opaque, int x, int y, int w, int h)
{
	Console *s = opaque;
	if (s->osd_enabled)
		osd_render(s->osd, (uint8_t *) s->fb,
			   s->width, s->height, s->width * 4);
	present(s);
}

/* --- input --- */

#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
static bool test_bit(const unsigned long *a, int bit)
{
	return (a[bit / BITS_PER_LONG] >> (bit % BITS_PER_LONG)) & 1;
}

static void input_open(Console *s, const char *path)
{
	if (s->nin >= MAX_INPUTS)
		return;
	int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	unsigned long ev[NLONGS(EV_MAX + 1)] = {0};
	unsigned long key[NLONGS(KEY_MAX + 1)] = {0};
	unsigned long rel[NLONGS(REL_MAX + 1)] = {0};
	ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
	if (test_bit(ev, EV_KEY))
		ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key);
	if (test_bit(ev, EV_REL))
		ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
	bool is_kbd = test_bit(key, KEY_A) && test_bit(key, KEY_ENTER);
	bool is_mouse = test_bit(rel, REL_X) && test_bit(rel, REL_Y) &&
			test_bit(key, BTN_LEFT);
	if (!is_kbd && !is_mouse) {
		close(fd);
		return;
	}
	InputDev *d = &s->in[s->nin++];
	memset(d, 0, sizeof(*d));
	d->fd = fd;
	snprintf(d->path, sizeof(d->path), "%s", path);
	d->is_kbd = is_kbd;
	d->is_mouse = is_mouse;
	char name[64] = "";
	ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	fprintf(stderr, "tiny386_drm: input %s: %s%s%s\n", path, name,
		is_kbd ? " (keyboard)" : "", is_mouse ? " (mouse)" : "");
}

static void input_close(Console *s, int i)
{
	if (s->in[i].grabbed)
		ioctl(s->in[i].fd, EVIOCGRAB, 0);
	close(s->in[i].fd);
	s->in[i] = s->in[--s->nin];
}

/* open event devices that are new since the last scan (hotplug) */
static void input_scan(Console *s)
{
	DIR *dir = opendir("/dev/input");
	if (!dir)
		return;
	struct dirent *de;
	while ((de = readdir(dir))) {
		if (strncmp(de->d_name, "event", 5) != 0)
			continue;
		char path[32];
		snprintf(path, sizeof(path), "/dev/input/%.20s", de->d_name);
		bool known = false;
		for (int i = 0; i < s->nin; i++)
			if (strcmp(s->in[i].path, path) == 0)
				known = true;
		if (!known)
			input_open(s, path);
	}
	closedir(dir);
	s->last_scan = get_uticks();
}

/*
 * Grab a device once none of its keys are down: a key grabbed while it is
 * down (the enter that started us, the ctrl + alt + Fn that switched back
 * to us) would never be released for the console's keyboard handler, and
 * would repeat there.
 */
static void input_try_grab(Console *s)
{
	for (int i = 0; i < s->nin; i++) {
		InputDev *d = &s->in[i];
		if (d->grabbed)
			continue;
		unsigned long key[NLONGS(KEY_MAX + 1)] = {0};
		if (ioctl(d->fd, EVIOCGKEY(sizeof(key)), key) >= 0) {
			bool down = false;
			for (size_t j = 0; j < NLONGS(KEY_MAX + 1); j++)
				if (key[j])
					down = true;
			if (down)
				continue;
		}
		if (ioctl(d->fd, EVIOCGRAB, 1) == 0) {
			d->grabbed = true;
		} else {
			fprintf(stderr, "tiny386_drm: cannot grab %s: %s\n",
				d->path, strerror(errno));
			input_close(s, i);
			i--;
		}
	}
}

static void input_ungrab_all(Console *s)
{
	for (int i = 0; i < s->nin; i++) {
		if (s->in[i].grabbed) {
			ioctl(s->in[i].fd, EVIOCGRAB, 0);
			s->in[i].grabbed = false;
		}
	}
}

static void reset_keys(Console *s)
{
	for (int i = 1; i <= KEYCODE_MAX; i++) {
		if (key_pressed[i]) {
			if (!s->osd_enabled)
				ps2_put_keycode(s->pc->kbd, 0, i);
			key_pressed[i] = 0;
		}
	}
	if (s->mbtn) {
		s->mbtn = 0;
		if (!s->osd_enabled)
			ps2_mouse_event(s->pc->mouse, 0, 0, 0, 0);
	}
}

void console_send_kbd(void *opaque, int keypress, int keycode)
{
	Console *s = opaque;
	ps2_put_keycode(s->pc->kbd, keypress, keycode);
}

static void handle_key(Console *s, int code, int value)
{
	bool down = value != 0;   /* 2 is autorepeat: a make code again */
	bool ctrl = key_pressed[KEY_LEFTCTRL] || key_pressed[KEY_RIGHTCTRL];
	bool alt = key_pressed[KEY_LEFTALT] || key_pressed[KEY_RIGHTALT];

	if (code > KEYCODE_MAX || code == 0)
		return;
	if (!down && !key_pressed[code])
		return;   /* released a key we did not see go down */

	if (down && value == 1) {
		if (code == KEY_RIGHTBRACE && ctrl) {
			g_running = 0;
			return;
		}
		if (code == KEY_LEFTBRACE && ctrl) {
			s->osd_enabled = !s->osd_enabled;
			osd_attach_emulink(s->osd, s->pc->emulink);
			osd_attach_ide(s->osd, s->pc->ide, s->pc->ide2);
			s->pc->full_update = s->osd_enabled ? 1 : 2;
			return;
		}
		if (ctrl && alt && s->vt_process) {
			int vt = 0;
			if (code >= KEY_F1 && code <= KEY_F10)
				vt = code - KEY_F1 + 1;
			else if (code == KEY_F11 || code == KEY_F12)
				vt = code - KEY_F11 + 11;
			if (vt) {
				ioctl(s->tty_fd, VT_ACTIVATE, vt);
				return;
			}
		}
	}

	key_pressed[code] = down;
	if (s->osd_enabled)
		osd_handle_key(s->osd, code, down);
	else
		ps2_put_keycode(s->pc->kbd, down, code);
}

static void handle_button(Console *s, int code, int down)
{
	int bit;
	switch (code) {
	case BTN_LEFT: bit = 1; break;
	case BTN_RIGHT: bit = 2; break;
	case BTN_MIDDLE: bit = 4; break;
	default: return;
	}
	if (down)
		s->mbtn |= bit;
	else
		s->mbtn &= ~bit;
	if (s->osd_enabled) {
		if (bit == 1)
			osd_handle_mouse_button(s->osd, s->mx, s->my, down, 1);
	} else {
		s->rel_pending = true;
	}
}

static void flush_rel(Console *s)
{
	if (!s->rel_pending)
		return;
	if (s->osd_enabled) {
		s->mx += s->rel_x;
		s->my += s->rel_y;
		if (s->mx < 0) s->mx = 0;
		if (s->my < 0) s->my = 0;
		if (s->mx >= s->width) s->mx = s->width - 1;
		if (s->my >= s->height) s->my = s->height - 1;
		if (s->rel_x || s->rel_y)
			osd_handle_mouse_motion(s->osd, s->mx, s->my);
	} else {
		ps2_mouse_event(s->pc->mouse, s->rel_x, s->rel_y,
				s->rel_z, s->mbtn);
	}
	s->rel_x = s->rel_y = s->rel_z = 0;
	s->rel_pending = false;
}

static void input_read(Console *s, int i)
{
	InputDev *d = &s->in[i];
	struct input_event ev[64];
	for (;;) {
		ssize_t n = read(d->fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == ENODEV) {   /* unplugged */
				input_close(s, i);
			}
			return;
		}
		if (n < (ssize_t) sizeof(ev[0]))
			return;
		if (!d->grabbed || !s->active)
			continue;   /* not ours yet: drop */
		for (int j = 0; j < n / (ssize_t) sizeof(ev[0]); j++) {
			struct input_event *e = &ev[j];
			switch (e->type) {
			case EV_KEY:
				if (e->code >= BTN_MISC && e->code < KEY_OK)
					handle_button(s, e->code, e->value);
				else
					handle_key(s, e->code, e->value);
				break;
			case EV_REL:
				if (e->code == REL_X)
					s->rel_x += e->value;
				else if (e->code == REL_Y)
					s->rel_y += e->value;
				else if (e->code == REL_WHEEL)
					s->rel_z -= e->value;
				else
					break;
				s->rel_pending = true;
				break;
			case EV_SYN:
				if (e->code == SYN_REPORT)
					flush_rel(s);
				break;
			}
		}
	}
}

/* --- VT --- */

static void vt_signal(int sig)
{
	if (sig == SIGUSR1)
		g_vt_release = 1;
	else
		g_vt_acquire = 1;
}

static void vt_init(Console *s)
{
	s->tty_fd = -1;
	int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (fd < 0 || ioctl(fd, KDGETMODE, &s->saved_kd_mode) < 0) {
		if (fd >= 0)
			close(fd);
		fprintf(stderr, "tiny386_drm: not started from a VT: "
			"no VT switching, and the console may draw over the "
			"screen\n");
		return;
	}
	s->tty_fd = fd;
	if (ioctl(fd, KDSETMODE, KD_GRAPHICS) < 0)
		fprintf(stderr, "tiny386_drm: KDSETMODE: %s\n", strerror(errno));

	struct sigaction sa = {0};
	sa.sa_handler = vt_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	struct vt_mode vtm = {0};
	vtm.mode = VT_PROCESS;
	vtm.relsig = SIGUSR1;
	vtm.acqsig = SIGUSR2;
	if (ioctl(fd, VT_SETMODE, &vtm) == 0)
		s->vt_process = true;
	else
		fprintf(stderr, "tiny386_drm: VT_SETMODE: %s\n", strerror(errno));
}

static void vt_handle(Console *s)
{
	if (g_vt_release) {
		g_vt_release = 0;
		if (s->active) {
			flush_rel(s);
			reset_keys(s);
			input_ungrab_all(s);
			drm_handle_events(s, s->flip_pending ? 50 : 0);
			s->flip_pending = false;
			drmDropMaster(s->drm_fd);
			s->active = false;
		}
		ioctl(s->tty_fd, VT_RELDISP, 1);
	}
	if (g_vt_acquire) {
		g_vt_acquire = 0;
		ioctl(s->tty_fd, VT_RELDISP, VT_ACKACQ);
		if (!s->active) {
			if (drmSetMaster(s->drm_fd) < 0)
				fprintf(stderr, "tiny386_drm: drmSetMaster: %s\n",
					strerror(errno));
			drmModeSetCrtc(s->drm_fd, s->crtc_id,
				       s->buf[s->front].fb_id, 0, 0,
				       &s->conn_id, 1, &s->mode);
			s->active = true;
			present(s);
		}
	}
}

/* --- setup and teardown --- */

static void cleanup(void)
{
	Console *s = g_console;
	if (!s)
		return;
	g_console = NULL;
	for (int i = 0; i < s->nin; i++) {
		if (s->in[i].grabbed)
			ioctl(s->in[i].fd, EVIOCGRAB, 0);
		close(s->in[i].fd);
	}
	s->nin = 0;
	if (s->active) {
		drm_handle_events(s, s->flip_pending ? 50 : 0);
		drmModeCrtc *c = s->saved_crtc;
		if (c && c->mode_valid)
			drmModeSetCrtc(s->drm_fd, c->crtc_id, c->buffer_id,
				       c->x, c->y, &s->conn_id, 1, &c->mode);
	}
	for (int i = 0; i < s->nbufs; i++)
		dumb_destroy(s, &s->buf[i]);
	if (s->saved_crtc)
		drmModeFreeCrtc(s->saved_crtc);
	drmDropMaster(s->drm_fd);
	close(s->drm_fd);
	if (s->tty_fd >= 0) {
		struct vt_mode vtm = {0};
		vtm.mode = VT_AUTO;
		ioctl(s->tty_fd, VT_SETMODE, &vtm);
		ioctl(s->tty_fd, KDSETMODE, s->saved_kd_mode);
		/* keys that reached the tty before the grab */
		tcflush(s->tty_fd, TCIFLUSH);
		close(s->tty_fd);
	}
}

static void sig_handler(int sig)
{
	g_running = 0;
}

Console *console_init(int width, int height)
{
	Console *s = calloc(1, sizeof(Console));
	s->osd = osd_init();
	s->osd_enabled = false;
	s->width = width;
	s->height = height;
	s->fb = bigmalloc(s->width * s->height * 4);
	s->tty_fd = -1;
	s->mx = width / 2;
	s->my = height / 2;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGHUP, sig_handler);

	drm_init(s);
	g_console = s;
	atexit(cleanup);
	vt_init(s);
	input_scan(s);
	if (s->nin == 0)
		fprintf(stderr, "tiny386_drm: no keyboard or mouse in "
			"/dev/input (are you in the input group?)\n");
	input_try_grab(s);
	osd_attach_console(s->osd, s);
	return s;
}

static void console_poll(Console *s)
{
	if (g_vt_release || g_vt_acquire)
		vt_handle(s);

	struct pollfd pfd[MAX_INPUTS + 1];
	int n = 0;
	pfd[n++] = (struct pollfd) { s->drm_fd, POLLIN, 0 };
	for (int i = 0; i < s->nin; i++)
		pfd[n++] = (struct pollfd) { s->in[i].fd, POLLIN, 0 };
	if (poll(pfd, n, 0) <= 0)
		goto out;
	if (pfd[0].revents & POLLIN)
		drm_handle_events(s, 0);
	/* backwards: input_read() may move the last device into slot i */
	for (int i = n - 2; i >= 0; i--)
		if (pfd[i + 1].revents & (POLLIN | POLLERR | POLLHUP))
			input_read(s, i);
out:
	if (s->active) {
		uint32_t now = get_uticks();
		if (now - s->last_scan > 2000000)
			input_scan(s);
		input_try_grab(s);
	}
}

#ifdef DRM_AUDIO
static void cnfa_callback(struct CNFADriver *sd, short *out, short *in,
			  int framesp, int framesr)
{
	PC *pc = sd->opaque;
	int channels = sd->channelsPlay;
	memset(out, 0, framesp * channels * 2);
	mixer_callback(pc, (void *) out, framesp * channels * 2);
}

static void setup_audio(PC *pc)
{
	if (!CNFAInit(NULL, "tiny386_audio", cnfa_callback,
		      44100, 0, 2, 0, 1024, 0, 0, pc))
		fprintf(stderr, "tiny386_drm: no audio\n");
}
#else
static void setup_audio(PC *pc)
{
}
#endif

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [-kvm] inifile\n"
		"Runs full-screen on a Linux console with KMS/DRM.\n"
		"ctrl + ] quits, ctrl + [ shows/hides the OSD, "
		"ctrl + alt + Fn switches VT.\n"
		"Environment: TINY386_DRM_DEVICE=/dev/dri/cardN, "
		"TINY386_DRM_MODE=WxH\n",
		argv0);
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

	bool enable_kvm = false;
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}
	for (int i = 1; i < argc - 1; i++) {
		if (strcmp(argv[i], "-kvm") == 0) {
			enable_kvm = true;
		} else {
			usage(argv[0]);
			return 1;
		}
	}
	const char *argv1 = argv[argc - 1];
	ne2000_set_config_file(argv1);

	int err = ini_parse(argv1, parse_conf_ini, &conf);
	if (err) {
		fprintf(stderr, "error %d\n", err);
		return err;
	}
	if (enable_kvm)
		conf.cpu_gen = -1;

	Console *console = console_init(conf.width, conf.height);
	PC *pc = pc_new(redraw, console, (u8 *) console->fb, &conf);
	console->pc = pc;
	setup_audio(pc);
	load_bios_and_reset(pc);

	pc->boot_start_time = get_uticks();
	for (; g_running && pc->shutdown_state != 8;) {
		pc_step(pc);
		console_poll(console);
		pc_vga_step(pc);
	}
	return 0;
}
