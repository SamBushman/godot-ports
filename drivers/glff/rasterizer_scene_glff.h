#ifndef RASTERIZER_SCENE_GLFF_H
#define RASTERIZER_SCENE_GLFF_H

#include "core/rid.h"
#include "servers/visual/rasterizer.h"

// 3D scene renderer for the GLFF (OpenGL 1.2 fixed-function) driver.
// render_scene() (see .cpp) is Phase 3's real mesh/material/lighting path:
// walks p_cull_result, decodes each RasterizerStorageGLFF::Surface's
// pre-decoded plain vertex arrays into glVertexPointer/etc., maps albedo
// color/texture from RasterizerStorageGLFF::Material, and drives
// GL_LIGHT0-7 from real Light storage. Multi-pass lightmap blending (#16)
// and shadows are not built here -- see the .cpp's render_scene() comment
// for the full list of what's deliberately out of scope. Everything below
// that a later phase will eventually flesh out is a minimal but
// RID-correct stub (real RID_Owner-backed create/free, no-op setters)
// rather than a bare RID()-returning placeholder, so the object lifecycle
// is safe to extend incrementally.

class RasterizerStorageGLFF;

class RasterizerSceneGLFF : public RasterizerScene {
public:
	RasterizerStorageGLFF *storage;

	/* SHADOW ATLAS (stub -- dynamic shadows dropped, see proposal §8.1) */

	struct ShadowAtlas : public RID_Data {};
	mutable RID_Owner<ShadowAtlas> shadow_atlas_owner;

	virtual RID shadow_atlas_create() { return shadow_atlas_owner.make_rid(memnew(ShadowAtlas)); }
	virtual void shadow_atlas_set_size(RID p_atlas, int p_size) {}
	virtual void shadow_atlas_set_quadrant_subdivision(RID p_atlas, int p_quadrant, int p_subdivision) {}
	virtual bool shadow_atlas_update_light(RID p_atlas, RID p_light_intance, float p_coverage, uint64_t p_light_version) { return false; }
	virtual int get_directional_light_shadow_size(RID p_light_intance) { return 0; }
	virtual void set_directional_shadow_count(int p_count) {}

	/* ENVIRONMENT -- minimal real storage for background color/ambient
	   (Phase 3 will read these for the clear-color/ambient-light path);
	   everything shader-only (sky, glow, SSAO/SSR, tonemap, DOF, fog
	   gradient/adjustment) is a no-op, see proposal §5.1/§8.1 */

	struct Environment : public RID_Data {
		VS::EnvironmentBG bg_mode;
		Color bg_color;
		float bg_energy;
		Color ambient_color;
		float ambient_energy;
		int canvas_max_layer;
		// godot-ports#30: real panorama-sky RID, drawn as an ordinary
		// textured skybox (see render_scene()'s _draw_skybox() call) when
		// bg_mode is ENV_BG_SKY/ENV_BG_COLOR_SKY and this is valid.
		RID sky;
		// godot-ports#31: real glow/bloom (see render_scene()'s
		// _draw_glow() call) via the classic fixed-function-era
		// "downsample capture + bilinear-upsample additive blend"
		// technique -- only enable/intensity are read, the rest of
		// environment_set_glow()'s many HDR/quality params have no
		// fixed-function equivalent and are ignored (this backend has no
		// HDR pipeline at all).
		bool glow_enabled;
		float glow_intensity;

		Environment() {
			bg_mode = VS::ENV_BG_CLEAR_COLOR;
			bg_energy = 1.0;
			ambient_energy = 1.0;
			canvas_max_layer = 0;
			glow_enabled = false;
			glow_intensity = 1.0;
		}
	};
	mutable RID_Owner<Environment> environment_owner;

