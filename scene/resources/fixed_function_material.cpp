/**************************************************************************/
/*  fixed_function_material.cpp                                          */
/**************************************************************************/

#include "fixed_function_material.h"

void FixedFunctionMaterial::_update_material_param(const StringName &p_param, const Variant &p_value) {
	VS::get_singleton()->material_set_param(get_rid(), p_param, p_value);
}

void FixedFunctionMaterial::set_texture_unit_texture(int p_unit, const Ref<Texture> &p_texture) {
	ERR_FAIL_INDEX(p_unit, TEXTURE_UNIT_MAX);
	texture_unit_texture[p_unit] = p_texture;
	RID rid = p_texture.is_valid() ? p_texture->get_rid() : RID();
	_update_material_param(p_unit == 0 ? StringName("ff_tex0") : StringName("ff_tex1"), rid);
	_change_notify();
}

Ref<Texture> FixedFunctionMaterial::get_texture_unit_texture(int p_unit) const {
	ERR_FAIL_INDEX_V(p_unit, TEXTURE_UNIT_MAX, Ref<Texture>());
	return texture_unit_texture[p_unit];
}

void FixedFunctionMaterial::set_texture_unit_env_mode(int p_unit, EnvMode p_mode) {
	ERR_FAIL_INDEX(p_unit, TEXTURE_UNIT_MAX);
	texture_unit_env_mode[p_unit] = p_mode;
	_update_material_param(p_unit == 0 ? StringName("ff_env_mode0") : StringName("ff_env_mode1"), (int)p_mode);
	_change_notify();
}

FixedFunctionMaterial::EnvMode FixedFunctionMaterial::get_texture_unit_env_mode(int p_unit) const {
	ERR_FAIL_INDEX_V(p_unit, TEXTURE_UNIT_MAX, ENV_MODULATE);
	return texture_unit_env_mode[p_unit];
}

void FixedFunctionMaterial::set_texture_unit_combine_func(int p_unit, CombineFunc p_func) {
	ERR_FAIL_INDEX(p_unit, TEXTURE_UNIT_MAX);
	texture_unit_combine_func[p_unit] = p_func;
	_update_material_param(p_unit == 0 ? StringName("ff_combine_func0") : StringName("ff_combine_func1"), (int)p_func);
	_change_notify();
}

