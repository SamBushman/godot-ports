#include "rasterizer_storage_glff.h"

#include <stdio.h>

#include "core/math/math_funcs.h"
#include "core/os/os.h"
#include "core/project_settings.h"
#include "servers/visual_server.h"

GLuint RasterizerStorageGLFF::system_fbo = 0;

/* TEXTURE */

RID RasterizerStorageGLFF::texture_create() {
	Texture *texture = memnew(Texture);
	ERR_FAIL_COND_V(!texture, RID());
	glGenTextures(1, &texture->tex_id);
	return texture_owner.make_rid(texture);
}

// Strict-GL-1.2 floor (proposal §2): no S3TC/DXT, no ARB_texture_compression.
// Anything that isn't already an uncompressed format Godot's Image class
// understands natively gets converted to plain RGBA8 before upload.
static void _get_gl_format(Image::Format p_format, Ref<Image> &r_image, GLenum &r_gl_format, GLenum &r_gl_internal_format, GLenum &r_gl_type) {
	switch (p_format) {
		case Image::FORMAT_L8:
			r_gl_format = GL_LUMINANCE;
			r_gl_internal_format = GL_LUMINANCE;
			r_gl_type = GL_UNSIGNED_BYTE;
			break;
		case Image::FORMAT_LA8:
			r_gl_format = GL_LUMINANCE_ALPHA;
			r_gl_internal_format = GL_LUMINANCE_ALPHA;
			r_gl_type = GL_UNSIGNED_BYTE;
			break;
		case Image::FORMAT_RGB8:
			r_gl_format = GL_RGB;
			r_gl_internal_format = GL_RGB;
			r_gl_type = GL_UNSIGNED_BYTE;
			break;
		case Image::FORMAT_RGBA8:
			r_gl_format = GL_RGBA;
			r_gl_internal_format = GL_RGBA;
			r_gl_type = GL_UNSIGNED_BYTE;
			break;
		default:
			if (r_image.is_valid()) {
				r_image->convert(Image::FORMAT_RGBA8);
			}
			r_gl_format = GL_RGBA;
			r_gl_internal_format = GL_RGBA;
			r_gl_type = GL_UNSIGNED_BYTE;
			break;
	}
}

