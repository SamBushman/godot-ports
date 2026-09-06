/**************************************************************************/
/*  fixed_function_material.cpp                                          */
/**************************************************************************/

#include "fixed_function_material.h"

#include "core/project_settings.h"

void FixedFunctionMaterial::_update_material_param(const StringName &p_param, const Variant &p_value) {
	VS::get_singleton()->material_set_param(get_rid(), p_param, p_value);
}

void FixedFunctionMaterial::set_texture_unit_texture(int p_unit, const Ref<Texture> &p_texture) {
	ERR_FAIL_INDEX(p_unit, TEXTURE_UNIT_MAX);
	texture_unit_texture[p_unit] = p_texture;
	RID rid = p_texture.is_valid() ? p_texture->get_rid() : RID();
	_update_material_param(StringName("ff_tex" + itos(p_unit)), rid);
	_change_notify();
}

Ref<Texture> FixedFunctionMaterial::get_texture_unit_texture(int p_unit) const {
	ERR_FAIL_INDEX_V(p_unit, TEXTURE_UNIT_MAX, Ref<Texture>());
	return texture_unit_texture[p_unit];
}

void FixedFunctionMaterial::set_texture_unit_env_mode(int p_unit, EnvMode p_mode) {
	ERR_FAIL_INDEX(p_unit, TEXTURE_UNIT_MAX);
	texture_unit_env_mode[p_unit] = p_mode;
	_update_material_param(StringName("ff_env_mode" + itos(p_unit)), (int)p_mode);
	_change_notify();
}

FixedFunctionMaterial::EnvMode FixedFunctionMaterial::get_texture_unit_env_mode(int p_unit) const {
	ERR_FAIL_INDEX_V(p_unit, TEXTURE_UNIT_MAX, ENV_MODULATE);
	return texture_unit_env_mode[p_unit];
}

void FixedFunctionMaterial::set_texture_unit_combine_func(int p_unit, CombineFunc p_func) {
	ERR_FAIL_INDEX(p_unit, TEXTURE_UNIT_MAX);
	texture_unit_combine_func[p_unit] = p_func;
	_update_material_param(StringName("ff_combine_func" + itos(p_unit)), (int)p_func);
	_change_notify();
}

FixedFunctionMaterial::CombineFunc FixedFunctionMaterial::get_texture_unit_combine_func(int p_unit) const {
	ERR_FAIL_INDEX_V(p_unit, TEXTURE_UNIT_MAX, COMBINE_MODULATE);
	return texture_unit_combine_func[p_unit];
}

void FixedFunctionMaterial::set_texture_unit_texgen_mode(int p_unit, TexgenMode p_mode) {
	ERR_FAIL_INDEX(p_unit, TEXTURE_UNIT_MAX);
	texture_unit_texgen_mode[p_unit] = p_mode;
	_update_material_param(StringName("ff_texgen_mode" + itos(p_unit)), (int)p_mode);
	_change_notify();
}

FixedFunctionMaterial::TexgenMode FixedFunctionMaterial::get_texture_unit_texgen_mode(int p_unit) const {
	ERR_FAIL_INDEX_V(p_unit, TEXTURE_UNIT_MAX, TEXGEN_NONE);
	return texture_unit_texgen_mode[p_unit];
}

void FixedFunctionMaterial::set_unshaded(bool p_unshaded) {
	unshaded = p_unshaded;
	_update_material_param("ff_unshaded", p_unshaded);
	_change_notify();
}

bool FixedFunctionMaterial::get_unshaded() const {
	return unshaded;
}

void FixedFunctionMaterial::set_depth_test_disabled(bool p_disabled) {
	depth_test_disabled = p_disabled;
	_update_material_param("ff_depth_test_disabled", p_disabled);
	_change_notify();
}

bool FixedFunctionMaterial::get_depth_test_disabled() const {
	return depth_test_disabled;
}

