// A one-frame trace of the models drawn, for the heads (gate 11 on).
//
// Mario's, Luigi's and Wario's heads do not show and Yoshi's sits at his
// feet. Each head is its own model, pinned to the body's bone 15 (face_1):
// Player::Render copies the body's bone-15 matrix into the head's first bone
// and draws it, and Yoshi's head takes the same matrix through Virtual10.
// When a rear-panel capture is asked for, the next frame logs every
// ModelComponents::Render (the model's file and bone count, its matrix, the
// scale, bone 0 and bone 15), every explicit dExtSimpleModel_c::Render and
// every MulMat4x3Mat4x3, with their callers, to gate11.log. The callers are
// raw addresses: "mtrace base" gives one known function's to line them up
// with gate11.syms.
//
// The glitch watch (always on): every model drawn is sized up -- the longest
// axis of its world matrix, times its scale, times its largest bone -- and a
// model that grows or shrinks more than three times from one frame to the
// next is logged ("glitch?") with the actor code that drew it (the caller of
// dExtSimpleModel_c / dExtAnmModel_c::Render; shadows and the level are not
// watched), and, when rear-tap captures are on, a capture is taken
// by itself (at most six a session, ten seconds apart): the screens, the
// one-frame model trace and a snapshot holding the frames just before.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" {
void __real__ZN15ModelComponents6RenderEP9Matrix4x3P7Vector3(void *self, int *mat, int *scale);
void __real__ZN17dExtSimpleModel_c6RenderEPK7Vector3(void *self, const int *scale);
void __real_MulMat4x3Mat4x3(const int *a, const int *b, int *dst);
void __real__ZN14dExtAnmModel_c6RenderEPK7Vector3(void *self, const int *scale);

/* who asked dExtSimpleModel_c / dExtAnmModel_c to draw (an actor's Render):
   the model's own Render below them is only the shared helper */
static const void *g_outer;
static const char *g_outer_anm;     /* the dExtAnmModel_c being drawn, if one */
void __wrap_MulMat4x3Mat4x3(const int *a, const int *b, int *dst);

static int g_armed, g_lines;
static const int kMaxLines = 600;

static bool room()
{
    if (!g_armed) return false;
    if (++g_lines > kMaxLines) return false;
    return true;
}

bool sm64ds_rear_capture_enabled(void);          /* vita_display.cpp */

static uint32_t g_frame;
static int g_glitch_lines, g_auto_left = 6;
static uint32_t g_auto_last;
static char g_auto_why[160];
static const char *g_auto_pending;

static double axis_len(const int *m)               /* longest of the 3 axes */
{
    double best = 0;
    for (int c = 0; c < 3; ++c) {
        const double x = m[c * 3], y = m[c * 3 + 1], z = m[c * 3 + 2];
        const double l = sqrt(x * x + y * y + z * z) / 4096.0;
        if (l > best) best = l;
    }
    return best;
}

/* What the bones were made from, for a "glitch?" line: the animation's
   playback cursor (when a dExtAnmModel_c draws), and per bone the pose the
   samplers wrote (ModelComponents +8, 0x34 bytes a bone: scale at +0xc,
   flags +0x18, the three angles +0x1a, translation +0x20, a bone's own
   animation +0x2c and its frame +0x30) next to the bone matrix's axes. */
