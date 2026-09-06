#include "rasterizer_canvas_glff.h"

#include "core/os/os.h"
#include "rasterizer_storage_glff.h"

void RasterizerCanvasGLFF::canvas_begin() {
	// A plain glLoadIdentity() here left the projection matrix mapping only
	// NDC [-1,1] -- real CanvasItem geometry is in pixel coordinates (e.g.
	// a 16x16 icon at (625,5)), so it was being clipped away entirely, not
	// just misplaced. This maps window pixel space to NDC directly, with Y
	// flipped since screen space grows downward and GL NDC grows upward.
	Size2 window_size = OS::get_singleton()->get_window_size();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, window_size.width, window_size.height, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);
}

void RasterizerCanvasGLFF::canvas_end() {
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisable(GL_SCISSOR_TEST);
}

void RasterizerCanvasGLFF::reset_canvas() {
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_BLEND);
}

void RasterizerCanvasGLFF::gl_enable_scissor(int p_x, int p_y, int p_width, int p_height) const {
	glEnable(GL_SCISSOR_TEST);
	glScissor(p_x, p_y, p_width, p_height);
}

void RasterizerCanvasGLFF::gl_disable_scissor() const {
	glDisable(GL_SCISSOR_TEST);
}

void RasterizerCanvasGLFF::canvas_render_items(Item *p_item_list, int p_z, const Color &p_modulate, Light *p_light, const Transform2D &p_base_transform) {
	batch_canvas_render_items(p_item_list, p_z, p_modulate, p_light, p_base_transform);
}

// Converts a 2D affine transform to a 4x4 GL matrix embedding it in the XY
// plane (Z untouched, W=1) -- Transform2D::elements[0]/[1] are the X/Y
// basis column vectors, elements[2] is the origin (core/math/transform_2d.h),
// matching GL's column-major glLoadMatrixf layout directly.
static void _load_transform2d_gl(const Transform2D &p_transform) {
	GLfloat gl[16];
	gl[0] = p_transform.elements[0].x;
	gl[1] = p_transform.elements[0].y;
	gl[2] = 0;
	gl[3] = 0;
	gl[4] = p_transform.elements[1].x;
	gl[5] = p_transform.elements[1].y;
	gl[6] = 0;
	gl[7] = 0;
	gl[8] = 0;
	gl[9] = 0;
	gl[10] = 1;
	gl[11] = 0;
	gl[12] = p_transform.elements[2].x;
	gl[13] = p_transform.elements[2].y;
	gl[14] = 0;
	gl[15] = 1;
	glLoadMatrixf(gl);
}

