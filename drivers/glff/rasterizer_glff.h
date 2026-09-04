#ifndef RASTERIZER_GLFF_H
#define RASTERIZER_GLFF_H

#include "rasterizer_canvas_glff.h"
#include "rasterizer_scene_glff.h"
#include "rasterizer_storage_glff.h"
#include "servers/visual/rasterizer.h"

// Top-level frame driver for GLFF, the OpenGL 1.2 fixed-function backend
// (godot-ports#14). Structurally mirrors RasterizerGLES2
// (drivers/gles2/rasterizer_gles2.h) -- see that file for the established
// pattern this one follows.
class RasterizerGLFF : public Rasterizer {
	static Rasterizer *_create_current();

	RasterizerStorageGLFF *storage;
	RasterizerCanvasGLFF *canvas;
	RasterizerSceneGLFF *scene;

public:
	virtual RasterizerStorage *get_storage();
	virtual RasterizerCanvas *get_canvas();
	virtual RasterizerScene *get_scene();

	virtual void set_boot_image(const Ref<Image> &p_image, const Color &p_color, bool p_scale, bool p_use_filter = true);
	virtual void set_shader_time_scale(float p_scale) {}

	virtual void initialize();
	virtual void begin_frame(double frame_step);
	virtual void set_current_render_target(RID p_render_target);
	virtual void restore_render_target(bool p_3d_was_drawn);
	virtual void clear_render_target(const Color &p_color);
	virtual void blit_render_target_to_screen(RID p_render_target, const Rect2 &p_screen_rect, int p_screen = 0);
	virtual void output_lens_distorted_to_screen(RID p_render_target, const Rect2 &p_screen_rect, float p_k1, float p_k2, const Vector2 &p_eye_center, float p_oversample) {}
	virtual void end_frame(bool p_swap_buffers);
	virtual void finalize();

	static Error is_viable();
	static void make_current();
	static void register_config();

	virtual bool is_low_end() const { return true; }

	RasterizerGLFF();
	~RasterizerGLFF();
};

#endif // RASTERIZER_GLFF_H
