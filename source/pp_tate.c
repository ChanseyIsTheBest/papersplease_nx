/* pp_tate.c -- portrait ("TATE") presentation on a swapchain that is stuck in
 * landscape.
 *
 * WHY THIS EXISTS
 *   Papers, Please on phone is portrait-only. The Switch will not give us a
 *   portrait window: geometry set BEFORE mesa creates the EGL surface succeeds,
 *   but afterwards the window is the panel (1280x720) and nwindowSetDimensions
 *   fails outright (rc=0xf59 on hardware). So the swapchain is landscape, full
 *   stop, and a 720x1280 image cannot be pushed through it:
 *
 *     - crop (0,0,720,1280) on a 1280x720 window is out of bounds -> nvnflinger
 *       rejects every present with BAD_VALUE -> black screen;
 *     - clamping the crop to fit shows the top 720 rows only -> vertical squash.
 *
 *   Neither is fixable at the window layer, because the window is not ours to
 *   resize. So do the rotation ourselves, in GL, where we own everything:
 *
 *     1. Give the engine a 720x1280 framebuffer object to render into. It thinks
 *        it has a portrait screen and uses its portrait layout.
 *     2. At present time, bind the REAL framebuffer (the 1280x720 window) and
 *        draw one full-screen quad that samples that texture with rotated
 *        texture coordinates.
 *
 *   Cost: one textured full-screen pass per frame. For a 2D game drawing through
 *   its own quad batcher this is noise.
 *
 * HOW THE ENGINE ENDS UP IN OUR FBO
 *   Unity returns to the default framebuffer with glBindFramebuffer(target, 0).
 *   The GL import table is ours (imports.c resolves every GLES symbol the engine
 *   uses), so we intercept that call and substitute our FBO. The engine never
 *   learns the difference; pp_tate_real_default() is the escape hatch for our
 *   own present path, which genuinely does want framebuffer 0.
 *
 * STATE HYGIENE
 *   The engine leaves arbitrary GL state bound. The present path saves and
 *   restores everything it touches, and binds VAO 0 because GLES3 forbids
 *   client-side vertex arrays while a non-zero VAO is bound -- the same trap the
 *   cursor overlay documents in android_native_unity.c.
 *
 * *** UNTESTED ON HARDWARE. *** This was written from the logs, not from a
 * device. If it misbehaves, PP_TATE_ENABLE in config.h turns it off and you are
 * back to the previous behaviour.
 *
 * MIT, same as the rest of the tree.
 */

#include <string.h>
#include <switch.h>
#include <GLES3/gl3.h>

#include "util.h"
#include "config.h"
#include "pp_tate.h"

static int      s_ready      = 0;
static int      s_failed     = 0;
static GLuint   s_fbo        = 0;
static GLuint   s_tex        = 0;
static GLuint   s_depth      = 0;
static GLuint   s_prog       = 0;
static GLint    s_loc_tex    = -1;
static int      s_rw = 0, s_rh = 0;      /* render (portrait) size  */
static int      s_ww = 0, s_wh = 0;      /* window (landscape) size */
static int      s_rot        = 1;        /* 1 = 90 CW, 2 = 90 CCW   */

/* ------------------------------------------------------------------ shaders */

static const char *kVS =
  "#version 300 es\n"
  "layout(location=0) in vec2 aPos;\n"
  "layout(location=1) in vec2 aUV;\n"
  "out vec2 vUV;\n"
  "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char *kFS =
  "#version 300 es\n"
  "precision mediump float;\n"
  "in vec2 vUV;\n"
  "uniform sampler2D uTex;\n"
  "out vec4 oCol;\n"
  "void main(){ oCol = texture(uTex, vUV); }\n";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {0};
    glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
    debugPrintf("[tate] shader compile failed: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

/* ------------------------------------------------------------------- setup  */

int pp_tate_init(int render_w, int render_h, int window_w, int window_h, int rot) {
  if (s_ready || s_failed) return s_ready;
  s_rw = render_w; s_rh = render_h;
  s_ww = window_w; s_wh = window_h;
  s_rot = (rot == 2) ? 2 : 1;

  /* colour target the engine will draw into */
  glGenTextures(1, &s_tex);
  glBindTexture(GL_TEXTURE_2D, s_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_rw, s_rh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  /* NEAREST, not LINEAR. Papers, Please is pixel art -- its lettering and stamp
   * detail are single-pixel features, and bilinear sampling on the final blit
   * softens exactly the edges the art depends on.
   *
   * The blit is nominally 1:1 (a 720x1280 texture onto a 1280x720 window,
   * rotated), so in principle filtering should never engage. In practice a
   * rotated full-screen quad can land sample points a hair off texel centres,
   * and LINEAR then blends neighbours for a visible softening across the whole
   * image. NEAREST removes the possibility rather than relying on the alignment
   * being exact, and costs nothing. */
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  /* Depth+stencil matters even for a 2D game: Unity's UI and its own clears
   * assume a complete framebuffer, and an FBO without depth silently changes
   * behaviour rather than failing loudly. */
  glGenRenderbuffers(1, &s_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, s_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_rw, s_rh);

  glGenFramebuffers(1, &s_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_depth);

  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    debugPrintf("[tate] FBO incomplete (0x%x) -- portrait disabled, falling back\n", st);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_failed = 1;
    return 0;
  }

  GLuint vs = compile(GL_VERTEX_SHADER, kVS);
  GLuint fs = compile(GL_FRAGMENT_SHADER, kFS);
  if (!vs || !fs) { s_failed = 1; glBindFramebuffer(GL_FRAMEBUFFER, 0); return 0; }
  s_prog = glCreateProgram();
  glAttachShader(s_prog, vs);
  glAttachShader(s_prog, fs);
  glBindAttribLocation(s_prog, 0, "aPos");
  glBindAttribLocation(s_prog, 1, "aUV");
  glLinkProgram(s_prog);
  GLint ok = 0; glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
  glDeleteShader(vs); glDeleteShader(fs);
  if (!ok) {
    char log[512] = {0};
    glGetProgramInfoLog(s_prog, sizeof log - 1, NULL, log);
    debugPrintf("[tate] program link failed: %s\n", log);
    s_failed = 1; glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;
  }
  s_loc_tex = glGetUniformLocation(s_prog, "uTex");

  glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);   /* engine starts here */
  s_ready = 1;
  debugPrintf("[tate] portrait compositor ready: engine renders %dx%d -> window %dx%d, rot=%d\n",
              s_rw, s_rh, s_ww, s_wh, s_rot);
  return 1;
}

