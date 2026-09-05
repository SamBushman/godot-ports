#include "rasterizer_scene_glff.h"

#include "rasterizer_storage_glff.h"

// Transform -> GL column-major 4x4. Basis::xform() (see core/math/basis.h)
// confirms elements[row] is a ROW of the basis, i.e. elements[row].x/y/z
// are that row's 3 columns -- so GL's column-major slot [col*4+row] is
// elements[row][col]. Used for both the view matrix (camera_transform's
// affine_inverse()) and each instance's model matrix.
static void _load_transform_gl(const Transform &p_transform, GLfloat *r_gl) {
	const Basis &b = p_transform.basis;
	r_gl[0] = b.elements[0].x;
	r_gl[1] = b.elements[1].x;
	r_gl[2] = b.elements[2].x;
	r_gl[3] = 0;
	r_gl[4] = b.elements[0].y;
	r_gl[5] = b.elements[1].y;
	r_gl[6] = b.elements[2].y;
	r_gl[7] = 0;
	r_gl[8] = b.elements[0].z;
	r_gl[9] = b.elements[1].z;
	r_gl[10] = b.elements[2].z;
	r_gl[11] = 0;
	r_gl[12] = p_transform.origin.x;
	r_gl[13] = p_transform.origin.y;
	r_gl[14] = p_transform.origin.z;
	r_gl[15] = 1;
}

// CameraMatrix -> GL column-major 4x4. CameraMatrix::xform() (core/math/
// camera_matrix.h) confirms matrix[col][row] is already exactly GL's own
// column-major layout -- a straight flatten, no transpose needed.
static void _load_camera_matrix_gl(const CameraMatrix &p_cm, GLfloat *r_gl) {
	for (int i = 0; i < 4; i++) {
		for (int j = 0; j < 4; j++) {
			r_gl[i * 4 + j] = (GLfloat)p_cm.matrix[i][j];
		}
	}
}

static GLenum _primitive_to_gl(VS::PrimitiveType p_primitive) {
	switch (p_primitive) {
		case VS::PRIMITIVE_POINTS:
			return GL_POINTS;
		case VS::PRIMITIVE_LINES:
			return GL_LINES;
		case VS::PRIMITIVE_LINE_STRIP:
			return GL_LINE_STRIP;
		case VS::PRIMITIVE_LINE_LOOP:
			return GL_LINE_LOOP;
		case VS::PRIMITIVE_TRIANGLE_STRIP:
			return GL_TRIANGLE_STRIP;
		case VS::PRIMITIVE_TRIANGLE_FAN:
			return GL_TRIANGLE_FAN;
		case VS::PRIMITIVE_TRIANGLES:
		default:
			return GL_TRIANGLES;
	}
}

