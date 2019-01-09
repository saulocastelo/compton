// SPDX-License-Identifier: MPL-2.0
// Copyright (c) Yuxuan Shui <yshuiv7@gmail.com>

#include <xcb/xcb_image.h>

#include "common.h"
#include "options.h"

#ifdef CONFIG_OPENGL
#include "opengl.h"
#endif

#include "vsync.h"
#include "win.h"

#include "backend/backend_common.h"
#include "render.h"

#ifdef CONFIG_OPENGL
/**
 * Bind texture in paint_t if we are using GLX backend.
 */
static inline bool paint_bind_tex(session_t *ps, paint_t *ppaint, unsigned wid,
                                  unsigned hei, unsigned depth, bool force) {
	if (!ppaint->pixmap)
		return false;

	if (force || !glx_tex_binded(ppaint->ptex, ppaint->pixmap))
		return glx_bind_pixmap(ps, &ppaint->ptex, ppaint->pixmap, wid, hei, depth);

	return true;
}
#else
static inline bool paint_bind_tex(session_t *ps, paint_t *ppaint, unsigned wid,
                                  unsigned hei, unsigned depth, bool force) {
	return true;
}
#endif

/**
 * Check if current backend uses XRender for rendering.
 */
static inline bool bkend_use_xrender(session_t *ps) {
	return BKEND_XRENDER == ps->o.backend || BKEND_XR_GLX_HYBRID == ps->o.backend;
}

/**
 * Reset filter on a <code>Picture</code>.
 */
static inline void xrfilter_reset(session_t *ps, xcb_render_picture_t p) {
#define FILTER "Nearest"
	xcb_render_set_picture_filter(ps->c, p, strlen(FILTER), FILTER, 0, NULL);
#undef FILTER
}

static inline void attr_nonnull(1, 2) set_tgt_clip(session_t *ps, region_t *reg) {
	switch (ps->o.backend) {
	case BKEND_XRENDER:
	case BKEND_XR_GLX_HYBRID:
		x_set_picture_clip_region(ps, ps->tgt_buffer.pict, 0, 0, reg);
		break;
#ifdef CONFIG_OPENGL
	case BKEND_GLX: glx_set_clip(ps, reg); break;
#endif
	default: assert(false);
	}
}

/**
 * Destroy a <code>Picture</code>.
 */
void free_picture(xcb_connection_t *c, xcb_render_picture_t *p) {
	if (*p) {
		xcb_render_free_picture(c, *p);
		*p = XCB_NONE;
	}
}

/**
 * Free paint_t.
 */
void free_paint(session_t *ps, paint_t *ppaint) {
#ifdef CONFIG_OPENGL
	free_paint_glx(ps, ppaint);
#endif
	free_picture(ps->c, &ppaint->pict);
	if (ppaint->pixmap)
		xcb_free_pixmap(ps->c, ppaint->pixmap);
	ppaint->pixmap = XCB_NONE;
}

void render(session_t *ps, int x, int y, int dx, int dy, int wid, int hei, double opacity,
            bool argb, bool neg, xcb_render_picture_t pict, glx_texture_t *ptex,
            const region_t *reg_paint, const glx_prog_main_t *pprogram) {
	switch (ps->o.backend) {
	case BKEND_XRENDER:
	case BKEND_XR_GLX_HYBRID: {
		int alpha_step = opacity * MAX_ALPHA;
		xcb_render_picture_t alpha_pict = ps->alpha_picts[alpha_step];
		if (alpha_step != 0) {
			int op = ((!argb && !alpha_pict) ? XCB_RENDER_PICT_OP_SRC
			                                 : XCB_RENDER_PICT_OP_OVER);
			xcb_render_composite(ps->c, op, pict, alpha_pict, ps->tgt_buffer.pict,
			                     x, y, 0, 0, dx, dy, wid, hei);
		}
		break;
	}
#ifdef CONFIG_OPENGL
	case BKEND_GLX:
		glx_render(ps, ptex, x, y, dx, dy, wid, hei, ps->psglx->z, opacity, argb,
		           neg, reg_paint, pprogram);
		ps->psglx->z += 1;
		break;
#endif
	default: assert(0);
	}
}

static inline void
paint_region(session_t *ps, win *w, int x, int y, int wid, int hei, double opacity,
             const region_t *reg_paint, xcb_render_picture_t pict) {
	const int dx = (w ? w->g.x : 0) + x;
	const int dy = (w ? w->g.y : 0) + y;
	const bool argb = (w && (win_has_alpha(w) || ps->o.force_win_blend));
	const bool neg = (w && w->invert_color);

	render(ps, x, y, dx, dy, wid, hei, opacity, argb, neg, pict,
	       (w ? w->paint.ptex : ps->root_tile_paint.ptex), reg_paint,
#ifdef CONFIG_OPENGL
	       w ? &ps->glx_prog_win : NULL
#else
	       NULL
#endif
	);
}

/**
 * Check whether a paint_t contains enough data.
 */
