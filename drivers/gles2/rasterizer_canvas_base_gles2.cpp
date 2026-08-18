/**************************************************************************/
/*  rasterizer_canvas_base_gles2.cpp                                      */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "rasterizer_canvas_base_gles2.h"

#include "core/os/os.h"
#include "core/project_settings.h"
#include "drivers/gles_common/rasterizer_asserts.h"
#include "rasterizer_scene_gles2.h"
#include "servers/visual/visual_server_raster.h"

#ifndef GLES_OVER_GL
#define glClearDepth glClearDepthf
#endif

RID RasterizerCanvasBaseGLES2::light_internal_create() {
	return RID();
}

void RasterizerCanvasBaseGLES2::light_internal_update(RID p_rid, Light *p_light) {
}

void RasterizerCanvasBaseGLES2::light_internal_free(RID p_rid) {
}

void RasterizerCanvasBaseGLES2::canvas_begin() {
	state.using_transparent_rt = false;

	// always start with light_angle unset
	state.using_light_angle = false;
	state.using_large_vertex = false;
	state.using_modulate = false;

	state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_DISTANCE_FIELD, false);
	state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_ATTRIB_LIGHT_ANGLE, false);
	state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_ATTRIB_MODULATE, false);
	state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_ATTRIB_LARGE_VERTEX, false);
	state.canvas_shader.bind();

	int viewport_x, viewport_y, viewport_width, viewport_height;

	if (storage->frame.current_rt) {
		glBindFramebuffer(GL_FRAMEBUFFER, storage->frame.current_rt->fbo);
		state.using_transparent_rt = storage->frame.current_rt->flags[RasterizerStorage::RENDER_TARGET_TRANSPARENT];

		if (storage->frame.current_rt->flags[RasterizerStorage::RENDER_TARGET_DIRECT_TO_SCREEN]) {
			// set Viewport and Scissor when rendering directly to screen
			viewport_width = storage->frame.current_rt->width;
			viewport_height = storage->frame.current_rt->height;
			viewport_x = storage->frame.current_rt->x;
			viewport_y = OS::get_singleton()->get_window_size().height - viewport_height - storage->frame.current_rt->y;
			glScissor(viewport_x, viewport_y, viewport_width, viewport_height);
			glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
			glEnable(GL_SCISSOR_TEST);
		}
	}

	if (storage->frame.clear_request) {
		glClearColor(storage->frame.clear_request_color.r,
				storage->frame.clear_request_color.g,
				storage->frame.clear_request_color.b,
				state.using_transparent_rt ? storage->frame.clear_request_color.a : 1.0);
		glClear(GL_COLOR_BUFFER_BIT);
		storage->frame.clear_request = false;
	}

	/*
	if (storage->frame.current_rt) {
		glBindFramebuffer(GL_FRAMEBUFFER, storage->frame.current_rt->fbo);
		glColorMask(1, 1, 1, 1);
	}
	*/

	reset_canvas();

	WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, storage->resources.white_tex);

	// ARRAY_COLOR's default/baseline state is set up by _bind_quad_buffer()
	// below (real array-fed white, not disabled -- see its comment) rather
	// than here, since this port's target driver corrupts the framebuffer
	// when a disabled attribute is written into a varying.

	// set up default uniforms

	Transform canvas_transform;

	if (storage->frame.current_rt) {
		float csy = 1.0;
		if (storage->frame.current_rt && storage->frame.current_rt->flags[RasterizerStorage::RENDER_TARGET_VFLIP]) {
			csy = -1.0;
		}
		canvas_transform.translate(-(storage->frame.current_rt->width / 2.0f), -(storage->frame.current_rt->height / 2.0f), 0.0f);
		canvas_transform.scale(Vector3(2.0f / storage->frame.current_rt->width, csy * -2.0f / storage->frame.current_rt->height, 1.0f));
	} else {
		Vector2 ssize = OS::get_singleton()->get_window_size();
		canvas_transform.translate(-(ssize.width / 2.0f), -(ssize.height / 2.0f), 0.0f);
		canvas_transform.scale(Vector3(2.0f / ssize.width, -2.0f / ssize.height, 1.0f));
	}

	state.uniforms.projection_matrix = canvas_transform;

	state.uniforms.final_modulate = Color(1, 1, 1, 1);

	state.uniforms.modelview_matrix = Transform2D();
	state.uniforms.extra_matrix = Transform2D();

	_set_uniforms();
	_bind_quad_buffer();
}

void RasterizerCanvasBaseGLES2::canvas_end() {
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	for (int i = 0; i < VS::ARRAY_MAX; i++) {
		glDisableVertexAttribArray(i);
	}

	if (storage->frame.current_rt && storage->frame.current_rt->flags[RasterizerStorage::RENDER_TARGET_DIRECT_TO_SCREEN]) {
		//reset viewport to full window size
		int viewport_width = OS::get_singleton()->get_window_size().width;
		int viewport_height = OS::get_singleton()->get_window_size().height;
		glViewport(0, 0, viewport_width, viewport_height);
		glScissor(0, 0, viewport_width, viewport_height);
	}

	state.using_texture_rect = false;
	state.using_skeleton = false;
	state.using_ninepatch = false;
	state.using_transparent_rt = false;
}

void RasterizerCanvasBaseGLES2::draw_generic_textured_rect(const Rect2 &p_rect, const Rect2 &p_src) {
	state.canvas_shader.set_uniform(CanvasShaderGLES2::DST_RECT, Color(p_rect.position.x, p_rect.position.y, p_rect.size.x, p_rect.size.y));
	state.canvas_shader.set_uniform(CanvasShaderGLES2::SRC_RECT, Color(p_src.position.x, p_src.position.y, p_src.size.x, p_src.size.y));

	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

void RasterizerCanvasBaseGLES2::_set_texture_rect_mode(bool p_texture_rect, bool p_light_angle, bool p_modulate, bool p_large_vertex) {
	// always set this directly (this could be state checked)
	state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_TEXTURE_RECT, p_texture_rect);

	if (state.using_light_angle != p_light_angle) {
		state.using_light_angle = p_light_angle;
		state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_ATTRIB_LIGHT_ANGLE, p_light_angle);
	}

	if (state.using_modulate != p_modulate) {
		state.using_modulate = p_modulate;
		state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_ATTRIB_MODULATE, p_modulate);
	}

	if (state.using_large_vertex != p_large_vertex) {
		state.using_large_vertex = p_large_vertex;
		state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_ATTRIB_LARGE_VERTEX, p_large_vertex);
	}
}

