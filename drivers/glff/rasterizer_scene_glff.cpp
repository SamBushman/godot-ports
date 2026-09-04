#include "rasterizer_scene_glff.h"

#include "rasterizer_storage_glff.h"
#include <stdio.h>

void RasterizerSceneGLFF::render_scene(const Transform &p_cam_transform, const CameraMatrix &p_cam_projection, const int p_eye, bool p_cam_ortogonal, InstanceBase **p_cull_result, int p_cull_count, RID *p_light_cull_result, int p_light_cull_count, RID *p_reflection_probe_cull_result, int p_reflection_probe_cull_count, RID p_environment, RID p_shadow_atlas, RID p_reflection_atlas, RID p_reflection_probe, int p_reflection_probe_pass) {
	fprintf(stderr, "GLFF DEBUG: render_scene enter\n");
	fflush(stderr);
	Color bg_color(0, 0, 0, 1);
	if (p_environment.is_valid()) {
		Environment *env = environment_owner.getornull(p_environment);
		if (env) {
			bg_color = env->bg_color;
		}
	}

	glClearColor(bg_color.r, bg_color.g, bg_color.b, 1.0);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Phase 1 milestone (godot-ports#14 proposal, Phase 1): prove the whole
	// driver-selection -> context -> frame-loop -> visible-pixels chain
	// works, independent of whether real scene content/cameras/materials
	// exist yet -- those come in Phase 3. Deliberately not using
	// p_cam_transform/p_cam_projection here: a fixed identity ortho view
	// guarantees this triangle is visible on screen regardless of what's
	// actually in the scene, decoupling "does the pipeline work" from
	// "is the camera math right" (a real concern, but a Phase 3 one).
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glDisable(GL_LIGHTING);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	static const GLfloat verts[9] = {
		0.0f, 0.5f, 0.0f,
		-0.5f, -0.5f, 0.0f,
		0.5f, -0.5f, 0.0f
	};
	static const GLfloat colors[9] = {
		1.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f,
		0.0f, 0.0f, 1.0f
	};

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, verts);
	glColorPointer(3, GL_FLOAT, 0, colors);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	fprintf(stderr, "GLFF DEBUG: render_scene exit\n");
	fflush(stderr);
}

bool RasterizerSceneGLFF::free(RID p_rid) {
	if (shadow_atlas_owner.owns(p_rid)) {
		memdelete(shadow_atlas_owner.getornull(p_rid));
		shadow_atlas_owner.free(p_rid);
		return true;
	} else if (environment_owner.owns(p_rid)) {
		memdelete(environment_owner.getornull(p_rid));
		environment_owner.free(p_rid);
		return true;
	} else if (light_instance_owner.owns(p_rid)) {
		memdelete(light_instance_owner.getornull(p_rid));
		light_instance_owner.free(p_rid);
		return true;
	} else if (reflection_atlas_owner.owns(p_rid)) {
		memdelete(reflection_atlas_owner.getornull(p_rid));
		reflection_atlas_owner.free(p_rid);
		return true;
	} else if (reflection_probe_instance_owner.owns(p_rid)) {
		memdelete(reflection_probe_instance_owner.getornull(p_rid));
		reflection_probe_instance_owner.free(p_rid);
		return true;
	} else if (gi_probe_instance_owner.owns(p_rid)) {
		memdelete(gi_probe_instance_owner.getornull(p_rid));
		gi_probe_instance_owner.free(p_rid);
		return true;
	}
	return false;
}

void RasterizerSceneGLFF::initialize() {
}

RasterizerSceneGLFF::RasterizerSceneGLFF() {
	storage = nullptr;
}

RasterizerSceneGLFF::~RasterizerSceneGLFF() {
}
