#include "rasterizer_scene_glff.h"

#include "rasterizer_storage_glff.h"
#include "core/map.h"
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
// godot-ports#42: non-POT textures allocate real GL storage at the next
// POT size (godot-ports#40) and only populate their top-left
// logical-size sub-rect -- 3D consumers (unlike 2D canvas commands,
// which rebuild a fresh UV array per draw) read mesh-baked UVs from a
// shared vertex array that can't be rescaled in place, so this rescales
// via the fixed-function texture matrix instead, transparently on top
// of whatever coordinates (vertex-array or texgen-generated) actually
// reach the unit. A no-op (identity scale) whenever a texture's real
// size is already POT.
static void _get_tex_uv_scale_3d(const RasterizerStorageGLFF::Texture *p_tex, float &r_scale_u, float &r_scale_v) {
	r_scale_u = 1.0f;
	r_scale_v = 1.0f;
	if (p_tex && p_tex->gl_alloc_width > 0 && p_tex->gl_alloc_height > 0) {
		r_scale_u = (float)p_tex->width / (float)p_tex->gl_alloc_width;
		r_scale_v = (float)p_tex->height / (float)p_tex->gl_alloc_height;
	}
}

// Sets (or resets to identity) the CURRENTLY ACTIVE texture unit's texture
// matrix -- caller must have already called glActiveTexture() for the
// unit it wants affected. Always called, never conditionally skipped, so
// no surface/unit can ever leak a stale non-identity scale into a later
// draw that isn't expecting one.
static void _set_tex_matrix_scale(const RasterizerStorageGLFF::Texture *p_tex) {
	float scale_u, scale_v;
	_get_tex_uv_scale_3d(p_tex, scale_u, scale_v);
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	if (scale_u != 1.0f || scale_v != 1.0f) {
		glScalef(scale_u, scale_v, 1.0f);
	}
	glMatrixMode(GL_MODELVIEW);
}

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
		_set_tex_matrix_scale(nullptr); // reset to identity -- this unit may be reused unscaled elsewhere
		return false;
	}

	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, p_tex->tex_id);
	_set_tex_matrix_scale(p_tex);

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

	// godot-ports#42: rescale for a non-POT panorama texture (godot-ports#40
	// pads its real GL storage to the next POT size) -- pushed/popped so
	// this self-contained function can never leak a non-identity texture
	// matrix into the main pass that follows.
	float scale_u, scale_v;
	_get_tex_uv_scale_3d(tex, scale_u, scale_v);
	glMatrixMode(GL_TEXTURE);
	glPushMatrix();
	glLoadIdentity();
	if (scale_u != 1.0f || scale_v != 1.0f) {
		glScalef(scale_u, scale_v, 1.0f);
	}
	glMatrixMode(GL_MODELVIEW);

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(3, GL_FLOAT, sizeof(_SkyboxVertex), &verts->ptr()[0].pos[0]);
	glTexCoordPointer(2, GL_FLOAT, sizeof(_SkyboxVertex), &verts->ptr()[0].uv[0]);
	glDrawArrays(GL_TRIANGLES, 0, verts->size());

	glMatrixMode(GL_TEXTURE);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);

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

// godot-ports#26: real two-pass (Heidmann/"z-pass") stencil shadow volumes
// for exactly one primary shadow-casting DirectionalLight -- see this
// issue's own reopened success criteria for the full scope statement.
// Everything below is real, working geometry construction + GL state, not
// a spike: verified live on G4/RV250 (see render_scene()'s own call site
// comment further down for the frame-level integration).
//
// _build_shadow_volume_triangles() builds a CLOSED volume (front cap +
// extruded silhouette side walls + reverse-wound back cap) in the
// SURFACE's own object space, given the light direction already converted
// to that same object space (mirrors godot-ports#38's own Dot3-dynamic-
// light object-space conversion, same reasoning: direction vectors are
// scale/translation-independent, so this avoids re-transforming every
// vertex to world space just to build shadow geometry). Capping is
// REQUIRED for z-pass to count correctly -- it's not an optional nicety,
// unlike a z-fail ("Carmack's Reverse") implementation, which trades the
// capping requirement for a stencil-wrap requirement this GL 1.2 hardware
// doesn't reliably have without an unchecked extension. Known, documented
// limitation of z-pass specifically: if the CAMERA itself ends up inside a
// shadow volume, the near clip plane can clip away part of the volume's
// own front cap and the stencil count goes wrong for that frame -- not
// fixed this pass (z-fail avoids it but isn't a small change on this
// hardware); acceptable for the scoped single-primary-light/simple-content
// case this issue targets.
// godot-ports#26 perf fix: builds (once, cached on the Surface -- see
// RasterizerStorageGLFF::Surface::shadow_tri_indices/shadow_edges) the
// mesh-topology-only edge adjacency this function used to rebuild from
// scratch every single call. Purely a function of vertex/index data, never
// the light direction, so it's safe to share across every instance/every
// frame that uses this surface until the underlying data changes
// (invalidated in _decode_surface_arrays()).
static void _build_shadow_topology_if_needed(RasterizerStorageGLFF::Surface *p_surface) {
	if (p_surface->shadow_topology_built) {
		return;
	}
	p_surface->shadow_topology_built = true;
	p_surface->shadow_tri_indices.clear();
	p_surface->shadow_edges.clear();
	p_surface->shadow_tri_edges.clear();

	if (p_surface->primitive != VS::PRIMITIVE_TRIANGLES || p_surface->index_count < 3 || p_surface->vertex_count == 0) {
		return;
	}

	int tri_count = p_surface->index_count / 3;
	PoolVector<uint8_t>::Read ir = p_surface->index_array.read();
	bool use_32 = p_surface->vertex_count >= (1 << 16);
	const uint16_t *idx16 = use_32 ? nullptr : (const uint16_t *)ir.ptr();
	const uint32_t *idx32 = use_32 ? (const uint32_t *)ir.ptr() : nullptr;

	p_surface->shadow_tri_indices.resize(tri_count * 3);
	for (int t = 0; t < tri_count; t++) {
		p_surface->shadow_tri_indices.write[t * 3 + 0] = use_32 ? (int)idx32[t * 3 + 0] : (int)idx16[t * 3 + 0];
		p_surface->shadow_tri_indices.write[t * 3 + 1] = use_32 ? (int)idx32[t * 3 + 1] : (int)idx16[t * 3 + 1];
		p_surface->shadow_tri_indices.write[t * 3 + 2] = use_32 ? (int)idx32[t * 3 + 2] : (int)idx16[t * 3 + 2];
	}

	// godot-ports#48: compute + cache each triangle's raw (unnormalized)
	// object-space geometric normal ONCE here, instead of recomputing the
	// same cross product every frame in _build_shadow_volume_triangles()
	// for a light direction that's the only thing that actually changes
	// frame to frame. See Surface::shadow_tri_normal_x's field comment.
	{
		PoolVector<Vector3>::Read norm_vr = p_surface->vertices.read();
		const int *norm_tri_idx = p_surface->shadow_tri_indices.ptr();
		p_surface->shadow_tri_normal_x.resize(tri_count);
		p_surface->shadow_tri_normal_y.resize(tri_count);
		p_surface->shadow_tri_normal_z.resize(tri_count);
		// godot-ports#53: each triangle's normal LENGTH, cached here too
		// (not just its direction) -- the temporal-coherence reuse-or-
		// retest bound needs |n| every frame to compare a raw
		// (unnormalized) dot product against a normalized-space margin,
		// and recomputing sqrt(nx^2+ny^2+nz^2) per triangle per frame was
		// measured to cost MORE than the exact per-triangle test it was
		// meant to help avoid -- caching it once here (same lifecycle as
		// the normal components themselves) makes the per-frame check
		// pure multiply/compare, no sqrt.
		p_surface->shadow_tri_normal_len.resize(tri_count);
		for (int t = 0; t < tri_count; t++) {
			const Vector3 &v0 = norm_vr[norm_tri_idx[t * 3 + 0]];
			const Vector3 &v1 = norm_vr[norm_tri_idx[t * 3 + 1]];
			const Vector3 &v2 = norm_vr[norm_tri_idx[t * 3 + 2]];
			Vector3 n = (v1 - v0).cross(v2 - v0); // not normalized -- only the dot's sign matters
			p_surface->shadow_tri_normal_x.write[t] = n.x;
			p_surface->shadow_tri_normal_y.write[t] = n.y;
			p_surface->shadow_tri_normal_z.write[t] = n.z;
			p_surface->shadow_tri_normal_len.write[t] = (float)n.length();
		}
	}

	// godot-ports#26 bugfix: real meshes (Godot's own CubeMesh, any glTF
	// export like this project's mob.glb/player.glb) duplicate vertices at
	// every hard edge/UV seam so each face can carry its own normal/UV --
	// e.g. a cube has 24 vertex-buffer entries for 8 geometric corners,
	// never sharing an index across faces. Keying silhouette-edge
	// adjacency off raw vertex INDEX (as this used to) therefore can never
	// recognize a shared edge between two faces at all -- every hard edge
	// misreads as a mesh boundary regardless of true adjacency, producing
	// a wildly over-extruded, self-intersecting shadow volume. Its net
	// stencil count then depends sensitively on exact orientation: fixed
	// (if wrong) for a static caster, so no visible symptom -- but as the
	// caster rotates (this project's own idle-bob AnimationPlayer, used by
	// both Player and every Mob) which triangles are light-facing keeps
	// changing, so the (wrong) volume's silhouette shape and net count
	// keep changing too, producing genuine per-frame stencil-count
	// instability -- confirmed via a minimal isolated repro (a plain
	// rotating CubeMesh box): motionless, no flicker; rotating, flickers
	// heavily. Fix: weld vertices by POSITION (quantized, since real
	// duplicate-for-normals/UVs vertices share bit-identical or
	// near-identical positions) before computing edge keys, so two
	// duplicate-but-coincident vertices across a real hard edge resolve to
	// the same welded id and the adjacency test sees the true topology.
	PoolVector<Vector3>::Read weld_vr = p_surface->vertices.read();
	Map<uint64_t, int> pos_to_welded;
	Vector<int> welded_id;
	welded_id.resize(p_surface->vertex_count);
	for (int v = 0; v < p_surface->vertex_count; v++) {
		const Vector3 &p = weld_vr[v];
		int64_t xi = (int64_t)Math::round((double)p.x * 4096.0);
		int64_t yi = (int64_t)Math::round((double)p.y * 4096.0);
		int64_t zi = (int64_t)Math::round((double)p.z * 4096.0);
		uint64_t h = 1469598103934665603ULL; // FNV-1a offset basis
		h = (h ^ (uint64_t)xi) * 1099511628211ULL;
		h = (h ^ (uint64_t)yi) * 1099511628211ULL;
		h = (h ^ (uint64_t)zi) * 1099511628211ULL;
		Map<uint64_t, int>::Element *WE = pos_to_welded.find(h);
		if (WE) {
			welded_id.write[v] = WE->value();
		} else {
			int new_id = pos_to_welded.size();
			pos_to_welded[h] = new_id;
			welded_id.write[v] = new_id;
		}
	}

	// Canonical edge key: pack two (unordered) WELDED vertex ids into one
	// 64-bit key -- real welded-id counts are always far under 2^32, so
	// this never collides. Only used here, once, at build time --
	// shadow_tri_edges below is what lets the per-frame pass skip
	// re-keying/re-searching entirely.
	Map<uint64_t, int> key_to_edge;
	p_surface->shadow_tri_edges.resize(tri_count * 3);
	for (int t = 0; t < tri_count; t++) {
		for (int e = 0; e < 3; e++) {
			int a = p_surface->shadow_tri_indices[t * 3 + e];
			int b = p_surface->shadow_tri_indices[t * 3 + (e + 1) % 3];
			int wa = welded_id[a];
			int wb = welded_id[b];
			uint64_t key = ((uint64_t)MIN(wa, wb) << 32) | (uint32_t)MAX(wa, wb);
			Map<uint64_t, int>::Element *E = key_to_edge.find(key);
			int edge_idx;
			if (E) {
				edge_idx = E->value();
			} else {
				edge_idx = p_surface->shadow_edges.size();
				RasterizerStorageGLFF::Surface::ShadowEdge se;
				se.va = a;
				se.vb = b;
				p_surface->shadow_edges.push_back(se);
				key_to_edge[key] = edge_idx;
			}
			p_surface->shadow_edges.write[edge_idx].owner_tris.push_back(t);
			p_surface->shadow_tri_edges.write[t * 3 + e] = edge_idx;
		}
	}

	// godot-ports#48 phase 2: flatten each edge's owner_tris (a separate
	// heap-allocated Vector<int> per edge -- see the field comment on
	// shadow_edge_owner0/1) into two parallel arrays indexed directly by
	// edge index, so the per-frame silhouette test doesn't have to follow
	// shadow_edges[ei].owner_tris.ptr() (a scattered, individually
	// allocated buffer per edge) just to read 1-2 ints.
	int edge_count = p_surface->shadow_edges.size();
	p_surface->shadow_edge_owner0.resize(edge_count);
	p_surface->shadow_edge_owner1.resize(edge_count);
	for (int ei = 0; ei < edge_count; ei++) {
		const Vector<int> &owners = p_surface->shadow_edges[ei].owner_tris;
		p_surface->shadow_edge_owner0.write[ei] = owners[0];
		if (owners.size() == 1) {
			p_surface->shadow_edge_owner1.write[ei] = -1; // boundary edge
		} else if (owners.size() == 2) {
			p_surface->shadow_edge_owner1.write[ei] = owners[1];
		} else {
			p_surface->shadow_edge_owner1.write[ei] = -2; // non-manifold overflow -- caller falls back to owner_tris
		}
	}
}