RasterizerStorageGLES2::Texture *RasterizerCanvasBaseGLES2::_bind_canvas_texture(const RID &p_texture, const RID &p_normal_map) {
	RasterizerStorageGLES2::Texture *tex_return = nullptr;

	if (p_texture.is_valid()) {
		RasterizerStorageGLES2::Texture *texture = storage->texture_owner.getornull(p_texture);

		if (!texture) {
			state.current_tex = RID();
			state.current_tex_ptr = nullptr;

			WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0 + storage->config.max_texture_image_units - 1);
			glBindTexture(GL_TEXTURE_2D, storage->resources.white_tex);

		} else {
			if (texture->redraw_if_visible) {
				VisualServerRaster::redraw_request(false);
			}

			texture = texture->get_ptr();

			if (texture->render_target) {
				texture->render_target->used_in_frame = true;
			}

			WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0 + storage->config.max_texture_image_units - 1);
			glBindTexture(GL_TEXTURE_2D, texture->tex_id);

			state.current_tex = p_texture;
			state.current_tex_ptr = texture;

			tex_return = texture;
		}
	} else {
		state.current_tex = RID();
		state.current_tex_ptr = nullptr;

		WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0 + storage->config.max_texture_image_units - 1);
		glBindTexture(GL_TEXTURE_2D, storage->resources.white_tex);
	}

	if (p_normal_map == state.current_normal) {
		//do none
		state.canvas_shader.set_uniform(CanvasShaderGLES2::USE_DEFAULT_NORMAL, state.current_normal.is_valid());

	} else if (p_normal_map.is_valid()) {
		RasterizerStorageGLES2::Texture *normal_map = storage->texture_owner.getornull(p_normal_map);

		if (!normal_map) {
			state.current_normal = RID();
			WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0 + storage->config.max_texture_image_units - 2);
			glBindTexture(GL_TEXTURE_2D, storage->resources.normal_tex);
			state.canvas_shader.set_uniform(CanvasShaderGLES2::USE_DEFAULT_NORMAL, false);

		} else {
			if (normal_map->redraw_if_visible) { //check before proxy, because this is usually used with proxies
				VisualServerRaster::redraw_request(false);
			}

			normal_map = normal_map->get_ptr();

			WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0 + storage->config.max_texture_image_units - 2);
			glBindTexture(GL_TEXTURE_2D, normal_map->tex_id);
			state.current_normal = p_normal_map;
			state.canvas_shader.set_uniform(CanvasShaderGLES2::USE_DEFAULT_NORMAL, true);
		}

	} else {
		state.current_normal = RID();
		WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0 + storage->config.max_texture_image_units - 2);
		glBindTexture(GL_TEXTURE_2D, storage->resources.normal_tex);
		state.canvas_shader.set_uniform(CanvasShaderGLES2::USE_DEFAULT_NORMAL, false);
	}

	return tex_return;
}

void RasterizerCanvasBaseGLES2::draw_window_margins(int *black_margin, RID *black_image) {
	Vector2 window_size = OS::get_singleton()->get_window_size();
	int window_h = window_size.height;
	int window_w = window_size.width;

	glBindFramebuffer(GL_FRAMEBUFFER, storage->system_fbo);
	glViewport(0, 0, window_size.width, window_size.height);
	canvas_begin();

	if (black_image[MARGIN_LEFT].is_valid()) {
		_bind_canvas_texture(black_image[MARGIN_LEFT], RID());
		Size2 sz(storage->texture_get_width(black_image[MARGIN_LEFT]), storage->texture_get_height(black_image[MARGIN_LEFT]));
		draw_generic_textured_rect(Rect2(0, 0, black_margin[MARGIN_LEFT], window_h),
				Rect2(0, 0, (float)black_margin[MARGIN_LEFT] / sz.x, (float)(window_h) / sz.y));
	} else if (black_margin[MARGIN_LEFT]) {
		WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, storage->resources.black_tex);

		draw_generic_textured_rect(Rect2(0, 0, black_margin[MARGIN_LEFT], window_h), Rect2(0, 0, 1, 1));
	}

	if (black_image[MARGIN_RIGHT].is_valid()) {
		_bind_canvas_texture(black_image[MARGIN_RIGHT], RID());
		Size2 sz(storage->texture_get_width(black_image[MARGIN_RIGHT]), storage->texture_get_height(black_image[MARGIN_RIGHT]));
		draw_generic_textured_rect(Rect2(window_w - black_margin[MARGIN_RIGHT], 0, black_margin[MARGIN_RIGHT], window_h),
				Rect2(0, 0, (float)black_margin[MARGIN_RIGHT] / sz.x, (float)window_h / sz.y));
	} else if (black_margin[MARGIN_RIGHT]) {
		WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, storage->resources.black_tex);

		draw_generic_textured_rect(Rect2(window_w - black_margin[MARGIN_RIGHT], 0, black_margin[MARGIN_RIGHT], window_h), Rect2(0, 0, 1, 1));
	}

	if (black_image[MARGIN_TOP].is_valid()) {
		_bind_canvas_texture(black_image[MARGIN_TOP], RID());

		Size2 sz(storage->texture_get_width(black_image[MARGIN_TOP]), storage->texture_get_height(black_image[MARGIN_TOP]));
		draw_generic_textured_rect(Rect2(0, 0, window_w, black_margin[MARGIN_TOP]),
				Rect2(0, 0, (float)window_w / sz.x, (float)black_margin[MARGIN_TOP] / sz.y));

	} else if (black_margin[MARGIN_TOP]) {
		WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, storage->resources.black_tex);

		draw_generic_textured_rect(Rect2(0, 0, window_w, black_margin[MARGIN_TOP]), Rect2(0, 0, 1, 1));
	}

	if (black_image[MARGIN_BOTTOM].is_valid()) {
		_bind_canvas_texture(black_image[MARGIN_BOTTOM], RID());

		Size2 sz(storage->texture_get_width(black_image[MARGIN_BOTTOM]), storage->texture_get_height(black_image[MARGIN_BOTTOM]));
		draw_generic_textured_rect(Rect2(0, window_h - black_margin[MARGIN_BOTTOM], window_w, black_margin[MARGIN_BOTTOM]),
				Rect2(0, 0, (float)window_w / sz.x, (float)black_margin[MARGIN_BOTTOM] / sz.y));

	} else if (black_margin[MARGIN_BOTTOM]) {
		WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, storage->resources.black_tex);

		draw_generic_textured_rect(Rect2(0, window_h - black_margin[MARGIN_BOTTOM], window_w, black_margin[MARGIN_BOTTOM]), Rect2(0, 0, 1, 1));
	}

	canvas_end();
}

void RasterizerCanvasBaseGLES2::_bind_quad_buffer() {
	storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.canvas_quad_vertices);
	glEnableVertexAttribArray(VS::ARRAY_VERTEX);
	glVertexAttribPointer(VS::ARRAY_VERTEX, 2, GL_FLOAT, GL_FALSE, 0, (void *)storage->gl_get_buffer_draw_ptr(GL_ARRAY_BUFFER));

	// Real array-fed white color for the quad's 4 vertices, matching
	// canvas_quad_vertices vertex-for-vertex -- kept enabled (not
	// glVertexAttrib4f-disabled) because this port's target driver
	// corrupts the framebuffer when a disabled attribute is written into
	// a varying, and canvas.glsl unconditionally does
	// color_interp = color_attrib regardless of draw mode. Callers of
	// this function (canvas_begin(), draw_lens_distortion_rect(), and the
	// textured-rect fast path in rasterizer_canvas_gles2.cpp) all draw
	// exactly these same 4 vertices, so a matching 4-entry buffer is
	// always correct for them.
	storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.canvas_quad_colors_white);
	glEnableVertexAttribArray(VS::ARRAY_COLOR);
	glVertexAttribPointer(VS::ARRAY_COLOR, 4, GL_FLOAT, GL_FALSE, 0, (void *)storage->gl_get_buffer_draw_ptr(GL_ARRAY_BUFFER));

	storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.canvas_quad_vertices);
}