static inline bool paint_isvalid(session_t *ps, const paint_t *ppaint) {
	// Don't check for presence of Pixmap here, because older X Composite doesn't
	// provide it
	if (!ppaint)
		return false;

	if (bkend_use_xrender(ps) && !ppaint->pict)
		return false;

#ifdef CONFIG_OPENGL
	if (BKEND_GLX == ps->o.backend && !glx_tex_binded(ppaint->ptex, XCB_NONE))
		return false;
#endif

	return true;
}

/**
 * Paint a window itself and dim it if asked.
 */
void paint_one(session_t *ps, win *w, const region_t *reg_paint) {
	glx_mark(ps, w->id, true);

	// Fetch Pixmap
	if (!w->paint.pixmap && ps->has_name_pixmap) {
		w->paint.pixmap = xcb_generate_id(ps->c);
		set_ignore_cookie(
		    ps, xcb_composite_name_window_pixmap(ps->c, w->id, w->paint.pixmap));
	}

	xcb_drawable_t draw = w->paint.pixmap;
	if (!draw)
		draw = w->id;

	// XRender: Build picture
	if (bkend_use_xrender(ps) && !w->paint.pict) {
		xcb_render_create_picture_value_list_t pa = {
		    .subwindowmode = IncludeInferiors,
		};

		w->paint.pict = x_create_picture_with_pictfmt_and_pixmap(
		    ps, w->pictfmt, draw, XCB_RENDER_CP_SUBWINDOW_MODE, &pa);
	}

	// GLX: Build texture
	// Let glx_bind_pixmap() determine pixmap size, because if the user
	// is resizing windows, the width and height we get may not be up-to-date,
	// causing the jittering issue M4he reported in #7.
	if (!paint_bind_tex(ps, &w->paint, 0, 0, 0,
	                    (!ps->o.glx_no_rebind_pixmap && w->pixmap_damaged))) {
		log_error("Failed to bind texture for window %#010x.", w->id);
	}
	w->pixmap_damaged = false;

	if (!paint_isvalid(ps, &w->paint)) {
		log_error("Window %#010x is missing painting data.", w->id);
		return;
	}

	const int x = w->g.x;
	const int y = w->g.y;
	const int wid = w->widthb;
	const int hei = w->heightb;

	xcb_render_picture_t pict = w->paint.pict;

	// Invert window color, if required
	if (bkend_use_xrender(ps) && w->invert_color) {
		xcb_render_picture_t newpict =
		    x_create_picture_with_pictfmt(ps, wid, hei, w->pictfmt, 0, NULL);
		if (newpict) {
			// Apply clipping region to save some CPU
			if (reg_paint) {
				region_t reg;
				pixman_region32_init(&reg);
				pixman_region32_copy(&reg, (region_t *)reg_paint);
				pixman_region32_translate(&reg, -x, -y);
				// FIXME XFixesSetPictureClipRegion(ps->dpy, newpict, 0, 0, reg);
				pixman_region32_fini(&reg);
			}

			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, pict, XCB_NONE,
			                     newpict, 0, 0, 0, 0, 0, 0, wid, hei);
			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_DIFFERENCE,
			                     ps->white_picture, XCB_NONE, newpict, 0, 0,
			                     0, 0, 0, 0, wid, hei);
			// We use an extra PictOpInReverse operation to get correct
			// pixel alpha. There could be a better solution.
			if (win_has_alpha(w))
				xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_IN_REVERSE,
				                     pict, XCB_NONE, newpict, 0, 0, 0, 0,
				                     0, 0, wid, hei);
			pict = newpict;
		}
	}

	const double dopacity = get_opacity_percent(w);

	if (w->frame_opacity == 1) {
		paint_region(ps, w, 0, 0, wid, hei, dopacity, reg_paint, pict);
	} else {
		// Painting parameters
		const margin_t extents = win_calc_frame_extents(w);
		const int t = extents.top;
		const int l = extents.left;
		const int b = extents.bottom;
		const int r = extents.right;

#define COMP_BDR(cx, cy, cwid, chei)                                                     \
	paint_region(ps, w, (cx), (cy), (cwid), (chei), w->frame_opacity *dopacity,      \
	             reg_paint, pict)

		// Sanitize the margins, in case some broken WM makes
		// top_width + bottom_width > height in some cases.

		do {
			// top
			int body_height = hei;
			// ctop = checked top
			// Make sure top margin is smaller than height
			int ctop = min_i(body_height, t);
			if (ctop > 0)
				COMP_BDR(0, 0, wid, ctop);

			body_height -= ctop;
			if (body_height <= 0)
				break;

			// bottom
			// cbot = checked bottom
			// Make sure bottom margin is not too large
			int cbot = min_i(body_height, b);
			if (cbot > 0)
				COMP_BDR(0, hei - cbot, wid, cbot);

			// Height of window exclude the margin
			body_height -= cbot;
			if (body_height <= 0)
				break;

			// left
			int body_width = wid;
			int cleft = min_i(body_width, l);
			if (cleft > 0)
				COMP_BDR(0, ctop, cleft, body_height);

			body_width -= cleft;
			if (body_width <= 0)
				break;

			// right
			int cright = min_i(body_width, r);
			if (cright > 0)
				COMP_BDR(wid - cright, ctop, cright, body_height);

			body_width -= cright;
			if (body_width <= 0)
				break;

			// body
			paint_region(ps, w, cleft, ctop, body_width, body_height,
			             dopacity, reg_paint, pict);
		} while (0);
	}

