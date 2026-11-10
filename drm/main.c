/*
 * Linux KMS/DRM frontend: runs full-screen from a text console (VT), with no
 * X11, Wayland or SDL.
 *
 * Display: libdrm, a connected connector in its preferred mode, two dumb
 * buffers flipped with drmModePageFlip (or one buffer and drmModeDirtyFB
 * where page flips are not available), XRGB8888 or, where the display only
 * takes that, RGB565.  What is shown is the guest's picture (the part of
 * the frame buffer vga.c last drew in), centred and scaled as -scale says:
 * fit (the default; whole-number factors up, a box filter down), stretch,
 * or 1:1 with ctrl + alt + arrows to pan.  On small displays text mode can
 * be drawn with a 4x10 font (-smalltext), so 80x25 fits in 320x250.
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
#include <strings.h>
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
#include <drm_fourcc.h>
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

/* --- options --- */

enum { SCALE_FIT, SCALE_STRETCH, SCALE_NONE };

typedef struct {
	int scale;              /* SCALE_* */
	int small_text;         /* -1: when the display is small */
	int width, height;      /* the guest's frame buffer, 0: from the ini */
	int bpp;                /* 0: what the display takes */
	int mode_w, mode_h;     /* 0: the preferred mode */
	const char *device;     /* NULL: the first with a display */
} DrmOpts;

static bool parse_scale(const char *v, int *scale)
{
	if (strcasecmp(v, "fit") == 0)
		*scale = SCALE_FIT;
	else if (strcasecmp(v, "stretch") == 0 || strcasecmp(v, "fill") == 0)
		*scale = SCALE_STRETCH;
	else if (strcmp(v, "1:1") == 0 || strcasecmp(v, "none") == 0)
		*scale = SCALE_NONE;
	else
		return false;
	return true;
}

