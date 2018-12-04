/*
 * Copyright © 2013,2014 Intel Corporation
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
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "igt.h"
#include <math.h>

#define MAX_FENCES 32

typedef struct {
	int gfx_fd;
	igt_display_t display;
	struct igt_fb fb;
	struct igt_fb fb_reference;
	struct igt_fb fb_unrotated;
	struct igt_fb fb_flip;
	igt_crc_t ref_crc;
	igt_crc_t flip_crc;
	igt_pipe_crc_t *pipe_crc;
	igt_rotation_t rotation;
	int pos_x;
	int pos_y;
	uint32_t override_fmt;
	uint64_t override_tiling;
	int devid;
} data_t;

typedef struct {
	float r;
	float g;
	float b;
} rgb_color_t;

static void set_color(rgb_color_t *color, float r, float g, float b)
{
	color->r = r;
	color->g = g;
	color->b = b;
}

static void rotate_colors(rgb_color_t *tl, rgb_color_t *tr, rgb_color_t *br,
			  rgb_color_t *bl, igt_rotation_t rotation)
{
	rgb_color_t bl_tmp, br_tmp, tl_tmp, tr_tmp;

	if (rotation & IGT_REFLECT_X) {
		igt_swap(*tl, *tr);
		igt_swap(*bl, *br);
	}

	if (rotation & IGT_ROTATION_90) {
		bl_tmp = *bl;
		br_tmp = *br;
		tl_tmp = *tl;
		tr_tmp = *tr;
		*tl = tr_tmp;
		*bl = tl_tmp;
		*tr = br_tmp;
		*br = bl_tmp;
	} else if (rotation & IGT_ROTATION_180) {
		igt_swap(*tl, *br);
		igt_swap(*tr, *bl);
	} else if (rotation & IGT_ROTATION_270) {
		bl_tmp = *bl;
		br_tmp = *br;
		tl_tmp = *tl;
		tr_tmp = *tr;
		*tl = bl_tmp;
		*bl = br_tmp;
		*tr = tl_tmp;
		*br = tr_tmp;
	}
}

#define RGB_COLOR(color) \
	color.r, color.g, color.b

static void
paint_squares(data_t *data, igt_rotation_t rotation,
	      struct igt_fb *fb, float o)
{
	cairo_t *cr;
	unsigned int w = fb->width;
	unsigned int h = fb->height;
	rgb_color_t tl, tr, bl, br;

	cr = igt_get_cairo_ctx(data->gfx_fd, fb);

	set_color(&tl, o, 0.0f, 0.0f);
	set_color(&tr, 0.0f, o, 0.0f);
	set_color(&br, o, o, o);
	set_color(&bl, 0.0f, 0.0f, o);

	rotate_colors(&tl, &tr, &br, &bl, rotation);

	igt_paint_color(cr, 0, 0, w / 2, h / 2, RGB_COLOR(tl));
	igt_paint_color(cr, w / 2, 0, w / 2, h / 2, RGB_COLOR(tr));
	igt_paint_color(cr, 0, h / 2, w / 2, h / 2, RGB_COLOR(bl));
	igt_paint_color(cr, w / 2, h / 2, w / 2, h / 2, RGB_COLOR(br));

	igt_put_cairo_ctx(data->gfx_fd, fb, cr);
}

static void remove_fbs(data_t *data)
{
	igt_remove_fb(data->gfx_fd, &data->fb);
	igt_remove_fb(data->gfx_fd, &data->fb_reference);
	igt_remove_fb(data->gfx_fd, &data->fb_unrotated);
	igt_remove_fb(data->gfx_fd, &data->fb_flip);
}

static void cleanup_crtc(data_t *data)
{
	igt_display_t *display = &data->display;

	igt_pipe_crc_free(data->pipe_crc);
	data->pipe_crc = NULL;

	remove_fbs(data);

	igt_display_reset(display);
}

static void prepare_crtc(data_t *data, igt_output_t *output, enum pipe pipe,
			 igt_plane_t *plane, bool start_crc)
{
	igt_display_t *display = &data->display;

	cleanup_crtc(data);

	igt_output_set_pipe(output, pipe);
	igt_plane_set_rotation(plane, IGT_ROTATION_0);

	/* create the pipe_crc object for this pipe */
	igt_pipe_crc_free(data->pipe_crc);

	igt_display_commit2(display, COMMIT_ATOMIC);
	data->pipe_crc = igt_pipe_crc_new(data->gfx_fd, pipe, INTEL_PIPE_CRC_SOURCE_AUTO);

	if (start_crc)
		igt_pipe_crc_start(data->pipe_crc);
}

