#include "rasterizer_scene_glff.h"

#include "rasterizer_storage_glff.h"
#include <stdio.h>
#include <string.h>

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

// Mirrors FixedFunctionMaterial::EnvMode (scene/resources/fixed_function_material.h)
// numerically -- drivers/ can't include scene/ headers, so the raw int
// values are translated here instead. Keep these two enums in sync.
static GLenum _ff_env_mode_to_gl(int p_mode) {
	switch (p_mode) {
		case 1: // ENV_REPLACE
			return GL_REPLACE;
		case 2: // ENV_DECAL
			return GL_DECAL;
		case 3: // ENV_BLEND
			return GL_BLEND;
		case 4: // ENV_COMBINE
			return GL_COMBINE;
		case 0: // ENV_MODULATE
		default:
			return GL_MODULATE;
	}
}

// Mirrors FixedFunctionMaterial::CombineFunc numerically. Only reached
// when env_mode is ENV_COMBINE and the driver actually has
// GL_ARB_texture_env_combine (checked by the caller) -- DOT3 additionally
// needs GL_ARB_texture_env_dot3, also checked by the caller, which
// substitutes GL_MODULATE instead when absent (e.g. Rage Pro/128, see
// godot-ports#25's hardware research) rather than emitting an enum the
// driver would reject.
static GLenum _ff_combine_func_to_gl(int p_func) {
	switch (p_func) {
		case 1: // COMBINE_ADD
			return GL_ADD;
		case 2: // COMBINE_SUBTRACT
			return GL_SUBTRACT;
		case 3: // COMBINE_DOT3
			return GL_DOT3_RGB;
		case 0: // COMBINE_MODULATE
		default:
			return GL_MODULATE;
	}
}

// Mirrors FixedFunctionMaterial::TexgenMode numerically. GL_SPHERE_MAP/
// GL_OBJECT_LINEAR/GL_EYE_LINEAR are all core since GL 1.0 and need no
// capability gate; GL_REFLECTION_MAP is gated by the caller
// (has_texgen_reflection_map) since it's GL 1.3/GL_NV_texgen_reflection.
static GLenum _ff_texgen_mode_to_gl(int p_mode) {
	switch (p_mode) {
		case 1: // TEXGEN_SPHERE_MAP
			return GL_SPHERE_MAP;
		case 2: // TEXGEN_REFLECTION_MAP
			return GL_REFLECTION_MAP;
		case 3: // TEXGEN_OBJECT_LINEAR
			return GL_OBJECT_LINEAR;
		case 4: // TEXGEN_EYE_LINEAR
			return GL_EYE_LINEAR;
		case 0: // TEXGEN_NONE
		default:
			return 0;
	}
}

// Sets up one texture unit's full fixed-function state (bind, env mode,
// real GL_COMBINE/dot3 combiner state, and texgen) for godot-ports#35's
// FixedFunctionMaterial. p_gl_texture_unit is GL_TEXTURE0+unit_index for
// glActiveTexture/glClientActiveTexture; p_second_operand_source is
// GL_PRIMARY_COLOR (unit 0, combining the texture against the surface's
// own per-vertex color) or GL_PREVIOUS (unit 1+, chaining against the
// prior stage's result) -- the standard multi-stage combiner pattern
// this authoring surface targets (godot-ports#25's dot3 bump-mapping
// technique: a base/diffuse unit feeding a normal-map unit in
// Combine+Dot3 mode). p_second_operand_source is IGNORED for a real
// Combine+Dot3 unit -- see the GL_DOT3_RGB branch below, which always
// dots the texture against p_dot3_light_direction (this material's one
// baked/static light direction) instead. Returns true if this unit ends
// up with real texture-coordinate-array data the caller should still
// supply (i.e. textured but NOT using texgen, which generates its own
// coordinates and makes the vertex array's UVs irrelevant for this unit).
static bool _ff_setup_texture_unit(RasterizerSceneGLFF *p_scene, GLenum p_gl_texture_unit, GLenum p_second_operand_source, RasterizerStorageGLFF::Texture *p_tex, int p_env_mode, int p_combine_func, int p_texgen_mode, const Vector3 &p_dot3_light_direction) {
	if (p_scene->has_multitexture) {
		glActiveTexture(p_gl_texture_unit);
		glClientActiveTexture(p_gl_texture_unit);
	}

	// Always reset texgen first -- a prior surface/unit may have left it
	// enabled, and this authoring surface has no other reset point since
	// draw order (and thus which unit last used texgen) isn't fixed.
	glDisable(GL_TEXTURE_GEN_S);
	glDisable(GL_TEXTURE_GEN_T);

	if (!p_tex) {
		glDisable(GL_TEXTURE_2D);
		return false;
	}

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, p_tex->tex_id);

	if (p_env_mode == 4 /* ENV_COMBINE */ && p_scene->has_texture_env_combine) {
		GLenum combine_func = _ff_combine_func_to_gl(p_combine_func);
		if (combine_func == GL_DOT3_RGB && !p_scene->has_texture_env_dot3) {
			combine_func = GL_MODULATE;
		}
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, combine_func);
		glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
		if (combine_func == GL_DOT3_RGB) {
			// godot-ports#25: a Dot3 unit's second operand is the ONE baked
			// light direction (this material's ff_dot3_light_direction,
			// object-space), not the vertex-color/previous-stage chain
			// p_second_operand_source represents for every other combine
			// func -- GL_CONSTANT + GL_TEXTURE_ENV_COLOR is the only way to
			// feed a fixed-function combiner stage an authored constant.
			// Encoded the same way any Dot3 bump-mapping texture is: a unit
			// vector's [-1,1] components packed into a color's [0,1] range.
			Vector3 dir = p_dot3_light_direction.normalized();
			GLfloat light_color[4] = {
				dir.x * 0.5f + 0.5f,
				dir.y * 0.5f + 0.5f,
				dir.z * 0.5f + 0.5f,
				1.0f
			};
			glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, light_color);
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_CONSTANT);
		} else {
			glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, p_second_operand_source);
		}
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
	} else {
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, _ff_env_mode_to_gl(p_env_mode));
	}

	GLenum texgen_gl = _ff_texgen_mode_to_gl(p_texgen_mode);
	if (texgen_gl == GL_REFLECTION_MAP && !p_scene->has_texgen_reflection_map) {
		texgen_gl = 0; // Degrade to no texgen (plain per-vertex UVs) rather than an unsupported mode.
	}
	if (texgen_gl != 0) {
		glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, texgen_gl);
		glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, texgen_gl);
		if (texgen_gl == GL_OBJECT_LINEAR || texgen_gl == GL_EYE_LINEAR) {
			// Simple planar projection (S along model/eye-space X, T along
			// Y) -- a fixed default, not a per-property-configurable plane
			// equation; sufficient to demonstrate/use these two texgen
			// modes without the larger scope of authoring arbitrary plane
			// vectors (deferred, see godot-ports#35's write-up).
			static const GLfloat plane_s[4] = { 1, 0, 0, 0 };
			static const GLfloat plane_t[4] = { 0, 1, 0, 0 };
			if (texgen_gl == GL_OBJECT_LINEAR) {
				glTexGenfv(GL_S, GL_OBJECT_PLANE, plane_s);
				glTexGenfv(GL_T, GL_OBJECT_PLANE, plane_t);
			} else {
				glTexGenfv(GL_S, GL_EYE_PLANE, plane_s);
				glTexGenfv(GL_T, GL_EYE_PLANE, plane_t);
			}
		}
		glEnable(GL_TEXTURE_GEN_S);
		glEnable(GL_TEXTURE_GEN_T);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		return false;
	}

	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	return true;
}

