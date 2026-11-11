// The live view on the Vita's GPU, through vitaGL (gate 11 on, SM64DS_VITAGL).
//
// What moves to the GPU is the 3D picture, the part the CPU could not keep up
// with: each captured frame's triangles (ntr::GxFrame: already transformed,
// lit and clipped by the geometry engine, port/ntr/gx.cpp) are drawn into a
// 256x192 render target with the DS's rules, then read back as the 3D layer
// the 2D compositor takes (ppu_compose_from), so every 2D rule -- priorities,
// windows, blending with BG0 -- stays the CPU's, unchanged. Both finished
// screens go back to the GPU as two textures and are drawn on the display,
// with the frame-rate line.
//
// The DS rules kept here (they are the software renderer's, gx.cpp
// render_impl, one by one):
//   - faces: POLYGON_ATTR bit 0 back, bit 1 front (culled on the CPU, with
//     the same screen-space winding test);
//   - alpha 0 (wireframe) and shadow polygons (mode 3) are not drawn;
//   - a texel with alpha 0 is not drawn; the pixel's alpha is the polygon's
//     times the texel's, (pa + 1) * (ta + 1) - 1 >> 5 on 5-bit values;
//   - the alpha test (DISP3DCNT bit 2): a pixel at or under ALPHA_TEST_REF
//     is not drawn; a pixel with alpha 0 never is;
//   - depth: opaque pixels write it, translucent ones only with
//     POLYGON_ATTR bit 11;
//   - blending (DISP3DCNT bit 3): translucent over what is there, the
//     layer's alpha the larger of the two; without it the pixel is written
//     with its own alpha (the 2D engine blends it with what is behind);
//   - the clear colour and its alpha (CLEAR_COLOR);
//   - textures: nearest texel, repeat / mirrored repeat / clamp per axis
//     (TEXIMAGE_PARAM bits 16-19); perspective-correct.
// The layer's alpha comes back as 8 bits and is turned into the DS's 5.
//
// Shaders are Cg (vitaGL compiles them at run time with the system's
// compiler, ur0:data/libshacccg.suprx, installed with ShaRKBR33D). The setup
// follows the Super Mario Sunshine Vita port's (JackRuss9/sms-vita,
// vita/src/vita_gl.cpp), whose findings it relies on: FBO readback with
// glReadPixels works, and a very small render target draws nothing.
#if defined(__vita__) && defined(SM64DS_VITAGL)

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <psp2/io/stat.h>
#include <vitaGL.h>

#include "ntr/gx.h"
#include "ntr/ppu.h"
#include "vita_gl3d.h"

