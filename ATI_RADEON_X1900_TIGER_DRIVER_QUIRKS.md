# ATI Radeon X1900 / Mac OS X 10.4 Tiger OpenGL driver quirks

This document catalogs every real, reproducible bug found in Apple's
closed-source `ATIRadeonX1000GLDriver` (Mac OS X 10.4.11, PowerPC,
`GL_RENDERER: ATI Radeon X1900 OpenGL Engine`, `GL_VERSION: 1.5
ATI-1.4.19`) while porting Godot 3.x's GLES2 renderer to run on it.
Apple will never update this driver again, so there is no upstream
fix for any of these -- only workarounds. If you're bringing up a
different renderer, engine, or app on this exact hardware/OS/driver
combination, everything below is worth checking for early, since none
of it produces an error message that points at the real cause.

Each entry gives the observed symptom, how it was actually diagnosed
(not guessed), the root cause to the extent it's knowable from outside
the closed-source driver, and the workaround that was shipped.

## 1. Writing a disabled vertex attribute into a varying corrupts the framebuffer

**Symptom:** the screen shows garbled, offset copies of whatever was
previously in the framebuffer (leftover desktop content, a dragged
window's pixels, etc.) instead of what was actually drawn. No GL
error at any point in the pipeline.

**Root cause:** if a generic vertex attribute's array was never
enabled via `glEnableVertexAttribArray` (i.e. it's being fed through
the constant-broadcast `glVertexAttrib4f`/`glVertexAttrib4fv` path
instead of a real per-vertex buffer), and the vertex shader writes
that attribute's value into a `varying`, the driver produces garbage
framebuffer content on the resulting draw. This is true regardless of
the varying's type or count, regardless of whether the fragment
shader actually reads the varying, and regardless of what constant
value the disabled attribute holds (an explicit `glVertexAttrib4f`
value corrupts just as much as the untouched default).

**How this was isolated:** a from-scratch minimal Cocoa/`NSOpenGLContext`
repro was built and bisected one variable at a time against a known-good
baseline (a plain `glClear`+swap, confirmed correct via direct visual
inspection, not just exit code). Each step added back one piece of the
real engine's actual draw (VBO, uniforms, matrices, textures, varyings,
attribute locations) until the corruption reappeared, then narrowed
further: first isolated to "writing *any* attribute-sourced value into a
varying," then to specifically the *disabled* attribute
(`uv_attrib`, location 4, never `glEnableVertexAttribArray`'d) as
opposed to the enabled one (`vertex`, location 0) used in the same
draw -- confirmed in both directions (disabling breaks it regardless
of value; enabling with even throwaway per-vertex content fixes it).

**Workaround:** never use the disabled-attribute-constant-broadcast
path for any attribute a shader writes into a varying. Always keep
the attribute array enabled and feed it from a real (even if
trivially small/constant-content) uploaded buffer instead.

## 2. A runtime `if`/`else` containing a `texture2D()` call anywhere inside it silently voids every fragment

**Symptom:** a shader compiles with no errors, links with no errors,
every uniform/attribute/texture input to it is independently confirmed
correct (verified via direct CPU-side memory reads and `glGetUniformLocation`/
value dumps immediately before the draw), the draw call itself reports
`glGetError() == GL_NO_ERROR`, and `glReadPixels` (correctly targeting
`GL_BACK`, with an explicit `glFinish()` first) shows **zero change** at
every sampled point across the entire window -- as if the draw never
happened at all.

**Root cause:** the fragment shader compiler silently miscompiles any
`if (cond) { ... texture2D(...) ... } else { ... }` construct (or the
mirror image, `texture2D()` only in the `else` branch) such that the
shader effectively writes nothing for the whole draw. Critically, this
happens **even when `cond` is always false at runtime**, so the branch
containing `texture2D()` is never actually executed -- the mere
presence of the construct in the compiled program is enough to break
the whole shader's output, not anything about the branch actually
being taken.

**How this was isolated:** after exhausting every plausible input-side
suspect against the real engine (color, texture content, all matrix/
modulate uniforms, attribute locations, blend state, viewport/scissor,
alpha/depth/stencil test state, FBO binding, read-buffer target -- all
independently confirmed correct with zero effect on the outcome), a
standalone Cocoa/OpenGL repro reproducing the exact same client-array
multi-attribute draw call pattern was built. A trivial hand-written
shader in that repro rendered correctly, ruling out the draw mechanism
itself. Swapping in the engine's *real*, exact preprocessed shader
source (pulled verbatim from its own shader cache) reproduced the bug.
A further from-scratch bisection tool ran four labeled variants
side-by-side in one process (simple-vertex/simple-fragment,
real-vertex/simple-fragment, simple-vertex/real-fragment,
real-vertex/real-fragment) and found the fault isolated to the
fragment shader; a final pair of variants (real fragment shader with
the `if`/`else`-around-`texture2D()` block removed vs. with only its
surrounding unused helper functions removed) pinned the exact
construct.

**Workaround:** never structure a fragment shader with `texture2D()`
inside a runtime conditional branch. Restructure to sample
unconditionally (optionally discarding/blending the result based on
the condition afterward instead of gating the sample itself), or drop
the conditional branch entirely if the feature it guards isn't needed.

## 3. `glTexImage2D` rejects any non-power-of-two size outright

**Symptom:** allocating a texture at an arbitrary (non-POT) size, e.g.
`1024x600` for a window-sized render target, fails immediately with
`GL_INVALID_VALUE` from the very first `glTexImage2D` call -- not a
silent clamp/pad, not a performance warning, an outright rejection.
This happens regardless of internal format (`GL_RGB8`, `GL_RGBA8`,
etc. all rejected equally for a NPOT size).

**How this was isolated:** an isolated standalone repro tried a matrix
of `glTexImage2D` calls varying only dimensions and internal format;
every POT size succeeded, every NPOT size failed identically,
independent of format.

**Workaround:** round texture dimensions up to the next power of two
for allocation (`glTexImage2D`/`glRenderbufferStorage`), while keeping
the logical/requested size for viewport setup and UV math (sampling
only the `logical/allocated` fraction of the texture).

## 4. Rendering into an FBO-attached texture doesn't reliably reach that texture's real sampled content

**Symptom:** create an FBO, attach a texture, render into it (clear or
draw), `glCheckFramebufferStatus` reports `GL_FRAMEBUFFER_COMPLETE`,
every GL call along the way reports no error -- and yet later sampling
that texture (via `texture2D()` in a subsequent draw, or via
`glReadPixels` against the FBO) does not reflect what was actually
rendered into it.

**How this was isolated:** an isolated repro rendered a known solid
color into an FBO-backed texture, then blitted that texture to the
screen via a textured quad; the on-screen result was wrong. Bypassing
the FBO step entirely and uploading the identical pixel data directly
via `glTexSubImage2D` instead, then running the exact same blit/shader
code, produced the correct on-screen result -- isolating the fault
specifically to "render-to-texture via FBO," not the blit/shader/UV
code, which was independently proven correct.

**Workaround:** avoid FBO-based render-to-texture for anything whose
correctness matters end-to-end on this driver. For a full-screen/root
render target specifically, render directly into the system
framebuffer instead (e.g. Godot's existing `RENDER_TARGET_DIRECT_TO_SCREEN`
low-end rendering path, which already exists for other reasons but
happens to sidestep this bug entirely for the root viewport).

## 5. `glReadPixels` against the system framebuffer returns garbage before the window has been shown

**Symptom:** calling `glReadPixels` against the default/system
framebuffer immediately after context creation, before the window has
been made key/ordered-front, returns stale/garbage pixel data even
though the GL context and drawable are otherwise fully valid.

**Workaround:** call `[NSApp activateIgnoringOtherApps:YES]` and
`[window makeKeyAndOrderFront:nil]` immediately after GL context
creation, before issuing any GL calls whose results will be read back.

## 6. `glReadPixels` against an FBO-attached texture is separately unreliable

**Symptom:** independent of quirk 4 above (i.e. even accounting for
it), reading back pixels from an FBO's attached texture via
`glReadPixels` targeting that FBO can return values inconsistent with
the texture's actual content as later sampled by a shader. Don't trust
an FBO `glReadPixels` result on this driver as proof of what a shader
will actually see when it samples that texture.

**Workaround:** when debugging FBO-related rendering on this driver,
verify by actually sampling the texture in a real draw and observing
final pixel output (e.g. after a full render+blit+present cycle),
rather than trusting a direct `glReadPixels` against the FBO.

## 7. The driver's GLSL compiler only accepts `#version 110`

**Symptom:** compiling a shader with `#version 120` fails outright:
`"Version number not supported by GL2"`. `#version 110` compiles fine
on the same driver, and `glGetString(GL_SHADING_LANGUAGE_VERSION)`
confirms the driver's real cap is GLSL 1.10 despite the GPU/driver
generation nominally supporting more on other platforms.

**Workaround:** force `#version 110` for every shader compiled on this
platform, regardless of what version string the engine would normally
pick for desktop GL.

## 8. The GLSL compiler loses track of declarations across nested `#if`/`#ifdef`/`#else`/`#endif` blocks

**Symptom:** a variable declared inside one branch of a *nested*
preprocessor conditional (an `#if`/`#ifdef` inside another
`#if`/`#ifdef`) is wrongly reported as "undeclared" (or produces a
syntax error) later in the same enclosing scope, even though the
branch that declares it is the one actually taken at compile time.
Purely a preprocessor-interaction bug in the shader compiler's own
front end, unrelated to anything about the shader's actual logic.

**How this was isolated:** confirmed via an isolated minimal repro
built specifically to exercise nested conditional declarations,
independent of any engine-specific shader code.

**Workaround:** run the fully-assembled shader source through a real,
correct C preprocessor before it ever reaches the driver, so that no
`#if`/`#ifdef`/`#else`/`#endif` directive survives into what the
driver's own (buggy) preprocessor has to parse -- only the single
branch actually taken remains as plain code. (This project did this
first by shelling out to a real `gcc -E`, then later replaced that
with a small in-process preprocessor covering the same subset of the
language actually used by the shaders in question, purely to drop a
runtime toolchain dependency -- the *reason* the preprocessing has to
happen at all is this driver bug, independent of which preprocessor
implementation does it.)

## 9. A VBO-table crash that turned out not to be independent of quirk 1

**Symptom (as originally observed):** any `glDrawArrays`/`glDrawElements`
call issued while a real VBO is bound to `GL_ARRAY_BUFFER`/
`GL_ELEMENT_ARRAY_BUFFER` crashes the process with `EXC_BAD_ACCESS`
inside the driver (`gldPageoffBuffer`, reached via
`gleDrawArraysOrElements_VBO_Exec` -> `gleExecuteVertexArrayRange`).

**Mechanically root-caused via live disassembly + register/memory dump
at the fault** (not guessed): the faulting instruction is
`stwcx. r9,0,r2` -- part of a PPC lock-free load-linked/store-conditional
pair, i.e. an atomic increment (almost certainly a refcount) applied
to `*(table_entry + 16)`, where `table_entry` was read from what looks
like the driver's internal per-VBO tracking table. The fault address
(`0x01000010`) implies the table slot actually held `0x01000000` -- a
suspiciously round value (matching a classic AGP-aperture/VRAM-pool
size constant for this hardware generation) rather than a real heap
pointer, meaning some slot in the driver's own buffer-object
bookkeeping contained stale/wrong data that it then blindly
dereferenced and atomically wrote through.

**The interesting twist:** an extensive standalone-repro campaign (17
distinct hypotheses -- buffer size, usage hint, shader/texture bound
or not, single vs. many buffers, CGL surface-backing-size overrides, a
second unused GL context, matching linked frameworks and compiler
flags, and more) never reproduced this crash in isolation, even while
faithfully matching the real app's GL state and window/context
architecture. It was originally worked around with a CPU-side
client-array shim that avoided ever binding a real VBO at draw time
(routing all vertex/index data through plain heap memory instead).
Once quirk 1 above (disabled-attribute-into-varying corruption) was
separately found and fixed, a direct experiment -- re-enabling real
VBOs everywhere with quirk 1's fix still in place, and running the
app through a full UI render plus an idle period -- showed **this
crash no longer reproduces at all**. The two were evidently never
independent bugs: the VBO-table crash only manifested alongside the
corrupted rendering state quirk 1 caused, not from VBO usage on its
own. (This is offered as a data point, not a guarantee -- if you hit a
VBO-related crash on this driver and have *not* independently
confirmed your renderer is free of quirk 1's pattern, don't assume
this one is safe to ignore.)

## 10. The GLSL compiler rejects `matN(matM_expr)` constructors as "reserved"

**Symptom:** a fragment or vertex shader that builds a smaller matrix
from a larger one using the standard GLSL constructor form --
`mat3(some_mat4_expression)`, to take the upper-left 3x3, is a common
idiom for normal-matrix computation -- fails to compile with
`'constructor' : constructing matrix from matrix (reserved)`. This is
standard, valid GLSL (1.20+, and widely supported even where not
strictly spec'd); this driver's GLSL 1.10-era compiler refuses it
outright as a reserved/unimplemented construct.

**Why this stayed hidden for a long time:** every prior test of this
port stayed on a 2D-only screen (Godot's Project Manager), which never
compiles any 3D shader. The first time a real project's 3D viewport
was opened -- compiling the scene renderer's normal-matrix code and
the editor's own gizmo shaders for the first time -- this hit
immediately and repeatedly, on two independent shaders:
`mat3(transpose(inverse(world_matrix)))`-style code in the engine's
own `scene.glsl`, and `mat3(MODELVIEW_MATRIX)` in the editor's
rotation-gizmo shader (the latter is not even in a `.glsl` file --
it's a Godot shading-language source embedded as a C++ string literal
in the editor's own source, compiled through the same GLSL pipeline
at runtime).

**How this was actually isolated -- and why the engine's own default
error report is actively misleading on this platform specifically:**
Godot's shader-compile-failure logging dumps the *pre-preprocessing*
source with its own line numbers, for readability. On every other
platform this lines up closely enough with what the driver compiled
to be useful. On this platform it doesn't: the whole reason a
preprocessing pass exists here at all (see quirk 8) is to strip out
every `#if`/`#ifdef`/`#else`/`#endif` before the driver ever sees the
text, so the *actual* compiled text has a completely different line
count than the raw dump, and the driver's own `0:NNN` error location
points at a line number that doesn't correspond to anything in the
default error report. Diagnosing this required temporarily capturing
and dumping the literal string handed to `glShaderSource` (numbered
the same way), which immediately showed the exact failing line. If
you're debugging a GLSL compile failure on this platform and Godot's
own error dump doesn't seem to correspond to anything reasonable, this
line-number mismatch is why -- don't trust it, dump the real compiled
text instead.

**Workaround:** replace `matN(matM_expr)` with an equivalent built from
column vectors, e.g. `mat3(m[0].xyz, m[1].xyz, m[2].xyz)` instead of
`mat3(m)` for a `mat4 m` -- mathematically identical (both just take
the upper-left submatrix), but expressed as a matrix-from-vectors
constructor instead of matrix-from-matrix, which this compiler has no
issue with. **This is a general pattern, not a one-off fix**: any
shader (including ones end users write themselves for their own
projects on this port) that uses `mat3(SOME_MAT4)`/`mat4(SOME_MAT3)`
will hit the same error. There's no compiler-level or preprocessor-level
fix for this implemented in this port yet -- only the two built-in
engine/editor shaders that were actually hit have been patched. A
future, more robust fix would catch this generally (e.g. a
preprocessing-time or shader-compiler-level rewrite of the
matrix-from-matrix constructor pattern), rather than requiring every
individual shader that happens to use it to be hand-patched as it's
discovered.

## 11. A shared line-primitive mesh scaled per-instance via `instance_set_transform()` renders with corrupted, non-uniformly skewed vertex positions

**Symptom:** a wireframe mesh (`Mesh::PRIMITIVE_LINES`) built once with
local unit-cube-ish coordinates and then positioned/scaled per use via
a plain (non-sheared, purely diagonal-scale-plus-translate) instance
transform renders as a corrupted, asymmetric shape instead of the
correct box -- edges connect to visibly wrong positions, and for
larger source AABBs the corruption can push vertices far outside the
visible area entirely ("off screen"). No GL error is reported at any
point in the pipeline, and the corruption reproduces identically
whether the transform's scale is uniform or the mesh has zero, one, or
several vertex attributes beyond position.

**Root cause:** not fully understood at the GPU/driver-internal level.
What's established, via extensive isolation:
- The CPU-side math is provably correct -- the exact `AABB`, `Basis`,
  and `Transform` values were printed and verified by hand at every
  step, the shared mesh's committed vertex buffer was verified
  byte-identical to what was authored, and a from-scratch minimal
  Cocoa/GLSL repro (a single VBO, a hand-written shader, no Godot
  involved at all) reproduces the exact same corruption -- ruling out
  an engine-side bug in the transform or mesh-generation code.
- It is **not** an instance of quirk #1 (disabled attribute into a
  varying) -- the corruption persists in the minimal repro even with
  every extra vertex attribute removed entirely (position-only
  shader, position-only mesh, `GL_POINTS` instead of `GL_LINES`).
- It is not about non-uniform scale, matrix row/column-major mixups,
  or shear -- both the world and projection matrices used in the
  minimal repro are provably diagonal-plus-translation only, which is
  mathematically incapable of producing a skewed result on its own.
- It is not primitive-topology-specific -- switching the minimal
  repro's draw call from `GL_LINES` to `GL_POINTS` still shows one
  (of four expected) vertex position landing in the wrong place.
- The one thing that reliably avoids it: baking the real, final
  world-space (or object-local-space) vertex positions directly into
  the mesh's vertex buffer and using an **identity-scale** instance
  transform (translation/rotation only, from the object's own natural
  transform), instead of authoring a small/unit-sized shared mesh and
  relying on a scaled `instance_set_transform()` to place it. This is
  exactly the pattern the engine's own origin/grid indicator lines
  (`SpatialEditor::_init_grid()`) already used, which is why those
  render correctly while a shared-unit-mesh gizmo (the 3D editor's
  orange selection-box AABB indicator) did not.

**How this was isolated:** the CPU-side transform math was verified
by printing the exact `AABB`, `Basis::scale()` result, and final
`Transform` at the point of use. The mesh's local vertex data was
verified both as authored and as committed into the final `Mesh`
resource. A minimal, fully isolated Cocoa/`NSOpenGLContext` + hand
written GLSL repro was then built to rule out any Godot-side
involvement, and bisected one variable at a time (extra vertex
attributes present/absent, disabled-vs-enabled attribute feeding,
`GL_LINES` vs `GL_POINTS`, non-identity vs identity world transform)
without finding a single-variable trigger -- strongly suggesting the
actual defect is in the driver's handling of this general
shared-geometry-plus-instance-transform-scale rendering pattern, not
any one specific ingredient of it.

**Workaround:** for any gizmo/indicator-style geometry that gets
repositioned or resized per use (not just drawn once at a fixed
transform), bake the actual final vertex positions into the mesh
directly and keep the instance transform at identity scale, rather
than authoring a small reusable unit mesh and scaling it via
`instance_set_transform()`. In this port this means regenerating (not
just re-transforming) the geometry whenever the underlying bounds
change; see `SpatialEditor::_bake_selection_box_mesh()` and
`SpatialEditorSelectedItem::sbox_mesh` in `spatial_editor_plugin.cpp`/
`.h` for the applied fix to the 3D editor's selection-box gizmo. A
`glBindBuffer(GL_ARRAY_BUFFER, ...)` call added defensively right
before the draw call, on the theory that some vertex-attribute-buffer
binding wasn't surviving the gap between attribute setup and the
actual draw, was tried and **made things categorically worse -- it
froze the entire machine solid**, requiring a hard power cycle. Do not
add extra `glBindBuffer`/`glVertexAttribPointer` calls immediately
before a draw call as a speculative fix for this; it is not a binding
survival issue, and doing so risks a full GPU/driver lockup on this
hardware, not just a rendering glitch.

## 12. The GLSL compiler miscomputes an in-shader `mat4 * mat4` multiply for non-trivial matrices

**Symptom:** a mesh renders with badly mispositioned, deformed, or
collapsed-looking geometry -- not merely offset, but visibly wrong
shape, as if some vertices moved independently of others -- while a
different mesh (or the same mesh at a different transform) drawn in
the identical frame, through the identical shader, with the identical
draw call structure, renders perfectly. No GL error at any point in
the pipeline, and the corruption is present from the very first
rendered frame; it requires no animation, drag, or repeated transform
change to appear.

**Root cause:** Godot's real GLES2 vertex shader (`scene.glsl`)
receives the camera and world matrices as two **separate** uniforms
and combines them **in-shader**:
```glsl
uniform mat4 camera_inverse_matrix;
uniform mat4 world_transform;
...
mat4 modelview = camera_inverse_matrix * world_matrix;
vertex = modelview * vertex;
```
This is standard, unremarkable GLSL and matches upstream Godot's own
shader exactly -- nothing platform-specific about the code being
compiled. On this driver, that `mat4 * mat4` product is silently wrong
whenever **both** operands are non-trivial. An identity `world_matrix`
(no rotation) multiplies correctly. A `world_matrix` built purely from
axis permutations (a "clean" 90-degree-style rotation, every entry
0/1/-1) also multiplies correctly. A `world_matrix` with any
fractional, non-axis-aligned rotation value (a real ~25-degree model
transform, and independently confirmed with a synthetic, hand-built,
textbook-clean 30-degree rotation -- so this is not about any specific
numeric bit pattern, just "non-trivial value present") does not.

This explains why this bug went unnoticed for a long time despite
being present on every single frame: most test geometry (UI panels,
axis-aligned editor gizmos, unrotated objects) never exercises a
non-trivial `world_matrix` at all, so the multiply happens to be
correct by construction for it. It only shows up once a real,
arbitrarily-rotated 3D object is on screen.

**How this was isolated:** a standalone Cocoa+OpenGL repro
(`legs_repro.m`) was built loading real extracted mesh/transform data
from a corrupted scene, bisected incrementally against a known-correct
baseline the same way every other quirk in this document was (see the
`incremental-repro-bisection` skill for the general method) -- flat
shading, client-side arrays, real VBOs, real per-vertex lighting, and
finally the literal real shader source captured verbatim from a
running debug build (via a temporary `fopen`/`fwrite` dump right
before `glShaderSource()`) were each ruled in or out one at a time.
The corruption first appeared only once the literal real shader's
separate-uniform, in-shader-combine structure was used instead of a
single precomputed matrix. Two more-specific hypotheses were tested
and ruled out along the way (non-contiguous vertex attribute
locations; an implicit vs. explicit `w=1.0` on the position attribute)
before isolating the actual variable: whether the model/view
combination happens on the GPU or the CPU, confirmed by drawing the
identical mesh+transform both ways side by side in the same frame.
Draw order (which of two same-shader objects is drawn first) was also
tested and ruled out as a contributing factor.

**Workaround:** never rely on this driver to combine two matrices via
an in-shader `mat4 * mat4` multiply when the result feeds vertex
positions. Precompute the combination on the CPU and upload a single
already-combined matrix instead. In this port: `ppc` now always
uploads an identity `camera_inverse_matrix`, and folds the real view
transform into `world_transform` itself (per-instance, on the CPU,
via `Transform::operator*`) before upload -- see
`RasterizerSceneGLES2::_render_render_list()` in
`drivers/gles2/rasterizer_scene_gles2.cpp`. Safe here because this
shader family uses `camera_inverse_matrix`/`world_transform` only in
that one combination line (confirmed against the real captured shader
source) -- if a future shader change ever reads either uniform for
anything else, this substitution would need revisiting. **This is a
general pattern, not just this one call site**: any other in-shader
`matA * matB` product (or `matA * matB * vec`) feeding a
position-affecting result, anywhere else in this codebase's shaders,
is worth suspecting if similarly "some objects fine, some deformed,
no GL error" symptoms show up elsewhere on this driver.

**IMPORTANT CORRECTION / addendum (2026-08-22, follow-up investigation
into GitHub issues #5/#7's remaining symptom):** the C++-side-only
workaround above is **necessary but not sufficient**. Uploading an
identity value for `camera_inverse_matrix` at runtime does NOT stop
the driver from executing the in-shader `mat4 * mat4` multiply --
`scene.glsl`'s vertex shader still contains the literal line
`mat4 modelview = camera_inverse_matrix * world_matrix;`
unconditionally, and the compiler has no way to know at compile time
that a *uniform* will happen to hold identity at runtime, so it always
emits the same generic multiply codegen regardless. That generic
codegen is itself unreliable for **some specific operand value
patterns even when the other operand is runtime-identity** -- found by
isolating a second, distinct symptom in the same player model: one leg
(`Cylinder001`) rendered *completely absent* (not merely deformed, not
mispositioned -- zero pixels, zero GL error) even after the CPU-side
fix above was fully live, while the other leg (`Cylinder`, a different
rotation angle) rendered correctly through the exact same code path.
Proven via a standalone repro (`legs_repro.m`'s `prog5` pass): swapping
which mesh/transform combination was used ruled out the mesh data
(`Cylinder001`'s own VBO renders fine through `Cylinder`'s transform,
and vice versa -- the bug follows the *transform*, not the mesh), and
both matrices involved are ordinary proper rotations (determinant +1,
not a mirror/reflection) -- ruling out a winding-order/backface-culling
explanation too. The only thing that made it render correctly was
removing the `mat4 * mat4` multiply from the **compiled shader source
itself**.

**Full fix (both halves needed together):** in addition to the
C++-side CPU-precompute described above, `drivers/gles2/shader_gles2.cpp`'s
`_preprocess_shader_ppc()` now does a targeted, literal string
replacement on the final preprocessed shader text --
`"mat4 modelview = camera_inverse_matrix * world_matrix;"` becomes
`"mat4 modelview = world_matrix;"` -- removing the multiply from what
actually gets compiled on ppc, rather than just hoping the runtime
identity value saves it. This is done at the **runtime preprocessing**
stage (C++ string manipulation on already-generated shader text), NOT
by editing `scene.glsl` directly with a `#ifdef __ppc__` guard --
editing the raw `.glsl` source would hit the `gles_builders.py`
substring-matching gotcha documented elsewhere in this project (see
[[project_godot_ppc_tiger_renderer]]'s Bug 5 notes): the literal string
`#ifdef ` appearing anywhere in a `.glsl` file, comments included,
gets misparsed into a bogus new `Conditionals` enum entry at build
time. Doing the substitution in `_preprocess_shader_ppc()` instead
sidesteps that entirely, since it runs on text that's already past
`gles_builders.py`'s build-time parsing. Confirmed safe for every
`scene.glsl` vertex-shader variant (skeleton/instancing included): the
`modelview = camera_inverse_matrix * world_matrix` line is computed
**unconditionally** in the shader source, but only ever *read* when
`VERTEX_WORLD_COORDS_USED` is not defined -- so aliasing it to
`world_matrix` is a functional no-op in every variant where it isn't
used, and the fix does not touch the separate, unconditional
`camera_inverse_matrix * vertex` codepath used when
`VERTEX_WORLD_COORDS_USED` *is* defined (untested against this quirk,
not yet known to be affected).

**Verified live in the real editor** (not just the repro): after this
second half of the fix, both `Cylinder` and `Cylinder001` render
correctly and symmetrically in a Top Orthogonal view, matching the
reference build exactly, and both the original translate-drag
(`TRANSLATE_STRESS`) and scale-drag (`SCALE_STRESS`) repros from issues
#7 and #5 show zero deformation on the eye or body through a full
drag.

## 13. The GLSL compiler rejects `gl_PointCoord` as an undeclared identifier

**Symptom:** any fragment shader referencing Godot's `POINT_COORD` builtin
(renamed to `gl_PointCoord` in the generated GLSL, per
`shader_compiler_gles2.cpp`) fails to compile:

```
ERROR: 0:179: 'gl_PointCoord' : undeclared identifier
ERROR: 0:179: 'texture2D' : no matching overloaded function found
ERROR: 0:179: '=' :  cannot convert from 'const float' to '4-component vector of float'
```

Per this driver's established behavior on other shader-compile failures
(quirk 7, 8, 10 above), the affected material's shader then silently
fails to bind rather than erroring visibly in the viewport, so whatever
uses it (a point-sprite/particle material) just doesn't render, with no
obvious on-screen sign anything is wrong. `gl_PointCoord` is a standard
GLSL built-in on any GLSL 1.20+/GLES2-conformant implementation; this
driver's GLSL 1.10-era compiler doesn't recognize it at all, consistent
with the general pattern of quirk 7/10 above (this compiler predates or
omits several otherwise-standard constructs).

**How this was isolated:** confirmed directly by writing a minimal
`shader_type spatial; void fragment() { ALBEDO = vec3(POINT_COORD, 0.0); }`
shader in the live editor on this hardware/driver and observing the
compile error in the Output panel before the fix, and a clean compile
(no errors in Output or the Shader panel) after it -- see GitHub issue #4.

**Workaround:** emulate `gl_PointCoord` manually instead of relying on
the driver to provide it. `shader_compiler_gles2.cpp` renames
`POINT_COORD` to `point_coord_emulated` (instead of `gl_PointCoord`) for
`SHADER_SPATIAL`, gated by a new `POINT_COORD_USED` usage-define
(exactly the same `renames`/`usage_defines` mechanism already used for
every other Godot-shading-language builtin in this file). `scene.glsl`
then, under `#if defined(POINT_COORD_USED)`:
- declares a `varying highp vec3 point_coord_data` (xy = the point's
  window-space pixel center, z = its pixel size);
- in the vertex shader, right after `gl_Position` is finalized, computes
  `point_coord_data.xy = (gl_Position.xy / gl_Position.w * 0.5 + 0.5) * viewport_size`
  and `point_coord_data.z = point_size` (`viewport_size` and `point_size`
  are both already-existing uniforms/locals in this shader -- no new
  plumbing needed for either);
- in the fragment shader, right before `FRAGMENT_SHADER_CODE` is spliced
  in, reconstructs `point_coord_emulated` from `gl_FragCoord` (which
  this driver does support) and `point_coord_data`, flipping Y to match
  `GL_POINT_SPRITE`'s default upper-left UV origin.

This works because a GL point primitive has exactly one vertex, so
`point_coord_data` is trivially constant across the whole primitive
(no `flat` qualifier needed) -- the fragment shader just needs to know
where that one point's screen-space center and size are to reconstruct
per-fragment sprite UV from `gl_FragCoord` itself.
**Scope note:** only `SHADER_SPATIAL` (3D materials/particles) is fixed
this way. `CANVAS_ITEM` (2D) still renames `POINT_COORD` straight to
`gl_PointCoord` and would hit the identical rejection if any 2D shader
used it -- `canvas.glsl` doesn't have an equivalent `viewport_size`
uniform in its vertex stage yet, so the same fix there needs that
plumbing added first. Not yet hit in practice, so not fixed preemptively
per this project's general policy of only patching constructs actually
encountered (see quirk 10's note on `mat3(matN)`).

## Incidental, non-bug observations worth knowing about this platform

- A freshly-created, not-yet-drawn-into window shows the OS's default
  gray backing color for a brief moment before the first real buffer
  swap. This is normal window-server behavior, not a driver bug or a
  sign that something is drawing over your content -- don't chase it
  as if it were one.
- `screencapture`'s `-R <x,y,w,h>` region-capture flag does not exist
  on this OS version's `screencapture`; only whole-screen/interactive/
  window-selection modes are available. To crop a screenshot
  programmatically on this machine, compile a small `NSImage`
  `drawInRect:fromRect:` + `NSBitmapImageRep` tool instead.