// godot-ports#49: flat 6-bucket normal-cone bounding hierarchy, built
// lazily (only the first time a surface is actually asked to use the
// NORMAL_CONE algorithm -- most content stays on the default FULL
// algorithm and never allocates this). See the field comments on
// RasterizerStorageGLFF::Surface::shadow_cluster_* for what each array
// holds; this function only fills them in, using the SAME cached
// (unnormalized) per-triangle normals _build_shadow_topology_if_needed()
// already computed for the plain per-triangle test -- no new geometry
// reads, no new cross products.
static void _build_shadow_clusters_if_needed(RasterizerStorageGLFF::Surface *p_surface) {
	_build_shadow_topology_if_needed(p_surface);
	if (p_surface->shadow_cluster_built) {
		return;
	}
	p_surface->shadow_cluster_built = true;

	int tri_count = p_surface->shadow_tri_indices.size() / 3;
	if (tri_count == 0) {
		return;
	}

	static const Vector3 CLUSTER_DIRS[6] = {
		Vector3(1, 0, 0), Vector3(-1, 0, 0),
		Vector3(0, 1, 0), Vector3(0, -1, 0),
		Vector3(0, 0, 1), Vector3(0, 0, -1)
	};

	const float *nx = p_surface->shadow_tri_normal_x.ptr();
	const float *ny = p_surface->shadow_tri_normal_y.ptr();
	const float *nz = p_surface->shadow_tri_normal_z.ptr();

	// Pass 1: classify each triangle into whichever of the 6 cluster
	// directions its (normalized) cached normal is closest to (max dot),
	// and count cluster sizes for a contiguous counting sort in pass 2.
	Vector<int> cluster_of_tri;
	cluster_of_tri.resize(tri_count);
	int counts[6] = { 0, 0, 0, 0, 0, 0 };
	for (int t = 0; t < tri_count; t++) {
		Vector3 n(nx[t], ny[t], nz[t]);
		real_t len = n.length();
		int best = 0;
		if (len > CMP_EPSILON) {
			n /= len;
			real_t best_dot = -2.0;
			for (int c = 0; c < 6; c++) {
				real_t d = n.dot(CLUSTER_DIRS[c]);
				if (d > best_dot) {
					best_dot = d;
					best = c;
				}
			}
		}
		// Degenerate (zero-area) triangle: len <= CMP_EPSILON, arbitrarily
		// assigned to cluster 0. Harmless either way -- a degenerate
		// triangle contributes nothing to the emitted shadow geometry
		// regardless of which cluster's verdict it inherits.
		cluster_of_tri.write[t] = best;
		counts[best]++;
	}

	// Pass 2: counting sort into shadow_cluster_tri_order, contiguous per
	// cluster (shadow_cluster_tri_start[c]..+shadow_cluster_tri_count[c]).
	int offsets[6];
	offsets[0] = 0;
	for (int c = 1; c < 6; c++) {
		offsets[c] = offsets[c - 1] + counts[c - 1];
	}
	for (int c = 0; c < 6; c++) {
		p_surface->shadow_cluster_tri_start[c] = offsets[c];
		p_surface->shadow_cluster_tri_count[c] = counts[c];
		p_surface->shadow_cluster_dir[c] = CLUSTER_DIRS[c];
	}
	p_surface->shadow_cluster_tri_order.resize(tri_count);
	{
		int cursor[6] = { offsets[0], offsets[1], offsets[2], offsets[3], offsets[4], offsets[5] };
		for (int t = 0; t < tri_count; t++) {
			int c = cluster_of_tri[t];
			p_surface->shadow_cluster_tri_order.write[cursor[c]++] = t;
		}
	}

	// Pass 3: per cluster, find the widest angle (smallest dot) between
	// the cluster's own representative direction and any triangle normal
	// actually assigned to it -- this is alpha, the half-angle margin.
	// NOT optional/approximate: a per-frame verdict of "every triangle in
	// this cluster is definitely lit/dark" is only provably correct if it
	// accounts for the cluster's full angular spread, not just its
	// average direction (see _build_shadow_volume_triangles()'s use of
	// shadow_cluster_sin_alpha for the exact test this guards).
	const int *order = p_surface->shadow_cluster_tri_order.ptr();
	for (int c = 0; c < 6; c++) {
		real_t min_dot = 1.0;
		int start = p_surface->shadow_cluster_tri_start[c];
		int count = p_surface->shadow_cluster_tri_count[c];
		for (int i = 0; i < count; i++) {
			int t = order[start + i];
			Vector3 n(nx[t], ny[t], nz[t]);
			real_t len = n.length();
			if (len > CMP_EPSILON) {
				n /= len;
				real_t d = n.dot(CLUSTER_DIRS[c]);
				if (d < min_dot) {
					min_dot = d;
				}
			}
		}
		real_t alpha = (count > 0) ? Math::acos(CLAMP(min_dot, (real_t)-1.0, (real_t)1.0)) : 0.0;
		p_surface->shadow_cluster_sin_alpha[c] = (float)Math::sin(alpha);
	}
}

// godot-ports#50: exact closed-form bound for dot(n, p_light_dir_objspace)
// over every unit normal n whose angle from the mesh's local +Y axis lies
// in [p_beta_lo, p_beta_hi] and whose azimuth around Y is completely free
// (a real ring covers the full 2*PI azimuth by construction -- see
// create_mesh_array() in scene/resources/primitive_meshes.cpp).
//
// Not a heuristic/sampled bound -- exact, derived as follows. Writing a
// candidate normal as n = (sin(beta)*cos(phi), cos(beta), sin(beta)*sin(phi))
// and L = (lx, ly, lz), with lxz = sqrt(lx^2+lz^2) and phi_L = atan2(lz,lx):
//   dot(n,L) = ly*cos(beta) + sin(beta)*lxz*cos(phi - phi_L)
// As phi sweeps the ring's full 2*PI, cos(phi-phi_L) sweeps [-1,1], so for
// a FIXED beta:
//   min over phi = ly*cos(beta) - lxz*sin(beta) = R*cos(beta + delta)
//   max over phi = ly*cos(beta) + lxz*sin(beta) = R*cos(beta - delta)
// where R = sqrt(ly^2+lxz^2) = |L| and delta = atan2(lxz, ly) (this is
// where cos(delta)=ly/R, sin(delta)=lxz/R comes from). These are TWO
// DIFFERENT cosines of beta (opposite-signed phase shift) -- min and max
// are each then just that cosine's own min/max over beta in
// [p_beta_lo, p_beta_hi], handled exactly below (endpoints plus any
// PI/2*PI crossing inside the range), not sampled/approximated.
static float _cos_range_extreme(float p_lo, float p_hi, bool p_want_max) {
	float c_lo = Math::cos((double)p_lo);
	float c_hi = Math::cos((double)p_hi);
	float result = p_want_max ? MAX(c_lo, c_hi) : MIN(c_lo, c_hi);
	if (p_want_max) {
		// Does [p_lo, p_hi] contain a point where theta is a multiple of
		// 2*PI (cos == +1, the unconstrained max)?
		double k = Math::floor((double)p_lo / (2.0 * Math_PI));
		double candidate = k * 2.0 * Math_PI;
		if (candidate < (double)p_lo) {
			candidate += 2.0 * Math_PI;
		}
		if (candidate <= (double)p_hi) {
			result = 1.0f;
		}
	} else {
		// Does it contain a point where theta is an odd multiple of PI
		// (cos == -1, the unconstrained min)?
		double k = Math::floor(((double)p_lo - Math_PI) / (2.0 * Math_PI));
		double candidate = Math_PI + k * 2.0 * Math_PI;
		if (candidate < (double)p_lo) {
			candidate += 2.0 * Math_PI;
		}
		if (candidate <= (double)p_hi) {
			result = -1.0f;
		}
	}
	return result;
}

static void _ring_dot_bounds(float p_beta_lo, float p_beta_hi, const Vector3 &p_light_dir_objspace, float &r_min_dot, float &r_max_dot) {
	float lxz = Math::sqrt(p_light_dir_objspace.x * p_light_dir_objspace.x + p_light_dir_objspace.z * p_light_dir_objspace.z);
	float ly = p_light_dir_objspace.y;
	float R = Math::sqrt(lxz * lxz + ly * ly);
	if (R < CMP_EPSILON) {
		// Degenerate: light direction has ~no component in this mesh's own
		// (Y, XZ-radius) plane at all -- vanishingly rare, but handle it
		// safely rather than divide by (near-)zero. Every dot is ~0 here;
		// report full ambiguity so the caller always falls back per-triangle.
		r_min_dot = -1.0f;
		r_max_dot = 1.0f;
		return;
	}
	float delta = Math::atan2(lxz, ly);
	// min uses R*cos(beta + delta); max uses R*cos(beta - delta) -- two
	// DIFFERENT phase-shifted ranges of beta, not the same one.
	float cmin = _cos_range_extreme(p_beta_lo + delta, p_beta_hi + delta, false);
	float cmax = _cos_range_extreme(p_beta_lo - delta, p_beta_hi - delta, true);
	r_min_dot = R * cmin;
	r_max_dot = R * cmax;
}

// godot-ports#50: ring-level coarse cull build, lazy + keyed on the
// (radial_segments, rings) pair actually requested (see the field comment
// on RasterizerStorageGLFF::Surface::shadow_ring_built_for_* for why).
// Requires the topology cache (for tri_count + cached triangle normals)
// but is otherwise independent of the #49 cluster cache.
static void _build_shadow_rings_if_needed(RasterizerStorageGLFF::Surface *p_surface, int p_radial_segments, int p_rings) {
	_build_shadow_topology_if_needed(p_surface);
	if (p_surface->shadow_ring_built && p_surface->shadow_ring_built_for_radial_segments == p_radial_segments && p_surface->shadow_ring_built_for_rings == p_rings) {
		return;
	}
	p_surface->shadow_ring_built = true;
	p_surface->shadow_ring_built_for_radial_segments = p_radial_segments;
	p_surface->shadow_ring_built_for_rings = p_rings;
	p_surface->shadow_ring_actual_count = 0;
	p_surface->shadow_ring_tri_start.clear();
	p_surface->shadow_ring_tri_count.clear();
	p_surface->shadow_ring_min_cos_from_y.clear();
	p_surface->shadow_ring_max_cos_from_y.clear();

	int tri_count = p_surface->shadow_tri_indices.size() / 3;
	if (p_radial_segments <= 0 || p_rings < 0 || tri_count == 0) {
		return; // no real ring topology declared -- leave empty, caller falls back to FULL
	}

	int side_rings = p_rings + 1; // create_mesh_array() emits rings+1 latitude bands
	int tris_per_ring = 2 * p_radial_segments;
	int side_tri_count = tris_per_ring * side_rings;
	if (side_tri_count <= 0 || side_tri_count > tri_count) {
		// Doesn't match this surface's real triangle count -- misconfigured
		// (radial_segments/rings pushed don't describe the actual mesh).
		// Leave the ring cache empty rather than reading past real data;
		// caller falls back to FULL for the whole surface.
		return;
	}

	int total_rings = side_rings + ((tri_count > side_tri_count) ? 1 : 0); // + 1 synthetic catch-all for any trailing (cap) triangles
	p_surface->shadow_ring_actual_count = total_rings;
	p_surface->shadow_ring_tri_start.resize(total_rings);
	p_surface->shadow_ring_tri_count.resize(total_rings);
	p_surface->shadow_ring_min_cos_from_y.resize(total_rings);
	p_surface->shadow_ring_max_cos_from_y.resize(total_rings);
	p_surface->shadow_ring_has_degenerate.resize(total_rings);

	const float *nx = p_surface->shadow_tri_normal_x.ptr();
	const float *ny = p_surface->shadow_tri_normal_y.ptr();
	const float *nz = p_surface->shadow_tri_normal_z.ptr();

	for (int r = 0; r < side_rings; r++) {
		int start = r * tris_per_ring;
		p_surface->shadow_ring_tri_start.write[r] = start;
		p_surface->shadow_ring_tri_count.write[r] = tris_per_ring;
		float min_cy = 1.0f, max_cy = -1.0f;
		bool has_degenerate = false;
		for (int i = 0; i < tris_per_ring; i++) {
			int t = start + i;
			Vector3 n(nx[t], ny[t], nz[t]);
			real_t len = n.length();
			if (len > CMP_EPSILON) {
				float cy = (float)(n.y / len);
				if (cy < min_cy) {
					min_cy = cy;
				}
				if (cy > max_cy) {
					max_cy = cy;
				}
			} else {
				// godot-ports#50 bugfix: a zero-area triangle (both pole
				// rings have half their triangles collapse to this, per
				// create_mesh_array()'s own row-0/row-(rings+1) generation)
				// has dot(normal, L) == 0 for ANY light direction under
				// FULL's exact test (`> 0.0f` is false) -- it can never
				// legitimately be swept into a ring's "definitely lit"
				// blanket verdict. Flagging the whole ring for per-triangle
				// fallback is simpler and safer than tracking exactly
				// which triangle indices are degenerate.
				has_degenerate = true;
			}
		}
		if (min_cy > max_cy) {
			// Every triangle in this ring was degenerate (zero-area) --
			// can't bound anything real, report full ambiguity (safe).
			min_cy = -1.0f;
			max_cy = 1.0f;
		}
		p_surface->shadow_ring_min_cos_from_y.write[r] = min_cy;
		p_surface->shadow_ring_max_cos_from_y.write[r] = max_cy;
		p_surface->shadow_ring_has_degenerate.write[r] = has_degenerate;
	}

	if (tri_count > side_tri_count) {
		// Synthetic catch-all bucket for cap/leftover triangles this ring
		// model doesn't describe -- [-1, 1] cos-from-Y range always spans
		// the full possible bound, so the per-frame test below always
		// treats it as ambiguous and falls back to exact per-triangle,
		// with zero special-casing needed at that call site.
		int last = side_rings;
		p_surface->shadow_ring_tri_start.write[last] = side_tri_count;
		p_surface->shadow_ring_tri_count.write[last] = tri_count - side_tri_count;
		p_surface->shadow_ring_min_cos_from_y.write[last] = -1.0f;
		p_surface->shadow_ring_max_cos_from_y.write[last] = 1.0f;
		p_surface->shadow_ring_has_degenerate.write[last] = false; // irrelevant -- [-1,1] always falls back anyway
	}
}

