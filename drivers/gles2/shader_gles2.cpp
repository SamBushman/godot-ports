/**************************************************************************/
/*  shader_gles2.cpp                                                      */
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

#include "shader_gles2.h"

#include "core/os/memory.h"
#include "core/print_string.h"
#include "core/project_settings.h"
#include "core/string_builder.h"
#include "rasterizer_gles2.h"
#include "rasterizer_storage_gles2.h"

#ifdef __ppc__
#include "core/hash_map.h"
#include "core/set.h"
#endif

#ifdef __ppc__
#include "core/os/dir_access.h"
#include "core/os/file_access.h"
#include "core/os/os.h"
#endif

// #define DEBUG_OPENGL

// #include "shaders/copy.glsl.gen.h"

#ifdef DEBUG_OPENGL

#define DEBUG_TEST_ERROR(m_section)                                         \
	{                                                                       \
		uint32_t err = glGetError();                                        \
		if (err) {                                                          \
			print_line("OpenGL Error #" + itos(err) + " at: " + m_section); \
		}                                                                   \
	}
#else

#define DEBUG_TEST_ERROR(m_section)

#endif

ShaderGLES2 *ShaderGLES2::active = nullptr;

//#define DEBUG_SHADER

#ifdef DEBUG_SHADER

#define DEBUG_PRINT(m_text) print_line(m_text);

#else

#define DEBUG_PRINT(m_text)

#endif

GLint ShaderGLES2::get_uniform_location(int p_index) const {
	ERR_FAIL_COND_V(!version, -1);

	return version->uniform_location[p_index];
}

bool ShaderGLES2::bind() {
	if (active != this || !version || !(new_conditional_version == conditional_version)) {
		conditional_version = new_conditional_version;
		version = get_current_version();
	} else {
		return false;
	}

	ERR_FAIL_COND_V(!version, false);

	if (!version->ok) { //broken, unable to bind (do not throw error, you saw it before already when it failed compilation).
		glUseProgram(0);
		return false;
	}

	glUseProgram(version->id);

	DEBUG_TEST_ERROR("use program");

	active = this;
	uniforms_dirty = true;

	return true;
}

void ShaderGLES2::unbind() {
	version = nullptr;
	glUseProgram(0);
	uniforms_dirty = true;
	active = nullptr;
}

static void _display_error_with_code(const String &p_error, const Vector<const char *> &p_code) {
	int line = 1;
	String total_code;

	for (int i = 0; i < p_code.size(); i++) {
		total_code += String(p_code[i]);
	}

	Vector<String> lines = String(total_code).split("\n");

	for (int j = 0; j < lines.size(); j++) {
		print_line(vformat("%4d | %s", line, lines[j]));
		line++;
	}

	ERR_PRINT(p_error);
}

String ShaderGLES2::_mkid(const String &p_id) {
	String id = "m_" + p_id;
	return id.replace("__", "_dus_"); //doubleunderscore is reserved in glsl
}

#ifdef __ppc__
// Tiger's ancient ATI GLSL compiler has a real bug with nested
// #if/#ifdef/#else/#endif blocks: a declaration made in one branch of
// an outer conditional isn't reliably visible later in the same scope,
// wrongly reporting it "undeclared" (confirmed via an isolated minimal
// repro, unrelated to any Godot-specific shader logic). The GLES2
// shaders nest this way ~235 times (scene.glsl alone has 168, some
// nested up to depth 9), so hand-flattening every instance isn't
// practical -- instead, the assembled shader text is run through a
// real preprocessor before it ever reaches the driver, so no
// #if/#else/#endif survives to trigger the bug.
//
// This used to shell out to Tigerbrew's gcc-7 (`gcc-7 -E -x c -P`) to
// do this, which worked but cost a full subprocess spawn (fork/exec/
// dyld) per shader variant -- tens of seconds on this hardware for a
// cold cache -- and made the port depend on gcc-7 being installed,
// which isn't acceptable for a genuinely portable/bundled .app. The
// in-process MiniPreprocessor below replaces it: a small, scoped-down
// real C-preprocessor implementation covering exactly what this
// engine's GLES2 shaders actually use (confirmed by grepping every
// .glsl file in this tree before writing this): #define/#undef
// (object-like and function-like, including multi-line via trailing
// backslash), #if/#ifdef/#ifndef/#elif/#else/#endif gated only by
// defined(X)/!/&&/||/parens (no arithmetic, no bare non-defined()
// identifiers, no ##/# operators -- none of those appear anywhere in
// this codebase's shaders), and real macro expansion/rescanning
// (needed: e.g. `#define lowp`/`mediump`/`highp` with empty bodies to
// strip precision qualifiers for desktop GL, and real function-like
// macros like SHADOW_TEST/SHADOW_DEPTH/texture2DLod's EXT fallback).
// #include is NOT handled here because it's already resolved by the
// build-time .glsl.gen.h generator (gles_builders.py) before any of
// this runtime code ever sees the shader text.
//
// Correctness was validated, not assumed: a temporary side-by-side
// mode ran both this and the old gcc-7 path across a full editor
// session and diffed their output for every shader variant actually
// requested, with zero mismatches, before the gcc-7 path was removed.
//
// The same fixed set of variants gets requested on basically every
// editor/game launch, so the on-disk cache (keyed by an md5 of the
// pre-preprocessing source, under get_cache_path() -- ~/Library/Caches
// on macOS, survives across launches/reboots unlike /tmp) is kept even
// though it's no longer working around subprocess-spawn cost: it's a
// cheap way to skip redoing this string work every launch on a slow
// CPU, with no real downside.
static String _ppc_shader_cache_dir() {
	String dir = OS::get_singleton()->get_cache_path().plus_file("godot_ppc_shader_cache");
	DirAccess *da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
	da->make_dir_recursive(dir);
	memdelete(da);
	return dir;
}