int    pp_tate_active(void)       { return s_ready; }
GLuint pp_tate_fbo(void)          { return s_fbo; }

/* ----------------------------------------------------------------- present  */

void pp_tate_present(void) {
  if (!s_ready) return;

  /* --- save the engine's state ------------------------------------------- */
  GLint  prev_prog = 0, prev_vao = 0, prev_tex = 0, prev_ab = 0;
  GLint  vp[4] = {0,0,0,0};
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_ab);
  glGetIntegerv(GL_VIEWPORT, vp);
  const GLboolean had_depth   = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean had_blend   = glIsEnabled(GL_BLEND);
  const GLboolean had_cull    = glIsEnabled(GL_CULL_FACE);
  const GLboolean had_scissor = glIsEnabled(GL_SCISSOR_TEST);

  /* --- draw the rotated blit into the real window ------------------------ */
  glBindFramebuffer(GL_FRAMEBUFFER, 0);      /* the genuine default FB       */
  glViewport(0, 0, s_ww, s_wh);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);

  /* GLES3 forbids client-side arrays while a non-zero VAO is bound, and the
   * engine leaves one bound. Bind 0 (and no ARRAY_BUFFER) for the draw. */
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);

  /* Full-screen quad. Positions are fixed; the ROTATION lives entirely in which
   * texture corner each screen corner samples.
   *
   * Screen corners, in triangle-strip order:  BL, BR, TL, TR
   * For 90 CW the portrait image's top-left goes to the panel's top-right, so
   * each screen corner reads the texture corner one step around the cycle.
   * rot=2 walks the cycle the other way, which is the mirror choice and is what
   * `portrait 2` in config.txt selects. */
  static const GLfloat pos[8] = { -1.f,-1.f,  1.f,-1.f,  -1.f, 1.f,  1.f, 1.f };

  /* Screen corners above are BL, BR, TL, TR (GL y-up). The table below says
   * which TEXTURE corner each screen corner samples, and that is the whole
   * rotation.
   *
   * CW: the portrait image's top edge must end up on the screen's RIGHT.
   *   screen BL <- tex bottom-right   screen BR <- tex TOP-right
   *   screen TL <- tex bottom-left    screen TR <- tex TOP-left
   *
   * These two were swapped in the first version -- `portrait 1` produced a
   * counter-clockwise image and `portrait 2` a clockwise one, the reverse of
   * what config.txt documents. That also threw touch off badly, because
   * android_native_unity.c derives its inverse touch mapping from the SAME
   * config.portrait value: the pointer maths was right for the stated direction
   * while the display did the opposite, so taps landed on the wrong axis. One
   * fix, both symptoms. */
  static const GLfloat uv_cw[8]  = {  1.f,0.f,   1.f,1.f,   0.f,0.f,   0.f,1.f };
  static const GLfloat uv_ccw[8] = {  0.f,1.f,   0.f,0.f,   1.f,1.f,   1.f,0.f };
  const GLfloat *uv = (s_rot == 2) ? uv_ccw : uv_cw;

  glUseProgram(s_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, s_tex);
  if (s_loc_tex >= 0) glUniform1i(s_loc_tex, 0);

  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  glDisableVertexAttribArray(1);

  /* --- restore, and hand the engine its FBO back ------------------------- */
  glUseProgram((GLuint)prev_prog);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_ab);
  glBindVertexArray((GLuint)prev_vao);
  if (had_depth)   glEnable(GL_DEPTH_TEST);
  if (had_blend)   glEnable(GL_BLEND);
  if (had_cull)    glEnable(GL_CULL_FACE);
  if (had_scissor) glEnable(GL_SCISSOR_TEST);
  glViewport(vp[0], vp[1], vp[2], vp[3]);

  glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
}