#undef COMP_BDR

	if (pict != w->paint.pict)
		free_picture(ps->c, &pict);

	// Dimming the window if needed
	if (w->dim) {
		double dim_opacity = ps->o.inactive_dim;
		if (!ps->o.inactive_dim_fixed)
			dim_opacity *= get_opacity_percent(w);

		switch (ps->o.backend) {
		case BKEND_XRENDER:
		case BKEND_XR_GLX_HYBRID: {
			unsigned short cval = 0xffff * dim_opacity;

			// Premultiply color
			xcb_render_color_t color = {
			    .red = 0,
			    .green = 0,
			    .blue = 0,
			    .alpha = cval,
			};

			xcb_rectangle_t rect = {
			    .x = x,
			    .y = y,
			    .width = wid,
			    .height = hei,
			};

			xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_OVER,
			                           ps->tgt_buffer.pict, color, 1, &rect);
		} break;
#ifdef CONFIG_OPENGL
		case BKEND_GLX:
			glx_dim_dst(ps, x, y, wid, hei, ps->psglx->z - 0.7, dim_opacity,
			            reg_paint);
			break;
#endif
		default: assert(false);
		}
	}

	glx_mark(ps, w->id, false);
}

extern const char *background_props_str[];

static bool get_root_tile(session_t *ps) {
	/*
	if (ps->o.paint_on_overlay) {
	  return ps->root_picture;
	} */

	assert(!ps->root_tile_paint.pixmap);
	ps->root_tile_fill = false;

	bool fill = false;
	xcb_pixmap_t pixmap = XCB_NONE;

	// Get the values of background attributes
	for (int p = 0; background_props_str[p]; p++) {
		winprop_t prop =
		    wid_get_prop(ps, ps->root, get_atom(ps, background_props_str[p]), 1L,
		                 XCB_ATOM_PIXMAP, 32);
		if (prop.nitems) {
			pixmap = *prop.p32;
			fill = false;
			free_winprop(&prop);
			break;
		}
		free_winprop(&prop);
	}

	// Make sure the pixmap we got is valid
	if (pixmap && !x_validate_pixmap(ps, pixmap))
		pixmap = XCB_NONE;

	// Create a pixmap if there isn't any
	if (!pixmap) {
		pixmap = x_create_pixmap(ps, ps->depth, ps->root, 1, 1);
		if (pixmap == XCB_NONE) {
			log_error("Failed to create pixmaps for root tile.");
			return false;
		}
		fill = true;
	}

	// Create Picture
	xcb_render_create_picture_value_list_t pa = {
	    .repeat = True,
	};
	ps->root_tile_paint.pict = x_create_picture_with_visual_and_pixmap(
	    ps, ps->vis, pixmap, XCB_RENDER_CP_REPEAT, &pa);

	// Fill pixmap if needed
	if (fill) {
		xcb_render_color_t col;
		xcb_rectangle_t rect;

		col.red = col.green = col.blue = 0x8080;
		col.alpha = 0xffff;

		rect.x = rect.y = 0;
		rect.width = rect.height = 1;

		xcb_render_fill_rectangles(ps->c, XCB_RENDER_PICT_OP_SRC,
		                           ps->root_tile_paint.pict, col, 1, &rect);
	}

	ps->root_tile_fill = fill;
	ps->root_tile_paint.pixmap = pixmap;
#ifdef CONFIG_OPENGL
	if (BKEND_GLX == ps->o.backend)
		return glx_bind_pixmap(ps, &ps->root_tile_paint.ptex,
		                       ps->root_tile_paint.pixmap, 0, 0, 0);
#endif

	return true;
}

/**
 * Paint root window content.
 */
static void paint_root(session_t *ps, const region_t *reg_paint) {
	// If there is no root tile pixmap, try getting one.
	// If that fails, give up.
	if (!ps->root_tile_paint.pixmap && !get_root_tile(ps))
		return;

	paint_region(ps, NULL, 0, 0, ps->root_width, ps->root_height, 1.0, reg_paint,
	             ps->root_tile_paint.pict);
}

/**
 * Generate shadow <code>Picture</code> for a window.
 */