void RasterizerCanvasBaseGLES2::_set_uniforms() {
	state.canvas_shader.set_uniform(CanvasShaderGLES2::PROJECTION_MATRIX, state.uniforms.projection_matrix);
	state.canvas_shader.set_uniform(CanvasShaderGLES2::MODELVIEW_MATRIX, state.uniforms.modelview_matrix);
	state.canvas_shader.set_uniform(CanvasShaderGLES2::EXTRA_MATRIX, state.uniforms.extra_matrix);

	state.canvas_shader.set_uniform(CanvasShaderGLES2::FINAL_MODULATE, state.uniforms.final_modulate);

	state.canvas_shader.set_uniform(CanvasShaderGLES2::TIME, storage->frame.time[0]);

	if (storage->frame.current_rt) {
		Vector2 screen_pixel_size;
		screen_pixel_size.x = 1.0 / storage->frame.current_rt->width;
		screen_pixel_size.y = 1.0 / storage->frame.current_rt->height;

		state.canvas_shader.set_uniform(CanvasShaderGLES2::SCREEN_PIXEL_SIZE, screen_pixel_size);
	}

	if (state.using_skeleton) {
		state.canvas_shader.set_uniform(CanvasShaderGLES2::SKELETON_TRANSFORM, state.skeleton_transform);
		state.canvas_shader.set_uniform(CanvasShaderGLES2::SKELETON_TRANSFORM_INVERSE, state.skeleton_transform_inverse);
		state.canvas_shader.set_uniform(CanvasShaderGLES2::SKELETON_TEXTURE_SIZE, state.skeleton_texture_size);
	}

	if (state.using_light) {
		Light *light = state.using_light;
		state.canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_MATRIX, light->light_shader_xform);
		Transform2D basis_inverse = light->light_shader_xform.affine_inverse().orthonormalized();
		basis_inverse[2] = Vector2();
		state.canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_MATRIX_INVERSE, basis_inverse);
		state.canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_LOCAL_MATRIX, light->xform_cache.affine_inverse());
		state.canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_COLOR, light->color * light->energy);
		state.canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_POS, light->light_shader_pos);
		state.canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_HEIGHT, light->height);
		state.canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_OUTSIDE_ALPHA, light->mode == VS::CANVAS_LIGHT_MODE_MASK ? 1.0 : 0.0);

		if (state.using_shadow) {
			RasterizerStorageGLES2::CanvasLightShadow *cls = storage->canvas_light_shadow_owner.get(light->shadow_buffer);
			WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0 + storage->config.max_texture_image_units - 5);
			glBindTexture(GL_TEXTURE_2D, cls->distance);
			state.canvas_shader.set_uniform(CanvasShaderGLES2::SHADOW_MATRIX, light->shadow_matrix_cache);
			state.canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_SHADOW_COLOR, light->shadow_color);

			state.canvas_shader.set_uniform(CanvasShaderGLES2::SHADOWPIXEL_SIZE, (1.0 / light->shadow_buffer_size) * (1.0 + light->shadow_smooth));
			if (light->radius_cache == 0) {
				state.canvas_shader.set_uniform(CanvasShaderGLES2::SHADOW_GRADIENT, 0.0);
			} else {
				state.canvas_shader.set_uniform(CanvasShaderGLES2::SHADOW_GRADIENT, light->shadow_gradient_length / (light->radius_cache * 1.1));
			}
			state.canvas_shader.set_uniform(CanvasShaderGLES2::SHADOW_DISTANCE_MULT, light->radius_cache * 1.1);

			/*canvas_shader.set_uniform(CanvasShaderGLES2::SHADOW_MATRIX,light->shadow_matrix_cache);
			canvas_shader.set_uniform(CanvasShaderGLES2::SHADOW_ESM_MULTIPLIER,light->shadow_esm_mult);
			canvas_shader.set_uniform(CanvasShaderGLES2::LIGHT_SHADOW_COLOR,light->shadow_color);*/
		}
	}
}

