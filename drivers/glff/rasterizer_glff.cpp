#include "rasterizer_glff.h"

#include "core/os/os.h"
#include <stdio.h>

RasterizerStorage *RasterizerGLFF::get_storage() {
	return storage;
}

RasterizerCanvas *RasterizerGLFF::get_canvas() {
	return canvas;
}

RasterizerScene *RasterizerGLFF::get_scene() {
	return scene;
}

Error RasterizerGLFF::is_viable() {
	// No GLAD/extension loader here at all (unlike GLES2) -- GLFF only
	// ever calls core OpenGL 1.2 entry points (proposal §2's strict
	// floor), which every GL context provides by construction. This is a
	// sanity check, not a capability probe.
	const char *version_str = (const char *)glGetString(GL_VERSION);
	if (!version_str) {
		return ERR_UNAVAILABLE;
	}

	int major = 0, minor = 0;
	if (sscanf(version_str, "%d.%d", &major, &minor) != 2) {
		return ERR_UNAVAILABLE;
	}

	if (major < 1 || (major == 1 && minor < 2)) {
		return ERR_UNAVAILABLE;
	}

	return OK;
}

void RasterizerGLFF::initialize() {
	print_line("OpenGL Renderer (GLFF, fixed-function): " + VisualServer::get_singleton()->get_video_adapter_name());
	storage->initialize();
	canvas->initialize();
	scene->initialize();
}

void RasterizerGLFF::begin_frame(double frame_step) {
	// No per-frame dirty-resource/time bookkeeping needed yet -- Phase 1's
	// milestone (clear + one hardcoded triangle) doesn't depend on it.
	// Revisit if a later phase needs frame-relative shader-free effects
	// (e.g. time-based UV animation) that would read this.
}

void RasterizerGLFF::set_current_render_target(RID p_render_target) {
	// GLFF has no FBO (proposal §2) -- the default framebuffer is always
	// bound, so every render target actually draws straight into the real
	// window backbuffer at glViewport(0,0,width,height). Before switching
	// to a DIFFERENT target (or back to the main screen) and overwriting
	// that same physical region, capture whatever the PREVIOUS target just
	// drew into its own real texture via glCopyTexSubImage2D -- this is
	// what makes SubViewport-as-texture (ViewportContainer, the editor's
	// own 3D panel, camera previews, minimaps) work at all; see the
	// RenderTarget comment in rasterizer_storage_glff.h for the full
	// reasoning (godot-ports#28).
	if (current_render_target.is_valid()) {
		fprintf(stderr, "GLFF DEBUG: capturing prev target %s\n", current_render_target.get_id() != 0 ? "valid" : "invalid");
		fflush(stderr);
		storage->render_target_copy_to_texture(current_render_target);
	}
	current_render_target = p_render_target;

	if (p_render_target.is_valid()) {
		RasterizerStorageGLFF::RenderTarget *rt = storage->render_target_owner.getornull(p_render_target);
		fprintf(stderr, "GLFF DEBUG: set_current_render_target valid rid, rt=%p w=%d h=%d\n", (void *)rt, rt ? rt->width : -1, rt ? rt->height : -1);
		fflush(stderr);
		if (rt) {
			glViewport(0, 0, rt->width, rt->height);
		}
	} else {
		fprintf(stderr, "GLFF DEBUG: set_current_render_target invalid rid (screen)\n");
		fflush(stderr);
		Size2 window_size = OS::get_singleton()->get_window_size();
		glViewport(0, 0, window_size.width, window_size.height);
	}
}

void RasterizerGLFF::restore_render_target(bool p_3d_was_drawn) {
	Size2 window_size = OS::get_singleton()->get_window_size();
	glViewport(0, 0, window_size.width, window_size.height);
}

void RasterizerGLFF::clear_render_target(const Color &p_color) {
	glClearColor(p_color.r, p_color.g, p_color.b, p_color.a);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void RasterizerGLFF::blit_render_target_to_screen(RID p_render_target, const Rect2 &p_screen_rect, int p_screen) {
	// Not implemented for Phase 1 -- the main viewport never reaches this
	// (render_direct_to_screen is forced on for GLFF, see
	// scene_tree.cpp/visual_server_viewport.cpp:370), and SubViewport-as-
	// screen-attached-texture is deferred future work, same as the
	// SubViewport-as-sampled-texture case in set_current_render_target.
}

void RasterizerGLFF::set_boot_image(const Ref<Image> &p_image, const Color &p_color, bool p_scale, bool p_use_filter) {
	Size2 window_size = OS::get_singleton()->get_window_size();
	glViewport(0, 0, window_size.width, window_size.height);
	glClearColor(p_color.r, p_color.g, p_color.b, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	// The boot image itself isn't drawn -- doing so needs the textured-quad
	// path that's Phase 2's job (RasterizerCanvas 2D rendering, #16). The
	// boot *background color* still shows, which is enough for Phase 1.
	end_frame(true);
}

void RasterizerGLFF::end_frame(bool p_swap_buffers) {
	if (p_swap_buffers) {
		OS::get_singleton()->swap_buffers();
	} else {
		glFinish();
	}
}

void RasterizerGLFF::finalize() {
	storage->finalize();
}

Rasterizer *RasterizerGLFF::_create_current() {
	return memnew(RasterizerGLFF);
}

void RasterizerGLFF::make_current() {
	_create_func = _create_current;
}

void RasterizerGLFF::register_config() {
}

RasterizerGLFF::RasterizerGLFF() {
	storage = memnew(RasterizerStorageGLFF);
	canvas = memnew(RasterizerCanvasGLFF);
	scene = memnew(RasterizerSceneGLFF);
	canvas->storage = storage;
	canvas->scene_render = scene;
	storage->canvas = canvas;
	scene->storage = storage;
}

RasterizerGLFF::~RasterizerGLFF() {
	memdelete(scene);
	memdelete(canvas);
	memdelete(storage);
}
