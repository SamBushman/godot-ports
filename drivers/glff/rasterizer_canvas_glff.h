#ifndef RASTERIZER_CANVAS_GLFF_H
#define RASTERIZER_CANVAS_GLFF_H

#include "core/rid.h"
#include "drivers/gles_common/rasterizer_canvas_batcher.h"
#include "servers/visual/rasterizer.h"

class RasterizerStorageGLFF;
class RasterizerSceneGLFF;

// 2D canvas compositor for the GLFF (OpenGL 1.2 fixed-function) driver.
// Phase 2 (godot-ports#14 proposal, #16's resolved design): batching is
// deliberately left OFF for this first pass (bdata.settings_use_batching
// forced false in initialize()) -- every CanvasItem is drawn individually
// via canvas_render_items_implementation() below, which is real,
// correctness-first fixed-function rendering, not a stub. The shared
// drivers/gles_common/rasterizer_canvas_batcher.h batching fast-path
// (render_batches() as actually called from the batcher's own joined-item
// flow, try_join_item(), _batch_upload_buffers()) is unreachable with
// batching off, so those overrides exist only to satisfy the CRTP
// interface and are trivial by construction, not because the feature is
// unimplemented -- revisit once correctness is established and batching
// is worth turning back on for performance.
//
// Command types actually drawn: RECT, LINE, POLYLINE, POLYGON, CIRCLE,
// MULTIRECT, TRANSFORM, CLIP_IGNORE. NINEPATCH draws as a plain stretched
// rect for now (ignores 9-slice margins -- a real simplification, not a
// crash). MESH, MULTIMESH, and PARTICLES commands are silently skipped
// (2D mesh/multimesh instancing and GPU/CPU particles-in-2D are rare
// enough content patterns to defer past this first pass). Custom
// CanvasItemMaterial/ShaderMaterial content is ignored -- everything
// draws with plain texture-modulate, per the proposal's material-mapping
// scope (godot-ports#17 covers 3D SpatialMaterial; an equivalent 2D pass
// hasn't been scoped yet).
class RasterizerCanvasGLFF : public RasterizerCanvas, public RasterizerCanvasBatcher<RasterizerCanvasGLFF, RasterizerStorageGLFF> {
public:
	RasterizerStorageGLFF *storage;
	RasterizerSceneGLFF *scene_render;

	struct LightInternal : public RID_Data {
		Light *light = nullptr;
	};
	mutable RID_Owner<LightInternal> light_internal_owner;

	virtual RID light_internal_create() { return light_internal_owner.make_rid(memnew(LightInternal)); }
	virtual void light_internal_update(RID p_rid, Light *p_light) {
		LightInternal *li = light_internal_owner.getornull(p_rid);
		ERR_FAIL_COND(!li);
		li->light = p_light;
	}
	virtual void light_internal_free(RID p_rid) {
		LightInternal *li = light_internal_owner.getornull(p_rid);
		if (li) {
			light_internal_owner.free(p_rid);
			memdelete(li);
		}
	}

	virtual void canvas_begin();
	virtual void canvas_end();
	virtual void canvas_render_items(Item *p_item_list, int p_z, const Color &p_modulate, Light *p_light, const Transform2D &p_base_transform);
	virtual void canvas_debug_viewport_shadows(Light *p_lights_with_shadow) {}
	virtual void canvas_light_shadow_buffer_update(RID p_buffer, const Transform2D &p_light_xform, int p_light_mask, float p_near, float p_far, LightOccluderInstance *p_occluders, CameraMatrix *p_xform_cache) {}
	virtual void reset_canvas();
	virtual void draw_window_margins(int *p_margins, RID *p_margin_textures) {}

	// CRTP callbacks the shared batcher requires to exist. Real content
	// lives in render_batches()'s BT_DEFAULT case (see .cpp) -- the others
	// are unreachable with batching off (see class comment above).
	void canvas_render_items_implementation(Item *p_item_list, int p_z, const Color &p_modulate, Light *p_light, const Transform2D &p_base_transform);
	void render_batches(Item *p_current_clip, bool &r_reclip, RasterizerStorageGLFF::Material *p_material);
	bool try_join_item(Item *p_ci, RenderItemState &r_ris, bool &r_batch_break) { return false; }
	void _batch_upload_buffers() {}
	void gl_enable_scissor(int p_x, int p_y, int p_width, int p_height) const;
	void gl_disable_scissor() const;

	void initialize();

	RasterizerCanvasGLFF();
	~RasterizerCanvasGLFF();
};

#endif // RASTERIZER_CANVAS_GLFF_H
