#ifndef RASTERIZER_STORAGE_GLFF_H
#define RASTERIZER_STORAGE_GLFF_H

#include "core/rid.h"
#include "servers/visual/rasterizer.h"

// Same platform GL header indirection GLES2 already uses successfully on
// this hardware (drivers/gles2/shader_gles2.h) -- platform_config.h picks
// <OpenGL/gl.h> (via platform/osx/glfixes.h) on OSX. GLFF never needs any
// of glfixes.h's FBO/VAO APPLE/EXT renames (no FBO, no VBO/VAO under the
// strict-GL-1.2 floor, see proposal §2), just the base GL 1.2 entry points.
#include "platform_config.h"
#include GLES2_INCLUDE_H

// Storage backend for the GLFF (OpenGL 1.2 fixed-function) rendering
// driver. Only Texture and Mesh are real: everything else here exists
// purely so RasterizerStorage's interface (pure virtual) compiles and RID
// lifecycle (create/free/get_base_type) behaves correctly for node types
// this backend doesn't render -- see godot-ports#14's proposal doc, §8.1,
// for which of these are meant to grow real behavior in a later phase
// (materials in Phase 4, lights in Phase 3, etc.) versus permanently
// stay a no-op (particles, reflection probes, GI probes -- no fixed-function
// equivalent exists for any of those).

class RasterizerCanvasGLFF;
class RasterizerSceneGLFF;

class RasterizerStorageGLFF : public RasterizerStorage {
public:
	RasterizerCanvasGLFF *canvas;
	RasterizerSceneGLFF *scene;

	static GLuint system_fbo;

	/* TEXTURE */

	struct Texture : public RID_Data {
		uint32_t width, height, depth;
		uint32_t flags;
		Image::Format format;
		VS::TextureType type;
		GLuint tex_id;
		GLenum gl_format_cache;
		GLenum gl_internal_format_cache;
		GLenum gl_type_cache;
		int data_size;
		bool active;
		String path;
		Ref<Image> cached_image;
		// True only for the Texture render_target_create() creates to back
		// a RenderTarget. glCopyTexSubImage2D pulls straight from the
		// screen framebuffer (GL's bottom-left-origin convention) into
		// texture row 0, which is the OPPOSITE of a normally-loaded image
		// texture's row 0 (top of the image, per how Image/PNG data is
		// uploaded) -- same as every FBO-based backend's render-target
		// textures. Godot itself already compensates for this universally
		// (ViewportContainer draws with a negative-height rect, which
		// canvas_item_add_texture_rect turns into CANVAS_RECT_FLIP_V), so
		// no GLFF-specific extra flip is applied on top of that -- see the
		// CANVAS_RECT_FLIP_V handling in rasterizer_canvas_glff.cpp
		// (godot-ports#28: an earlier version added a redundant extra
		// flip here, which canceled Godot's own compensation back out and
		// rendered SubViewport-as-texture content upside down).
		bool is_render_target;
		// Real GL texture storage dimensions when they differ from the
		// logical width/height above -- only ever set for is_render_target
		// textures. This driver (ATI/Mesa GL 1.2, no
		// GL_ARB_texture_non_power_of_two) rejects glTexImage2D outright
		// (GL_INVALID_VALUE) for non-power-of-two sizes, which is most
		// render targets (a 716x822 editor viewport panel, the 1280x980
		// main window, etc.) -- confirmed via a standalone GL repro,
		// godot-ports#28. Fix: allocate the real GL texture at the next
		// POT size (gl_alloc_width/height) and only ever populate its
		// top-left width x height sub-rect via glCopyTexSubImage2D; the
		// logical width/height stays the real requested size so the rest
		// of the engine (UI layout, aspect-ratio math) sees the correct
		// value. Sampling code must scale UVs by
		// (width/gl_alloc_width, height/gl_alloc_height) to stay within
		// the populated sub-rect -- see the is_render_target handling in
		// rasterizer_canvas_glff.cpp. Zero means "no padding" (ordinary
		// textures, and any render target whose size already happens to
		// be POT).
		uint32_t gl_alloc_width, gl_alloc_height;

		// ViewportTexture (scene/main/viewport.cpp) never samples the
		// render target's own Texture directly -- it hands out a separate,
		// otherwise-empty "proxy" Texture (its own RID, from a bare
		// texture_create()) and calls texture_set_proxy(proxy, real_rid)
		// to link them. Every consumer (ViewportContainer included, the
		// direct cause of godot-ports#28's blank editor 3D viewport) reads
		// tex_id/width/height/is_render_target/etc. from that PROXY, never
		// the real texture, so this indirection must be resolved before
		// any of those fields are used for real rendering -- same
		// established pattern as drivers/gles2's Texture::get_ptr().
		Texture *proxy;
		Set<Texture *> proxy_owners;

		Texture() {
			width = height = depth = 0;
			flags = VS::TEXTURE_FLAGS_DEFAULT;
			format = Image::FORMAT_L8;
			type = VS::TEXTURE_TYPE_2D;
			tex_id = 0;
			data_size = 0;
			active = false;
			is_render_target = false;
			gl_alloc_width = gl_alloc_height = 0;
			proxy = nullptr;
		}

		_ALWAYS_INLINE_ Texture *get_ptr() {
			if (proxy) {
				return proxy; // Only one level of indirection -- proxies never chain.
			}
			return this;
		}

		~Texture() {
			for (Set<Texture *>::Element *E = proxy_owners.front(); E; E = E->next()) {
				E->get()->proxy = nullptr;
			}
			if (proxy) {
				proxy->proxy_owners.erase(this);
			}
		}
	};

	mutable RID_Owner<Texture> texture_owner;

	virtual RID texture_create();
	virtual void texture_allocate(RID p_texture, int p_width, int p_height, int p_depth_3d, Image::Format p_format, VS::TextureType p_type, uint32_t p_flags = VS::TEXTURE_FLAGS_DEFAULT);
	virtual void texture_set_data(RID p_texture, const Ref<Image> &p_image, int p_level = 0);
	virtual void texture_set_data_partial(RID p_texture, const Ref<Image> &p_image, int src_x, int src_y, int src_w, int src_h, int dst_x, int dst_y, int p_dst_mip, int p_level = 0);
	virtual Ref<Image> texture_get_data(RID p_texture, int p_level = 0) const;
	virtual void texture_set_flags(RID p_texture, uint32_t p_flags);
	virtual uint32_t texture_get_flags(RID p_texture) const;
	virtual Image::Format texture_get_format(RID p_texture) const;
	virtual VS::TextureType texture_get_type(RID p_texture) const;
	virtual uint32_t texture_get_texid(RID p_texture) const;
	virtual uint32_t texture_get_width(RID p_texture) const;
	virtual uint32_t texture_get_height(RID p_texture) const;
	virtual uint32_t texture_get_depth(RID p_texture) const;
	virtual void texture_set_size_override(RID p_texture, int p_width, int p_height, int p_depth_3d);
	virtual void texture_bind(RID p_texture, uint32_t p_texture_no);
	virtual void texture_set_path(RID p_texture, const String &p_path) {
		Texture *t = texture_owner.getornull(p_texture);
		ERR_FAIL_COND(!t);
		t->path = p_path;
	}
	virtual String texture_get_path(RID p_texture) const {
		Texture *t = texture_owner.getornull(p_texture);
		ERR_FAIL_COND_V(!t, String());
		return t->path;
	}
	virtual void texture_set_shrink_all_x2_on_set_data(bool p_enable) {}
	virtual void texture_debug_usage(List<VS::TextureInfo> *r_info) {}
	virtual RID texture_create_radiance_cubemap(RID p_source, int p_resolution = -1) const { return RID(); }
	virtual void texture_set_detect_3d_callback(RID p_texture, VisualServer::TextureDetectCallback p_callback, void *p_userdata) {}
	virtual void texture_set_detect_srgb_callback(RID p_texture, VisualServer::TextureDetectCallback p_callback, void *p_userdata) {}
	virtual void texture_set_detect_normal_callback(RID p_texture, VisualServer::TextureDetectCallback p_callback, void *p_userdata) {}
	virtual void textures_keep_original(bool p_enable) {}
	// NOTE: despite the (p_proxy, p_base) parameter names inherited from
	// the base VisualServer API, the real semantics (matching every other
	// backend, see drivers/gles2/rasterizer_storage_gles2.cpp) are
	// "p_texture->proxy = p_base" -- i.e. whichever RID is p_proxy here IS
	// the proxy object being configured, not the thing to fetch. Callers
	// always pass their OWN proxy RID first (see
	// scene/main/viewport.cpp:80's texture_set_proxy(proxy, vp->texture_rid)).
	virtual void texture_set_proxy(RID p_proxy, RID p_base) {
		Texture *proxy_tex = texture_owner.getornull(p_proxy);
		ERR_FAIL_COND(!proxy_tex);
		if (proxy_tex->proxy) {
			proxy_tex->proxy->proxy_owners.erase(proxy_tex);
			proxy_tex->proxy = nullptr;
		}
		if (p_base.is_valid()) {
			Texture *base_tex = texture_owner.getornull(p_base);
			ERR_FAIL_COND(!base_tex);
			ERR_FAIL_COND(base_tex == proxy_tex);
			base_tex->proxy_owners.insert(proxy_tex);
			proxy_tex->proxy = base_tex;
		}
	}
	virtual Size2 texture_size_with_proxy(RID p_texture) const { return Size2(texture_get_width(p_texture), texture_get_height(p_texture)); }
	virtual void texture_set_force_redraw_if_visible(RID p_texture, bool p_enable) {}