static bool parse_size(const char *v, int *w, int *h)
{
	return sscanf(v, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

static int parse_small_text(const char *v)
{
	if (strcasecmp(v, "auto") == 0)
		return -1;
	return atoi(v) != 0;
}

/* [drm] in the ini file; other frontends skip the section */
static int parse_drm_ini(void *user, const char *section,
			 const char *name, const char *value)
{
	DrmOpts *o = user;
	if (strcmp(section, "drm") != 0)
		return 1;
	if (strcmp(name, "scale") == 0) {
		if (!parse_scale(value, &o->scale))
			fprintf(stderr, "tiny386_drm: bad scale = %s\n", value);
	} else if (strcmp(name, "small_text") == 0) {
		o->small_text = parse_small_text(value);
	} else if (strcmp(name, "mode") == 0) {
		if (!parse_size(value, &o->mode_w, &o->mode_h))
			o->mode_w = o->mode_h = 0;
	} else if (strcmp(name, "bpp") == 0) {
		o->bpp = atoi(value);
	} else if (strcmp(name, "device") == 0) {
		o->device = strdup(value);
	}
	return 1;
}

/* --- display --- */

typedef struct {
	uint32_t handle, fb_id, pitch;
	uint64_t size;
	uint8_t *map;
	unsigned gen;           /* the layout it was cleared for */
} DumbBuf;

#define MAX_INPUTS 32
typedef struct {
	int fd;
	char path[32];
	bool is_kbd, is_mouse;
	bool grabbed;
} InputDev;

typedef struct {
	int x, y, w, h;
} Rect;

/* the OSD's window (osd/osd.c), in frame buffer pixels */
static const Rect osd_rect = { 40, 40, 320, 240 };

typedef struct {
	int width, height;      /* the guest's frame buffer */
	uint32_t *fb;
	PC *pc;
	OSD *osd;
	bool osd_enabled;
	DrmOpts *opts;

	/* DRM */
	int drm_fd;
	uint32_t conn_id, crtc_id;
	int crtc_index;
	drmModeModeInfo mode;
	drmModeCrtc *saved_crtc;
	int bpp;                /* of the dumb buffers: 32 or 16 */
	DumbBuf buf[2];
	int nbufs;
	int front;
	bool flip_pending;
	bool dirty;
	bool active;            /* our VT is shown and we are DRM master */

	/* layout: the source rectangle, where it goes, and lookup tables */
	Rect src, dst;
	unsigned gen;
	int *xs0, *xs1, *ys0, *ys1;
	bool box;               /* some output pixel covers several sources */
	int pan_x, pan_y;       /* SCALE_NONE: the picture's pixel at dst's corner */

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
	creq.bpp = s->bpp;
	if (drmIoctl(s->drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
		return false;
	b->handle = creq.handle;
	b->pitch = creq.pitch;
	b->size = creq.size;
	if (drmModeAddFB(s->drm_fd, creq.width, creq.height,
			 s->bpp == 16 ? 16 : 24, s->bpp,
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
	b->gen = 0;
	return true;
}

static void dumb_destroy(Console *s, DumbBuf *b)
{
	struct drm_mode_destroy_dumb dreq = { .handle = b->handle };
	munmap(b->map, b->size);
	drmModeRmFB(s->drm_fd, b->fb_id);
	drmIoctl(s->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
}

static int crtc_index(drmModeRes *res, uint32_t crtc)
{
	for (int j = 0; j < res->count_crtcs; j++)
		if (res->crtcs[j] == crtc)
			return j;
	return -1;
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
 * want_h choose a mode other than the preferred.
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
				int j;
				for (j = 0; j < conn->count_modes; j++) {
					if (conn->modes[j].hdisplay == want_w &&
					    conn->modes[j].vdisplay == want_h) {
						m = j;
						break;
					}
				}
				if (j == conn->count_modes)
					fprintf(stderr, "tiny386_drm: no %dx%d "
						"mode on %s\n", want_w, want_h,
						path);
			}
			uint32_t crtc = find_crtc(fd, res, conn);
			if (crtc) {
				s->drm_fd = fd;
				s->conn_id = conn->connector_id;
				s->crtc_id = crtc;
				s->crtc_index = crtc_index(res, crtc);
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

/*
 * 32 bpp (XRGB8888) unless the CRTC's primary plane does not take it but
 * takes RGB565, as some small SPI panels (panel-mipi-dbi and the like).
 */
static int drm_pick_bpp(Console *s)
{
	bool xrgb = false, rgb565 = false, seen = false;
	drmSetClientCap(s->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
	drmModePlaneRes *pr = drmModeGetPlaneResources(s->drm_fd);
	for (uint32_t i = 0; pr && i < pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(s->drm_fd, pr->planes[i]);
		if (!p)
			continue;
		bool primary = false;
		drmModeObjectProperties *props = drmModeObjectGetProperties(
			s->drm_fd, p->plane_id, DRM_MODE_OBJECT_PLANE);
		for (uint32_t j = 0; props && j < props->count_props; j++) {
			drmModePropertyRes *prop =
				drmModeGetProperty(s->drm_fd, props->props[j]);
			if (prop && strcmp(prop->name, "type") == 0 &&
			    props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY)
				primary = true;
			drmModeFreeProperty(prop);
		}
		drmModeFreeObjectProperties(props);
		if (primary && s->crtc_index >= 0 &&
		    (p->possible_crtcs & (1u << s->crtc_index))) {
			seen = true;
			for (uint32_t j = 0; j < p->count_formats; j++) {
				if (p->formats[j] == DRM_FORMAT_XRGB8888)
					xrgb = true;
				if (p->formats[j] == DRM_FORMAT_RGB565)
					rgb565 = true;
			}
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pr);
	drmSetClientCap(s->drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 0);
	if (seen && !xrgb && rgb565)
		return 16;
	return 32;
}

static void drm_init(Console *s)
{
	DrmOpts *o = s->opts;
	bool ok = false;
	if (o->device) {
		ok = drm_try_device(s, o->device, o->mode_w, o->mode_h);
	} else {
		for (int i = 0; i < 16 && !ok; i++) {
			char path[32];
			snprintf(path, sizeof(path), "/dev/dri/card%d", i);
			ok = drm_try_device(s, path, o->mode_w, o->mode_h);
		}
	}
	if (!ok)
		die("no DRM device with a connected display "
		    "(set TINY386_DRM_DEVICE=/dev/dri/cardN?)");

	if (drmSetMaster(s->drm_fd) < 0)
		fprintf(stderr, "tiny386_drm: drmSetMaster: %s "
			"(is a display server running on this device?)\n",
			strerror(errno));

	s->saved_crtc = drmModeGetCrtc(s->drm_fd, s->crtc_id);

	s->bpp = o->bpp == 16 || o->bpp == 32 ? o->bpp : drm_pick_bpp(s);
	s->nbufs = 0;
	for (int i = 0; i < 2; i++)
		if (dumb_create(s, &s->buf[i]))
			s->nbufs++;
		else
			break;
	if (s->nbufs == 0 && s->bpp == 32 && !o->bpp) {
		/* some drivers still want 16 bpp */
		s->bpp = 16;
		for (int i = 0; i < 2; i++)
			if (dumb_create(s, &s->buf[i]))
				s->nbufs++;
			else
				break;
	}
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
	s->gen = 1;
	fprintf(stderr, "tiny386_drm: %dx%d@%d %d bpp, %d buffer%s\n",
		s->mode.hdisplay, s->mode.vdisplay, s->mode.vrefresh, s->bpp,
		s->nbufs, s->nbufs > 1 ? "s" : "");
}

static Rect rect_clip(Rect r, int w, int h)
{
	if (r.x < 0) { r.w += r.x; r.x = 0; }
	if (r.y < 0) { r.h += r.y; r.y = 0; }
	if (r.w > w - r.x) r.w = w - r.x;
	if (r.h > h - r.y) r.h = h - r.y;
	if (r.w <= 0 || r.h <= 0)
		r = (Rect) { 0, 0, w, h };
	return r;
}

/* what to show: the guest's picture, or the OSD over it */
static Rect source_rect(Console *s)
{
	int pw = s->mode.hdisplay, ph = s->mode.vdisplay;
	Rect r;
	if (s->osd_enabled) {
		/* the whole frame if it is shown 1:1 or bigger, else the
		   OSD's window, so that it stays readable */
		if (s->width <= pw && s->height <= ph)
			r = (Rect) { 0, 0, s->width, s->height };
		else
			r = rect_clip(osd_rect, s->width, s->height);
	} else {
		vga_get_picture(s->pc->vga, &r.x, &r.y, &r.w, &r.h);
		r = rect_clip(r, s->width, s->height);
	}
	if (s->opts->scale == SCALE_NONE && (r.w > pw || r.h > ph)) {
		/* a window on the picture, where it has been panned to */
		int maxx = r.w > pw ? r.w - pw : 0;
		int maxy = r.h > ph ? r.h - ph : 0;
		if (s->pan_x > maxx) s->pan_x = maxx;
		if (s->pan_y > maxy) s->pan_y = maxy;
		if (s->pan_x < 0) s->pan_x = 0;
		if (s->pan_y < 0) s->pan_y = 0;
		r.x += s->pan_x;
		r.y += s->pan_y;
		if (r.w > pw) r.w = pw;
		if (r.h > ph) r.h = ph;
	}
	return r;
}

static void make_map(int *s0, int *s1, int n, int src_off, int src_n)
{
	for (int i = 0; i < n; i++) {
		int a = (long) i * src_n / n;
		int b = (long) (i + 1) * src_n / n;
		if (b <= a)
			b = a + 1;
		s0[i] = src_off + a;
		s1[i] = src_off + b;
	}
}

static void layout(Console *s, Rect src)
{
	int pw = s->mode.hdisplay, ph = s->mode.vdisplay;
	Rect dst;
	if (s->opts->scale == SCALE_STRETCH) {
		dst = (Rect) { 0, 0, pw, ph };
	} else if (s->opts->scale == SCALE_NONE) {
		dst.w = src.w;
		dst.h = src.h;
	} else {
		int k = pw / src.w < ph / src.h ? pw / src.w : ph / src.h;
		if (k >= 1) {
			dst.w = src.w * k;
			dst.h = src.h * k;
		} else if ((long) pw * src.h <= (long) ph * src.w) {
			dst.w = pw;
			dst.h = (long) src.h * pw / src.w;
		} else {
			dst.h = ph;
			dst.w = (long) src.w * ph / src.h;
		}
		if (dst.w < 1) dst.w = 1;
		if (dst.h < 1) dst.h = 1;
	}
	dst.x = (pw - dst.w) / 2;
	dst.y = (ph - dst.h) / 2;

	if (s->xs0 && memcmp(&src, &s->src, sizeof(src)) == 0 &&
	    memcmp(&dst, &s->dst, sizeof(dst)) == 0)
		return;
	bool moved = !s->xs0 || memcmp(&dst, &s->dst, sizeof(dst)) != 0;
	s->src = src;
	s->dst = dst;
	free(s->xs0); free(s->xs1); free(s->ys0); free(s->ys1);
	s->xs0 = malloc(sizeof(int) * dst.w);
	s->xs1 = malloc(sizeof(int) * dst.w);
	s->ys0 = malloc(sizeof(int) * dst.h);
	s->ys1 = malloc(sizeof(int) * dst.h);
	make_map(s->xs0, s->xs1, dst.w, src.x, src.w);
	make_map(s->ys0, s->ys1, dst.h, src.y, src.h);
	s->box = src.w > dst.w || src.h > dst.h;
	if (moved)
		s->gen++;   /* the borders change: clear the buffers */
}

static inline uint16_t to565(uint32_t c)
{
	return ((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f);
}

static void put_row(Console *s, uint8_t *row, const uint32_t *px, int n)
{
	if (s->bpp == 32) {
		memcpy(row, px, n * 4);
	} else {
		uint16_t *d = (uint16_t *) row;
		for (int x = 0; x < n; x++)
			d[x] = to565(px[x]);
	}
}

/*
 * Each output pixel is the average of the source pixels it covers
 * [xs0, xs1) x [ys0, ys1): a box filter going down, nearest going up.
 */
static void blit(Console *s, DumbBuf *b)
{
	if (b->gen != s->gen) {
		memset(b->map, 0, b->size);
		b->gen = s->gen;
	}
	const uint32_t *src = s->fb;
	int sw = s->width;
	int dw = s->dst.w;
	int bypp = s->bpp / 8;
	uint8_t *row0 = b->map + (size_t) s->dst.y * b->pitch + s->dst.x * bypp;
	static uint32_t *line;
	static int line_n;
	if (line_n < dw) {
		free(line);
		line = malloc(sizeof(uint32_t) * dw);
		line_n = dw;
	}
	int last_y0 = -1, last_y1 = -1;
	uint8_t *last_row = NULL;
	for (int y = 0; y < s->dst.h; y++) {
		uint8_t *row = row0 + (size_t) y * b->pitch;
		int y0 = s->ys0[y], y1 = s->ys1[y];
		if (y0 == last_y0 && y1 == last_y1) {
			memcpy(row, last_row, dw * bypp);
			continue;
		}
		if (!s->box) {
			const uint32_t *sl = src + (size_t) y0 * sw;
			if (dw == s->src.w) {
				put_row(s, row, sl + s->src.x, dw);
			} else {
				for (int x = 0; x < dw; x++)
					line[x] = sl[s->xs0[x]];
				put_row(s, row, line, dw);
			}
		} else {
			for (int x = 0; x < dw; x++) {
				uint32_t r = 0, g = 0, bl = 0, n = 0;
				int x0 = s->xs0[x], x1 = s->xs1[x];
				for (int yy = y0; yy < y1; yy++) {
					const uint32_t *p = src + (size_t) yy * sw;
					for (int xx = x0; xx < x1; xx++) {
						uint32_t c = p[xx];
						r += (c >> 16) & 0xff;
						g += (c >> 8) & 0xff;
						bl += c & 0xff;
					}
					n += x1 - x0;
				}
				line[x] = ((r / n) << 16) | ((g / n) << 8) |
					  (bl / n);
			}
			put_row(s, row, line, dw);
		}
		last_y0 = y0;
		last_y1 = y1;
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
	layout(s, source_rect(s));
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
	drmModeClip clip = { 0, 0, s->mode.hdisplay, s->mode.vdisplay };
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

static void toggle_osd(Console *s)
{
	s->osd_enabled = !s->osd_enabled;
	osd_attach_emulink(s->osd, s->pc->emulink);
	osd_attach_ide(s->osd, s->pc->ide, s->pc->ide2);
	s->pc->full_update = s->osd_enabled ? 1 : 2;
	if (s->osd_enabled) {
		/* the pointer starts in the middle of the OSD's window */
		Rect r = rect_clip(osd_rect, s->width, s->height);
		s->mx = r.x + r.w / 2;
		s->my = r.y + r.h / 2;
	}
	present(s);
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

	if (down && ctrl && alt && s->opts->scale == SCALE_NONE) {
		/* pan the 1:1 picture by a quarter of the display */
		int dx = 0, dy = 0;
		switch (code) {
		case KEY_LEFT: dx = -1; break;
		case KEY_RIGHT: dx = 1; break;
		case KEY_UP: dy = -1; break;
		case KEY_DOWN: dy = 1; break;
		}
		if (dx || dy) {
			s->pan_x += dx * (s->mode.hdisplay / 4);
			s->pan_y += dy * (s->mode.vdisplay / 4);
			present(s);
			return;
		}
	}

	if (down && value == 1) {
		if (code == KEY_RIGHTBRACE && ctrl) {
			g_running = 0;
			return;
		}
		if (code == KEY_LEFTBRACE && ctrl) {
			toggle_osd(s);
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
		/* the pointer moves in frame buffer pixels, kept to what is
		   on the display */
		Rect r = s->xs0 ? s->src : (Rect) { 0, 0, s->width, s->height };
		s->mx += s->rel_x;
		s->my += s->rel_y;
		if (s->mx < r.x) s->mx = r.x;
		if (s->my < r.y) s->my = r.y;
		if (s->mx >= r.x + r.w) s->mx = r.x + r.w - 1;
		if (s->my >= r.y + r.h) s->my = r.y + r.h - 1;
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

/* the display, opened before the guest's frame buffer is sized */
Console *console_init(DrmOpts *opts)
{
	Console *s = calloc(1, sizeof(Console));
	s->opts = opts;
	s->osd = osd_init();
	s->osd_enabled = false;
	s->tty_fd = -1;

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGHUP, sig_handler);

	drm_init(s);
	g_console = s;
	atexit(cleanup);
	return s;
}

void console_start(Console *s, int width, int height)
{
	s->width = width;
	s->height = height;
	s->fb = bigmalloc(s->width * s->height * 4);
	s->mx = width / 2;
	s->my = height / 2;
	vt_init(s);
	input_scan(s);
	if (s->nin == 0)
		fprintf(stderr, "tiny386_drm: no keyboard or mouse in "
			"/dev/input (are you in the input group?)\n");
	input_try_grab(s);
	osd_attach_console(s->osd, s);
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
		"Usage: %s [options] inifile\n"
		"Runs full-screen on a Linux console with KMS/DRM.\n"
		"  -kvm                 run with KVM (USE_CPUABS=y builds)\n"
		"  -scale fit|stretch|1:1\n"
		"                       fit: keep the aspect, whole factors up and "
		"a box filter down\n"
		"                       (default); stretch: fill the display; "
		"1:1: no scaling,\n"
		"                       ctrl + alt + arrows pan a picture bigger "
		"than the display\n"
		"  -smalltext, -nosmalltext\n"
		"                       text mode with a 4x10 font, 80x25 in "
		"320x250 (default: when\n"
		"                       the display is smaller than 640x400)\n"
		"  -size WxH            the guest's frame buffer ([display] "
		"width/height)\n"
		"  -mode WxH            a display mode other than the preferred\n"
		"The ini file's [drm] section takes scale, small_text "
		"(0/1/auto), mode, bpp\n"
		"(16/32) and device; the environment TINY386_DRM_DEVICE, "
		"TINY386_DRM_MODE\n"
		"and TINY386_DRM_BPP.\n"
		"ctrl + ] quits, ctrl + [ shows/hides the OSD, "
		"ctrl + alt + Fn switches VT.\n",
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

	DrmOpts opts = { .scale = SCALE_FIT, .small_text = -1 };
	DrmOpts cli = { .scale = -1, .small_text = -2 };
	bool enable_kvm = false;
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}
	for (int i = 1; i < argc - 1; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc - 1 ? argv[i + 1] : NULL;
		if (strcmp(a, "-kvm") == 0) {
			enable_kvm = true;
		} else if (strcmp(a, "-smalltext") == 0) {
			cli.small_text = 1;
		} else if (strcmp(a, "-nosmalltext") == 0) {
			cli.small_text = 0;
		} else if (strcmp(a, "-scale") == 0 && v &&
			   parse_scale(v, &cli.scale)) {
			i++;
		} else if (strcmp(a, "-size") == 0 && v &&
			   parse_size(v, &cli.width, &cli.height)) {
			i++;
		} else if (strcmp(a, "-mode") == 0 && v &&
			   parse_size(v, &cli.mode_w, &cli.mode_h)) {
			i++;
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
	ini_parse(argv1, parse_drm_ini, &opts);
	if (enable_kvm)
		conf.cpu_gen = -1;

	/* the command line, then the environment, over the ini file */
	const char *e;
	if ((e = getenv("TINY386_DRM_DEVICE")))
		opts.device = e;
	if ((e = getenv("TINY386_DRM_MODE")) &&
	    !parse_size(e, &opts.mode_w, &opts.mode_h))
		opts.mode_w = opts.mode_h = 0;
	if ((e = getenv("TINY386_DRM_BPP")))
		opts.bpp = atoi(e);
	if (cli.scale >= 0)
		opts.scale = cli.scale;
	if (cli.small_text >= -1)
		opts.small_text = cli.small_text;
	if (cli.mode_w) {
		opts.mode_w = cli.mode_w;
		opts.mode_h = cli.mode_h;
	}
	if (cli.width) {
		conf.width = cli.width;
		conf.height = cli.height;
	}

	Console *console = console_init(&opts);
	console_start(console, conf.width, conf.height);
	PC *pc = pc_new(redraw, console, (u8 *) console->fb, &conf);
	console->pc = pc;

	int small = opts.small_text;
	if (small < 0)
		small = console->mode.hdisplay < 640 ||
			console->mode.vdisplay < 400;
	vga_set_small_text(pc->vga, small);
	if (small)
		fprintf(stderr, "tiny386_drm: text mode with the small font\n");

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