static bool win_build_shadow(session_t *ps, win *w, double opacity) {
	const int width = w->widthb;
	const int height = w->heightb;
	// log_trace("(): building shadow for %s %d %d", w->name, width, height);

	xcb_image_t *shadow_image = NULL;
	xcb_pixmap_t shadow_pixmap = XCB_NONE, shadow_pixmap_argb = XCB_NONE;
	xcb_render_picture_t shadow_picture = XCB_NONE, shadow_picture_argb = XCB_NONE;
	xcb_gcontext_t gc = XCB_NONE;

	shadow_image =
	    make_shadow(ps->c, ps->gaussian_map, opacity, width, height);
	if (!shadow_image) {
		log_error("failed to make shadow");
		return XCB_NONE;
	}

	shadow_pixmap =
	    x_create_pixmap(ps, 8, ps->root, shadow_image->width, shadow_image->height);
	shadow_pixmap_argb =
	    x_create_pixmap(ps, 32, ps->root, shadow_image->width, shadow_image->height);

	if (!shadow_pixmap || !shadow_pixmap_argb) {
		log_error("failed to create shadow pixmaps");
		goto shadow_picture_err;
	}

	shadow_picture = x_create_picture_with_standard_and_pixmap(
	    ps, XCB_PICT_STANDARD_A_8, shadow_pixmap, 0, NULL);
	shadow_picture_argb = x_create_picture_with_standard_and_pixmap(
	    ps, XCB_PICT_STANDARD_ARGB_32, shadow_pixmap_argb, 0, NULL);
	if (!shadow_picture || !shadow_picture_argb)
		goto shadow_picture_err;

	gc = xcb_generate_id(ps->c);
	xcb_create_gc(ps->c, gc, shadow_pixmap, 0, NULL);

	xcb_image_put(ps->c, shadow_pixmap, gc, shadow_image, 0, 0, 0);
	xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, ps->cshadow_picture,
	                     shadow_picture, shadow_picture_argb, 0, 0, 0, 0, 0, 0,
	                     shadow_image->width, shadow_image->height);

	assert(!w->shadow_paint.pixmap);
	w->shadow_paint.pixmap = shadow_pixmap_argb;
	assert(!w->shadow_paint.pict);
	w->shadow_paint.pict = shadow_picture_argb;

	xcb_free_gc(ps->c, gc);
	xcb_image_destroy(shadow_image);
	xcb_free_pixmap(ps->c, shadow_pixmap);
	xcb_render_free_picture(ps->c, shadow_picture);

	return true;

shadow_picture_err:
	if (shadow_image)
		xcb_image_destroy(shadow_image);
	if (shadow_pixmap)
		xcb_free_pixmap(ps->c, shadow_pixmap);
	if (shadow_pixmap_argb)
		xcb_free_pixmap(ps->c, shadow_pixmap_argb);
	if (shadow_picture)
		xcb_render_free_picture(ps->c, shadow_picture);
	if (shadow_picture_argb)
		xcb_render_free_picture(ps->c, shadow_picture_argb);
	if (gc)
		xcb_free_gc(ps->c, gc);

	return false;
}

/**
 * Paint the shadow of a window.
 */
static inline void win_paint_shadow(session_t *ps, win *w, region_t *reg_paint) {
	// Bind shadow pixmap to GLX texture if needed
	paint_bind_tex(ps, &w->shadow_paint, 0, 0, 32, false);

	if (!paint_isvalid(ps, &w->shadow_paint)) {
		log_error("Window %#010x is missing shadow data.", w->id);
		return;
	}

	render(ps, 0, 0, w->g.x + w->shadow_dx, w->g.y + w->shadow_dy, w->shadow_width,
	       w->shadow_height, w->shadow_opacity, true, false, w->shadow_paint.pict,
	       w->shadow_paint.ptex, reg_paint, NULL);
}

/**
 * Normalize a convolution kernel.
 */
static inline void normalize_conv_kern(int wid, int hei, xcb_render_fixed_t *kern) {
	double sum = 0.0;
	for (int i = 0; i < wid * hei; ++i)
		sum += XFIXED_TO_DOUBLE(kern[i]);
	double factor = 1.0 / sum;
	for (int i = 0; i < wid * hei; ++i)
		kern[i] = DOUBLE_TO_XFIXED(XFIXED_TO_DOUBLE(kern[i]) * factor);
}

/**
 * @brief Blur an area on a buffer.
 *
 * @param ps current session
 * @param tgt_buffer a buffer as both source and destination
 * @param x x pos
 * @param y y pos
 * @param wid width
 * @param hei height
 * @param blur_kerns blur kernels, ending with a NULL, guaranteed to have at
 *                    least one kernel
 * @param reg_clip a clipping region to be applied on intermediate buffers
 *
 * @return true if successful, false otherwise
 */
