/*
 * gputest: draw on a render node with no display, to see a GPU do work.
 *
 * Added for OmonullOS, October 2026 (Galaxy S10 5G, Panfrost bring-up). Opens
 * the node with GBM, makes a surfaceless GLES2 context, fills a full-screen
 * sized framebuffer object N times with a fragment shader that does some
 * arithmetic per pixel, reads one pixel back to check it, and reports frames
 * per second. Build on the phone:
 *   gcc -O2 -o gputest gputest.c -lEGL -lGLESv2 -lgbm
 *   gputest /dev/dri/renderD128 1440 3040 100
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static const char *vs =
	"attribute vec2 p; varying vec2 uv;\n"
	"void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *fs =
	"precision highp float; varying vec2 uv; uniform float t;\n"
	"void main() { float v = 0.0;\n"
	"  for (int i = 0; i < 16; i++) v += sin(uv.x * 40.0 + t + float(i)) * cos(uv.y * 40.0 - float(i));\n"
	"  gl_FragColor = vec4(fract(v), uv, 1.0); }\n";

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;
	char log[512];

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "shader: %s\n", log);
		exit(1);
	}
	return s;
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int w = argc > 2 ? atoi(argv[2]) : 1440, h = argc > 3 ? atoi(argv[3]) : 3040;
	int frames = argc > 4 ? atoi(argv[4]) : 100;
	int fd = open(node, O_RDWR | O_CLOEXEC);
	struct gbm_device *gbm;
	EGLDisplay dpy;
	EGLConfig cfg;
	EGLContext ctx;
	EGLint n;
	static const EGLint cfg_attr[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
	static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint tex, fbo, prog;
	unsigned char px[4];
	double t0, t1;

	if (fd < 0 || !(gbm = gbm_create_device(fd))) {
		perror(node);
		return 1;
	}
	dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	if (!eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
	    !eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n < 1 ||
	    !(ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr)) ||
	    !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
		fprintf(stderr, "EGL setup failed: 0x%x\n", eglGetError());
		return 1;
	}
	printf("renderer: %s\n", glGetString(GL_RENDERER));

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		fprintf(stderr, "framebuffer incomplete\n");
		return 1;
	}
	prog = glCreateProgram();
	glAttachShader(prog, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(prog, 0, "p");
	glLinkProgram(prog);
	glUseProgram(prog);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, w, h);

	t0 = now();
	for (int i = 0; i < frames; i++) {
		glUniform1f(glGetUniformLocation(prog, "t"), i * 0.1f);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glFinish();
	}
	t1 = now();
	glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("%d frames of %dx%d in %.2f s: %.1f fps; centre pixel %u %u %u %u, GL error 0x%x\n",
	       frames, w, h, t1 - t0, frames / (t1 - t0), px[0], px[1], px[2], px[3], glGetError());
	return 0;
}