void RasterizerCanvasBaseGLES2::reset_canvas() {
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DITHER);
	glEnable(GL_BLEND);

	if (storage->frame.current_rt && storage->frame.current_rt->flags[RasterizerStorage::RENDER_TARGET_TRANSPARENT]) {
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	// bind the back buffer to a texture so shaders can use it.
	// It should probably use texture unit -3 (as GLES2 does as well) but currently that's buggy.
	// keeping this for now as there's nothing else that uses texture unit 2
	// TODO ^
	if (storage->frame.current_rt) {
		// glActiveTexture(GL_TEXTURE0 + 2);
		// glBindTexture(GL_TEXTURE_2D, storage->frame.current_rt->copy_screen_effect.color);
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void RasterizerCanvasBaseGLES2::canvas_debug_viewport_shadows(Light *p_lights_with_shadow) {
}

void RasterizerCanvasBaseGLES2::_copy_texscreen(const Rect2 &p_rect) {
	state.canvas_texscreen_used = true;

	_copy_screen(p_rect);

	// back to canvas, force rebind
	state.using_texture_rect = false;
	state.canvas_shader.bind();
	_bind_canvas_texture(state.current_tex, state.current_normal);
	_set_uniforms();
}

void RasterizerCanvasBaseGLES2::_draw_polygon(const int *p_indices, int p_index_count, int p_vertex_count, const Vector2 *p_vertices, const Vector2 *p_uvs, const Color *p_colors, bool p_singlecolor, const float *p_weights, const int *p_bones) {
	storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.polygon_buffer);
	// See rasterizer_storage_gles2.h: on ppc, gl_get_buffer_draw_ptr() returns
	// the real CPU-side staging pointer instead of a VBO-relative offset;
	// elsewhere it's nullptr, so this is exactly the buffer_ofs the code
	// already used and behaves identically to before.
	auto arr_ptr = [&](uint32_t p_off) -> void * {
		return (uint8_t *)((uintptr_t)storage->gl_get_buffer_draw_ptr(GL_ARRAY_BUFFER) + p_off);
	};

	uint32_t buffer_ofs = 0;
	uint32_t buffer_ofs_after = buffer_ofs + (sizeof(Vector2) * p_vertex_count);
#ifdef DEBUG_ENABLED
	ERR_FAIL_COND(buffer_ofs_after > data.polygon_buffer_size);
#endif

	storage->buffer_orphan_and_upload(data.polygon_buffer_size, 0, sizeof(Vector2) * p_vertex_count, p_vertices, GL_ARRAY_BUFFER, _buffer_upload_usage_flag, true);

	glEnableVertexAttribArray(VS::ARRAY_VERTEX);
	glVertexAttribPointer(VS::ARRAY_VERTEX, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(0));
	buffer_ofs = buffer_ofs_after;

	// On some drivers (e.g. the ATI Radeon X1900 / Mac OS X 10.4 Tiger GL
	// driver this port targets), writing a DISABLED vertex attribute --
	// one fed via glVertexAttrib* instead of a real per-vertex array --
	// into a varying corrupts the framebuffer. canvas.glsl unconditionally
	// does color_interp = color_attrib and uv_interp = uv_attrib, so a
	// flat-color or UV-less draw using glDisableVertexAttribArray here
	// would trip that bug. Always feed ARRAY_COLOR/ARRAY_TEX_UV from a
	// real per-vertex array (filled with the constant value when there's
	// no real per-vertex data) instead of disabling them.
	if (p_singlecolor || !p_colors) {
		Color m = p_singlecolor ? *p_colors : Color(1, 1, 1, 1);
		Color *color_fill = (Color *)alloca(sizeof(Color) * p_vertex_count);
		for (int i = 0; i < p_vertex_count; i++) {
			color_fill[i] = m;
		}
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Color) * p_vertex_count, color_fill, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_COLOR);
		glVertexAttribPointer(VS::ARRAY_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(Color), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	} else {
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Color) * p_vertex_count, p_colors, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_COLOR);
		glVertexAttribPointer(VS::ARRAY_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(Color), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	}

	if (p_uvs) {
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Vector2) * p_vertex_count, p_uvs, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_TEX_UV);
		glVertexAttribPointer(VS::ARRAY_TEX_UV, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	} else {
		Vector2 *uv_fill = (Vector2 *)alloca(sizeof(Vector2) * p_vertex_count);
		for (int i = 0; i < p_vertex_count; i++) {
			uv_fill[i] = Vector2();
		}
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Vector2) * p_vertex_count, uv_fill, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_TEX_UV);
		glVertexAttribPointer(VS::ARRAY_TEX_UV, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	}

	if (p_weights && p_bones) {
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(float) * 4 * p_vertex_count, p_weights, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_WEIGHTS);
		glVertexAttribPointer(VS::ARRAY_WEIGHTS, 4, GL_FLOAT, GL_FALSE, sizeof(float) * 4, arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;

		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(int) * 4 * p_vertex_count, p_bones, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_BONES);
		glVertexAttribPointer(VS::ARRAY_BONES, 4, GL_UNSIGNED_INT, GL_FALSE, sizeof(int) * 4, arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;

	} else {
		glDisableVertexAttribArray(VS::ARRAY_WEIGHTS);
		glDisableVertexAttribArray(VS::ARRAY_BONES);
	}

	storage->gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, data.polygon_index_buffer);
	auto idx_ptr = [&]() -> void * {
		return (void *)storage->gl_get_buffer_draw_ptr(GL_ELEMENT_ARRAY_BUFFER);
	};

	if (storage->config.support_32_bits_indices) { //should check for
#ifdef DEBUG_ENABLED
		ERR_FAIL_COND((sizeof(int) * p_index_count) > data.polygon_index_buffer_size);
#endif
		storage->buffer_orphan_and_upload(data.polygon_index_buffer_size, 0, sizeof(int) * p_index_count, p_indices, GL_ELEMENT_ARRAY_BUFFER, _buffer_upload_usage_flag, true);
		glDrawElements(GL_TRIANGLES, p_index_count, GL_UNSIGNED_INT, idx_ptr());
		storage->info.render._2d_draw_call_count++;
	} else {
#ifdef DEBUG_ENABLED
		ERR_FAIL_COND((sizeof(uint16_t) * p_index_count) > data.polygon_index_buffer_size);
#endif
		uint16_t *index16 = (uint16_t *)alloca(sizeof(uint16_t) * p_index_count);
		for (int i = 0; i < p_index_count; i++) {
			index16[i] = uint16_t(p_indices[i]);
		}
		storage->buffer_orphan_and_upload(data.polygon_index_buffer_size, 0, sizeof(uint16_t) * p_index_count, index16, GL_ELEMENT_ARRAY_BUFFER, _buffer_upload_usage_flag, true);
		glDrawElements(GL_TRIANGLES, p_index_count, GL_UNSIGNED_SHORT, idx_ptr());
		storage->info.render._2d_draw_call_count++;
	}

	storage->gl_bind_buffer(GL_ARRAY_BUFFER, 0);
	storage->gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void RasterizerCanvasBaseGLES2::_draw_generic(GLuint p_primitive, int p_vertex_count, const Vector2 *p_vertices, const Vector2 *p_uvs, const Color *p_colors, bool p_singlecolor) {
	storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.polygon_buffer);
	auto arr_ptr = [&](uint32_t p_off) -> void * {
		return (uint8_t *)((uintptr_t)storage->gl_get_buffer_draw_ptr(GL_ARRAY_BUFFER) + p_off);
	};

	uint32_t buffer_ofs = 0;
	uint32_t buffer_ofs_after = buffer_ofs + (sizeof(Vector2) * p_vertex_count);
#ifdef DEBUG_ENABLED
	ERR_FAIL_COND(buffer_ofs_after > data.polygon_buffer_size);
#endif
	storage->buffer_orphan_and_upload(data.polygon_buffer_size, 0, sizeof(Vector2) * p_vertex_count, p_vertices, GL_ARRAY_BUFFER, _buffer_upload_usage_flag, true);

	glEnableVertexAttribArray(VS::ARRAY_VERTEX);
	glVertexAttribPointer(VS::ARRAY_VERTEX, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(0));
	buffer_ofs = buffer_ofs_after;

	// Same fix as _draw_polygon() above: never disable ARRAY_COLOR /
	// ARRAY_TEX_UV for flat-color / UV-less draws -- feed them from a
	// real per-vertex array instead, to avoid the disabled-attribute-
	// into-varying framebuffer corruption on this port's target driver.
	if (p_singlecolor || !p_colors) {
		Color m = p_singlecolor ? *p_colors : Color(1, 1, 1, 1);
		Color *color_fill = (Color *)alloca(sizeof(Color) * p_vertex_count);
		for (int i = 0; i < p_vertex_count; i++) {
			color_fill[i] = m;
		}
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Color) * p_vertex_count, color_fill, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_COLOR);
		glVertexAttribPointer(VS::ARRAY_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(Color), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	} else {
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Color) * p_vertex_count, p_colors, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_COLOR);
		glVertexAttribPointer(VS::ARRAY_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(Color), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	}

	if (p_uvs) {
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Vector2) * p_vertex_count, p_uvs, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_TEX_UV);
		glVertexAttribPointer(VS::ARRAY_TEX_UV, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	} else {
		Vector2 *uv_fill = (Vector2 *)alloca(sizeof(Vector2) * p_vertex_count);
		for (int i = 0; i < p_vertex_count; i++) {
			uv_fill[i] = Vector2();
		}
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Vector2) * p_vertex_count, uv_fill, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_TEX_UV);
		glVertexAttribPointer(VS::ARRAY_TEX_UV, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	}

	glDrawArrays(p_primitive, 0, p_vertex_count);
	storage->info.render._2d_draw_call_count++;

	storage->gl_bind_buffer(GL_ARRAY_BUFFER, 0);
}