// godot-ports#27: Light2D as a multiplicative/additive light-shape-texture
// blend -- draw the light's own texture (a radial gradient for a point
// light, a cone for a spotlight-style light, whatever the artist assigned)
// as an ordinary textured quad positioned/rotated by the light's real
// transform, blended over the already-drawn opaque 2D scene according to
// its VS::CanvasLightMode. This is a second, separate pass over p_light's
// linked list -- not a real per-pixel lighting model (no per-fragment math
// exists in fixed-function), but a close visual approximation for this
// backend's common use cases (torches, glows, flashlights) per the issue's
// own hypothesis.
//
// Deliberately NOT implemented in this first pass (see godot-ports#27's
// success criteria, which explicitly defers this): LightOccluder2D shadow
// casting, CANVAS_LIGHT_MODE_MASK (needs a real stencil/alpha-mask
// multiply pass, a bigger step up than ADD/SUB/MIX), and Light::
// texture_offset (assumes the light texture is centered on the light's
// origin, true for every standard Godot light-shape texture but not a
// sub-rect atlas case).
static void _draw_canvas_light(RasterizerStorageGLFF *p_storage, RasterizerCanvas::Light *p_light) {
	if (!p_light->enabled || !p_light->texture.is_valid()) {
		return;
	}
	RasterizerStorageGLFF::Texture *tex = p_storage->texture_owner.getornull(p_light->texture);
	if (tex) {
		tex = tex->get_ptr();
	}
	if (!tex) {
		return;
	}

	switch (p_light->mode) {
		case VS::CANVAS_LIGHT_MODE_ADD:
			glBlendEquation(GL_FUNC_ADD);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		case VS::CANVAS_LIGHT_MODE_SUB:
			glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			break;
		case VS::CANVAS_LIGHT_MODE_MIX:
		default:
			// CANVAS_LIGHT_MODE_MASK falls through to this -- an alpha mix
			// is a closer approximation than a plain additive blend (see
			// this function's own comment on why a real mask pass isn't
			// implemented yet).
			glBlendEquation(GL_FUNC_ADD);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
	}

	_load_transform2d_gl(p_light->xform_curr);

	Color c = p_light->color;
	c.r *= p_light->energy;
	c.g *= p_light->energy;
	c.b *= p_light->energy;
	glColor4f(c.r, c.g, c.b, c.a);

	float hw = tex->width * p_light->scale * 0.5f;
	float hh = tex->height * p_light->scale * 0.5f;
	GLfloat verts[8] = { -hw, -hh, hw, -hh, hw, hh, -hw, hh };
	GLfloat uvs[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };

	glBindTexture(GL_TEXTURE_2D, tex->tex_id);
	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, verts);
	glTexCoordPointer(2, GL_FLOAT, 0, uvs);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

static void _draw_canvas_lights(RasterizerStorageGLFF *p_storage, RasterizerCanvas::Light *p_light) {
	if (!p_light) {
		return;
	}
	glMatrixMode(GL_MODELVIEW);
	while (p_light) {
		_draw_canvas_light(p_storage, p_light);
		p_light = p_light->next_ptr;
	}
	// Restore the state every regular CanvasItem draw call in this file
	// assumes: alpha-blend (not the ADD/SUB this loop may have left
	// active) and identity modelview (a light's own xform_curr, just
	// loaded above, must not leak into whatever draws next).
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glLoadIdentity();
}

void RasterizerCanvasGLFF::canvas_render_items_implementation(Item *p_item_list, int p_z, const Color &p_modulate, Light *p_light, const Transform2D &p_base_transform) {
	Item *current_clip = nullptr;
	bool reclip = false;

	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_SCISSOR_TEST);

	while (p_item_list) {
		Item *ci = p_item_list;

		if (ci->visible) {
			if (current_clip != ci->final_clip_owner) {
				current_clip = ci->final_clip_owner;

				if (current_clip) {
					Size2 window_size = OS::get_singleton()->get_window_size();
					int y = window_size.height - (current_clip->final_clip_rect.position.y + current_clip->final_clip_rect.size.y);
					gl_enable_scissor(current_clip->final_clip_rect.position.x, y, current_clip->final_clip_rect.size.width, current_clip->final_clip_rect.size.height);
				} else {
					gl_disable_scissor();
				}
			}

			_legacy_canvas_item_render_commands(ci, current_clip, reclip, nullptr);
		}

		p_item_list = p_item_list->next;
	}

	glDisable(GL_SCISSOR_TEST);

	// godot-ports#27: real Light2D rendering, a second pass over this same
	// item list's light set -- see _draw_canvas_lights()'s own comment for
	// the technique and this pass's known limitations.
	_draw_canvas_lights(storage, p_light);
}

