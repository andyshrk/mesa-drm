/*
 * Weston-style property management for wbtest
 *
 * Property IDs are cached once at init, then used for efficient
 * drmModeAtomicAddProperty() calls per test case.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include "xf86drm.h"
#include "xf86drmMode.h"

#include "wb_props.h"

/* Static property name tables (templates for population) */

static const struct wb_property_info plane_prop_table[WB_PLANE_PROP_COUNT] = {
	[WB_PLANE_FB_ID]   = { .name = "FB_ID" },
	[WB_PLANE_CRTC_ID] = { .name = "CRTC_ID" },
	[WB_PLANE_SRC_X]   = { .name = "SRC_X" },
	[WB_PLANE_SRC_Y]   = { .name = "SRC_Y" },
	[WB_PLANE_SRC_W]   = { .name = "SRC_W" },
	[WB_PLANE_SRC_H]   = { .name = "SRC_H" },
	[WB_PLANE_CRTC_X]  = { .name = "CRTC_X" },
	[WB_PLANE_CRTC_Y]  = { .name = "CRTC_Y" },
	[WB_PLANE_CRTC_W]  = { .name = "CRTC_W" },
	[WB_PLANE_CRTC_H]  = { .name = "CRTC_H" },
	[WB_PLANE_ZPOS]    = { .name = "zpos" },
};

static const struct wb_property_info crtc_prop_table[WB_CRTC_PROP_COUNT] = {
	[WB_CRTC_MODE_ID] = { .name = "MODE_ID" },
	[WB_CRTC_ACTIVE]  = { .name = "ACTIVE" },
};

static const struct wb_property_info connector_prop_table[WB_CONNECTOR_PROP_COUNT] = {
	[WB_CONNECTOR_CRTC_ID]               = { .name = "CRTC_ID" },
	[WB_CONNECTOR_WRITEBACK_FB_ID]        = { .name = "WRITEBACK_FB_ID" },
	[WB_CONNECTOR_WRITEBACK_OUT_FENCE_PTR] = { .name = "WRITEBACK_OUT_FENCE_PTR" },
};

/**
 * wb_property_info_populate - Cache KMS property IDs for one object
 *
 * Iterates through KMS properties on the object, matches by name against
 * the static template, and stores the property ID in the destination array.
 * Properties not found on the hardware remain with prop_id == 0.
 */
void wb_property_info_populate(int fd, uint32_t obj_id, uint32_t obj_type,
			       const struct wb_property_info *src,
			       struct wb_property_info *dst,
			       unsigned int count)
{
	drmModeObjectPropertiesPtr props;
	unsigned int i, j;

	/* Initialize from template */
	for (i = 0; i < count; i++) {
		dst[i].name = src[i].name;
		dst[i].prop_id = 0;
	}

	props = drmModeObjectGetProperties(fd, obj_id, obj_type);
	if (!props) {
		fprintf(stderr, "Failed to get properties for object %u\n", obj_id);
		return;
	}

	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);
		if (!prop)
			continue;

		/* Match by name against our template */
		for (j = 0; j < count; j++) {
			if (dst[j].name && strcmp(prop->name, dst[j].name) == 0) {
				dst[j].prop_id = props->props[i];
				break;
			}
		}

		drmModeFreeProperty(prop);
	}

	drmModeFreeObjectProperties(props);
}

/**
 * wb_plane_props_populate - Populate all properties for a plane
 */
void wb_plane_props_populate(int fd, struct wb_plane_props *plane)
{
	wb_property_info_populate(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE,
				  plane_prop_table, plane->props,
				  WB_PLANE_PROP_COUNT);
}

/**
 * wb_crtc_props_populate - Populate all properties for a CRTC
 */
void wb_crtc_props_populate(int fd, struct wb_crtc_props *crtc)
{
	wb_property_info_populate(fd, crtc->crtc_id, DRM_MODE_OBJECT_CRTC,
				  crtc_prop_table, crtc->props,
				  WB_CRTC_PROP_COUNT);
}

/**
 * wb_connector_props_populate - Populate all properties for a connector
 */
void wb_connector_props_populate(int fd, struct wb_connector_props *conn)
{
	wb_property_info_populate(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR,
				  connector_prop_table, conn->props,
				  WB_CONNECTOR_PROP_COUNT);
}

/**
 * wb_plane_add_prop - Add a plane property to an atomic request
 *
 * Returns 0 on success, -1 if the property is missing or add fails.
 */
int wb_plane_prop_add(drmModeAtomicReqPtr req, struct wb_plane_props *p,
		      enum wb_plane_property prop, uint64_t val)
{
	struct wb_property_info *info = &p->props[prop];
	int ret;

	if (info->prop_id == 0) {
		fprintf(stderr, "Plane %u: property '%s' not found\n",
			p->plane_id, info->name);
		return -1;
	}

	ret = drmModeAtomicAddProperty(req, p->plane_id, info->prop_id, val);
	if (ret <= 0) {
		fprintf(stderr, "Plane %u: failed to set '%s' to %"PRIu64": %s\n",
			p->plane_id, info->name, val, strerror(-ret));
		return -1;
	}

	return 0;
}

/**
 * wb_crtc_add_prop - Add a CRTC property to an atomic request
 */
int wb_crtc_prop_add(drmModeAtomicReqPtr req, struct wb_crtc_props *c,
		     enum wb_crtc_property prop, uint64_t val)
{
	struct wb_property_info *info = &c->props[prop];
	int ret;

	if (info->prop_id == 0) {
		fprintf(stderr, "CRTC %u: property '%s' not found\n",
			c->crtc_id, info->name);
		return -1;
	}

	ret = drmModeAtomicAddProperty(req, c->crtc_id, info->prop_id, val);
	if (ret <= 0) {
		fprintf(stderr, "CRTC %u: failed to set '%s' to %"PRIu64": %s\n",
			c->crtc_id, info->name, val, strerror(-ret));
		return -1;
	}

	return 0;
}

/**
 * wb_connector_add_prop - Add a connector property to an atomic request
 */
int wb_connector_prop_add(drmModeAtomicReqPtr req, struct wb_connector_props *c,
			  enum wb_connector_property prop, uint64_t val)
{
	struct wb_property_info *info = &c->props[prop];
	int ret;

	if (info->prop_id == 0) {
		fprintf(stderr, "Connector %u: property '%s' not found\n",
			c->connector_id, info->name);
		return -1;
	}

	ret = drmModeAtomicAddProperty(req, c->connector_id, info->prop_id, val);
	if (ret <= 0) {
		fprintf(stderr, "Connector %u: failed to set '%s' to %"PRIu64": %s\n",
			c->connector_id, info->name, val, strerror(-ret));
		return -1;
	}

	return 0;
}