void RasterizerStorageGLFF::texture_allocate(RID p_texture, int p_width, int p_height, int p_depth_3d, Image::Format p_format, VS::TextureType p_type, uint32_t p_flags) {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND(!texture);

	texture->width = p_width;
	texture->height = p_height;
	texture->depth = p_depth_3d;
	texture->format = p_format;
	texture->type = p_type;
	texture->flags = p_flags;
	texture->active = true;

	glBindTexture(GL_TEXTURE_2D, texture->tex_id);

	GLenum gl_format, gl_internal_format, gl_type;
	Ref<Image> dummy;
	_get_gl_format(p_format, dummy, gl_format, gl_internal_format, gl_type);
	texture->gl_format_cache = gl_format;
	texture->gl_internal_format_cache = gl_internal_format;
	texture->gl_type_cache = gl_type;

	// Allocate storage now with no data -- texture_set_data uploads the
	// real pixels later. No mipmap auto-generation exists under strict GL
	// 1.2 (no SGIS_generate_mipmap/glGenerateMipmap assumed, see proposal
	// §2) -- CPU-side mip generation is deferred to whichever phase needs
	// filtered minification; base level alone is enough to compile/display.
	while (glGetError() != GL_NO_ERROR) {}
	glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, p_width, p_height, 0, gl_format, gl_type, nullptr);
	{
		GLenum alloc_err = glGetError();
		fprintf(stderr, "GLFF DEBUG: texture_allocate w=%d h=%d pot=%d glTexImage2D err=0x%x\n",
				p_width, p_height,
				(int)(((p_width & (p_width - 1)) == 0) && ((p_height & (p_height - 1)) == 0)),
				(unsigned)alloc_err);
		fflush(stderr);
	}

	GLenum wrap = (p_flags & VS::TEXTURE_FLAG_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	GLenum filter = (p_flags & VS::TEXTURE_FLAG_FILTER) ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

void RasterizerStorageGLFF::texture_set_data(RID p_texture, const Ref<Image> &p_image, int p_level) {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND(!texture);
	ERR_FAIL_COND(p_image.is_null());

	Ref<Image> img = p_image->duplicate();

	GLenum gl_format, gl_internal_format, gl_type;
	_get_gl_format(texture->format, img, gl_format, gl_internal_format, gl_type);

	glBindTexture(GL_TEXTURE_2D, texture->tex_id);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	PoolVector<uint8_t> data = img->get_data();
	PoolVector<uint8_t>::Read r = data.read();
	glTexImage2D(GL_TEXTURE_2D, p_level, gl_internal_format, img->get_width(), img->get_height(), 0, gl_format, gl_type, r.ptr());

	texture->data_size = data.size();
	// Cache the uploaded image so texture_get_data() (called by e.g.
	// default_theme.cpp's flip_icon()) has something real to hand back --
	// this backend has no glReadPixels-based readback path (and wouldn't
	// want one for a plain upload-only texture anyway), so the last
	// image actually given to texture_set_data is exactly what a caller
	// asking "what does this texture contain" should get.
	texture->cached_image = img;
}

void RasterizerStorageGLFF::texture_set_data_partial(RID p_texture, const Ref<Image> &p_image, int src_x, int src_y, int src_w, int src_h, int dst_x, int dst_y, int p_dst_mip, int p_level) {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND(!texture);
	ERR_FAIL_COND(p_image.is_null());

	Ref<Image> img = p_image->duplicate();
	GLenum gl_format, gl_internal_format, gl_type;
	_get_gl_format(texture->format, img, gl_format, gl_internal_format, gl_type);

	Ref<Image> region = img->get_rect(Rect2(src_x, src_y, src_w, src_h));
	glBindTexture(GL_TEXTURE_2D, texture->tex_id);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	PoolVector<uint8_t> data = region->get_data();
	PoolVector<uint8_t>::Read r = data.read();
	glTexSubImage2D(GL_TEXTURE_2D, p_dst_mip, dst_x, dst_y, src_w, src_h, gl_format, gl_type, r.ptr());
}

Ref<Image> RasterizerStorageGLFF::texture_get_data(RID p_texture, int p_level) const {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND_V(!texture, Ref<Image>());
	// No glReadPixels-based GPU readback (not needed -- this backend never
	// renders *to* a texture, only uploads *from* Image data, see §2's
	// no-FBO design), but callers like default_theme.cpp's flip_icon()
	// legitimately ask "what does this texture contain" for a texture we
	// uploaded ourselves -- returning the cached copy from texture_set_data
	// is exactly correct for that case, not just a stand-in.
	return texture->cached_image;
}

void RasterizerStorageGLFF::texture_set_flags(RID p_texture, uint32_t p_flags) {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND(!texture);
	texture->flags = p_flags;

	glBindTexture(GL_TEXTURE_2D, texture->tex_id);
	GLenum wrap = (p_flags & VS::TEXTURE_FLAG_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
	GLenum filter = (p_flags & VS::TEXTURE_FLAG_FILTER) ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
}

uint32_t RasterizerStorageGLFF::texture_get_flags(RID p_texture) const {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND_V(!texture, 0);
	return texture->flags;
}

Image::Format RasterizerStorageGLFF::texture_get_format(RID p_texture) const {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND_V(!texture, Image::FORMAT_L8);
	return texture->format;
}

VS::TextureType RasterizerStorageGLFF::texture_get_type(RID p_texture) const {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND_V(!texture, VS::TEXTURE_TYPE_2D);
	return texture->type;
}

uint32_t RasterizerStorageGLFF::texture_get_texid(RID p_texture) const {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND_V(!texture, 0);
	return texture->tex_id;
}

uint32_t RasterizerStorageGLFF::texture_get_width(RID p_texture) const {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND_V(!texture, 0);
	return texture->width;
}

uint32_t RasterizerStorageGLFF::texture_get_height(RID p_texture) const {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND_V(!texture, 0);
	return texture->height;
}

uint32_t RasterizerStorageGLFF::texture_get_depth(RID p_texture) const {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND_V(!texture, 0);
	return texture->depth;
}

void RasterizerStorageGLFF::texture_set_size_override(RID p_texture, int p_width, int p_height, int p_depth_3d) {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND(!texture);
	texture->width = p_width;
	texture->height = p_height;
}

void RasterizerStorageGLFF::texture_bind(RID p_texture, uint32_t p_texture_no) {
	Texture *texture = texture_owner.getornull(p_texture);
	ERR_FAIL_COND(!texture);
	glActiveTexture(GL_TEXTURE0 + p_texture_no);
	glBindTexture(GL_TEXTURE_2D, texture->tex_id);
}

/* MESH */

RID RasterizerStorageGLFF::mesh_create() {
	Mesh *mesh = memnew(Mesh);
	return mesh_owner.make_rid(mesh);
}

// Reconstructs the same per-attribute byte offsets/strides
// VisualServer::mesh_surface_make_offsets_from_format() used to pack the
// raw p_array this mesh was handed (that function is the authoritative
// wire layout, shared by every backend regardless of which compression
// flags a given surface's format uses -- it isn't GLFF-specific). Ported
// locally rather than called back into the VisualServer singleton,
// matching the existing precedent in this codebase: GLES2/GLES3's own
// mesh_add_surface() do the same local reconstruction instead of reaching
// up into servers/ from drivers/.
//
// rendering/misc/mesh_storage/split_stream defaults to (and in practice
// always is) false, so every attribute after ARRAY_INDEX shares one
// combined per-vertex stride (positions_stride + attributes_stride) --
// this only matters for the ARRAY_VERTEX/use_split_stream special case,
// kept here for correctness in case that project setting is ever flipped.
static void _mesh_surface_make_offsets(uint32_t p_format, int p_vertex_len, uint32_t *r_offsets, uint32_t *r_strides) {
	bool use_split_stream = GLOBAL_GET("rendering/misc/mesh_storage/split_stream") && !(p_format & VS::ARRAY_FLAG_USE_DYNAMIC_UPDATE);

	int attributes_base_offset = 0;
	int attributes_stride = 0;
	int positions_stride = 0;

	for (int i = 0; i < VS::ARRAY_MAX; i++) {
		r_offsets[i] = 0;

		if (!(p_format & (1 << i))) {
			continue;
		}

		int elem_size = 0;

		switch (i) {
			case VS::ARRAY_VERTEX: {
				elem_size = (p_format & VS::ARRAY_FLAG_USE_2D_VERTICES) ? 2 : 3;
				elem_size *= (p_format & VS::ARRAY_COMPRESS_VERTEX) ? sizeof(int16_t) : sizeof(float);
				if (elem_size == 6) {
					elem_size = 8;
				}
				r_offsets[i] = 0;
				positions_stride = elem_size;
				attributes_base_offset = use_split_stream ? elem_size * p_vertex_len : elem_size;
			} break;
			case VS::ARRAY_NORMAL: {
				if (p_format & VS::ARRAY_FLAG_USE_OCTAHEDRAL_COMPRESSION) {
					if ((p_format & VS::ARRAY_COMPRESS_NORMAL) && (p_format & VS::ARRAY_FORMAT_TANGENT) && (p_format & VS::ARRAY_COMPRESS_TANGENT)) {
						elem_size = sizeof(uint8_t) * 2;
					} else {
						elem_size = sizeof(uint16_t) * 2;
					}
				} else {
					elem_size = (p_format & VS::ARRAY_COMPRESS_NORMAL) ? sizeof(uint32_t) : sizeof(float) * 3;
				}
				r_offsets[i] = attributes_base_offset + attributes_stride;
				attributes_stride += elem_size;
			} break;
			case VS::ARRAY_TANGENT: {
				if (p_format & VS::ARRAY_FLAG_USE_OCTAHEDRAL_COMPRESSION) {
					if ((p_format & VS::ARRAY_COMPRESS_TANGENT) && (p_format & VS::ARRAY_FORMAT_NORMAL) && (p_format & VS::ARRAY_COMPRESS_NORMAL)) {
						elem_size = sizeof(uint8_t) * 2;
					} else {
						elem_size = sizeof(uint16_t) * 2;
					}
				} else {
					elem_size = (p_format & VS::ARRAY_COMPRESS_TANGENT) ? sizeof(uint32_t) : sizeof(float) * 4;
				}
				r_offsets[i] = attributes_base_offset + attributes_stride;
				attributes_stride += elem_size;
			} break;
			case VS::ARRAY_COLOR: {
				elem_size = (p_format & VS::ARRAY_COMPRESS_COLOR) ? sizeof(uint32_t) : sizeof(float) * 4;
				r_offsets[i] = attributes_base_offset + attributes_stride;
				attributes_stride += elem_size;
			} break;
			case VS::ARRAY_TEX_UV: {
				elem_size = (p_format & VS::ARRAY_COMPRESS_TEX_UV) ? sizeof(uint32_t) : sizeof(float) * 2;
				r_offsets[i] = attributes_base_offset + attributes_stride;
				attributes_stride += elem_size;
			} break;
			case VS::ARRAY_TEX_UV2: {
				elem_size = (p_format & VS::ARRAY_COMPRESS_TEX_UV2) ? sizeof(uint32_t) : sizeof(float) * 2;
				r_offsets[i] = attributes_base_offset + attributes_stride;
				attributes_stride += elem_size;
			} break;
			case VS::ARRAY_WEIGHTS: {
				elem_size = (p_format & VS::ARRAY_COMPRESS_WEIGHTS) ? sizeof(uint16_t) * 4 : sizeof(float) * 4;
				r_offsets[i] = attributes_base_offset + attributes_stride;
				attributes_stride += elem_size;
			} break;
			case VS::ARRAY_BONES: {
				elem_size = (p_format & VS::ARRAY_FLAG_USE_16_BIT_BONES) ? sizeof(uint16_t) * 4 : sizeof(uint32_t);
				r_offsets[i] = attributes_base_offset + attributes_stride;
				attributes_stride += elem_size;
			} break;
			case VS::ARRAY_INDEX: {
				continue; // index buffer is its own separate array, not interleaved here
			}
			default: {
			} break;
		}
	}

	if (use_split_stream) {
		r_strides[VS::ARRAY_VERTEX] = positions_stride;
		for (int i = 1; i < VS::ARRAY_MAX - 1; i++) {
			r_strides[i] = attributes_stride;
		}
	} else {
		for (int i = 0; i < VS::ARRAY_MAX - 1; i++) {
			r_strides[i] = positions_stride + attributes_stride;
		}
	}
}

void RasterizerStorageGLFF::mesh_add_surface(RID p_mesh, uint32_t p_format, VS::PrimitiveType p_primitive, const PoolVector<uint8_t> &p_array, int p_vertex_count, const PoolVector<uint8_t> &p_index_array, int p_index_count, const AABB &p_aabb, const Vector<PoolVector<uint8_t>> &p_blend_shapes, const Vector<AABB> &p_bone_aabbs) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);

	Surface *surface = memnew(Surface);
	surface->format = p_format;
	surface->primitive = p_primitive;
	surface->array = p_array;
	surface->vertex_count = p_vertex_count;
	surface->index_array = p_index_array;
	surface->index_count = p_index_count;
	surface->aabb = p_aabb;
	surface->blend_shapes = p_blend_shapes;
	surface->bone_aabbs = p_bone_aabbs;

	surface->has_normals = (p_format & VS::ARRAY_FORMAT_NORMAL) != 0;
	surface->has_colors = (p_format & VS::ARRAY_FORMAT_COLOR) != 0;
	surface->has_uvs = (p_format & VS::ARRAY_FORMAT_TEX_UV) != 0;

	if (p_vertex_count > 0 && (p_format & VS::ARRAY_FORMAT_VERTEX)) {
		uint32_t offsets[VS::ARRAY_MAX];
		uint32_t strides[VS::ARRAY_MAX];
		_mesh_surface_make_offsets(p_format, p_vertex_count, offsets, strides);

		PoolVector<uint8_t>::Read r = p_array.read();
		const uint8_t *base = r.ptr();

		surface->vertices.resize(p_vertex_count);
		PoolVector<Vector3>::Write vw = surface->vertices.write();

		PoolVector<Vector3>::Write nw;
		if (surface->has_normals) {
			surface->normals.resize(p_vertex_count);
			nw = surface->normals.write();
		}
		PoolVector<Color>::Write cw;
		if (surface->has_colors) {
			surface->colors.resize(p_vertex_count);
			cw = surface->colors.write();
		}
		PoolVector<Vector2>::Write uw;
		if (surface->has_uvs) {
			surface->uvs.resize(p_vertex_count);
			uw = surface->uvs.write();
		}

		bool normal_octahedral = (p_format & VS::ARRAY_FLAG_USE_OCTAHEDRAL_COMPRESSION) != 0;
		bool normal_packed_byte = normal_octahedral && (p_format & VS::ARRAY_COMPRESS_NORMAL) && (p_format & VS::ARRAY_FORMAT_TANGENT) && (p_format & VS::ARRAY_COMPRESS_TANGENT);

		for (int i = 0; i < p_vertex_count; i++) {
			const uint8_t *vptr = base + offsets[VS::ARRAY_VERTEX] + i * strides[VS::ARRAY_VERTEX];
			if (p_format & VS::ARRAY_COMPRESS_VERTEX) {
				const uint16_t *h = (const uint16_t *)vptr;
				vw[i] = Vector3(Math::half_to_float(h[0]), Math::half_to_float(h[1]), Math::half_to_float(h[2]));
			} else {
				const float *f = (const float *)vptr;
				vw[i] = Vector3(f[0], f[1], f[2]);
			}

			if (surface->has_normals) {
				const uint8_t *nptr = base + offsets[VS::ARRAY_NORMAL] + i * strides[VS::ARRAY_NORMAL];
				if (normal_octahedral) {
					Vector2 oct;
					if (normal_packed_byte) {
						const int8_t *b = (const int8_t *)nptr;
						oct = Vector2(b[0] / 127.0f, b[1] / 127.0f);
					} else {
						const int16_t *s = (const int16_t *)nptr;
						oct = Vector2(s[0] / 32767.0f, s[1] / 32767.0f);
					}
					nw[i] = VisualServer::oct_to_norm(oct);
				} else if (p_format & VS::ARRAY_COMPRESS_NORMAL) {
					const int8_t *b = (const int8_t *)nptr;
					nw[i] = Vector3(b[0] / 127.0f, b[1] / 127.0f, b[2] / 127.0f);
				} else {
					const float *f = (const float *)nptr;
					nw[i] = Vector3(f[0], f[1], f[2]);
				}
			}

			if (surface->has_colors) {
				const uint8_t *cptr = base + offsets[VS::ARRAY_COLOR] + i * strides[VS::ARRAY_COLOR];
				if (p_format & VS::ARRAY_COMPRESS_COLOR) {
					cw[i] = Color(cptr[0] / 255.0f, cptr[1] / 255.0f, cptr[2] / 255.0f, cptr[3] / 255.0f);
				} else {
					const float *f = (const float *)cptr;
					cw[i] = Color(f[0], f[1], f[2], f[3]);
				}
			}

			if (surface->has_uvs) {
				const uint8_t *uptr = base + offsets[VS::ARRAY_TEX_UV] + i * strides[VS::ARRAY_TEX_UV];
				if (p_format & VS::ARRAY_COMPRESS_TEX_UV) {
					const uint16_t *h = (const uint16_t *)uptr;
					uw[i] = Vector2(Math::half_to_float(h[0]), Math::half_to_float(h[1]));
				} else {
					const float *f = (const float *)uptr;
					uw[i] = Vector2(f[0], f[1]);
				}
			}
		}
	}

	mesh->surfaces.push_back(surface);
}