void RasterizerCanvasBaseGLES2::_draw_generic_indices(GLuint p_primitive, const int *p_indices, int p_index_count, int p_vertex_count, const Vector2 *p_vertices, const Vector2 *p_uvs, const Color *p_colors, bool p_singlecolor) {
	storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.polygon_buffer);
	auto arr_ptr = [&](uint32_t p_off) -> void * {
		return (uint8_t *)((uintptr_t)storage->gl_get_buffer_draw_ptr(GL_ARRAY_BUFFER) + p_off);
	};

	uint32_t buffer_ofs = 0;
	uint32_t buffer_ofs_after = buffer_ofs + (sizeof(Vector2) * p_vertex_count);
#ifdef DEBUG_ENABLED
	ERR_FAIL_COND(buffer_ofs_after > data.polygon_buffer_size);
#endif
	storage->buffer_orphan_and_upload(data.polygon_buffer_size, 0, sizeof(Vector2) * p_vertex_count, p_vertices, GL_ARRAY_BUFFER, _buffer_upload_usage_flag, true);

	glEnableVertexAttribArray(VS::ARRAY_VERTEX);
	glVertexAttribPointer(VS::ARRAY_VERTEX, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(0));
	buffer_ofs = buffer_ofs_after;

	// Same fix as _draw_polygon() above: never disable ARRAY_COLOR /
	// ARRAY_TEX_UV for flat-color / UV-less draws -- feed them from a
	// real per-vertex array instead, to avoid the disabled-attribute-
	// into-varying framebuffer corruption on this port's target driver.
	if (p_singlecolor || !p_colors) {
		Color m = p_singlecolor ? *p_colors : Color(1, 1, 1, 1);
		Color *color_fill = (Color *)alloca(sizeof(Color) * p_vertex_count);
		for (int i = 0; i < p_vertex_count; i++) {
			color_fill[i] = m;
		}
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Color) * p_vertex_count, color_fill, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_COLOR);
		glVertexAttribPointer(VS::ARRAY_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(Color), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	} else {
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Color) * p_vertex_count, p_colors, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_COLOR);
		glVertexAttribPointer(VS::ARRAY_COLOR, 4, GL_FLOAT, GL_FALSE, sizeof(Color), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	}

	if (p_uvs) {
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Vector2) * p_vertex_count, p_uvs, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_TEX_UV);
		glVertexAttribPointer(VS::ARRAY_TEX_UV, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	} else {
		Vector2 *uv_fill = (Vector2 *)alloca(sizeof(Vector2) * p_vertex_count);
		for (int i = 0; i < p_vertex_count; i++) {
			uv_fill[i] = Vector2();
		}
		RAST_FAIL_COND(!storage->safe_buffer_sub_data(data.polygon_buffer_size, GL_ARRAY_BUFFER, buffer_ofs, sizeof(Vector2) * p_vertex_count, uv_fill, buffer_ofs_after));
		glEnableVertexAttribArray(VS::ARRAY_TEX_UV);
		glVertexAttribPointer(VS::ARRAY_TEX_UV, 2, GL_FLOAT, GL_FALSE, sizeof(Vector2), arr_ptr(buffer_ofs));
		buffer_ofs = buffer_ofs_after;
	}

#ifdef RASTERIZER_EXTRA_CHECKS
	// very slow, do not enable in normal use
	for (int n = 0; n < p_index_count; n++) {
		RAST_DEV_DEBUG_ASSERT(p_indices[n] < p_vertex_count);
	}
#endif

	storage->gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, data.polygon_index_buffer);
	auto idx_ptr = [&]() -> void * {
		return (void *)storage->gl_get_buffer_draw_ptr(GL_ELEMENT_ARRAY_BUFFER);
	};

	if (storage->config.support_32_bits_indices) { //should check for
#ifdef DEBUG_ENABLED
		ERR_FAIL_COND((sizeof(int) * p_index_count) > data.polygon_index_buffer_size);
#endif
		storage->buffer_orphan_and_upload(data.polygon_index_buffer_size, 0, sizeof(int) * p_index_count, p_indices, GL_ELEMENT_ARRAY_BUFFER, _buffer_upload_usage_flag, true);
		glDrawElements(p_primitive, p_index_count, GL_UNSIGNED_INT, idx_ptr());
		storage->info.render._2d_draw_call_count++;
	} else {
#ifdef DEBUG_ENABLED
		ERR_FAIL_COND((sizeof(uint16_t) * p_index_count) > data.polygon_index_buffer_size);
#endif
		uint16_t *index16 = (uint16_t *)alloca(sizeof(uint16_t) * p_index_count);
		for (int i = 0; i < p_index_count; i++) {
			index16[i] = uint16_t(p_indices[i]);
		}
		storage->buffer_orphan_and_upload(data.polygon_index_buffer_size, 0, sizeof(uint16_t) * p_index_count, index16, GL_ELEMENT_ARRAY_BUFFER, _buffer_upload_usage_flag, true);
		glDrawElements(p_primitive, p_index_count, GL_UNSIGNED_SHORT, idx_ptr());
		storage->info.render._2d_draw_call_count++;
	}

	storage->gl_bind_buffer(GL_ARRAY_BUFFER, 0);
	storage->gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void RasterizerCanvasBaseGLES2::_draw_gui_primitive(int p_points, const Vector2 *p_vertices, const Color *p_colors, const Vector2 *p_uvs, const float *p_light_angles) {
	static const GLenum prim[5] = { GL_POINTS, GL_POINTS, GL_LINES, GL_TRIANGLES, GL_TRIANGLE_FAN };

	// ARRAY_COLOR and ARRAY_TEX_UV are always reserved and always fed from
	// a real per-vertex array below (filled with a constant when there's
	// no real per-vertex data) rather than being left disabled / at
	// whatever ambient state a previous draw left them in -- see the
	// comment in _draw_polygon() above for why: this port's target driver
	// corrupts the framebuffer when a disabled attribute is written into
	// a varying, and canvas.glsl unconditionally does
	// color_interp = color_attrib.
	int color_offset = 2;
	int uv_offset = color_offset + 4;
	int light_angle_offset = uv_offset + 2;
	int stride = uv_offset + 2;

	if (p_light_angles) { //light_angles
		stride += 1;
	}

	DEV_ASSERT(p_points <= 4);
	float buffer_data[(2 + 2 + 4 + 1) * 4];

	for (int i = 0; i < p_points; i++) {
		buffer_data[stride * i + 0] = p_vertices[i].x;
		buffer_data[stride * i + 1] = p_vertices[i].y;
	}

	if (p_colors) {
		for (int i = 0; i < p_points; i++) {
			buffer_data[stride * i + color_offset + 0] = p_colors[i].r;
			buffer_data[stride * i + color_offset + 1] = p_colors[i].g;
			buffer_data[stride * i + color_offset + 2] = p_colors[i].b;
			buffer_data[stride * i + color_offset + 3] = p_colors[i].a;
		}
	} else {
		for (int i = 0; i < p_points; i++) {
			buffer_data[stride * i + color_offset + 0] = 1.0f;
			buffer_data[stride * i + color_offset + 1] = 1.0f;
			buffer_data[stride * i + color_offset + 2] = 1.0f;
			buffer_data[stride * i + color_offset + 3] = 1.0f;
		}
	}

	if (p_uvs) {
		for (int i = 0; i < p_points; i++) {
			buffer_data[stride * i + uv_offset + 0] = p_uvs[i].x;
			buffer_data[stride * i + uv_offset + 1] = p_uvs[i].y;
		}
	} else {
		for (int i = 0; i < p_points; i++) {
			buffer_data[stride * i + uv_offset + 0] = 0.0f;
			buffer_data[stride * i + uv_offset + 1] = 0.0f;
		}
	}

	if (p_light_angles) {
		for (int i = 0; i < p_points; i++) {
			buffer_data[stride * i + light_angle_offset + 0] = p_light_angles[i];
		}
	}

	storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.polygon_buffer);
	storage->buffer_orphan_and_upload(data.polygon_buffer_size, 0, p_points * stride * sizeof(float), buffer_data, GL_ARRAY_BUFFER, _buffer_upload_usage_flag, true);
	auto arr_ptr = [&](uint32_t p_off) -> void * {
		return (uint8_t *)((uintptr_t)storage->gl_get_buffer_draw_ptr(GL_ARRAY_BUFFER) + p_off);
	};

	glVertexAttribPointer(VS::ARRAY_VERTEX, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), arr_ptr(0));

	glVertexAttribPointer(VS::ARRAY_COLOR, 4, GL_FLOAT, GL_FALSE, stride * sizeof(float), arr_ptr(color_offset * sizeof(float)));
	glEnableVertexAttribArray(VS::ARRAY_COLOR);

	glVertexAttribPointer(VS::ARRAY_TEX_UV, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), arr_ptr(uv_offset * sizeof(float)));
	glEnableVertexAttribArray(VS::ARRAY_TEX_UV);

	if (p_light_angles) {
		glVertexAttribPointer(VS::ARRAY_TANGENT, 1, GL_FLOAT, GL_FALSE, stride * sizeof(float), arr_ptr(light_angle_offset * sizeof(float)));
		glEnableVertexAttribArray(VS::ARRAY_TANGENT);
	}

	glDrawArrays(prim[p_points], 0, p_points);
	storage->info.render._2d_draw_call_count++;

	if (p_light_angles) {
		// may not be needed
		glDisableVertexAttribArray(VS::ARRAY_TANGENT);
	}

	storage->gl_bind_buffer(GL_ARRAY_BUFFER, 0);
}