namespace {
constexpr int W = ntr::SCREEN_W, H = ntr::SCREEN_H;   // 256 x 192

GLuint g_prog3d, g_prog2d, g_progmix;
GLint g_u_tex3d, g_u_p, g_u_tex2d, g_u_mix_off, g_u_mix_ctl, g_u_mix_3d, g_u_mix_b, g_u_mix_m;
GLuint g_rt_tex, g_rt_fbo, g_rt_depth;
bool g_rt_stencil;      // the render target has a stencil buffer
int g_rt_scale;                    // the 3D render target: 256 x 192 times this
bool g_rt_wide;                    // ... and 4/3 as wide (widescreen)
int g_rt_w, g_rt_h;
GLuint g_screen_tex[2], g_text_tex;
GLuint g_a_off_tex, g_a_ctl_tex;   // engine A's 2D, for mixing with the 3D picture
GLuint g_menu_tex;                 // the settings menu
int g_menu_tw, g_menu_th;
constexpr int TEXT_W = 1024, TEXT_H = 16;
uint8_t g_readback[W * H * 4];

struct GlTex {
    GLuint id;
    int w, h;
    bool partial;      // some texel with an alpha between 0 and full
};
std::unordered_map<const uint32_t *, GlTex> g_tex;

/* Decoded texels freed (gx_set_texture_free_hook, any thread): their GPU
   copies go before the next frame looks any texture up, since the decoder
   may put another texture at the same address -- with the same size, the
   old copy was taken for it (Yoshi's eyes on the doors, Yoshi's blank). */
std::mutex g_freed_mutex;
std::vector<const uint32_t *> g_freed;
void on_texels_freed(const uint32_t *p)
{
    std::lock_guard<std::mutex> lock(g_freed_mutex);
    g_freed.push_back(p);
}
void forget_freed_textures()
{
    std::vector<const uint32_t *> freed;
    {
        std::lock_guard<std::mutex> lock(g_freed_mutex);
        freed.swap(g_freed);
    }
    for (const uint32_t *p : freed) {
        auto it = g_tex.find(p);
        if (it == g_tex.end()) continue;
        glDeleteTextures(1, &it->second.id);
        g_tex.erase(it);
    }
}

const char *kVs3d =
    "void main(float4 a_pos, float2 a_uv, float4 a_col,\n"
    "  out float4 gl_Position : POSITION, out float2 v_uv : TEXCOORD0, out float4 v_col : COLOR0) {\n"
    "  gl_Position = a_pos; v_uv = a_uv; v_col = a_col; }\n";
// u_p: x alpha-test reference (5-bit; -1 off), y 1 when textured
const char *kFs3d =
    "uniform sampler2D u_tex; uniform float4 u_p;\n"
    "float4 main(float2 v_uv : TEXCOORD0, float4 v_col : COLOR0) : COLOR {\n"
    "  float4 t = float4(1.0, 1.0, 1.0, 1.0);\n"
    "  float pa = floor(v_col.a * 31.0 + 0.5);\n"
    "  float a = pa;\n"
    "  if (u_p.y > 0.5) {\n"
    "    t = tex2D(u_tex, v_uv);\n"
    "    if (t.a < 0.5 / 255.0) discard;\n"
    "    float ta = floor(t.a * 255.0 / 8.0 + 0.01);\n"
    "    a = floor(((pa + 1.0) * (ta + 1.0) - 1.0) / 32.0);\n"
    "  }\n"
    "  if (a < 0.5 || a <= u_p.x) discard;\n"
    "  return float4(v_col.rgb * t.rgb, a / 31.0); }\n";
const char *kVs2d =
    "void main(float2 a_pos, float2 a_uv, out float4 gl_Position : POSITION, out float2 v_uv : TEXCOORD0) {\n"
    "  gl_Position = float4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
const char *kFs2d =
    "uniform sampler2D u_tex;\n"
    "float4 main(float2 v_uv : TEXCOORD0) : COLOR { return tex2D(u_tex, v_uv); }\n";

// Engine A's screen: its 2D picture where the 3D layer has nothing (u_off),
// and where it has something, the 3D pixel mixed in by the code in u_ctl
// (ntr/ppu.h, ppu_compose_lines: the CPU worked out the layers, the GPU has
// the 3D picture, at any resolution). u_b: BLDALPHA EVA/16, EVB/16, BLDY
// EVY/16, BLDCNT's mode; u_m: BG0HOFS / 256, the master brightness mode
// (1 up, 2 down) and factor / 16, and the 2D picture's share of the width:
// 1, or 0.75 in widescreen, where the 3D picture is wider than the DS's and
// the 2D one sits in the middle (beside it, the 3D picture alone).
const char *kFsMix =
    "uniform sampler2D u_off; uniform sampler2D u_ctl; uniform sampler2D u_3d;\n"
    "uniform float4 u_b; uniform float4 u_m;\n"
    "float4 main(float2 v_uv : TEXCOORD0) : COLOR {\n"
    "  float wf = u_m.w;\n"
    "  float2 uv2 = float2((v_uv.x - (1.0 - wf) * 0.5) / wf, v_uv.y);\n"
    "  float side = (uv2.x < 0.0 || uv2.x >= 1.0) ? 1.0 : 0.0;\n"
    "  float3 o = tex2D(u_off, uv2).rgb;\n"
    "  float4 k = tex2D(u_ctl, uv2);\n"
    "  float code = side > 0.5 ? 1.0 : floor(k.a * 255.0 + 0.5);\n"
    "  float mode = fmod(code, 4.0);\n"
    "  float2 uv3 = float2(v_uv.x + u_m.x * wf, v_uv.y);\n"
    "  if (mode > 0.5 && uv3.x >= 0.0 && uv3.x < 1.0) {\n"
    "    float4 c = tex2D(u_3d, uv3);\n"
    "    float a3 = floor(c.a * 31.0 + 0.5);\n"
    "    if (a3 > 0.5) {\n"
    "      if (mode < 1.5) {\n"
    "        float s_ok = fmod(floor(code / 4.0), 2.0);\n"
    "        float fxf = fmod(floor(code / 8.0), 2.0);\n"
    "        if (a3 < 30.5 && s_ok > 0.5) o = (c.rgb * (a3 + 1.0) + k.rgb * (31.0 - a3)) / 32.0;\n"
    "        else if (fxf > 0.5 && u_b.w > 2.5) o = c.rgb - c.rgb * u_b.z;\n"
    "        else if (fxf > 0.5 && u_b.w > 1.5) o = c.rgb + (1.0 - c.rgb) * u_b.z;\n"
    "        else if (fxf > 0.5 && u_b.w > 0.5 && s_ok > 0.5) o = min(c.rgb * u_b.x + k.rgb * u_b.y, 1.0);\n"
    "        else o = c.rgb;\n"
    "      } else {\n"
    "        float wa = fmod(floor(code / 4.0), 32.0) / 16.0;\n"
    "        float wb = code > 127.5 ? u_b.y : 1.0 - wa;\n"
    "        o = min(k.rgb * wa + c.rgb * wb, 1.0);\n"
    "      }\n"
    "    }\n"
    "  }\n"
    "  if (u_m.y > 1.5) o = o - o * u_m.z;\n"
    "  else if (u_m.y > 0.5) o = o + (1.0 - o) * u_m.z;\n"
    "  return float4(o, 1.0); }\n";

GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        printf("  gpu: a shader did not compile: %s\n", log);
        return 0;
    }
    return sh;
}