void RasterizerStorageGLFF::mesh_set_blend_shape_count(RID p_mesh, int p_amount) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);
	mesh->blend_shape_count = p_amount;
}

int RasterizerStorageGLFF::mesh_get_blend_shape_count(RID p_mesh) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, 0);
	return mesh->blend_shape_count;
}

void RasterizerStorageGLFF::mesh_set_blend_shape_mode(RID p_mesh, VS::BlendShapeMode p_mode) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);
	mesh->blend_shape_mode = p_mode;
}

VS::BlendShapeMode RasterizerStorageGLFF::mesh_get_blend_shape_mode(RID p_mesh) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, VS::BLEND_SHAPE_MODE_NORMALIZED);
	return mesh->blend_shape_mode;
}

void RasterizerStorageGLFF::mesh_set_blend_shape_values(RID p_mesh, PoolVector<float> p_values) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);
	mesh->blend_shape_values = p_values;
}

PoolVector<float> RasterizerStorageGLFF::mesh_get_blend_shape_values(RID p_mesh) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, PoolVector<float>());
	return mesh->blend_shape_values;
}

void RasterizerStorageGLFF::mesh_surface_update_region(RID p_mesh, int p_surface, int p_offset, const PoolVector<uint8_t> &p_data) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);
	ERR_FAIL_INDEX(p_surface, mesh->surfaces.size());
	Surface *s = mesh->surfaces[p_surface];
	PoolVector<uint8_t>::Write w = s->array.write();
	PoolVector<uint8_t>::Read r = p_data.read();
	ERR_FAIL_COND(p_offset + p_data.size() > s->array.size());
	memcpy(w.ptr() + p_offset, r.ptr(), p_data.size());
}