	/* SKY (stub -- procedural/panorama sky is a shader feature, dropped, see proposal §4.4c) */

	virtual RID sky_create() { return RID(); }
	virtual void sky_set_texture(RID p_sky, RID p_cube_map, int p_radiance_size) {}

	/* SHADER -- real for Phase 4 (godot-ports#24), but only as a tiny
	   render_mode-flag parser plus a handful of exact-literal body-marker
	   checks, never a real shader compiler. SpatialMaterial communicates
	   cull_mode/blend_mode/unshaded/ambient_light_disabled/specular_mode/
	   etc. exclusively by generating a real GLSL source string with a
	   leading "render_mode a,b,c;" line and calling shader_set_code() with
	   it (scene/resources/material.cpp's Material3D::_update_shader()) --
	   there is no other path (these are NOT sent via material_set_param()).
	   A few flags (FLAG_ALBEDO_FROM_VERTEX_COLOR, FLAG_USE_ALPHA_SCISSOR,
	   FEATURE_EMISSION) aren't render_mode tokens at all -- they're baked
	   directly into the generated fragment body/uniform declarations, so
	   detecting them means searching the code string for the exact,
	   deterministic literal substrings this fork's OWN generator emits
	   ("albedo_tex *= COLOR;" etc, see the Shader struct's own comment)
	   -- still not a general GLSL parse or shader compiler, and safe for
	   the same reason the render_mode scan is: this is recognizing fixed,
	   known output from code we control on both ends, not interpreting
	   arbitrary developer-authored GLSL (which #35's design explicitly
	   ruled out for ShaderMaterial, a genuinely different problem). GLFF
	   never compiles or executes any of this as real GLSL either way. */
	enum GLFFCullMode {
		GLFF_CULL_BACK,
		GLFF_CULL_FRONT,
		GLFF_CULL_DISABLED,
	};
	enum GLFFBlendMode {
		GLFF_BLEND_MIX,
		GLFF_BLEND_ADD,
		GLFF_BLEND_SUB,
		GLFF_BLEND_MUL,
	};

	struct Shader : public RID_Data {
		GLFFCullMode cull_mode;
		GLFFBlendMode blend_mode;
		bool unshaded;
		bool depth_test_disabled;
		// godot-ports#24 (Phase 4 remainder): ambient_light_disabled and
		// specular_disabled are, like unshaded/depth_test_disable above,
		// plain render_mode tokens (Material3D::_update_shader() emits
		// ",ambient_light_disabled"/one of ",specular_*"). Reused the
		// same token-scan mechanism, no new plumbing needed.
		bool ambient_light_disabled;
		bool specular_disabled;
		// albedo_from_vertex_color/use_alpha_scissor are NOT render_mode
		// tokens -- SpatialMaterial bakes them straight into the
		// generated fragment shader BODY ("albedo_tex *= COLOR;" /
		// "ALPHA_SCISSOR=alpha_scissor_threshold;"). This is still safe
		// to detect via a plain substring search: unlike a hand-authored
		// ShaderMaterial's arbitrary GLSL (which #35's design explicitly
		// refuses to parse, no semantic anchor), this is OUR OWN
		// deterministic first-party generator's fixed, known output on
		// this exact fork -- there's nothing "arbitrary" being
		// interpreted, just recognizing two exact literal strings this
		// codebase's own code generator can produce.
		bool albedo_from_vertex_color;
		bool use_alpha_scissor;
		bool emission_enabled;
		String code;

		Shader() {
			cull_mode = GLFF_CULL_BACK;
			blend_mode = GLFF_BLEND_MIX;
			unshaded = false;
			depth_test_disabled = false;
			ambient_light_disabled = false;
			specular_disabled = false;
			albedo_from_vertex_color = false;
			use_alpha_scissor = false;
			emission_enabled = false;
		}
	};
	mutable RID_Owner<Shader> shader_owner;

	virtual RID shader_create() {
		Shader *s = memnew(Shader);
		return shader_owner.make_rid(s);
	}
	virtual void shader_set_code(RID p_shader, const String &p_code) {
		Shader *s = shader_owner.getornull(p_shader);
		ERR_FAIL_COND(!s);
		s->code = p_code;

		// Pull out just the "render_mode a,b,c;" line's token list --
		// everything else in p_code (uniform decls, vertex()/fragment()
		// function bodies) is real GLSL this backend never compiles.
		int rm_pos = p_code.find("render_mode");
		if (rm_pos == -1) {
			return;
		}
		int semi_pos = p_code.find(";", rm_pos);
		String tokens = semi_pos == -1 ? p_code.substr(rm_pos + 11) : p_code.substr(rm_pos + 11, semi_pos - (rm_pos + 11));

		if (tokens.find("cull_front") != -1) {
			s->cull_mode = GLFF_CULL_FRONT;
		} else if (tokens.find("cull_disabled") != -1) {
			s->cull_mode = GLFF_CULL_DISABLED;
		} else {
			s->cull_mode = GLFF_CULL_BACK;
		}

		if (tokens.find("blend_add") != -1) {
			s->blend_mode = GLFF_BLEND_ADD;
		} else if (tokens.find("blend_sub") != -1) {
			s->blend_mode = GLFF_BLEND_SUB;
		} else if (tokens.find("blend_mul") != -1) {
			s->blend_mode = GLFF_BLEND_MUL;
		} else {
			s->blend_mode = GLFF_BLEND_MIX;
		}

		s->unshaded = tokens.find("unshaded") != -1;
		s->depth_test_disabled = tokens.find("depth_test_disable") != -1;
		s->ambient_light_disabled = tokens.find("ambient_light_disabled") != -1;
		s->specular_disabled = tokens.find("specular_disabled") != -1;

		// Body-marker detection (see the Shader struct comment above) --
		// deliberately exact, whole-fragment substrings from this fork's
		// own Material3D::_update_shader(), not a general GLSL parse.
		s->albedo_from_vertex_color = p_code.find("albedo_tex *= COLOR;") != -1;
		s->use_alpha_scissor = p_code.find("ALPHA_SCISSOR=alpha_scissor_threshold;") != -1;
		s->emission_enabled = p_code.find("uniform vec4 emission : hint_color;") != -1;
	}
	virtual String shader_get_code(RID p_shader) const {
		Shader *s = shader_owner.getornull(p_shader);
		ERR_FAIL_COND_V(!s, String());
		return s->code;
	}
	virtual void shader_get_param_list(RID p_shader, List<PropertyInfo> *p_param_list) const {}
	virtual void shader_set_default_texture_param(RID p_shader, const StringName &p_name, RID p_texture) {}
	virtual RID shader_get_default_texture_param(RID p_shader, const StringName &p_name) const { return RID(); }
	virtual void shader_add_custom_define(RID p_shader, const String &p_define) {}
	virtual void shader_get_custom_defines(RID p_shader, Vector<String> *p_defines) const {}
	virtual void shader_remove_custom_define(RID p_shader, const String &p_define) {}
	virtual void set_shader_async_hidden_forbidden(bool p_forbidden) {}
	virtual bool is_shader_async_hidden_forbidden() { return false; }