GLuint program(const char *vs, const char *fs, const char *const *attrs, int nattrs)
{
    const GLuint v = compile(GL_CG_VERTEX_SHADER_EXT, vs), f = compile(GL_CG_FRAGMENT_SHADER_EXT, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    for (int i = 0; i < nattrs; ++i) glBindAttribLocation(p, i, attrs[i]);
    glLinkProgram(p);
    return p;
}

/* A texture; `bgra` stores it the way the port's pixels already are in
   memory (0xAARRGGBB words: bytes B, G, R, A), so an upload is a copy. */
GLuint make_texture(int w, int h, GLint filter, bool bgra = false)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    const GLenum f = bgra ? GL_BGRA : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, f, w, h, 0, f, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* A decoded DS texture (0xAARRGGBB) on the GPU, uploaded once. */
const GlTex &texture_for(const ntr::GxTriangle &t)
{
    auto it = g_tex.find(t.tex);
    if (it != g_tex.end()) {
        if (it->second.w == t.tw && it->second.h == t.th) return it->second;
        glDeleteTextures(1, &it->second.id);    // the memory now holds another texture
        g_tex.erase(it);
    }
    const size_t n = (size_t)t.tw * t.th;
    bool partial = false;
    for (size_t i = 0; i < n && !partial; ++i) {
        const uint32_t a = t.tex[i] >> 24;
        if (a != 0 && a < 0xf8) partial = true;
    }
    GlTex g;
    glGenTextures(1, &g.id);
    glBindTexture(GL_TEXTURE_2D, g.id);
    // the decoded texels (0xAARRGGBB) go up as they are
    glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA, t.tw, t.th, 0, GL_BGRA, GL_UNSIGNED_BYTE, t.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g.partial = partial;
    g.w = t.tw;
    g.h = t.th;
    return g_tex.emplace(t.tex, g).first->second;
}

GLint wrap_mode(bool repeat, bool flip)
{
    return !repeat ? GL_CLAMP_TO_EDGE : (flip ? GL_MIRRORED_REPEAT : GL_REPEAT);
}

/* Textures no longer in the game's decode cache: freed. */
void drop_stale(const ntr::GxFrame &frame)
{
    static unsigned n;
    if (++n % 30) return;                   // twice a second is plenty
    std::unordered_set<const uint32_t *> live;
    for (const auto &k : frame.keep)
        if (k && !k->empty()) live.insert(k->data());
    for (auto it = g_tex.begin(); it != g_tex.end();) {
        if (!live.count(it->first)) {
            glDeleteTextures(1, &it->second.id);
            it = g_tex.erase(it);
        } else {
            ++it;
        }
    }
}

// one batch: consecutive triangles that share the draw state
struct Batch {
    const ntr::GxTriangle *first;   // texture and wrap come from it
    bool textured, depth_write;
    int trans_id;                   // translucent polygon: its ID (0..63); -1 opaque
    std::vector<float> pos, uv;
    std::vector<uint8_t> col;
};

/* The 3D picture's render target, 256 x 192 times `scale`, with a depth
   buffer; again when the scale changes. */
void make_render_target(int scale, bool wide, int nw = 0, int nh = 0)
{
    if (scale == 0 && (nw < 64 || nh < 48)) scale = 1;      // no size to match: the DS's
    if (scale != 0) nw = nh = 0;
    if (g_rt_scale == scale && g_rt_wide == wide && (scale || (g_rt_w == nw && g_rt_h == nh))) return;
    if (g_rt_fbo) {
        glDeleteFramebuffers(1, &g_rt_fbo);
        glDeleteRenderbuffers(1, &g_rt_depth);
        glDeleteTextures(1, &g_rt_tex);
    }
    g_rt_scale = scale;
    g_rt_wide = wide;
    if (scale == 0) {
        // the screen's own size on the display (widescreen's is already wide)
        g_rt_w = nw;
        g_rt_h = nh;
    } else {
        g_rt_w = wide ? (W * 4 * scale + 2) / 3 : W * scale;
        g_rt_h = H * scale;
    }
    g_rt_tex = make_texture(g_rt_w, g_rt_h, GL_NEAREST);
    glGenFramebuffers(1, &g_rt_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_rt_tex, 0);
    glGenRenderbuffers(1, &g_rt_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_rt_depth);
    // depth and stencil: the stencil holds the polygon ID of the translucent
    // pixel drawn (the DS's attribute buffer, see sm64ds_gl_render_3d)
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_rt_w, g_rt_h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_rt_depth);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_rt_depth);
    g_rt_stencil = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (!g_rt_stencil) {        // no depth+stencil target here: depth only, no ID rule
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_rt_w, g_rt_h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_rt_depth);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    printf("  gpu: the 3D picture is drawn at %dx%d (%dx%s)%s\n", g_rt_w, g_rt_h, scale, wide ? ", widescreen" : "",
           g_rt_stencil ? "" : ", no stencil (translucent polygon IDs not kept)");
}