void RasterizerStorageGLFF::mesh_surface_set_material(RID p_mesh, int p_surface, RID p_material) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);
	ERR_FAIL_INDEX(p_surface, mesh->surfaces.size());
	mesh->surfaces[p_surface]->material = p_material;
}

RID RasterizerStorageGLFF::mesh_surface_get_material(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, RID());
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), RID());
	return mesh->surfaces[p_surface]->material;
}

int RasterizerStorageGLFF::mesh_surface_get_array_len(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, 0);
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), 0);
	return mesh->surfaces[p_surface]->vertex_count;
}

int RasterizerStorageGLFF::mesh_surface_get_array_index_len(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, 0);
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), 0);
	return mesh->surfaces[p_surface]->index_count;
}

PoolVector<uint8_t> RasterizerStorageGLFF::mesh_surface_get_array(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, PoolVector<uint8_t>());
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), PoolVector<uint8_t>());
	return mesh->surfaces[p_surface]->array;
}

PoolVector<uint8_t> RasterizerStorageGLFF::mesh_surface_get_index_array(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, PoolVector<uint8_t>());
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), PoolVector<uint8_t>());
	return mesh->surfaces[p_surface]->index_array;
}

uint32_t RasterizerStorageGLFF::mesh_surface_get_format(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, 0);
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), 0);
	return mesh->surfaces[p_surface]->format;
}

