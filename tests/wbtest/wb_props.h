/*
 * Weston-style property management for wbtest
 * Caches KMS property IDs at init time for efficient atomic requests
 */

#ifndef WB_PROPS_H
#define WB_PROPS_H

#include <stdint.h>
#include <xf86drmMode.h>

/* Plane property indices */
enum wb_plane_property {
	WB_PLANE_FB_ID = 0,
	WB_PLANE_CRTC_ID,
	WB_PLANE_SRC_X,
	WB_PLANE_SRC_Y,
	WB_PLANE_SRC_W,
	WB_PLANE_SRC_H,
	WB_PLANE_CRTC_X,
	WB_PLANE_CRTC_Y,
	WB_PLANE_CRTC_W,
	WB_PLANE_CRTC_H,
	WB_PLANE_ZPOS,
	WB_PLANE_PROP_COUNT,
};

/* CRTC property indices */
enum wb_crtc_property {
	WB_CRTC_MODE_ID = 0,
	WB_CRTC_ACTIVE,
	WB_CRTC_PROP_COUNT,
};

/* Connector property indices */
enum wb_connector_property {
	WB_CONNECTOR_CRTC_ID = 0,
	WB_CONNECTOR_WRITEBACK_FB_ID,
	WB_CONNECTOR_WRITEBACK_OUT_FENCE_PTR,
	WB_CONNECTOR_PROP_COUNT,
};

/* Lightweight property info: name + cached KMS property ID */
struct wb_property_info {
	const char *name;
	uint32_t prop_id;	/* 0 = not found / not supported */
};

/* Per-object property caches */
struct wb_plane_props {
	uint32_t plane_id;
	struct wb_property_info props[WB_PLANE_PROP_COUNT];
};

struct wb_crtc_props {
	uint32_t crtc_id;
	struct wb_property_info props[WB_CRTC_PROP_COUNT];
};

struct wb_connector_props {
	uint32_t connector_id;
	struct wb_property_info props[WB_CONNECTOR_PROP_COUNT];
};

/* Populate property cache from KMS (call once per object at init) */
void wb_property_info_populate(int fd, uint32_t obj_id, uint32_t obj_type,
			       const struct wb_property_info *src,
			       struct wb_property_info *dst,
			       unsigned int count);

/* Typed populate helpers */
void wb_plane_props_populate(int fd, struct wb_plane_props *plane);
void wb_crtc_props_populate(int fd, struct wb_crtc_props *crtc);
void wb_connector_props_populate(int fd, struct wb_connector_props *conn);

/* Typed add_prop helpers for atomic requests */
int wb_plane_prop_add(drmModeAtomicReqPtr req, struct wb_plane_props *p,
		      enum wb_plane_property prop, uint64_t val);
int wb_crtc_prop_add(drmModeAtomicReqPtr req, struct wb_crtc_props *c,
		     enum wb_crtc_property prop, uint64_t val);
int wb_connector_prop_add(drmModeAtomicReqPtr req, struct wb_connector_props *c,
			  enum wb_connector_property prop, uint64_t val);

#endif /* WB_PROPS_H */