enum rectangle_type {
	rectangle,
	square,
	portrait,
	landscape,
	num_rectangle_types /* must be last */
};

static void prepare_fbs(data_t *data, igt_output_t *output,
			igt_plane_t *plane, enum rectangle_type rect, uint32_t format)
{
	drmModeModeInfo *mode;
	igt_display_t *display = &data->display;
	unsigned int w, h, ref_w, ref_h, min_w, min_h;
	uint64_t tiling = data->override_tiling ?: LOCAL_DRM_FORMAT_MOD_NONE;
	uint32_t pixel_format = data->override_fmt ?: DRM_FORMAT_XRGB8888;
	const float flip_opacity = 0.75;

	remove_fbs(data);

	igt_plane_set_rotation(plane, IGT_ROTATION_0);

	mode = igt_output_get_mode(output);
	if (plane->type != DRM_PLANE_TYPE_CURSOR) {
		w = mode->hdisplay;
		h = mode->vdisplay;

		min_w = 256;
		min_h = 256;
	} else {
		pixel_format = data->override_fmt ?: DRM_FORMAT_ARGB8888;

		w = h = 256;
		min_w = min_h = 64;
	}

	switch (rect) {
	case rectangle:
		break;
	case square:
		w = h = min(h, w);
		break;
	case portrait:
		w = min_w;
		break;
	case landscape:
		h = min_h;
		break;
	case num_rectangle_types:
		igt_assert(0);
	}

	ref_w = w;
	ref_h = h;

	/*
	 * For 90/270, we will use create smaller fb so that the rotated
	 * frame can fit in
	 */
	if (data->rotation & (IGT_ROTATION_90 | IGT_ROTATION_270)) {
		tiling = data->override_tiling ?: LOCAL_I915_FORMAT_MOD_Y_TILED;

		igt_swap(w, h);
	}

	/*
	 * Create a reference software rotated flip framebuffer.
	 */
	igt_create_fb(data->gfx_fd, ref_w, ref_h, pixel_format, tiling,
		      &data->fb_flip);
	paint_squares(data, data->rotation, &data->fb_flip,
		      flip_opacity);
	igt_plane_set_fb(plane, &data->fb_flip);
	if (plane->type != DRM_PLANE_TYPE_CURSOR)
		igt_plane_set_position(plane, data->pos_x, data->pos_y);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &data->flip_crc);

	/*
	  * Prepare the non-rotated flip fb.
	  */
	igt_remove_fb(data->gfx_fd, &data->fb_flip);
	igt_create_fb(data->gfx_fd, w, h, pixel_format, tiling,
		      &data->fb_flip);
	paint_squares(data, IGT_ROTATION_0, &data->fb_flip,
		      flip_opacity);

	/*
	 * Create a reference CRC for a software-rotated fb.
	 */
	igt_create_fb(data->gfx_fd, ref_w, ref_h, pixel_format,
		      data->override_tiling ?: LOCAL_DRM_FORMAT_MOD_NONE, &data->fb_reference);
	paint_squares(data, data->rotation, &data->fb_reference, 1.0);

	igt_plane_set_fb(plane, &data->fb_reference);
	if (plane->type != DRM_PLANE_TYPE_CURSOR)
		igt_plane_set_position(plane, data->pos_x, data->pos_y);
	igt_display_commit2(display, COMMIT_ATOMIC);

	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &data->ref_crc);

	/*
	 * Prepare the non-rotated reference fb.
	 */
	igt_create_fb(data->gfx_fd, ref_w, ref_h, pixel_format, tiling, &data->fb_unrotated);
	paint_squares(data, IGT_ROTATION_0, &data->fb_unrotated, 1.0);
	igt_plane_set_fb(plane, &data->fb_unrotated);
	igt_plane_set_rotation(plane, IGT_ROTATION_0);
	if (plane->type != DRM_PLANE_TYPE_CURSOR)
		igt_plane_set_position(plane, data->pos_x, data->pos_y);
	igt_display_commit2(display, COMMIT_ATOMIC);

	/*
	 * Prepare the plane with an non-rotated fb let the hw rotate it.
	 */
	igt_create_fb(data->gfx_fd, w, h, pixel_format, tiling, &data->fb);
	paint_squares(data, IGT_ROTATION_0, &data->fb, 1.0);
	igt_plane_set_fb(plane, &data->fb);

	if (plane->type != DRM_PLANE_TYPE_CURSOR)
		igt_plane_set_position(plane, data->pos_x, data->pos_y);
}