// godot-ports#33: real CPU skinning for a bone-weighted Polygon2D, the 2D
// equivalent of godot-ports#22's 3D software-skinning fallback -- no
// existing engine mechanism to reuse here (unlike #22's MeshInstance/
// software_skinning_fallback), so this is new CPU vertex-transform code.
// Matches drivers/gles2/shaders/canvas.glsl's USE_SKELETON block exactly
// (same skeleton_transform/skeleton_transform_inverse sandwich, same
// linear weighted-blend-of-raw-matrix-components technique -- NOT a
// proper decomposed/slerped blend, just like the shader it mirrors), just
// computed on the CPU instead of a vertex shader: a bone's stored
// Transform2D (pushed by Skeleton2D/Bone2D via skeleton_bone_set_
// transform_2d) already maps a rest-pose point *in skeleton space* to its
// deformed position, also in skeleton space -- so a vertex must be
// converted world-space -> skeleton-space, deformed there, then converted
// back, rather than deformed directly in the item's own local space.
// Returns false (nothing written to r_verts, caller should fall back to
// the plain undeformed path) when the polygon has no real bone/weight
// data or its skeleton is missing/not a 2D skeleton.
static bool _skin_polygon_vertices(RasterizerStorageGLFF *p_storage, const RasterizerCanvas::Item::CommandPolygon *p_poly, const Transform2D &p_local_to_world, const Transform2D &p_item_group_base_transform, RID p_skeleton, Vector<GLfloat> &r_verts) {
	if (!p_skeleton.is_valid()) {
		return false;
	}
	RasterizerStorageGLFF::Skeleton *skel = p_storage->skeleton_owner.getornull(p_skeleton);
	if (!skel || !skel->use_2d || skel->bone_count == 0) {
		return false;
	}

	int vcount = p_poly->points.size();
	if (vcount == 0 || p_poly->bones.size() < vcount * 4 || p_poly->weights.size() < vcount * 4) {
		return false;
	}

	Transform2D skeleton_transform = p_item_group_base_transform * skel->base_transform_2d;
	Transform2D skeleton_transform_inv = skeleton_transform.affine_inverse();

	r_verts.resize(vcount * 2);
	for (int p = 0; p < vcount; p++) {
		Vector2 world_rest = p_local_to_world.xform(p_poly->points[p]);
		Vector2 skel_local = skeleton_transform_inv.xform(world_rest);

		Transform2D blended;
		blended.elements[0] = Vector2();
		blended.elements[1] = Vector2();
		blended.elements[2] = Vector2();
		for (int i = 0; i < 4; i++) {
			float w = p_poly->weights[p * 4 + i];
			if (w == 0.0f) {
				continue;
			}
			int b = p_poly->bones[p * 4 + i];
			if (b < 0 || b >= skel->bone_count) {
				continue;
			}
			const Transform2D &bt = skel->bones_2d[b];
			blended.elements[0] += bt.elements[0] * w;
			blended.elements[1] += bt.elements[1] * w;
			blended.elements[2] += bt.elements[2] * w;
		}

		Vector2 skel_deformed = blended.xform(skel_local);
		Vector2 world_deformed = skeleton_transform.xform(skel_deformed);
		r_verts.write[p * 2] = world_deformed.x;
		r_verts.write[p * 2 + 1] = world_deformed.y;
	}
	return true;
}