// godot-ports#53: temporal coherence. p_prev_faces_light/p_prev_light_dir
// (both null unless the caller actually has a previous frame's state for
// THIS exact instance+surface -- see the call site) enable a per-triangle
// reuse-or-retest decision that is PROVEN correct, not a heuristic margin:
// for a normalized triangle normal n and light directions L_old (last
// frame) / L_new (this frame), Cauchy-Schwarz gives
//   |dot(n, L_new) - dot(n, L_old)| == |dot(n, L_new - L_old)| <= |L_new - L_old|
// so if |dot(n, L_old)| (the OLD classification's distance from the zero/
// facing threshold) exceeds margin = |L_new - L_old| (the exact chord
// distance between the two unit light directions), dot(n, L_new) is
// PROVABLY on the same side of zero as dot(n, L_old) -- the cached
// classification can be reused with zero risk of being wrong, at any
// rotation speed. A fast rotation just makes margin large, so more
// triangles fail this test and fall through to an exact per-triangle
// retest -- degrading gracefully to the same cost as no coherence at all,
// never silently wrong. No periodic full-recompute safety net is needed
// because there is no accumulated drift to correct for: margin is
// recomputed fresh from the actual frame-to-frame light delta every call.
// godot-ports#55 perf fix: the caller used to reset r_triangles between
// frames via `vol_tris.resize(0)`, which for CowData means "free the
// buffer entirely" (see core/cowdata.h's resize()), not "keep capacity,
// reset size" -- so any animating caster's persistent per-instance
// vol_tris buffer was regrowing from a null pointer every single frame
// (~14 reallocations, doubling from 0 up to its real size) instead of
// settling into a stable buffer reused frame to frame. Measured ~71%
// faster emission-phase time and ~27% less total per-frame shadow cost
// with the fix (see #55 for the full before/after breakdown). Writes now
// go through r_triangles.write[(*r_write_idx)++] = ... at fixed indices
// instead of push_back(), into a buffer the caller guarantees is already
// sized to this call's real worst case (24 verts/triangle -- 2 cap tris
// + up to 3 silhouette-edge quads, 6 verts each -- see the ensure-
// capacity block just below), so nothing in this function ever grows/
// reallocates r_triangles itself.
static void _build_shadow_volume_triangles(RasterizerStorageGLFF::Surface *p_surface, const Vector3 &p_light_dir_objspace, float p_extrude_distance, VS::ShadowSilhouetteAlgorithm p_algorithm, int p_ring_radial_segments, int p_ring_count, const Vector<bool> *p_prev_faces_light, const Vector3 *p_prev_light_dir, Vector<bool> *r_out_faces_light, Vector<Vector3> &r_triangles, int *r_write_idx) {
	_build_shadow_topology_if_needed(p_surface);
	int tri_count = p_surface->shadow_tri_indices.size() / 3;
	if (tri_count == 0) {
		return;
	}

	// Worst case: every triangle lit (2 cap tris, 6 verts) and every one
	// of its 3 edges a silhouette wall (2 tris each, 6 verts) -- 24 verts/
	// triangle, never exceeded regardless of real light direction. Only
	// grows the buffer (never shrinks it) -- once an instance's proxy
	// mesh has been built once, every later frame for the same instance
	// finds the buffer already big enough and this is a no-op check, not
	// a real allocation.
	int needed = *r_write_idx + tri_count * 24;
	if (r_triangles.size() < needed) {
		r_triangles.resize(needed);
	}

	PoolVector<Vector3>::Read vr = p_surface->vertices.read();
	const int *tri_idx = p_surface->shadow_tri_indices.ptr();

	// Only this part is genuinely per-frame/light-dependent: which
	// triangles currently face the light. Both the geometric normal
	// (shadow_tri_normal_x/y/z, cached at topology-build time) and the
	// edge adjacency (shadow_edges/shadow_edge_owner0/1) were already
	// built (or reused from the cache) above -- this is now just a dot
	// product per triangle against a cached vector, no vertex reads, no
	// cross product.
	Vector<bool> faces_light;
	faces_light.resize(tri_count);
	const float *nx = p_surface->shadow_tri_normal_x.ptr();
	const float *ny = p_surface->shadow_tri_normal_y.ptr();
	const float *nz = p_surface->shadow_tri_normal_z.ptr();

	// godot-ports#53: temporal coherence takes priority over the
	// silhouette-algorithm axis when the caller actually has usable
	// previous-frame state for this instance+surface (same mesh, same
	// triangle count) -- see the proof in this function's own leading
	// comment block for why this per-triangle reuse-or-retest is exact,
	// not approximate.
	if (p_prev_faces_light != nullptr && p_prev_faces_light->size() == tri_count && p_prev_light_dir != nullptr) {
		Vector3 L_new = p_light_dir_objspace.normalized();
		Vector3 L_old = p_prev_light_dir->normalized();
		real_t dot_ll = CLAMP(L_new.dot(L_old), (real_t)-1.0, (real_t)1.0);
		real_t margin = Math::sqrt(MAX((real_t)0.0, (real_t)2.0 - (real_t)2.0 * dot_ll)); // exact chord distance |L_new - L_old|
		const float *nlen = p_surface->shadow_tri_normal_len.ptr();
		for (int t = 0; t < tri_count; t++) {
			// Compare the RAW (unnormalized) dot against margin*|n| instead
			// of dividing the dot by |n| -- avoids a per-triangle sqrt
			// entirely (|n| is precomputed once at build time, see
			// shadow_tri_normal_len's own comment); mathematically
			// identical to the normalized-space comparison since |n| > 0.
			real_t len = nlen[t];
			if (len > CMP_EPSILON) {
				real_t d_old_raw = nx[t] * L_old.x + ny[t] * L_old.y + nz[t] * L_old.z;
				if (Math::abs(d_old_raw) > margin * len) {
					// Provably can't have crossed zero since last frame --
					// reuse the cached classification directly, no retest.
					faces_light.write[t] = (*p_prev_faces_light)[t];
					continue;
				}
			}
			// Degenerate normal, or within the provable-uncertainty band --
			// exact retest against the real per-frame light direction.
			faces_light.write[t] = (nx[t] * p_light_dir_objspace.x + ny[t] * p_light_dir_objspace.y + nz[t] * p_light_dir_objspace.z) > 0.0f;
		}
	} else if (p_algorithm == VS::SHADOW_SILHOUETTE_ALGORITHM_NORMAL_CONE) {
		_build_shadow_clusters_if_needed(p_surface);
		const int *order = p_surface->shadow_cluster_tri_order.ptr();
		for (int c = 0; c < 6; c++) {
			int start = p_surface->shadow_cluster_tri_start[c];
			int count = p_surface->shadow_cluster_tri_count[c];
			if (count == 0) {
				continue;
			}
			const Vector3 &C = p_surface->shadow_cluster_dir[c];
			float dot_cl = C.x * p_light_dir_objspace.x + C.y * p_light_dir_objspace.y + C.z * p_light_dir_objspace.z;
			float sin_alpha = p_surface->shadow_cluster_sin_alpha[c];
			if (dot_cl > sin_alpha) {
				// angle(C, L) < 90 - alpha: every triangle in the cluster
				// clears 90 degrees even at its widest deviation -- all lit.
				for (int i = 0; i < count; i++) {
					faces_light.write[order[start + i]] = true;
				}
			} else if (dot_cl < -sin_alpha) {
				// angle(C, L) > 90 + alpha: symmetric guarantee -- all dark.
				for (int i = 0; i < count; i++) {
					faces_light.write[order[start + i]] = false;
				}
			} else {
				// Gray band: light falls close enough to this cluster's
				// own 90-degree line that individual members could
				// disagree with the cluster average -- resolve each one
				// exactly, same test the FULL algorithm always uses.
				for (int i = 0; i < count; i++) {
					int t = order[start + i];
					faces_light.write[t] = (nx[t] * p_light_dir_objspace.x + ny[t] * p_light_dir_objspace.y + nz[t] * p_light_dir_objspace.z) > 0.0f;
				}
			}
		}
	} else if (p_algorithm == VS::SHADOW_SILHOUETTE_ALGORITHM_RING_SEGMENT && p_ring_radial_segments > 0) {
		// godot-ports#50: ring-level coarse cull, only for content with a
		// real declared ring/radial-segment topology (SphereMesh/
		// CylinderMesh -- see mesh_instance.cpp's push site). If the
		// requested topology doesn't actually match this surface's real
		// triangle count, _build_shadow_rings_if_needed() leaves the ring
		// cache empty (shadow_ring_actual_count == 0) and this whole branch
		// safely degrades to the plain per-triangle test below.
		_build_shadow_rings_if_needed(p_surface, p_ring_radial_segments, p_ring_count);
		int ring_n = p_surface->shadow_ring_actual_count;
		if (ring_n == 0) {
			for (int t = 0; t < tri_count; t++) {
				faces_light.write[t] = (nx[t] * p_light_dir_objspace.x + ny[t] * p_light_dir_objspace.y + nz[t] * p_light_dir_objspace.z) > 0.0f;
			}
		} else {
			for (int r = 0; r < ring_n; r++) {
				int start = p_surface->shadow_ring_tri_start[r];
				int count = p_surface->shadow_ring_tri_count[r];

				if (p_surface->shadow_ring_has_degenerate[r]) {
					// godot-ports#50 bugfix: never blanket-resolve a ring
					// containing a degenerate triangle (both pole rings) --
					// see the field comment on shadow_ring_has_degenerate.
					for (int i = start; i < start + count; i++) {
						faces_light.write[i] = (nx[i] * p_light_dir_objspace.x + ny[i] * p_light_dir_objspace.y + nz[i] * p_light_dir_objspace.z) > 0.0f;
					}
					continue;
				}

				float min_cy = p_surface->shadow_ring_min_cos_from_y[r];
				float max_cy = p_surface->shadow_ring_max_cos_from_y[r];
				// cos is decreasing on [0, PI] -- max_cy (smallest angle
				// from +Y) maps to the SMALLER beta bound, min_cy to the larger.
				float beta_lo = Math::acos(CLAMP(max_cy, -1.0f, 1.0f));
				float beta_hi = Math::acos(CLAMP(min_cy, -1.0f, 1.0f));
				float min_dot, max_dot;
				_ring_dot_bounds(beta_lo, beta_hi, p_light_dir_objspace, min_dot, max_dot);

				if (min_dot > 0.0f) {
					for (int i = 0; i < count; i++) {
						faces_light.write[start + i] = true;
					}
				} else if (max_dot <= 0.0f) {
					for (int i = 0; i < count; i++) {
						faces_light.write[start + i] = false;
					}
				} else {
					for (int i = start; i < start + count; i++) {
						faces_light.write[i] = (nx[i] * p_light_dir_objspace.x + ny[i] * p_light_dir_objspace.y + nz[i] * p_light_dir_objspace.z) > 0.0f;
					}
				}
			}
		}
	} else {
		// SHADOW_SILHOUETTE_ALGORITHM_FULL (the default) and any algorithm
		// not yet implemented/applicable (including RING_SEGMENT requested
		// on content with no declared ring topology) fall back to this
		// exact, unconditional per-triangle test -- always correct, just
		// O(tri_count).
		for (int t = 0; t < tri_count; t++) {
			faces_light.write[t] = (nx[t] * p_light_dir_objspace.x + ny[t] * p_light_dir_objspace.y + nz[t] * p_light_dir_objspace.z) > 0.0f;
		}
	}

	// godot-ports#53: hand the finished classification back to the caller
	// so it can be cached as next frame's "previous" state, regardless of
	// which branch above actually built it.
	if (r_out_faces_light != nullptr) {
		*r_out_faces_light = faces_light;
	}

	Vector3 extrude = -p_light_dir_objspace.normalized() * p_extrude_distance;

	// Caps: exactly one front+back cap pair per light-facing triangle,
	// nothing shared between triangles here -- unchanged from before.
	for (int t = 0; t < tri_count; t++) {
		if (!faces_light[t]) {
			continue;
		}
		Vector3 vp0 = vr[tri_idx[t * 3 + 0]], vp1 = vr[tri_idx[t * 3 + 1]], vp2 = vr[tri_idx[t * 3 + 2]];

		// Front cap: the light-facing triangle itself, unmodified winding.
		r_triangles.write[(*r_write_idx)++] = vp0;
		r_triangles.write[(*r_write_idx)++] = vp1;
		r_triangles.write[(*r_write_idx)++] = vp2;
		// Back cap: the same triangle extruded, winding REVERSED so it
		// faces the opposite way once translated behind the object --
		// this is what closes the volume correctly for z-pass counting.
		r_triangles.write[(*r_write_idx)++] = vp0 + extrude;
		r_triangles.write[(*r_write_idx)++] = vp2 + extrude;
		r_triangles.write[(*r_write_idx)++] = vp1 + extrude;
	}

	// godot-ports#48 phase 2: silhouette walls. Two prior restructurings of
	// this loop were tried and rejected (see the #48 comment thread for
	// the numbers): a pure edge-centric walk gave up the outer loop's
	// cheap "skip dark triangles entirely" and roughly broke even; adding
	// a per-frame visited-edge tracking array on top of the original
	// per-triangle walk cost MORE than the redundant owners-list rescans
	// it eliminated. The real bottleneck was never redundant computation
	// -- there isn't any real math in this loop to redo (determining
	// silhouette-ness is pure lookups/comparisons, no floating point at
	// all) -- it's memory INDIRECTION: shadow_edges[ei].owner_tris is a
	// separate heap-allocated Vector<int> PER EDGE, so reading a shared
	// edge's 1-2 owners meant hopping into a scattered, individually
	// allocated buffer for every edge, every frame. shadow_edge_owner0/1
	// (flat arrays, built once alongside the rest of the topology cache)
	// replace that hop with a single direct array read, keeping the
	// original triangle-centric outer loop (still skips dark triangles
	// for ~free) and the original per-triangle emission (still uses `t`'s
	// own vi[e]/vi[(e+1)%3] directly, no reverse-search needed to recover
	// a specific owner's directed edge order).
	for (int t = 0; t < tri_count; t++) {
		if (!faces_light[t]) {
			continue;
		}
		int vi[3] = { tri_idx[t * 3 + 0], tri_idx[t * 3 + 1], tri_idx[t * 3 + 2] };
		for (int e = 0; e < 3; e++) {
			int a = vi[e];
			int b = vi[(e + 1) % 3];
			int ei = p_surface->shadow_tri_edges[t * 3 + e];
			int o1 = p_surface->shadow_edge_owner1[ei];
			bool is_silhouette;
			if (o1 == -1) {
				is_silhouette = true; // boundary edge -- t is its only owner
			} else if (o1 == -2) {
				// Rare non-manifold case (3+ owners) -- fast arrays can't
				// represent it, fall back to the full owners-list scan:
				// silhouette from t's perspective iff some OTHER owner
				// doesn't face the light. Same logic (and cost) as the
				// very first version of this loop had for every edge.
				const Vector<int> &owners = p_surface->shadow_edges[ei].owner_tris;
				is_silhouette = false;
				for (int k = 0; k < owners.size(); k++) {
					if (owners[k] != t && !faces_light[owners[k]]) {
						is_silhouette = true;
						break;
					}
				}
			} else {
				// The overwhelmingly common case: exactly 2 owners, one
				// of which is `t` itself. Silhouette iff the OTHER owner
				// doesn't face the light -- both lit means this edge is
				// purely interior, no wall needed.
				int o0 = p_surface->shadow_edge_owner0[ei];
				int other = (o0 == t) ? o1 : o0;
				is_silhouette = !faces_light[other];
			}
			if (!is_silhouette) {
				continue;
			}
			Vector3 va = vr[a];
			Vector3 vb = vr[b];
			Vector3 va_ext = va + extrude;
			Vector3 vb_ext = vb + extrude;
			// Side quad (split into 2 triangles), wound to match the front
			// cap's own directed-edge sense so the whole volume's outward
			// winding stays consistent.
			r_triangles.write[(*r_write_idx)++] = va;
			r_triangles.write[(*r_write_idx)++] = vb;
			r_triangles.write[(*r_write_idx)++] = vb_ext;
			r_triangles.write[(*r_write_idx)++] = va;
			r_triangles.write[(*r_write_idx)++] = vb_ext;
			r_triangles.write[(*r_write_idx)++] = va_ext;
		}
	}
}

