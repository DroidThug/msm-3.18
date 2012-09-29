/* exynos_drm_drv.h
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 * Authors:
 *	Inki Dae <inki.dae@samsung.com>
 *	Joonyoung Shim <jy0922.shim@samsung.com>
 *	Seung-Woo Kim <sw0312.kim@samsung.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef _EXYNOS_DRM_DRV_H_
#define _EXYNOS_DRM_DRV_H_

#include <linux/module.h>
#include "drmP.h"
#include "drm.h"
#include "drm_crtc.h"

#ifdef CONFIG_EXYNOS_IOMMU
#include <mach/sysmmu.h>
#include <linux/of_platform.h>
#endif

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
#include "linux/kds.h"
#include <linux/dma-buf.h>
#endif

#define MAX_CRTC	3
#define MAX_PLANE	5
#define MAX_FB_BUFFER	4
#define DEFAULT_ZPOS	-1

struct drm_device;
struct exynos_drm_overlay;
struct drm_connector;
struct exynos_drm_gem_object;

extern unsigned int drm_vblank_offdelay;

/* this enumerates display type. */
enum exynos_drm_output_type {
	EXYNOS_DISPLAY_TYPE_NONE,
	/* RGB or CPU Interface. */
	EXYNOS_DISPLAY_TYPE_LCD,
	/* HDMI Interface. */
	EXYNOS_DISPLAY_TYPE_HDMI,
	/* Virtual Display Interface. */
	EXYNOS_DISPLAY_TYPE_VIDI,
};

/*
 * Exynos drm overlay ops structure.
 *
 * @mode_set: copy drm overlay info to hw specific overlay info.
 * @commit: apply hardware specific overlay data to registers.
 * @disable: disable hardware specific overlay.
 */
struct exynos_drm_overlay_ops {
	void (*mode_set)(struct device *subdrv_dev,
			 struct exynos_drm_overlay *overlay);
	void (*page_flip)(struct device *subdrv_dev,
			 struct exynos_drm_overlay *overlay);
	void (*commit)(struct device *subdrv_dev, int zpos);
	void (*disable)(struct device *subdrv_dev, int zpos);
};

/*
 * Exynos drm common overlay structure.
 *
 * @fb_x: offset x on a framebuffer to be displayed.
 *	- the unit is screen coordinates.
 * @fb_y: offset y on a framebuffer to be displayed.
 *	- the unit is screen coordinates.
 * @fb_width: width of a framebuffer.
 * @fb_height: height of a framebuffer.
 * @fb_pitch: pitch of a framebuffer.
 * @crtc_x: offset x on hardware screen.
 * @crtc_y: offset y on hardware screen.
 * @crtc_width: window width to be displayed (hardware screen).
 * @crtc_height: window height to be displayed (hardware screen).
 * @mode_width: width of screen mode.
 * @mode_height: height of screen mode.
 * @refresh: refresh rate.
 * @scan_flag: interlace or progressive way.
 *	(it could be DRM_MODE_FLAG_*)
 * @bpp: pixel size.(in bit)
 * @pixel_format: fourcc pixel format of this overlay
 * @dma_addr: array of bus(accessed by dma) address to the memory region
 *	      allocated for a overlay.
 * @vaddr: array of virtual memory addresss to this overlay.
 * @zpos: order of overlay layer(z position).
 * @default_win: a window to be enabled.
 * @color_key: color key on or off.
 * @index_color: if using color key feature then this value would be used
 *			as index color.
 * @local_path: in case of lcd type, local path mode on or off.
 * @transparency: transparency on or off.
 * @activated: activated or not.
 *
 * this structure is common to exynos SoC and its contents would be copied
 * to hardware specific overlay info.
 */
struct exynos_drm_overlay {
	unsigned int fb_x;
	unsigned int fb_y;
	unsigned int fb_width;
	unsigned int fb_height;
	unsigned int fb_pitch;
	unsigned int crtc_x;
	unsigned int crtc_y;
	unsigned int crtc_width;
	unsigned int crtc_height;
	unsigned int mode_width;
	unsigned int mode_height;
	unsigned int refresh;
	unsigned int scan_flag;
	unsigned int bpp;
	uint32_t pixel_format;
	dma_addr_t dma_addr[MAX_FB_BUFFER];
	void __iomem *vaddr[MAX_FB_BUFFER];
	int zpos;