	/* MATERIAL -- Phase 4 (godot-ports#24) real for albedo color/texture
	   (Phase 3), cull_mode/blend_mode/unshaded/depth_test_disabled/
	   ambient_light_disabled/specular_disabled (parsed off the linked
	   Shader's render_mode line, see above), emission/emission_energy/
	   specular/roughness/alpha_scissor_threshold (real material_set_param()
	   values, gated by the Shader's emission_enabled/use_alpha_scissor
	   body-marker bools), and FLAG_ALBEDO_FROM_VERTEX_COLOR (also a
	   Shader body-marker bool, applied in render_scene() via
	   GL_COLOR_MATERIAL). Godot-ports#17's remaining mapping-table rows
	   (PBR textures beyond flat albedo, non-Lambert diffuse modes,
	   non-Blinn/Phong specular modes, point-size billboarding, dithering)
	   are confirmed-drop rows per #17/#24's own scoping, not gaps --
	   they degrade to this same flat-albedo/no-special-state behavior,
	   which is the documented fallback for all of those. */
	// Real fixed-function texture-unit combiner state for a
	// FixedFunctionMaterial (godot-ports#35) -- driven entirely by
	// material_set_param()'s "ff_*" well-known names below, since this
	// material type never has a Shader at all (no GLSL, nothing to parse
	// a render_mode line out of). env_mode/combine_func/texgen_mode mirror
	// FixedFunctionMaterial::EnvMode/CombineFunc/TexgenMode numerically
	// (both start at 0 with the same ordering) so the raw int can be used
	// directly without a translation table. FF_TEXTURE_UNIT_MAX (4) is a
	// fixed authoring-surface ceiling, not a live GL_MAX_TEXTURE_UNITS
	// query -- how many of these units are actually usable/exposed in the
	// Inspector for a given asset is instead driven by the project's
	// target-GPU tier setting (rendering/quality/gl_fixed_function/target_gpu),
	// consistent with this project's decision to gate capability off a
	// declared target rather than the editing machine's own GPU (see
	// FixedFunctionMaterial::_validate_property()). render_scene() still
	// double-checks against real detected hardware capability
	// (has_multitexture/has_texture_env_combine/has_texture_env_dot3/
	// has_texgen_reflection_map) before ever issuing a GL call for a unit
	// beyond what's actually present, regardless of the declared tier.
	static const int FF_TEXTURE_UNIT_MAX = 4;
	struct Material : public RID_Data {
		Color albedo;
		RID albedo_texture;
		RID shader;

		// godot-ports#24 (Phase 4 remainder): real SpatialMaterial params
		// beyond albedo/albedo_texture, sent via material_set_param()
		// under the exact string keys Material3D itself uses (confirmed
		// by reading scene/resources/material.cpp's ShaderNames setup --
		// "emission"/"emission_energy"/"specular"/"roughness"/
		// "alpha_scissor_threshold"). Whether each is actually USED for a
		// given surface is gated by the linked Shader's render_mode-
		// derived bools below (emission_enabled/specular_disabled/
		// use_alpha_scissor) -- e.g. emission is always stored but only
		// applied when FEATURE_EMISSION was on. Defaults mirror
		// SpatialMaterial's own real defaults.
		Color emission;
		float emission_energy;
		float specular;
		float roughness;
		float alpha_scissor_threshold;

		// True only for a FixedFunctionMaterial (set via the "ff_active"
		// param, always sent once by its constructor) -- when true,
		// render_scene() drives real per-unit glTexEnvi/GL_COMBINE/
		// GL_DOT3_RGB/glTexGeni state from ff_tex/ff_env_mode/
		// ff_combine_func/ff_texgen_mode instead of the single-albedo-
		// texture SpatialMaterial path, and reads cull/blend/unshaded/
		// depth-test from ff_cull_mode/ff_blend_mode/ff_unshaded/
		// ff_depth_test_disabled directly (there's no Shader/render_mode
		// string to parse those from here).
		bool ff_active;
		RID ff_tex[FF_TEXTURE_UNIT_MAX];
		int ff_env_mode[FF_TEXTURE_UNIT_MAX];
		int ff_combine_func[FF_TEXTURE_UNIT_MAX];
		int ff_texgen_mode[FF_TEXTURE_UNIT_MAX];
		GLFFCullMode ff_cull_mode;
		GLFFBlendMode ff_blend_mode;
		bool ff_unshaded;
		bool ff_depth_test_disabled;

		Material() {
			albedo = Color(1, 1, 1, 1);
			emission = Color(0, 0, 0, 1);
			emission_energy = 1.0;
			specular = 0.5;
			roughness = 1.0;
			alpha_scissor_threshold = 0.5;
			ff_active = false;
			for (int i = 0; i < FF_TEXTURE_UNIT_MAX; i++) {
				ff_env_mode[i] = 0;
				ff_combine_func[i] = 0;
				ff_texgen_mode[i] = 0;
			}
			ff_cull_mode = GLFF_CULL_BACK;
			ff_blend_mode = GLFF_BLEND_MIX;
			ff_unshaded = false;
			ff_depth_test_disabled = false;
		}
	};
	mutable RID_Owner<Material> material_owner;

