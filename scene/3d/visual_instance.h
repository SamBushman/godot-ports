/**************************************************************************/
/*  visual_instance.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#ifndef VISUAL_INSTANCE_H
#define VISUAL_INSTANCE_H

#include "core/math/face3.h"
#include "core/rid.h"
#include "scene/3d/cull_instance.h"
#include "scene/resources/material.h"

class Mesh;

class VisualInstance : public CullInstance {
	GDCLASS(VisualInstance, CullInstance);
	OBJ_CATEGORY("3D Visual Nodes");

	RID base;
	RID instance;
	uint32_t layers;
	float sorting_offset;
	bool sorting_use_aabb_center;

	RID _get_visual_instance_rid() const;

protected:
	void _update_visibility();
	virtual void _refresh_portal_mode();
	virtual void _physics_interpolated_changed();
	void set_instance_use_identity_transform(bool p_enable);

	void _notification(int p_what);
	static void _bind_methods();

public:
	enum GetFacesFlags {
		FACES_SOLID = 1, // solid geometry
		FACES_ENCLOSING = 2,
		FACES_DYNAMIC = 4 // dynamic object geometry
	};

	RID get_instance() const;
	virtual AABB get_aabb() const = 0;
	virtual PoolVector<Face3> get_faces(uint32_t p_usage_flags) const = 0;

	virtual AABB get_transformed_aabb() const; // helper

	void set_base(const RID &p_base);
	RID get_base() const;

	void set_layer_mask(uint32_t p_mask);
	uint32_t get_layer_mask() const;

	void set_layer_mask_bit(int p_layer, bool p_enable);
	bool get_layer_mask_bit(int p_layer) const;

	void set_sorting_offset(float p_offset);
	float get_sorting_offset();

	void set_sorting_use_aabb_center(bool p_enabled);
	bool is_sorting_use_aabb_center();

	VisualInstance();
	~VisualInstance();
};

class GeometryInstance : public VisualInstance {
	GDCLASS(GeometryInstance, VisualInstance);

public:
	enum Flags {
		FLAG_USE_BAKED_LIGHT = VS::INSTANCE_FLAG_USE_BAKED_LIGHT,
		FLAG_DRAW_NEXT_FRAME_IF_VISIBLE = VS::INSTANCE_FLAG_DRAW_NEXT_FRAME_IF_VISIBLE,
		FLAG_MAX = VS::INSTANCE_FLAG_MAX,
	};

	enum LightmapScale {
		LIGHTMAP_SCALE_1X,
		LIGHTMAP_SCALE_2X,
		LIGHTMAP_SCALE_4X,
		LIGHTMAP_SCALE_8X,
		LIGHTMAP_SCALE_MAX,
	};

	enum ShadowCastingSetting {
		SHADOW_CASTING_SETTING_OFF = VS::SHADOW_CASTING_SETTING_OFF,
		SHADOW_CASTING_SETTING_ON = VS::SHADOW_CASTING_SETTING_ON,
		SHADOW_CASTING_SETTING_DOUBLE_SIDED = VS::SHADOW_CASTING_SETTING_DOUBLE_SIDED,
		SHADOW_CASTING_SETTING_SHADOWS_ONLY = VS::SHADOW_CASTING_SETTING_SHADOWS_ONLY
	};

	// godot-ports#54: GLFF shadow-volume optimization strategy, three
	// independent composable axes -- see VisualServer's own enum comments
	// (servers/visual_server.h) for the full per-technique rationale
	// (godot-ports#49-53). Default (0) on every axis is today's exact
	// existing behavior; only the GLFF backend reads these at all.
	enum ShadowGeometrySource {
		SHADOW_GEOMETRY_SOURCE_RENDER_MESH = VS::SHADOW_GEOMETRY_SOURCE_RENDER_MESH,
		SHADOW_GEOMETRY_SOURCE_LOD_PROXY = VS::SHADOW_GEOMETRY_SOURCE_LOD_PROXY,
		SHADOW_GEOMETRY_SOURCE_BILLBOARD_DISC = VS::SHADOW_GEOMETRY_SOURCE_BILLBOARD_DISC,
	};

	enum ShadowSilhouetteAlgorithm {
		SHADOW_SILHOUETTE_ALGORITHM_FULL = VS::SHADOW_SILHOUETTE_ALGORITHM_FULL,
		SHADOW_SILHOUETTE_ALGORITHM_NORMAL_CONE = VS::SHADOW_SILHOUETTE_ALGORITHM_NORMAL_CONE,
		SHADOW_SILHOUETTE_ALGORITHM_RING_SEGMENT = VS::SHADOW_SILHOUETTE_ALGORITHM_RING_SEGMENT,
	};

	enum ShadowTemporalCache {
		SHADOW_TEMPORAL_CACHE_NONE = VS::SHADOW_TEMPORAL_CACHE_NONE,
		SHADOW_TEMPORAL_CACHE_DIRECTION_QUANTIZED = VS::SHADOW_TEMPORAL_CACHE_DIRECTION_QUANTIZED,
		SHADOW_TEMPORAL_CACHE_TEMPORAL_COHERENCE = VS::SHADOW_TEMPORAL_CACHE_TEMPORAL_COHERENCE,
	};

	// godot-ports#55: relight-pass culling controls -- only meaningful
	// under a light using SUBTRACTIVE relight (godot-ports#56; the
	// default, ADDITIVE, never reads these). GLFF-only, same as the #54
	// family above. Default (0) on every axis is today's exact existing
	// behavior.
	enum ShadowRelightInclusion {
		SHADOW_RELIGHT_INCLUSION_DYNAMIC = VS::SHADOW_RELIGHT_INCLUSION_DYNAMIC,
		SHADOW_RELIGHT_INCLUSION_ALWAYS = VS::SHADOW_RELIGHT_INCLUSION_ALWAYS,
		SHADOW_RELIGHT_INCLUSION_NEVER = VS::SHADOW_RELIGHT_INCLUSION_NEVER,
	};

private:
	bool flags[FLAG_MAX];
	bool generate_lightmap;
	LightmapScale lightmap_scale;
	ShadowCastingSetting shadow_casting_setting;
	ShadowGeometrySource shadow_geometry_source;
	ShadowSilhouetteAlgorithm shadow_silhouette_algorithm;
	ShadowTemporalCache shadow_temporal_cache;
	Ref<Mesh> shadow_lod_proxy_mesh;
	float shadow_billboard_disc_radius;
	Vector3 shadow_billboard_disc_offset;
	ShadowRelightInclusion shadow_relight_inclusion;
	bool shadow_relight_self;
	bool shadow_relight_aabb_enabled;
	AABB shadow_relight_aabb;
	Ref<Material> material_override;
	Ref<Material> material_overlay;

	float extra_cull_margin;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_flag(Flags p_flag, bool p_value);
	bool get_flag(Flags p_flag) const;

	void set_cast_shadows_setting(ShadowCastingSetting p_shadow_casting_setting);
	ShadowCastingSetting get_cast_shadows_setting() const;

	void set_shadow_geometry_source(ShadowGeometrySource p_source);
	ShadowGeometrySource get_shadow_geometry_source() const;

	void set_shadow_silhouette_algorithm(ShadowSilhouetteAlgorithm p_algorithm);
	ShadowSilhouetteAlgorithm get_shadow_silhouette_algorithm() const;

	void set_shadow_temporal_cache(ShadowTemporalCache p_cache);
	ShadowTemporalCache get_shadow_temporal_cache() const;

	void set_shadow_lod_proxy_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_shadow_lod_proxy_mesh() const;

	void set_shadow_billboard_disc_radius(float p_radius);
	float get_shadow_billboard_disc_radius() const;

	void set_shadow_billboard_disc_offset(const Vector3 &p_offset);
	Vector3 get_shadow_billboard_disc_offset() const;

	void set_shadow_relight_inclusion(ShadowRelightInclusion p_inclusion);
	ShadowRelightInclusion get_shadow_relight_inclusion() const;

	void set_shadow_relight_self(bool p_enabled);
	bool get_shadow_relight_self() const;

	void set_shadow_relight_aabb_enabled(bool p_enabled);
	bool get_shadow_relight_aabb_enabled() const;

	void set_shadow_relight_aabb(const AABB &p_aabb);
	AABB get_shadow_relight_aabb() const;

	void set_generate_lightmap(bool p_enabled);
	bool get_generate_lightmap() const;

	void set_lightmap_scale(LightmapScale p_scale);
	LightmapScale get_lightmap_scale() const;

	virtual void set_material_override(const Ref<Material> &p_material);
	Ref<Material> get_material_override() const;

	virtual void set_material_overlay(const Ref<Material> &p_material);
	Ref<Material> get_material_overlay() const;

	void set_extra_cull_margin(float p_margin);
	float get_extra_cull_margin() const;

	void set_custom_aabb(AABB aabb);

	GeometryInstance();
};

VARIANT_ENUM_CAST(GeometryInstance::Flags);
VARIANT_ENUM_CAST(GeometryInstance::LightmapScale);
VARIANT_ENUM_CAST(GeometryInstance::ShadowCastingSetting);
VARIANT_ENUM_CAST(GeometryInstance::ShadowGeometrySource);
VARIANT_ENUM_CAST(GeometryInstance::ShadowSilhouetteAlgorithm);
VARIANT_ENUM_CAST(GeometryInstance::ShadowTemporalCache);
VARIANT_ENUM_CAST(GeometryInstance::ShadowRelightInclusion);

#endif // VISUAL_INSTANCE_H