	bool default_win;
	bool color_key;
	unsigned int index_color;
	bool local_path;
	bool transparency;
	bool activated;
};

/*
 * Exynos DRM Display Structure.
 *	- this structure is common to analog tv, digital tv and lcd panel.
 *
 * @type: one of EXYNOS_DISPLAY_TYPE_LCD and HDMI.
 * @is_connected: check for that display is connected or not.
 * @get_edid: get edid modes from display driver.
 * @get_panel: get panel object from display driver.
 * @check_timing: check if timing is valid or not.
 * @power_on: display device on or off.
 */
struct exynos_drm_display_ops {
	enum exynos_drm_output_type type;
	bool (*is_connected)(struct device *dev);
	int (*get_edid)(struct device *dev, struct drm_connector *connector,
				u8 *edid, int len);
	void *(*get_panel)(struct device *dev);
	int (*check_timing)(struct device *dev, void *timing);
	int (*power_on)(struct device *dev, int mode);
};

/*
 * Exynos drm manager ops
 *
 * @dpms: control device power.
 * @apply: set timing, vblank and overlay data to registers.
 * @mode_fixup: fix mode data comparing to hw specific display mode.
 * @mode_set: convert drm_display_mode to hw specific display mode and
 *	      would be called by encoder->mode_set().
 * @get_max_resol: get maximum resolution to specific hardware.
 * @commit: set current hw specific display mode to hw.
 * @enable_vblank: specific driver callback for enabling vblank interrupt.
 * @disable_vblank: specific driver callback for disabling vblank interrupt.
 */
struct exynos_drm_manager_ops {
	void (*dpms)(struct device *subdrv_dev, int mode);
	void (*apply)(struct device *subdrv_dev);
	void (*mode_fixup)(struct device *subdrv_dev,
				struct drm_connector *connector,
				struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode);
	void (*mode_set)(struct device *subdrv_dev, void *mode);
	void (*get_max_resol)(struct device *subdrv_dev, unsigned int *width,
				unsigned int *height);
	void (*commit)(struct device *subdrv_dev);
	int (*enable_vblank)(struct device *subdrv_dev);
	void (*disable_vblank)(struct device *subdrv_dev);
};

/*
 * Exynos drm common manager structure.
 *
 * @dev: pointer to device object for subdrv device driver.
 *	sub drivers such as display controller or hdmi driver,
 *	have their own device object.
 * @ops: pointer to callbacks for exynos drm specific framebuffer.
 *	these callbacks should be set by specific drivers such fimd
 *	or hdmi driver and are used to control hardware global registers.
 * @overlay_ops: pointer to callbacks for exynos drm specific framebuffer.
 *	these callbacks should be set by specific drivers such fimd
 *	or hdmi driver and are used to control hardware overlay reigsters.
 * @display: pointer to callbacks for exynos drm specific framebuffer.
 *	these callbacks should be set by specific drivers such fimd
 *	or hdmi driver and are used to control display devices such as
 *	analog tv, digital tv and lcd panel and also get timing data for them.
 */
struct exynos_drm_manager {
	struct device *dev;
	int pipe;
	struct exynos_drm_manager_ops *ops;
	struct exynos_drm_overlay_ops *overlay_ops;
	struct exynos_drm_display_ops *display_ops;
};

/*
 * Exynos drm private structure.
 */
struct exynos_drm_private {
	struct drm_fb_helper *fb_helper;

	/*
	 * wait_vsync_event is set to zero by crtc whenever a VSYNC interrupt
	 * is received. After setting wait_vsync_event to 0, wait_vsync_queue
	 * is woken up.
	 */
	wait_queue_head_t wait_vsync_queue;
	atomic_t wait_vsync_event;

	/*
	 * created crtc object would be contained at this array and
	 * this array is used to be aware of which crtc did it request vblank.
	 */
	struct drm_crtc *crtc[MAX_CRTC];

#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	struct kds_callback kds_cb;
#endif
};