	virtual RID material_create() {
		Material *m = memnew(Material);
		return material_owner.make_rid(m);
	}
	virtual void material_set_render_priority(RID p_material, int priority) {}
	virtual void material_set_shader(RID p_shader_material, RID p_shader) {
		Material *m = material_owner.getornull(p_shader_material);
		ERR_FAIL_COND(!m);
		m->shader = p_shader;
	}
	virtual RID material_get_shader(RID p_shader_material) const {
		Material *m = material_owner.getornull(p_shader_material);
		ERR_FAIL_COND_V(!m, RID());
		return m->shader;
	}
	virtual void material_set_param(RID p_material, const StringName &p_param, const Variant &p_value) {
		Material *m = material_owner.getornull(p_material);
		ERR_FAIL_COND(!m);
		if (p_param == StringName("albedo")) {
			m->albedo = p_value;
		} else if (p_param == StringName("texture_albedo")) {
			m->albedo_texture = p_value;
		} else if (p_param == StringName("emission")) {
			m->emission = p_value;
		} else if (p_param == StringName("emission_energy")) {
			m->emission_energy = p_value;
		} else if (p_param == StringName("specular")) {
			m->specular = p_value;
		} else if (p_param == StringName("roughness")) {
			m->roughness = p_value;
		} else if (p_param == StringName("alpha_scissor_threshold")) {
			m->alpha_scissor_threshold = p_value;
		} else if (p_param == StringName("ff_active")) {
			m->ff_active = p_value;
		} else if (p_param == StringName("ff_cull_mode")) {
			m->ff_cull_mode = (GLFFCullMode)(int)p_value;
		} else if (p_param == StringName("ff_blend_mode")) {
			m->ff_blend_mode = (GLFFBlendMode)(int)p_value;
		} else if (p_param == StringName("ff_unshaded")) {
			m->ff_unshaded = p_value;
		} else if (p_param == StringName("ff_depth_test_disabled")) {
			m->ff_depth_test_disabled = p_value;
		} else {
			// Per-texture-unit params ("ff_tex0".."ff_tex3", etc.) --
			// parsed by prefix + trailing unit index instead of one
			// explicit branch per unit per property, since
			// FF_TEXTURE_UNIT_MAX properties would otherwise mean 4
			// near-identical branches per property.
			String param_str = String(p_param);
			auto parse_unit_suffix = [&](const char *p_prefix) -> int {
				String prefix(p_prefix);
				if (!param_str.begins_with(prefix)) {
					return -1;
				}
				String suffix = param_str.substr(prefix.length(), param_str.length() - prefix.length());
				if (!suffix.is_valid_integer()) {
					return -1;
				}
				int idx = suffix.to_int();
				if (idx < 0 || idx >= FF_TEXTURE_UNIT_MAX) {
					return -1;
				}
				return idx;
			};
			int idx;
			if ((idx = parse_unit_suffix("ff_tex")) >= 0) {
				m->ff_tex[idx] = p_value;
			} else if ((idx = parse_unit_suffix("ff_env_mode")) >= 0) {
				m->ff_env_mode[idx] = p_value;
			} else if ((idx = parse_unit_suffix("ff_combine_func")) >= 0) {
				m->ff_combine_func[idx] = p_value;
			} else if ((idx = parse_unit_suffix("ff_texgen_mode")) >= 0) {
				m->ff_texgen_mode[idx] = p_value;
			}
		}
	}
	virtual Variant material_get_param(RID p_material, const StringName &p_param) const { return Variant(); }
	virtual Variant material_get_param_default(RID p_material, const StringName &p_param) const { return Variant(); }
	virtual void material_set_line_width(RID p_material, float p_width) {}
	virtual void material_set_next_pass(RID p_material, RID p_next_material) {}
	virtual bool material_is_animated(RID p_material) { return false; }
	virtual bool material_casts_shadows(RID p_material) { return false; }
	virtual void material_add_instance_owner(RID p_material, RasterizerScene::InstanceBase *p_instance) {}
	virtual void material_remove_instance_owner(RID p_material, RasterizerScene::InstanceBase *p_instance) {}

	/* MESH -- real for Phase 1: client-side arrays only (no VBOs, see
	   proposal §2 -- strict GL 1.2, no ARB_vertex_buffer_object assumed) */

	struct Surface {
		uint32_t format;
		VS::PrimitiveType primitive;
		PoolVector<uint8_t> array;
		int vertex_count;
		PoolVector<uint8_t> index_array;
		int index_count;
		AABB aabb;
		Vector<PoolVector<uint8_t>> blend_shapes;
		Vector<AABB> bone_aabbs;
		RID material;

		// Phase 3: decoded once at mesh_add_surface() time into plain,
		// uncompressed CPU-side arrays -- fixed-function glVertexPointer/
		// glNormalPointer/glColorPointer/glTexCoordPointer can't consume
		// the packed/compressed formats a shader-based glVertexAttribPointer
		// pipeline can (half-float, octahedral-compressed normals, byte
		// colors -- see VisualServer::mesh_surface_make_offsets_from_format
		// for the authoritative wire layout this decode mirrors). Tangent,
		// UV2 (lightmap), bones, and weights are deliberately not decoded --
		// no normal-mapping, lightmap-blending, or skinning in this backend
		// yet (see godot-ports#14 proposal, Phase 3 scope).
		PoolVector<Vector3> vertices;
		PoolVector<Vector3> normals;
		PoolVector<Color> colors;
		PoolVector<Vector2> uvs;
		bool has_normals = false;
		bool has_colors = false;
		bool has_uvs = false;
	};

	struct Mesh : public RID_Data {
		Vector<Surface *> surfaces;
		int blend_shape_count;
		VS::BlendShapeMode blend_shape_mode;
		PoolVector<float> blend_shape_values;
		AABB custom_aabb;
		bool custom_aabb_valid;

		Mesh() {
			blend_shape_count = 0;
			blend_shape_mode = VS::BLEND_SHAPE_MODE_NORMALIZED;
			custom_aabb_valid = false;
		}
	};

	mutable RID_Owner<Mesh> mesh_owner;

	// godot-ports#22: decodes a Surface's raw surface->array bytes (per
	// surface->format) into the plain CPU-side vertices/normals/colors/uvs
	// PoolVectors render_scene() actually reads. Originally only ran once,
	// inline in mesh_add_surface() -- factored out so
	// mesh_surface_update_region() (called every frame by the engine's
	// software_skinning_fallback mechanism, see has_os_feature() below) can
	// re-run it after patching surface->array, instead of leaving the
	// decoded arrays permanently frozen at their initial bind-pose values
	// regardless of any later per-vertex update. Real bug found live: a
	// software-skinned mesh rendered its correct REST pose but never
	// visibly deformed for any bone pose, confirmed via a straight-vs-bent
	// screenshot comparison showing zero difference before this fix.
	static void _decode_surface_arrays(Surface *surface);

	virtual RID mesh_create();
	virtual void mesh_add_surface(RID p_mesh, uint32_t p_format, VS::PrimitiveType p_primitive, const PoolVector<uint8_t> &p_array, int p_vertex_count, const PoolVector<uint8_t> &p_index_array, int p_index_count, const AABB &p_aabb, const Vector<PoolVector<uint8_t>> &p_blend_shapes = Vector<PoolVector<uint8_t>>(), const Vector<AABB> &p_bone_aabbs = Vector<AABB>());
	virtual void mesh_set_blend_shape_count(RID p_mesh, int p_amount);
	virtual int mesh_get_blend_shape_count(RID p_mesh) const;
	virtual void mesh_set_blend_shape_mode(RID p_mesh, VS::BlendShapeMode p_mode);
	virtual VS::BlendShapeMode mesh_get_blend_shape_mode(RID p_mesh) const;
	virtual void mesh_set_blend_shape_values(RID p_mesh, PoolVector<float> p_values);
	virtual PoolVector<float> mesh_get_blend_shape_values(RID p_mesh) const;
	virtual void mesh_surface_update_region(RID p_mesh, int p_surface, int p_offset, const PoolVector<uint8_t> &p_data);
	virtual void mesh_surface_set_material(RID p_mesh, int p_surface, RID p_material);
	virtual RID mesh_surface_get_material(RID p_mesh, int p_surface) const;
	virtual int mesh_surface_get_array_len(RID p_mesh, int p_surface) const;
	virtual int mesh_surface_get_array_index_len(RID p_mesh, int p_surface) const;
	virtual PoolVector<uint8_t> mesh_surface_get_array(RID p_mesh, int p_surface) const;
	virtual PoolVector<uint8_t> mesh_surface_get_index_array(RID p_mesh, int p_surface) const;
	virtual uint32_t mesh_surface_get_format(RID p_mesh, int p_surface) const;
	virtual VS::PrimitiveType mesh_surface_get_primitive_type(RID p_mesh, int p_surface) const;
	virtual AABB mesh_surface_get_aabb(RID p_mesh, int p_surface) const;
	virtual Vector<PoolVector<uint8_t>> mesh_surface_get_blend_shapes(RID p_mesh, int p_surface) const;
	virtual Vector<AABB> mesh_surface_get_skeleton_aabb(RID p_mesh, int p_surface) const;
	virtual void mesh_remove_surface(RID p_mesh, int p_index);
	virtual int mesh_get_surface_count(RID p_mesh) const;
	virtual void mesh_set_custom_aabb(RID p_mesh, const AABB &p_aabb);
	virtual AABB mesh_get_custom_aabb(RID p_mesh) const;
	virtual AABB mesh_get_aabb(RID p_mesh, RID p_skeleton) const;
	virtual void mesh_clear(RID p_mesh);