	virtual RID environment_create() { return environment_owner.make_rid(memnew(Environment)); }
	virtual void environment_set_background(RID p_env, VS::EnvironmentBG p_bg) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND(!e);
		e->bg_mode = p_bg;
	}
	virtual void environment_set_sky(RID p_env, RID p_sky) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND(!e);
		e->sky = p_sky;
	}
	virtual void environment_set_sky_custom_fov(RID p_env, float p_scale) {}
	virtual void environment_set_sky_orientation(RID p_env, const Basis &p_orientation) {}
	virtual void environment_set_bg_color(RID p_env, const Color &p_color) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND(!e);
		e->bg_color = p_color;
	}
	virtual void environment_set_bg_energy(RID p_env, float p_energy) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND(!e);
		e->bg_energy = p_energy;
	}
	virtual void environment_set_canvas_max_layer(RID p_env, int p_max_layer) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND(!e);
		e->canvas_max_layer = p_max_layer;
	}
	virtual void environment_set_ambient_light(RID p_env, const Color &p_color, float p_energy = 1.0, float p_sky_contribution = 0.0) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND(!e);
		e->ambient_color = p_color;
		e->ambient_energy = p_energy;
	}
	virtual void environment_set_camera_feed_id(RID p_env, int p_camera_feed_id) {}
	virtual void environment_set_dof_blur_near(RID p_env, bool p_enable, float p_distance, float p_transition, float p_far_amount, VS::EnvironmentDOFBlurQuality p_quality) {}
	virtual void environment_set_dof_blur_far(RID p_env, bool p_enable, float p_distance, float p_transition, float p_far_amount, VS::EnvironmentDOFBlurQuality p_quality) {}
	virtual void environment_set_glow(RID p_env, bool p_enable, int p_level_flags, float p_intensity, float p_strength, float p_bloom_threshold, VS::EnvironmentGlowBlendMode p_blend_mode, float p_hdr_bleed_threshold, float p_hdr_bleed_scale, float p_hdr_luminance_cap, bool p_bicubic_upscale, bool p_high_quality) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND(!e);
		e->glow_enabled = p_enable;
		e->glow_intensity = p_intensity;
	}
	virtual void environment_set_fog(RID p_env, bool p_enable, float p_begin, float p_end, RID p_gradient_texture) {}
	virtual void environment_set_ssr(RID p_env, bool p_enable, int p_max_steps, float p_fade_int, float p_fade_out, float p_depth_tolerance, bool p_roughness) {}
	virtual void environment_set_ssao(RID p_env, bool p_enable, float p_radius, float p_intensity, float p_radius2, float p_intensity2, float p_bias, float p_light_affect, float p_ao_channel_affect, const Color &p_color, VS::EnvironmentSSAOQuality p_quality, VS::EnvironmentSSAOBlur p_blur, float p_bilateral_sharpness) {}
	virtual void environment_set_tonemap(RID p_env, VS::EnvironmentToneMapper p_tone_mapper, float p_exposure, float p_white, bool p_auto_exposure, float p_min_luminance, float p_max_luminance, float p_auto_exp_speed, float p_auto_exp_scale) {}
	virtual void environment_set_adjustment(RID p_env, bool p_enable, float p_brightness, float p_contrast, float p_saturation, RID p_ramp) {}
	virtual void environment_set_fog(RID p_env, bool p_enable, const Color &p_color, const Color &p_sun_color, float p_sun_amount) {}
	virtual void environment_set_fog_depth(RID p_env, bool p_enable, float p_depth_begin, float p_depth_end, float p_depth_curve, bool p_transmit, float p_transmit_curve) {}
	virtual void environment_set_fog_height(RID p_env, bool p_enable, float p_min_height, float p_max_height, float p_height_curve) {}
	virtual bool is_environment(RID p_env) { return environment_owner.owns(p_env); }
	virtual VS::EnvironmentBG environment_get_background(RID p_env) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND_V(!e, VS::ENV_BG_CLEAR_COLOR);
		return e->bg_mode;
	}
	virtual int environment_get_canvas_max_layer(RID p_env) {
		Environment *e = environment_owner.getornull(p_env);
		ERR_FAIL_COND_V(!e, 0);
		return e->canvas_max_layer;
	}

	/* LIGHT INSTANCE -- minimal real transform storage; per-vertex
	   GL_LIGHT0-7 application from this is Phase 3's job */

	struct LightInstance : public RID_Data {
		RID light;
		Transform transform;
	};
	mutable RID_Owner<LightInstance> light_instance_owner;

	virtual RID light_instance_create(RID p_light) {
		LightInstance *li = memnew(LightInstance);
		li->light = p_light;
		return light_instance_owner.make_rid(li);
	}
	virtual void light_instance_set_transform(RID p_light_instance, const Transform &p_transform) {
		LightInstance *li = light_instance_owner.getornull(p_light_instance);
		ERR_FAIL_COND(!li);
		li->transform = p_transform;
	}
	virtual void light_instance_set_shadow_transform(RID p_light_instance, const CameraMatrix &p_projection, const Transform &p_transform, float p_far, float p_split, int p_pass, float p_bias_scale = 1.0) {}
	virtual void light_instance_mark_visible(RID p_light_instance) {}

	/* REFLECTION ATLAS / PROBE INSTANCE (stub -- reflection probes
	   dropped, see proposal §8.1) */

	struct ReflectionAtlas : public RID_Data {};
	mutable RID_Owner<ReflectionAtlas> reflection_atlas_owner;
	virtual RID reflection_atlas_create() { return reflection_atlas_owner.make_rid(memnew(ReflectionAtlas)); }
	virtual void reflection_atlas_set_size(RID p_ref_atlas, int p_size) {}
	virtual void reflection_atlas_set_subdivision(RID p_ref_atlas, int p_subdiv) {}

	struct ReflectionProbeInstance : public RID_Data {};
	mutable RID_Owner<ReflectionProbeInstance> reflection_probe_instance_owner;
	virtual RID reflection_probe_instance_create(RID p_probe) { return reflection_probe_instance_owner.make_rid(memnew(ReflectionProbeInstance)); }
	virtual void reflection_probe_instance_set_transform(RID p_instance, const Transform &p_transform) {}
	virtual void reflection_probe_release_atlas_index(RID p_instance) {}
	virtual bool reflection_probe_instance_needs_redraw(RID p_instance) { return false; }
	virtual bool reflection_probe_instance_has_reflection(RID p_instance) { return false; }
	virtual bool reflection_probe_instance_begin_render(RID p_instance, RID p_reflection_atlas) { return false; }
	virtual bool reflection_probe_instance_postprocess_step(RID p_instance) { return true; }

	/* GI PROBE INSTANCE (stub -- dropped, see proposal §8.1) */

	struct GIProbeInstance : public RID_Data {};
	mutable RID_Owner<GIProbeInstance> gi_probe_instance_owner;
	virtual RID gi_probe_instance_create() { return gi_probe_instance_owner.make_rid(memnew(GIProbeInstance)); }
	virtual void gi_probe_instance_set_light_data(RID p_probe, RID p_base, RID p_data) {}
	virtual void gi_probe_instance_set_transform_to_data(RID p_probe, const Transform &p_xform) {}
	virtual void gi_probe_instance_set_bounds(RID p_probe, const Vector3 &p_bounds) {}

	/* FRAME */

	virtual void render_scene(const Transform &p_cam_transform, const CameraMatrix &p_cam_projection, const int p_eye, bool p_cam_ortogonal, InstanceBase **p_cull_result, int p_cull_count, RID *p_light_cull_result, int p_light_cull_count, RID *p_reflection_probe_cull_result, int p_reflection_probe_cull_count, RID p_environment, RID p_shadow_atlas, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass);
	virtual void render_shadow(RID p_light, RID p_shadow_atlas, int p_pass, InstanceBase **p_cull_result, int p_cull_count) {}

	virtual void set_scene_pass(uint64_t p_pass) {}
	virtual void set_debug_draw_mode(VS::ViewportDebugDraw p_debug_draw) {}

	virtual bool free(RID p_rid);

	// Real hardware capability, detected once at initialize() by scanning
	// GL_EXTENSIONS -- ARB_multitexture/ARB_texture_env_combine/
	// ARB_texture_env_dot3 are all optional under this backend's strict
	// GL 1.2 floor (see proposal §2), confirmed present on every chip
	// this project has hardware-verified except Rage Pro (no dot3 at
	// all) and Rage 128 (multitexture+combine, no dot3) -- see the
	// godot-ports#25 research this FixedFunctionMaterial authoring
	// surface (godot-ports#35) exists to expose. A FixedFunctionMaterial
	// asking for Combine/Dot3 on hardware that lacks it degrades to
	// GL_MODULATE on that unit rather than emitting an invalid enum.
	bool has_multitexture;
	bool has_texture_env_combine;
	bool has_texture_env_dot3;

	// Real max texture units this GL context actually supports (queried
	// via glGetIntegerv(GL_MAX_TEXTURE_UNITS) when has_multitexture, else
	// 1) -- render_scene() never issues a unit beyond this regardless of
	// what a FixedFunctionMaterial or its project target-GPU tier declare.
	int max_texture_units;

	// GL_REFLECTION_MAP texgen -- core since GL 1.3, also available as
	// GL_NV_texgen_reflection on older hardware (godot-ports#25/#32
	// research). GL_SPHERE_MAP/GL_OBJECT_LINEAR/GL_EYE_LINEAR texgen are
	// all core since GL 1.0 and need no capability gate.
	bool has_texgen_reflection_map;

	// godot-ports#31: GL_SGIS_generate_mipmap (promoted to core
	// GL_GENERATE_MIPMAP in GL 1.4, same token value 0x8191) -- confirmed
	// present in our G4/RV250 GL_EXTENSIONS capture. Used by _draw_glow()
	// to get a real blurred-and-downsampled capture of the just-rendered
	// frame "for free" from the driver's own mip-chain generation,
	// instead of a real multi-pass blur kernel.
	bool has_generate_mipmap;
	// Lazily allocated on first real glow use; a persistent GL texture
	// name recaptured every frame via glCopyTexImage2D, not per-mesh
	// storage (this is a whole-framebuffer post-process, not scene
	// content), so it doesn't belong in RasterizerStorageGLFF's
	// Texture/RID_Owner bookkeeping. Plain "unsigned long", not GLuint --
	// this header (unlike rasterizer_storage_glff.h) never pulls in the
	// real GL headers, and this Tiger SDK's GLuint is a typedef for
	// exactly this (confirmed via a real build error: "invalid conversion
	// from 'unsigned int*' to 'GLuint* {aka long unsigned int*}'" when
	// this field was plain "unsigned int" instead).
	unsigned long glow_capture_tex;
	// The glow_capture_tex's real allocated size -- ROUNDED UP TO A POWER
	// OF TWO. This driver (G4/RV250, a 2002-era chip) rejects a direct
	// glCopyTexImage2D at the viewport's actual (non-POT) size with
	// GL_INVALID_VALUE -- confirmed live via glGetError() during
	// development, not a spec assumption. The fix is the classic one:
	// allocate the texture once at POT size via glTexImage2D, then update
	// just the used (bottom-left) sub-rectangle every frame via
	// glCopyTexSubImage2D (which has no such POT requirement), sampling
	// only that sub-rectangle's UV range thereafter. Re-allocated whenever
	// the real viewport size changes (e.g. window resize).
	int glow_capture_pot_w;
	int glow_capture_pot_h;

	// godot-ports#31: whole-framebuffer glow/bloom via the classic
	// fixed-function-era "downsample capture + forced-small-mip + additive
	// upsample" technique -- see the .cpp definition for the full
	// walkthrough. Called from render_scene() right before its final
	// client-state cleanup, so it captures the fully-composited opaque+
	// blended scene (but not the 2D canvas pass, which runs after
	// render_scene() returns).
	void _draw_glow(float p_intensity);

	void initialize();

	RasterizerSceneGLFF();
	~RasterizerSceneGLFF();
};

#endif // RASTERIZER_SCENE_GLFF_H