void flush(Batch &b, float alpha_ref)
{
    const size_t n = b.pos.size() / 4;
    if (!n) return;
    const ntr::GxTriangle &t = *b.first;
    if (b.textured) {
        const GlTex &g = texture_for(t);
        glBindTexture(GL_TEXTURE_2D, g.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_mode(t.wrap & 1, t.wrap & 4));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_mode(t.wrap & 2, t.wrap & 8));
    }
    glUniform4f(g_u_p, alpha_ref, b.textured ? 1.0f : 0.0f, 0.0f, 0.0f);
    glDepthMask(b.depth_write ? GL_TRUE : GL_FALSE);
    // A translucent pixel is not drawn over a translucent pixel of the same
    // polygon ID (GBATEK, "3D Polygon Attributes"): a model fading in or out
    // shows its nearest layer, not every layer blended. The stencil keeps
    // the ID (+1, 0 being none).
    if (g_rt_stencil && b.trans_id >= 0) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_NOTEQUAL, b.trans_id + 1, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    } else if (g_rt_stencil) {
        glDisable(GL_STENCIL_TEST);
    }
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, b.pos.data());
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, b.uv.data());
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, b.col.data());
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n);
    b.pos.clear();
    b.uv.clear();
    b.col.clear();
}
}  // namespace

/* Once, on the drawing thread. False when the GPU path cannot run (the
   shader compiler is missing, or a shader did not build): the caller keeps
   the software renderer. */