void RasterizerCanvasBaseGLES2::_bind_constant_vertex_attrib(int p_location, int p_components, const float *p_value, int p_count) {
	Vector<float> fill;
	fill.resize(p_components * p_count);
	float *w = fill.ptrw();
	for (int i = 0; i < p_count; i++) {
		for (int c = 0; c < p_components; c++) {
			w[i * p_components + c] = p_value[c];
		}
	}

	uint32_t bytes = sizeof(float) * p_components * p_count;
	storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.const_fill_buffer);
	storage->buffer_orphan_and_upload(bytes, 0, bytes, fill.ptr(), GL_ARRAY_BUFFER, GL_STREAM_DRAW);
	glEnableVertexAttribArray(p_location);
	glVertexAttribPointer(p_location, p_components, GL_FLOAT, GL_FALSE, 0, (void *)storage->gl_get_buffer_draw_ptr(GL_ARRAY_BUFFER));
}

void RasterizerCanvasBaseGLES2::_copy_screen(const Rect2 &p_rect) {
	if (storage->frame.current_rt->flags[RasterizerStorage::RENDER_TARGET_DIRECT_TO_SCREEN]) {
		ERR_PRINT_ONCE("Cannot use screen texture copying in render target set to render direct to screen.");
		return;
	}

	ERR_FAIL_COND_MSG(storage->frame.current_rt->copy_screen_effect.color == 0, "Can't use screen texture copying in a render target configured without copy buffers. To resolve this, change the viewport's Usage property to \"2D\" or \"3D\" instead of \"2D Without Sampling\" or \"3D Without Effects\" respectively.");

	glDisable(GL_BLEND);

	Vector2 wh(storage->frame.current_rt->width, storage->frame.current_rt->height);

	Color copy_section(p_rect.position.x / wh.x, p_rect.position.y / wh.y, p_rect.size.x / wh.x, p_rect.size.y / wh.y);

	if (p_rect != Rect2()) {
		storage->shaders.copy.set_conditional(CopyShaderGLES2::USE_COPY_SECTION, true);
	}

	storage->shaders.copy.set_conditional(CopyShaderGLES2::USE_NO_ALPHA, !state.using_transparent_rt);

	glBindFramebuffer(GL_FRAMEBUFFER, storage->frame.current_rt->copy_screen_effect.fbo);
	WRAPPED_GL_ACTIVE_TEXTURE(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, storage->frame.current_rt->color);

	storage->shaders.copy.bind();
	storage->shaders.copy.set_uniform(CopyShaderGLES2::COPY_SECTION, copy_section);

	const Vector2 vertpos[4] = {
		Vector2(-1, -1),
		Vector2(-1, 1),
		Vector2(1, 1),
		Vector2(1, -1),
	};

	const Vector2 uvpos[4] = {
		Vector2(0, 0),
		Vector2(0, 1),
		Vector2(1, 1),
		Vector2(1, 0)
	};

	const int indexpos[6] = {
		0, 1, 2,
		2, 3, 0
	};

	_draw_polygon(indexpos, 6, 4, vertpos, uvpos, nullptr, false);

	storage->shaders.copy.set_conditional(CopyShaderGLES2::USE_COPY_SECTION, false);
	storage->shaders.copy.set_conditional(CopyShaderGLES2::USE_NO_ALPHA, false);

	glBindFramebuffer(GL_FRAMEBUFFER, storage->frame.current_rt->fbo); //back to front
	glEnable(GL_BLEND);
}