static void test_single_case(data_t *data, enum pipe pipe,
			     igt_output_t *output, igt_plane_t *plane,
			     enum rectangle_type rect,
			     uint32_t format, bool test_bad_format)
{
	igt_display_t *display = &data->display;
	igt_crc_t crc_output;
	int ret;

	igt_debug("Testing case %i on pipe %s, format %s\n", rect, kmstest_pipe_name(pipe), igt_format_str(format));
	prepare_fbs(data, output, plane, rect, format);

	igt_plane_set_rotation(plane, data->rotation);
	if (data->rotation & (IGT_ROTATION_90 | IGT_ROTATION_270))
		igt_plane_set_size(plane, data->fb.height, data->fb.width);

	ret = igt_display_try_commit2(display, COMMIT_ATOMIC);
	if (test_bad_format) {
		igt_assert_eq(ret, -EINVAL);
		return;
	}

	/* Verify commit was ok. */
	igt_assert_eq(ret, 0);

	/* Check CRC */
	igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &crc_output);
	igt_assert_crc_equal(&data->ref_crc, &crc_output);

	/*
	 * If flips are requested flip to a different fb and
	 * check CRC against that one as well.
	 */
	if (data->fb_flip.fb_id) {
		igt_plane_set_fb(plane, &data->fb_flip);
		if (data->rotation == IGT_ROTATION_90 || data->rotation == IGT_ROTATION_270)
			igt_plane_set_size(plane, data->fb.height, data->fb.width);

		if (plane->type != DRM_PLANE_TYPE_PRIMARY) {
			igt_display_commit_atomic(display, DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, NULL);
		} else {
			ret = drmModePageFlip(data->gfx_fd,
					output->config.crtc->crtc_id,
					data->fb_flip.fb_id,
					DRM_MODE_PAGE_FLIP_EVENT,
					NULL);
			igt_assert_eq(ret, 0);
		}
		kmstest_wait_for_pageflip(data->gfx_fd);
		igt_pipe_crc_get_current(display->drm_fd, data->pipe_crc, &crc_output);
		igt_assert_crc_equal(&data->flip_crc,
				     &crc_output);
	}
}

static void test_plane_rotation(data_t *data, int plane_type, bool test_bad_format)
{
	igt_display_t *display = &data->display;
	igt_output_t *output;
	enum pipe pipe;

	if (plane_type == DRM_PLANE_TYPE_CURSOR)
		igt_require(display->has_cursor_plane);

	igt_display_require_output(display);

	for_each_pipe_with_valid_output(display, pipe, output) {
		igt_plane_t *plane;
		int i, j;

		if (IS_CHERRYVIEW(data->devid) && pipe != PIPE_B)
			continue;

		igt_output_set_pipe(output, pipe);

		plane = igt_output_get_plane_type(output, plane_type);
		igt_require(igt_plane_has_prop(plane, IGT_PLANE_ROTATION));

		prepare_crtc(data, output, pipe, plane, true);

		for (i = 0; i < num_rectangle_types; i++) {
			/* Unsupported on i915 */
			if (plane_type == DRM_PLANE_TYPE_CURSOR &&
			    i != square)
				continue;

			/* Only support partial covering primary plane on gen9+ */
			if (plane_type == DRM_PLANE_TYPE_PRIMARY &&
			    i != rectangle && intel_gen(intel_get_drm_devid(data->gfx_fd)) < 9)
				continue;

			if (!data->override_fmt) {
				for (j = 0; j < plane->drm_plane->count_formats; j++) {
					uint32_t format = plane->drm_plane->formats[j];

					if (!igt_fb_supported_format(format))
						continue;

					test_single_case(data, pipe, output, plane, i,
							 format, test_bad_format);
				}
			} else {
				test_single_case(data, pipe, output, plane, i,
						 data->override_fmt, test_bad_format);
			}
		}
		igt_pipe_crc_stop(data->pipe_crc);
	}
}