VS::PrimitiveType RasterizerStorageGLFF::mesh_surface_get_primitive_type(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, VS::PRIMITIVE_TRIANGLES);
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), VS::PRIMITIVE_TRIANGLES);
	return mesh->surfaces[p_surface]->primitive;
}

AABB RasterizerStorageGLFF::mesh_surface_get_aabb(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, AABB());
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), AABB());
	return mesh->surfaces[p_surface]->aabb;
}

Vector<PoolVector<uint8_t>> RasterizerStorageGLFF::mesh_surface_get_blend_shapes(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, Vector<PoolVector<uint8_t>>());
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), Vector<PoolVector<uint8_t>>());
	return mesh->surfaces[p_surface]->blend_shapes;
}

Vector<AABB> RasterizerStorageGLFF::mesh_surface_get_skeleton_aabb(RID p_mesh, int p_surface) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, Vector<AABB>());
	ERR_FAIL_INDEX_V(p_surface, mesh->surfaces.size(), Vector<AABB>());
	return mesh->surfaces[p_surface]->bone_aabbs;
}

void RasterizerStorageGLFF::mesh_remove_surface(RID p_mesh, int p_index) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);
	ERR_FAIL_INDEX(p_index, mesh->surfaces.size());
	memdelete(mesh->surfaces[p_index]);
	mesh->surfaces.remove(p_index);
}