namespace {

struct PPMacro {
	bool is_function = false;
	Vector<String> params;
	String body; // raw, unexpanded replacement text
};

_FORCE_INLINE_ static bool _pp_is_ident_start(CharType c) {
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
_FORCE_INLINE_ static bool _pp_is_ident_char(CharType c) {
	return _pp_is_ident_start(c) || (c >= '0' && c <= '9');
}

// A small, real (not naive-text-stripping) C-preprocessor subset --
// see the comment above _ppc_shader_cache_dir() for exactly what it
// covers and why. Macro expansion happens lazily at use time against
// whatever the macro table looks like at that point (matching real
// cpp semantics), with a per-expansion hide-set to prevent infinite
// self-referential recursion.
class MiniPreprocessor {
public:
	String run(const String &p_body) {
		String no_comments = _strip_comments(p_body);
		Vector<String> lines = _join_continuations(no_comments);

		Vector<CondFrame> stack;
		String out;

		for (int li = 0; li < lines.size(); li++) {
			const String &line = lines[li];
			String trimmed = line.strip_edges();
			bool active = _is_active(stack);

			if (trimmed.begins_with("#")) {
				String rest = trimmed.substr(1, trimmed.length() - 1).strip_edges();
				String directive, args_str;
				int sp = rest.find(" ");
				int tb = rest.find("\t");
				if (tb != -1 && (sp == -1 || tb < sp)) sp = tb;
				if (sp == -1) {
					directive = rest;
				} else {
					directive = rest.substr(0, sp);
					args_str = rest.substr(sp + 1, rest.length() - sp - 1).strip_edges();
				}

				if (directive == "define") {
					if (active) _do_define(args_str);
				} else if (directive == "undef") {
					if (active) macros.erase(args_str.strip_edges());
				} else if (directive == "ifdef") {
					bool taking = active && macros.has(args_str.strip_edges());
					CondFrame f;
					f.parent_active = active;
					f.branch_active = taking;
					f.any_taken = taking;
					stack.push_back(f);
				} else if (directive == "ifndef") {
					bool taking = active && !macros.has(args_str.strip_edges());
					CondFrame f;
					f.parent_active = active;
					f.branch_active = taking;
					f.any_taken = taking;
					stack.push_back(f);
				} else if (directive == "if") {
					bool taking = active && _eval_expr(args_str);
					CondFrame f;
					f.parent_active = active;
					f.branch_active = taking;
					f.any_taken = taking;
					stack.push_back(f);
				} else if (directive == "elif") {
					if (stack.size() > 0) {
						CondFrame &f = stack.write[stack.size() - 1];
						bool taking = f.parent_active && !f.any_taken && _eval_expr(args_str);
						f.branch_active = taking;
						if (taking) f.any_taken = true;
					}
				} else if (directive == "else") {
					if (stack.size() > 0) {
						CondFrame &f = stack.write[stack.size() - 1];
						bool taking = f.parent_active && !f.any_taken;
						f.branch_active = taking;
						if (taking) f.any_taken = true;
					}
				} else if (directive == "endif") {
					if (stack.size() > 0) stack.remove(stack.size() - 1);
				} else if (active) {
					// unrecognized directive (shouldn't occur in practice) -- pass through verbatim
					out += line;
					out += "\n";
				}
			} else if (active) {
				Set<String> hide_set;
				out += _expand_text(line, hide_set);
				out += "\n";
			}
		}
		return out;
	}

private:
	struct CondFrame {
		bool parent_active = false;
		bool branch_active = false;
		bool any_taken = false;
	};

	HashMap<String, PPMacro> macros;

	static bool _is_active(const Vector<CondFrame> &p_stack) {
		if (p_stack.size() == 0) return true;
		return p_stack[p_stack.size() - 1].branch_active;
	}

	static String _strip_comments(const String &p_src) {
		String out;
		int len = p_src.length();
		int i = 0;
		while (i < len) {
			CharType c = p_src[i];
			if (c == '/' && i + 1 < len && p_src[i + 1] == '/') {
				while (i < len && p_src[i] != '\n') i++;
				continue;
			}
			if (c == '/' && i + 1 < len && p_src[i + 1] == '*') {
				i += 2;
				while (i < len && !(p_src[i] == '*' && i + 1 < len && p_src[i + 1] == '/')) {
					if (p_src[i] == '\n') out += "\n";
					i++;
				}
				i += 2;
				continue;
			}
			out += c;
			i++;
		}
		return out;
	}

	static Vector<String> _join_continuations(const String &p_src) {
		Vector<String> raw_lines = p_src.split("\n", true);
		Vector<String> lines;
		String pending;
		for (int i = 0; i < raw_lines.size(); i++) {
			const String &l = raw_lines[i];
			if (l.ends_with("\\")) {
				pending += l.substr(0, l.length() - 1);
				continue;
			}
			pending += l;
			lines.push_back(pending);
			pending = "";
		}
		if (pending.length() > 0) lines.push_back(pending);
		return lines;
	}

	void _do_define(const String &p_args) {
		int len = p_args.length();
		int i = 0;
		int start = i;
		while (i < len && _pp_is_ident_char(p_args[i])) i++;
		String name = p_args.substr(start, i - start);
		if (name.length() == 0) return;

		PPMacro m;
		if (i < len && p_args[i] == '(') {
			m.is_function = true;
			i++;
			while (i < len && p_args[i] != ')') {
				while (i < len && (p_args[i] == ' ' || p_args[i] == ',' || p_args[i] == '\t')) i++;
				int ps = i;
				while (i < len && _pp_is_ident_char(p_args[i])) i++;
				if (i > ps) m.params.push_back(p_args.substr(ps, i - ps));
				while (i < len && (p_args[i] == ' ' || p_args[i] == '\t')) i++;
			}
			if (i < len && p_args[i] == ')') i++;
			while (i < len && (p_args[i] == ' ' || p_args[i] == '\t')) i++;
			m.body = p_args.substr(i, len - i);
		} else {
			while (i < len && (p_args[i] == ' ' || p_args[i] == '\t')) i++;
			m.body = p_args.substr(i, len - i);
		}
		macros.set(name, m);
	}

	static String _substitute_params(const String &p_body, const HashMap<String, String> &p_args) {
		String out;
		int len = p_body.length();
		int i = 0;
		while (i < len) {
			CharType c = p_body[i];
			if (_pp_is_ident_start(c)) {
				int start = i;
				while (i < len && _pp_is_ident_char(p_body[i])) i++;
				String word = p_body.substr(start, i - start);
				const String *v = p_args.getptr(word);
				out += v ? *v : word;
				continue;
			}
			out += c;
			i++;
		}
		return out;
	}

	String _expand_text(const String &p_text, const Set<String> &p_hide_set) {
		String out;
		int len = p_text.length();
		int i = 0;
		while (i < len) {
			CharType c = p_text[i];
			if (_pp_is_ident_start(c)) {
				int start = i;
				while (i < len && _pp_is_ident_char(p_text[i])) i++;
				String word = p_text.substr(start, i - start);

				const PPMacro *m = p_hide_set.has(word) ? nullptr : macros.getptr(word);
				if (!m) {
					out += word;
					continue;
				}

				if (!m->is_function) {
					Set<String> new_hide = p_hide_set;
					new_hide.insert(word);
					out += _expand_text(m->body, new_hide);
					continue;
				}

				// function-like: only invoked if immediately (modulo
				// whitespace) followed by '('
				int j = i;
				while (j < len && (p_text[j] == ' ' || p_text[j] == '\t')) j++;
				if (j >= len || p_text[j] != '(') {
					out += word;
					continue;
				}
				j++;
				Vector<String> args;
				String cur_arg;
				int depth = 1;
				while (j < len && depth > 0) {
					CharType cc = p_text[j];
					if (cc == '(') {
						depth++;
						cur_arg += cc;
						j++;
					} else if (cc == ')') {
						depth--;
						j++;
						if (depth == 0) break;
						cur_arg += cc;
					} else if (cc == ',' && depth == 1) {
						args.push_back(cur_arg);
						cur_arg = "";
						j++;
					} else {
						cur_arg += cc;
						j++;
					}
				}
				if (m->params.size() > 0 || cur_arg.strip_edges().length() > 0 || args.size() > 0) {
					args.push_back(cur_arg);
				}

				HashMap<String, String> argmap;
				for (int k = 0; k < m->params.size() && k < args.size(); k++) {
					Set<String> arg_hide = p_hide_set;
					argmap.set(m->params[k], _expand_text(args[k], arg_hide));
				}
				String substituted = _substitute_params(m->body, argmap);
				Set<String> new_hide = p_hide_set;
				new_hide.insert(word);
				out += _expand_text(substituted, new_hide);
				i = j;
				continue;
			}
			out += c;
			i++;
		}
		return out;
	}

	static Vector<String> _tokenize_expr(const String &p_expr) {
		Vector<String> toks;
		int len = p_expr.length();
		int i = 0;
		while (i < len) {
			CharType c = p_expr[i];
			if (c == ' ' || c == '\t') {
				i++;
				continue;
			}
			if (_pp_is_ident_start(c)) {
				int start = i;
				while (i < len && _pp_is_ident_char(p_expr[i])) i++;
				toks.push_back(p_expr.substr(start, i - start));
				continue;
			}
			if (c == '(' || c == ')' || c == '!') {
				toks.push_back(String::chr(c));
				i++;
				continue;
			}
			if (c == '&' && i + 1 < len && p_expr[i + 1] == '&') {
				toks.push_back("&&");
				i += 2;
				continue;
			}
			if (c == '|' && i + 1 < len && p_expr[i + 1] == '|') {
				toks.push_back("||");
				i += 2;
				continue;
			}
			i++; // unrecognized char (e.g. stray punctuation) -- skip
		}
		return toks;
	}

	// tiny recursive-descent parser: or_expr := and_expr ('||' and_expr)*
	// and_expr := unary ('&&' unary)* ; unary := '!' unary | primary
	// primary := '(' or_expr ')' | 'defined' ['('] IDENT [')'] | IDENT
	struct ExprParser {
		const Vector<String> &toks;
		const HashMap<String, PPMacro> &macros;
		int pos = 0;

		String peek() const { return pos < toks.size() ? toks[pos] : String(); }
		String advance() { return pos < toks.size() ? toks[pos++] : String(); }

		bool parse_or() {
			bool v = parse_and();
			while (peek() == "||") {
				advance();
				bool r = parse_and();
				v = v || r;
			}
			return v;
		}
		bool parse_and() {
			bool v = parse_unary();
			while (peek() == "&&") {
				advance();
				bool r = parse_unary();
				v = v && r;
			}
			return v;
		}
		bool parse_unary() {
			if (peek() == "!") {
				advance();
				return !parse_unary();
			}
			return parse_primary();
		}
		bool parse_primary() {
			if (peek() == "(") {
				advance();
				bool v = parse_or();
				if (peek() == ")") advance();
				return v;
			}
			if (peek() == "defined") {
				advance();
				bool paren = false;
				if (peek() == "(") {
					paren = true;
					advance();
				}
				String name = advance();
				if (paren && peek() == ")") advance();
				return macros.has(name);
			}
			String name = advance();
			if (name.length() == 0) return false;
			// bare identifier in #if context: not used anywhere in this
			// codebase's shaders (confirmed by inspection), but handle it
			// reasonably rather than asserting -- macro-expand and treat
			// a "0"/empty result as false, anything else defined as true.
			const PPMacro *m = macros.getptr(name);
			if (!m) return false;
			String v = m->body.strip_edges();
			return v != "0";
		}
	};

	bool _eval_expr(const String &p_expr) {
		Vector<String> toks = _tokenize_expr(p_expr);
		ExprParser parser{ toks, macros };
		return parser.parse_or();
	}
};

} // namespace

static CharString _preprocess_shader_ppc(const Vector<const char *> &p_strings) {
	String version_line;
	String body;
	for (int i = 0; i < p_strings.size(); i++) {
		String s = String::utf8(p_strings[i]);
		if (s.begins_with("#version")) {
			version_line = s;
		} else {
			body += s;
		}
	}

	String cache_path = _ppc_shader_cache_dir().plus_file(body.md5_text() + ".glsl");
	{
		FileAccessRef cf = FileAccess::open(cache_path, FileAccess::READ);
		if (cf) {
			String cached = cf->get_as_utf8_string();
			return (version_line + "\n" + cached).utf8();
		}
	}

	MiniPreprocessor pp;
	String output = pp.run(body);
	ERR_FAIL_COND_V_MSG(output.length() == 0, version_line.utf8(), "ppc in-process shader preprocessing produced no output for a non-empty shader body");

	// The ATI Radeon X1900/Tiger driver's GLSL compiler can silently miscompute
	// an in-shader mat4*mat4 multiply for certain non-trivial matrices, even
	// when one runtime operand happens to be identity -- the compiler can't see
	// that at compile time (it's a uniform, not a literal), so it still emits
	// the same generic multiply codegen either way, and that codegen is what's
	// unreliable for some operand value patterns (see
	// ATI_RADEON_X1900_TIGER_DRIVER_QUIRKS.md quirk #12; confirmed via a
	// standalone repro that this can produce a fully degenerate/invisible
	// result -- not just deformation -- for specific real matrices, e.g. one
	// of the two player.glb leg transforms). rasterizer_scene_gles2.cpp
	// already uploads world_transform as the fully CPU-precomputed modelview
	// and camera_inverse_matrix as identity on this platform, so the in-shader
	// combine is redundant math the driver can still get wrong -- strip it
	// from the compiled source entirely rather than relying on the runtime
	// identity value alone. Safe for every scene.glsl vertex-shader variant:
	// this line is computed unconditionally but only ever READ when
	// VERTEX_WORLD_COORDS_USED is not defined (see scene.glsl), so making it
	// an alias for world_matrix instead of a product is a no-op everywhere
	// else. camera_inverse_matrix itself is untouched -- a separate,
	// unconditional mat4*vec4 codepath still uses it directly when
	// VERTEX_WORLD_COORDS_USED IS defined, which isn't the pattern this quirk
	// was ever observed to affect.
	output = output.replace("mat4 modelview = camera_inverse_matrix * world_matrix;", "mat4 modelview = world_matrix;");

	{
		FileAccessRef cf = FileAccess::open(cache_path, FileAccess::WRITE);
		if (cf) {
			cf->store_string(output);
		}
	}

	String result = version_line + "\n" + output;
	return result.utf8();
}
#endif

ShaderGLES2::Version *ShaderGLES2::get_current_version() {
	Version *_v = version_map.getptr(conditional_version);

	if (_v) {
		if (conditional_version.code_version != 0) {
			CustomCode *cc = custom_code_map.getptr(conditional_version.code_version);
			ERR_FAIL_COND_V(!cc, _v);
			if (cc->version == _v->code_version) {
				return _v;
			}
		} else {
			return _v;
		}
	}

	if (!_v) {
		version_map[conditional_version] = Version();
	}

	Version &v = version_map[conditional_version];

	if (!_v) {
		v.uniform_location = memnew_arr(GLint, uniform_count);
	} else {
		if (v.ok) {
			glDeleteShader(v.vert_id);
			glDeleteShader(v.frag_id);
			glDeleteProgram(v.id);
			v.id = 0;
		}
	}

	v.ok = false;

	Vector<const char *> strings;

#ifdef GLES_OVER_GL
#ifdef __ppc__
	// Tiger's driver for this GPU generation caps out at GLSL 1.10
	// (confirmed live: GL_SHADING_LANGUAGE_VERSION reports "1.10", and
	// the compiler flatly rejects "#version 120" with "Version number
	// not supported by GL2" while "#version 110" compiles fine).
	strings.push_back("#version 110\n");
	// GLSL 1.10 has no transpose() builtin at all (added in 1.20) -- unlike
	// stock desktop GL, this driver enforces that strictly (see issue #10).
	// Signal stdlib.glsl to compile the transpose() polyfills even though
	// USE_GLES_OVER_GL is set.
	strings.push_back("#define GLES_OVER_GL_GLSL110\n");
#else
	strings.push_back("#version 120\n");
#endif
	strings.push_back("#define USE_GLES_OVER_GL\n");
#else
	strings.push_back("#version 100\n");
//angle does not like
#ifdef JAVASCRIPT_ENABLED
	strings.push_back("#define USE_HIGHP_PRECISION\n");
#endif

	if (GLOBAL_GET("rendering/gles2/compatibility/enable_high_float.Android")) {
		// enable USE_HIGHP_PRECISION but safeguarded by an availability check as highp support is optional in GLES2
		// see Section 4.5.4 of the GLSL_ES_Specification_1.00
		strings.push_back("#ifdef GL_FRAGMENT_PRECISION_HIGH\n  #define USE_HIGHP_PRECISION\n#endif\n");
	}

#endif

#ifdef ANDROID_ENABLED
	strings.push_back("#define ANDROID_ENABLED\n");
#endif

	for (int i = 0; i < custom_defines.size(); i++) {
		strings.push_back(custom_defines[i].get_data());
		strings.push_back("\n");
	}

	for (int j = 0; j < conditional_count; j++) {
		bool enable = (conditional_version.version & (uint64_t(1) << j)) > 0;

		if (enable) {
			strings.push_back(conditional_defines[j]);
			DEBUG_PRINT(conditional_defines[j]);
		}
	}

	// keep them around during the function
	CharString code_string;
	CharString code_string2;
	CharString code_globals;

	CustomCode *cc = nullptr;

	if (conditional_version.code_version > 0) {
		cc = custom_code_map.getptr(conditional_version.code_version);

		ERR_FAIL_COND_V(!cc, nullptr);
		v.code_version = cc->version;
	}

	// program

	v.id = glCreateProgram();
	ERR_FAIL_COND_V(v.id == 0, nullptr);

	if (cc) {
		for (int i = 0; i < cc->custom_defines.size(); i++) {
			strings.push_back(cc->custom_defines.write[i]);
			DEBUG_PRINT("CD #" + itos(i) + ": " + String(cc->custom_defines[i].get_data()));
		}
	}

	// vertex shader

	int string_base_size = strings.size();

	strings.push_back(vertex_code0.get_data());

	if (cc) {
		code_globals = cc->vertex_globals.ascii();
		strings.push_back(code_globals.get_data());
	}

	strings.push_back(vertex_code1.get_data());

	if (cc) {
		code_string = cc->vertex.ascii();
		strings.push_back(code_string.get_data());
	}

	strings.push_back(vertex_code2.get_data());

#ifdef DEBUG_SHADER

	DEBUG_PRINT("\nVertex Code:\n\n" + String(code_string.get_data()));

#endif

	v.vert_id = glCreateShader(GL_VERTEX_SHADER);
#ifdef __ppc__
	{
		CharString pp = _preprocess_shader_ppc(strings);
		const char *pp_ptr = pp.get_data();
		glShaderSource(v.vert_id, 1, &pp_ptr, nullptr);
	}
#else
	glShaderSource(v.vert_id, strings.size(), (const GLchar**) &strings[0], nullptr);
#endif
	glCompileShader(v.vert_id);

	GLint status;

	glGetShaderiv(v.vert_id, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		GLsizei iloglen;
		glGetShaderiv(v.vert_id, GL_INFO_LOG_LENGTH, &iloglen);

		if (iloglen < 0) {
			glDeleteShader(v.vert_id);
			glDeleteProgram(v.id);
			v.id = 0;

			ERR_PRINT("No OpenGL vertex shader compiler log. What the frick?");
		} else {
			if (iloglen == 0) {
				iloglen = 4096; // buggy driver (Adreno 220+)
			}

			char *ilogmem = (char *)Memory::alloc_static(iloglen + 1);
			ilogmem[iloglen] = '\0';
			glGetShaderInfoLog(v.vert_id, iloglen, &iloglen, ilogmem);

			String err_string = get_shader_name() + ": Vertex shader compilation failed:\n";

			err_string += ilogmem;

			_display_error_with_code(err_string, strings);

			Memory::free_static(ilogmem);
			glDeleteShader(v.vert_id);
			glDeleteProgram(v.id);
			v.id = 0;
		}

		ERR_FAIL_V(nullptr);
	}

	strings.resize(string_base_size);

	// fragment shader

	strings.push_back(fragment_code0.get_data());

	if (cc) {
		code_globals = cc->fragment_globals.ascii();
		strings.push_back(code_globals.get_data());
	}

	strings.push_back(fragment_code1.get_data());

	if (cc) {
		code_string = cc->light.ascii();
		strings.push_back(code_string.get_data());
	}

	strings.push_back(fragment_code2.get_data());

	if (cc) {
		code_string2 = cc->fragment.ascii();
		strings.push_back(code_string2.get_data());
	}

	strings.push_back(fragment_code3.get_data());

#ifdef DEBUG_SHADER

	if (cc) {
		DEBUG_PRINT("\nFragment Code:\n\n" + String(cc->fragment_globals));
	}
	DEBUG_PRINT("\nFragment Code:\n\n" + String(code_string.get_data()));
#endif

	v.frag_id = glCreateShader(GL_FRAGMENT_SHADER);
#ifdef __ppc__
	{
		CharString pp = _preprocess_shader_ppc(strings);
		const char *pp_ptr = pp.get_data();
		glShaderSource(v.frag_id, 1, &pp_ptr, nullptr);
	}
#else
	glShaderSource(v.frag_id, strings.size(), (const GLchar**) &strings[0], nullptr);
#endif
	glCompileShader(v.frag_id);

	glGetShaderiv(v.frag_id, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		GLsizei iloglen;
		glGetShaderiv(v.frag_id, GL_INFO_LOG_LENGTH, &iloglen);

		if (iloglen < 0) {
			glDeleteShader(v.frag_id);
			glDeleteShader(v.vert_id);
			glDeleteProgram(v.id);
			v.id = 0;

			ERR_PRINT("No OpenGL fragment shader compiler log. What the frick?");
		} else {
			if (iloglen == 0) {
				iloglen = 4096; // buggy driver (Adreno 220+)
			}

			char *ilogmem = (char *)Memory::alloc_static(iloglen + 1);
			ilogmem[iloglen] = '\0';
			glGetShaderInfoLog(v.frag_id, iloglen, &iloglen, ilogmem);

			String err_string = get_shader_name() + ": Fragment shader compilation failed:\n";

			err_string += ilogmem;

			_display_error_with_code(err_string, strings);

			Memory::free_static(ilogmem);
			glDeleteShader(v.frag_id);
			glDeleteShader(v.vert_id);
			glDeleteProgram(v.id);
			v.id = 0;
		}

		ERR_FAIL_V(nullptr);
	}

	glAttachShader(v.id, v.frag_id);
	glAttachShader(v.id, v.vert_id);

	// bind the attribute locations. This has to be done before linking so that the
	// linker doesn't assign some random indices

	for (int i = 0; i < attribute_pair_count; i++) {
		glBindAttribLocation(v.id, attribute_pairs[i].index, attribute_pairs[i].name);
	}

	glLinkProgram(v.id);

	glGetProgramiv(v.id, GL_LINK_STATUS, &status);
	if (status == GL_FALSE) {
		GLsizei iloglen;
		glGetProgramiv(v.id, GL_INFO_LOG_LENGTH, &iloglen);

		if (iloglen < 0) {
			glDeleteShader(v.frag_id);
			glDeleteShader(v.vert_id);
			glDeleteProgram(v.id);
			v.id = 0;

			ERR_PRINT("No OpenGL program link log. What the frick?");
			ERR_FAIL_V(nullptr);
		}

		if (iloglen == 0) {
			iloglen = 4096; // buggy driver (Adreno 220+)
		}

		char *ilogmem = (char *)Memory::alloc_static(iloglen + 1);
		ilogmem[iloglen] = '\0';
		glGetProgramInfoLog(v.id, iloglen, &iloglen, ilogmem);

		String err_string = get_shader_name() + ": Program linking failed:\n";

		err_string += ilogmem;

		_display_error_with_code(err_string, strings);

		Memory::free_static(ilogmem);
		glDeleteShader(v.frag_id);
		glDeleteShader(v.vert_id);
		glDeleteProgram(v.id);
		v.id = 0;

		ERR_FAIL_V(nullptr);
	}

	// get uniform locations

	glUseProgram(v.id);

	for (int i = 0; i < uniform_count; i++) {
		v.uniform_location[i] = glGetUniformLocation(v.id, uniform_names[i]);
	}

	for (int i = 0; i < texunit_pair_count; i++) {
		GLint loc = glGetUniformLocation(v.id, texunit_pairs[i].name);
		if (loc >= 0) {
			if (texunit_pairs[i].index < 0) {
				glUniform1i(loc, max_image_units + texunit_pairs[i].index);
			} else {
				glUniform1i(loc, texunit_pairs[i].index);
			}
		}
	}

	if (cc) {
		// uniforms
		for (int i = 0; i < cc->custom_uniforms.size(); i++) {
			String native_uniform_name = _mkid(cc->custom_uniforms[i]);
			GLint location = glGetUniformLocation(v.id, (native_uniform_name).ascii().get_data());
			v.custom_uniform_locations[cc->custom_uniforms[i]] = location;
		}

		// textures
		for (int i = 0; i < cc->texture_uniforms.size(); i++) {
			String native_uniform_name = _mkid(cc->texture_uniforms[i]);
			GLint location = glGetUniformLocation(v.id, (native_uniform_name).ascii().get_data());
			v.custom_uniform_locations[cc->texture_uniforms[i]] = location;
			glUniform1i(location, i);
		}
	}

	glUseProgram(0);
	v.ok = true;

	if (cc) {
		cc->versions.insert(conditional_version.version);
	}

	return &v;
}

GLint ShaderGLES2::get_uniform_location(const String &p_name) const {
	ERR_FAIL_COND_V(!version, -1);
	return glGetUniformLocation(version->id, p_name.ascii().get_data());
}

void ShaderGLES2::setup(
		const char **p_conditional_defines,
		int p_conditional_count,
		const char **p_uniform_names,
		int p_uniform_count,
		const AttributePair *p_attribute_pairs,
		int p_attribute_count,
		const TexUnitPair *p_texunit_pairs,
		int p_texunit_pair_count,
		const char *p_vertex_code,
		const char *p_fragment_code,
		int p_vertex_code_start,
		int p_fragment_code_start) {
	ERR_FAIL_COND(version);

	memset(conditional_version.key, 0, sizeof(conditional_version.key));
	memset(new_conditional_version.key, 0, sizeof(new_conditional_version.key));
	uniform_count = p_uniform_count;
	conditional_count = p_conditional_count;
	conditional_defines = p_conditional_defines;
	uniform_names = p_uniform_names;
	vertex_code = p_vertex_code;
	fragment_code = p_fragment_code;
	texunit_pairs = p_texunit_pairs;
	texunit_pair_count = p_texunit_pair_count;
	vertex_code_start = p_vertex_code_start;
	fragment_code_start = p_fragment_code_start;
	attribute_pairs = p_attribute_pairs;
	attribute_pair_count = p_attribute_count;

	{
		String globals_tag = "\nVERTEX_SHADER_GLOBALS";
		String code_tag = "\nVERTEX_SHADER_CODE";
		String code = vertex_code;
		int cpos = code.find(globals_tag);
		if (cpos == -1) {
			vertex_code0 = code.ascii();
		} else {
			vertex_code0 = code.substr(0, cpos).ascii();
			code = code.substr(cpos + globals_tag.length(), code.length());

			cpos = code.find(code_tag);

			if (cpos == -1) {
				vertex_code1 = code.ascii();
			} else {
				vertex_code1 = code.substr(0, cpos).ascii();
				vertex_code2 = code.substr(cpos + code_tag.length(), code.length()).ascii();
			}
		}
	}

	{
		String globals_tag = "\nFRAGMENT_SHADER_GLOBALS";
		String code_tag = "\nFRAGMENT_SHADER_CODE";
		String light_code_tag = "\nLIGHT_SHADER_CODE";
		String code = fragment_code;
		int cpos = code.find(globals_tag);
		if (cpos == -1) {
			fragment_code0 = code.ascii();
		} else {
			fragment_code0 = code.substr(0, cpos).ascii();
			code = code.substr(cpos + globals_tag.length(), code.length());

			cpos = code.find(light_code_tag);

			String code2;

			if (cpos != -1) {
				fragment_code1 = code.substr(0, cpos).ascii();
				code2 = code.substr(cpos + light_code_tag.length(), code.length());
			} else {
				code2 = code;
			}

			cpos = code2.find(code_tag);
			if (cpos == -1) {
				fragment_code2 = code2.ascii();
			} else {
				fragment_code2 = code2.substr(0, cpos).ascii();
				fragment_code3 = code2.substr(cpos + code_tag.length(), code2.length()).ascii();
			}
		}
	}

	// The upper limit must match the version used in storage.
	max_image_units = RasterizerStorageGLES2::safe_gl_get_integer(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, RasterizerStorageGLES2::Config::max_desired_texture_image_units);
}

void ShaderGLES2::finish() {
	const VersionKey *V = nullptr;

	while ((V = version_map.next(V))) {
		Version &v = version_map[*V];
		glDeleteShader(v.vert_id);
		glDeleteShader(v.frag_id);
		glDeleteProgram(v.id);
		memdelete_arr(v.uniform_location);
	}
}

void ShaderGLES2::clear_caches() {
	const VersionKey *V = nullptr;

	while ((V = version_map.next(V))) {
		Version &v = version_map[*V];
		glDeleteShader(v.vert_id);
		glDeleteShader(v.frag_id);
		glDeleteProgram(v.id);
		memdelete_arr(v.uniform_location);
	}

	version_map.clear();

	custom_code_map.clear();
	version = nullptr;
	last_custom_code = 1;
	uniforms_dirty = true;
}

uint32_t ShaderGLES2::create_custom_shader() {
	custom_code_map[last_custom_code] = CustomCode();
	custom_code_map[last_custom_code].version = 1;
	return last_custom_code++;
}

void ShaderGLES2::set_custom_shader_code(uint32_t p_code_id,
		const String &p_vertex,
		const String &p_vertex_globals,
		const String &p_fragment,
		const String &p_light,
		const String &p_fragment_globals,
		const Vector<StringName> &p_uniforms,
		const Vector<StringName> &p_texture_uniforms,
		const Vector<CharString> &p_custom_defines) {
	CustomCode *cc = custom_code_map.getptr(p_code_id);
	ERR_FAIL_COND(!cc);

	cc->vertex = p_vertex;
	cc->vertex_globals = p_vertex_globals;
	cc->fragment = p_fragment;
	cc->fragment_globals = p_fragment_globals;
	cc->light = p_light;
	cc->custom_uniforms = p_uniforms;
	cc->custom_defines = p_custom_defines;
	cc->texture_uniforms = p_texture_uniforms;
	cc->version++;
}

void ShaderGLES2::set_custom_shader(uint32_t p_code_id) {
	new_conditional_version.code_version = p_code_id;
}

void ShaderGLES2::free_custom_shader(uint32_t p_code_id) {
	ERR_FAIL_COND(!custom_code_map.has(p_code_id));
	if (conditional_version.code_version == p_code_id) {
		conditional_version.code_version = 0; //do not keep using a version that is going away
		unbind();
	}

	VersionKey key;
	key.code_version = p_code_id;
	for (Set<uint64_t>::Element *E = custom_code_map[p_code_id].versions.front(); E; E = E->next()) {
		key.version = E->get();
		ERR_CONTINUE(!version_map.has(key));
		Version &v = version_map[key];

		glDeleteShader(v.vert_id);
		glDeleteShader(v.frag_id);
		glDeleteProgram(v.id);
		memdelete_arr(v.uniform_location);
		v.id = 0;

		version_map.erase(key);
	}

	custom_code_map.erase(p_code_id);
}

void ShaderGLES2::use_material(void *p_material) {
	RasterizerStorageGLES2::Material *material = (RasterizerStorageGLES2::Material *)p_material;

	if (!material) {
		return;
	}

	if (!material->shader) {
		return;
	}

	Version *v = version_map.getptr(conditional_version);

	// bind uniforms
	for (Map<StringName, ShaderLanguage::ShaderNode::Uniform>::Element *E = material->shader->uniforms.front(); E; E = E->next()) {
		if (E->get().texture_order >= 0) {
			continue; // this is a texture, doesn't go here
		}

		Map<StringName, GLint>::Element *L = v->custom_uniform_locations.find(E->key());
		if (!L || L->get() < 0) {
			continue; //uniform not valid
		}

		GLuint location = L->get();

		Map<StringName, Variant>::Element *V = material->params.find(E->key());

		if (V) {
			switch (E->get().type) {
				case ShaderLanguage::TYPE_BOOL: {
					bool boolean = V->get();
					glUniform1i(location, boolean ? 1 : 0);
				} break;

				case ShaderLanguage::TYPE_BVEC2: {
					int flags = V->get();
					glUniform2i(location, (flags & 1) ? 1 : 0, (flags & 2) ? 1 : 0);
				} break;

				case ShaderLanguage::TYPE_BVEC3: {
					int flags = V->get();
					glUniform3i(location, (flags & 1) ? 1 : 0, (flags & 2) ? 1 : 0, (flags & 4) ? 1 : 0);

				} break;

				case ShaderLanguage::TYPE_BVEC4: {
					int flags = V->get();
					glUniform4i(location, (flags & 1) ? 1 : 0, (flags & 2) ? 1 : 0, (flags & 4) ? 1 : 0, (flags & 8) ? 1 : 0);

				} break;

				case ShaderLanguage::TYPE_INT:
				case ShaderLanguage::TYPE_UINT: {
					int value = V->get();
					glUniform1i(location, value);
				} break;

				case ShaderLanguage::TYPE_IVEC2:
				case ShaderLanguage::TYPE_UVEC2: {
					Array r = V->get();
					const int count = 2;
					if (r.size() == count) {
						int values[count];
						for (int i = 0; i < count; i++) {
							values[i] = r[i];
						}
						glUniform2i(location, values[0], values[1]);
					}

				} break;

				case ShaderLanguage::TYPE_IVEC3:
				case ShaderLanguage::TYPE_UVEC3: {
					Array r = V->get();
					const int count = 3;
					if (r.size() == count) {
						int values[count];
						for (int i = 0; i < count; i++) {
							values[i] = r[i];
						}
						glUniform3i(location, values[0], values[1], values[2]);
					}

				} break;

				case ShaderLanguage::TYPE_IVEC4:
				case ShaderLanguage::TYPE_UVEC4: {
					Array r = V->get();
					const int count = 4;
					if (r.size() == count) {
						int values[count];
						for (int i = 0; i < count; i++) {
							values[i] = r[i];
						}
						glUniform4i(location, values[0], values[1], values[2], values[3]);
					}

				} break;

				case ShaderLanguage::TYPE_FLOAT: {
					float value = V->get();
					glUniform1f(location, value);

				} break;

				case ShaderLanguage::TYPE_VEC2: {
					Vector2 value = V->get();
					glUniform2f(location, value.x, value.y);
				} break;

				case ShaderLanguage::TYPE_VEC3: {
					Vector3 value = V->get();
					glUniform3f(location, value.x, value.y, value.z);
				} break;

				case ShaderLanguage::TYPE_VEC4: {
					if (V->get().get_type() == Variant::COLOR) {
						Color value = V->get();
						glUniform4f(location, value.r, value.g, value.b, value.a);
					} else if (V->get().get_type() == Variant::QUAT) {
						Quat value = V->get();
						glUniform4f(location, value.x, value.y, value.z, value.w);
					} else {
						Plane value = V->get();
						glUniform4f(location, value.normal.x, value.normal.y, value.normal.z, value.d);
					}

				} break;

				case ShaderLanguage::TYPE_MAT2: {
					Transform2D tr = V->get();
					GLfloat matrix[4] = {
						/* build a 16x16 matrix */
						tr.elements[0][0],
						tr.elements[0][1],
						tr.elements[1][0],
						tr.elements[1][1],
					};
					glUniformMatrix2fv(location, 1, GL_FALSE, matrix);

				} break;

				case ShaderLanguage::TYPE_MAT3: {
					Basis val = V->get();

					GLfloat mat[9] = {
						val.elements[0][0],
						val.elements[1][0],
						val.elements[2][0],
						val.elements[0][1],
						val.elements[1][1],
						val.elements[2][1],
						val.elements[0][2],
						val.elements[1][2],
						val.elements[2][2],
					};

					glUniformMatrix3fv(location, 1, GL_FALSE, mat);

				} break;

				case ShaderLanguage::TYPE_MAT4: {
					if (V->get().get_type() == Variant::TRANSFORM) {
						Transform tr = V->get();
						GLfloat matrix[16] = { /* build a 16x16 matrix */
							tr.basis.elements[0][0],
							tr.basis.elements[1][0],
							tr.basis.elements[2][0],
							0,
							tr.basis.elements[0][1],
							tr.basis.elements[1][1],
							tr.basis.elements[2][1],
							0,
							tr.basis.elements[0][2],
							tr.basis.elements[1][2],
							tr.basis.elements[2][2],
							0,
							tr.origin.x,
							tr.origin.y,
							tr.origin.z,
							1
						};
						glUniformMatrix4fv(location, 1, GL_FALSE, matrix);
					} else {
						Transform2D tr = V->get();
						GLfloat matrix[16] = { /* build a 16x16 matrix */
							tr.elements[0][0],
							tr.elements[0][1],
							0,
							0,
							tr.elements[1][0],
							tr.elements[1][1],
							0,
							0,
							0,
							0,
							1,
							0,
							tr.elements[2][0],
							tr.elements[2][1],
							0,
							1
						};
						glUniformMatrix4fv(location, 1, GL_FALSE, matrix);
					}

				} break;

				default: {
					ERR_PRINT("ShaderNode type missing, bug?");
				} break;
			}
		} else if (E->get().default_value.size()) {
			const Vector<ShaderLanguage::ConstantNode::Value> &values = E->get().default_value;
			switch (E->get().type) {
				case ShaderLanguage::TYPE_BOOL: {
					glUniform1i(location, values[0].boolean);
				} break;

				case ShaderLanguage::TYPE_BVEC2: {
					glUniform2i(location, values[0].boolean, values[1].boolean);
				} break;

				case ShaderLanguage::TYPE_BVEC3: {
					glUniform3i(location, values[0].boolean, values[1].boolean, values[2].boolean);
				} break;

				case ShaderLanguage::TYPE_BVEC4: {
					glUniform4i(location, values[0].boolean, values[1].boolean, values[2].boolean, values[3].boolean);
				} break;

				case ShaderLanguage::TYPE_INT: {
					glUniform1i(location, values[0].sint);
				} break;

				case ShaderLanguage::TYPE_IVEC2: {
					glUniform2i(location, values[0].sint, values[1].sint);
				} break;

				case ShaderLanguage::TYPE_IVEC3: {
					glUniform3i(location, values[0].sint, values[1].sint, values[2].sint);
				} break;

				case ShaderLanguage::TYPE_IVEC4: {
					glUniform4i(location, values[0].sint, values[1].sint, values[2].sint, values[3].sint);
				} break;

				case ShaderLanguage::TYPE_UINT: {
					glUniform1i(location, values[0].uint);
				} break;

				case ShaderLanguage::TYPE_UVEC2: {
					glUniform2i(location, values[0].uint, values[1].uint);
				} break;

				case ShaderLanguage::TYPE_UVEC3: {
					glUniform3i(location, values[0].uint, values[1].uint, values[2].uint);
				} break;

				case ShaderLanguage::TYPE_UVEC4: {
					glUniform4i(location, values[0].uint, values[1].uint, values[2].uint, values[3].uint);
				} break;

				case ShaderLanguage::TYPE_FLOAT: {
					glUniform1f(location, values[0].real);
				} break;

				case ShaderLanguage::TYPE_VEC2: {
					glUniform2f(location, values[0].real, values[1].real);
				} break;

				case ShaderLanguage::TYPE_VEC3: {
					glUniform3f(location, values[0].real, values[1].real, values[2].real);
				} break;

				case ShaderLanguage::TYPE_VEC4: {
					glUniform4f(location, values[0].real, values[1].real, values[2].real, values[3].real);
				} break;

				case ShaderLanguage::TYPE_MAT2: {
					GLfloat mat[4];

					for (int i = 0; i < 4; i++) {
						mat[i] = values[i].real;
					}

					glUniformMatrix2fv(location, 1, GL_FALSE, mat);
				} break;

				case ShaderLanguage::TYPE_MAT3: {
					GLfloat mat[9];

					for (int i = 0; i < 9; i++) {
						mat[i] = values[i].real;
					}

					glUniformMatrix3fv(location, 1, GL_FALSE, mat);

				} break;

				case ShaderLanguage::TYPE_MAT4: {
					GLfloat mat[16];

					for (int i = 0; i < 16; i++) {
						mat[i] = values[i].real;
					}

					glUniformMatrix4fv(location, 1, GL_FALSE, mat);

				} break;

				case ShaderLanguage::TYPE_SAMPLER2D: {
				} break;

				case ShaderLanguage::TYPE_SAMPLEREXT: {
				} break;

				case ShaderLanguage::TYPE_ISAMPLER2D: {
				} break;

				case ShaderLanguage::TYPE_USAMPLER2D: {
				} break;

				case ShaderLanguage::TYPE_SAMPLERCUBE: {
				} break;

				case ShaderLanguage::TYPE_SAMPLER2DARRAY:
				case ShaderLanguage::TYPE_ISAMPLER2DARRAY:
				case ShaderLanguage::TYPE_USAMPLER2DARRAY:
				case ShaderLanguage::TYPE_SAMPLER3D:
				case ShaderLanguage::TYPE_ISAMPLER3D:
				case ShaderLanguage::TYPE_USAMPLER3D: {
					// Not implemented in GLES2
				} break;

				case ShaderLanguage::TYPE_VOID: {
					// Nothing to do?
				} break;
				default: {
					ERR_PRINT("ShaderNode type missing, bug?");
				} break;
			}
		} else { //zero

			switch (E->get().type) {
				case ShaderLanguage::TYPE_BOOL: {
					glUniform1i(location, GL_FALSE);
				} break;

				case ShaderLanguage::TYPE_BVEC2: {
					glUniform2i(location, GL_FALSE, GL_FALSE);
				} break;

				case ShaderLanguage::TYPE_BVEC3: {
					glUniform3i(location, GL_FALSE, GL_FALSE, GL_FALSE);
				} break;

				case ShaderLanguage::TYPE_BVEC4: {
					glUniform4i(location, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				} break;

				case ShaderLanguage::TYPE_INT: {
					glUniform1i(location, 0);
				} break;

				case ShaderLanguage::TYPE_IVEC2: {
					glUniform2i(location, 0, 0);
				} break;

				case ShaderLanguage::TYPE_IVEC3: {
					glUniform3i(location, 0, 0, 0);
				} break;

				case ShaderLanguage::TYPE_IVEC4: {
					glUniform4i(location, 0, 0, 0, 0);
				} break;

				case ShaderLanguage::TYPE_UINT: {
					glUniform1i(location, 0);
				} break;

				case ShaderLanguage::TYPE_UVEC2: {
					glUniform2i(location, 0, 0);
				} break;

				case ShaderLanguage::TYPE_UVEC3: {
					glUniform3i(location, 0, 0, 0);
				} break;

				case ShaderLanguage::TYPE_UVEC4: {
					glUniform4i(location, 0, 0, 0, 0);
				} break;

				case ShaderLanguage::TYPE_FLOAT: {
					glUniform1f(location, 0);
				} break;

				case ShaderLanguage::TYPE_VEC2: {
					glUniform2f(location, 0, 0);
				} break;

				case ShaderLanguage::TYPE_VEC3: {
					glUniform3f(location, 0, 0, 0);
				} break;

				case ShaderLanguage::TYPE_VEC4: {
					glUniform4f(location, 0, 0, 0, 0);
				} break;

				case ShaderLanguage::TYPE_MAT2: {
					GLfloat mat[4] = { 0, 0, 0, 0 };

					glUniformMatrix2fv(location, 1, GL_FALSE, mat);
				} break;

				case ShaderLanguage::TYPE_MAT3: {
					GLfloat mat[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

					glUniformMatrix3fv(location, 1, GL_FALSE, mat);

				} break;

				case ShaderLanguage::TYPE_MAT4: {
					GLfloat mat[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

					glUniformMatrix4fv(location, 1, GL_FALSE, mat);

				} break;

				case ShaderLanguage::TYPE_SAMPLER2D: {
				} break;

				case ShaderLanguage::TYPE_SAMPLEREXT: {
				} break;

				case ShaderLanguage::TYPE_ISAMPLER2D: {
				} break;

				case ShaderLanguage::TYPE_USAMPLER2D: {
				} break;

				case ShaderLanguage::TYPE_SAMPLERCUBE: {
				} break;

				case ShaderLanguage::TYPE_SAMPLER2DARRAY:
				case ShaderLanguage::TYPE_ISAMPLER2DARRAY:
				case ShaderLanguage::TYPE_USAMPLER2DARRAY:
				case ShaderLanguage::TYPE_SAMPLER3D:
				case ShaderLanguage::TYPE_ISAMPLER3D:
				case ShaderLanguage::TYPE_USAMPLER3D: {
					// Not implemented in GLES2
				} break;

				case ShaderLanguage::TYPE_VOID: {
					// Nothing to do?
				} break;
				default: {
					ERR_PRINT("ShaderNode type missing, bug?");
				} break;
			}
		}
	}
}

ShaderGLES2::ShaderGLES2() {
	version = nullptr;
	last_custom_code = 1;
	uniforms_dirty = true;
}

ShaderGLES2::~ShaderGLES2() {
	finish();
}