// godot-ports#26 perf fix: per-instance cache of a fully-built shadow
// volume (see its use in _render_primary_shadow_and_relight() below for the
// full reasoning). Keyed by the instance pointer -- stable/persistent for a
// given logical node across frames, but not tied to that node's lifetime,
// so entries for since-destroyed instances would otherwise accumulate
// forever over a long play session (e.g. this project's own Squash the
// Creeps, which spawns a new Mob every 0.5s indefinitely). Pruned by a
// simple size cap rather than real lifecycle tracking (which would need a
// destroy hook this cull-result-only code path doesn't have): once the
// cache holds more entries than a single frame plausibly needs, it's
// cheaper and simpler to drop the whole thing and let it repopulate from
// scratch (a one-frame cost) than to chase down which entries are actually
// stale.
// godot-ports#52: direction-quantized precomputed silhouette caching.
// Fixed set of 26 evenly-spread unit directions (a cube's 6 face normals +
// 12 edge midpoint directions + 8 corner directions, all normalized) --
// a standard, simple discrete covering of the sphere of directions, not
// hand-tuned. Real per-frame cost is just finding the nearest of these 26
// via max dot product (26 dot products + compares, no trig) -- cheap
// regardless of whether the result hits or misses the object-space light
// direction's own per-instance cache slot.
static const Vector3 DIRECTION_BUCKET_DIRS_RAW[26] = {
	// 6 face directions
	Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1),
	// 12 edge directions
	Vector3(1, 1, 0), Vector3(1, -1, 0), Vector3(-1, 1, 0), Vector3(-1, -1, 0),
	Vector3(1, 0, 1), Vector3(1, 0, -1), Vector3(-1, 0, 1), Vector3(-1, 0, -1),
	Vector3(0, 1, 1), Vector3(0, 1, -1), Vector3(0, -1, 1), Vector3(0, -1, -1),
	// 8 corner directions
	Vector3(1, 1, 1), Vector3(1, 1, -1), Vector3(1, -1, 1), Vector3(1, -1, -1),
	Vector3(-1, 1, 1), Vector3(-1, 1, -1), Vector3(-1, -1, 1), Vector3(-1, -1, -1)
};
static const int DIRECTION_BUCKET_COUNT = 26;

static Vector3 g_direction_bucket_dirs[DIRECTION_BUCKET_COUNT];
static bool g_direction_bucket_dirs_init = false;

static int _direction_bucket_index(const Vector3 &p_dir_objspace) {
	if (!g_direction_bucket_dirs_init) {
		for (int i = 0; i < DIRECTION_BUCKET_COUNT; i++) {
			g_direction_bucket_dirs[i] = DIRECTION_BUCKET_DIRS_RAW[i].normalized();
		}
		g_direction_bucket_dirs_init = true;
	}
	Vector3 n = p_dir_objspace.normalized();
	int best = 0;
	real_t best_dot = -2.0;
	for (int i = 0; i < DIRECTION_BUCKET_COUNT; i++) {
		real_t d = n.dot(g_direction_bucket_dirs[i]);
		if (d > best_dot) {
			best_dot = d;
			best = i;
		}
	}
	return best;
}

struct ShadowVolumeCacheEntry {
	Transform transform;
	Vector3 light_dir_objspace;
	RasterizerStorageGLFF::Mesh *mesh = nullptr;
	// godot-ports#49/#54: included in the cache key so a script changing an
	// instance's shadow_silhouette_algorithm at runtime (rare, but possible)
	// invalidates the cache instead of silently reusing volume geometry
	// built under the OLD algorithm for one stale frame.
	VS::ShadowSilhouetteAlgorithm algorithm = VS::SHADOW_SILHOUETTE_ALGORITHM_FULL;
	// godot-ports#50: also part of the cache key for the same reason --
	// only meaningful when algorithm == RING_SEGMENT, but cheap to compare
	// unconditionally.
	int ring_radial_segments = 0;
	int ring_count = 0;
	// godot-ports#52: -1 means "built for the exact per-frame light
	// direction" (shadow_temporal_cache == NONE, today's default
	// behavior); >= 0 means "built for direction bucket N's own
	// representative direction, not the real per-frame one" -- see
	// _direction_bucket_index()'s own comment for what this trades away.
	int bucket_index = -1;
	// godot-ports#55: vol_tris is a persistent scratch buffer that only
	// ever GROWS (see the call site's own comment for why) -- vol_tris.
	// size() is its allocated capacity, not how much of it is actually
	// valid this frame. vol_tris_valid (a vertex count, same units
	// .size() used to be trusted for) is the real "how much to draw"
	// value now.
	Vector<Vector3> vol_tris;
	int vol_tris_valid = 0;
	// godot-ports#53: last frame's per-surface light-facing classification,
	// only populated/consumed when shadow_temporal_cache ==
	// TEMPORAL_COHERENCE. Indexed [surface_index][triangle_index]. Reset
	// (cleared) whenever the mesh changes, so a stale array from a
	// different mesh/surface-count can never be misread against new data.
	Vector<Vector<bool>> temporal_faces_light;
};
static Map<RasterizerScene::InstanceBase *, ShadowVolumeCacheEntry> shadow_volume_cache;
static const int SHADOW_VOLUME_CACHE_MAX_ENTRIES = 256;