static void test_plane_rotation_exhaust_fences(data_t *data,
					       enum pipe pipe,
					       igt_output_t *output,
					       igt_plane_t *plane)
{
	igt_display_t *display = &data->display;
	uint64_t tiling = LOCAL_I915_FORMAT_MOD_Y_TILED;
	uint32_t format = DRM_FORMAT_XRGB8888;
	int fd = data->gfx_fd;
	drmModeModeInfo *mode;
	struct igt_fb fb[MAX_FENCES+1] = {};
	uint64_t size;
	unsigned int stride, w, h;
	uint64_t total_aperture_size, total_fbs_size;
	int i;

	igt_require(igt_plane_has_prop(plane, IGT_PLANE_ROTATION));

	prepare_crtc(data, output, pipe, plane, false);

	mode = igt_output_get_mode(output);
	w = mode->hdisplay;
	h = mode->vdisplay;

	igt_calc_fb_size(fd, w, h, format, tiling, &size, &stride);

	/*
	 * Make sure there is atleast 90% of the available GTT space left
	 * for creating (MAX_FENCES+1) framebuffers.
	 */
	total_fbs_size = size * (MAX_FENCES + 1);
	total_aperture_size = gem_available_aperture_size(fd);
	igt_require(total_fbs_size < total_aperture_size * 0.9);

	for (i = 0; i < MAX_FENCES + 1; i++) {
		igt_create_fb(fd, w, h, format, tiling, &fb[i]);

		igt_plane_set_fb(plane, &fb[i]);
		igt_plane_set_rotation(plane, IGT_ROTATION_0);
		igt_display_commit2(display, COMMIT_ATOMIC);

		igt_plane_set_rotation(plane, IGT_ROTATION_90);
		igt_plane_set_size(plane, h, w);
		igt_display_commit2(display, COMMIT_ATOMIC);
	}

	for (i = 0; i < MAX_FENCES + 1; i++)
		igt_remove_fb(fd, &fb[i]);
}

static const char *plane_test_str(unsigned plane)
{
	switch (plane) {
	case DRM_PLANE_TYPE_PRIMARY:
		return "primary";
	case DRM_PLANE_TYPE_OVERLAY:
		return "sprite";
	case DRM_PLANE_TYPE_CURSOR:
		return "cursor";
	default:
		igt_assert(0);
	}
}

static const char *rot_test_str(igt_rotation_t rot)
{
	switch (rot) {
	case IGT_ROTATION_0:
		return "0";
	case IGT_ROTATION_90:
		return "90";
	case IGT_ROTATION_180:
		return "180";
	case IGT_ROTATION_270:
		return "270";
	default:
		igt_assert(0);
	}
}

static const char *tiling_test_str(uint64_t tiling)
{
	switch (tiling) {
	case LOCAL_I915_FORMAT_MOD_X_TILED:
		return "x-tiled";
	case LOCAL_I915_FORMAT_MOD_Y_TILED:
		return "y-tiled";
	case LOCAL_I915_FORMAT_MOD_Yf_TILED:
		return "yf-tiled";
	default:
		igt_assert(0);
	}
}