int RasterizerStorageGLFF::mesh_get_surface_count(RID p_mesh) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, 0);
	return mesh->surfaces.size();
}

void RasterizerStorageGLFF::mesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);
	mesh->custom_aabb = p_aabb;
	mesh->custom_aabb_valid = true;
}

AABB RasterizerStorageGLFF::mesh_get_custom_aabb(RID p_mesh) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, AABB());
	return mesh->custom_aabb;
}

AABB RasterizerStorageGLFF::mesh_get_aabb(RID p_mesh, RID p_skeleton) const {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND_V(!mesh, AABB());

	if (mesh->custom_aabb_valid) {
		return mesh->custom_aabb;
	}

	AABB aabb;
	for (int i = 0; i < mesh->surfaces.size(); i++) {
		if (i == 0) {
			aabb = mesh->surfaces[i]->aabb;
		} else {
			aabb.merge_with(mesh->surfaces[i]->aabb);
		}
	}
	return aabb;
}

void RasterizerStorageGLFF::mesh_clear(RID p_mesh) {
	Mesh *mesh = mesh_owner.getornull(p_mesh);
	ERR_FAIL_COND(!mesh);
	for (int i = 0; i < mesh->surfaces.size(); i++) {
		memdelete(mesh->surfaces[i]);
	}
	mesh->surfaces.clear();
}