// godot-ports#30: a panorama sky background is ordinary textured geometry
// (a lat/long "skydome" sphere with equirectangular UVs, matching
// PanoramaSky's own texture layout: u = longitude/2pi, v = latitude/pi),
// not a fixed-function capability question at all -- the earlier "dropped,
// shader-only feature" call confused GLES2's specific *implementation*
// (a fullscreen shader pass sampling by view-ray direction) with the
// underlying concept. Generated once (lazily, on first real use) and
// cached for the process lifetime -- this geometry never changes.
struct _SkyboxVertex {
	GLfloat pos[3];
	GLfloat uv[2];
};
static Vector<_SkyboxVertex> *_skybox_verts = nullptr;
static const int SKYBOX_LAT_SEGMENTS = 16;
static const int SKYBOX_LON_SEGMENTS = 24;

static Vector<_SkyboxVertex> *_get_skybox_verts() {
	if (_skybox_verts) {
		return _skybox_verts;
	}
	_skybox_verts = memnew(Vector<_SkyboxVertex>);
	const float radius = 50.0f;
	for (int lat = 0; lat < SKYBOX_LAT_SEGMENTS; lat++) {
		float v0 = (float)lat / SKYBOX_LAT_SEGMENTS;
		float v1 = (float)(lat + 1) / SKYBOX_LAT_SEGMENTS;
		float theta0 = v0 * Math_PI;
		float theta1 = v1 * Math_PI;
		for (int lon = 0; lon < SKYBOX_LON_SEGMENTS; lon++) {
			float u0 = (float)lon / SKYBOX_LON_SEGMENTS;
			float u1 = (float)(lon + 1) / SKYBOX_LON_SEGMENTS;
			float phi0 = u0 * Math_PI * 2.0f;
			float phi1 = u1 * Math_PI * 2.0f;

			// Quad corners on the unit sphere (y = up, matching Godot's
			// convention); position is that unit vector times radius.
			// Winding is CW-front when viewed from INSIDE the sphere
			// (this backend's glFrontFace(GL_CW) convention, see
			// initialize()) -- a skydome is viewed from its interior, the
			// opposite of ordinary outward-facing scene geometry.
			auto vertex_at = [&](float theta, float phi, float u, float v) {
				_SkyboxVertex vx;
				float sin_theta = Math::sin(theta), cos_theta = Math::cos(theta);
				float sin_phi = Math::sin(phi), cos_phi = Math::cos(phi);
				vx.pos[0] = radius * sin_theta * cos_phi;
				vx.pos[1] = radius * cos_theta;
				vx.pos[2] = radius * sin_theta * sin_phi;
				vx.uv[0] = u;
				vx.uv[1] = v;
				return vx;
			};

			_SkyboxVertex v00 = vertex_at(theta0, phi0, u0, v0);
			_SkyboxVertex v10 = vertex_at(theta0, phi1, u1, v0);
			_SkyboxVertex v01 = vertex_at(theta1, phi0, u0, v1);
			_SkyboxVertex v11 = vertex_at(theta1, phi1, u1, v1);

			_skybox_verts->push_back(v00);
			_skybox_verts->push_back(v01);
			_skybox_verts->push_back(v10);

			_skybox_verts->push_back(v10);
			_skybox_verts->push_back(v01);
			_skybox_verts->push_back(v11);
		}
	}
	return _skybox_verts;
}

