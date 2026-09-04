#include "rasterizer_storage_glff.h"

#include "core/os/os.h"
#include "core/project_settings.h"

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
	glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, p_width, p_height, 0, gl_format, gl_type, nullptr);

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
	// Reading GL texture contents back is not needed by Phase 1 (no editor
	// thumbnail/viewport-capture path exists yet under GLFF); revisit once
	// SubViewport-as-texture (glCopyTexSubImage2D fallback, §2) is built.
	return Ref<Image>();
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