igt_main
{
	struct rot_subtest {
		unsigned plane;
		igt_rotation_t rot;
	} *subtest, subtests[] = {
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_90 },
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_180 },
		{ DRM_PLANE_TYPE_PRIMARY, IGT_ROTATION_270 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_90 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_180 },
		{ DRM_PLANE_TYPE_OVERLAY, IGT_ROTATION_270 },
		{ DRM_PLANE_TYPE_CURSOR, IGT_ROTATION_180 },
		{ 0, 0}
	};

	struct reflect_x {
		uint64_t tiling;
		igt_rotation_t rot;
	} *reflect_x, reflect_x_subtests[] = {
		{ LOCAL_I915_FORMAT_MOD_X_TILED, IGT_ROTATION_0 },
		{ LOCAL_I915_FORMAT_MOD_X_TILED, IGT_ROTATION_180 },
		{ LOCAL_I915_FORMAT_MOD_Y_TILED, IGT_ROTATION_0 },
		{ LOCAL_I915_FORMAT_MOD_Y_TILED, IGT_ROTATION_90 },
		{ LOCAL_I915_FORMAT_MOD_Y_TILED, IGT_ROTATION_180 },
		{ LOCAL_I915_FORMAT_MOD_Y_TILED, IGT_ROTATION_270 },
		{ LOCAL_I915_FORMAT_MOD_Yf_TILED, IGT_ROTATION_0 },
		{ LOCAL_I915_FORMAT_MOD_Yf_TILED, IGT_ROTATION_90 },
		{ LOCAL_I915_FORMAT_MOD_Yf_TILED, IGT_ROTATION_180 },
		{ LOCAL_I915_FORMAT_MOD_Yf_TILED, IGT_ROTATION_270 },
		{ 0, 0 }
	};

	data_t data = {};
	int gen = 0;

	igt_skip_on_simulation();

	igt_fixture {
		data.gfx_fd = drm_open_driver_master(DRIVER_INTEL);
		data.devid = intel_get_drm_devid(data.gfx_fd);
		gen = intel_gen(data.devid);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(data.gfx_fd);

		igt_display_require(&data.display, data.gfx_fd);
	}

	for (subtest = subtests; subtest->rot; subtest++) {
		igt_subtest_f("%s-rotation-%s",
			      plane_test_str(subtest->plane),
			      rot_test_str(subtest->rot)) {
			igt_require(!(subtest->rot &
				    (IGT_ROTATION_90 | IGT_ROTATION_270)) ||
				    gen >= 9);
			data.rotation = subtest->rot;
			test_plane_rotation(&data, subtest->plane, false);
		}
	}

	igt_subtest_f("sprite-rotation-90-pos-100-0") {
		igt_require(gen >= 9);
		data.rotation = IGT_ROTATION_90;
		data.pos_x = 100,
		data.pos_y = 0;
		test_plane_rotation(&data, DRM_PLANE_TYPE_OVERLAY, false);
	}
	data.pos_x = 0,
	data.pos_y = 0;

	igt_subtest_f("bad-pixel-format") {
		/*
		 * gen11 enables RGB565 rotation for 90/270 degrees.
		 * DRM_FORMAT_C8 fmt need to be enabled for IGT if want to run
		 * this test on gen11 and later.
		 */
		igt_require(gen >= 9 && gen < 11);
		data.rotation = IGT_ROTATION_90;
		data.override_fmt = DRM_FORMAT_RGB565;
		test_plane_rotation(&data, DRM_PLANE_TYPE_PRIMARY, true);
	}
	data.override_fmt = 0;

	igt_subtest_f("bad-tiling") {
		igt_require(gen >= 9);
		data.rotation = IGT_ROTATION_90;
		data.override_tiling = LOCAL_I915_FORMAT_MOD_X_TILED;
		test_plane_rotation(&data, DRM_PLANE_TYPE_PRIMARY, true);
	}
	data.override_tiling = 0;

	for (reflect_x = reflect_x_subtests; reflect_x->tiling; reflect_x++) {
		igt_subtest_f("primary-%s-reflect-x-%s",
			      tiling_test_str(reflect_x->tiling),
			      rot_test_str(reflect_x->rot)) {
			igt_require(gen >= 10 ||
				    (IS_CHERRYVIEW(data.devid) && reflect_x->rot == IGT_ROTATION_0
				     && reflect_x->tiling == LOCAL_I915_FORMAT_MOD_X_TILED));
			data.rotation = (IGT_REFLECT_X | reflect_x->rot);
			data.override_tiling = reflect_x->tiling;
			test_plane_rotation(&data, DRM_PLANE_TYPE_PRIMARY, false);
		}
	}

	/*
	 * exhaust-fences should be last test, if it fails we may OOM in
	 * the following subtests otherwise.
	 */
	igt_subtest_f("exhaust-fences") {
		enum pipe pipe;
		igt_output_t *output;

		igt_require(gen >= 9);
		igt_display_require_output(&data.display);

		for_each_pipe_with_valid_output(&data.display, pipe, output) {
			igt_plane_t *primary = &data.display.pipes[pipe].planes[0];

			test_plane_rotation_exhaust_fences(&data, pipe, output, primary);
			break;
		}
	}

	igt_fixture {
		igt_display_fini(&data.display);
	}
}