	/* MULTIMESH (stub -- confirmed in research that GLES2 already submits
	   each instance as a separate CPU-side draw call, not real GPU
	   instancing, so a real implementation here is just "call
	   mesh rendering once per instance transform" -- deferred to whichever
	   phase wires up RasterizerScene's instance loop, not needed to
	   compile Phase 1) */

	struct MultiMesh : public RID_Data {
		RID mesh;
		int instance_count;
	};
	mutable RID_Owner<MultiMesh> multimesh_owner;

	virtual void multimesh_attach_canvas_item(RID p_multimesh, RID p_canvas_item, bool p_attach) {}
	virtual RID _multimesh_create() {
		MultiMesh *mm = memnew(MultiMesh);
		mm->instance_count = 0;
		return multimesh_owner.make_rid(mm);
	}
	virtual void _multimesh_allocate(RID p_multimesh, int p_instances, VS::MultimeshTransformFormat p_transform_format, VS::MultimeshColorFormat p_color_format, VS::MultimeshCustomDataFormat p_data = VS::MULTIMESH_CUSTOM_DATA_NONE) {
		MultiMesh *mm = multimesh_owner.getornull(p_multimesh);
		ERR_FAIL_COND(!mm);
		mm->instance_count = p_instances;
	}
	virtual int _multimesh_get_instance_count(RID p_multimesh) const {
		MultiMesh *mm = multimesh_owner.getornull(p_multimesh);
		ERR_FAIL_COND_V(!mm, 0);
		return mm->instance_count;
	}
	virtual void _multimesh_set_mesh(RID p_multimesh, RID p_mesh) {
		MultiMesh *mm = multimesh_owner.getornull(p_multimesh);
		ERR_FAIL_COND(!mm);
		mm->mesh = p_mesh;
	}
	virtual void _multimesh_instance_set_transform(RID p_multimesh, int p_index, const Transform &p_transform) {}
	virtual void _multimesh_instance_set_transform_2d(RID p_multimesh, int p_index, const Transform2D &p_transform) {}
	virtual void _multimesh_instance_set_color(RID p_multimesh, int p_index, const Color &p_color) {}
	virtual void _multimesh_instance_set_custom_data(RID p_multimesh, int p_index, const Color &p_color) {}
	virtual RID _multimesh_get_mesh(RID p_multimesh) const {
		MultiMesh *mm = multimesh_owner.getornull(p_multimesh);
		ERR_FAIL_COND_V(!mm, RID());
		return mm->mesh;
	}
	virtual Transform _multimesh_instance_get_transform(RID p_multimesh, int p_index) const { return Transform(); }
	virtual Transform2D _multimesh_instance_get_transform_2d(RID p_multimesh, int p_index) const { return Transform2D(); }
	virtual Color _multimesh_instance_get_color(RID p_multimesh, int p_index) const { return Color(); }
	virtual Color _multimesh_instance_get_custom_data(RID p_multimesh, int p_index) const { return Color(); }
	virtual void _multimesh_set_as_bulk_array(RID p_multimesh, const PoolVector<float> &p_array) {}
	virtual void _multimesh_set_visible_instances(RID p_multimesh, int p_visible) {}
	virtual int _multimesh_get_visible_instances(RID p_multimesh) const { return -1; }
	virtual AABB _multimesh_get_aabb(RID p_multimesh) const { return AABB(); }
	virtual MMInterpolator *_multimesh_get_interpolator(RID p_multimesh) const { return nullptr; }

	/* IMMEDIATE (stub -- editor/debug immediate-mode draw calls; low
	   priority for Phase 1, revisit if editor gizmo work needs it) */

	struct Immediate : public RID_Data {};
	mutable RID_Owner<Immediate> immediate_owner;

	virtual RID immediate_create() { return immediate_owner.make_rid(memnew(Immediate)); }
	virtual void immediate_begin(RID p_immediate, VS::PrimitiveType p_rimitive, RID p_texture = RID()) {}
	virtual void immediate_vertex(RID p_immediate, const Vector3 &p_vertex) {}
	virtual void immediate_normal(RID p_immediate, const Vector3 &p_normal) {}
	virtual void immediate_tangent(RID p_immediate, const Plane &p_tangent) {}
	virtual void immediate_color(RID p_immediate, const Color &p_color) {}
	virtual void immediate_uv(RID p_immediate, const Vector2 &tex_uv) {}
	virtual void immediate_uv2(RID p_immediate, const Vector2 &tex_uv) {}
	virtual void immediate_end(RID p_immediate) {}
	virtual void immediate_clear(RID p_immediate) {}
	virtual void immediate_set_material(RID p_immediate, RID p_material) {}
	virtual RID immediate_get_material(RID p_immediate) const { return RID(); }
	virtual AABB immediate_get_aabb(RID p_immediate) const { return AABB(); }

	/* SKELETON (stub -- this RasterizerStorage-side Skeleton object is
	   never actually used for 3D skinning at all. godot-ports#22 wires
	   real CPU skinning up entirely via has_os_feature("skinning_fallback")
	   below, reusing the engine's existing software_skinning_fallback
	   mechanism (scene/3d/mesh_instance.cpp) -- MeshInstance detaches its
	   skeleton from the VisualServer instance once software skinning is
	   active, so this Skeleton RID exists only to satisfy the interface
	   for any code path that still queries it, never consulted for real
	   bone data.) */

	struct Skeleton : public RID_Data {
		int bone_count;
		Vector<Transform> bones;
	};
	mutable RID_Owner<Skeleton> skeleton_owner;

	virtual RID skeleton_create() { return skeleton_owner.make_rid(memnew(Skeleton)); }
	virtual void skeleton_allocate(RID p_skeleton, int p_bones, bool p_2d_skeleton = false) {
		Skeleton *s = skeleton_owner.getornull(p_skeleton);
		ERR_FAIL_COND(!s);
		s->bone_count = p_bones;
		s->bones.resize(p_bones);
	}
	virtual int skeleton_get_bone_count(RID p_skeleton) const {
		Skeleton *s = skeleton_owner.getornull(p_skeleton);
		ERR_FAIL_COND_V(!s, 0);
		return s->bone_count;
	}
	virtual void skeleton_bone_set_transform(RID p_skeleton, int p_bone, const Transform &p_transform) {
		Skeleton *s = skeleton_owner.getornull(p_skeleton);
		ERR_FAIL_COND(!s);
		ERR_FAIL_INDEX(p_bone, s->bones.size());
		s->bones.write[p_bone] = p_transform;
	}
	virtual Transform skeleton_bone_get_transform(RID p_skeleton, int p_bone) const {
		Skeleton *s = skeleton_owner.getornull(p_skeleton);
		ERR_FAIL_COND_V(!s, Transform());
		ERR_FAIL_INDEX_V(p_bone, s->bones.size(), Transform());
		return s->bones[p_bone];
	}
	virtual void skeleton_bone_set_transform_2d(RID p_skeleton, int p_bone, const Transform2D &p_transform) {}
	virtual Transform2D skeleton_bone_get_transform_2d(RID p_skeleton, int p_bone) const { return Transform2D(); }
	virtual void skeleton_set_base_transform_2d(RID p_skeleton, const Transform2D &p_base_transform) {}
	virtual uint32_t skeleton_get_revision(RID p_skeleton) const { return 0; }
	virtual void skeleton_attach_canvas_item(RID p_skeleton, RID p_canvas_item, bool p_attach) {}