static void detail(const void *self, uint32_t nb)
{
    void *const *mc = static_cast<void *const *>(self);
    const char *elems = static_cast<const char *>(mc[2]);
    const int *tr = static_cast<const int *>(mc[3]);
    if (g_outer_anm && static_cast<const void *>(g_outer_anm + 8) == self) {
        const uint32_t nf = *(const uint32_t *)(g_outer_anm + 0x54);
        const int cur = *(const int *)(g_outer_anm + 0x58), sp = *(const int *)(g_outer_anm + 0x5c);
        const uint16_t *bca = *(const uint16_t *const *)(g_outer_anm + 0x60);
        printf("    anim: frame %d.%03d of %u (flags %x) speed %d bca %p", cur >> 12,
               (int)(((cur & 0xfff) * 1000) >> 12), (unsigned)(nf & 0x3fffffff), (unsigned)(nf >> 30), sp,
               (const void *)bca);
        if (bca) printf(" [bones %u frames %u loop %u]", bca[0], bca[1], bca[2]);
        printf("\n");
    }
    if (!elems || !tr) return;
    for (uint32_t b = 0; b < nb && b < 40; ++b) {
        const char *e = elems + b * 0x34;
        const int *sc = (const int *)(e + 0xc), *t = (const int *)(e + 0x20);
        const uint16_t *a = (const uint16_t *)(e + 0x18);
        printf("    bone %2u: axes %.2f scale %d %d %d flags %x angles %03x %03x %03x trans %d %d %d own %p@%u\n",
               (unsigned)b, axis_len(tr + b * 12), sc[0], sc[1], sc[2], a[0], a[1], a[2], a[3], t[0], t[1],
               t[2], *(void *const *)(e + 0x2c), *(const uint16_t *)(e + 0x30));
    }
}

struct Seen { const void *mc; float size; uint32_t frame; uint8_t multi; };
static Seen g_seen[2048];

static void watch(const void *self, const int *mat, const int *scale, const void *from)
{
    const uint32_t *file = static_cast<const uint32_t *>(static_cast<void *const *>(self)[0]);
    const int *tr = static_cast<const int *>(static_cast<void *const *>(self)[3]);
    if (!mat || !file || !tr) return;
    const uint32_t nb = file[1];
    double bones = 0;
    for (uint32_t b = 0; b < nb && b < 128; ++b) {
        const double l = axis_len(tr + b * 12);
        if (l > bones) bones = l;
    }
    double sc = 1;
    if (scale) {
        sc = 0;
        for (int k = 0; k < 3; ++k) { const double v = fabs(scale[k] / 4096.0); if (v > sc) sc = v; }
    }
    const double size = axis_len(mat) * sc * (nb ? bones : 1);
    // the model's entry: one Render a frame, compared with the frame before
    uint32_t h = (uint32_t)((uintptr_t)self >> 3) * 2654435761u >> 21;
    Seen *e = nullptr;
    for (int k = 0; k < 16; ++k) {
        Seen &c = g_seen[(h + k) & 2047];
        if (c.mc == self || !c.mc || g_frame - c.frame > 120) { e = &c; break; }
    }
    if (!e) return;
    const char *what = nullptr;
    double before = 0;
    if (e->mc == self && e->frame == g_frame) {
        e->multi = 1;                               /* drawn many times a frame */
        return;
    }
    if (e->mc == self && !e->multi && g_frame - e->frame <= 2 && e->size > 0.001) {
        before = e->size;
        const double r = size / before;
        if (r > 3) what = "grew";
        else if (r < 1.0 / 3) what = "shrank";
    }
    if (e->mc != self) e->multi = 0;
    e->mc = self;
    e->size = (float)size;
    e->frame = g_frame;
    if (!what) return;
    if (g_glitch_lines < 60) {
        if (!g_glitch_lines++)
            printf("  mtrace base %p (__wrap_MulMat4x3Mat4x3)\n", (void *)&__wrap_MulMat4x3Mat4x3);
        printf("  glitch? frame %u: model %p (file %p, %u bones) drawn from %p %s: size %.2f -> %.2f"
               " (matrix %.2f, scale %.2f, bones %.2f)\n", (unsigned)g_frame, self, (const void *)file,
               (unsigned)nb, from, what, before, size, axis_len(mat), sc, bones);
        printf("    matrix [%d %d %d|%d %d %d|%d %d %d|%d %d %d]\n", mat[0], mat[1], mat[2], mat[3],
               mat[4], mat[5], mat[6], mat[7], mat[8], mat[9], mat[10], mat[11]);
        detail(self, nb);
    }
    if (strcmp(what, "shrank") && g_auto_left > 0 && !g_auto_pending &&
        (g_auto_last == 0 || g_frame - g_auto_last > 600) && sm64ds_rear_capture_enabled()) {
        --g_auto_left;
        g_auto_last = g_frame;
        snprintf(g_auto_why, sizeof g_auto_why, "automatic: model %p drawn from %p %s to size %.2f",
                 self, from, what, size);
        g_auto_pending = g_auto_why;
    }
}

