/*
 * KMS through the kernel's DRM ioctls, without libdrm: the handful of
 * calls drm/main.c makes.  Only the kernel uapi headers are needed
 * (<drm/drm.h>, <drm/drm_mode.h>, <drm/drm_fourcc.h>: linux-libc-dev, a
 * sysroot's kernel headers, or the ones zig ships), and nothing is linked.
 *
 * The "get" ioctls that return arrays are called twice: once for the
 * counts, then with buffers of that size.  If a count grew in between
 * (a hotplug), they are called again.
 */
#ifndef KMS_H
#define KMS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>

/* not in the uapi headers: drm_mode_get_connector.connection, plane "type" */
#define KMS_CONNECTED 1
#define KMS_PLANE_TYPE_PRIMARY 1

#define KMS_PTR(p) ((uint64_t)(uintptr_t)(p))

static int kms_ioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r;
}

/* a zeroed array of n elements of size sz; never NULL for n == 0 */
static void *kms_alloc(uint32_t n, size_t sz)
{
	return calloc(n ? n : 1, sz);
}

static int kms_get_cap(int fd, uint64_t cap, uint64_t *value)
{
	struct drm_get_cap c = { .capability = cap };
	if (kms_ioctl(fd, DRM_IOCTL_GET_CAP, &c) < 0)
		return -1;
	*value = c.value;
	return 0;
}

static int kms_set_client_cap(int fd, uint64_t cap, uint64_t value)
{
	struct drm_set_client_cap c = { .capability = cap, .value = value };
	return kms_ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, &c);
}

static int kms_set_master(int fd)
{
	return kms_ioctl(fd, DRM_IOCTL_SET_MASTER, NULL);
}

static int kms_drop_master(int fd)
{
	return kms_ioctl(fd, DRM_IOCTL_DROP_MASTER, NULL);
}

/* --- resources: CRTCs and connectors --- */

typedef struct {
	uint32_t count_crtcs, count_connectors;
	uint32_t *crtcs, *connectors;
} KmsRes;

static void kms_free_res(KmsRes *r)
{
	if (!r)
		return;
	free(r->crtcs);
	free(r->connectors);
	free(r);
}

static KmsRes *kms_get_res(int fd)
{
	KmsRes *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	for (;;) {
		struct drm_mode_card_res c = {0};
		if (kms_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &c) < 0)
			break;
		uint32_t ncrtc = c.count_crtcs, nconn = c.count_connectors;
		free(r->crtcs);
		free(r->connectors);
		r->crtcs = kms_alloc(ncrtc, sizeof(uint32_t));
		r->connectors = kms_alloc(nconn, sizeof(uint32_t));
		if (!r->crtcs || !r->connectors)
			break;
		memset(&c, 0, sizeof(c));
		c.count_crtcs = ncrtc;
		c.crtc_id_ptr = KMS_PTR(r->crtcs);
		c.count_connectors = nconn;
		c.connector_id_ptr = KMS_PTR(r->connectors);
		if (kms_ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &c) < 0)
			break;
		if (c.count_crtcs > ncrtc || c.count_connectors > nconn)
			continue;
		r->count_crtcs = c.count_crtcs;
		r->count_connectors = c.count_connectors;
		return r;
	}
	kms_free_res(r);
	return NULL;
}

/* --- connectors and encoders --- */

typedef struct {
	uint32_t connector_id, encoder_id, connection;
	uint32_t count_modes, count_encoders;
	struct drm_mode_modeinfo *modes;
	uint32_t *encoders;
} KmsConnector;

static void kms_free_connector(KmsConnector *k)
{
	if (!k)
		return;
	free(k->modes);
	free(k->encoders);
	free(k);
}

