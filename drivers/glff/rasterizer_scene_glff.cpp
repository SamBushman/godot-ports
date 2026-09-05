#include "rasterizer_scene_glff.h"

#include "rasterizer_storage_glff.h"
#include <stdio.h>

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
	Color ambient_color(0, 0, 0, 1);
	Environment *env = p_environment.is_valid() ? environment_owner.getornull(p_environment) : nullptr;
	if (env) {
		ambient_color = Color(env->ambient_color.r * env->ambient_energy, env->ambient_color.g * env->ambient_energy, env->ambient_color.b * env->ambient_energy, 1.0);
	}

	// Only explicitly re-clear the color buffer here when a real Environment
	// requests a specific solid background (ENV_BG_COLOR/CANVAS/COLOR_SKY).
	// Every other case (no Environment at all -- true for the editor's own
	// 3D viewport camera and any scene without a WorldEnvironment node, and
	// also VS::ENV_BG_CLEAR_COLOR/ENV_BG_SKY, sky being dropped/degraded to
	// clear-color per this backend's design) must NOT touch the color
	// buffer: VisualServerViewport::_draw_viewport() (servers/visual/
	// visual_server_viewport.cpp:99) already called clear_render_target()
	// with the correct default (the "rendering/environment/default_clear_color"
	// project setting, a light grey, not black) before this function runs.
	// An earlier version of this code unconditionally re-cleared to a
	// hardcoded Color(0,0,0,1) fallback whenever no Environment was set,
	// stomping that correct clear with solid black -- exactly the case hit
	// by the editor's own camera, which has no Environment (godot-ports#28).
	// Matches GLES2's identical precedence (rasterizer_scene_gles2.cpp,
	// around its own "clear color" comment).
	if (env && (env->bg_mode == VS::ENV_BG_COLOR || env->bg_mode == VS::ENV_BG_CANVAS || env->bg_mode == VS::ENV_BG_COLOR_SKY)) {
		glClearColor(env->bg_color.r, env->bg_color.g, env->bg_color.b, 1.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	} else {
		glClear(GL_DEPTH_BUFFER_BIT);
	}

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

			// TEMPORARY incremental isolation test (godot-ports#28): gizmo-
			// only, then gizmo+grid+origin-lines, both confirmed reliable
			// by the user. Adding back the Ground CubeMesh next (a
			// standard Godot PrimitiveMesh cube, exactly 24 verts -- 4 per
			// face x 6 faces -- confirmed via Main.tscn's
			// SubResource(CubeMesh, size 60x2x60)) while still excluding
			// the Player/Mob character meshes (hundreds+ verts from
			// imported .glb data) to narrow down the conflict further.
			bool is_gizmo_arrow = (surface->vertex_count == 384);
			bool is_grid_or_origin_lines = (surface->primitive == VS::PRIMITIVE_LINES);
			bool is_ground_cube = (surface->vertex_count == 24);
			if (!is_gizmo_arrow && !is_grid_or_origin_lines && !is_ground_cube) {
				continue;
			}

			RID mat_rid = instance->material_override.is_valid() ? instance->material_override : ((s < instance->materials.size() && instance->materials[s].is_valid()) ? instance->materials[s] : surface->material);
			RasterizerStorageGLFF::Material *mat = storage->material_owner.getornull(mat_rid);
			Color albedo = mat ? mat->albedo : Color(1, 1, 1, 1);
			RasterizerStorageGLFF::Texture *tex = (mat && mat->albedo_texture.is_valid()) ? storage->texture_owner.getornull(mat->albedo_texture) : nullptr;
			if (tex) {
				// resolve ViewportTexture proxies (e.g. a SubViewport used as
				// a material's albedo texture) -- see the Texture::proxy
				// comment in rasterizer_storage_glff.h (godot-ports#28).
				tex = tex->get_ptr();
			}
			RasterizerStorageGLFF::Shader *shader = (mat && mat->shader.is_valid()) ? storage->shader_owner.getornull(mat->shader) : nullptr;

			glColor4f(albedo.r, albedo.g, albedo.b, albedo.a);
			GLfloat mat_diffuse[4] = { albedo.r, albedo.g, albedo.b, albedo.a };
			glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_diffuse);
			glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mat_diffuse);

			// Per-material blend mode (godot-ports#24/#17): MIX is the
			// default alpha-blend-if-transparent behavior already in
			// place; ADD/MUL/SUB are real GL blend-equation/-func direct
			// maps, not approximations (SUB needs glBlendEquation, core
			// GL 1.4 -- a real finding from the proposal's material
			// research, not an oversight of the strict-1.2 floor).
			glBlendEquation(GL_FUNC_ADD);
			if (shader && shader->blend_mode == RasterizerStorageGLFF::GLFF_BLEND_ADD) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			} else if (shader && shader->blend_mode == RasterizerStorageGLFF::GLFF_BLEND_MUL) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_DST_COLOR, GL_ZERO);
			} else if (shader && shader->blend_mode == RasterizerStorageGLFF::GLFF_BLEND_SUB) {
				glEnable(GL_BLEND);
				glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE);
			} else if (albedo.a < 0.999f) {
				glEnable(GL_BLEND);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			} else {
				glDisable(GL_BLEND);
			}

			// Per-material cull mode override (default is the GL_BACK set
			// once per-frame above). Unshaded surfaces (editor gizmos,
			// axis lines -- always unshaded, see surface_unshaded below)
			// also skip backface culling by default, regardless of
			// cull_mode: this fixed-function backend has NO CHOICE but to
			// rely on hardware glFrontFace/glCullFace for culling (unlike
			// GLES2/GLES3, which compute front/back-facing per-fragment
			// in a shader, decoupled from hardware winding state) -- so
			// GLFF is the first backend where a real winding mismatch
			// between imported glTF content (reordered to match Godot's
			// CW-front convention at import time) and procedurally-
			// authored SurfaceTool geometry (gizmos, never reordered,
			// possibly GL's native CCW-front) actually matters. This
			// exactly matches the reported symptom (godot-ports#28): the
			// move-gizmo's per-axis cone abruptly vanishing/reappearing
			// as the camera rotates, with no visible size change --
			// classic binary backface-cull behavior, not a scale/culling-
			// frustum/depth issue (all independently ruled out first).
			if (shader && shader->cull_mode == RasterizerStorageGLFF::GLFF_CULL_FRONT) {
				glEnable(GL_CULL_FACE);
				glCullFace(GL_FRONT);
			} else if ((shader && shader->cull_mode == RasterizerStorageGLFF::GLFF_CULL_DISABLED) || (shader && shader->unshaded)) {
				glDisable(GL_CULL_FACE);
			} else {
				glEnable(GL_CULL_FACE);
				glCullFace(GL_BACK);
			}

			if (shader && shader->depth_test_disabled) {
				glDisable(GL_DEPTH_TEST);
			} else {
				glEnable(GL_DEPTH_TEST);
			}

			// FLAG_UNSHADED: this surface ignores GL_LIGHT0-7 regardless
			// of whether lighting is on for the rest of the scene.
			bool surface_unshaded = shader && shader->unshaded;
			if (surface_unshaded) {
				glDisable(GL_LIGHTING);
			} else if (max_lights > 0) {
				glEnable(GL_LIGHTING);
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
			// GL_COLOR_MATERIAL, which we don't enable here -- a *shaded*
			// surface with both vertex colors and active lighting draws
			// using the material's albedo only (documented simplification,
			// no content in this driver's target scope combines the two).
			// This must NOT gate on the scene-wide max_lights count, though
			// -- editor gizmos (axis lines, move/rotate/scale handles) are
			// real per-vertex-colored (red/green/blue per axis), always-
			// unshaded geometry (surface_unshaded above), and were going
			// uncolored in any scene with a real light (e.g. the "Squash
			// the Creeps" tutorial's DirectionalLight), since max_lights>0
			// disabled vertex colors globally regardless of whether THIS
			// surface even has lighting enabled (godot-ports#28).
			PoolVector<Color>::Read cr;
			if (surface->has_colors && (surface_unshaded || max_lights == 0)) {
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

			bool trace_this = (surface->vertex_count == 384);
			if (trace_this) {
				GLfloat mv[16];
				glGetFloatv(GL_MODELVIEW_MATRIX, mv);

				// Find the vertex farthest from the local origin -- for this
				// arrow-cone geometry that's the tip/apex -- to directly
				// test the "gizmo scale places 2 of 3 axes' geometry
				// outside the view frustum" hypothesis (godot-ports#28):
				// transform it through the full MVP and check its clip-
				// space coordinates against the standard -w..w frustum
				// bounds, rather than continuing to infer this indirectly.
				PoolVector<Vector3>::Read vr2 = surface->vertices.read();
				int farthest_idx = 0;
				float farthest_d2 = 0;
				for (int vi = 0; vi < surface->vertex_count; vi++) {
					float d2 = vr2[vi].length_squared();
					if (d2 > farthest_d2) {
						farthest_d2 = d2;
						farthest_idx = vi;
					}
				}
				Vector3 tip = vr2[farthest_idx];

				// mv is column-major (GL convention): eye = mv * local.
				float ex = mv[0] * tip.x + mv[4] * tip.y + mv[8] * tip.z + mv[12];
				float ey = mv[1] * tip.x + mv[5] * tip.y + mv[9] * tip.z + mv[13];
				float ez = mv[2] * tip.x + mv[6] * tip.y + mv[10] * tip.z + mv[14];
				float ew = mv[3] * tip.x + mv[7] * tip.y + mv[11] * tip.z + mv[15];
				// gl_proj (declared at the top of render_scene()) is also
				// column-major.
				float cx = gl_proj[0] * ex + gl_proj[4] * ey + gl_proj[8] * ez + gl_proj[12] * ew;
				float cy = gl_proj[1] * ex + gl_proj[5] * ey + gl_proj[9] * ez + gl_proj[13] * ew;
				float cz = gl_proj[2] * ex + gl_proj[6] * ey + gl_proj[10] * ez + gl_proj[14] * ew;
				float cw = gl_proj[3] * ex + gl_proj[7] * ey + gl_proj[11] * ez + gl_proj[15] * ew;

				static long frame_counter = 0;
				frame_counter++;
				fprintf(stderr, "GLFF DEBUG: gizmo-arrow frame=%ld inst=%p albedo=(%.2f,%.2f,%.2f) tip_local=(%.3f,%.3f,%.3f) clip=(%.2f,%.2f,%.2f,%.2f) ndc=(%.2f,%.2f,%.2f)\n",
						frame_counter, (void *)instance, albedo.r, albedo.g, albedo.b,
						tip.x, tip.y, tip.z, cx, cy, cz, cw,
						cw != 0 ? cx / cw : 0, cw != 0 ? cy / cw : 0, cw != 0 ? cz / cw : 0);
				fflush(stderr);
			}

			if (surface->index_count > 0) {
				GLenum index_type = (surface->vertex_count >= (1 << 16)) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
				PoolVector<uint8_t>::Read ir = surface->index_array.read();
				glDrawElements(gl_primitive, surface->index_count, index_type, ir.ptr());
			} else {
				glDrawArrays(gl_primitive, 0, surface->vertex_count);
			}

			if (trace_this) {
				GLenum err = glGetError();
				fprintf(stderr, "GLFF DEBUG: gizmo-arrow post-draw err=0x%x\n", (unsigned)err);
				fflush(stderr);
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
	// A BLEND_MODE_SUB surface leaves GL_FUNC_REVERSE_SUBTRACT active --
	// canvas_begin() (2D pass, runs right after this) never touches the
	// blend equation itself, only glBlendFunc, so this must be reset here
	// or every subsequent 2D draw this frame silently blends with the
	// wrong arithmetic operator.
	glBlendEquation(GL_FUNC_ADD);
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