// Draws the skydome centered on the camera's world position (so it always
// surrounds the viewer regardless of movement) with a FIXED world
// orientation (no camera-rotation coupling -- a real static environment,
// not something that spins with the player). Called after the frame's
// initial clear and before the main opaque pass, with depth test/write
// both off (it must never occlude or be occluded by real geometry; drawn
// first, into an otherwise-empty depth buffer, is what makes it always
// appear "behind" everything else).
static void _draw_skybox(RasterizerStorageGLFF *p_storage, const Transform &p_cam_transform, RID p_panorama) {
	RasterizerStorageGLFF::Texture *tex = p_storage->texture_owner.getornull(p_panorama);
	if (tex) {
		tex = tex->get_ptr();
	}
	if (!tex) {
		return;
	}

	Vector<_SkyboxVertex> *verts = _get_skybox_verts();

	glPushMatrix();
	GLfloat gl_model[16];
	Transform camera_pos_only;
	camera_pos_only.origin = p_cam_transform.origin;
	_load_transform_gl(camera_pos_only, gl_model);
	glMultMatrixf(gl_model);

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_LIGHTING);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glColor4f(1, 1, 1, 1);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex->tex_id);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(_SkyboxVertex), &verts->ptr()[0].pos[0]);
	glTexCoordPointer(2, GL_FLOAT, sizeof(_SkyboxVertex), &verts->ptr()[0].uv[0]);
	glDrawArrays(GL_TRIANGLES, 0, verts->size());

	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
	glPopMatrix();
}

// godot-ports#31: whole-framebuffer glow/bloom. Fixed-function GL 1.2 has no
// shaders and no framebuffer objects, so this can't be a real HDR threshold
// pass -- it's the classic "capture the frame, force-sample a blurry small
// mip, blend it back additively" trick instead: cheap, texture-only, and
// entirely expressible with glCopyTexImage2D + GL_SGIS_generate_mipmap +
// GL_TEXTURE_BASE_LEVEL/MAX_LEVEL, all of which are present in this project's
// GL 1.2 floor (see has_generate_mipmap's capability comment in the header).
// Deliberately no threshold/luminance-cap/HDR-bleed support (environment_set_
// glow() ignores those params) -- everything above the ambient/opaque scene
// bleeds into the blur uniformly, which reads close enough to "glow" for
// this driver's vintage-hardware target and needs zero per-pixel math.
static int _next_pot(int p_value) {
	int p = 1;
	while (p < p_value) {
		p <<= 1;
	}
	return p;
}