/* (asking for the mode count makes the kernel probe the connector) */
static KmsConnector *kms_get_connector(int fd, uint32_t id)
{
	KmsConnector *k = calloc(1, sizeof(*k));
	if (!k)
		return NULL;
	for (;;) {
		struct drm_mode_get_connector c = { .connector_id = id };
		if (kms_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
			break;
		uint32_t nmodes = c.count_modes, nenc = c.count_encoders;
		free(k->modes);
		free(k->encoders);
		k->modes = kms_alloc(nmodes, sizeof(*k->modes));
		k->encoders = kms_alloc(nenc, sizeof(uint32_t));
		if (!k->modes || !k->encoders)
			break;
		memset(&c, 0, sizeof(c));
		c.connector_id = id;
		c.count_modes = nmodes;
		c.modes_ptr = KMS_PTR(k->modes);
		c.count_encoders = nenc;
		c.encoders_ptr = KMS_PTR(k->encoders);
		if (kms_ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
			break;
		if (c.count_modes > nmodes || c.count_encoders > nenc)
			continue;
		k->connector_id = c.connector_id;
		k->encoder_id = c.encoder_id;
		k->connection = c.connection;
		k->count_modes = c.count_modes;
		k->count_encoders = c.count_encoders;
		return k;
	}
	kms_free_connector(k);
	return NULL;
}

static int kms_get_encoder(int fd, uint32_t id, struct drm_mode_get_encoder *e)
{
	memset(e, 0, sizeof(*e));
	e->encoder_id = id;
	return kms_ioctl(fd, DRM_IOCTL_MODE_GETENCODER, e);
}

/* --- CRTCs and frame buffers --- */

static int kms_get_crtc(int fd, uint32_t id, struct drm_mode_crtc *c)
{
	memset(c, 0, sizeof(*c));
	c->crtc_id = id;
	return kms_ioctl(fd, DRM_IOCTL_MODE_GETCRTC, c);
}

static int kms_set_crtc(int fd, uint32_t crtc, uint32_t fb, uint32_t x,
			uint32_t y, uint32_t *conns, uint32_t nconns,
			const struct drm_mode_modeinfo *mode)
{
	struct drm_mode_crtc c = {
		.set_connectors_ptr = KMS_PTR(conns),
		.count_connectors = nconns,
		.crtc_id = crtc,
		.fb_id = fb,
		.x = x,
		.y = y,
	};
	if (mode) {
		c.mode = *mode;
		c.mode_valid = 1;
	}
	return kms_ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &c);
}

static int kms_add_fb(int fd, uint32_t w, uint32_t h, uint8_t depth,
		      uint8_t bpp, uint32_t pitch, uint32_t handle,
		      uint32_t *fb_id)
{
	struct drm_mode_fb_cmd f = {
		.width = w, .height = h, .pitch = pitch,
		.bpp = bpp, .depth = depth, .handle = handle,
	};
	if (kms_ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f) < 0)
		return -1;
	*fb_id = f.fb_id;
	return 0;
}

static int kms_rm_fb(int fd, uint32_t fb_id)
{
	return kms_ioctl(fd, DRM_IOCTL_MODE_RMFB, &fb_id);
}

static int kms_page_flip(int fd, uint32_t crtc, uint32_t fb, uint32_t flags,
			 void *user_data)
{
	struct drm_mode_crtc_page_flip f = {
		.crtc_id = crtc, .fb_id = fb, .flags = flags,
		.user_data = KMS_PTR(user_data),
	};
	return kms_ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &f);
}

static int kms_dirty_fb(int fd, uint32_t fb, struct drm_clip_rect *clips,
			uint32_t n)
{
	struct drm_mode_fb_dirty_cmd d = {
		.fb_id = fb, .num_clips = n, .clips_ptr = KMS_PTR(clips),
	};
	return kms_ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &d);
}

/*
 * Read the events queued on fd and call flip(user_data) for each completed
 * page flip.  Returns -1 if the read failed.
 */