// Phase 3 (godot-ports#14 proposal): real mesh/material rendering, walking
// p_cull_result instead of Phase 1's hardcoded test triangle. Scope
// deliberately excludes (see rasterizer_storage_glff.h's Surface/Material
// comments): normal-mapping (no tangents), lightmaps (no UV2), skinning (no
// bones/weights -- fine for this driver's Phase 5 acceptance test, whose
// player/mob animation is pure Pivot-node transform, not skeletal), shadows,
// and the rest of SpatialMaterial's PBR params beyond albedo color/texture.
// Lighting is per-vertex GL_LIGHT0-7 (up to 8, the GL 1.2 floor's
// guaranteed minimum) driven directly off RasterizerStorageGLFF::Light's
// already-real color/type/param storage.
void RasterizerSceneGLFF::render_scene(const Transform &p_cam_transform, const CameraMatrix &p_cam_projection, const int p_eye, bool p_cam_ortogonal, InstanceBase **p_cull_result, int p_cull_count, RID *p_light_cull_result, int p_light_cull_count, RID *p_reflection_probe_cull_result, int p_reflection_probe_cull_count, RID p_environment, RID p_shadow_atlas, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass) {
	Color bg_color(0, 0, 0, 1);
	Color ambient_color(0, 0, 0, 1);
	if (p_environment.is_valid()) {
		Environment *env = environment_owner.getornull(p_environment);
		if (env) {
			bg_color = env->bg_color;
			ambient_color = Color(env->ambient_color.r * env->ambient_energy, env->ambient_color.g * env->ambient_energy, env->ambient_color.b * env->ambient_energy, 1.0);
		}
	}

	glClearColor(bg_color.r, bg_color.g, bg_color.b, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glDisable(GL_BLEND);

	GLfloat gl_proj[16];
	_load_camera_matrix_gl(p_cam_projection, gl_proj);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(gl_proj);

	Transform view_transform = p_cam_transform.affine_inverse();
	GLfloat gl_view[16];
	_load_transform_gl(view_transform, gl_view);
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(gl_view);

	int max_lights = MIN(p_light_cull_count, 8);
	if (max_lights > 0) {
		glEnable(GL_LIGHTING);
		glEnable(GL_NORMALIZE);
		GLfloat amb[4] = { ambient_color.r, ambient_color.g, ambient_color.b, 1.0f };
		glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);

		for (int i = 0; i < max_lights; i++) {
			GLenum gl_light = GL_LIGHT0 + i;
			LightInstance *li = light_instance_owner.getornull(p_light_cull_result[i]);
			RasterizerStorageGLFF::Light *light = li ? storage->light_owner.getornull(li->light) : nullptr;
			if (!light) {
				glDisable(gl_light);
				continue;
			}

			glEnable(gl_light);

			float energy = light->param[VS::LIGHT_PARAM_ENERGY];
			GLfloat diffuse[4] = { light->color.r * energy, light->color.g * energy, light->color.b * energy, 1.0f };
			GLfloat zero[4] = { 0, 0, 0, 1 };
			glLightfv(gl_light, GL_DIFFUSE, diffuse);
			glLightfv(gl_light, GL_SPECULAR, diffuse);
			glLightfv(gl_light, GL_AMBIENT, zero);

			GLfloat pos[4];
			if (light->type == VS::LIGHT_DIRECTIONAL) {
				// Godot directional lights shine along their transform's -Z;
				// GL's directional light position is the direction *toward*
				// the light, i.e. the opposite (+Z) of that.
				Vector3 dir = li->transform.basis.xform(Vector3(0, 0, 1));
				pos[0] = dir.x;
				pos[1] = dir.y;
				pos[2] = dir.z;
				pos[3] = 0.0f;
				glLightf(gl_light, GL_SPOT_CUTOFF, 180.0f);
			} else {
				Vector3 origin = li->transform.origin;
				pos[0] = origin.x;
				pos[1] = origin.y;
				pos[2] = origin.z;
				pos[3] = 1.0f;
				float range = MAX(light->param[VS::LIGHT_PARAM_RANGE], 0.01f);
				glLightf(gl_light, GL_CONSTANT_ATTENUATION, 1.0f);
				glLightf(gl_light, GL_LINEAR_ATTENUATION, 0.0f);
				glLightf(gl_light, GL_QUADRATIC_ATTENUATION, 1.0f / (range * range));
				if (light->type == VS::LIGHT_SPOT) {
					Vector3 spot_dir = li->transform.basis.xform(Vector3(0, 0, -1));
					GLfloat sdir[3] = { spot_dir.x, spot_dir.y, spot_dir.z };
					glLightfv(gl_light, GL_SPOT_DIRECTION, sdir);
					glLightf(gl_light, GL_SPOT_CUTOFF, CLAMP(Math::rad2deg(light->param[VS::LIGHT_PARAM_SPOT_ANGLE]), 0.0f, 90.0f));
					glLightf(gl_light, GL_SPOT_EXPONENT, light->param[VS::LIGHT_PARAM_SPOT_ATTENUATION] * 128.0f);
				} else {
					glLightf(gl_light, GL_SPOT_CUTOFF, 180.0f);
				}
			}
			glLightfv(gl_light, GL_POSITION, pos);
		}
	} else {
		glDisable(GL_LIGHTING);
	}
	for (int i = max_lights; i < 8; i++) {
		glDisable(GL_LIGHT0 + i);
	}

	for (int i = 0; i < p_cull_count; i++) {
		InstanceBase *instance = p_cull_result[i];
		if (!instance->visible || instance->base_type != VS::INSTANCE_MESH) {
			continue;
		}

		RasterizerStorageGLFF::Mesh *mesh = storage->mesh_owner.getornull(instance->base);
		if (!mesh) {
			continue;
		}

		glPushMatrix();
		GLfloat gl_model[16];
		_load_transform_gl(instance->transform, gl_model);
		glMultMatrixf(gl_model);

		for (int s = 0; s < mesh->surfaces.size(); s++) {
			RasterizerStorageGLFF::Surface *surface = mesh->surfaces[s];
			if (surface->vertex_count == 0) {
				continue;
			}

			RID mat_rid = instance->material_override.is_valid() ? instance->material_override : ((s < instance->materials.size() && instance->materials[s].is_valid()) ? instance->materials[s] : surface->material);
			RasterizerStorageGLFF::Material *mat = storage->material_owner.getornull(mat_rid);
			Color albedo = mat ? mat->albedo : Color(1, 1, 1, 1);
			RasterizerStorageGLFF::Texture *tex = (mat && mat->albedo_texture.is_valid()) ? storage->texture_owner.getornull(mat->albedo_texture) : nullptr;

			glColor4f(albedo.r, albedo.g, albedo.b, albedo.a);
			GLfloat mat_diffuse[4] = { albedo.r, albedo.g, albedo.b, albedo.a };
			glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_diffuse);
			glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mat_diffuse);

			if (albedo.a < 0.999f) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			} else {
				glDisable(GL_BLEND);
			}

			glEnableClientState(GL_VERTEX_ARRAY);
			PoolVector<Vector3>::Read vr = surface->vertices.read();
			glVertexPointer(3, GL_FLOAT, 0, vr.ptr());

			PoolVector<Vector3>::Read nr;
			if (surface->has_normals) {
				nr = surface->normals.read();
				glEnableClientState(GL_NORMAL_ARRAY);
				glNormalPointer(GL_FLOAT, 0, nr.ptr());
			} else {
				glDisableClientState(GL_NORMAL_ARRAY);
			}

			// Per-vertex color arrays and GL_LIGHTING interact via
			// GL_COLOR_MATERIAL, which we don't enable here -- a surface
			// with both vertex colors and active lighting draws using the
			// material's albedo only (documented simplification, no
			// content in this driver's target scope combines the two).
			PoolVector<Color>::Read cr;
			if (surface->has_colors && max_lights == 0) {
				cr = surface->colors.read();
				glEnableClientState(GL_COLOR_ARRAY);
				glColorPointer(4, GL_FLOAT, 0, cr.ptr());
			} else {
				glDisableClientState(GL_COLOR_ARRAY);
			}

			PoolVector<Vector2>::Read ur;
			if (surface->has_uvs && tex) {
				glEnable(GL_TEXTURE_2D);
				glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
				glBindTexture(GL_TEXTURE_2D, tex->tex_id);
				ur = surface->uvs.read();
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glTexCoordPointer(2, GL_FLOAT, 0, ur.ptr());
			} else {
				glDisable(GL_TEXTURE_2D);
				glDisableClientState(GL_TEXTURE_COORD_ARRAY);
			}

			GLenum gl_primitive = _primitive_to_gl(surface->primitive);

			if (surface->index_count > 0) {
				GLenum index_type = (surface->vertex_count >= (1 << 16)) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
				PoolVector<uint8_t>::Read ir = surface->index_array.read();
				glDrawElements(gl_primitive, surface->index_count, index_type, ir.ptr());
			} else {
				glDrawArrays(gl_primitive, 0, surface->vertex_count);
			}
		}

		glPopMatrix();
	}

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
}

