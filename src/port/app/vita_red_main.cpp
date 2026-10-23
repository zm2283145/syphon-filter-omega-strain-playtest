// Port-only (checkpoint 132vt1): the PS Vita display bring-up check (SFOS00003, omega_vita_red.vpk). Nothing from
// the game: the same newlib heap and vitaGL initialisation as omega_vita (gfx_vitagl.cpp), then phases that each
// show a distinct picture (research/VITA.md "Display bring-up check"):
//   0..6 s    red: glClearColor + glClear + vglSwapBuffers
//   6..12 s   blue + an orange/white checkerboard quad (a runtime-compiled Cg program sampling a texture)
//   12..18 s  yellow + the checkerboard drawn into a 960x544 framebuffer object, copied to the display (the game's
//             frame structure: a back-buffer texture, then a full-screen copy and the flip), on the main thread
//   18 s on   the same with magenta, on a second thread pinned to core 1 (where the game's render thread runs)
// It logs to ux0:data/omega_strain/red.log.
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

#include <cstdint>
#include <cstdio>
#include <thread>

unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;  // as vita_main.cpp

namespace {

std::FILE* g_log = nullptr;

void log(const char* text) {
    if (!g_log) return;
    std::fprintf(g_log, "[%8.3f] %s\n", sceKernelGetProcessTimeWide() / 1e6, text);
    // Vita3K keeps a guest file's writes in a host buffer until it is closed.
    std::fclose(g_log);
    g_log = std::fopen("ux0:data/omega_strain/red.log", "a");
}

GLuint shader(GLenum type, const char* source) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &source, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char text[512] = {};
        glGetShaderInfoLog(s, sizeof text - 1, nullptr, text);
        log("Cg compile failed:");
        log(text);
    }
    return s;
}

const float quad[]{-0.5F, -0.5F, 0, 1, 0.5F, -0.5F, 1, 1, -0.5F, 0.5F, 0, 0,
                   -0.5F, 0.5F,  0, 0, 0.5F,  -0.5F, 1, 1, 0.5F,  0.5F, 1, 0};
const float full[]{-1, -1, 0, 1, 1, -1, 1, 1, -1, 1, 0, 0, -1, 1, 0, 0, 1, -1, 1, 1, 1, 1, 1, 0};

struct Scene {
    GLuint program = 0, checker = 0, back = 0, fbo = 0, small = 0, small_fbo = 0;

    void build() {
        const GLuint vs = shader(GL_CG_VERTEX_SHADER_EXT,
                                 "void main(float2 position, float2 uv, out float4 oPosition : POSITION, "
                                 "out float2 oUv : TEXCOORD0) { oPosition = float4(position, 0.0, 1.0); oUv = uv; }");
        const GLuint fs = shader(GL_CG_FRAGMENT_SHADER_EXT,
                                 "float4 main(float2 vUv : TEXCOORD0, uniform sampler2D image : TEXUNIT0) : COLOR "
                                 "{ return tex2D(image, vUv); }");
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "position");
        glBindAttribLocation(program, 1, "uv");
        glLinkProgram(program);
        std::uint32_t texels[8 * 8];
        for (unsigned k = 0; k < 64; ++k) texels[k] = ((k / 8 + k % 8) % 2) ? 0xffffffffU : 0xff00a0ffU;  // ABGR
        glGenTextures(1, &checker);
        glBindTexture(GL_TEXTURE_2D, checker);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenTextures(1, &back);
        glBindTexture(GL_TEXTURE_2D, back);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 960, 544, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, back, 0);
        char text[80];
        std::snprintf(text, sizeof text, "framebuffer status 0x%04x", unsigned(glCheckFramebufferStatus(GL_FRAMEBUFFER)));
        log(text);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glGenTextures(1, &small);
        glBindTexture(GL_TEXTURE_2D, small);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &small_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, small_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, small, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // A 256x256 framebuffer object drawn first, then the display with the small texture sampled in a quad.
    void small_target(float r, float g, float b) {
        glBindFramebuffer(GL_FRAMEBUFFER, small_fbo);
        glViewport(0, 0, 256, 256);
        glClearColor(1.0F, 1.0F, 1.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        textured(checker, quad);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 960, 544);
        glClearColor(r, g, b, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        textured(small, quad);
        vglSwapBuffers(GL_FALSE);
    }

    // The display scene first, then a 960x544 framebuffer-object scene, then the flip.
    void display_first(float r, float g, float b) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 960, 544);
        glClearColor(r, g, b, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        textured(checker, quad);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 960, 544);
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        textured(checker, quad);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        vglSwapBuffers(GL_FALSE);
    }

    void textured(GLuint texture, const float* vertices) {
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, vertices);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, vertices + 2);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisableVertexAttribArray(0);
        glDisableVertexAttribArray(1);
        glUseProgram(0);
    }

    // The game's frame: the picture into the back-buffer texture, then a full-screen copy onto the display.
    void through_fbo(float r, float g, float b, bool sample = true, bool finish = false) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, 960, 544);
        glClearColor(r, g, b, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        textured(checker, quad);
        if (finish) glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, 960, 544);
        if (sample) {
            glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            textured(back, full);
        } else {
            glClearColor(r, g, b, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            textured(checker, quad);
        }
        vglSwapBuffers(GL_FALSE);
    }
};