void FixedFunctionMaterial::set_cull_mode(CullMode p_mode) {
	cull_mode = p_mode;
	_update_material_param("ff_cull_mode", (int)p_mode);
	_change_notify();
}

FixedFunctionMaterial::CullMode FixedFunctionMaterial::get_cull_mode() const {
	return cull_mode;
}

void FixedFunctionMaterial::set_blend_mode(BlendMode p_mode) {
	blend_mode = p_mode;
	_update_material_param("ff_blend_mode", (int)p_mode);
	_change_notify();
}

FixedFunctionMaterial::BlendMode FixedFunctionMaterial::get_blend_mode() const {
	return blend_mode;
}

// X-macro-style repetition for the 4 near-identical texture unit
// property blocks -- both here and in the header's accessor
// declarations, since ClassDB::bind_method needs a distinct method
// pointer literal per unit (no runtime-sized property arrays in this
// Godot version's ADD_PROPERTY convention).
#define FF_BIND_TEXTURE_UNIT(m_unit)                                                                                                                                       \
	ClassDB::bind_method(D_METHOD("set_texture_unit_" #m_unit "_texture", "texture"), &FixedFunctionMaterial::set_texture_unit_##m_unit##_texture);                       \
	ClassDB::bind_method(D_METHOD("get_texture_unit_" #m_unit "_texture"), &FixedFunctionMaterial::get_texture_unit_##m_unit##_texture);                                  \
	ClassDB::bind_method(D_METHOD("set_texture_unit_" #m_unit "_env_mode", "mode"), &FixedFunctionMaterial::set_texture_unit_##m_unit##_env_mode);                        \
	ClassDB::bind_method(D_METHOD("get_texture_unit_" #m_unit "_env_mode"), &FixedFunctionMaterial::get_texture_unit_##m_unit##_env_mode);                                \
	ClassDB::bind_method(D_METHOD("set_texture_unit_" #m_unit "_combine_func", "func"), &FixedFunctionMaterial::set_texture_unit_##m_unit##_combine_func);                \
	ClassDB::bind_method(D_METHOD("get_texture_unit_" #m_unit "_combine_func"), &FixedFunctionMaterial::get_texture_unit_##m_unit##_combine_func);                        \
	ClassDB::bind_method(D_METHOD("set_texture_unit_" #m_unit "_texgen_mode", "mode"), &FixedFunctionMaterial::set_texture_unit_##m_unit##_texgen_mode);                  \
	ClassDB::bind_method(D_METHOD("get_texture_unit_" #m_unit "_texgen_mode"), &FixedFunctionMaterial::get_texture_unit_##m_unit##_texgen_mode);                          \
	ADD_GROUP("Texture Unit " #m_unit, "texture_unit_" #m_unit "_");                                                                                                       \
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture_unit_" #m_unit "_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture"), "set_texture_unit_" #m_unit "_texture", "get_texture_unit_" #m_unit "_texture"); \
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_unit_" #m_unit "_env_mode", PROPERTY_HINT_ENUM, "Modulate,Replace,Decal,Blend,Combine"), "set_texture_unit_" #m_unit "_env_mode", "get_texture_unit_" #m_unit "_env_mode"); \
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_unit_" #m_unit "_combine_func", PROPERTY_HINT_ENUM, "Modulate,Add,Subtract,Dot3"), "set_texture_unit_" #m_unit "_combine_func", "get_texture_unit_" #m_unit "_combine_func"); \
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_unit_" #m_unit "_texgen_mode", PROPERTY_HINT_ENUM, "None,Sphere Map,Reflection Map,Object Linear,Eye Linear"), "set_texture_unit_" #m_unit "_texgen_mode", "get_texture_unit_" #m_unit "_texgen_mode");

void FixedFunctionMaterial::_bind_methods() {
	FF_BIND_TEXTURE_UNIT(0)
	FF_BIND_TEXTURE_UNIT(1)
	FF_BIND_TEXTURE_UNIT(2)
	FF_BIND_TEXTURE_UNIT(3)
#undef FF_BIND_TEXTURE_UNIT

	ClassDB::bind_method(D_METHOD("set_unshaded", "unshaded"), &FixedFunctionMaterial::set_unshaded);
	ClassDB::bind_method(D_METHOD("get_unshaded"), &FixedFunctionMaterial::get_unshaded);
	ClassDB::bind_method(D_METHOD("set_depth_test_disabled", "disabled"), &FixedFunctionMaterial::set_depth_test_disabled);
	ClassDB::bind_method(D_METHOD("get_depth_test_disabled"), &FixedFunctionMaterial::get_depth_test_disabled);
	ClassDB::bind_method(D_METHOD("set_cull_mode", "mode"), &FixedFunctionMaterial::set_cull_mode);
	ClassDB::bind_method(D_METHOD("get_cull_mode"), &FixedFunctionMaterial::get_cull_mode);
	ClassDB::bind_method(D_METHOD("set_blend_mode", "mode"), &FixedFunctionMaterial::set_blend_mode);
	ClassDB::bind_method(D_METHOD("get_blend_mode"), &FixedFunctionMaterial::get_blend_mode);

	ADD_GROUP("Render State", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "unshaded"), "set_unshaded", "get_unshaded");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "depth_test_disabled"), "set_depth_test_disabled", "get_depth_test_disabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mode", PROPERTY_HINT_ENUM, "Back,Front,Disabled"), "set_cull_mode", "get_cull_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blend_mode", PROPERTY_HINT_ENUM, "Mix,Add,Sub,Mul"), "set_blend_mode", "get_blend_mode");

	BIND_ENUM_CONSTANT(ENV_MODULATE);
	BIND_ENUM_CONSTANT(ENV_REPLACE);
	BIND_ENUM_CONSTANT(ENV_DECAL);
	BIND_ENUM_CONSTANT(ENV_BLEND);
	BIND_ENUM_CONSTANT(ENV_COMBINE);

	BIND_ENUM_CONSTANT(COMBINE_MODULATE);
	BIND_ENUM_CONSTANT(COMBINE_ADD);
	BIND_ENUM_CONSTANT(COMBINE_SUBTRACT);
	BIND_ENUM_CONSTANT(COMBINE_DOT3);

	BIND_ENUM_CONSTANT(TEXGEN_NONE);
	BIND_ENUM_CONSTANT(TEXGEN_SPHERE_MAP);
	BIND_ENUM_CONSTANT(TEXGEN_REFLECTION_MAP);
	BIND_ENUM_CONSTANT(TEXGEN_OBJECT_LINEAR);
	BIND_ENUM_CONSTANT(TEXGEN_EYE_LINEAR);

	BIND_ENUM_CONSTANT(CULL_BACK);
	BIND_ENUM_CONSTANT(CULL_FRONT);
	BIND_ENUM_CONSTANT(CULL_DISABLED);

	BIND_ENUM_CONSTANT(BLEND_MIX);
	BIND_ENUM_CONSTANT(BLEND_ADD);
	BIND_ENUM_CONSTANT(BLEND_SUB);
	BIND_ENUM_CONSTANT(BLEND_MUL);
}

int FixedFunctionMaterial::_get_target_gpu_tier() {
	if (!ProjectSettings::get_singleton()) {
		return TARGET_GPU_RV250_AND_NEWER;
	}
	return (int)ProjectSettings::get_singleton()->get("rendering/quality/gl_fixed_function/target_gpu");
}

int FixedFunctionMaterial::_get_tier_unit_count(int p_tier) {
	switch (p_tier) {
		case TARGET_GPU_RAGE_PRO:
			return 1;
		case TARGET_GPU_RAGE_128:
			return 2;
		case TARGET_GPU_RV250_AND_NEWER:
		default:
			return TEXTURE_UNIT_MAX;
	}
}

bool FixedFunctionMaterial::_get_tier_has_dot3(int p_tier) {
	// Confirmed absent on both Rage Pro and Rage 128 (godot-ports#25's
	// hardware research); present on RV250/GeForce2 MX and newer.
	return p_tier == TARGET_GPU_RV250_AND_NEWER;
}

bool FixedFunctionMaterial::_get_tier_has_reflection_map(int p_tier) {
	// Not verified present on Rage Pro/128 by this project's research
	// (godot-ports#25/#32 focused on RV250+/NV1x); gated to the tier
	// that's actually been confirmed, same conservative posture as Dot3.
	return p_tier == TARGET_GPU_RV250_AND_NEWER;
}

void FixedFunctionMaterial::_validate_property(PropertyInfo &property) const {
	Material::_validate_property(property);

	String name = property.name;
	if (!name.begins_with("texture_unit_")) {
		return;
	}

	int tier = _get_target_gpu_tier();
	int unit_count = _get_tier_unit_count(tier);

	// "texture_unit_<N>_..." -- pull N back out to compare against this
	// tier's real unit count.
	String rest = name.substr(String("texture_unit_").length(), name.length());
	int underscore = rest.find("_");
	String unit_str = underscore >= 0 ? rest.substr(0, underscore) : rest;
	if (!unit_str.is_valid_integer()) {
		return;
	}
	int unit = unit_str.to_int();

	if (unit >= unit_count) {
		// This tier doesn't have this many texture units at all -- hide
		// the whole property rather than leave a control that would look
		// functional but never do anything on the declared target
		// hardware.
		property.usage = PROPERTY_USAGE_NOEDITOR;
		return;
	}

	if (name.ends_with("_combine_func") && !_get_tier_has_dot3(tier)) {
		property.hint_string = "Modulate,Add,Subtract";
	} else if (name.ends_with("_texgen_mode") && !_get_tier_has_reflection_map(tier)) {
		property.hint_string = "None,Sphere Map,Object Linear,Eye Linear";
	}
}

FixedFunctionMaterial::FixedFunctionMaterial() {
	unshaded = false;
	depth_test_disabled = false;
	cull_mode = CULL_BACK;
	blend_mode = BLEND_MIX;

	for (int i = 0; i < TEXTURE_UNIT_MAX; i++) {
		texture_unit_env_mode[i] = ENV_MODULATE;
		texture_unit_combine_func[i] = COMBINE_MODULATE;
		texture_unit_texgen_mode[i] = TEXGEN_NONE;
	}

	// Push initial defaults so the VisualServer-side material always has a
	// fully-defined fixed-function state, even before any property is
	// explicitly touched -- mirrors the pattern of every other GLFF
	// material param, which is only ever *set* here, never queried back.
	for (int i = 0; i < TEXTURE_UNIT_MAX; i++) {
		_update_material_param(StringName("ff_tex" + itos(i)), RID());
		_update_material_param(StringName("ff_env_mode" + itos(i)), (int)ENV_MODULATE);
		_update_material_param(StringName("ff_combine_func" + itos(i)), (int)COMBINE_MODULATE);
		_update_material_param(StringName("ff_texgen_mode" + itos(i)), (int)TEXGEN_NONE);
	}
	_update_material_param("ff_unshaded", false);
	_update_material_param("ff_depth_test_disabled", false);
	_update_material_param("ff_cull_mode", (int)CULL_BACK);
	_update_material_param("ff_blend_mode", (int)BLEND_MIX);
	// Marker param GLFF's material_set_param() uses to flag this material
	// as fixed-function-driven (not SpatialMaterial-style albedo/albedo_texture)
	// -- see the RasterizerStorageGLFF::Material comment (godot-ports#35).
	_update_material_param("ff_active", true);
}

FixedFunctionMaterial::~FixedFunctionMaterial() {
}
