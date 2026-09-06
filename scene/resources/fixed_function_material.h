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
// Second pass (2026-09-05): 4 texture units (a fixed authoring-surface
// ceiling comfortably covering this era's real hardware, not a literal
// live GL_MAX_TEXTURE_UNITS query -- see rasterizer_storage_glff.h's
// FF_TEXTURE_UNIT_MAX comment for why), texgen (#32's authoring
// surface), and Inspector-side hardware-capability graying driven by a
// new project-level "rendering/quality/gl_fixed_function/target_gpu"
// setting (main.cpp) rather than live-querying the editing machine's
// own GPU -- see _validate_property() below. First pass (2 units,
// Combine/Dot3, no texgen/graying) shipped as godot-ports#35's initial
// commit; this pass completes the issue's original success criteria.
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

	enum TexgenMode {
		TEXGEN_NONE,
		TEXGEN_SPHERE_MAP,
		TEXGEN_REFLECTION_MAP,
		TEXGEN_OBJECT_LINEAR,
		TEXGEN_EYE_LINEAR,
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

	// Target-GPU tiers for rendering/quality/gl_fixed_function/target_gpu
	// (main.cpp) -- must stay in the same order as that setting's
	// PROPERTY_HINT_ENUM string.
	enum TargetGPUTier {
		TARGET_GPU_RAGE_PRO,
		TARGET_GPU_RAGE_128,
		TARGET_GPU_RV250_AND_NEWER,
	};

	static const int TEXTURE_UNIT_MAX = 4;

private:
	Ref<Texture> texture_unit_texture[TEXTURE_UNIT_MAX];
	EnvMode texture_unit_env_mode[TEXTURE_UNIT_MAX];
	CombineFunc texture_unit_combine_func[TEXTURE_UNIT_MAX];
	TexgenMode texture_unit_texgen_mode[TEXTURE_UNIT_MAX];

	bool unshaded;
	bool depth_test_disabled;
	CullMode cull_mode;
	BlendMode blend_mode;

	// godot-ports#25: the one baked/static light direction a
	// COMBINE_DOT3-mode texture unit dots its normal-map texel against,
	// authored in the mesh's own object space. Material-level (not
	// per-unit) since a single spike/authoring surface only needs one
	// bump unit at a time -- see rasterizer_scene_glff.cpp's
	// _ff_setup_texture_unit() for how this becomes a GL_CONSTANT
	// texture-environment color.
	Vector3 dot3_light_direction;

	// Pushes every property's current value to the VisualServer under the
	// well-known param names RasterizerStorageGLFF::material_set_param()
	// recognizes (see rasterizer_storage_glff.h). Called once from the
	// constructor (defaults) and again from every setter, mirroring how
	// SpatialMaterial's own setters immediately call material_set_param()
	// rather than batching updates.
	void _update_material_param(const StringName &p_param, const Variant &p_value);

	// How many texture units the CURRENT rendering/quality/gl_fixed_function/
	// target_gpu project setting exposes (1 for Rage Pro, 2 for Rage 128,
	// TEXTURE_UNIT_MAX for RV250+). Also used by _validate_property().
	static int _get_target_gpu_tier();
	static int _get_tier_unit_count(int p_tier);
	static bool _get_tier_has_dot3(int p_tier);
	static bool _get_tier_has_reflection_map(int p_tier);

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &property) const;

public:
	void set_texture_unit_texture(int p_unit, const Ref<Texture> &p_texture);
	Ref<Texture> get_texture_unit_texture(int p_unit) const;

	void set_texture_unit_env_mode(int p_unit, EnvMode p_mode);
	EnvMode get_texture_unit_env_mode(int p_unit) const;

	void set_texture_unit_combine_func(int p_unit, CombineFunc p_func);
	CombineFunc get_texture_unit_combine_func(int p_unit) const;

	void set_texture_unit_texgen_mode(int p_unit, TexgenMode p_mode);
	TexgenMode get_texture_unit_texgen_mode(int p_unit) const;

	// Per-slot property forwarding (texture_unit_0_*.. texture_unit_3_*) --
	// ClassDB needs fixed, individually-bound methods (no runtime-sized
	// property arrays in this Godot version's ADD_PROPERTY convention).
#define FF_DECLARE_TEXTURE_UNIT_ACCESSORS(m_unit)                                                                                    \
	void set_texture_unit_##m_unit##_texture(const Ref<Texture> &p_texture) { set_texture_unit_texture(m_unit, p_texture); }         \
	Ref<Texture> get_texture_unit_##m_unit##_texture() const { return get_texture_unit_texture(m_unit); }                            \
	void set_texture_unit_##m_unit##_env_mode(EnvMode p_mode) { set_texture_unit_env_mode(m_unit, p_mode); }                         \
	EnvMode get_texture_unit_##m_unit##_env_mode() const { return get_texture_unit_env_mode(m_unit); }                               \
	void set_texture_unit_##m_unit##_combine_func(CombineFunc p_func) { set_texture_unit_combine_func(m_unit, p_func); }             \
	CombineFunc get_texture_unit_##m_unit##_combine_func() const { return get_texture_unit_combine_func(m_unit); }                   \
	void set_texture_unit_##m_unit##_texgen_mode(TexgenMode p_mode) { set_texture_unit_texgen_mode(m_unit, p_mode); }                \
	TexgenMode get_texture_unit_##m_unit##_texgen_mode() const { return get_texture_unit_texgen_mode(m_unit); }

	FF_DECLARE_TEXTURE_UNIT_ACCESSORS(0)
	FF_DECLARE_TEXTURE_UNIT_ACCESSORS(1)
	FF_DECLARE_TEXTURE_UNIT_ACCESSORS(2)
	FF_DECLARE_TEXTURE_UNIT_ACCESSORS(3)
#undef FF_DECLARE_TEXTURE_UNIT_ACCESSORS

	void set_unshaded(bool p_unshaded);
	bool get_unshaded() const;

	void set_depth_test_disabled(bool p_disabled);
	bool get_depth_test_disabled() const;

	void set_cull_mode(CullMode p_mode);
	CullMode get_cull_mode() const;

	void set_blend_mode(BlendMode p_mode);
	BlendMode get_blend_mode() const;

	void set_dot3_light_direction(const Vector3 &p_dir);
	Vector3 get_dot3_light_direction() const;

	virtual Shader::Mode get_shader_mode() const { return Shader::MODE_SPATIAL; }

	FixedFunctionMaterial();
	virtual ~FixedFunctionMaterial();
};

VARIANT_ENUM_CAST(FixedFunctionMaterial::EnvMode)
VARIANT_ENUM_CAST(FixedFunctionMaterial::CombineFunc)
VARIANT_ENUM_CAST(FixedFunctionMaterial::TexgenMode)
VARIANT_ENUM_CAST(FixedFunctionMaterial::CullMode)
VARIANT_ENUM_CAST(FixedFunctionMaterial::BlendMode)

#endif // FIXED_FUNCTION_MATERIAL_H