void log_frame(unsigned frame, const char* what) {
    if (frame % 120) return;
    char text[96];
    std::snprintf(text, sizeof text, "frame %u: %s", frame, what);
    log(text);
}

}  // namespace

int main() {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/omega_strain", 0777);
    sceIoRemove("ux0:data/omega_strain/red.log");
    g_log = std::fopen("ux0:data/omega_strain/red.log", "w");
    log("display bring-up check (SFOS00003)");
    vglSetupRuntimeShaderCompiler(SHARK_OPT_DEFAULT, GL_FALSE, GL_FALSE, GL_FALSE);
    // vglInitExtended returns GL_TRUE only when it fell back to a smaller resolution.
    const GLboolean fallback = vglInitExtended(0x100000, 960, 544, 8 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    log(fallback ? "vglInitExtended: fell back to a smaller resolution" : "vglInitExtended: 960x544");

    Scene scene;
    unsigned frame = 0;
    for (; frame < 4 * 60; ++frame) {
        log_frame(frame, "red");
        glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_FALSE);
    }
    log("building the textured quad (runtime Cg)");
    scene.build();
    for (; frame < 8 * 60; ++frame) {
        log_frame(frame, "blue + textured quad");
        glClearColor(0.0F, 0.0F, 1.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        scene.textured(scene.checker, quad);
        vglSwapBuffers(GL_FALSE);
    }
    for (; frame < 12 * 60; ++frame) {
        log_frame(frame, "green: a 256x256 framebuffer object first, sampled on the display");
        scene.small_target(0.0F, 1.0F, 0.0F);
    }
    for (; frame < 16 * 60; ++frame) {
        log_frame(frame, "yellow: display scene first, then a 960x544 framebuffer object scene");
        scene.display_first(1.0F, 1.0F, 0.0F);
    }
    for (; frame < 20 * 60; ++frame) {
        log_frame(frame, "cyan through a framebuffer object, main thread");
        scene.through_fbo(0.0F, 1.0F, 1.0F);
    }
    std::thread render([&] {
        const int affinity = sceKernelChangeThreadCpuAffinityMask(0, 0x20000);
        const int priority = sceKernelChangeThreadPriority(0, 96);
        char text[96];
        std::snprintf(text, sizeof text, "second thread: core 1 affinity 0x%x, priority 0x%x", unsigned(affinity),
                      unsigned(priority));
        log(text);
        for (;; ++frame) {
            log_frame(frame, "magenta through a framebuffer object, core 1 thread");
            scene.through_fbo(1.0F, 0.0F, 1.0F);
        }
    });
    render.join();
}
