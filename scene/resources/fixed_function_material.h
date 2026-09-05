/**************************************************************************/
/*  fixed_function_material.h                                            */
/**************************************************************************/

#ifndef FIXED_FUNCTION_MATERIAL_H
#define FIXED_FUNCTION_MATERIAL_H

#include "scene/resources/material.h"

// Authoring surface for GLFF's real OpenGL 1.2 fixed-function pipeline
// (godot-ports#35) -- a third Material subclass alongside SpatialMaterial/
// ShaderMaterial. Unlike ShaderMaterial, this never compiles or executes
// any shader code (GLFF has no fragment-shader capability at all, by
// design -- see godot-ports#14): every property here maps directly to a
// real fixed-function GL call (glTexEnvi/GL_COMBINE/GL_DOT3_RGB etc,
// GLFF-side in rasterizer_scene_glff.cpp), communicated via
// material_set_param() under well-known names, the same mechanism
// SpatialMaterial already uses for "albedo"/"texture_albedo" (see #17/#24).
//
// First-pass scope (2026-09-05): 2 texture units (this driver's floor
// design assumes at most optional ARB_multitexture, and every chip this
// project has researched -- Rage Pro/128, Radeon 7000, GeForce2 MX --
// supports at least 2 when multitexture is present at all), enough to
// author #25's dot3 bump-mapping technique with zero GLSL. Deferred to
// follow-up, per explicit user-agreed scoping: more texture units,
// texgen (#32), and hardware-capability-driven graying in the Inspector
// (a project-level "minimum target GPU" setting, not live-querying the
// editing machine's own GPU -- see godot-ports#35's discussion).
class FixedFunctionMaterial : public Material {
	GDCLASS(FixedFunctionMaterial, Material);

public:
	enum EnvMode {
		ENV_MODULATE,
		ENV_REPLACE,
		ENV_DECAL,
		ENV_BLEND,
		ENV_COMBINE,
	};

	enum CombineFunc {
		COMBINE_MODULATE,
		COMBINE_ADD,
		COMBINE_SUBTRACT,
		COMBINE_DOT3,
	};

	enum CullMode {
		CULL_BACK,
		CULL_FRONT,
		CULL_DISABLED,
	};

	enum BlendMode {
		BLEND_MIX,
		BLEND_ADD,
		BLEND_SUB,
		BLEND_MUL,
	};

	static const int TEXTURE_UNIT_MAX = 2;

private:
	Ref<Texture> texture_unit_texture[TEXTURE_UNIT_MAX];
	EnvMode texture_unit_env_mode[TEXTURE_UNIT_MAX];
	CombineFunc texture_unit_combine_func[TEXTURE_UNIT_MAX];

	bool unshaded;
	bool depth_test_disabled;
	CullMode cull_mode;
	BlendMode blend_mode;

	// Pushes every property's current value to the VisualServer under the
	// well-known param names RasterizerStorageGLFF::material_set_param()
	// recognizes (see rasterizer_storage_glff.h). Called once from the
	// constructor (defaults) and again from every setter, mirroring how
	// SpatialMaterial's own setters immediately call material_set_param()
	// rather than batching updates.
	void _update_material_param(const StringName &p_param, const Variant &p_value);

protected:
	static void _bind_methods();

public:
	void set_texture_unit_texture(int p_unit, const Ref<Texture> &p_texture);
	Ref<Texture> get_texture_unit_texture(int p_unit) const;

	void set_texture_unit_env_mode(int p_unit, EnvMode p_mode);
	EnvMode get_texture_unit_env_mode(int p_unit) const;

	void set_texture_unit_combine_func(int p_unit, CombineFunc p_func);
	CombineFunc get_texture_unit_combine_func(int p_unit) const;

	// Per-slot property forwarding (texture_unit_0_*/texture_unit_1_*) --
	// ClassDB needs fixed, individually-bound methods (no runtime-sized
	// property arrays in this Godot version's ADD_PROPERTY convention),
	// matching the "fixed max count, not a dynamic array" call made for
	// this first pass.
	void set_texture_unit_0_texture(const Ref<Texture> &p_texture) { set_texture_unit_texture(0, p_texture); }
	Ref<Texture> get_texture_unit_0_texture() const { return get_texture_unit_texture(0); }
	void set_texture_unit_0_env_mode(EnvMode p_mode) { set_texture_unit_env_mode(0, p_mode); }
	EnvMode get_texture_unit_0_env_mode() const { return get_texture_unit_env_mode(0); }
	void set_texture_unit_0_combine_func(CombineFunc p_func) { set_texture_unit_combine_func(0, p_func); }
	CombineFunc get_texture_unit_0_combine_func() const { return get_texture_unit_combine_func(0); }

	void set_texture_unit_1_texture(const Ref<Texture> &p_texture) { set_texture_unit_texture(1, p_texture); }
	Ref<Texture> get_texture_unit_1_texture() const { return get_texture_unit_texture(1); }
	void set_texture_unit_1_env_mode(EnvMode p_mode) { set_texture_unit_env_mode(1, p_mode); }
	EnvMode get_texture_unit_1_env_mode() const { return get_texture_unit_env_mode(1); }
	void set_texture_unit_1_combine_func(CombineFunc p_func) { set_texture_unit_combine_func(1, p_func); }
	CombineFunc get_texture_unit_1_combine_func() const { return get_texture_unit_combine_func(1); }

	void set_unshaded(bool p_unshaded);
	bool get_unshaded() const;

	void set_depth_test_disabled(bool p_disabled);
	bool get_depth_test_disabled() const;

	void set_cull_mode(CullMode p_mode);
	CullMode get_cull_mode() const;

	void set_blend_mode(BlendMode p_mode);
	BlendMode get_blend_mode() const;

	virtual Shader::Mode get_shader_mode() const { return Shader::MODE_SPATIAL; }

	FixedFunctionMaterial();
	virtual ~FixedFunctionMaterial();
};

VARIANT_ENUM_CAST(FixedFunctionMaterial::EnvMode)
VARIANT_ENUM_CAST(FixedFunctionMaterial::CombineFunc)
VARIANT_ENUM_CAST(FixedFunctionMaterial::CullMode)
VARIANT_ENUM_CAST(FixedFunctionMaterial::BlendMode)

#endif // FIXED_FUNCTION_MATERIAL_H
