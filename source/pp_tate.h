/* pp_tate.h -- portrait presentation via render-to-texture. See pp_tate.c. */
#ifndef PP_TATE_H
#define PP_TATE_H

#include <GLES3/gl3.h>

/* Create the portrait FBO the engine renders into. render_* is the portrait size
 * the engine believes it has; window_* is the real landscape swapchain.
 * rot: 1 = 90 CW, 2 = 90 CCW. Returns 1 on success, 0 if unavailable (in which
 * case every other entry point is inert and the port behaves as before). */
int pp_tate_init(int render_w, int render_h, int window_w, int window_h, int rot);

/* Non-zero once the compositor owns the engine's default framebuffer. */
int pp_tate_active(void);

/* The FBO to substitute when the engine binds framebuffer 0. */
GLuint pp_tate_fbo(void);

/* Blit the portrait texture, rotated, into the real window. Call immediately
 * before eglSwapBuffers. Saves and restores all GL state it touches. */
void pp_tate_present(void);

#endif /* PP_TATE_H */