static bool
xr_blur_dst(session_t *ps, xcb_render_picture_t tgt_buffer, int x, int y, int wid,
            int hei, xcb_render_fixed_t **blur_kerns, const region_t *reg_clip) {
	assert(blur_kerns[0]);

	// Directly copying from tgt_buffer to it does not work, so we create a
	// Picture in the middle.
	xcb_render_picture_t tmp_picture =
	    x_create_picture_with_pictfmt(ps, wid, hei, NULL, 0, NULL);

	if (!tmp_picture) {
		log_error("Failed to build intermediate Picture.");
		return false;
	}

	if (reg_clip && tmp_picture)
		x_set_picture_clip_region(ps, tmp_picture, 0, 0, reg_clip);

	xcb_render_picture_t src_pict = tgt_buffer, dst_pict = tmp_picture;
	for (int i = 0; blur_kerns[i]; ++i) {
		assert(i < MAX_BLUR_PASS - 1);
		xcb_render_fixed_t *convolution_blur = blur_kerns[i];
		int kwid = XFIXED_TO_DOUBLE(convolution_blur[0]),
		    khei = XFIXED_TO_DOUBLE(convolution_blur[1]);
		bool rd_from_tgt = (tgt_buffer == src_pict);

		// Copy from source picture to destination. The filter must
		// be applied on source picture, to get the nearby pixels outside the
		// window.
		xcb_render_set_picture_filter(ps->c, src_pict, strlen(XRFILTER_CONVOLUTION),
		                              XRFILTER_CONVOLUTION, kwid * khei + 2,
		                              convolution_blur);
		xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, src_pict, XCB_NONE,
		                     dst_pict, (rd_from_tgt ? x : 0),
		                     (rd_from_tgt ? y : 0), 0, 0, (rd_from_tgt ? 0 : x),
		                     (rd_from_tgt ? 0 : y), wid, hei);
		xrfilter_reset(ps, src_pict);

		{
			xcb_render_picture_t tmp = src_pict;
			src_pict = dst_pict;
			dst_pict = tmp;
		}
	}

	if (src_pict != tgt_buffer)
		xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, src_pict, XCB_NONE,
		                     tgt_buffer, 0, 0, 0, 0, x, y, wid, hei);

	free_picture(ps->c, &tmp_picture);

	return true;
}

/**
 * Blur the background of a window.
 */
static inline void win_blur_background(session_t *ps, win *w, xcb_render_picture_t tgt_buffer,
                                       const region_t *reg_paint) {
	const int x = w->g.x;
	const int y = w->g.y;
	const int wid = w->widthb;
	const int hei = w->heightb;

	double factor_center = 1.0;
	// Adjust blur strength according to window opacity, to make it appear
	// better during fading
	if (!ps->o.blur_background_fixed) {
		double pct = 1.0 - get_opacity_percent(w) * (1.0 - 1.0 / 9.0);
		factor_center = pct * 8.0 / (1.1 - pct);
	}

	switch (ps->o.backend) {
	case BKEND_XRENDER:
	case BKEND_XR_GLX_HYBRID: {
		// Normalize blur kernels
		for (int i = 0; i < MAX_BLUR_PASS; ++i) {
			xcb_render_fixed_t *kern_src = ps->o.blur_kerns[i];
			xcb_render_fixed_t *kern_dst = ps->blur_kerns_cache[i];
			assert(i < MAX_BLUR_PASS);
			if (!kern_src) {
				assert(!kern_dst);
				break;
			}

			assert(!kern_dst ||
			       (kern_src[0] == kern_dst[0] && kern_src[1] == kern_dst[1]));

			// Skip for fixed factor_center if the cache exists already
			if (ps->o.blur_background_fixed && kern_dst)
				continue;

			int kwid = XFIXED_TO_DOUBLE(kern_src[0]),
			    khei = XFIXED_TO_DOUBLE(kern_src[1]);

			// Allocate cache space if needed
			if (!kern_dst) {
				kern_dst = ccalloc(kwid * khei + 2, xcb_render_fixed_t);
				ps->blur_kerns_cache[i] = kern_dst;
			}

			// Modify the factor of the center pixel
			kern_src[2 + (khei / 2) * kwid + kwid / 2] =
			    DOUBLE_TO_XFIXED(factor_center);

			// Copy over
			memcpy(kern_dst, kern_src,
			       (kwid * khei + 2) * sizeof(xcb_render_fixed_t));
			normalize_conv_kern(kwid, khei, kern_dst + 2);
		}

		// Minimize the region we try to blur, if the window itself is not
		// opaque, only the frame is.
		region_t reg_blur = win_get_bounding_shape_global_by_val(w);
		if (win_is_solid(ps, w)) {
			region_t reg_noframe;
			pixman_region32_init(&reg_noframe);
			win_get_region_noframe_local(w, &reg_noframe);
			pixman_region32_translate(&reg_noframe, w->g.x, w->g.y);
			pixman_region32_subtract(&reg_blur, &reg_blur, &reg_noframe);
			pixman_region32_fini(&reg_noframe);
		}
		// Translate global coordinates to local ones
		pixman_region32_translate(&reg_blur, -x, -y);
		xr_blur_dst(ps, tgt_buffer, x, y, wid, hei, ps->blur_kerns_cache, &reg_blur);
		pixman_region32_clear(&reg_blur);
	} break;
#ifdef CONFIG_OPENGL
	case BKEND_GLX:
		// TODO: Handle frame opacity
		glx_blur_dst(ps, x, y, wid, hei, ps->psglx->z - 0.5, factor_center,
		             reg_paint, &w->glx_blur_cache);
		break;
#endif
	default: assert(0);
	}
}