static int kms_handle_events(int fd, void (*flip)(void *user_data))
{
	char buf[1024];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n < 0)
		return -1;
	for (ssize_t i = 0; i + (ssize_t)sizeof(struct drm_event) <= n;) {
		struct drm_event e;
		memcpy(&e, buf + i, sizeof(e));
		if (e.length < sizeof(e) || i + e.length > n)
			break;
		if (e.type == DRM_EVENT_FLIP_COMPLETE &&
		    e.length >= sizeof(struct drm_event_vblank)) {
			struct drm_event_vblank v;
			memcpy(&v, buf + i, sizeof(v));
			flip((void *)(uintptr_t)v.user_data);
		}
		i += e.length;
	}
	return 0;
}

/* --- planes and properties --- */

/* the IDs of all planes (with DRM_CLIENT_CAP_UNIVERSAL_PLANES set, the
   primary and cursor planes too); *n is their count */
static uint32_t *kms_get_planes(int fd, uint32_t *n)
{
	uint32_t *ids = NULL;
	for (;;) {
		struct drm_mode_get_plane_res c = {0};
		if (kms_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &c) < 0)
			break;
		uint32_t cnt = c.count_planes;
		free(ids);
		if (!(ids = kms_alloc(cnt, sizeof(uint32_t))))
			break;
		c.count_planes = cnt;
		c.plane_id_ptr = KMS_PTR(ids);
		if (kms_ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &c) < 0)
			break;
		if (c.count_planes > cnt)
			continue;
		*n = c.count_planes;
		return ids;
	}
	free(ids);
	*n = 0;
	return NULL;
}

typedef struct {
	uint32_t plane_id, possible_crtcs, count_formats;
	uint32_t *formats;
} KmsPlane;

static void kms_free_plane(KmsPlane *p)
{
	if (!p)
		return;
	free(p->formats);
	free(p);
}

static KmsPlane *kms_get_plane(int fd, uint32_t id)
{
	KmsPlane *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	for (;;) {
		struct drm_mode_get_plane c = { .plane_id = id };
		if (kms_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &c) < 0)
			break;
		uint32_t cnt = c.count_format_types;
		free(p->formats);
		if (!(p->formats = kms_alloc(cnt, sizeof(uint32_t))))
			break;
		memset(&c, 0, sizeof(c));
		c.plane_id = id;
		c.count_format_types = cnt;
		c.format_type_ptr = KMS_PTR(p->formats);
		if (kms_ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &c) < 0)
			break;
		if (c.count_format_types > cnt)
			continue;
		p->plane_id = c.plane_id;
		p->possible_crtcs = c.possible_crtcs;
		p->count_formats = c.count_format_types;
		return p;
	}
	kms_free_plane(p);
	return NULL;
}

/*
 * The value of the object's property called name, in *value.  Returns -1
 * if it has none.
 */
static int kms_get_prop(int fd, uint32_t obj, uint32_t type,
			const char *name, uint64_t *value)
{
	uint32_t *ids = NULL;
	uint64_t *vals = NULL;
	int ret = -1;
	for (;;) {
		struct drm_mode_obj_get_properties c = {
			.obj_id = obj, .obj_type = type,
		};
		if (kms_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &c) < 0)
			break;
		uint32_t cnt = c.count_props;
		free(ids);
		free(vals);
		ids = kms_alloc(cnt, sizeof(*ids));
		vals = kms_alloc(cnt, sizeof(*vals));
		if (!ids || !vals)
			break;
		c.count_props = cnt;
		c.props_ptr = KMS_PTR(ids);
		c.prop_values_ptr = KMS_PTR(vals);
		if (kms_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &c) < 0)
			break;
		if (c.count_props > cnt)
			continue;
		for (uint32_t i = 0; i < c.count_props; i++) {
			/* no value or enum buffers: just the name */
			struct drm_mode_get_property p = { .prop_id = ids[i] };
			if (kms_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &p) == 0 &&
			    strncmp(p.name, name, DRM_PROP_NAME_LEN) == 0) {
				*value = vals[i];
				ret = 0;
				break;
			}
		}
		break;
	}
	free(ids);
	free(vals);
	return ret;
}

#endif /* KMS_H */