void RasterizerCanvasBaseGLES2::canvas_light_shadow_buffer_update(RID p_buffer, const Transform2D &p_light_xform, int p_light_mask, float p_near, float p_far, LightOccluderInstance *p_occluders, CameraMatrix *p_xform_cache) {
	RasterizerStorageGLES2::CanvasLightShadow *cls = storage->canvas_light_shadow_owner.get(p_buffer);
	ERR_FAIL_COND(!cls);

	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DITHER);
	glDisable(GL_CULL_FACE);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_DEPTH_TEST);
	glDepthMask(true);

	glBindFramebuffer(GL_FRAMEBUFFER, cls->fbo);

	state.canvas_shadow_shader.set_conditional(CanvasShadowShaderGLES2::USE_RGBA_SHADOWS, storage->config.use_rgba_2d_shadows);
	state.canvas_shadow_shader.bind();

	glViewport(0, 0, cls->size, cls->height);
	glClearDepth(1.0f);
	glClearColor(1, 1, 1, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	VS::CanvasOccluderPolygonCullMode cull = VS::CANVAS_OCCLUDER_POLYGON_CULL_DISABLED;

	for (int i = 0; i < 4; i++) {
		//make sure it remains orthogonal, makes easy to read angle later

		Transform light;
		light.origin[0] = p_light_xform[2][0];
		light.origin[1] = p_light_xform[2][1];
		light.basis[0][0] = p_light_xform[0][0];
		light.basis[0][1] = p_light_xform[1][0];
		light.basis[1][0] = p_light_xform[0][1];
		light.basis[1][1] = p_light_xform[1][1];

		//light.basis.scale(Vector3(to_light.elements[0].length(),to_light.elements[1].length(),1));

		//p_near=1;
		CameraMatrix projection;
		{
			real_t fov = 90;
			real_t nearp = p_near;
			real_t farp = p_far;
			real_t aspect = 1.0;

			real_t ymax = nearp * Math::tan(Math::deg2rad(fov * 0.5));
			real_t ymin = -ymax;
			real_t xmin = ymin * aspect;
			real_t xmax = ymax * aspect;

			projection.set_frustum(xmin, xmax, ymin, ymax, nearp, farp);
		}

		Vector3 cam_target = Basis(Vector3(0, 0, Math_PI * 2 * (i / 4.0))).xform(Vector3(0, 1, 0));
		projection = projection * CameraMatrix(Transform().looking_at(cam_target, Vector3(0, 0, -1)).affine_inverse());

		state.canvas_shadow_shader.set_uniform(CanvasShadowShaderGLES2::PROJECTION_MATRIX, projection);
		state.canvas_shadow_shader.set_uniform(CanvasShadowShaderGLES2::LIGHT_MATRIX, light);
		state.canvas_shadow_shader.set_uniform(CanvasShadowShaderGLES2::DISTANCE_NORM, 1.0 / p_far);

		if (i == 0) {
			*p_xform_cache = projection;
		}

		glViewport(0, (cls->height / 4) * i, cls->size, cls->height / 4);

		LightOccluderInstance *instance = p_occluders;

		while (instance) {
			RasterizerStorageGLES2::CanvasOccluder *cc = storage->canvas_occluder_owner.getornull(instance->polygon_buffer);
			if (!cc || cc->len == 0 || !(p_light_mask & instance->light_mask)) {
				instance = instance->next;
				continue;
			}

			state.canvas_shadow_shader.set_uniform(CanvasShadowShaderGLES2::WORLD_MATRIX, instance->xform_cache);

			VS::CanvasOccluderPolygonCullMode transformed_cull_cache = instance->cull_cache;

			if (transformed_cull_cache != VS::CANVAS_OCCLUDER_POLYGON_CULL_DISABLED &&
					(p_light_xform.determinant() * instance->xform_cache.determinant()) < 0) {
				transformed_cull_cache = (transformed_cull_cache == VS::CANVAS_OCCLUDER_POLYGON_CULL_CLOCKWISE)
						? VS::CANVAS_OCCLUDER_POLYGON_CULL_COUNTER_CLOCKWISE
						: VS::CANVAS_OCCLUDER_POLYGON_CULL_CLOCKWISE;
			}

			if (cull != transformed_cull_cache) {
				cull = transformed_cull_cache;
				switch (cull) {
					case VS::CANVAS_OCCLUDER_POLYGON_CULL_DISABLED: {
						glDisable(GL_CULL_FACE);

					} break;
					case VS::CANVAS_OCCLUDER_POLYGON_CULL_CLOCKWISE: {
						glEnable(GL_CULL_FACE);
						glCullFace(GL_FRONT);
					} break;
					case VS::CANVAS_OCCLUDER_POLYGON_CULL_COUNTER_CLOCKWISE: {
						glEnable(GL_CULL_FACE);
						glCullFace(GL_BACK);

					} break;
				}
			}

			glBindBuffer(GL_ARRAY_BUFFER, cc->vertex_id);
			glEnableVertexAttribArray(VS::ARRAY_VERTEX);
			glVertexAttribPointer(VS::ARRAY_VERTEX, 3, GL_FLOAT, false, 0, nullptr);
			glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cc->index_id);

			glDrawElements(GL_TRIANGLES, cc->len * 3, GL_UNSIGNED_SHORT, nullptr);

			instance = instance->next;
		}
	}

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void RasterizerCanvasBaseGLES2::draw_lens_distortion_rect(const Rect2 &p_rect, float p_k1, float p_k2, const Vector2 &p_eye_center, float p_oversample) {
	Vector2 half_size;
	if (storage->frame.current_rt) {
		half_size = Vector2(storage->frame.current_rt->width, storage->frame.current_rt->height);
	} else {
		half_size = OS::get_singleton()->get_window_size();
	}
	half_size *= 0.5;
	Vector2 offset((p_rect.position.x - half_size.x) / half_size.x, (p_rect.position.y - half_size.y) / half_size.y);
	Vector2 scale(p_rect.size.x / half_size.x, p_rect.size.y / half_size.y);

	float aspect_ratio = p_rect.size.x / p_rect.size.y;

	// setup our lens shader
	state.lens_shader.bind();
	state.lens_shader.set_uniform(LensDistortedShaderGLES2::OFFSET, offset);
	state.lens_shader.set_uniform(LensDistortedShaderGLES2::SCALE, scale);
	state.lens_shader.set_uniform(LensDistortedShaderGLES2::K1, p_k1);
	state.lens_shader.set_uniform(LensDistortedShaderGLES2::K2, p_k2);
	state.lens_shader.set_uniform(LensDistortedShaderGLES2::EYE_CENTER, p_eye_center);
	state.lens_shader.set_uniform(LensDistortedShaderGLES2::UPSCALE, p_oversample);
	state.lens_shader.set_uniform(LensDistortedShaderGLES2::ASPECT_RATIO, aspect_ratio);

	// bind our quad buffer
	_bind_quad_buffer();

	// and draw
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	// and cleanup
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	for (int i = 0; i < VS::ARRAY_MAX; i++) {
		glDisableVertexAttribArray(i);
	}
}