void RasterizerSceneGLFF::_draw_glow(float p_intensity) {
	if (!has_generate_mipmap) {
		return;
	}

	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	int vp_w = viewport[2];
	int vp_h = viewport[3];
	if (vp_w <= 0 || vp_h <= 0) {
		return;
	}

	// This driver (a real, live-tested finding on G4/RV250 -- confirmed via
	// glGetError() returning GL_INVALID_VALUE, not a spec assumption)
	// rejects glCopyTexImage2D at the viewport's actual size whenever
	// either dimension isn't a power of two (a 2002-era chip, predating
	// GL_ARB_texture_non_power_of_two). The standard fix: allocate the
	// texture once at POT size via glTexImage2D (a plain empty allocation,
	// no capture involved, so its size is unconstrained), then update just
	// the used bottom-left sub-rectangle every frame via
	// glCopyTexSubImage2D, which has no such POT requirement. Sample only
	// that sub-rectangle's UV range thereafter (see quad_uv below) --
	// never the full [0,1] range, which would also pull in the unused
	// (undefined) padding this texture carries whenever vp_w/vp_h aren't
	// already POT.
	int pot_w = _next_pot(vp_w);
	int pot_h = _next_pot(vp_h);

	if (glow_capture_tex == 0) {
		glGenTextures(1, &glow_capture_tex);
	}
	glBindTexture(GL_TEXTURE_2D, glow_capture_tex);

	if (pot_w != glow_capture_pot_w || pot_h != glow_capture_pot_h) {
		glow_capture_pot_w = pot_w;
		glow_capture_pot_h = pot_h;
		glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP_SGIS, GL_TRUE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, pot_w, pot_h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
	}

	// Capturing into a texture with GL_GENERATE_MIPMAP_SGIS enabled makes
	// the driver regenerate the entire mip chain as a side effect of this
	// call -- a real box-filtered downsample series, "for free."
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1], vp_w, vp_h);

	// Force sampling from a small mip level instead of level 0 -- this is
	// what actually produces the blur; ordinary GL_LINEAR magnification of
	// a tiny image back up to full-screen size does the rest. Deliberately
	// shallow (capped at 3, i.e. 1/8th resolution) rather than descending
	// as deep as possible, matching this same live-tested caution as the
	// POT fix above: this 2002-era driver's GL_SGIS_generate_mipmap chain
	// is not assumed trustworthy many levels down without hardware
	// verification, and a shallow level still gives a visible, if softer,
	// bloom.
	int level = 0;
	int w = pot_w, h = pot_h;
	while (level < 3 && w > 2 && h > 2) {
		level++;
		w >>= 1;
		h >>= 1;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, level);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, level);

	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0, 1, 0, 1, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_LIGHTING);
	glDisable(GL_CULL_FACE);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);

	glEnable(GL_TEXTURE_2D);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	float intensity = CLAMP(p_intensity, 0.0f, 4.0f);
	glColor4f(intensity, intensity, intensity, 1.0f);

	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_ONE, GL_ONE);

	// UVs sample only the real (bottom-left) sub-rectangle actually
	// written by glCopyTexSubImage2D above, not the full [0,1] range --
	// the rest of this POT-sized texture is unused padding.
	float u_max = (float)vp_w / (float)pot_w;
	float v_max = (float)vp_h / (float)pot_h;
	const GLfloat quad_pos[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
	const GLfloat quad_uv[8] = { 0, 0, u_max, 0, u_max, v_max, 0, v_max };
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, quad_pos);
	glTexCoordPointer(2, GL_FLOAT, 0, quad_uv);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

	glDisable(GL_BLEND);
	glColor4f(1, 1, 1, 1);

	// Reset the mip clamp back to the full chain -- this texture object is
	// reused frame to frame (recaptured, not recreated), so a stale forced-
	// small-mip range must not survive past this draw.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 1000);

	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();

	glDepthMask(GL_TRUE);
	glEnable(GL_DEPTH_TEST);
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
	// also VS::ENV_BG_CLEAR_COLOR) must NOT touch the color buffer:
	// VisualServerViewport::_draw_viewport() (servers/visual/
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

	// godot-ports#30: real panorama sky, drawn as an ordinary textured
	// skydome -- see _draw_skybox()'s own comment for why this isn't a
	// fixed-function capability question at all. Drawn right after the
	// view/projection matrices are set (it needs them, like any other
	// geometry) and before the main opaque pass, with its own internal
	// depth test/write override so it never occludes or is occluded by
	// real content regardless of draw order after this point.
	if (env && env->sky.is_valid() && (env->bg_mode == VS::ENV_BG_SKY || env->bg_mode == VS::ENV_BG_COLOR_SKY)) {
		RasterizerStorageGLFF::Sky *sky = storage->sky_owner.getornull(env->sky);
		if (sky && sky->panorama.is_valid()) {
			_draw_skybox(storage, p_cam_transform, sky->panorama);
		}
	}

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

	// Two passes: real (depth-tested) scene content first, then anything
	// whose material requests depth_test_disable (editor gizmos/handles,
	// "always on top" overlays) last. This backend has no FBO/depth
	// texture to give "on top" content real depth protection against
	// content drawn after it, so draw ORDER is the only thing that can
	// guarantee it -- p_cull_result's order comes straight from the
	// octree/frustum culling pass and is NOT guaranteed to already put
	// opaque content before on-top content (godot-ports#28: confirmed via
	// incremental isolation testing that the move-gizmo was reliably
	// getting overwritten by the Ground cube's opaque draw whenever the
	// cube happened to be culled/ordered after the gizmo for a given
	// frame -- exactly reproducing the user's "camera-angle-dependent,
	// abrupt disappearance" report, and explained by the octree's
	// traversal order changing with camera position/angle).
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < p_cull_count; i++) {
			InstanceBase *instance = p_cull_result[i];
			if (!instance->visible || instance->base_type != VS::INSTANCE_MESH) {
				continue;
			}

			RasterizerStorageGLFF::Mesh *mesh = storage->mesh_owner.getornull(instance->base);
			if (!mesh) {
				continue;
			}

			bool matrix_pushed = false;

			// godot-ports#34: real LightmapCapture ambient. lightmap_capture_data
			// is 12 real captured colors (one per fixed cone-trace direction,
			// baked by existing backend-independent engine code, see
			// servers/visual/visual_server_scene.cpp's
			// _update_instance_lightmap_captures) -- no per-pixel directional
			// reconstruction here (that needs real per-fragment math this
			// backend doesn't have), just their flat average as this
			// instance's own GL_LIGHT_MODEL_AMBIENT override, restored back
			// to the scene's own ambient_color once this instance's surfaces
			// are done (see the matching restore at this loop's matrix_pushed
			// cleanup below).
			Color instance_ambient = ambient_color;
			if (instance->lightmap_capture_data.size() == 12) {
				float r = 0, g = 0, b = 0;
				for (int c = 0; c < 12; c++) {
					r += instance->lightmap_capture_data[c].r;
					g += instance->lightmap_capture_data[c].g;
					b += instance->lightmap_capture_data[c].b;
				}
				instance_ambient = Color(r / 12.0f, g / 12.0f, b / 12.0f, 1.0f);
			}

			for (int s = 0; s < mesh->surfaces.size(); s++) {
				RasterizerStorageGLFF::Surface *surface = mesh->surfaces[s];
				if (surface->vertex_count == 0) {
					continue;
				}

				RID mat_rid = instance->material_override.is_valid() ? instance->material_override : ((s < instance->materials.size() && instance->materials[s].is_valid()) ? instance->materials[s] : surface->material);
				RasterizerStorageGLFF::Material *mat = storage->material_owner.getornull(mat_rid);
				RasterizerStorageGLFF::Shader *shader = (mat && mat->shader.is_valid()) ? storage->shader_owner.getornull(mat->shader) : nullptr;

				bool surface_on_top = shader && shader->depth_test_disabled;
				if (surface_on_top != (pass == 1)) {
					continue;
				}

				if (!matrix_pushed) {
					glPushMatrix();
					GLfloat gl_model[16];
					_load_transform_gl(instance->transform, gl_model);
					glMultMatrixf(gl_model);
					matrix_pushed = true;

					GLfloat inst_amb[4] = { instance_ambient.r, instance_ambient.g, instance_ambient.b, 1.0f };
					glLightModelfv(GL_LIGHT_MODEL_AMBIENT, inst_amb);
				}

				Color albedo = mat ? mat->albedo : Color(1, 1, 1, 1);
				RasterizerStorageGLFF::Texture *tex = (mat && mat->albedo_texture.is_valid()) ? storage->texture_owner.getornull(mat->albedo_texture) : nullptr;
				if (tex) {
					// resolve ViewportTexture proxies (e.g. a SubViewport used as
					// a material's albedo texture) -- see the Texture::proxy
					// comment in rasterizer_storage_glff.h (godot-ports#28).
					tex = tex->get_ptr();
				}

				glColor4f(albedo.r, albedo.g, albedo.b, albedo.a);
				GLfloat mat_diffuse[4] = { albedo.r, albedo.g, albedo.b, albedo.a };
				glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_diffuse);
				glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mat_diffuse);

				// godot-ports#24 (Phase 4 remainder): FLAG_DISABLE_AMBIENT_LIGHT.
				// GL's own lighting equation is
				// ambient_contrib = GL_LIGHT_MODEL_AMBIENT * GL_AMBIENT(material),
				// computed independently of GL_DIFFUSE -- overriding just the
				// material's own GL_AMBIENT to black zeroes the scene-ambient
				// contribution for this surface while direct-light diffuse/
				// specular continue normally, a real (not approximated) match
				// for what this flag means. Not exposed on FixedFunctionMaterial
				// (godot-ports#35 didn't request it), always explicitly set
				// either way to avoid leaking a previous surface's state.
				bool effective_ambient_disabled = (mat && mat->ff_active) ? false : (shader && shader->ambient_light_disabled);
				if (effective_ambient_disabled) {
					GLfloat zero_ambient[4] = { 0, 0, 0, 1 };
					glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, zero_ambient);
				}

				// godot-ports#24: FEATURE_EMISSION -> GL_EMISSION (core GL 1.0,
				// a genuine direct map, not an approximation). Always set
				// explicitly (including the disabled/black case) since
				// GL_EMISSION is sticky material state that would otherwise
				// leak into a following surface with no emission at all.
				bool effective_emission_enabled = (mat && mat->ff_active) ? false : (shader && shader->emission_enabled);
				if (effective_emission_enabled && mat) {
					GLfloat mat_emission[4] = { mat->emission.r * mat->emission_energy, mat->emission.g * mat->emission_energy, mat->emission.b * mat->emission_energy, 1.0f };
					glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, mat_emission);
				} else {
					GLfloat zero_emission[4] = { 0, 0, 0, 1 };
					glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero_emission);
				}

				// godot-ports#24: SPECULAR_PHONG approximation. Fixed-function
				// GL_LIGHTING's built-in specular term is itself a Blinn-Phong
				// model (not true Phong), the natural fixed-function
				// equivalent -- SpatialMaterial's "specular" (0..1 intensity)
				// and "roughness" (0..1, inverted here into a GL_SHININESS
				// exponent) drive it directly. specular_disabled (a
				// render_mode token alongside specular_schlick_ggx/toon --
				// none of which have a further fixed-function equivalent
				// beyond this same Blinn-Phong approximation) zeroes it
				// instead, a real, not approximated, "no specular" result.
				bool effective_specular_disabled = (mat && mat->ff_active) ? true : (shader && shader->specular_disabled);
				if (!effective_specular_disabled && mat) {
					GLfloat spec = mat->specular;
					GLfloat mat_specular[4] = { spec, spec, spec, 1.0f };
					glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, mat_specular);
					GLfloat shininess = CLAMP((1.0f - mat->roughness) * 128.0f, 0.0f, 128.0f);
					glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, shininess);
				} else {
					GLfloat zero_specular[4] = { 0, 0, 0, 1 };
					glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, zero_specular);
					glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 0.0f);
				}

				// godot-ports#24: FLAG_USE_ALPHA_SCISSOR -> glAlphaFunc, a real
				// direct map (core GL 1.0). Always explicitly enabled/disabled
				// per surface to avoid leaking into unrelated draws.
				bool effective_use_alpha_scissor = (mat && mat->ff_active) ? false : (shader && shader->use_alpha_scissor);
				if (effective_use_alpha_scissor && mat) {
					glEnable(GL_ALPHA_TEST);
					glAlphaFunc(GL_GREATER, mat->alpha_scissor_threshold);
				} else {
					glDisable(GL_ALPHA_TEST);
				}

				// Per-material blend mode (godot-ports#24/#17): MIX is the
				// default alpha-blend-if-transparent behavior already in
				// place; ADD/MUL/SUB are real GL blend-equation/-func direct
				// maps, not approximations (SUB needs glBlendEquation, core
				// GL 1.4 -- a real finding from the proposal's material
				// research, not an oversight of the strict-1.2 floor).
				// FixedFunctionMaterial (godot-ports#35) exposes this same
				// blend_mode directly as its own property (criterion 4) --
				// mat->ff_blend_mode, not a Shader/render_mode string, since
				// this material type never has a Shader at all.
				RasterizerStorageGLFF::GLFFBlendMode effective_blend_mode = (mat && mat->ff_active) ? mat->ff_blend_mode : (shader ? shader->blend_mode : RasterizerStorageGLFF::GLFF_BLEND_MIX);
				glBlendEquation(GL_FUNC_ADD);
				if (effective_blend_mode == RasterizerStorageGLFF::GLFF_BLEND_ADD) {
					glEnable(GL_BLEND);
					glBlendFunc(GL_SRC_ALPHA, GL_ONE);
				} else if (effective_blend_mode == RasterizerStorageGLFF::GLFF_BLEND_MUL) {
					glEnable(GL_BLEND);
					glBlendFunc(GL_DST_COLOR, GL_ZERO);
				} else if (effective_blend_mode == RasterizerStorageGLFF::GLFF_BLEND_SUB) {
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
				RasterizerStorageGLFF::GLFFCullMode effective_cull_mode = (mat && mat->ff_active) ? mat->ff_cull_mode : (shader ? shader->cull_mode : RasterizerStorageGLFF::GLFF_CULL_BACK);
				bool effective_unshaded_for_cull = (mat && mat->ff_active) ? mat->ff_unshaded : (shader && shader->unshaded);
				if (effective_cull_mode == RasterizerStorageGLFF::GLFF_CULL_FRONT) {
					glEnable(GL_CULL_FACE);
					glCullFace(GL_FRONT);
				} else if (effective_cull_mode == RasterizerStorageGLFF::GLFF_CULL_DISABLED || effective_unshaded_for_cull) {
					glDisable(GL_CULL_FACE);
				} else {
					glEnable(GL_CULL_FACE);
					glCullFace(GL_BACK);
				}

				bool effective_depth_test_disabled = (mat && mat->ff_active) ? mat->ff_depth_test_disabled : (shader && shader->depth_test_disabled);
				if (effective_depth_test_disabled) {
					glDisable(GL_DEPTH_TEST);
				} else {
					glEnable(GL_DEPTH_TEST);
				}

				// FLAG_UNSHADED: this surface ignores GL_LIGHT0-7 regardless
				// of whether lighting is on for the rest of the scene.
				// FixedFunctionMaterial (godot-ports#35) exposes this
				// directly too (mat->ff_unshaded), same reasoning as
				// blend/cull above.
				bool surface_unshaded = (mat && mat->ff_active) ? mat->ff_unshaded : (shader && shader->unshaded);
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

				// godot-ports#24: FLAG_ALBEDO_FROM_VERTEX_COLOR. A *shaded*
				// surface requesting this needs GL_COLOR_MATERIAL enabled so
				// the bound per-vertex color array actually feeds the
				// GL_LIGHTING equation's ambient+diffuse material term,
				// instead of glColor4f()'s single flat "current color" above
				// -- the real fixed-function equivalent of this flag (a
				// genuine direct map, not an approximation). Always
				// explicitly enabled/disabled per surface to avoid leaking
				// into unrelated draws. Not exposed on FixedFunctionMaterial
				// (godot-ports#35 didn't request it).
				bool effective_albedo_from_vertex_color = (mat && mat->ff_active) ? false : (shader && shader->albedo_from_vertex_color);
				if (effective_albedo_from_vertex_color) {
					glEnable(GL_COLOR_MATERIAL);
					glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
				} else {
					glDisable(GL_COLOR_MATERIAL);
				}

				// Per-vertex color arrays: the unshaded/max_lights==0 cases
				// (editor gizmos -- axis lines, move/rotate/scale handles,
				// always-unshaded per-vertex-colored geometry, godot-ports#28)
				// draw via glColor4f()'s "current color" directly, no
				// GL_COLOR_MATERIAL needed since lighting is off entirely.
				// effective_albedo_from_vertex_color (just above) is the
				// separate *shaded*-surface case, needing both the array
				// AND GL_COLOR_MATERIAL together.
				PoolVector<Color>::Read cr;
				if (surface->has_colors && (surface_unshaded || max_lights == 0 || effective_albedo_from_vertex_color)) {
					cr = surface->colors.read();
					glEnableClientState(GL_COLOR_ARRAY);
					glColorPointer(4, GL_FLOAT, 0, cr.ptr());
				} else {
					glDisableClientState(GL_COLOR_ARRAY);
				}

				PoolVector<Vector2>::Read ur;
				int highest_unit_used = -1;
				if (mat && mat->ff_active) {
					// godot-ports#35: FixedFunctionMaterial's real
					// multi-texture-unit path. Every unit (when usable --
					// unit 0 always, units 1+ only when has_multitexture)
					// samples the SAME uv set -- this authoring surface
					// targets the classic multi-stage compositing layout
					// (base color feeding a normal-map/detail unit in
					// Combine+Dot3 mode, etc.), not independent UV2-style
					// per-unit coordinates. How many of
					// RasterizerStorageGLFF::FF_TEXTURE_UNIT_MAX units are
					// actually iterated is capped by max_texture_units --
					// real detected hardware capability, regardless of
					// what the material or its project target-GPU tier
					// declare (see rasterizer_storage_glff.h's
					// FF_TEXTURE_UNIT_MAX comment).
					if (surface->has_uvs) {
						ur = surface->uvs.read();
					}

					int unit_cap = MIN(RasterizerStorageGLFF::FF_TEXTURE_UNIT_MAX, max_texture_units);
					for (int u = 0; u < unit_cap; u++) {
						if (u > 0 && !has_multitexture) {
							break;
						}
						RasterizerStorageGLFF::Texture *ff_tex = mat->ff_tex[u].is_valid() ? storage->texture_owner.getornull(mat->ff_tex[u]) : nullptr;
						if (ff_tex) {
							ff_tex = ff_tex->get_ptr();
						}
						GLenum second_operand_source = (u == 0) ? GL_PRIMARY_COLOR : GL_PREVIOUS;
						bool wants_uv_array = _ff_setup_texture_unit(this, GL_TEXTURE0 + u, second_operand_source, ff_tex, mat->ff_env_mode[u], mat->ff_combine_func[u], mat->ff_texgen_mode[u], mat->ff_dot3_light_direction);
						if (ff_tex && wants_uv_array && surface->has_uvs) {
							glTexCoordPointer(2, GL_FLOAT, 0, ur.ptr());
						} else if (ff_tex && wants_uv_array) {
							glDisableClientState(GL_TEXTURE_COORD_ARRAY);
						}
						if (ff_tex) {
							highest_unit_used = u;
						}
					}
				} else if (surface->has_uvs && tex) {
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

				// godot-ports#23: baked lightmap modulation, a second draw
				// pass over the SAME geometry (not a single-pass multitexture
				// combine, per godot-ports#16's design -- strict GL 1.2 can't
				// assume GL_ARB_multitexture, but a second pass works on any
				// GL 1.0+ implementation). instance->lightmap/lightmap_uv_rect
				// are set by VisualServerScene::instance_set_use_lightmap()
				// (core, unrelated to LightmapCapture -- see the Surface
				// struct's uv2 comment) for a *static* baked-lightmap mesh.
				// glDepthFunc(GL_EQUAL) + a fresh GL_DST_COLOR/GL_ZERO
				// multiply blend means this pass only darkens/tints exactly
				// the fragments the opaque pass just drew, by exactly the
				// lightmap's own baked color -- gated on pass==0 (opaque
				// only, never the on-top/gizmo pass) and !surface_unshaded
				// (matches GLES2's own gating: an unshaded surface ignores
				// all lighting, lightmap included).
				if (pass == 0 && !surface_unshaded && surface->has_uv2 && instance->lightmap.is_valid()) {
					RasterizerStorageGLFF::Texture *lightmap_tex = storage->texture_owner.getornull(instance->lightmap);
					if (lightmap_tex) {
						lightmap_tex = lightmap_tex->get_ptr();
					}
					if (lightmap_tex) {
						PoolVector<Vector2>::Read u2r = surface->uv2.read();

						glActiveTexture(GL_TEXTURE0);
						glClientActiveTexture(GL_TEXTURE0);
						glEnable(GL_TEXTURE_2D);
						glBindTexture(GL_TEXTURE_2D, lightmap_tex->tex_id);
						glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
						glEnableClientState(GL_TEXTURE_COORD_ARRAY);
						glTexCoordPointer(2, GL_FLOAT, 0, u2r.ptr());

						// lightmap_uv_rect remaps UV2 for atlas-packed
						// lightmap textures (a plain scale+offset) -- the
						// fixed-function texture matrix does this for free,
						// no need to touch the vertex array itself.
						glMatrixMode(GL_TEXTURE);
						glPushMatrix();
						glLoadIdentity();
						const Rect2 &uv_rect = instance->lightmap_uv_rect;
						glTranslatef(uv_rect.position.x, uv_rect.position.y, 0.0f);
						glScalef(uv_rect.size.x, uv_rect.size.y, 1.0f);
						glMatrixMode(GL_MODELVIEW);

						glDisableClientState(GL_COLOR_ARRAY);
						glColor4f(1, 1, 1, 1);
						glDisable(GL_LIGHTING);

						glDepthFunc(GL_EQUAL);
						glDepthMask(GL_FALSE);
						glEnable(GL_BLEND);
						glBlendEquation(GL_FUNC_ADD);
						glBlendFunc(GL_DST_COLOR, GL_ZERO);

						if (surface->index_count > 0) {
							GLenum index_type = (surface->vertex_count >= (1 << 16)) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
							PoolVector<uint8_t>::Read ir = surface->index_array.read();
							glDrawElements(gl_primitive, surface->index_count, index_type, ir.ptr());
						} else {
							glDrawArrays(gl_primitive, 0, surface->vertex_count);
						}

						glDepthFunc(GL_LEQUAL);
						glDepthMask(GL_TRUE);
						if (!surface_unshaded && max_lights > 0) {
							glEnable(GL_LIGHTING);
						}
						glMatrixMode(GL_TEXTURE);
						glPopMatrix();
						glMatrixMode(GL_MODELVIEW);

						// Restore this surface's own blend state -- the
						// lightmap pass forced GL_DST_COLOR/GL_ZERO above,
						// unconditionally enabled, regardless of what the
						// surface's own effective_blend_mode/albedo alpha
						// established earlier in this same iteration.
						glBlendEquation(GL_FUNC_ADD);
						if (effective_blend_mode == RasterizerStorageGLFF::GLFF_BLEND_ADD) {
							glEnable(GL_BLEND);
							glBlendFunc(GL_SRC_ALPHA, GL_ONE);
						} else if (effective_blend_mode == RasterizerStorageGLFF::GLFF_BLEND_MUL) {
							glEnable(GL_BLEND);
							glBlendFunc(GL_DST_COLOR, GL_ZERO);
						} else if (effective_blend_mode == RasterizerStorageGLFF::GLFF_BLEND_SUB) {
							glEnable(GL_BLEND);
							glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
							glBlendFunc(GL_SRC_ALPHA, GL_ONE);
						} else if (albedo.a < 0.999f) {
							glEnable(GL_BLEND);
							glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
						} else {
							glDisable(GL_BLEND);
						}
					}
				}

				// Leave texture-unit state back at unit 0 for every other
				// code path (2D canvas rendering, other materials in this
				// same pass) that assumes GL_TEXTURE0 is always the active
				// unit and never touches multitexture at all.
				if (has_multitexture && (highest_unit_used > 0 || (mat && mat->ff_active))) {
					glClientActiveTexture(GL_TEXTURE0);
					glActiveTexture(GL_TEXTURE0);
				}
			}

			if (matrix_pushed) {
				GLfloat amb[4] = { ambient_color.r, ambient_color.g, ambient_color.b, 1.0f };
				glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
				glPopMatrix();
			}
		}
	}

	// godot-ports#31: capture+blur+blend the fully-composited opaque/
	// blended scene built up above, before the cleanup below tears down
	// the client-state this function's own draws relied on. Must run
	// before that cleanup (glow re-establishes and then re-tears-down its
	// own subset of it) and, being a screen-space post-process, is
	// correctly placed after every real 3D draw call for this frame and
	// before the 2D canvas pass that runs after render_scene() returns.
	if (env && env->glow_enabled) {
		_draw_glow(env->glow_intensity);
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

	// godot-ports#35: real capability check, not an assumption -- see the
	// has_multitexture/has_texture_env_combine/has_texture_env_dot3
	// comment in the header for why these specific three extensions.
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	has_multitexture = ext && strstr(ext, "GL_ARB_multitexture") != nullptr;
	has_texture_env_combine = ext && (strstr(ext, "GL_ARB_texture_env_combine") != nullptr || strstr(ext, "GL_EXT_texture_env_combine") != nullptr);
	has_texture_env_dot3 = ext && (strstr(ext, "GL_ARB_texture_env_dot3") != nullptr || strstr(ext, "GL_EXT_texture_env_dot3") != nullptr);

	max_texture_units = 1;
	if (has_multitexture) {
		GLint gl_max_texture_units = 1;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, &gl_max_texture_units);
		max_texture_units = MAX(1, (int)gl_max_texture_units);
	}

	// GL_REFLECTION_MAP texgen mode is core since GL 1.3, or available via
	// GL_NV_texgen_reflection on older hardware -- parse the leading
	// "major.minor" out of GL_VERSION (format is "<major>.<minor> <vendor
	// string>", e.g. "1.3 ATI-1.4.18") rather than assuming a specific
	// driver's string layout beyond that leading version token.
	has_texgen_reflection_map = ext && strstr(ext, "GL_NV_texgen_reflection") != nullptr;
	if (!has_texgen_reflection_map) {
		const char *ver = (const char *)glGetString(GL_VERSION);
		int major = 0, minor = 0;
		if (ver && sscanf(ver, "%d.%d", &major, &minor) == 2) {
			if (major > 1 || (major == 1 && minor >= 3)) {
				has_texgen_reflection_map = true;
			}
		}
	}

	// godot-ports#31: GL_SGIS_generate_mipmap gives us real driver-generated
	// mip levels from a single glCopyTexImage2D capture, which is what makes
	// a cheap fixed-function-era blur (force-sample a small mip, let normal
	// bilinear magnification do the blur) possible at all.
	has_generate_mipmap = ext && strstr(ext, "GL_SGIS_generate_mipmap") != nullptr;
}

RasterizerSceneGLFF::RasterizerSceneGLFF() {
	storage = nullptr;
	glow_capture_tex = 0;
	glow_capture_pot_w = 0;
	glow_capture_pot_h = 0;
}

RasterizerSceneGLFF::~RasterizerSceneGLFF() {
}