/// paint all windows
/// region = ??
/// region_real = the damage region
void paint_all(session_t *ps, region_t *region, const region_t *region_real, win *const t) {
	if (ps->o.xrender_sync_fence) {
		if (!x_fence_sync(ps, ps->sync_fence)) {
			log_error("x_fence_sync failed, xrender-sync-fence will be "
			          "disabled from now on.");
			xcb_sync_destroy_fence(ps->c, ps->sync_fence);
			ps->sync_fence = XCB_NONE;
			ps->o.xrender_sync_fence = false;
		}
	}

	if (!region_real) {
		region_real = region;
	}

#ifdef DEBUG_REPAINT
	static struct timespec last_paint = {0};
#endif

	if (!region)
		region_real = region = &ps->screen_reg;
	else
		// Remove the damaged area out of screen
		pixman_region32_intersect(region, region, &ps->screen_reg);

#ifdef CONFIG_OPENGL
	if (bkend_use_glx(ps))
		glx_paint_pre(ps, region);
#endif

	if (!paint_isvalid(ps, &ps->tgt_buffer)) {
		if (!ps->tgt_buffer.pixmap) {
			free_paint(ps, &ps->tgt_buffer);
			ps->tgt_buffer.pixmap = x_create_pixmap(
			    ps, ps->depth, ps->root, ps->root_width, ps->root_height);
			if (ps->tgt_buffer.pixmap == XCB_NONE) {
				log_fatal("Failed to allocate a screen-sized pixmap for"
				          "painting");
				exit(1);
			}
		}

		if (BKEND_GLX != ps->o.backend)
			ps->tgt_buffer.pict = x_create_picture_with_visual_and_pixmap(
			    ps, ps->vis, ps->tgt_buffer.pixmap, 0, 0);
	}

	if (BKEND_XRENDER == ps->o.backend) {
		x_set_picture_clip_region(ps, ps->tgt_picture, 0, 0, region_real);
	}

	region_t reg_tmp, *reg_paint;
	pixman_region32_init(&reg_tmp);
	if (t) {
		// Calculate the region upon which the root window is to be painted
		// based on the ignore region of the lowest window, if available
		pixman_region32_subtract(&reg_tmp, region, t->reg_ignore);
		reg_paint = &reg_tmp;
	} else {
		reg_paint = region;
	}

	set_tgt_clip(ps, reg_paint);
	paint_root(ps, reg_paint);

	// Windows are sorted from bottom to top
	// Each window has a reg_ignore, which is the region obscured by all the windows
	// on top of that window. This is used to reduce the number of pixels painted.
	//
	// Whether this is beneficial is to be determined XXX
	for (win *w = t; w; w = w->prev_trans) {
		region_t bshape = win_get_bounding_shape_global_by_val(w);
		// Painting shadow
		if (w->shadow) {
			// Lazy shadow building
			if (!w->shadow_paint.pixmap)
				if (!win_build_shadow(ps, w, 1))
					log_error("build shadow failed");

			// Shadow doesn't need to be painted underneath the body of
			// the window Because no one can see it
			pixman_region32_subtract(&reg_tmp, region, w->reg_ignore);

			// Mask out the region we don't want shadow on
			if (pixman_region32_not_empty(&ps->shadow_exclude_reg))
				pixman_region32_subtract(&reg_tmp, &reg_tmp,
				                         &ps->shadow_exclude_reg);

			// Might be worth while to crop the region to shadow border
			pixman_region32_intersect_rect(
			    &reg_tmp, &reg_tmp, w->g.x + w->shadow_dx,
			    w->g.y + w->shadow_dy, w->shadow_width, w->shadow_height);

			// Mask out the body of the window from the shadow if needed
			// Doing it here instead of in make_shadow() for saving GPU
			// power and handling shaped windows (XXX unconfirmed)
			if (!ps->o.wintype_option[w->window_type].full_shadow)
				pixman_region32_subtract(&reg_tmp, &reg_tmp, &bshape);

#ifdef CONFIG_XINERAMA
			if (ps->o.xinerama_shadow_crop && w->xinerama_scr >= 0 &&
			    w->xinerama_scr < ps->xinerama_nscrs)
				// There can be a window where number of screens is
				// updated, but the screen number attached to the
				// windows have not.
				//
				// Window screen number will be updated eventually,
				// so here we just check to make sure we don't access
				// out of bounds.
				pixman_region32_intersect(
				    &reg_tmp, &reg_tmp,
				    &ps->xinerama_scr_regs[w->xinerama_scr]);
#endif

			// Detect if the region is empty before painting
			if (pixman_region32_not_empty(&reg_tmp)) {
				set_tgt_clip(ps, &reg_tmp);
				win_paint_shadow(ps, w, &reg_tmp);
			}
		}

		// Calculate the region based on the reg_ignore of the next (higher)
		// window and the bounding region
		// XXX XXX
		pixman_region32_subtract(&reg_tmp, region, w->reg_ignore);
		pixman_region32_intersect(&reg_tmp, &reg_tmp, &bshape);
		pixman_region32_fini(&bshape);

		if (pixman_region32_not_empty(&reg_tmp)) {
			set_tgt_clip(ps, &reg_tmp);
			// Blur window background
			if (w->blur_background &&
			    (!win_is_solid(ps, w) ||
			     (ps->o.blur_background_frame && w->frame_opacity != 1)))
				win_blur_background(ps, w, ps->tgt_buffer.pict, &reg_tmp);

			// Painting the window
			paint_one(ps, w, &reg_tmp);
		}
	}

	// Free up all temporary regions
	pixman_region32_fini(&reg_tmp);

	// Do this as early as possible
	set_tgt_clip(ps, &ps->screen_reg);

	if (ps->o.vsync) {
		// Make sure all previous requests are processed to achieve best
		// effect
		x_sync(ps->c);
#ifdef CONFIG_OPENGL
		if (glx_has_context(ps)) {
			if (ps->o.vsync_use_glfinish)
				glFinish();
			else
				glFlush();
			glXWaitX();
		}
#endif
	}

	// Wait for VBlank. We could do it aggressively (send the painting
	// request and XFlush() on VBlank) or conservatively (send the request
	// only on VBlank).
	if (!ps->o.vsync_aggressive)
		vsync_wait(ps);

	switch (ps->o.backend) {
	case BKEND_XRENDER:
		if (ps->o.monitor_repaint) {
			// Copy the screen content to a new picture, and highlight
			// the paint region. This is not very efficient, but since
			// it's for debug only, we don't really care

			// First, we clear tgt_buffer.pict's clip region, since we
			// want to copy everything
			x_set_picture_clip_region(ps, ps->tgt_buffer.pict, 0, 0,
			                          &ps->screen_reg);

			// Then we create a new picture, and copy content to it
			xcb_render_pictforminfo_t *pictfmt =
			    x_get_pictform_for_visual(ps, ps->vis);
			xcb_render_picture_t new_pict = x_create_picture_with_pictfmt(
			    ps, ps->root_width, ps->root_height, pictfmt, 0, NULL);
			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC,
			                     ps->tgt_buffer.pict, XCB_NONE, new_pict, 0, 0,
			                     0, 0, 0, 0, ps->root_width, ps->root_height);

			// Next, we set the region of paint and highlight it
			x_set_picture_clip_region(ps, new_pict, 0, 0, region_real);
			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_OVER,
			                     ps->white_picture,
			                     ps->alpha_picts[MAX_ALPHA / 2], new_pict, 0, 0,
			                     0, 0, 0, 0, ps->root_width, ps->root_height);

			// Finally, clear clip region and put the whole thing on screen
			x_set_picture_clip_region(ps, new_pict, 0, 0, &ps->screen_reg);
			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC, new_pict,
			                     XCB_NONE, ps->tgt_picture, 0, 0, 0, 0, 0, 0,
			                     ps->root_width, ps->root_height);
			xcb_render_free_picture(ps->c, new_pict);
		} else
			xcb_render_composite(ps->c, XCB_RENDER_PICT_OP_SRC,
			                     ps->tgt_buffer.pict, XCB_NONE,
			                     ps->tgt_picture, 0, 0, 0, 0, 0, 0,
			                     ps->root_width, ps->root_height);
		break;