extern "C" bool sm64ds_gl_init(void)
{
    SceIoStat st;
    if (sceIoGetstat("ur0:data/libshacccg.suprx", &st) < 0
        && sceIoGetstat("ur0:data/external/libshacccg.suprx", &st) < 0) {
        printf("  gpu: ur0:data/libshacccg.suprx is missing (install it with ShaRKBR33D):"
               " the 3D stays on the CPU\n");
        return false;
    }
    sceIoMkdir("ux0:data/sm64ds/shader_cache", 0777);
    vglSetShaderCachePath("ux0:data/sm64ds/shader_cache");
    vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, GL_TRUE, GL_FALSE, GL_FALSE);
    // Fixed, modest pools. vglInitExtended hands vitaGL all the memory the
    // game's heap leaves but a margin: on the first run the memory card's
    // writes then crawled (a log line a second, the watchdog fired while the
    // castle grounds loaded). The 3D needs little: a 256x192 target, the
    // decoded DS textures (a few MiB at most) and the vertex streams.
    ntr::gx_set_texture_free_hook(on_texels_freed);
    if (vglInitWithCustomSizes(0, 960, 544, 16 * 1024 * 1024, 32 * 1024 * 1024, 0, 0, SCE_GXM_MULTISAMPLE_NONE))
        printf("  gpu: vitaGL lowered the display resolution\n");
    vglWaitVblankStart(GL_FALSE);

    static const char *const a3d[] = {"a_pos", "a_uv", "a_col"};
    static const char *const a2d[] = {"a_pos", "a_uv"};
    g_prog3d = program(kVs3d, kFs3d, a3d, 3);
    g_prog2d = program(kVs2d, kFs2d, a2d, 2);
    g_progmix = program(kVs2d, kFsMix, a2d, 2);
    if (!g_prog3d || !g_prog2d || !g_progmix) return false;
    g_u_tex3d = glGetUniformLocation(g_prog3d, "u_tex");
    g_u_p = glGetUniformLocation(g_prog3d, "u_p");
    g_u_tex2d = glGetUniformLocation(g_prog2d, "u_tex");
    g_u_mix_off = glGetUniformLocation(g_progmix, "u_off");
    g_u_mix_ctl = glGetUniformLocation(g_progmix, "u_ctl");
    g_u_mix_3d = glGetUniformLocation(g_progmix, "u_3d");
    g_u_mix_b = glGetUniformLocation(g_progmix, "u_b");
    g_u_mix_m = glGetUniformLocation(g_progmix, "u_m");
    make_render_target(1, false);

    // the two finished screens and the frame-rate line; the screens are
    // scaled with a linear filter (nearest would make uneven pixels at 1.875x)
    g_screen_tex[0] = make_texture(W, H, GL_LINEAR, true);
    g_screen_tex[1] = make_texture(W, H, GL_LINEAR, true);
    g_text_tex = make_texture(TEXT_W, TEXT_H, GL_NEAREST, true);
    // engine A's two layers of information: read texel by texel (a code
    // cannot be interpolated)
    g_a_off_tex = make_texture(W, H, GL_NEAREST, true);
    g_a_ctl_tex = make_texture(W, H, GL_NEAREST, true);
    printf("  gpu: vitaGL ready, the 3D is drawn on the GPU (%u KiB of GPU memory free)\n",
           (unsigned)(vglMemFree(VGL_MEM_ALL) / 1024));
    return true;
}

/* The 3D layer of `frame`, as gx_render_frame would draw it, at `scale`
   (1..4) times the DS's resolution; read back into `fb` too when given
   (at 1x only: the CPU then mixes it in). */