/*
 * Exynos drm sub driver structure.
 *
 * @list: sub driver has its own list object to register to exynos drm driver.
 * @dev: pointer to device object for subdrv device driver.
 * @drm_dev: pointer to drm_device and this pointer would be set
 *	when sub driver calls exynos_drm_subdrv_register().
 * @manager: subdrv has its own manager to control a hardware appropriately
 *	and we can access a hardware drawing on this manager.
 * @probe: this callback would be called by exynos drm driver after
 *	subdrv is registered to it.
 * @remove: this callback is used to release resources created
 *	by probe callback.
 * @open: this would be called with drm device file open.
 * @close: this would be called with drm device file close.
 * @encoder: encoder object owned by this sub driver.
 * @connector: connector object owned by this sub driver.
 */
struct exynos_drm_subdrv {
	struct list_head list;
	struct device *dev;
	struct drm_device *drm_dev;
	struct exynos_drm_manager *manager;

	int (*probe)(struct drm_device *drm_dev, struct device *dev);
	void (*remove)(struct drm_device *dev);
	int (*open)(struct drm_device *drm_dev, struct device *dev,
			struct drm_file *file);
	void (*close)(struct drm_device *drm_dev, struct device *dev,
			struct drm_file *file);

	struct drm_encoder *encoder;
	struct drm_connector *connector;
};

/*
 * this function calls a probe callback registered to sub driver list and
 * create its own encoder and connector and then set drm_device object
 * to global one.
 */
int exynos_drm_device_register(struct drm_device *dev);
/*
 * this function calls a remove callback registered to sub driver list and
 * destroy its own encoder and connetor.
 */
int exynos_drm_device_unregister(struct drm_device *dev);

/*
 * this function would be called by sub drivers such as display controller
 * or hdmi driver to register this sub driver object to exynos drm driver
 * and when a sub driver is registered to exynos drm driver a probe callback
 * of the sub driver is called and creates its own encoder and connector.
 */
int exynos_drm_subdrv_register(struct exynos_drm_subdrv *drm_subdrv);

/* this function removes subdrv list from exynos drm driver */
int exynos_drm_subdrv_unregister(struct exynos_drm_subdrv *drm_subdrv);

void exynos_fimd_dp_attach(struct device *dev);

int exynos_drm_subdrv_open(struct drm_device *dev, struct drm_file *file);
void exynos_drm_subdrv_close(struct drm_device *dev, struct drm_file *file);

/*
 * exynos specific framebuffer structure.
 *
 * @fb: drm framebuffer obejct.
 * @exynos_gem_obj: array of exynos specific gem object containing a gem object.
 */
struct exynos_drm_fb {
	struct drm_framebuffer		fb;
	struct exynos_drm_gem_obj	*exynos_gem_obj[MAX_FB_BUFFER];
#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	struct kds_resource_set		*kds_res_set;
	struct dma_buf			*dma_buf;
#endif
};

/*
 * Exynos specific crtc structure.
 *
 * @drm_crtc: crtc object.
 * @overlay: contain information common to display controller and hdmi and
 *	contents of this overlay object would be copied to sub driver size.
 * @event: vblank event that is currently queued for flip
 * @pipe: a crtc index created at load() with a new crtc object creation
 *	and the crtc object would be set to private->crtc array
 *	to get a crtc object corresponding to this pipe from private->crtc
 *	array when irq interrupt occured. the reason of using this pipe is that
 *	drm framework doesn't support multiple irq yet.
 *	we can refer to the crtc to current hardware interrupt occured through
 *	this pipe value.
 * @dpms: store the crtc dpms value
 * @flip_pending: there is a flip pending that we need to process next vblank
 */
struct exynos_drm_crtc {
	struct drm_crtc			drm_crtc;
	struct exynos_drm_overlay	overlay;
#ifdef CONFIG_DMA_SHARED_BUFFER_USES_KDS
	struct drm_pending_vblank_event *event;
#endif
	unsigned int			pipe;
	unsigned int			dpms;
	atomic_t			flip_pending;
};

#define to_exynos_fb(x)	container_of(x, struct exynos_drm_fb, fb)
#define to_exynos_crtc(x)	container_of(x, struct exynos_drm_crtc,\
				drm_crtc)

extern struct platform_driver fimd_driver;
extern struct platform_driver hdmi_driver;
extern struct platform_driver dp_driver;
extern struct platform_driver mixer_driver;
extern struct platform_driver exynos_drm_common_hdmi_driver;
extern struct platform_driver vidi_driver;
#ifdef CONFIG_EXYNOS_IOMMU
extern struct dma_iommu_mapping *exynos_drm_common_mapping;
#endif
#endif
