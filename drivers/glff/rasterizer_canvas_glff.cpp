#include "rasterizer_canvas_glff.h"

#include "rasterizer_storage_glff.h"

void RasterizerCanvasGLFF::canvas_begin() {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void RasterizerCanvasGLFF::canvas_end() {
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

void RasterizerCanvasGLFF::reset_canvas() {
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_BLEND);
}

void RasterizerCanvasGLFF::initialize() {
}

RasterizerCanvasGLFF::RasterizerCanvasGLFF() {
	storage = nullptr;
	scene_render = nullptr;
}

RasterizerCanvasGLFF::~RasterizerCanvasGLFF() {
}