extern "C" void sm64ds_gl_render_3d(const ntr::GxFrame &frame, ntr::Framebuffer *fb, int scale, bool wide,
                                    int native_w, int native_h)
{
    if (fb) { scale = 1; wide = false; }
    forget_freed_textures();
    make_render_target(scale < 0 ? 1 : scale > 4 ? 4 : scale, wide, native_w, native_h);
    // widescreen: the same projection, 3/4 as wide on the screen, so a 16:9
    // picture shows the DS's view in its middle and more of the scene beside
    // it (what the game draws there: it culls by its own 4:3 view, so things
    // at the very edges can appear late)
    const float xs = wide ? 0.75f : 1.0f;
    glBindFramebuffer(GL_FRAMEBUFFER, g_rt_fbo);
    glViewport(0, 0, g_rt_w, g_rt_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    const uint32_t c = frame.clear;
    glClearColor((c & 31) / 31.0f, ((c >> 5) & 31) / 31.0f, ((c >> 10) & 31) / 31.0f, ((c >> 16) & 31) / 31.0f);
    glClearDepthf(1.0f);
    glDepthMask(GL_TRUE);
    if (g_rt_stencil) {
        glClearStencil(0);
        glStencilMask(0xff);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    glUseProgram(g_prog3d);
    glUniform1i(g_u_tex3d, 0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    if (frame.disp3d & 8) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
    } else {
        glDisable(GL_BLEND);
    }
    const float alpha_ref = (frame.disp3d & 4) ? (float)(frame.alpha_ref & 31) : -1.0f;
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    static Batch b;
    b.first = nullptr;
    for (const ntr::GxTriangle &t : frame.tris) {
        const ntr::GxVertex &v0 = t.v[0], &v1 = t.v[1], &v2 = t.v[2];
        const float area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
        if (area > -1e-6f && area < 1e-6f) continue;
        if (area > 0.0f ? !(t.faces & 1) : !(t.faces & 2)) continue;
        if (t.alpha == 0) continue;                       // wireframe: not drawn
        if (((t.attr >> 4) & 3) == 3) continue;           // shadow polygon: a mask
        const bool textured = t.tex && t.tw > 0 && t.th > 0;
        bool opaque = t.alpha >= 31;
        if (opaque && textured) opaque = !texture_for(t).partial;
        const bool depth_write = opaque || (t.attr & 0x800);
        const int trans_id = t.alpha < 31 ? (int)((t.attr >> 24) & 63) : -1;
        if (b.first && (textured != b.textured || depth_write != b.depth_write || trans_id != b.trans_id
                        || (textured && (t.tex != b.first->tex || t.wrap != b.first->wrap)))) {
            flush(b, alpha_ref);
            b.first = nullptr;
        }
        if (!b.first) {
            b.first = &t;
            b.textured = textured;
            b.depth_write = depth_write;
            b.trans_id = trans_id;
        }
        const float pa = (float)t.alpha;
        for (const ntr::GxVertex *v : {&v0, &v1, &v2}) {
            // screen pixels back to clip space, w kept so the GPU interpolates
            // perspective-correctly; screen row 0 is the render target's
            // first row (vitaGL's FBOs are not flipped), and so the readback's
            const float w = v->w > 1e-6f ? v->w : 1.0f;
            const float nx = (v->x / (W / 2.0f) - 1.0f) * xs, ny = v->y / (H / 2.0f) - 1.0f;
            const float nz = v->z * 2.0f - 1.0f;
            b.pos.insert(b.pos.end(), {nx * w, ny * w, nz * w, w});
            if (textured) b.uv.insert(b.uv.end(), {v->u / t.tw, v->v / t.th});
            else b.uv.insert(b.uv.end(), {0.0f, 0.0f});
            b.col.insert(b.col.end(), {(uint8_t)(v->color >> 16), (uint8_t)(v->color >> 8), (uint8_t)v->color,
                                       (uint8_t)(pa * 255.0f / 31.0f + 0.5f)});
        }
    }
    if (b.first) flush(b, alpha_ref);
    glDepthMask(GL_TRUE);
    if (g_rt_stencil) glDisable(GL_STENCIL_TEST);

    if (!fb) {                  // mixed in on the GPU (sm64ds_gl_present)
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        drop_stale(frame);
        return;
    }
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, g_readback);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (int y = 0; y < H; ++y) {
        const uint8_t *s = g_readback + y * W * 4;
        uint32_t *d = fb->px[y];
        for (int x = 0; x < W; ++x, s += 4) {
            const uint32_t a5 = ((uint32_t)s[3] * 31 + 127) / 255;
            d[x] = a5 << 24 | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
        }
    }
    drop_stale(frame);
}

namespace {
/* A finished screen (0xFFRRGGBB words) into its BGRA texture: a copy. */
void upload(GLuint tex, const ntr::Framebuffer &fb)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, W, H, GL_BGRA, GL_UNSIGNED_BYTE, &fb.px[0][0]);
}