FixedFunctionMaterial::CombineFunc FixedFunctionMaterial::get_texture_unit_combine_func(int p_unit) const {
	ERR_FAIL_INDEX_V(p_unit, TEXTURE_UNIT_MAX, COMBINE_MODULATE);
	return texture_unit_combine_func[p_unit];
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

void FixedFunctionMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_texture_unit_0_texture", "texture"), &FixedFunctionMaterial::set_texture_unit_0_texture);
	ClassDB::bind_method(D_METHOD("get_texture_unit_0_texture"), &FixedFunctionMaterial::get_texture_unit_0_texture);
	ClassDB::bind_method(D_METHOD("set_texture_unit_0_env_mode", "mode"), &FixedFunctionMaterial::set_texture_unit_0_env_mode);
	ClassDB::bind_method(D_METHOD("get_texture_unit_0_env_mode"), &FixedFunctionMaterial::get_texture_unit_0_env_mode);
	ClassDB::bind_method(D_METHOD("set_texture_unit_0_combine_func", "func"), &FixedFunctionMaterial::set_texture_unit_0_combine_func);
	ClassDB::bind_method(D_METHOD("get_texture_unit_0_combine_func"), &FixedFunctionMaterial::get_texture_unit_0_combine_func);

	ClassDB::bind_method(D_METHOD("set_texture_unit_1_texture", "texture"), &FixedFunctionMaterial::set_texture_unit_1_texture);
	ClassDB::bind_method(D_METHOD("get_texture_unit_1_texture"), &FixedFunctionMaterial::get_texture_unit_1_texture);
	ClassDB::bind_method(D_METHOD("set_texture_unit_1_env_mode", "mode"), &FixedFunctionMaterial::set_texture_unit_1_env_mode);
	ClassDB::bind_method(D_METHOD("get_texture_unit_1_env_mode"), &FixedFunctionMaterial::get_texture_unit_1_env_mode);
	ClassDB::bind_method(D_METHOD("set_texture_unit_1_combine_func", "func"), &FixedFunctionMaterial::set_texture_unit_1_combine_func);
	ClassDB::bind_method(D_METHOD("get_texture_unit_1_combine_func"), &FixedFunctionMaterial::get_texture_unit_1_combine_func);

	ClassDB::bind_method(D_METHOD("set_unshaded", "unshaded"), &FixedFunctionMaterial::set_unshaded);
	ClassDB::bind_method(D_METHOD("get_unshaded"), &FixedFunctionMaterial::get_unshaded);
	ClassDB::bind_method(D_METHOD("set_depth_test_disabled", "disabled"), &FixedFunctionMaterial::set_depth_test_disabled);
	ClassDB::bind_method(D_METHOD("get_depth_test_disabled"), &FixedFunctionMaterial::get_depth_test_disabled);
	ClassDB::bind_method(D_METHOD("set_cull_mode", "mode"), &FixedFunctionMaterial::set_cull_mode);
	ClassDB::bind_method(D_METHOD("get_cull_mode"), &FixedFunctionMaterial::get_cull_mode);
	ClassDB::bind_method(D_METHOD("set_blend_mode", "mode"), &FixedFunctionMaterial::set_blend_mode);
	ClassDB::bind_method(D_METHOD("get_blend_mode"), &FixedFunctionMaterial::get_blend_mode);

	ADD_GROUP("Texture Unit 0", "texture_unit_0_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture_unit_0_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture"), "set_texture_unit_0_texture", "get_texture_unit_0_texture");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_unit_0_env_mode", PROPERTY_HINT_ENUM, "Modulate,Replace,Decal,Blend,Combine"), "set_texture_unit_0_env_mode", "get_texture_unit_0_env_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_unit_0_combine_func", PROPERTY_HINT_ENUM, "Modulate,Add,Subtract,Dot3"), "set_texture_unit_0_combine_func", "get_texture_unit_0_combine_func");

	ADD_GROUP("Texture Unit 1", "texture_unit_1_");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture_unit_1_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture"), "set_texture_unit_1_texture", "get_texture_unit_1_texture");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_unit_1_env_mode", PROPERTY_HINT_ENUM, "Modulate,Replace,Decal,Blend,Combine"), "set_texture_unit_1_env_mode", "get_texture_unit_1_env_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_unit_1_combine_func", PROPERTY_HINT_ENUM, "Modulate,Add,Subtract,Dot3"), "set_texture_unit_1_combine_func", "get_texture_unit_1_combine_func");

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

	BIND_ENUM_CONSTANT(CULL_BACK);
	BIND_ENUM_CONSTANT(CULL_FRONT);
	BIND_ENUM_CONSTANT(CULL_DISABLED);

	BIND_ENUM_CONSTANT(BLEND_MIX);
	BIND_ENUM_CONSTANT(BLEND_ADD);
	BIND_ENUM_CONSTANT(BLEND_SUB);
	BIND_ENUM_CONSTANT(BLEND_MUL);
}

FixedFunctionMaterial::FixedFunctionMaterial() {
	unshaded = false;
	depth_test_disabled = false;
	cull_mode = CULL_BACK;
	blend_mode = BLEND_MIX;

	for (int i = 0; i < TEXTURE_UNIT_MAX; i++) {
		texture_unit_env_mode[i] = ENV_MODULATE;
		texture_unit_combine_func[i] = COMBINE_MODULATE;
	}

	// Push initial defaults so the VisualServer-side material always has a
	// fully-defined fixed-function state, even before any property is
	// explicitly touched -- mirrors the pattern of every other GLFF
	// material param, which is only ever *set* here, never queried back.
	_update_material_param("ff_tex0", RID());
	_update_material_param("ff_env_mode0", (int)ENV_MODULATE);
	_update_material_param("ff_combine_func0", (int)COMBINE_MODULATE);
	_update_material_param("ff_tex1", RID());
	_update_material_param("ff_env_mode1", (int)ENV_MODULATE);
	_update_material_param("ff_combine_func1", (int)COMBINE_MODULATE);
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