	/* LIGHT -- minimal real storage for Phase 1/3: enough state that a
	   later phase can map it onto glLight()/glMaterial(); no shadow
	   support (dropped, see proposal §5/§8.1) */

	struct Light : public RID_Data {
		VS::LightType type;
		Color color;
		float param[VS::LIGHT_PARAM_MAX];
		bool negative;
		uint32_t cull_mask;
		bool reverse_cull;
		bool use_gi;

		Light() {
			type = VS::LIGHT_DIRECTIONAL;
			color = Color(1, 1, 1);
			for (int i = 0; i < VS::LIGHT_PARAM_MAX; i++) {
				param[i] = 0;
			}
			negative = false;
			cull_mask = 0xFFFFFFFF;
			reverse_cull = false;
			use_gi = false;
		}
	};
	mutable RID_Owner<Light> light_owner;

	virtual RID light_create(VS::LightType p_type) {
		Light *l = memnew(Light);
		l->type = p_type;
		return light_owner.make_rid(l);
	}
	virtual void light_set_color(RID p_light, const Color &p_color) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND(!l);
		l->color = p_color;
	}
	virtual void light_set_param(RID p_light, VS::LightParam p_param, float p_value) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND(!l);
		ERR_FAIL_INDEX(p_param, VS::LIGHT_PARAM_MAX);
		l->param[p_param] = p_value;
	}
	virtual void light_set_shadow(RID p_light, bool p_enabled) {}
	virtual void light_set_shadow_color(RID p_light, const Color &p_color) {}
	virtual void light_set_projector(RID p_light, RID p_texture) {}
	virtual void light_set_negative(RID p_light, bool p_enable) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND(!l);
		l->negative = p_enable;
	}
	virtual void light_set_cull_mask(RID p_light, uint32_t p_mask) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND(!l);
		l->cull_mask = p_mask;
	}
	virtual void light_set_reverse_cull_face_mode(RID p_light, bool p_enabled) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND(!l);
		l->reverse_cull = p_enabled;
	}
	virtual void light_set_use_gi(RID p_light, bool p_enable) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND(!l);
		l->use_gi = p_enable;
	}
	virtual void light_set_bake_mode(RID p_light, VS::LightBakeMode p_bake_mode) {}
	virtual void light_omni_set_shadow_mode(RID p_light, VS::LightOmniShadowMode p_mode) {}
	virtual void light_omni_set_shadow_detail(RID p_light, VS::LightOmniShadowDetail p_detail) {}
	virtual void light_directional_set_shadow_mode(RID p_light, VS::LightDirectionalShadowMode p_mode) {}
	virtual void light_directional_set_blend_splits(RID p_light, bool p_enable) {}
	virtual bool light_directional_get_blend_splits(RID p_light) const { return false; }
	virtual void light_directional_set_shadow_depth_range_mode(RID p_light, VS::LightDirectionalShadowDepthRangeMode p_range_mode) {}
	virtual VS::LightDirectionalShadowDepthRangeMode light_directional_get_shadow_depth_range_mode(RID p_light) const { return VS::LIGHT_DIRECTIONAL_SHADOW_DEPTH_RANGE_STABLE; }
	virtual VS::LightDirectionalShadowMode light_directional_get_shadow_mode(RID p_light) { return VS::LIGHT_DIRECTIONAL_SHADOW_ORTHOGONAL; }
	virtual VS::LightOmniShadowMode light_omni_get_shadow_mode(RID p_light) { return VS::LIGHT_OMNI_SHADOW_DUAL_PARABOLOID; }
	virtual bool light_has_shadow(RID p_light) const { return false; }
	virtual VS::LightType light_get_type(RID p_light) const {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND_V(!l, VS::LIGHT_DIRECTIONAL);
		return l->type;
	}
	virtual AABB light_get_aabb(RID p_light) const { return AABB(); }
	virtual float light_get_param(RID p_light, VS::LightParam p_param) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND_V(!l, 0);
		ERR_FAIL_INDEX_V(p_param, VS::LIGHT_PARAM_MAX, 0);
		return l->param[p_param];
	}
	virtual Color light_get_color(RID p_light) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND_V(!l, Color());
		return l->color;
	}
	virtual bool light_get_use_gi(RID p_light) {
		Light *l = light_owner.getornull(p_light);
		ERR_FAIL_COND_V(!l, false);
		return l->use_gi;
	}
	virtual VS::LightBakeMode light_get_bake_mode(RID p_light) { return VS::LIGHT_BAKE_DISABLED; }
	virtual uint64_t light_get_version(RID p_light) const { return 0; }

	/* REFLECTION PROBE (stub -- dropped, see proposal §8.1) */

	struct ReflectionProbe : public RID_Data {};
	mutable RID_Owner<ReflectionProbe> reflection_probe_owner;

	virtual RID reflection_probe_create() { return reflection_probe_owner.make_rid(memnew(ReflectionProbe)); }
	virtual void reflection_probe_set_update_mode(RID p_probe, VS::ReflectionProbeUpdateMode p_mode) {}
	virtual void reflection_probe_set_resolution(RID p_probe, int p_resolution) {}
	virtual void reflection_probe_set_intensity(RID p_probe, float p_intensity) {}
	virtual void reflection_probe_set_interior_ambient(RID p_probe, const Color &p_ambient) {}
	virtual void reflection_probe_set_interior_ambient_energy(RID p_probe, float p_energy) {}
	virtual void reflection_probe_set_interior_ambient_probe_contribution(RID p_probe, float p_contrib) {}
	virtual void reflection_probe_set_max_distance(RID p_probe, float p_distance) {}
	virtual void reflection_probe_set_extents(RID p_probe, const Vector3 &p_extents) {}
	virtual void reflection_probe_set_origin_offset(RID p_probe, const Vector3 &p_offset) {}
	virtual void reflection_probe_set_as_interior(RID p_probe, bool p_enable) {}
	virtual void reflection_probe_set_enable_box_projection(RID p_probe, bool p_enable) {}
	virtual void reflection_probe_set_enable_shadows(RID p_probe, bool p_enable) {}
	virtual void reflection_probe_set_cull_mask(RID p_probe, uint32_t p_layers) {}
	virtual AABB reflection_probe_get_aabb(RID p_probe) const { return AABB(); }
	virtual VS::ReflectionProbeUpdateMode reflection_probe_get_update_mode(RID p_probe) const { return VS::REFLECTION_PROBE_UPDATE_ONCE; }
	virtual uint32_t reflection_probe_get_cull_mask(RID p_probe) const { return 0; }
	virtual Vector3 reflection_probe_get_extents(RID p_probe) const { return Vector3(); }
	virtual Vector3 reflection_probe_get_origin_offset(RID p_probe) const { return Vector3(); }
	virtual float reflection_probe_get_origin_max_distance(RID p_probe) const { return 0; }
	virtual bool reflection_probe_renders_shadows(RID p_probe) const { return false; }

	virtual void instance_add_skeleton(RID p_skeleton, RasterizerScene::InstanceBase *p_instance) {}
	virtual void instance_remove_skeleton(RID p_skeleton, RasterizerScene::InstanceBase *p_instance) {}
	virtual void instance_add_dependency(RID p_base, RasterizerScene::InstanceBase *p_instance) {}
	virtual void instance_remove_dependency(RID p_base, RasterizerScene::InstanceBase *p_instance) {}

	/* GI PROBE (stub -- dropped, see proposal §8.1) */

	struct GIProbe : public RID_Data {};
	mutable RID_Owner<GIProbe> gi_probe_owner;

	virtual RID gi_probe_create() { return gi_probe_owner.make_rid(memnew(GIProbe)); }
	virtual void gi_probe_set_bounds(RID p_probe, const AABB &p_bounds) {}
	virtual AABB gi_probe_get_bounds(RID p_probe) const { return AABB(); }
	virtual void gi_probe_set_cell_size(RID p_probe, float p_range) {}
	virtual float gi_probe_get_cell_size(RID p_probe) const { return 0; }
	virtual void gi_probe_set_to_cell_xform(RID p_probe, const Transform &p_xform) {}
	virtual Transform gi_probe_get_to_cell_xform(RID p_probe) const { return Transform(); }
	virtual void gi_probe_set_dynamic_data(RID p_probe, const PoolVector<int> &p_data) {}
	virtual PoolVector<int> gi_probe_get_dynamic_data(RID p_probe) const { return PoolVector<int>(); }
	virtual void gi_probe_set_dynamic_range(RID p_probe, int p_range) {}
	virtual int gi_probe_get_dynamic_range(RID p_probe) const { return 0; }
	virtual void gi_probe_set_energy(RID p_probe, float p_range) {}
	virtual float gi_probe_get_energy(RID p_probe) const { return 0; }
	virtual void gi_probe_set_bias(RID p_probe, float p_range) {}
	virtual float gi_probe_get_bias(RID p_probe) const { return 0; }
	virtual void gi_probe_set_normal_bias(RID p_probe, float p_range) {}
	virtual float gi_probe_get_normal_bias(RID p_probe) const { return 0; }
	virtual void gi_probe_set_propagation(RID p_probe, float p_range) {}
	virtual float gi_probe_get_propagation(RID p_probe) const { return 0; }
	virtual void gi_probe_set_interior(RID p_probe, bool p_enable) {}
	virtual bool gi_probe_is_interior(RID p_probe) const { return false; }
	virtual void gi_probe_set_compress(RID p_probe, bool p_enable) {}
	virtual bool gi_probe_is_compressed(RID p_probe) const { return false; }
	virtual uint32_t gi_probe_get_version(RID p_probe) { return 0; }
	virtual RID gi_probe_dynamic_data_create(int p_width, int p_height, int p_depth, GIProbeCompression p_compression) { return RID(); }
	virtual void gi_probe_dynamic_data_update(RID p_gi_probe_data, int p_depth_slice, int p_slice_count, int p_mipmap, const void *p_data) {}

	/* LIGHTMAP CAPTURE (stub -- dynamic-object indirect lighting has no
	   FF equivalent, dropped per proposal §5/§8.1; static surface
	   lightmapping, which IS kept, is a RasterizerScene concern, not
	   this) */

	struct LightmapCapture : public RID_Data {};
	mutable RID_Owner<LightmapCapture> lightmap_capture_data_owner;

	virtual RID lightmap_capture_create() { return lightmap_capture_data_owner.make_rid(memnew(LightmapCapture)); }
	virtual void lightmap_capture_set_bounds(RID p_capture, const AABB &p_bounds) {}
	virtual AABB lightmap_capture_get_bounds(RID p_capture) const { return AABB(); }
	virtual void lightmap_capture_set_octree(RID p_capture, const PoolVector<uint8_t> &p_octree) {}
	virtual PoolVector<uint8_t> lightmap_capture_get_octree(RID p_capture) const { return PoolVector<uint8_t>(); }
	virtual void lightmap_capture_set_octree_cell_transform(RID p_capture, const Transform &p_xform) {}
	virtual Transform lightmap_capture_get_octree_cell_transform(RID p_capture) const { return Transform(); }
	virtual void lightmap_capture_set_octree_cell_subdiv(RID p_capture, int p_subdiv) {}
	virtual int lightmap_capture_get_octree_cell_subdiv(RID p_capture) const { return 0; }
	virtual void lightmap_capture_set_energy(RID p_capture, float p_energy) {}
	virtual float lightmap_capture_get_energy(RID p_capture) const { return 0; }
	virtual void lightmap_capture_set_interior(RID p_capture, bool p_interior) {}
	virtual bool lightmap_capture_is_interior(RID p_capture) const { return false; }
	virtual const PoolVector<LightmapCaptureOctree> *lightmap_capture_get_octree_ptr(RID p_capture) const { return nullptr; }

	/* PARTICLES (stub -- GPUParticles has no fixed-function path; this is
	   a content-authoring constraint (use CPUParticles instead), not
	   something this backend can bypass, see proposal §8.1) */

	struct Particles : public RID_Data {};
	mutable RID_Owner<Particles> particles_owner;

	virtual RID particles_create() { return particles_owner.make_rid(memnew(Particles)); }
	virtual void particles_set_emitting(RID p_particles, bool p_emitting) {}
	virtual bool particles_get_emitting(RID p_particles) { return false; }
	virtual void particles_set_amount(RID p_particles, int p_amount) {}
	virtual void particles_set_lifetime(RID p_particles, float p_lifetime) {}
	virtual void particles_set_one_shot(RID p_particles, bool p_one_shot) {}
	virtual void particles_set_pre_process_time(RID p_particles, float p_time) {}
	virtual void particles_set_explosiveness_ratio(RID p_particles, float p_ratio) {}
	virtual void particles_set_randomness_ratio(RID p_particles, float p_ratio) {}
	virtual void particles_set_custom_aabb(RID p_particles, const AABB &p_aabb) {}
	virtual void particles_set_speed_scale(RID p_particles, float p_scale) {}
	virtual void particles_set_use_local_coordinates(RID p_particles, bool p_enable) {}
	virtual void particles_set_process_material(RID p_particles, RID p_material) {}
	virtual void particles_set_fixed_fps(RID p_particles, int p_fps) {}
	virtual void particles_set_fractional_delta(RID p_particles, bool p_enable) {}
	virtual void particles_restart(RID p_particles) {}
	virtual bool particles_is_inactive(RID p_particles) const { return true; }
	virtual void particles_set_draw_order(RID p_particles, VS::ParticlesDrawOrder p_order) {}
	virtual void particles_set_draw_passes(RID p_particles, int p_count) {}
	virtual void particles_set_draw_pass_mesh(RID p_particles, int p_pass, RID p_mesh) {}
	virtual void particles_request_process(RID p_particles) {}
	virtual AABB particles_get_current_aabb(RID p_particles) { return AABB(); }
	virtual AABB particles_get_aabb(RID p_particles) const { return AABB(); }
	virtual void particles_set_emission_transform(RID p_particles, const Transform &p_transform) {}
	virtual int particles_get_draw_passes(RID p_particles) const { return 0; }
	virtual RID particles_get_draw_pass_mesh(RID p_particles, int p_pass) const { return RID(); }

	/* RENDER TARGET. GLFF has no FBO (see proposal §2), so there is no
	   real off-screen surface to render into -- the main viewport is
	   forced render-direct-to-screen (§4.3/§4.4a) and draws straight to
	   the window backbuffer, never touching any of this. But a
	   SubViewport used purely *as a texture* (ViewportContainer, camera
	   previews, minimaps, and the editor's own 3D panel -- confirmed via
	   SpatialEditorViewport wrapping a real Viewport in a
	   ViewportContainer) still needs SOMETHING real behind
	   render_target_get_texture(), or every consumer draws an untextured
	   (blank/white) quad -- this was the actual cause of godot-ports#28's
	   blank editor 3D viewport.
	   Fix: every "render target" here IS a real Texture (reusing
	   texture_create()/texture_allocate()'s existing GL_RGBA8 storage
	   allocation, not new code); RasterizerGLFF::set_current_render_target()
	   captures whatever was just drawn into the shared window backbuffer's
	   (0,0)-origin region via glCopyTexSubImage2D into that texture right
	   before the NEXT render target (or the final on-screen pass)
	   overwrites the same physical pixels -- see render_target_copy_to_texture()
	   below and the call site in rasterizer_glff.cpp. This works because
	   every render target already draws at glViewport(0,0,width,height)
	   (unchanged behavior), so capturing "whatever's currently at the
	   window's origin, sized to this target" is always exactly this
	   target's own just-rendered content, never another target's. */

	struct RenderTarget : public RID_Data {
		int width, height;
		RID texture;
	};
	mutable RID_Owner<RenderTarget> render_target_owner;

	virtual RID render_target_create() {
		RenderTarget *rt = memnew(RenderTarget);
		rt->width = rt->height = 0;
		rt->texture = texture_create();
		Texture *tex = texture_owner.getornull(rt->texture);
		if (tex) {
			tex->active = true;
			tex->is_render_target = true;
		}
		return render_target_owner.make_rid(rt);
	}
	virtual void render_target_set_position(RID p_render_target, int p_x, int p_y) {}
	virtual void render_target_set_size(RID p_render_target, int p_width, int p_height) {
		RenderTarget *rt = render_target_owner.getornull(p_render_target);
		ERR_FAIL_COND(!rt);
		if (rt->width == p_width && rt->height == p_height) {
			return;
		}
		rt->width = p_width;
		rt->height = p_height;
		if (p_width > 0 && p_height > 0) {
			int pot_w = 1;
			while (pot_w < p_width) {
				pot_w <<= 1;
			}
			int pot_h = 1;
			while (pot_h < p_height) {
				pot_h <<= 1;
			}
			texture_allocate(rt->texture, pot_w, pot_h, 0, Image::FORMAT_RGBA8, VS::TEXTURE_TYPE_2D, VS::TEXTURE_FLAG_FILTER);
			Texture *tex = texture_owner.getornull(rt->texture);
			if (tex) {
				tex->width = p_width;
				tex->height = p_height;
				tex->gl_alloc_width = pot_w;
				tex->gl_alloc_height = pot_h;
			}
		}
	}
	// Called by RasterizerGLFF::set_current_render_target() right before
	// this target's shared backbuffer region gets overwritten by whatever
	// renders next -- see the RenderTarget comment above for why this is
	// the only correct place to do this capture under strict-GL-1.2's
	// no-FBO floor.
	void render_target_copy_to_texture(RID p_render_target) {
		RenderTarget *rt = render_target_owner.getornull(p_render_target);
		if (!rt || rt->width <= 0 || rt->height <= 0) {
			return;
		}
		Texture *tex = texture_owner.getornull(rt->texture);
		if (!tex) {
			return;
		}
		// Drain any backlogged, unrelated GL errors first -- glGetError()
		// only ever returns the OLDEST unretrieved error, so without this
		// an error from some earlier, unrelated engine GL call would be
		// misattributed to this copy (a real gotcha already hit once
		// earlier this project, see feedback_opengl_graphics_debugging).
		while (glGetError() != GL_NO_ERROR) {
		}
		glBindTexture(GL_TEXTURE_2D, tex->tex_id);
		// Only the real (logical, possibly non-POT) width/height is ever
		// populated -- see the gl_alloc_width/height comment on Texture
		// above for why the underlying storage may be larger.
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, rt->width, rt->height);
	}
	virtual RID render_target_get_texture(RID p_render_target) const {
		RenderTarget *rt = render_target_owner.getornull(p_render_target);
		ERR_FAIL_COND_V(!rt, RID());
		return rt->texture;
	}
	virtual uint32_t render_target_get_depth_texture_id(RID p_render_target) const { return 0; }
	virtual void render_target_set_external_texture(RID p_render_target, unsigned int p_texture_id, unsigned int p_depth_id) {}
	virtual void render_target_set_flag(RID p_render_target, RenderTargetFlags p_flag, bool p_value) {}
	virtual bool render_target_was_used(RID p_render_target) { return false; }
	virtual void render_target_clear_used(RID p_render_target) {}
	virtual void render_target_set_msaa(RID p_render_target, VS::ViewportMSAA p_msaa) {}
	virtual void render_target_set_use_fxaa(RID p_render_target, bool p_fxaa) {}
	virtual void render_target_set_use_debanding(RID p_render_target, bool p_debanding) {}
	virtual void render_target_set_sharpen_intensity(RID p_render_target, float p_intensity) {}

	/* CANVAS SHADOW / OCCLUDER (stub -- 2D dynamic light shadows dropped,
	   see proposal §5.1) */

	struct CanvasLightShadow : public RID_Data {};
	mutable RID_Owner<CanvasLightShadow> canvas_light_shadow_owner;
	virtual RID canvas_light_shadow_buffer_create(int p_width) { return canvas_light_shadow_owner.make_rid(memnew(CanvasLightShadow)); }

	struct CanvasOccluder : public RID_Data {};
	mutable RID_Owner<CanvasOccluder> canvas_occluder_owner;
	virtual RID canvas_light_occluder_create() { return canvas_occluder_owner.make_rid(memnew(CanvasOccluder)); }
	virtual void canvas_light_occluder_set_polylines(RID p_occluder, const PoolVector<Vector2> &p_lines) {}

	/* GENERIC */

	virtual VS::InstanceType get_base_type(RID p_rid) const;
	virtual bool free(RID p_rid);

	// godot-ports#22: this backend has zero GPU skinning capability at
	// all (no vertex shaders, period -- not a conditionally-constrained
	// case like GLES2's config.use_skeleton_software, which only needs
	// this on hardware with too few vertex uniform slots for bone
	// matrices). Flagging "skinning_fallback" here is the entire fix --
	// MeshInstance::_is_global_software_skinning_enabled()
	// (scene/3d/mesh_instance.cpp) checks exactly this via
	// VSG::storage->has_os_feature("skinning_fallback") and, once true,
	// its whole SoftwareSkinning mechanism re-skins vertices on the CPU
	// into a plain, ordinary ArrayMesh (bone/weight arrays stripped, no
	// skeleton ever attached to the VisualServer instance --
	// scene/3d/mesh_instance.cpp:358's render_mesh swap) that this
	// backend's existing mesh_add_surface()/render_scene() path already
	// renders correctly with zero further GLFF-side code -- confirmed by
	// reading the mechanism, not guessed.
	virtual bool has_os_feature(const String &p_feature) const {
		if (p_feature == "skinning_fallback") {
			return true;
		}
		return false;
	}
	virtual void update_dirty_resources() {}
	virtual void set_debug_generate_wireframes(bool p_generate) {}
	virtual void render_info_begin_capture() {}
	virtual void render_info_end_capture() {}
	virtual int get_captured_render_info(VS::RenderInfo p_info) { return 0; }
	virtual uint64_t get_render_info(VS::RenderInfo p_info) { return 0; }
	virtual String get_video_adapter_name() const;
	virtual String get_video_adapter_vendor() const;

	void initialize();
	void finalize();

	RasterizerStorageGLFF();
	~RasterizerStorageGLFF();
};

#endif // RASTERIZER_STORAGE_GLFF_H