void RasterizerCanvasBaseGLES2::initialize() {
	int flag_stream_mode = GLOBAL_GET("rendering/2d/opengl/legacy_stream");
	switch (flag_stream_mode) {
		default: {
			_buffer_upload_usage_flag = GL_STREAM_DRAW;
		} break;
		case 1: {
			_buffer_upload_usage_flag = GL_DYNAMIC_DRAW;
		} break;
		case 2: {
			_buffer_upload_usage_flag = GL_STREAM_DRAW;
		} break;
	}

	// quad buffer
	{
		glGenBuffers(1, &data.canvas_quad_vertices);
		storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.canvas_quad_vertices);

		const float qv[8] = {
			0, 0,
			0, 1,
			1, 1,
			1, 0
		};

		storage->buffer_orphan_and_upload(sizeof(float) * 8, 0, sizeof(float) * 8, qv, GL_ARRAY_BUFFER, GL_STATIC_DRAW);

		storage->gl_bind_buffer(GL_ARRAY_BUFFER, 0);

		// Companion real (array-fed, not disabled) white color for the
		// quad's 4 vertices -- see the Data::canvas_quad_colors_white
		// comment in the header for why this exists.
		glGenBuffers(1, &data.canvas_quad_colors_white);
		storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.canvas_quad_colors_white);

		const float qc[16] = {
			1, 1, 1, 1,
			1, 1, 1, 1,
			1, 1, 1, 1,
			1, 1, 1, 1
		};

		storage->buffer_orphan_and_upload(sizeof(float) * 16, 0, sizeof(float) * 16, qc, GL_ARRAY_BUFFER, GL_STATIC_DRAW);

		storage->gl_bind_buffer(GL_ARRAY_BUFFER, 0);
	}

	// scratch buffer for _bind_constant_vertex_attrib()
	glGenBuffers(1, &data.const_fill_buffer);

	// polygon buffer
	{
		uint32_t poly_size = GLOBAL_DEF("rendering/limits/buffers/canvas_polygon_buffer_size_kb", 128);
		ProjectSettings::get_singleton()->set_custom_property_info("rendering/limits/buffers/canvas_polygon_buffer_size_kb", PropertyInfo(Variant::INT, "rendering/limits/buffers/canvas_polygon_buffer_size_kb", PROPERTY_HINT_RANGE, "0,256,1,or_greater"));
		poly_size = MAX(poly_size, 2); // minimum 2k, may still see anomalies in editor
		poly_size *= 1024;
		glGenBuffers(1, &data.polygon_buffer);
		storage->gl_bind_buffer(GL_ARRAY_BUFFER, data.polygon_buffer);
		storage->buffer_orphan_and_upload(poly_size, 0, 0, nullptr, GL_ARRAY_BUFFER, GL_DYNAMIC_DRAW);

		data.polygon_buffer_size = poly_size;

		storage->gl_bind_buffer(GL_ARRAY_BUFFER, 0);

		uint32_t index_size = GLOBAL_DEF("rendering/limits/buffers/canvas_polygon_index_buffer_size_kb", 128);
		ProjectSettings::get_singleton()->set_custom_property_info("rendering/limits/buffers/canvas_polygon_index_buffer_size_kb", PropertyInfo(Variant::INT, "rendering/limits/buffers/canvas_polygon_index_buffer_size_kb", PROPERTY_HINT_RANGE, "0,256,1,or_greater"));
		index_size = MAX(index_size, 2);
		index_size *= 1024; // kb
		glGenBuffers(1, &data.polygon_index_buffer);
		storage->gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, data.polygon_index_buffer);
		storage->buffer_orphan_and_upload(index_size, 0, 0, nullptr, GL_ELEMENT_ARRAY_BUFFER, GL_DYNAMIC_DRAW);
		storage->gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);

		data.polygon_index_buffer_size = index_size;
	}

	// ninepatch buffers
	{
		// array buffer -- content is fully re-uploaded (via
		// storage->buffer_orphan_and_upload()) before every draw in the
		// TYPE_NINEPATCH case in rasterizer_canvas_gles2.cpp, so no initial
		// reservation upload is needed here, just the buffer name.
		glGenBuffers(1, &data.ninepatch_vertices);

		// element buffer -- unlike the vertex buffer above, this is
		// uploaded ONCE here and only re-bound (never re-uploaded) at draw
		// time, so it MUST go through storage->gl_bind_buffer()/
		// buffer_orphan_and_upload() rather than raw glBindBuffer()/
		// glBufferData(): on ppc those wrapped calls are what actually
		// populate the CPU-side staging buffer this id's draw-time index
		// pointer is read from (see gl_bind_buffer()'s declaration
		// comment) -- a raw upload here would leave that staging buffer
		// permanently empty, so every glDrawElements() using it would
		// silently read zeroed/garbage indices.
		glGenBuffers(1, &data.ninepatch_elements);
		storage->gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, data.ninepatch_elements);

#define _EIDX(y, x) (y * 4 + x)
		uint8_t elems[3 * 2 * 9] = {

			// first row

			_EIDX(0, 0), _EIDX(0, 1), _EIDX(1, 1),
			_EIDX(1, 1), _EIDX(1, 0), _EIDX(0, 0),

			_EIDX(0, 1), _EIDX(0, 2), _EIDX(1, 2),
			_EIDX(1, 2), _EIDX(1, 1), _EIDX(0, 1),

			_EIDX(0, 2), _EIDX(0, 3), _EIDX(1, 3),
			_EIDX(1, 3), _EIDX(1, 2), _EIDX(0, 2),

			// second row

			_EIDX(1, 0), _EIDX(1, 1), _EIDX(2, 1),
			_EIDX(2, 1), _EIDX(2, 0), _EIDX(1, 0),

			// the center one would be here, but we'll put it at the end
			// so it's easier to disable the center and be able to use
			// one draw call for both

			_EIDX(1, 2), _EIDX(1, 3), _EIDX(2, 3),
			_EIDX(2, 3), _EIDX(2, 2), _EIDX(1, 2),

			// third row

			_EIDX(2, 0), _EIDX(2, 1), _EIDX(3, 1),
			_EIDX(3, 1), _EIDX(3, 0), _EIDX(2, 0),

			_EIDX(2, 1), _EIDX(2, 2), _EIDX(3, 2),
			_EIDX(3, 2), _EIDX(3, 1), _EIDX(2, 1),

			_EIDX(2, 2), _EIDX(2, 3), _EIDX(3, 3),
			_EIDX(3, 3), _EIDX(3, 2), _EIDX(2, 2),

			// center field

			_EIDX(1, 1), _EIDX(1, 2), _EIDX(2, 2),
			_EIDX(2, 2), _EIDX(2, 1), _EIDX(1, 1)
		};
#undef _EIDX

		storage->buffer_orphan_and_upload(sizeof(elems), 0, sizeof(elems), elems, GL_ELEMENT_ARRAY_BUFFER, GL_STATIC_DRAW);

		storage->gl_bind_buffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}

	state.canvas_shadow_shader.init();

	state.canvas_shader.init();

	state.using_light_angle = false;
	state.using_large_vertex = false;
	state.using_modulate = false;

	_set_texture_rect_mode(true);
	state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_RGBA_SHADOWS, storage->config.use_rgba_2d_shadows);

	state.canvas_shader.bind();

	state.lens_shader.init();

	state.canvas_shader.set_conditional(CanvasShaderGLES2::USE_PIXEL_SNAP, GLOBAL_DEF("rendering/2d/snapping/use_gpu_pixel_snap", false));

	state.using_light = nullptr;
	state.using_transparent_rt = false;
	state.using_skeleton = false;
}

void RasterizerCanvasBaseGLES2::finalize() {
}

RasterizerCanvasBaseGLES2::RasterizerCanvasBaseGLES2() {
}