void RasterizerCanvasGLFF::render_batches(Item *p_current_clip, bool &r_reclip, RasterizerStorageGLFF::Material *p_material) {
	// Reached only via _legacy_canvas_item_render_commands() (batching is
	// off, see class comment in the header) -- batch.type is always
	// BT_DEFAULT here, one batch per item, real work below dispatches by
	// command type. try_join_item()/_batch_upload_buffers() are the parts
	// of the CRTP interface that would matter for the batched fast path;
	// they're unreachable with batching off.
	int num_batches = bdata.batches.size();

	for (int batch_num = 0; batch_num < num_batches; batch_num++) {
		const Batch &batch = bdata.batches[batch_num];
		int end_command = batch.first_command + batch.num_commands;
		Item::Command *const *commands = batch.item->commands.ptr();

		Transform2D extra_matrix; // accumulates TYPE_TRANSFORM commands within this item
		bool clip_active = true; // TYPE_CLIP_IGNORE can suspend scissor for the rest of the item

		for (int i = batch.first_command; i < end_command; i++) {
			Item::Command *command = commands[i];

			switch (command->type) {
				case Item::Command::TYPE_TRANSFORM: {
					Item::CommandTransform *t = static_cast<Item::CommandTransform *>(command);
					extra_matrix = extra_matrix * t->xform;
				} break;

				case Item::Command::TYPE_CLIP_IGNORE: {
					Item::CommandClipIgnore *ci = static_cast<Item::CommandClipIgnore *>(command);
					bool want_active = !ci->ignore;
					if (want_active != clip_active) {
						clip_active = want_active;
						if (clip_active && p_current_clip) {
							Size2 window_size = OS::get_singleton()->get_window_size();
							int y = window_size.height - (p_current_clip->final_clip_rect.position.y + p_current_clip->final_clip_rect.size.y);
							gl_enable_scissor(p_current_clip->final_clip_rect.position.x, y, p_current_clip->final_clip_rect.size.width, p_current_clip->final_clip_rect.size.height);
						} else {
							gl_disable_scissor();
						}
					}
				} break;

				case Item::Command::TYPE_RECT: {
					Item::CommandRect *r = static_cast<Item::CommandRect *>(command);

					_load_transform2d_gl(batch.item->final_transform * extra_matrix);

					RasterizerStorageGLFF::Texture *tex = r->texture.is_valid() ? storage->texture_owner.getornull(r->texture) : nullptr;
					// ViewportContainer (and any other SubViewport-as-texture
					// consumer) hands out a separate "proxy" Texture RID (see
					// the Texture::proxy comment in rasterizer_storage_glff.h)
					// -- resolve to the real render target texture before
					// reading any of its fields, same as every other Godot
					// GL backend (godot-ports#28).
					if (tex) {
						tex = tex->get_ptr();
					}
					if (tex) {
						glEnable(GL_TEXTURE_2D);
						glBindTexture(GL_TEXTURE_2D, tex->tex_id);
						glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					} else {
						glDisable(GL_TEXTURE_2D);
					}

					Rect2 src = (r->flags & CANVAS_RECT_REGION) ? r->source : Rect2(0, 0, 1, 1);
					float u0 = src.position.x, u1 = src.position.x + src.size.width;
					float v0 = src.position.y, v1 = src.position.y + src.size.height;
					if (tex && (r->flags & CANVAS_RECT_REGION)) {
						// source rect for a region is in pixels; normalize to 0..1
						u0 /= tex->width;
						u1 /= tex->width;
						v0 /= tex->height;
						v1 /= tex->height;
					}
					if (tex && tex->is_render_target && tex->gl_alloc_width > 0 && tex->gl_alloc_height > 0) {
						// This driver has no GL_ARB_texture_non_power_of_two,
						// so render targets whose logical size isn't already
						// POT are backed by a larger, POT-rounded GL texture
						// with only the top-left width x height sub-rect
						// ever populated (see the gl_alloc_width/height
						// comment on Texture in rasterizer_storage_glff.h,
						// godot-ports#28) -- rescale the logical 0..1 UV
						// range down to that populated sub-rect.
						float scale_u = (float)tex->width / (float)tex->gl_alloc_width;
						float scale_v = (float)tex->height / (float)tex->gl_alloc_height;
						u0 *= scale_u;
						u1 *= scale_u;
						v0 *= scale_v;
						v1 *= scale_v;
					}
					if (r->flags & CANVAS_RECT_FLIP_H) {
						SWAP(u0, u1);
					}
					if (r->flags & CANVAS_RECT_FLIP_V) {
						// Render-target textures are populated bottom-up
						// (glCopyTexSubImage2D pulls from GL's bottom-left-
						// origin framebuffer into texture row 0 -- the
						// OPPOSITE of a normally-loaded top-down image
						// texture), same as every FBO-based backend's
						// render-target textures. Godot itself already
						// knows this and compensates universally:
						// ViewportContainer (scene/gui/viewport_container.cpp)
						// draws with a *negative*-height rect specifically
						// for its child Viewport's texture, which is what
						// sets this FLIP_V flag in the first place (see
						// visual_server_canvas.cpp's canvas_item_add_texture_rect).
						// No GLFF-specific extra flip is needed on top of
						// this -- an earlier version of this code added one
						// unconditionally, which canceled this correct flip
						// back out and rendered SubViewport-as-texture
						// content (the editor's own 3D panel) upside down
						// (godot-ports#28).
						SWAP(v0, v1);
					}

					Color c = r->modulate * batch.item->final_modulate;
					glColor4f(c.r, c.g, c.b, c.a);

					Rect2 rect = r->rect;
					GLfloat verts[8] = {
						(GLfloat)rect.position.x, (GLfloat)rect.position.y,
						(GLfloat)(rect.position.x + rect.size.width), (GLfloat)rect.position.y,
						(GLfloat)(rect.position.x + rect.size.width), (GLfloat)(rect.position.y + rect.size.height),
						(GLfloat)rect.position.x, (GLfloat)(rect.position.y + rect.size.height)
					};
					GLfloat uvs[8] = { u0, v0, u1, v0, u1, v1, u0, v1 };

					glEnableClientState(GL_VERTEX_ARRAY);
					glVertexPointer(2, GL_FLOAT, 0, verts);
					glEnableClientState(GL_TEXTURE_COORD_ARRAY);
					glTexCoordPointer(2, GL_FLOAT, 0, uvs);
					glDisableClientState(GL_COLOR_ARRAY);
					glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
				} break;

				case Item::Command::TYPE_NINEPATCH: {
					// Simplification for this pass: draw as a plain stretched
					// rect, ignoring the 9-slice margins entirely -- a real,
					// documented approximation (see class comment), not a
					// silent gap.
					Item::CommandNinePatch *np = static_cast<Item::CommandNinePatch *>(command);

					_load_transform2d_gl(batch.item->final_transform * extra_matrix);

					RasterizerStorageGLFF::Texture *tex = np->texture.is_valid() ? storage->texture_owner.getornull(np->texture) : nullptr;
					if (tex) {
						tex = tex->get_ptr(); // resolve proxies, see TYPE_RECT above
					}
					if (tex) {
						glEnable(GL_TEXTURE_2D);
						glBindTexture(GL_TEXTURE_2D, tex->tex_id);
						glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					} else {
						glDisable(GL_TEXTURE_2D);
					}

					Color c = np->color * batch.item->final_modulate;
					glColor4f(c.r, c.g, c.b, c.a);

					Rect2 rect = np->rect;
					GLfloat verts[8] = {
						(GLfloat)rect.position.x, (GLfloat)rect.position.y,
						(GLfloat)(rect.position.x + rect.size.width), (GLfloat)rect.position.y,
						(GLfloat)(rect.position.x + rect.size.width), (GLfloat)(rect.position.y + rect.size.height),
						(GLfloat)rect.position.x, (GLfloat)(rect.position.y + rect.size.height)
					};
					GLfloat uvs[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };

					glEnableClientState(GL_VERTEX_ARRAY);
					glVertexPointer(2, GL_FLOAT, 0, verts);
					glEnableClientState(GL_TEXTURE_COORD_ARRAY);
					glTexCoordPointer(2, GL_FLOAT, 0, uvs);
					glDisableClientState(GL_COLOR_ARRAY);
					glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
				} break;

				case Item::Command::TYPE_MULTIRECT: {
					Item::CommandMultiRect *mr = static_cast<Item::CommandMultiRect *>(command);

					_load_transform2d_gl(batch.item->final_transform * extra_matrix);

					RasterizerStorageGLFF::Texture *tex = mr->texture.is_valid() ? storage->texture_owner.getornull(mr->texture) : nullptr;
					if (tex) {
						tex = tex->get_ptr(); // resolve proxies, see TYPE_RECT above
					}
					if (tex) {
						glEnable(GL_TEXTURE_2D);
						glBindTexture(GL_TEXTURE_2D, tex->tex_id);
						glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					} else {
						glDisable(GL_TEXTURE_2D);
					}

					Color c = mr->modulate * batch.item->final_modulate;
					glColor4f(c.r, c.g, c.b, c.a);

					int rect_count = mr->rects.size();
					for (int r = 0; r < rect_count; r++) {
						Rect2 rect = mr->rects[r];
						Rect2 src = (r < mr->sources.size()) ? mr->sources[r] : Rect2(0, 0, 1, 1);
						float u0 = src.position.x, u1 = src.position.x + src.size.width;
						float v0 = src.position.y, v1 = src.position.y + src.size.height;
						if (tex) {
							u0 /= tex->width;
							u1 /= tex->width;
							v0 /= tex->height;
							v1 /= tex->height;
						}

						GLfloat verts[8] = {
							(GLfloat)rect.position.x, (GLfloat)rect.position.y,
							(GLfloat)(rect.position.x + rect.size.width), (GLfloat)rect.position.y,
							(GLfloat)(rect.position.x + rect.size.width), (GLfloat)(rect.position.y + rect.size.height),
							(GLfloat)rect.position.x, (GLfloat)(rect.position.y + rect.size.height)
						};
						GLfloat uvs[8] = { u0, v0, u1, v0, u1, v1, u0, v1 };

						glEnableClientState(GL_VERTEX_ARRAY);
						glVertexPointer(2, GL_FLOAT, 0, verts);
						glEnableClientState(GL_TEXTURE_COORD_ARRAY);
						glTexCoordPointer(2, GL_FLOAT, 0, uvs);
						glDisableClientState(GL_COLOR_ARRAY);
						glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
					}
				} break;

				case Item::Command::TYPE_LINE: {
					Item::CommandLine *line = static_cast<Item::CommandLine *>(command);

					_load_transform2d_gl(batch.item->final_transform * extra_matrix);
					glDisable(GL_TEXTURE_2D);

					Color c = line->color * batch.item->final_modulate;
					glColor4f(c.r, c.g, c.b, c.a);
					glLineWidth(MAX(line->width, 1.0f));

					GLfloat verts[4] = { (GLfloat)line->from.x, (GLfloat)line->from.y, (GLfloat)line->to.x, (GLfloat)line->to.y };
					glEnableClientState(GL_VERTEX_ARRAY);
					glVertexPointer(2, GL_FLOAT, 0, verts);
					glDisableClientState(GL_TEXTURE_COORD_ARRAY);
					glDisableClientState(GL_COLOR_ARRAY);
					glDrawArrays(GL_LINES, 0, 2);
				} break;

				case Item::Command::TYPE_POLYLINE: {
					Item::CommandPolyLine *pline = static_cast<Item::CommandPolyLine *>(command);

					_load_transform2d_gl(batch.item->final_transform * extra_matrix);
					glDisable(GL_TEXTURE_2D);

					const Vector<Point2> &pts = pline->triangles.size() ? pline->triangles : pline->lines;
					const Vector<Color> &cols = pline->triangles.size() ? pline->triangle_colors : pline->line_colors;
					GLenum prim = pline->triangles.size() ? GL_TRIANGLES : GL_LINES;

					int count = pts.size();
					if (count > 0) {
						Vector<GLfloat> verts;
						verts.resize(count * 2);
						for (int p = 0; p < count; p++) {
							verts.write[p * 2] = pts[p].x;
							verts.write[p * 2 + 1] = pts[p].y;
						}

						if (cols.size() == count) {
							Color c0 = cols[0] * batch.item->final_modulate;
							glColor4f(c0.r, c0.g, c0.b, c0.a);
						} else {
							Color c = batch.item->final_modulate;
							glColor4f(c.r, c.g, c.b, c.a);
						}

						glEnableClientState(GL_VERTEX_ARRAY);
						glVertexPointer(2, GL_FLOAT, 0, verts.ptr());
						glDisableClientState(GL_TEXTURE_COORD_ARRAY);
						glDisableClientState(GL_COLOR_ARRAY);
						glDrawArrays(prim, 0, count);
					}
				} break;

				case Item::Command::TYPE_PRIMITIVE: {
					Item::CommandPrimitive *prim = static_cast<Item::CommandPrimitive *>(command);

					_load_transform2d_gl(batch.item->final_transform * extra_matrix);

					RasterizerStorageGLFF::Texture *tex = prim->texture.is_valid() ? storage->texture_owner.getornull(prim->texture) : nullptr;
					if (tex) {
						tex = tex->get_ptr(); // resolve proxies, see TYPE_RECT above
					}
					if (tex) {
						glEnable(GL_TEXTURE_2D);
						glBindTexture(GL_TEXTURE_2D, tex->tex_id);
						glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					} else {
						glDisable(GL_TEXTURE_2D);
					}

					int count = prim->points.size();
					if (count > 0 && count <= 4) {
						GLfloat verts[8];
						GLfloat uvs[8];
						for (int p = 0; p < count; p++) {
							verts[p * 2] = prim->points[p].x;
							verts[p * 2 + 1] = prim->points[p].y;
							uvs[p * 2] = (p < prim->uvs.size()) ? prim->uvs[p].x : 0;
							uvs[p * 2 + 1] = (p < prim->uvs.size()) ? prim->uvs[p].y : 0;
						}

						Color c = (prim->colors.size() > 0 ? prim->colors[0] : Color(1, 1, 1, 1)) * batch.item->final_modulate;
						glColor4f(c.r, c.g, c.b, c.a);

						glEnableClientState(GL_VERTEX_ARRAY);
						glVertexPointer(2, GL_FLOAT, 0, verts);
						glEnableClientState(GL_TEXTURE_COORD_ARRAY);
						glTexCoordPointer(2, GL_FLOAT, 0, uvs);
						glDisableClientState(GL_COLOR_ARRAY);
						glDrawArrays(count == 3 ? GL_TRIANGLES : GL_TRIANGLE_FAN, 0, count);
					}
				} break;

				case Item::Command::TYPE_POLYGON: {
					Item::CommandPolygon *poly = static_cast<Item::CommandPolygon *>(command);

					int vcount = poly->points.size();
					int icount = poly->indices.size();
					if (vcount > 0) {
						// godot-ports#33: a bone-weighted Polygon2D deforms
						// entirely on the CPU, in world/canvas space --
						// _skin_polygon_vertices() already applies
						// final_transform (and the skeleton sandwich) to
						// every point, so this path loads an identity
						// modelview instead of the usual per-item
						// transform. Every other command in this switch
						// still calls _load_transform2d_gl() itself before
						// drawing, so no explicit restore is needed after.
						Vector<GLfloat> skinned_verts;
						bool skinned = _skin_polygon_vertices(storage, poly, batch.item->final_transform * extra_matrix, _render_item_state.item_group_base_transform, batch.item->skeleton, skinned_verts);
						if (skinned) {
							glMatrixMode(GL_MODELVIEW);
							glLoadIdentity();
						} else {
							_load_transform2d_gl(batch.item->final_transform * extra_matrix);
						}

						RasterizerStorageGLFF::Texture *tex = poly->texture.is_valid() ? storage->texture_owner.getornull(poly->texture) : nullptr;
						if (tex) {
							tex = tex->get_ptr(); // resolve proxies, see TYPE_RECT above
						}
						if (tex) {
							glEnable(GL_TEXTURE_2D);
							glBindTexture(GL_TEXTURE_2D, tex->tex_id);
							glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
						} else {
							glDisable(GL_TEXTURE_2D);
						}

						Vector<GLfloat> verts, uvs, cols;
						uvs.resize(vcount * 2);
						bool has_colors = poly->colors.size() == vcount;
						bool per_vertex_colors = has_colors && poly->colors.size() > 1;
						if (per_vertex_colors) {
							cols.resize(vcount * 4);
						}
						if (!skinned) {
							verts.resize(vcount * 2);
						}
						for (int p = 0; p < vcount; p++) {
							if (!skinned) {
								verts.write[p * 2] = poly->points[p].x;
								verts.write[p * 2 + 1] = poly->points[p].y;
							}
							uvs.write[p * 2] = (p < poly->uvs.size()) ? poly->uvs[p].x : 0;
							uvs.write[p * 2 + 1] = (p < poly->uvs.size()) ? poly->uvs[p].y : 0;
							if (per_vertex_colors) {
								Color pc = poly->colors[p] * batch.item->final_modulate;
								cols.write[p * 4] = pc.r;
								cols.write[p * 4 + 1] = pc.g;
								cols.write[p * 4 + 2] = pc.b;
								cols.write[p * 4 + 3] = pc.a;
							}
						}

						glEnableClientState(GL_VERTEX_ARRAY);
						glVertexPointer(2, GL_FLOAT, 0, skinned ? skinned_verts.ptr() : verts.ptr());
						glEnableClientState(GL_TEXTURE_COORD_ARRAY);
						glTexCoordPointer(2, GL_FLOAT, 0, uvs.ptr());

						if (per_vertex_colors) {
							glEnableClientState(GL_COLOR_ARRAY);
							glColorPointer(4, GL_FLOAT, 0, cols.ptr());
						} else {
							glDisableClientState(GL_COLOR_ARRAY);
							Color c = (has_colors ? poly->colors[0] : Color(1, 1, 1, 1)) * batch.item->final_modulate;
							glColor4f(c.r, c.g, c.b, c.a);
						}

						if (icount > 0) {
							Vector<GLushort> idx;
							idx.resize(icount);
							for (int idx_i = 0; idx_i < icount; idx_i++) {
								idx.write[idx_i] = poly->indices[idx_i];
							}
							glDrawElements(GL_TRIANGLES, icount, GL_UNSIGNED_SHORT, idx.ptr());
						} else {
							glDrawArrays(GL_TRIANGLE_FAN, 0, vcount);
						}
					}
				} break;

				case Item::Command::TYPE_CIRCLE: {
					Item::CommandCircle *circle = static_cast<Item::CommandCircle *>(command);

					_load_transform2d_gl(batch.item->final_transform * extra_matrix);
					glDisable(GL_TEXTURE_2D);

					Color c = circle->color * batch.item->final_modulate;
					glColor4f(c.r, c.g, c.b, c.a);

					const int segments = 32;
					GLfloat verts[(segments + 2) * 2];
					verts[0] = circle->pos.x;
					verts[1] = circle->pos.y;
					for (int s = 0; s <= segments; s++) {
						float a = s * (Math_TAU / segments);
						verts[(s + 1) * 2] = circle->pos.x + Math::cos(a) * circle->radius;
						verts[(s + 1) * 2 + 1] = circle->pos.y + Math::sin(a) * circle->radius;
					}

					glEnableClientState(GL_VERTEX_ARRAY);
					glVertexPointer(2, GL_FLOAT, 0, verts);
					glDisableClientState(GL_TEXTURE_COORD_ARRAY);
					glDisableClientState(GL_COLOR_ARRAY);
					glDrawArrays(GL_TRIANGLE_FAN, 0, segments + 2);
				} break;

				default: {
					// TYPE_MESH, TYPE_MULTIMESH, TYPE_PARTICLES: not
					// implemented in this pass (see class comment) --
					// silently skipped, not drawn.
				} break;
			}
		}
	}
}

void RasterizerCanvasGLFF::initialize() {
	batch_constructor();
	batch_initialize();
	// Real, correctness-first per-item rendering for this pass (see class
	// comment) -- the batched fast path is a later optimization, not
	// required for content to render correctly.
	bdata.settings_use_batching = false;
}

RasterizerCanvasGLFF::RasterizerCanvasGLFF() {
	storage = nullptr;
	scene_render = nullptr;
}

RasterizerCanvasGLFF::~RasterizerCanvasGLFF() {
}