/* A textured quad at display pixels (x, y, w, h), top left origin. */
void quad(GLuint tex, float x, float y, float w, float h, float u1, float v1)
{
    const float x0 = x / 480.0f - 1.0f, x1 = (x + w) / 480.0f - 1.0f;
    const float y0 = 1.0f - y / 272.0f, y1 = 1.0f - (y + h) / 272.0f;
    const float pos[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
    const float uv[8] = {0, 0, u1, 0, 0, v1, u1, v1};
    glBindTexture(GL_TEXTURE_2D, tex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
}  // namespace

/* Both screens at their layout rectangles and the frame-rate line at the
   top left; then shown. */
extern "C" void sm64ds_gl_present(const GlScreens &s)
{
    const int *rect_a = s.a_top ? s.top_rect : s.bottom_rect;
    const int *rect_b = s.a_top ? s.bottom_rect : s.top_rect;
    if (rect_a[2] > 0) {
        if (s.a_ctl) {
            upload(g_a_off_tex, *s.a);
            upload(g_a_ctl_tex, *s.a_ctl);
        } else {
            upload(g_screen_tex[0], *s.a);
        }
    }
    if (rect_b[2] > 0) upload(g_screen_tex[1], *s.b);
    glBindTexture(GL_TEXTURE_2D, g_text_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.text_w, s.text_h, GL_BGRA, GL_UNSIGNED_BYTE, s.text);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    // The larger screen first, the smaller one over it: in Widescreen 16:9
    // one screen fills the display and the other is an inset in its corner.
    // While the game shows engine A on the touch screen (POWCNT bit 15 clear,
    // as at the title and file select), engine B is the full-screen one, and
    // drawn after A it covered the inset: the touch screen was missing.
    const bool b_first = rect_b[2] * rect_b[3] > rect_a[2] * rect_a[3];
    auto draw_b = [&] {
        glUseProgram(g_prog2d);
        glUniform1i(g_u_tex2d, 0);
        glActiveTexture(GL_TEXTURE0);
        if (rect_b[2] > 0)
            quad(g_screen_tex[1], (float)rect_b[0], (float)rect_b[1], (float)rect_b[2], (float)rect_b[3], 1, 1);
    };
    if (b_first) draw_b();
    if (rect_a[2] > 0 && s.a_ctl) {
        glUseProgram(g_progmix);
        glUniform1i(g_u_mix_off, 0);
        glUniform1i(g_u_mix_ctl, 1);
        glUniform1i(g_u_mix_3d, 2);
        glUniform4f(g_u_mix_b, s.mix_b[0], s.mix_b[1], s.mix_b[2], s.mix_b[3]);
        glUniform4f(g_u_mix_m, s.mix_m[0], s.mix_m[1], s.mix_m[2], s.mix_m[3]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_a_ctl_tex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, g_rt_tex);
        glActiveTexture(GL_TEXTURE0);
        quad(g_a_off_tex, (float)rect_a[0], (float)rect_a[1], (float)rect_a[2], (float)rect_a[3], 1, 1);
    }
    glUseProgram(g_prog2d);
    glUniform1i(g_u_tex2d, 0);
    glActiveTexture(GL_TEXTURE0);
    if (rect_a[2] > 0 && !s.a_ctl)
        quad(g_screen_tex[0], (float)rect_a[0], (float)rect_a[1], (float)rect_a[2], (float)rect_a[3], 1, 1);
    if (!b_first) draw_b();
    // the line over the picture: its unused part is transparent
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBlendEquation(GL_FUNC_ADD);
    quad(g_text_tex, 0, 0, (float)s.text_w, (float)s.text_h, (float)s.text_w / TEXT_W, (float)s.text_h / TEXT_H);
    if (s.menu) {               // the settings menu, in the middle, 1:1
        if (!g_menu_tex || g_menu_tw != s.menu_w || g_menu_th != s.menu_h) {
            if (g_menu_tex) glDeleteTextures(1, &g_menu_tex);
            g_menu_tex = make_texture(s.menu_w, s.menu_h, GL_NEAREST, true);
            g_menu_tw = s.menu_w;
            g_menu_th = s.menu_h;
            glBindTexture(GL_TEXTURE_2D, g_menu_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.menu_w, s.menu_h, GL_BGRA, GL_UNSIGNED_BYTE, s.menu);
        } else if (s.menu_changed) {
            glBindTexture(GL_TEXTURE_2D, g_menu_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, s.menu_w, s.menu_h, GL_BGRA, GL_UNSIGNED_BYTE, s.menu);
        }
        quad(g_menu_tex, (960.0f - s.menu_w) / 2, (544.0f - s.menu_h) / 2, (float)s.menu_w, (float)s.menu_h, 1, 1);
    }
    glDisable(GL_BLEND);
    vglSwapBuffers(GL_FALSE);
}

#endif  // __vita__ && SM64DS_VITAGL