const char *sm64ds_glitch_capture_requested(void)
{
    const char *w = g_auto_pending;
    g_auto_pending = nullptr;
    return w;
}

static void put_m(const char *tag, const int *m)
{
    if (!m) { printf(" %s=null", tag); return; }
    printf(" %s=[%d %d %d|%d %d %d|%d %d %d|%d %d %d]", tag, m[0], m[1], m[2], m[3], m[4], m[5],
           m[6], m[7], m[8], m[9], m[10], m[11]);
}

void __wrap__ZN15ModelComponents6RenderEP9Matrix4x3P7Vector3(void *self, int *mat, int *scale)
{
    if (room()) {
        void **mc = static_cast<void **>(self);
        const uint32_t *file = static_cast<const uint32_t *>(mc[0]);
        const int *tr = static_cast<const int *>(mc[3]);
        const uint32_t nb = file ? file[1] : 0;
        printf("  mtrace MC %p file %p bones %u from %p", self, (void *)file, (unsigned)nb,
               __builtin_return_address(0));
        if (scale) printf(" scale %d %d %d", scale[0], scale[1], scale[2]);
        else printf(" scale null");
        put_m("mat", mat);
        put_m("b0", tr);
        if (tr && nb > 15) put_m("b15", tr + 15 * 12);
        printf("\n");
    }
    __real__ZN15ModelComponents6RenderEP9Matrix4x3P7Vector3(self, mat, scale);
    if (g_outer) watch(self, mat, scale, g_outer);
}

void __wrap__ZN17dExtSimpleModel_c6RenderEPK7Vector3(void *self, const int *scale)
{
    if (room()) {
        const int *obj = static_cast<const int *>(self);
        printf("  mtrace SM %p vt %p from %p", self, *(void **)self, __builtin_return_address(0));
        put_m("mat4x3", obj + 7);           /* +0x1c */
        printf("\n");
    }
    const void *was = g_outer;
    const char *was_anm = g_outer_anm;
    g_outer = __builtin_return_address(0);
    g_outer_anm = nullptr;
    __real__ZN17dExtSimpleModel_c6RenderEPK7Vector3(self, scale);
    g_outer = was;
    g_outer_anm = was_anm;
}

void __wrap__ZN14dExtAnmModel_c6RenderEPK7Vector3(void *self, const int *scale)
{
    const void *was = g_outer;
    const char *was_anm = g_outer_anm;
    g_outer = __builtin_return_address(0);
    g_outer_anm = static_cast<const char *>(self);
    __real__ZN14dExtAnmModel_c6RenderEPK7Vector3(self, scale);
    g_outer = was;
    g_outer_anm = was_anm;
}

void __wrap_MulMat4x3Mat4x3(const int *a, const int *b, int *dst)
{
    const bool log = room();
    if (log) {
        printf("  mtrace MM from %p a %p b %p d %p", __builtin_return_address(0), (const void *)a,
               (const void *)b, (void *)dst);
        put_m("a", a);
        put_m("b", b);
    }
    __real_MulMat4x3Mat4x3(a, b, dst);
    if (log) {
        put_m("d", dst);
        printf("\n");
    }
}

/* The frame loop: arm for the frame after a capture, then disarm. */
void sm64ds_model_trace_frame(bool arm)
{
    if (!arm) ++g_frame;
    if (!arm && g_armed && --g_armed == 0) {   /* two vblanks: one 30 fps game frame */
        printf("  mtrace end (%d lines)\n", g_lines);
        fflush(stdout);
        g_armed = 0;
    }
    if (arm) {
        printf("  mtrace base %p (__wrap_MulMat4x3Mat4x3)\n", (void *)&__wrap_MulMat4x3Mat4x3);
        g_armed = 2;
        g_lines = 0;
    }
}
}