/* GENERIC */

VS::InstanceType RasterizerStorageGLFF::get_base_type(RID p_rid) const {
	if (mesh_owner.owns(p_rid)) {
		return VS::INSTANCE_MESH;
	} else if (multimesh_owner.owns(p_rid)) {
		return VS::INSTANCE_MULTIMESH;
	} else if (immediate_owner.owns(p_rid)) {
		return VS::INSTANCE_IMMEDIATE;
	} else if (light_owner.owns(p_rid)) {
		return VS::INSTANCE_LIGHT;
	} else if (reflection_probe_owner.owns(p_rid)) {
		return VS::INSTANCE_REFLECTION_PROBE;
	} else if (gi_probe_owner.owns(p_rid)) {
		return VS::INSTANCE_GI_PROBE;
	} else if (lightmap_capture_data_owner.owns(p_rid)) {
		return VS::INSTANCE_LIGHTMAP_CAPTURE;
	} else if (particles_owner.owns(p_rid)) {
		return VS::INSTANCE_PARTICLES;
	} else {
		return VS::INSTANCE_NONE;
	}
}

bool RasterizerStorageGLFF::free(RID p_rid) {
	if (texture_owner.owns(p_rid)) {
		Texture *t = texture_owner.getornull(p_rid);
		glDeleteTextures(1, &t->tex_id);
		texture_owner.free(p_rid);
		memdelete(t);
		return true;
	} else if (mesh_owner.owns(p_rid)) {
		mesh_clear(p_rid);
		Mesh *m = mesh_owner.getornull(p_rid);
		mesh_owner.free(p_rid);
		memdelete(m);
		return true;
	} else if (material_owner.owns(p_rid)) {
		Material *m = material_owner.getornull(p_rid);
		material_owner.free(p_rid);
		memdelete(m);
		return true;
	} else if (multimesh_owner.owns(p_rid)) {
		MultiMesh *mm = multimesh_owner.getornull(p_rid);
		multimesh_owner.free(p_rid);
		memdelete(mm);
		return true;
	} else if (immediate_owner.owns(p_rid)) {
		Immediate *im = immediate_owner.getornull(p_rid);
		immediate_owner.free(p_rid);
		memdelete(im);
		return true;
	} else if (skeleton_owner.owns(p_rid)) {
		Skeleton *s = skeleton_owner.getornull(p_rid);
		skeleton_owner.free(p_rid);
		memdelete(s);
		return true;
	} else if (light_owner.owns(p_rid)) {
		Light *l = light_owner.getornull(p_rid);
		light_owner.free(p_rid);
		memdelete(l);
		return true;
	} else if (reflection_probe_owner.owns(p_rid)) {
		ReflectionProbe *r = reflection_probe_owner.getornull(p_rid);
		reflection_probe_owner.free(p_rid);
		memdelete(r);
		return true;
	} else if (gi_probe_owner.owns(p_rid)) {
		GIProbe *g = gi_probe_owner.getornull(p_rid);
		gi_probe_owner.free(p_rid);
		memdelete(g);
		return true;
	} else if (lightmap_capture_data_owner.owns(p_rid)) {
		LightmapCapture *l = lightmap_capture_data_owner.getornull(p_rid);
		lightmap_capture_data_owner.free(p_rid);
		memdelete(l);
		return true;
	} else if (particles_owner.owns(p_rid)) {
		Particles *p = particles_owner.getornull(p_rid);
		particles_owner.free(p_rid);
		memdelete(p);
		return true;
	} else if (render_target_owner.owns(p_rid)) {
		RenderTarget *rt = render_target_owner.getornull(p_rid);
		render_target_owner.free(p_rid);
		memdelete(rt);
		return true;
	} else if (canvas_light_shadow_owner.owns(p_rid)) {
		CanvasLightShadow *c = canvas_light_shadow_owner.getornull(p_rid);
		canvas_light_shadow_owner.free(p_rid);
		memdelete(c);
		return true;
	} else if (canvas_occluder_owner.owns(p_rid)) {
		CanvasOccluder *c = canvas_occluder_owner.getornull(p_rid);
		canvas_occluder_owner.free(p_rid);
		memdelete(c);
		return true;
	}
	return false;
}

String RasterizerStorageGLFF::get_video_adapter_name() const {
	return (const char *)glGetString(GL_RENDERER);
}

String RasterizerStorageGLFF::get_video_adapter_vendor() const {
	return (const char *)glGetString(GL_VENDOR);
}

void RasterizerStorageGLFF::initialize() {
	print_verbose("Using GLFF (OpenGL 1.2 fixed-function) video driver");
	print_line("OpenGL Renderer: " + get_video_adapter_name());
}

void RasterizerStorageGLFF::finalize() {
}

RasterizerStorageGLFF::RasterizerStorageGLFF() {
	canvas = nullptr;
	scene = nullptr;
}

RasterizerStorageGLFF::~RasterizerStorageGLFF() {
}