#ifdef CONFIG_OPENGL
	case BKEND_XR_GLX_HYBRID:
		x_sync(ps->c);
		if (ps->o.vsync_use_glfinish)
			glFinish();
		else
			glFlush();
		glXWaitX();
		assert(ps->tgt_buffer.pixmap);
		paint_bind_tex(ps, &ps->tgt_buffer, ps->root_width, ps->root_height,
		               ps->depth, !ps->o.glx_no_rebind_pixmap);
		if (ps->o.vsync_use_glfinish)
			glFinish();
		else
			glFlush();
		glXWaitX();
		glx_render(ps, ps->tgt_buffer.ptex, 0, 0, 0, 0, ps->root_width,
		           ps->root_height, 0, 1.0, false, false, region_real, NULL);
		// falls through
	case BKEND_GLX: glXSwapBuffers(ps->dpy, get_tgt_window(ps)); break;
#endif
	default: assert(0);
	}
	glx_mark_frame(ps);

	if (ps->o.vsync_aggressive)
		vsync_wait(ps);

	XFlush(ps->dpy);

#ifdef CONFIG_OPENGL
	if (glx_has_context(ps)) {
		glFlush();
		glXWaitX();
	}
#endif

#ifdef DEBUG_REPAINT
	struct timespec now = get_time_timespec();
	struct timespec diff = {0};
	timespec_subtract(&diff, &now, &last_paint);
	log_trace("[ %5ld:%09ld ] ", diff.tv_sec, diff.tv_nsec);
	last_paint = now;
	log_trace("paint:");
	for (win *w = t; w; w = w->prev_trans)
		log_trace(" %#010lx", w->id);
#endif

	// Check if fading is finished on all painted windows
	{
		win *pprev = NULL;
		for (win *w = t; w; w = pprev) {
			pprev = w->prev_trans;
			win_check_fade_finished(ps, &w);
		}
	}
}

/**
 * Query needed X Render / OpenGL filters to check for their existence.
 */