// The stencil-volume + additive-relight frame pass. Called once per frame
// (not per-instance) from render_scene(), AFTER the normal opaque pass has
// already populated the real color+depth buffers -- the shadow volumes'
// own depth test needs to compare against that real scene depth to know
// which fragments the shadow-casting light's ray actually reaches before
// hitting real geometry. p_light_dir_world is GL's own light-position
// convention already used elsewhere in this file: the direction FROM a
// surface TOWARD the light, not the direction the light travels.
static void _render_primary_shadow_and_relight(RasterizerStorageGLFF *p_storage, GLenum p_gl_light, const Vector3 &p_light_dir_world, const Transform &p_cam_transform, RasterizerScene::InstanceBase **p_cull_result, int p_cull_count, bool p_subtractive, const Color &p_ambient_color, int p_max_distance_casters, int p_max_priority_casters) {
	if (shadow_volume_cache.size() > SHADOW_VOLUME_CACHE_MAX_ENTRIES) {
		shadow_volume_cache.clear();
	}
	static const float SHADOW_EXTRUDE_DISTANCE = 200.0f;

	// godot-ports#26 perf fix: shadow-caster budget. The per-instance
	// volume cache above only pays off for casters that stay still --
	// real content (this project's own Squash the Creeps) can have every
	// single shadow-casting instance animating every frame (Player and
	// every Mob share a continuous idle-bob AnimationPlayer), so caching
	// alone doesn't bound the cost as caster count grows over a long
	// session (MobTimer spawns a new one every 0.5s, indefinitely, with
	// nothing despawning them but leaving the screen or being squashed).
	// Cap the real silhouette-extraction + stencil-draw work to the
	// MAX_DISTANCE_CASTERS instances closest to the camera -- a reasonable
	// proxy for "large enough on screen for its shadow to actually read
	// as one," since a shadow from something far away/small on screen is
	// the least likely to be missed. Selection is a bounded O(p_cull_count
	// * MAX_DISTANCE_CASTERS) partial selection (track the current worst
	// of the kept set, replace it when something closer turns up) rather
	// than a full sort, since the budget is small and fixed regardless of
	// how many total casters exist. Instances beyond the budget simply
	// don't cast a shadow this frame -- they're still fully relit and
	// still correctly receive shadows cast by the instances that made the
	// cut, only their own casting is skipped, the same real trade-off
	// "max shadow casters" budgets make in modern engines.
	//
	// PRIORITY_SHADOW_LAYER_BIT is a separate, unconditional guarantee on
	// top of that distance budget: an instance with this bit set in its
	// layer_mask (VisualInstance's existing, already-editor-exposed
	// `layers` property -- no new engine API needed) always casts a
	// shadow this frame regardless of distance to camera. Exists because
	// distance-to-camera is only a proxy for "will be missed if it
	// doesn't cast a shadow" -- it breaks down for a specific, important
	// case this project's own camera setup hits directly: Main.tscn's
	// Camera is a fixed Position3D rig, not something that follows the
	// Player, so the Player can end up farther from the camera than a
	// cluster of Mobs converging on it and lose its budget slot to them
	// even though the Player losing its own shadow is far more
	// noticeable than any one Mob losing its. Content opts a specific
	// instance into this guarantee by setting the bit itself (e.g.
	// Player.gd could call set_layer_mask_bit(31, true) in _ready()) --
	// this file only defines which bit means "always cast," it doesn't
	// decide who gets it.
	static const uint32_t PRIORITY_SHADOW_LAYER_BIT = 1u << 31;
	// godot-ports#47: MAX_DISTANCE_CASTERS/MAX_PRIORITY_CASTERS are now a
	// per-light configurable budget (Light.shadow_max_distance_casters/
	// shadow_max_priority_casters, defaulting to today'''s exact 3/8)
	// instead of hardcoded constants -- clamped to a fixed absolute
	// ceiling so the backing arrays below can stay plain fixed-size stack
	// arrays rather than needing a heap allocation for an arbitrary runtime
	// size.
	static const int ABSOLUTE_MAX_PRIORITY_CASTERS = 32;
	static const int ABSOLUTE_MAX_DISTANCE_CASTERS = 64;
	const int MAX_PRIORITY_CASTERS = CLAMP(p_max_priority_casters, 0, ABSOLUTE_MAX_PRIORITY_CASTERS);
	const int MAX_DISTANCE_CASTERS = CLAMP(p_max_distance_casters, 0, ABSOLUTE_MAX_DISTANCE_CASTERS);
	int caster_idx[ABSOLUTE_MAX_PRIORITY_CASTERS + ABSOLUTE_MAX_DISTANCE_CASTERS];
	float caster_dist_sq[ABSOLUTE_MAX_DISTANCE_CASTERS];
	int priority_count = 0;
	int distance_count = 0;
	for (int i = 0; i < p_cull_count; i++) {
		RasterizerScene::InstanceBase *instance = p_cull_result[i];
		if (!instance->visible || instance->base_type != VS::INSTANCE_MESH) {
			continue;
		}
		if (instance->cast_shadows == VS::SHADOW_CASTING_SETTING_OFF) {
			continue;
		}
		if ((instance->layer_mask & PRIORITY_SHADOW_LAYER_BIT) && priority_count < MAX_PRIORITY_CASTERS) {
			caster_idx[priority_count] = i;
			priority_count++;
			continue;
		}
		float dist_sq = instance->transform.origin.distance_squared_to(p_cam_transform.origin);
		if (distance_count < MAX_DISTANCE_CASTERS) {
			caster_idx[MAX_PRIORITY_CASTERS + distance_count] = i;
			caster_dist_sq[distance_count] = dist_sq;
			distance_count++;
		} else {
			int worst = 0;
			for (int k = 1; k < MAX_DISTANCE_CASTERS; k++) {
				if (caster_dist_sq[k] > caster_dist_sq[worst]) {
					worst = k;
				}
			}
			if (dist_sq < caster_dist_sq[worst]) {
				caster_idx[MAX_PRIORITY_CASTERS + worst] = i;
				caster_dist_sq[worst] = dist_sq;
			}
		}
	}
	// Compact the two ranges (priority casters at [0, priority_count),
	// distance casters stored starting at a fixed offset) into one
	// contiguous [0, caster_count) run the draw loop can walk plainly.
	for (int k = 0; k < distance_count; k++) {
		caster_idx[priority_count + k] = caster_idx[MAX_PRIORITY_CASTERS + k];
	}
	int caster_count = priority_count + distance_count;

	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_2D);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glEnableClientState(GL_VERTEX_ARRAY);

	// Stencil is now cleared once, up front, alongside the frame's main
	// color+depth clear (see render_scene()'s own comment on this) --
	// not re-cleared here.
	glEnable(GL_STENCIL_TEST);
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask(GL_FALSE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	glStencilFunc(GL_ALWAYS, 0, 0xFF);

	// Self-shadow z-fighting fix: a shadow volume's own front cap is built
	// directly from the caster's light-facing triangles (see
	// _build_shadow_volume_triangles()), so for any caster whose own
	// surface faces the light -- which for a flat receiver like a ground
	// plane (a default SHADOW_CASTING_SETTING_ON GeometryInstance, same as
	// everything else) means its own visible top surface -- the cap is
	// EXACTLY coincident with geometry the opaque base pass already wrote
	// into the depth buffer. Testing that cap with GL_LESS against its own
	// identical depth is a coin flip decided by FP rounding, which differs
	// frame to frame as the camera moves -- exactly the "ground flickers
	// like a light switching on/off" symptom, since the ground is most of
	// the screen. A small constant polygon offset pushes the cap's tested
	// depth reliably farther than the real surface, so the coincident case
	// deterministically resolves to "not in front of itself" (the
	// physically sane answer -- a single-layer surface cannot occlude
	// itself) instead of flickering.
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(1.0f, 4.0f);

	for (int ci = 0; ci < caster_count; ci++) {
		RasterizerScene::InstanceBase *instance = p_cull_result[caster_idx[ci]];
		RasterizerStorageGLFF::Mesh *mesh = p_storage->mesh_owner.getornull(instance->base);
		if (!mesh) {
			continue;
		}

		// godot-ports#51: LOD proxy geometry source -- walk a separate,
		// lower-poly Mesh for shadow-volume construction instead of the
		// actual render mesh, when explicitly assigned. Purely a
		// substitution at THIS point (which Surfaces get walked below);
		// everything else about this instance (its own transform, the
		// per-instance cache, the light direction) is unaffected, and the
		// proxy is assumed authored in the same local coordinate space as
		// the render mesh (same convention as shadow_lod_proxy_mesh's own
		// property doc). An invalid/unresolvable proxy RID (never
		// assigned, or a freed resource) safely falls back to the real
		// render mesh -- GLFF never silently drops a caster's shadow for a
		// misconfigured proxy.
		RasterizerStorageGLFF::Mesh *shadow_mesh = mesh;
		if (instance->shadow_geometry_source == VS::SHADOW_GEOMETRY_SOURCE_LOD_PROXY && instance->shadow_lod_proxy_mesh.is_valid()) {
			RasterizerStorageGLFF::Mesh *proxy = p_storage->mesh_owner.getornull(instance->shadow_lod_proxy_mesh);
			if (proxy) {
				shadow_mesh = proxy;
			}
		}

		Basis inv_rot = instance->transform.basis.orthonormalized().transposed();
		Vector3 light_dir_objspace = inv_rot.xform(p_light_dir_world).normalized();

		// godot-ports#26 perf fix: the fully-built shadow volume (front
		// cap + extruded silhouette walls + back cap, across all of this
		// instance's surfaces) is cached per-instance and only rebuilt
		// when something it actually depends on changes -- its own
		// transform (world position/orientation), the mesh it's using,
		// or the light direction in its own object space. For a caster
		// that never moves (level geometry, a static Ground plane) this
		// is every field, every frame, forever -- exactly the case that
		// used to pay the full CPU-side silhouette-extraction cost (a
		// per-triangle light-facing test plus a walk of every candidate
		// silhouette edge) for literally zero change in output. Genuinely
		// moving/rotating casters (this project's own Player/Mob, both
		// using a continuous idle-bob AnimationPlayer) still rebuild
		// every frame, same as before -- their transform really does
		// change every frame, so the cache correctly never hits for them.
		// Keyed by the instance pointer, which is stable/persistent for a
		// given logical node across frames (VisualServerScene::Instance
		// objects are created once and reused by culling, not recreated
		// per frame) -- see the cache's own pruning comment further down
		// for how a since-destroyed instance's stale entry gets cleared
		// out rather than accumulating forever.
		// godot-ports#52: direction-quantized caching. When selected, snap
		// light_dir_objspace to the nearest of 26 fixed bucket directions
		// and key/build the cache off the BUCKET's own representative
		// direction instead of the real per-frame one -- trades a small,
		// bounded silhouette error (using the nearest sampled direction
		// instead of the true one) for a cache that stays hit across many
		// consecutive frames of a rotating caster (as long as it hasn't
		// crossed into a different bucket), instead of missing every
		// single frame the way the NONE default does for anything that
		// rotates. Unlike the transform-exact NONE mode, this doesn't
		// depend on translation at all (silhouette shape is a pure
		// function of object-space light direction, never position), so
		// the cache key intentionally omits `transform`.
		bool use_bucket = instance->shadow_temporal_cache == VS::SHADOW_TEMPORAL_CACHE_DIRECTION_QUANTIZED;
		int bucket_idx = use_bucket ? _direction_bucket_index(light_dir_objspace) : -1;
		Vector3 build_light_dir = use_bucket ? g_direction_bucket_dirs[bucket_idx] : light_dir_objspace;

		Map<RasterizerScene::InstanceBase *, ShadowVolumeCacheEntry>::Element *CE = shadow_volume_cache.find(instance);
		bool cache_hit;
		if (use_bucket) {
			cache_hit = CE && CE->value().mesh == shadow_mesh && CE->value().algorithm == instance->shadow_silhouette_algorithm && CE->value().ring_radial_segments == instance->shadow_ring_radial_segments && CE->value().ring_count == instance->shadow_ring_count && CE->value().bucket_index == bucket_idx;
		} else {
			cache_hit = CE && CE->value().mesh == shadow_mesh && CE->value().transform == instance->transform && CE->value().light_dir_objspace == light_dir_objspace && CE->value().algorithm == instance->shadow_silhouette_algorithm && CE->value().ring_radial_segments == instance->shadow_ring_radial_segments && CE->value().ring_count == instance->shadow_ring_count && CE->value().bucket_index == -1;
		}
		if (!CE) {
			CE = shadow_volume_cache.insert(instance, ShadowVolumeCacheEntry());
		}
		if (!cache_hit) {
			// godot-ports#53: only meaningful if the previous entry is for
			// the SAME mesh with the SAME surface count -- a mesh swap
			// invalidates temporal state the same way it invalidates
			// everything else cached here. Read the previous light
			// direction BEFORE it gets overwritten below.
			bool use_coherence = instance->shadow_temporal_cache == VS::SHADOW_TEMPORAL_CACHE_TEMPORAL_COHERENCE && CE->value().mesh == shadow_mesh && CE->value().temporal_faces_light.size() == shadow_mesh->surfaces.size();
			Vector3 prev_light_dir = CE->value().light_dir_objspace;
			Vector<Vector<bool>> new_faces_light;
			bool want_faces_light_out = instance->shadow_temporal_cache == VS::SHADOW_TEMPORAL_CACHE_TEMPORAL_COHERENCE;
			if (want_faces_light_out) {
				new_faces_light.resize(shadow_mesh->surfaces.size());
			}

			// godot-ports#55: used to be `CE->value().vol_tris.resize(0);`
			// here, which for CowData means "free the buffer", not "keep
			// the allocation, reset the logical size" (see
			// _build_shadow_volume_triangles()'s own leading comment).
			// That forced every animating caster's persistent per-instance
			// vol_tris buffer to regrow FROM NULL every single frame via
			// ~14 reallocations (doubling from 0 up to ~8-16KB) instead of
			// settling into a stable buffer reused frame to frame. Now:
			// don't touch vol_tris' allocation at all here -- just reset
			// the write cursor, and let _build_shadow_volume_triangles's
			// own ensure-capacity check (grow-only, sized to that call's
			// real worst case) decide whether anything needs to grow.
			int write_idx = 0;
			for (int s = 0; s < shadow_mesh->surfaces.size(); s++) {
				const Vector<bool> *prev_fl = (use_coherence && s < CE->value().temporal_faces_light.size()) ? &CE->value().temporal_faces_light[s] : nullptr;
				const Vector3 *prev_ld = use_coherence ? &prev_light_dir : nullptr;
				Vector<bool> out_fl_local;
				_build_shadow_volume_triangles(shadow_mesh->surfaces[s], build_light_dir, SHADOW_EXTRUDE_DISTANCE, instance->shadow_silhouette_algorithm, instance->shadow_ring_radial_segments, instance->shadow_ring_count, prev_fl, prev_ld, want_faces_light_out ? &out_fl_local : nullptr, CE->value().vol_tris, &write_idx);
				if (want_faces_light_out) {
					new_faces_light.write[s] = out_fl_local;
				}
			}
			CE->value().vol_tris_valid = write_idx;
			if (want_faces_light_out) {
				CE->value().temporal_faces_light = new_faces_light;
			} else if (CE->value().temporal_faces_light.size() > 0) {
				// Switched away from TEMPORAL_COHERENCE -- drop stale state
				// rather than let it linger unused.
				CE->value().temporal_faces_light.clear();
			}
			CE->value().mesh = shadow_mesh;
			CE->value().transform = instance->transform;
			CE->value().light_dir_objspace = build_light_dir;
			CE->value().algorithm = instance->shadow_silhouette_algorithm;
			CE->value().ring_radial_segments = instance->shadow_ring_radial_segments;
			CE->value().ring_count = instance->shadow_ring_count;
			CE->value().bucket_index = bucket_idx;
		}
		const Vector<Vector3> &vol_tris = CE->value().vol_tris;
		int vol_tris_valid = CE->value().vol_tris_valid;
		if (vol_tris_valid == 0) {
			continue;
		}

		glPushMatrix();
		GLfloat gl_model[16];
		_load_transform_gl(instance->transform, gl_model);
		glMultMatrixf(gl_model);

		glVertexPointer(3, GL_FLOAT, 0, vol_tris.ptr());

		glEnable(GL_CULL_FACE);
		glCullFace(GL_BACK);
		glStencilOp(GL_KEEP, GL_KEEP, GL_INCR);
		glDrawArrays(GL_TRIANGLES, 0, vol_tris_valid);

		glCullFace(GL_FRONT);
		glStencilOp(GL_KEEP, GL_KEEP, GL_DECR);
		glDrawArrays(GL_TRIANGLES, 0, vol_tris_valid);

		glDisable(GL_CULL_FACE);
		glPopMatrix();
	}

	glDisable(GL_POLYGON_OFFSET_FILL);

	// godot-ports#55/#56: relight-culling AABB test. An earlier version
	// swept each caster's AABB by the full SHADOW_EXTRUDE_DISTANCE
	// (200 -- the distance the real shadow-volume geometry uses, chosen
	// there to guarantee correctness against a receiver arbitrarily far
	// away) and measured ZERO skips: Mob and Player kept "crossing" each
	// other's shadow reach despite standing only ~3 units apart. That
	// swept-AABB math was exact, not buggy -- merging a box with itself
	// translated by a vector IS the true Minkowski sum of the box and that
	// segment. The real problem was the 200-unit magnitude itself: this
	// scene's light comes from (3,6,4) looking at the origin, so its
	// travel direction has real X/Z components, not just straight down --
	// sweeping 200 units along a diagonal shifts the box tens of units
	// sideways too, dwarfing the 3-unit gap between the characters and
	// guaranteeing overlap almost regardless of where they actually stand.
	// Fix: don't use one global sweep distance at all. For each (caster,
	// receiver) PAIR, bound the sweep to roughly how far that specific
	// receiver actually is from that specific caster (plus the receiver's
	// own size as margin) -- a receiver can't be in a caster's shadow path
	// if the shadow doesn't need to travel anywhere near that far to reach
	// it. This is computed fresh per pair below (caster AABBs cached
	// unswept here; the receiver loop builds each pair's own swept box).
	bool subtractive = p_subtractive;
	AABB caster_world_aabb[ABSOLUTE_MAX_PRIORITY_CASTERS + ABSOLUTE_MAX_DISTANCE_CASTERS];
	Vector3 light_travel_dir; // unit vector, the direction light actually travels (away from the light)
	if (subtractive && caster_count > 0) {
		light_travel_dir = -p_light_dir_world.normalized();
		for (int ci = 0; ci < caster_count; ci++) {
			RasterizerScene::InstanceBase *cinst = p_cull_result[caster_idx[ci]];
			// godot-ports#55: shadow_relight_aabb, when enabled on this
			// instance, replaces the real mesh AABB as the shape this
			// caster's shadow reach is built from -- an explicit override
			// for content where the automatic mesh bounds are a poor
			// stand-in (e.g. deliberately larger/smaller than the render
			// mesh itself).
			AABB local_aabb = cinst->shadow_relight_aabb_enabled ? cinst->shadow_relight_aabb : p_storage->mesh_get_aabb(cinst->base, RID());
			caster_world_aabb[ci] = cinst->transform.xform(local_aabb);
		}
	}

	// Additive relight: only this one light, only where the stencil buffer
	// is still exactly 0 (never net-entered a shadow volume), only onto
	// fragments that already exist at this exact depth (the opaque pass's
	// own fragments).
	//
	// godot-ports#26 flicker fix: this used to be glDepthFunc(GL_EQUAL),
	// requiring this redraw's fragment depth to bit-exactly match what the
	// base pass wrote. Confirmed via instrumentation that the CPU-side
	// state driving this pass (which light, which casters, frame timing)
	// is 100% stable frame to frame -- so the visible flicker has to be
	// happening at the GL rasterization level, and GL_EQUAL depth matching
	// across two separate draw calls is exactly the kind of thing that
	// isn't guaranteed bit-exact on real hardware, especially with
	// different GL state active in between (GL_LIGHTING/GL_TEXTURE_2D
	// toggled off for the stencil-build sub-pass, back on here) on an old,
	// quirky driver (see `ati-x1900-driver-quirks` for the sibling GPU's
	// own catalog of similar precision surprises -- RV250 is a different
	// chip, not yet cataloged, but the category of bug is the same
	// class). Fix: GL_LEQUAL (this codebase's own established pattern
	// elsewhere for "redraw this surface again without z-fighting", see
	// the two GL_LEQUAL restores after this file's other GL_EQUAL-gated
	// sub-passes) plus a small camera-ward polygon-offset bias, so the
	// redraw's depth is reliably <= the stored depth regardless of FP
	// noise -- deterministic instead of a per-pixel coin flip.
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (subtractive) {
		glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
	} else {
		glStencilFunc(GL_EQUAL, 0, 0xFF);
	}
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_FALSE);
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(-1.0f, -4.0f);
	glEnable(GL_LIGHTING);

	// godot-ports#58: SUBTRACTIVE mode's shadowed-side draw does NOT try to
	// compute this light's contribution and subtract it back out (that's
	// what #57 attempted, and it's provably unsafe: specular is a sharp,
	// non-linear term, and two SEPARATE draw calls computing it can't be
	// relied on to match closely enough for a subtraction to land on
	// exactly zero -- confirmed live, a highlight that didn't cancel
	// cleanly clamped to solid black on a shadowed region). Instead,
	// shadowed fragments are OVERWRITTEN with the exact correct value
	// directly: this light disabled, real scene ambient restored (it's
	// no longer "already accounted for" here -- this draw isn't adding on
	// top of the base pass's own contribution, it's replacing it
	// outright) -- precisely the same computation ADDITIVE mode's base
	// pass already does for every fragment, just gated to the shadowed
	// side instead of drawn everywhere. No subtraction anywhere, so no
	// cancellation risk, and still exactly one extra draw -- the existing
	// correction pass just does a different thing now, not a new one.
	if (subtractive) {
		glDisable(p_gl_light);
		glDisable(GL_BLEND);
		GLfloat real_amb[4] = { p_ambient_color.r, p_ambient_color.g, p_ambient_color.b, 1.0f };
		glLightModelfv(GL_LIGHT_MODEL_AMBIENT, real_amb);
	} else {
		glEnable(p_gl_light);
		glEnable(GL_BLEND);
		glBlendEquation(GL_FUNC_ADD);
		glBlendFunc(GL_ONE, GL_ONE);
		GLfloat zero_amb[4] = { 0, 0, 0, 1 };
		glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zero_amb); // ambient already accounted for in the base pass
	}

	for (int i = 0; i < p_cull_count; i++) {
		RasterizerScene::InstanceBase *instance = p_cull_result[i];
		if (!instance->visible || instance->base_type != VS::INSTANCE_MESH) {
			continue;
		}
		RasterizerStorageGLFF::Mesh *mesh = p_storage->mesh_owner.getornull(instance->base);
		if (!mesh) {
			continue;
		}
		if (subtractive) {
			// godot-ports#55: per-instance override, checked before the
			// automatic test even runs -- ALWAYS/NEVER are a hard escape
			// hatch for content where the bounding-volume heuristic below
			// gets the wrong answer for this specific mesh.
			if (instance->shadow_relight_inclusion == VS::SHADOW_RELIGHT_INCLUSION_ALWAYS) {
				// falls through to the draw below
			} else if (instance->shadow_relight_inclusion == VS::SHADOW_RELIGHT_INCLUSION_NEVER) {
				continue;
			} else if (caster_count == 0) {
				continue; // no real casters this frame -- nothing anywhere needs correcting
			} else {
				// godot-ports#55: shadow_relight_aabb, when enabled, replaces
				// the real mesh AABB as the shape this instance presents in
				// its RECEIVER role too -- see the caster-side use above for
				// the same override on the other side of the pair test.
				AABB local_aabb = instance->shadow_relight_aabb_enabled ? instance->shadow_relight_aabb : p_storage->mesh_get_aabb(instance->base, RID());
				AABB world_aabb = instance->transform.xform(local_aabb);
				// Receiver's own size, used as margin below -- a receiver isn't
				// a point, its own extent counts toward "close enough".
				real_t receiver_radius = world_aabb.get_longest_axis_size() * 0.5f;
				bool touched = false;
				for (int ci = 0; ci < caster_count; ci++) {
					// godot-ports#55: an instance's own AABB is checked
					// against OTHER casters only by default -- comparing it
					// against ITSELF is a tautology (a box always contains
					// itself, and the swept version only ever grows the
					// box, so self-vs-self would intersect unconditionally
					// regardless of geometry, light direction, or sweep
					// length). Counting that as "touched" meant every
					// caster always qualified for correction no matter
					// what, defeating the entire point of this filter.
					// shadow_relight_self opts a specific instance back
					// into being checked against its own shadow reach too
					// (still just this same coarse AABB test, so it can't
					// truly detect self-shadowing -- it's a blunt "always
					// include" for this instance when enabled, not a real
					// self-shadow test).
					bool is_self = p_cull_result[caster_idx[ci]] == instance;
					if (is_self && !instance->shadow_relight_self) {
						continue;
					}
					// Bound this PAIR's sweep to how far the shadow actually
					// needs to travel to plausibly reach this receiver, not the
					// full (and here, wildly oversized) real extrude distance --
					// see this function's own leading comment on why a fixed
					// global sweep length defeated lateral separation for a
					// diagonal light in a small scene.
					Vector3 caster_center = caster_world_aabb[ci].position + caster_world_aabb[ci].size * 0.5f;
					Vector3 receiver_center = world_aabb.position + world_aabb.size * 0.5f;
					real_t pair_sweep_len = MIN((real_t)SHADOW_EXTRUDE_DISTANCE, caster_center.distance_to(receiver_center) + receiver_radius);
					Vector3 extrude_vec = light_travel_dir * pair_sweep_len;
					AABB swept = caster_world_aabb[ci];
					AABB translated = swept;
					translated.position += extrude_vec;
					swept.merge_with(translated);
					if (world_aabb.intersects(swept)) {
						touched = true;
						break;
					}
				}
				if (!touched) {
					continue;
				}
			}
		}

		bool matrix_pushed = false;
		for (int s = 0; s < mesh->surfaces.size(); s++) {
			RasterizerStorageGLFF::Surface *surface = mesh->surfaces[s];
			if (surface->vertex_count == 0 || !surface->has_normals) {
				continue;
			}

			RID mat_rid = instance->material_override.is_valid() ? instance->material_override : ((s < instance->materials.size() && instance->materials[s].is_valid()) ? instance->materials[s] : surface->material);
			RasterizerStorageGLFF::Material *mat = p_storage->material_owner.getornull(mat_rid);
			RasterizerStorageGLFF::Shader *shader = (mat && mat->shader.is_valid()) ? p_storage->shader_owner.getornull(mat->shader) : nullptr;
			bool surface_on_top = shader && shader->depth_test_disabled;
			bool surface_unshaded = (mat && mat->ff_active) ? mat->ff_unshaded : (shader && shader->unshaded);
			if (surface_on_top || surface_unshaded) {
				continue; // matches the base pass's own pass==0/!surface_unshaded gating
			}

			// godot-ports#26 bugfix: this pass used to hardcode
			// glCullFace(GL_BACK) for every surface regardless of its real
			// cull_mode, while the base pass (further down in this same
			// function) correctly honors each material's own setting. For
			// any double-sided surface (cull_mode == GLFF_CULL_DISABLED --
			// e.g. every material in this project's own mob.glb/player.glb,
			// confirmed via their glTF source: all `"doubleSided":true`),
			// this meant a real, camera/rotation-dependent subset of the
			// base pass's own visible fragments (whichever triangles are
			// currently back-facing on a double-sided mesh) got silently
			// skipped here -- present in the base pass, absent from the
			// relight pass -- so those fragments permanently lost the
			// primary light's contribution for however long that triangle
			// stayed back-facing. As an animated/rotating character (the
			// idle float/bob AnimationPlayer both Player and Mob use)
			// slowly turns, which triangles are back-facing keeps changing,
			// which reads as exactly the reported symptom: the moving
			// characters flickering in brightness while the static,
			// single-sided Ground does not.
			RasterizerStorageGLFF::GLFFCullMode effective_cull_mode = (mat && mat->ff_active) ? mat->ff_cull_mode : (shader ? shader->cull_mode : RasterizerStorageGLFF::GLFF_CULL_BACK);
			if (effective_cull_mode == RasterizerStorageGLFF::GLFF_CULL_FRONT) {
				glEnable(GL_CULL_FACE);
				glCullFace(GL_FRONT);
			} else if (effective_cull_mode == RasterizerStorageGLFF::GLFF_CULL_DISABLED) {
				glDisable(GL_CULL_FACE);
			} else {
				glEnable(GL_CULL_FACE);
				glCullFace(GL_BACK);
			}

			if (!matrix_pushed) {
				glPushMatrix();
				GLfloat gl_model[16];
				_load_transform_gl(instance->transform, gl_model);
				glMultMatrixf(gl_model);
				matrix_pushed = true;
			}

			Color albedo = mat ? mat->albedo : Color(1, 1, 1, 1);
			GLfloat mat_diffuse[4] = { albedo.r, albedo.g, albedo.b, 1.0f };
			glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_diffuse);
			glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mat_diffuse);
			// godot-ports#26 bugfix (original): GL_SPECULAR/GL_SHININESS
			// were never reset per-surface here, so this pass silently
			// inherited whatever the BASE pass's own specular
			// approximation (godot-ports#24) last left active -- a real,
			// order-dependent state leak, fixed at the time by zeroing
			// both explicitly.
			//
			// godot-ports#58: the zero VALUE from that fix is no longer
			// needed at all, in EITHER mode. Real GL_SPECULAR/GL_SHININESS
			// (mirroring godot-ports#24's own SPECULAR_PHONG
			// approximation exactly) is always safe to set here now,
			// because the LIGHT's own enabled/disabled state (toggled
			// above, per mode) is what actually gates whether it
			// contributes: ADDITIVE enables it (this draw only ever adds,
			// on the unshadowed side, so real specular is pure upside);
			// SUBTRACTIVE disables it (this draw overwrites the shadowed
			// side with "everything except this light," so a disabled
			// light naturally contributes zero specular from it, no
			// subtraction or cancellation involved). See godot-ports#57
			// for why actually SUBTRACTING a computed specular value
			// (the first attempt) was unsafe -- confirmed live to clamp
			// to solid black under real content.
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
			// godot-ports#26 (original): zeroed unconditionally here to
			// avoid double-counting against the base pass's own emission
			// -- correct for ADDITIVE (this draw adds on top of a base
			// pass that already drew the surface's real emission once).
			// godot-ports#58: SUBTRACTIVE's draw OVERWRITES the shadowed
			// fragment rather than adding to it, so zeroing emission there
			// would erase it from shadowed regions instead of leaving it
			// -- emission isn't light-dependent at all, so it must
			// survive being in shadow. Real value for SUBTRACTIVE, zero
			// (unchanged) for ADDITIVE.
			if (subtractive) {
				bool effective_emission_enabled = (mat && mat->ff_active) ? false : (shader && shader->emission_enabled);
				if (effective_emission_enabled && mat) {
					GLfloat mat_emission[4] = { mat->emission.r * mat->emission_energy, mat->emission.g * mat->emission_energy, mat->emission.b * mat->emission_energy, 1.0f };
					glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, mat_emission);
				} else {
					GLfloat zero_emission[4] = { 0, 0, 0, 1 };
					glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero_emission);
				}
			} else {
				GLfloat zero_emission[4] = { 0, 0, 0, 1 };
				glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, zero_emission);
			}
			// Same leak, same fix, for GL_COLOR_MATERIAL: any surface
			// using FLAG_ALBEDO_FROM_VERTEX_COLOR (common on imported
			// glTF meshes like this project's own mob.glb/player.glb)
			// enables GL_COLOR_MATERIAL(GL_AMBIENT_AND_DIFFUSE) in the
			// base pass. If that's still enabled here, it makes OpenGL
			// track ambient+diffuse from the "current color" instead of
			// the explicit glMaterialfv() calls just above, silently
			// overriding them with whatever color happened to be active
			// -- worse, this backend's asset-dependent (only triggers on
			// vertex-colored meshes, absent from a plain CubeMesh repro).
			// GL_ALPHA_TEST has the identical latent-leak shape (base
			// pass enables/disables it per-surface, relight never
			// touches it) -- fixed alongside for the same reason, even
			// without a confirmed repro for it specifically.
			glDisable(GL_COLOR_MATERIAL);
			glDisable(GL_ALPHA_TEST);

			PoolVector<Vector3>::Read vr = surface->vertices.read();
			PoolVector<Vector3>::Read nr = surface->normals.read();
			glVertexPointer(3, GL_FLOAT, 0, vr.ptr());
			glEnableClientState(GL_NORMAL_ARRAY);
			glNormalPointer(GL_FLOAT, 0, nr.ptr());

			GLenum gl_primitive = _primitive_to_gl(surface->primitive);
			if (surface->index_count > 0) {
				GLenum index_type = (surface->vertex_count >= (1 << 16)) ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
				PoolVector<uint8_t>::Read ir = surface->index_array.read();
				glDrawElements(gl_primitive, surface->index_count, index_type, ir.ptr());
			} else {
				glDrawArrays(gl_primitive, 0, surface->vertex_count);
			}
		}
		if (matrix_pushed) {
			glPopMatrix();
		}
	}
	glDisable(GL_STENCIL_TEST);
	glDisable(p_gl_light);
	glDisable(GL_POLYGON_OFFSET_FILL);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);
	glDisable(GL_BLEND);
	// godot-ports#58: SUBTRACTIVE's shadowed-side draw temporarily set
	// GL_LIGHT_MODEL_AMBIENT to the real scene ambient (see above) --
	// explicit reset here rather than relying on next frame's base pass
	// to overwrite it, matching this function's own established
	// no-assumed-state discipline (the same class of bug godot-ports#26
	// fixed for specular/emission leaking to whatever runs next).
	GLfloat cleanup_zero_amb[4] = { 0, 0, 0, 1 };
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, cleanup_zero_amb);
	glDisableClientState(GL_NORMAL_ARRAY);
}

