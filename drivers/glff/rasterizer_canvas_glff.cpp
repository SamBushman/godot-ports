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

void RasterizerCanvasGLFF::canvas_render_items_implementation(Item *p_item_list, int p_z, const Color &p_modulate, Light *p_light, const Transform2D &p_base_transform) {
	// Light2D has no fixed-function equivalent (proposal §5/§5.2, found
	// during godot-ports#17) -- every CanvasItem draws unshaded regardless
	// of p_light, by design, not omission.
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
						SWAP(v0, v1);
					}
					if (tex && tex->is_render_target) {
						// See the is_render_target comment in
						// rasterizer_storage_glff.h -- composes correctly
						// with the FLIP_V case above (two swaps cancel out).
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

					_load_transform2d_gl(batch.item->final_transform * extra_matrix);

					RasterizerStorageGLFF::Texture *tex = poly->texture.is_valid() ? storage->texture_owner.getornull(poly->texture) : nullptr;
					if (tex) {
						glEnable(GL_TEXTURE_2D);
						glBindTexture(GL_TEXTURE_2D, tex->tex_id);
						glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
					} else {
						glDisable(GL_TEXTURE_2D);
					}

					int vcount = poly->points.size();
					int icount = poly->indices.size();
					if (vcount > 0) {
						Vector<GLfloat> verts, uvs, cols;
						verts.resize(vcount * 2);
						uvs.resize(vcount * 2);
						bool has_colors = poly->colors.size() == vcount;
						bool per_vertex_colors = has_colors && poly->colors.size() > 1;
						if (per_vertex_colors) {
							cols.resize(vcount * 4);
						}
						for (int p = 0; p < vcount; p++) {
							verts.write[p * 2] = poly->points[p].x;
							verts.write[p * 2 + 1] = poly->points[p].y;
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
						glVertexPointer(2, GL_FLOAT, 0, verts.ptr());
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
