#ifndef RASTERIZER_CANVAS_GLFF_H
#define RASTERIZER_CANVAS_GLFF_H

#include "core/rid.h"
#include "servers/visual/rasterizer.h"

// 2D canvas compositor for the GLFF (OpenGL 1.2 fixed-function) driver.
// Phase 1 scope only (godot-ports#14 proposal): compiles and sets up basic
// per-frame GL state, but canvas_render_items() is a stub -- real 2D
// rendering (adapting drivers/gles_common/rasterizer_canvas_batcher.h via
// its CRTP extension points, multi-pass lightmap-independent since that's
// a 3D concern, per #16's resolved design) is Phase 2's job.

class RasterizerStorageGLFF;
class RasterizerSceneGLFF;

class RasterizerCanvasGLFF : public RasterizerCanvas {
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
	virtual void canvas_render_items(Item *p_item_list, int p_z, const Color &p_modulate, Light *p_light, const Transform2D &p_base_transform) {}
	virtual void canvas_debug_viewport_shadows(Light *p_lights_with_shadow) {}
	virtual void canvas_light_shadow_buffer_update(RID p_buffer, const Transform2D &p_light_xform, int p_light_mask, float p_near, float p_far, LightOccluderInstance *p_occluders, CameraMatrix *p_xform_cache) {}
	virtual void reset_canvas();
	virtual void draw_window_margins(int *p_margins, RID *p_margin_textures) {}

	void initialize();

	RasterizerCanvasGLFF();
	~RasterizerCanvasGLFF();
};

#endif // RASTERIZER_CANVAS_GLFF_H