// godot-ports#45: real per-instance CPU-side rendering for
// VS::INSTANCE_MULTIMESH (CPUParticles/CPUParticles3D's own backing
// instance type -- confirmed by reading scene/3d/cpu_particles.cpp/.h
// directly, not assumed: it allocates a real MultiMesh, packs per-particle
// state via CPUParticles::_fill_particle_data() into the exact layout
// RasterizerStorageGLFF::MultiMesh now stores for real, and pushes it via
// multimesh_set_as_bulk_array() every frame). No real GPU instancing
// exists on GL 1.2 -- one real glDrawElements/glDrawArrays per active
// instance, matching this project's own already-established "GLES2 also
// does CPU-side per-instance MultiMesh draws" precedent (#16's design
// audit).
//
// Real root cause this issue actually chases: Material3D's billboard
// modes (BILLBOARD_ENABLED/BILLBOARD_FIXED_Y/BILLBOARD_PARTICLES -- the
// latter is what a real ParticlesMaterial-driven CPUParticles node's own
// SpatialMaterial sets) are implemented by overwriting MODELVIEW_MATRIX in
// generated vertex-shader code, computed live from CAMERA_MATRIX/
// INV_CAMERA_MATRIX/WORLD_MATRIX every frame (scene/resources/material.cpp,
// confirmed by direct read, not inferred from the #39 gizmo precedent
// alone) -- a real per-frame camera-relative computation GLFF (no vertex
// shader stage at all) can never execute. This function computes the SAME
// real math on the CPU instead, per active particle instance, replacing
// the instance's own world-space basis before it reaches
// glMultMatrixf() -- see each BillboardMode case below for the literal
// GLSL formula it mirrors.
//
// Real, explicit scope cuts (not oversights): only the plain-SpatialMaterial
// albedo/texture/lighting path is supported for multimesh instances --
// FixedFunctionMaterial's own multi-texture-unit combiner state
// (godot-ports#35) is NOT replicated here, since every real
// ParticlesMaterial-driven particle system this project has needed so far
// authors a plain SpatialMaterial with a billboard mode, never
// FixedFunctionMaterial. BILLBOARD_PARTICLES' own animation-frame UV
// sub-feature (particles_anim_h_frames/v_frames spritesheet flipbook) is
// also NOT implemented this pass -- particles render with the material's
// whole texture/UV as authored, no flipbook animation; a real, flagged
// remainder, not silently dropped. Multimesh instances neither cast nor
// receive the #26 shadow-volume pass or the #23 baked-lightmap pass
// (both scoped to INSTANCE_MESH only, and neither is a common particle
// use case).
static void _render_multimesh_instances(RasterizerStorageGLFF *p_storage, const Transform &p_cam_transform, RasterizerScene::InstanceBase **p_cull_result, int p_cull_count, int p_max_lights) {
	for (int i = 0; i < p_cull_count; i++) {
		RasterizerScene::InstanceBase *instance = p_cull_result[i];
		if (!instance->visible || instance->base_type != VS::INSTANCE_MULTIMESH) {
			continue;
		}
		RasterizerStorageGLFF::MultiMesh *mm = p_storage->multimesh_owner.getornull(instance->base);
		if (!mm || !mm->mesh.is_valid() || mm->transform_format != VS::MULTIMESH_TRANSFORM_3D) {
			continue;
		}
		RasterizerStorageGLFF::Mesh *mesh = p_storage->mesh_owner.getornull(mm->mesh);
		if (!mesh) {
			continue;
		}

		RID mat_rid;
		if (instance->material_override.is_valid()) {
			mat_rid = instance->material_override;
		} else if (instance->materials.size() > 0 && instance->materials[0].is_valid()) {
			mat_rid = instance->materials[0];
		} else if (mesh->surfaces.size() > 0) {
			mat_rid = mesh->surfaces[0]->material;
		}
		RasterizerStorageGLFF::Material *mat = p_storage->material_owner.getornull(mat_rid);
		RasterizerStorageGLFF::Shader *shader = (mat && mat->shader.is_valid()) ? p_storage->shader_owner.getornull(mat->shader) : nullptr;

		bool surface_unshaded = (mat && mat->ff_active) ? mat->ff_unshaded : (shader && shader->unshaded);
		if (surface_unshaded || p_max_lights == 0) {
			glDisable(GL_LIGHTING);
		} else {
			glEnable(GL_LIGHTING);
		}

		Color albedo = mat ? mat->albedo : Color(1, 1, 1, 1);
		RasterizerStorageGLFF::Texture *tex = (mat && mat->albedo_texture.is_valid()) ? p_storage->texture_owner.getornull(mat->albedo_texture) : nullptr;
		if (tex) {
			tex = tex->get_ptr();
		}
		if (tex) {
			glEnable(GL_TEXTURE_2D);
			glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
			glBindTexture(GL_TEXTURE_2D, tex->tex_id);
		} else {
			glDisable(GL_TEXTURE_2D);
		}

		RasterizerStorageGLFF::Shader::BillboardMode billboard = shader ? shader->billboard_mode : RasterizerStorageGLFF::Shader::BILLBOARD_DISABLED;

		int visible = (mm->visible_instances >= 0) ? MIN(mm->visible_instances, mm->instance_count) : mm->instance_count;
		int stride = mm->stride();
		const float *data = mm->data.ptr();
		Vector3 cam_x = p_cam_transform.basis.get_axis(0).normalized();
		Vector3 cam_y = p_cam_transform.basis.get_axis(1).normalized();
		Vector3 cam_z = p_cam_transform.basis.get_axis(2).normalized();

		for (int p = 0; p < visible; p++) {
			const float *d = data + p * stride;
			if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[4] == 0 && d[5] == 0 && d[6] == 0 && d[8] == 0 && d[9] == 0 && d[10] == 0) {
				continue; // real, zeroed inactive-slot marker (CPUParticles's own convention)
			}

			Basis inst_basis;
			inst_basis.elements[0] = Vector3(d[0], d[1], d[2]);
			inst_basis.elements[1] = Vector3(d[4], d[5], d[6]);
			inst_basis.elements[2] = Vector3(d[8], d[9], d[10]);
			Vector3 inst_origin(d[3], d[7], d[11]);

			Transform world_xform = instance->transform * Transform(inst_basis, inst_origin);

			switch (billboard) {
				case RasterizerStorageGLFF::Shader::BILLBOARD_ENABLED: {
					// GLSL: MODELVIEW_MATRIX = INV_CAMERA_MATRIX * mat4(CAMERA_MATRIX[0],CAMERA_MATRIX[1],CAMERA_MATRIX[2],WORLD_MATRIX[3]);
					// i.e. full billboard: copy the camera's own world basis outright, keep the real world origin.
					world_xform.basis = p_cam_transform.basis;
				} break;
				case RasterizerStorageGLFF::Shader::BILLBOARD_FIXED_Y: {
					// GLSL: right = normalize(cross(vec3(0,1,0), CAMERA_MATRIX[2].xyz)); fwd = normalize(cross(CAMERA_MATRIX[0].xyz, vec3(0,1,0))); Y fixed.
					// NOTE: Basis's own 3-Vector3 constructor takes ROWS, not columns (confirmed
					// via core/math/basis.h) -- built via set_axis() (confirmed COLUMN-setter)
					// instead, to avoid silently building the transpose of the intended basis.
					Vector3 up(0, 1, 0);
					Vector3 right = up.cross(cam_z).normalized();
					Vector3 fwd = cam_x.cross(up).normalized();
					Basis fixed_y_basis;
					fixed_y_basis.set_axis(0, right);
					fixed_y_basis.set_axis(1, up);
					fixed_y_basis.set_axis(2, fwd);
					world_xform.basis = fixed_y_basis;
				} break;
				case RasterizerStorageGLFF::Shader::BILLBOARD_PARTICLES: {
					// GLSL: mat_world's basis columns = camera's own X/Y/Z axes, X&Y scaled by
					// length(WORLD_MATRIX[0]), Z scaled by length(WORLD_MATRIX[2]); then rotated
					// around the resulting local Z by INSTANCE_CUSTOM.x (the per-particle angle).
					// Same set_axis()-based construction as BILLBOARD_FIXED_Y above, same reason.
					float scale_xy = world_xform.basis.get_axis(0).length();
					float scale_z = world_xform.basis.get_axis(2).length();
					Basis b;
					b.set_axis(0, cam_x * scale_xy);
					b.set_axis(1, cam_y * scale_xy);
					b.set_axis(2, cam_z * scale_z);
					float angle = (mm->custom_data_floats > 0) ? d[stride - mm->custom_data_floats] : 0.0f;
					world_xform.basis = b.rotated(cam_z, angle);
				} break;
				default:
					break;
			}

			glPushMatrix();
			GLfloat gl_model[16];
			_load_transform_gl(world_xform, gl_model);
			glMultMatrixf(gl_model);

			Color inst_color = albedo;
			if (mm->color_floats > 0) {
				const float *cd = d + mm->xform_floats;
				Color pc;
				if (mm->color_format == VS::MULTIMESH_COLOR_8BIT) {
					const uint8_t *c8 = (const uint8_t *)cd;
					pc = Color(c8[0] / 255.0f, c8[1] / 255.0f, c8[2] / 255.0f, c8[3] / 255.0f);
				} else {
					pc = Color(cd[0], cd[1], cd[2], cd[3]);
				}
				inst_color = Color(albedo.r * pc.r, albedo.g * pc.g, albedo.b * pc.b, albedo.a * pc.a);
			}
			GLfloat mat_diffuse[4] = { inst_color.r, inst_color.g, inst_color.b, inst_color.a };
			glColor4f(inst_color.r, inst_color.g, inst_color.b, inst_color.a);
			glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, mat_diffuse);
			glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, mat_diffuse);

			if (inst_color.a < 0.999f) {
				glEnable(GL_BLEND);
				glBlendEquation(GL_FUNC_ADD);
				glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			} else {
				glDisable(GL_BLEND);
			}

			for (int s = 0; s < mesh->surfaces.size(); s++) {
				RasterizerStorageGLFF::Surface *surface = mesh->surfaces[s];
				if (surface->vertex_count == 0) {
					continue;
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

				PoolVector<Vector2>::Read ur;
				if (tex && surface->has_uvs) {
					_set_tex_matrix_scale(tex);
					ur = surface->uvs.read();
					glEnableClientState(GL_TEXTURE_COORD_ARRAY);
					glTexCoordPointer(2, GL_FLOAT, 0, ur.ptr());
				} else {
					glDisableClientState(GL_TEXTURE_COORD_ARRAY);
				}
				glDisableClientState(GL_COLOR_ARRAY);

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
	}
	glDisable(GL_BLEND);
	_set_tex_matrix_scale(nullptr);
}