bool RasterizerSceneGLFF::free(RID p_rid) {
	if (shadow_atlas_owner.owns(p_rid)) {
		memdelete(shadow_atlas_owner.getornull(p_rid));
		shadow_atlas_owner.free(p_rid);
		return true;
	} else if (environment_owner.owns(p_rid)) {
		memdelete(environment_owner.getornull(p_rid));
		environment_owner.free(p_rid);
		return true;
	} else if (light_instance_owner.owns(p_rid)) {
		memdelete(light_instance_owner.getornull(p_rid));
		light_instance_owner.free(p_rid);
		return true;
	} else if (reflection_atlas_owner.owns(p_rid)) {
		memdelete(reflection_atlas_owner.getornull(p_rid));
		reflection_atlas_owner.free(p_rid);
		return true;
	} else if (reflection_probe_instance_owner.owns(p_rid)) {
		memdelete(reflection_probe_instance_owner.getornull(p_rid));
		reflection_probe_instance_owner.free(p_rid);
		return true;
	} else if (gi_probe_instance_owner.owns(p_rid)) {
		memdelete(gi_probe_instance_owner.getornull(p_rid));
		gi_probe_instance_owner.free(p_rid);
		return true;
	}
	return false;
}

void RasterizerSceneGLFF::initialize() {
	// Godot's mesh winding convention is clockwise-front (matching GLES2/
	// GLES3's own glFrontFace(GL_CW) in their initialize()), not GL's
	// default CCW -- without this, render_scene()'s GL_CULL_FACE/GL_BACK
	// setup culls the real front faces and shows back faces instead.
	glFrontFace(GL_CW);
}

RasterizerSceneGLFF::RasterizerSceneGLFF() {
	storage = nullptr;
}

RasterizerSceneGLFF::~RasterizerSceneGLFF() {
}