static bool xr_init_blur(session_t *ps) {
	// Query filters
	xcb_render_query_filters_reply_t *pf = xcb_render_query_filters_reply(
	    ps->c, xcb_render_query_filters(ps->c, get_tgt_window(ps)), NULL);
	if (pf) {
		xcb_str_iterator_t iter = xcb_render_query_filters_filters_iterator(pf);
		for (; iter.rem; xcb_str_next(&iter)) {
			int len = xcb_str_name_length(iter.data);
			char *name = xcb_str_name(iter.data);
			// Check for the convolution filter
			if (strlen(XRFILTER_CONVOLUTION) == len &&
			    !memcmp(XRFILTER_CONVOLUTION, name, strlen(XRFILTER_CONVOLUTION)))
				ps->xrfilter_convolution_exists = true;
		}
		free(pf);
	}

	// Turn features off if any required filter is not present
	if (!ps->xrfilter_convolution_exists) {
		log_error("Xrender convolution filter "
		          "unsupported by your X server. "
		          "Background blur is not possible.");
		return false;
	}

	return true;
}

/**
 * Pregenerate alpha pictures.
 */
static bool init_alpha_picts(session_t *ps) {
	ps->alpha_picts = ccalloc(MAX_ALPHA + 1, xcb_render_picture_t);

	for (int i = 0; i <= MAX_ALPHA; ++i) {
		double o = (double)i / MAX_ALPHA;
		ps->alpha_picts[i] = solid_picture(ps, false, o, 0, 0, 0);
		if (ps->alpha_picts[i] == XCB_NONE)
			return false;
	}
	return true;
}

bool init_render(session_t *ps) {
	// Initialize OpenGL as early as possible
	if (bkend_use_glx(ps)) {
#ifdef CONFIG_OPENGL
		if (!glx_init(ps, true))
			return false;
#else
		log_error("GLX backend support not compiled in.");
		return false;
#endif
	}

	// Initialize VSync
	if (!vsync_init(ps)) {
		return false;
	}

	// Initialize window GL shader
	if (BKEND_GLX == ps->o.backend && ps->o.glx_fshader_win_str) {
#ifdef CONFIG_OPENGL
		if (!glx_load_prog_main(ps, NULL, ps->o.glx_fshader_win_str, &ps->glx_prog_win))
			return false;
#else
		log_error("GLSL supported not compiled in, can't load "
		          "shader.");
		return false;
#endif
	}

	if (!init_alpha_picts(ps)) {
		log_error("Failed to init alpha pictures.");
		return false;
	}

	// Blur filter
	if (ps->o.blur_background || ps->o.blur_background_frame) {
		bool ret;
		if (ps->o.backend == BKEND_GLX) {
#ifdef CONFIG_OPENGL
			ret = glx_init_blur(ps);
#else
			assert(false);
#endif
		} else
			ret = xr_init_blur(ps);
		if (!ret)
			return false;
	}

	ps->gaussian_map = gaussian_kernel(ps->o.shadow_radius);
	shadow_preprocess(ps->gaussian_map);

	ps->black_picture = solid_picture(ps, true, 1, 0, 0, 0);
	ps->white_picture = solid_picture(ps, true, 1, 1, 1, 1);

	if (ps->black_picture == XCB_NONE || ps->white_picture == XCB_NONE) {
		log_error("Failed to create solid xrender pictures.");
		return false;
	}

	// Generates another Picture for shadows if the color is modified by
	// user
	if (!ps->o.shadow_red && !ps->o.shadow_green && !ps->o.shadow_blue) {
		ps->cshadow_picture = ps->black_picture;
	} else {
		ps->cshadow_picture = solid_picture(
		    ps, true, 1, ps->o.shadow_red, ps->o.shadow_green, ps->o.shadow_blue);
		if (ps->cshadow_picture == XCB_NONE) {
			log_error("Failed to create shadow picture.");
			return false;
		}
	}
	return true;
}

/**
 * Free root tile related things.
 */
void free_root_tile(session_t *ps) {
	free_picture(ps->c, &ps->root_tile_paint.pict);
#ifdef CONFIG_OPENGL
	free_texture(ps, &ps->root_tile_paint.ptex);
#else
	assert(!ps->root_tile_paint.ptex);
#endif
	if (ps->root_tile_fill) {
		xcb_free_pixmap(ps->c, ps->root_tile_paint.pixmap);
		ps->root_tile_paint.pixmap = XCB_NONE;
	}
	ps->root_tile_paint.pixmap = XCB_NONE;
	ps->root_tile_fill = false;
}

void deinit_render(session_t *ps) {
	// Free alpha_picts
	for (int i = 0; i <= MAX_ALPHA; ++i)
		free_picture(ps->c, &ps->alpha_picts[i]);
	free(ps->alpha_picts);
	ps->alpha_picts = NULL;

	// Free cshadow_picture and black_picture
	if (ps->cshadow_picture == ps->black_picture)
		ps->cshadow_picture = XCB_NONE;
	else
		free_picture(ps->c, &ps->cshadow_picture);

	free_picture(ps->c, &ps->black_picture);
	free_picture(ps->c, &ps->white_picture);
	free_conv(ps->gaussian_map);

	// Free other X resources
	free_root_tile(ps);

#ifdef CONFIG_OPENGL
	glx_destroy(ps);
#endif
}

// vim: set ts=8 sw=8 noet :