// Phase 3 (godot-ports#14 proposal): real mesh/material rendering, walking
// p_cull_result instead of Phase 1's hardcoded test triangle. Scope
// deliberately excludes (see rasterizer_storage_glff.h's Surface/Material
// comments): normal-mapping (no tangents), lightmaps (no UV2), skinning (no
// bones/weights -- fine for this driver's Phase 5 acceptance test, whose
// player/mob animation is pure Pivot-node transform, not skeletal), and the
// rest of SpatialMaterial's PBR params beyond albedo color/texture. Real
// stencil shadow volumes for one primary DirectionalLight (godot-ports#26)
// ARE implemented -- see _render_primary_shadow_and_relight() above and
// this function's own call site further down.
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
	// godot-ports#26 bugfix: GL_STENCIL_BUFFER_BIT is folded into this
	// same top-of-frame clear (was a separate, later glClear(GL_STENCIL_
	// BUFFER_BIT) call inside _render_primary_shadow_and_relight()).
	// Confirmed via a minimal isolated repro (a single-mesh scene,
	// stepped one discrete transform change at a time) that the visible
	// per-object brightness was toggling between two states on EVERY
	// distinct transform-change event regardless of the actual resulting
	// orientation -- e.g. resetting to the exact same zero rotation twice
	// in a row still flipped the visible state each time -- which rules
	// out anything keyed off geometry/orientation and points at a
	// stateful clear/buffer issue instead. A stencil-only glClear issued
	// well after, and separately from, the frame's main color+depth
	// clear is exactly the kind of thing that isn't guaranteed to behave
	// consistently on an old driver -- folding it into one combined
	// clear at the top of the frame removes that separation entirely.
	if (env && (env->bg_mode == VS::ENV_BG_COLOR || env->bg_mode == VS::ENV_BG_CANVAS || env->bg_mode == VS::ENV_BG_COLOR_SKY)) {
		glClearColor(env->bg_color.r, env->bg_color.g, env->bg_color.b, 1.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	} else {
		glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
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

	// godot-ports#38: dynamic Dot3 light tracking. The scene's first/
	// primary DirectionalLight's world-space "toward the light" unit
	// vector (Godot directional lights shine along -Z, so this is +Z of
	// their transform -- same convention already used for GL_POSITION
	// below), captured once here and reused per-instance further down
	// for any FixedFunctionMaterial opting into ff_dot3_dynamic_light.
	// Deliberately just the first directional light found (matching this
	// backend's existing single-primary-light treatment elsewhere), not
	// a full multi-light Dot3 blend -- see #38's own success criteria.
	bool has_primary_directional_light = false;
	Vector3 primary_directional_light_dir_world;
	// godot-ports#26: which GL_LIGHTn (if any) is the primary shadow-
	// casting directional light this frame, and whether it actually wants
	// shadows (Light::shadow_enabled, wired for real by this issue --
	// previously always false, a no-op stub). -1 means "no shadow pass
	// this frame" -- the common case renders exactly as before.
	GLenum primary_shadow_gl_light = 0;
	bool primary_light_casts_shadow = false;
	// godot-ports#56: additive (default) vs subtractive relight, read off
	// the same primary directional light shadow_enabled comes from.
	bool primary_light_relight_subtractive = false;
	// godot-ports#47: per-light shadow-caster budget override, read off
	// the same primary directional light. Defaults match the values that
	// used to be hardcoded (3 distance-budget, 8 priority-budget).
	int primary_light_max_distance_casters = 3;
	int primary_light_max_priority_casters = 8;

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
				if (!has_primary_directional_light) {
					has_primary_directional_light = true;
					primary_directional_light_dir_world = dir;
					primary_shadow_gl_light = gl_light;
					primary_light_casts_shadow = light->shadow_enabled;
					primary_light_relight_subtractive = light->shadow_relight_mode == VS::SHADOW_RELIGHT_MODE_SUBTRACTIVE;
					primary_light_max_distance_casters = light->shadow_max_distance_casters;
					primary_light_max_priority_casters = light->shadow_max_priority_casters;
				}
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

	// godot-ports#26: the base pass below must NOT include the shadow-
	// casting light's own contribution -- it gets added back in
	// separately by _render_primary_shadow_and_relight(), masked to only
	// the unshadowed fragments. Its GL_POSITION/GL_DIFFUSE/etc are already
	// set above; disabling it here only turns off its CONTRIBUTION for
	// this base pass, it stays fully configured for later re-enabling.
	// godot-ports#56: in SUBTRACTIVE mode the base pass keeps it enabled --
	// everything is drawn fully lit here, and the second pass subtracts
	// this light's contribution back out only where shadowed instead.
	if (primary_light_casts_shadow && !primary_light_relight_subtractive) {
		glDisable(primary_shadow_gl_light);
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

			// godot-ports#38: the primary directional light's world-space
			// direction, re-expressed in THIS instance's own object space
			// (orthonormalized -- scale doesn't apply to a direction --
			// then transposed, i.e. inverted, since it's already
			// orthonormal). Computed once per instance, not per-unit:
			// every Dot3-combine unit on every surface of this instance
			// that opts into ff_dot3_dynamic_light shares the same value.
			Vector3 instance_dot3_dynamic_dir;
			if (has_primary_directional_light) {
				Basis inv_rot = instance->transform.basis.orthonormalized().transposed();
				instance_dot3_dynamic_dir = inv_rot.xform(primary_directional_light_dir_world).normalized();
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
						// godot-ports#38: a material opting into dynamic
						// light tracking gets the real primary directional
						// light's direction (already converted to this
						// instance's object space above) instead of its
						// own authored static ff_dot3_light_direction --
						// falls back to the static value when the scene has
						// no directional light at all, rather than an
						// undefined/zero direction.
						const Vector3 &effective_dot3_dir = (mat->ff_dot3_dynamic_light && has_primary_directional_light) ? instance_dot3_dynamic_dir : mat->ff_dot3_light_direction;
						bool wants_uv_array = _ff_setup_texture_unit(this, GL_TEXTURE0 + u, second_operand_source, ff_tex, mat->ff_env_mode[u], mat->ff_combine_func[u], mat->ff_texgen_mode[u], effective_dot3_dir);
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
					_set_tex_matrix_scale(tex); // godot-ports#42
					ur = surface->uvs.read();
					glEnableClientState(GL_TEXTURE_COORD_ARRAY);
					glTexCoordPointer(2, GL_FLOAT, 0, ur.ptr());
				} else {
					glDisable(GL_TEXTURE_2D);
					glDisableClientState(GL_TEXTURE_COORD_ARRAY);
					_set_tex_matrix_scale(nullptr); // godot-ports#42: reset GL_TEXTURE0, the unit this branch's sibling above uses
				}

				GLenum gl_primitive = _primitive_to_gl(surface->primitive);

				// godot-ports#45: real point-primitive size. A real, core
				// GL 1.0 fixed-function state -- GL_POINTS geometry (e.g.
				// editor gizmo handle markers) is inherently always
				// screen-facing already, no billboard trick needed, but
				// was rendering at OpenGL's own default (1px) since
				// nothing here ever called glPointSize() before now.
				if (surface->primitive == VS::PRIMITIVE_POINTS) {
					glPointSize((mat && !mat->ff_active) ? MAX(1.0f, mat->point_size) : 1.0f);
				}

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
						// godot-ports#42: composed with the lightmap
						// texture's own POT-padding scale (godot-ports#40),
						// since lightmap_uv_rect is normalized against its
						// LOGICAL size, not the real (possibly POT-padded)
						// GL storage -- applied outermost so it maps the
						// atlas-remapped coordinate into the real texture's
						// populated sub-rect.
						float lm_scale_u, lm_scale_v;
						_get_tex_uv_scale_3d(lightmap_tex, lm_scale_u, lm_scale_v);
						glMatrixMode(GL_TEXTURE);
						glPushMatrix();
						glLoadIdentity();
						if (lm_scale_u != 1.0f || lm_scale_v != 1.0f) {
							glScalef(lm_scale_u, lm_scale_v, 1.0f);
						}
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
				//
				// Switching which unit is ACTIVE is not enough on its own --
				// GL_TEXTURE_2D's enabled state is tracked independently
				// PER UNIT, not just on whichever unit is currently active.
				// _ff_setup_texture_unit() enables unit 1+ directly and
				// nothing else ever disables it again once this material's
				// draw is done, so it stayed permanently enabled/bound/
				// combining for the rest of the frame (and every frame
				// after) -- a real, confirmed bug: an FF material using 2+
				// units visibly tinted the ENTIRE editor (2D UI panels,
				// unrelated 3D geometry, the skydome) with its unit-1
				// texture, and is the likely trigger for a real ATI driver
				// hang seen switching Env Mode to Combine afterward (the
				// driver ending up evaluating an inconsistent multi-unit
				// combiner state across units the rest of the code never
				// expected to still be active). Explicitly disable every
				// unit above 0 here, not just re-select unit 0.
				if (has_multitexture && (highest_unit_used > 0 || (mat && mat->ff_active))) {
					for (int u = 1; u < RasterizerStorageGLFF::FF_TEXTURE_UNIT_MAX; u++) {
						glActiveTexture(GL_TEXTURE0 + u);
						glClientActiveTexture(GL_TEXTURE0 + u);
						glDisable(GL_TEXTURE_2D);
						glDisable(GL_TEXTURE_GEN_S);
						glDisable(GL_TEXTURE_GEN_T);
						glDisableClientState(GL_TEXTURE_COORD_ARRAY);
					}
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

	// godot-ports#45: real per-instance MultiMesh (CPUParticles/CPUParticles3D)
	// rendering, including the CPU-computed billboard-matrix substitute --
	// see _render_multimesh_instances()'s own header comment above for the
	// full account. Placed alongside the main opaque/on-top passes (same
	// ambient/ambient-restore state already active), before the shadow and
	// glow passes so particles are visible in a glow capture too.
	_render_multimesh_instances(storage, p_cam_transform, p_cull_result, p_cull_count, max_lights);

	// godot-ports#26: real stencil-shadow-volume pass + additive relight
	// for the one primary shadow-casting DirectionalLight, if any. Must
	// run AFTER the two passes above (needs their real, already-populated
	// depth buffer to test shadow-volume fragments against) and BEFORE
	// the glow capture below (glow should see the fully shadow-relit
	// scene, matching how it already sees the lightmap pass's own
	// contribution from inside the loop above).
	if (primary_light_casts_shadow) {
		_render_primary_shadow_and_relight(storage, primary_shadow_gl_light, primary_directional_light_dir_world, p_cam_transform, p_cull_result, p_cull_count, primary_light_relight_subtractive, ambient_color, primary_light_max_distance_casters, primary_light_max_priority_casters);
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
