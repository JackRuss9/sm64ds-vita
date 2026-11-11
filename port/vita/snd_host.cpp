// The DS's sound, made on the host (gate 11 on).
//
// On the DS the ARM9 game only sends commands; the ARM7 runs the Nitro SDK's
// sound driver: sixteen sound channels (PCM8, PCM16, IMA-ADPCM, PSG square
// waves on 8-13, noise on 14-15), a sequence player per SSEQ (16 players, 16
// tracks), the banks' instruments (SBNK) and wave archives (SWAR), envelopes,
// LFOs and sweeps, all stepped every 2728 * 64 ARM7 cycles (~5.21 ms). The
// host has no ARM7: this is that driver, written from the formats and the
// SDK's documented behaviour, mixing straight to the Vita's 48 kHz stereo
// output on a thread of its own on the third core.
//
// The commands arrive as the ARM9 queued them (port/vita/arm7_host.cpp hands
// each list here): this SDK's numbering, src/engine/snd/SND_Interface.cpp.
// What the ARM9 reads back goes into the shared work it handed over
// (SND_Work.cpp): which players and channels are busy, each player's tick
// counter, and the sequence variables (players' and global), which live
// there.
//
// Not done yet: the capture units (the game's echo effect records the mix
// and plays it back on channels 1 and 3: those play nothing here, and the
// mix goes out as it is), the alarms (streamed sound), and the DS's exact
// volume and pitch quantization (here in floating point).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <mutex>

#include "ntr/mmio.h"

#if defined(__vita__)
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace {
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;

constexpr int OUT_RATE = 48000;
constexpr double ARM7_CLOCK = 33513982.0;
constexpr double FRAME_SECONDS = 2728.0 * 64.0 / ARM7_CLOCK;   // the driver's step, ~5.21 ms
constexpr double TIMER_CLOCK = ARM7_CLOCK / 2;                   // a channel timer's input
constexpr int CHANNELS = 16, PLAYERS = 16, TRACKS = 32;          // tracks: a pool, as the SDK's
constexpr s32 ENV_MIN = -92544;                                  // -72.3 dB, << 7

// A pointer the game handed over: a host address, or (in a table copied
// from the ROM) a DS one.
template <typename T> T *ptr(u32 a)
{
    if (!a) return nullptr;
    if (a >= 0x02000000u && a < 0x03000000u) return static_cast<T *>(::ntr::host_ptr(a));
    return reinterpret_cast<T *>(static_cast<uintptr_t>(a));
}
inline u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
inline u32 rd24(const u8 *p) { return p[0] | p[1] << 8 | (u32)p[2] << 16; }
inline u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

// --- the SDK's tables, as formulas ---
// volume 0..127 squared, in tenths of a decibel (-723 for silence)
s16 g_db_sq[128];
void init_tables()
{
    for (int v = 0; v < 128; ++v) {
        if (v == 0) { g_db_sq[v] = -723; continue; }
        const double d = 400.0 * log10(v / 127.0);
        g_db_sq[v] = (s16)(d < -723 ? -723 : lround(d));
    }
}
u16 attack_rate(int a)
{
    static const u8 tail[] = {0x00, 0x01, 0x05, 0x0E, 0x1A, 0x26, 0x33, 0x3F, 0x49,
                              0x54, 0x5C, 0x64, 0x6D, 0x74, 0x7B, 0x7F, 0x84, 0x89, 0x8F};
    if (a > 127) a = 127;
    return a < 0x6D ? (u16)(0xFF - a) : tail[0x7F - a];
}
u16 decay_rate(int d)       // also release
{
    if (d >= 0x7F) return 0xFFFF;
    if (d == 0x7E) return 0x3C00;
    if (d < 0x32) return (u16)(d * 2 + 1);
    return (u16)(0x1E00 / (0x7E - d));
}
inline float db_to_gain(int db) { return db <= -723 ? 0.0f : powf(10.0f, db / 200.0f); }

const s8 kIndexTable[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
s16 g_adpcm_step[89];
void init_adpcm()
{
    // IMA ADPCM's step table: 7, then x1.1 each, rounded as the standard's
    static const s16 t[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55,
                              60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
                              337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
                              1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
                              5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
                              20350, 22385, 24623, 27086, 29794, 32767};
    memcpy(g_adpcm_step, t, sizeof t);
}

// --- channels ---
enum ChType { CH_PCM8, CH_PCM16, CH_ADPCM, CH_PSG, CH_NOISE };
enum EnvState { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

struct Channel {
    bool active;
    bool seq;                   // played by a track (else set up directly)
    int type;                   // ChType
    const u8 *data;             // samples (PCM, ADPCM: after its 4-byte header)
    u32 loop_start, length;     // in samples
    bool loop;
    double timer;               // the DS timer value the pitch gives
    double pos;                 // in samples
    // ADPCM
    s32 ad_pred, ad_index, ad_pos;            // decoded up to sample ad_pos
    s32 ad_loop_pred, ad_loop_index;
    bool ad_loop_saved;
    s16 ad_cur, ad_prev;
    // PSG / noise
    int duty;
    u32 lfsr;
    // what the mix uses
    float gain_l, gain_r;
    // the sequence player's side
    int track;                  // owning track, -1 none
    int prio;
    int key, orig_key, velocity;
    int length_ticks;           // ticks left until release, -1 forever
    s32 env;                    // envelope level, tenths of dB << 7
    int env_state;
    u16 attack, decay, release;
    s32 sustain_level;
    int inst_pan;
    double base_timer;          // the instrument's timer at its original key
    int sweep_pitch, sweep_len, sweep_count;   // pitch sweep, 1/64 semitones, frames
    int lfo_count;              // frames since start (LFO delay) and phase
    u32 lfo_phase;
    // directly set up
    int direct_vol, direct_shift, direct_pan;
    bool direct_set;
};

struct Lfo { u8 target, speed, depth, range; u16 delay; };

struct Track {
    bool active, muted, note_wait, tie, porta, cmp_flag;
    int player;
    const u8 *base;             // the sequence's base: offsets count from it
    u32 pc;
    int wait;
    u16 prg;
    u8 volume, volume2;
    s8 pitch_bend;
    u8 bend_range;
    int pan;                    // 0..127
    int ext_pan;
    s16 ext_fader, ext_pitch;
    u8 attack, decay, sustain, release;   // 0xff: the instrument's
    u8 prio;
    s8 transpose;
    u8 porta_key, porta_time;
    s16 sweep_pitch;
    Lfo lfo;
    u32 call_stack[3];
    u8 loop_count[3];
    int stack_depth;
    int last_channel;           // for ties
    bool allocated;
};

struct Player {
    bool active, paused;
    u8 prio, volume;
    s16 ext_fader;
    u16 tempo, tempo_ratio;
    u32 tempo_counter;
    const u8 *bank;
    s8 tracks[16];
    u32 ticks;
};

struct Driver {
    Channel ch[CHANNELS];
    Player pl[PLAYERS];
    Track tr[TRACKS];
    u16 locked;
    u32 *shared;                // SNDSharedWork
    const u8 *capture_buf[2];
    double frame_acc;
    u32 rng = 0x12345678;
    // statistics for the log
    unsigned seqs_started, notes, unknown_ops, frames;
    u32 unknown_op_seen[8];
} g;
std::mutex g_lock;
unsigned g_track_fail, g_end_logs, g_start_logs;      // for the log: track pool full, multi-track sequences ending

u32 rnd() { g.rng = g.rng * 1664525u + 1013904223u; return g.rng >> 8; }

// the shared work: variables live there
s16 *var_ptr(int player, int idx)
{
    if (!g.shared) return nullptr;
    u8 *sw = reinterpret_cast<u8 *>(g.shared);
    if (idx < 16) return reinterpret_cast<s16 *>(sw + 0x20 + player * 0x24 + idx * 2);
    if (idx < 32) return reinterpret_cast<s16 *>(sw + 0x260 + (idx - 16) * 2);
    return nullptr;
}

// --- banks and waves ---
struct Inst {
    int type;           // 1 PCM, 2 PSG, 3 noise, 4 direct PCM, 5 null
    u16 wave, arc;      // PCM: wave and archive; PSG: wave = duty
    u8 orig_key, attack, decay, sustain, release, pan;
};
bool read_inst_param(const u8 *p, int type, Inst &out)
{
    out.type = type;
    out.wave = rd16(p);
    out.arc = rd16(p + 2);
    out.orig_key = p[4];
    out.attack = p[5];
    out.decay = p[6];
    out.sustain = p[7];
    out.release = p[8];
    out.pan = p[9];
    return type >= 1 && type <= 4;
}
bool find_inst(const u8 *bank, int prg, int key, Inst &out)
{
    if (!bank) return false;
    const u32 count = rd32(bank + 0x38);
    if ((u32)prg >= count) return false;
    const u32 io = rd32(bank + 0x3c + prg * 4);
    const int type = io & 0xff;
    const u8 *p = bank + (io >> 8);
    if (type >= 1 && type <= 5) return read_inst_param(p, type, out);
    if (type == 16) {                                   // drum set: a range of keys
        const int lo = p[0], hi = p[1];
        if (key < lo || key > hi) return false;
        const u8 *e = p + 2 + (key - lo) * 12;
        return read_inst_param(e + 2, e[0], out);
    }
    if (type == 17) {                                   // key split: up to 8 regions
        int i = 0;
        while (i < 8 && p[i] && key > p[i]) ++i;
        if (i >= 8 || !p[i]) return false;
        const u8 *e = p + 8 + i * 12;
        return read_inst_param(e + 2, e[0], out);
    }
    return false;
}
// a wave of a bank's archive: its 12-byte parameters, then samples
const u8 *find_wave(const u8 *bank, int arc, int wave)
{
    if (!bank || arc < 0 || arc > 3) return nullptr;
    const u8 *wa = ptr<const u8>(rd32(bank + 0x18 + arc * 8));
    if (!wa) return nullptr;
    if ((u32)wave >= rd32(wa + 0x38)) return nullptr;
    const u32 off = rd32(wa + 0x3c + wave * 4);
    if (!off) return nullptr;
    return off < 0x02000000u ? wa + off : ptr<const u8>(off);
}

// --- channel set-up ---
void stop_channel(int i)
{
    Channel &c = g.ch[i];
    c.active = false;
    if (c.track >= 0 && g.tr[c.track].last_channel == i) g.tr[c.track].last_channel = -1;
    c.track = -1;
}

void start_wave(Channel &c, const u8 *wave)
{
    const u8 fmt = wave[0];
    c.loop = wave[1] != 0;
    const u16 timer = rd16(wave + 4);
    const u32 loop_start_w = rd16(wave + 6), loop_len_w = rd32(wave + 8);
    const u8 *data = wave + 12;
    c.base_timer = timer ? timer : 1;
    c.pos = 0;
    c.type = fmt == 0 ? CH_PCM8 : fmt == 1 ? CH_PCM16 : CH_ADPCM;
    if (c.type == CH_ADPCM) {
        c.ad_pred = (s16)rd16(data);
        c.ad_index = data[2] > 88 ? 88 : data[2];
        c.ad_pos = 0;
        c.ad_cur = c.ad_prev = (s16)c.ad_pred;
        c.ad_loop_saved = false;
        c.data = data + 4;
        // in words, counting the header word
        c.loop_start = loop_start_w ? (loop_start_w - 1) * 8 : 0;
        c.length = c.loop_start + loop_len_w * 8;
    } else if (c.type == CH_PCM8) {
        c.data = data;
        c.loop_start = loop_start_w * 4;
        c.length = c.loop_start + loop_len_w * 4;
    } else {
        c.data = data;
        c.loop_start = loop_start_w * 2;
        c.length = c.loop_start + loop_len_w * 2;
    }
}

// --- the mix ---
inline s16 adpcm_step(Channel &c, u8 nib)
{
    const s32 step = g_adpcm_step[c.ad_index];
    s32 diff = step >> 3;
    if (nib & 1) diff += step >> 2;
    if (nib & 2) diff += step >> 1;
    if (nib & 4) diff += step;
    if (nib & 8) c.ad_pred -= diff;
    else c.ad_pred += diff;
    if (c.ad_pred > 32767) c.ad_pred = 32767;
    if (c.ad_pred < -32768) c.ad_pred = -32768;
    c.ad_index += kIndexTable[nib & 7];
    if (c.ad_index < 0) c.ad_index = 0;
    if (c.ad_index > 88) c.ad_index = 88;
    return (s16)c.ad_pred;
}

/* One output sample of channel `c`, -1..1, and advance it; false when the
   channel has ended. */
inline bool channel_sample(Channel &c, float &out)
{
    const double rate = TIMER_CLOCK / c.timer / OUT_RATE;    // source samples per output sample
    switch (c.type) {
    case CH_PSG: {
        // eight steps a period, `duty` + 1 of them high
        const int step = ((int)c.pos) & 7;
        out = step > 6 - c.duty ? 0.5f : -0.5f;   // roughly the DS's level
        if (c.duty >= 7) out = -0.5f;
        c.pos += rate;
        if (c.pos >= 8.0 * 65536) c.pos -= 8.0 * 65536;
        return true;
    }
    case CH_NOISE: {
        out = (c.lfsr & 1) ? -0.5f : 0.5f;
        const double np = c.pos + rate;
        for (int k = (int)c.pos; k < (int)np; ++k)
            c.lfsr = (c.lfsr & 1) ? (c.lfsr >> 1) ^ 0x6000 : c.lfsr >> 1;
        c.pos = np;
        if (c.pos > 1e9) c.pos -= 1e9;
        return true;
    }
    default: break;
    }
    u32 i = (u32)c.pos;
    if (c.length && i >= c.length) {
        if (!c.loop || c.length <= c.loop_start) return false;
        const double over = c.pos - c.length;
        c.pos = c.loop_start + fmod(over, (double)(c.length - c.loop_start));
        i = (u32)c.pos;
        if (c.type == CH_ADPCM && c.ad_loop_saved) {
            c.ad_pred = c.ad_loop_pred;
            c.ad_index = c.ad_loop_index;
            c.ad_pos = c.loop_start;
            c.ad_cur = c.ad_prev = (s16)c.ad_pred;
        }
    }
    float s;
    if (c.type == CH_PCM8) {
        s = (s8)c.data[i] / 128.0f;
    } else if (c.type == CH_PCM16) {
        s = (s16)rd16(c.data + i * 2) / 32768.0f;
    } else {
        while ((u32)c.ad_pos <= i) {
            if ((u32)c.ad_pos == c.loop_start && !c.ad_loop_saved) {
                c.ad_loop_pred = c.ad_pred;
                c.ad_loop_index = c.ad_index;
                c.ad_loop_saved = true;
            }
            const u8 b = c.data[c.ad_pos >> 1];
            c.ad_prev = c.ad_cur;
            c.ad_cur = adpcm_step(c, (c.ad_pos & 1) ? b >> 4 : b & 15);
            ++c.ad_pos;
        }
        s = c.ad_cur / 32768.0f;
    }
    out = s;
    c.pos += rate;
    return true;
}

// --- tracks ---
int alloc_track()
{
    for (int i = 0; i < TRACKS; ++i)
        if (!g.tr[i].allocated) return i;
    return -1;
}

void init_track(Track &t, int player, const u8 *base, u32 pc)
{
    memset(&t, 0, sizeof t);
    t.allocated = true;
    t.active = true;
    t.player = player;
    t.base = base;
    t.pc = pc;
    t.note_wait = true;
    t.volume = 127;
    t.volume2 = 127;
    t.bend_range = 2;
    t.pan = 64;
    t.attack = t.decay = t.sustain = t.release = 0xff;
    t.prio = 64;
    t.porta_key = 60;
    t.lfo.range = 1;
    t.lfo.speed = 16;
    t.last_channel = -1;
}

void release_track_channels(int ti, bool kill)
{
    for (int i = 0; i < CHANNELS; ++i) {
        Channel &c = g.ch[i];
        if (!c.active || c.track != ti) continue;
        if (kill) stop_channel(i);
        else {
            c.env_state = ENV_RELEASE;
            c.length_ticks = -1;
        }
    }
}

void stop_player(int p)
{
    Player &pl = g.pl[p];
    for (int k = 0; k < 16; ++k) {
        const int ti = pl.tracks[k];
        if (ti < 0) continue;
        release_track_channels(ti, true);
        g.tr[ti].allocated = false;
        g.tr[ti].active = false;
        pl.tracks[k] = -1;
    }
    pl.active = false;
}

/* A channel for a note of type `type` at priority `prio`: a free one in the
   allowed set, else the lowest-priority one playing (releasing first). */
int alloc_channel(int type, int prio)
{
    u16 mask = type == CH_PSG ? 0x3F00 : type == CH_NOISE ? 0xC000 : 0xFFFF;
    mask &= (u16)~g.locked;
    int best = -1, best_prio = 1 << 30;
    for (int i = 0; i < CHANNELS; ++i) {
        if (!(mask & (1u << i))) continue;
        const Channel &c = g.ch[i];
        if (!c.active) return i;
        if (!c.seq) continue;
        int pr = c.prio;
        if (c.env_state == ENV_RELEASE) pr -= 1000;     // releasing ones go first
        if (pr < best_prio) { best_prio = pr; best = i; }
    }
    if (best >= 0 && best_prio <= prio) {
        stop_channel(best);
        return best;
    }
    return -1;
}

void note_on(int ti, int key, int velocity, int length)
{
    Track &t = g.tr[ti];
    Player &pl = g.pl[t.player];
    key += t.transpose;
    if (key < 0) key = 0;
    if (key > 127) key = 127;
    if (t.muted) return;
    // a tie: the note before carries on, at the new key
    if (t.tie && t.last_channel >= 0 && g.ch[t.last_channel].active && g.ch[t.last_channel].track == ti) {
        Channel &c = g.ch[t.last_channel];
        c.key = key;
        c.velocity = velocity;
        c.length_ticks = length;
        return;
    }
    Inst in;
    if (!find_inst(pl.bank, t.prg, key, in)) return;
    const int type = in.type == 2 ? CH_PSG : in.type == 3 ? CH_NOISE : CH_PCM16;
    const int prio = pl.prio + t.prio;
    const int ci = alloc_channel(type, prio);
    if (ci < 0) return;
    Channel &c = g.ch[ci];
    memset(&c, 0, sizeof c);
    c.track = ti;
    c.seq = true;
    c.prio = prio;
    if (in.type == 1 || in.type == 4) {
        const u8 *w = in.type == 1 ? find_wave(pl.bank, in.arc, in.wave)
                                   : ptr<const u8>((u32)in.wave | (u32)in.arc << 16);
        if (!w) return;
        start_wave(c, w);
    } else if (in.type == 2) {
        c.type = CH_PSG;
        c.duty = in.wave & 7;
        c.base_timer = 8006;
    } else if (in.type == 3) {
        c.type = CH_NOISE;
        c.lfsr = 0x7FFF;
        c.base_timer = 8006;
    } else {
        return;
    }
    c.key = key;
    c.orig_key = in.orig_key;
    c.velocity = velocity;
    c.length_ticks = length;
    c.attack = attack_rate(t.attack != 0xff ? t.attack : in.attack);
    c.decay = decay_rate(t.decay != 0xff ? t.decay : in.decay);
    c.release = decay_rate(t.release != 0xff ? t.release : in.release);
    c.sustain_level = (s32)g_db_sq[t.sustain != 0xff ? t.sustain : in.sustain] << 7;
    c.inst_pan = in.pan;
    c.env = ENV_MIN;
    c.env_state = ENV_ATTACK;
    // the pitch sweep: from the portamento key, or the track's sweep
    int sweep = t.sweep_pitch;
    if (t.porta) sweep += (t.porta_key - key) * 64;
    c.sweep_pitch = sweep;
    if (t.porta && t.porta_time) {
        const int s = sweep < 0 ? -sweep : sweep;
        c.sweep_len = (t.porta_time * t.porta_time * s) >> 11;
    } else {
        c.sweep_len = length > 0 ? length : 0;
    }
    c.sweep_count = 0;
    t.porta_key = (u8)key;
    c.timer = c.base_timer;
    c.active = true;
    t.last_channel = ci;
    ++g.notes;
}

// a variable-length number (7 bits a byte, high bit: more)
u32 read_varlen(Track &t)
{
    u32 v = 0;
    for (int i = 0; i < 4; ++i) {
        const u8 b = t.base[t.pc++];
        v = v << 7 | (b & 0x7f);
        if (!(b & 0x80)) break;
    }
    return v;
}

enum ArgType { ARG_U8, ARG_S16, ARG_VARLEN, ARG_RANDOM, ARG_VARIABLE };

s32 read_arg(Track &t, int type)
{
    switch (type) {
    case ARG_U8: return t.base[t.pc++];
    case ARG_S16: { const s16 v = (s16)rd16(t.base + t.pc); t.pc += 2; return v; }
    case ARG_VARLEN: return (s32)read_varlen(t);
    case ARG_RANDOM: {
        const s16 lo = (s16)rd16(t.base + t.pc), hi = (s16)rd16(t.base + t.pc + 2);
        t.pc += 4;
        return lo + (s32)(rnd() % (u32)(hi - lo + 1 > 0 ? hi - lo + 1 : 1));
    }
    case ARG_VARIABLE: {
        const u8 idx = t.base[t.pc++];
        const s16 *v = var_ptr(t.player, idx);
        return v ? *v : 0;
    }
    }
    return 0;
}

/* One track's commands up to its next wait. */
void run_track(int ti)
{
    Track &t = g.tr[ti];
    for (int guard = 0; t.active && t.wait <= 0 && guard < 10000; ++guard) {
        u8 op = t.base[t.pc++];
        bool run = true;
        int arg_type = -1;          // a prefix's override of the last argument
        if (op == 0xA2) {           // if: only when the last comparison held
            run = t.cmp_flag;
            op = t.base[t.pc++];
        }
        if (op == 0xA0) { arg_type = ARG_RANDOM; op = t.base[t.pc++]; }
        else if (op == 0xA1) { arg_type = ARG_VARIABLE; op = t.base[t.pc++]; }
        auto arg = [&](int def) { return read_arg(t, arg_type >= 0 ? arg_type : def); };

        if (op < 0x80) {            // note: velocity, length
            const int vel = t.base[t.pc++];
            const int len = arg(ARG_VARLEN);
            if (!run) continue;
            note_on(ti, op, vel, len);
            if (t.note_wait) t.wait = len;
            continue;
        }
        switch (op) {
        case 0x80: { const int w = arg(ARG_VARLEN); if (run) t.wait = w; break; }
        case 0x81: { const int p = arg(ARG_VARLEN); if (run) t.prg = (u16)p; break; }
        case 0x93: {                // open a track: number, offset
            const int n = t.base[t.pc++];
            const u32 off = rd24(t.base + t.pc);
            t.pc += 3;
            if (!run) break;
            Player &pl = g.pl[t.player];
            if (n < 16 && pl.tracks[n] < 0) {
                const int nt = alloc_track();
                if (nt < 0) ++g_track_fail;
                if (nt >= 0) {
                    init_track(g.tr[nt], t.player, t.base, off);
                    pl.tracks[n] = (s8)nt;
                }
            }
            break;
        }
        case 0x94: { const u32 off = rd24(t.base + t.pc); t.pc += 3; if (run) t.pc = off; break; }
        case 0x95: {
            const u32 off = rd24(t.base + t.pc);
            t.pc += 3;
            if (run && t.stack_depth < 3) {
                t.call_stack[t.stack_depth] = t.pc;
                t.loop_count[t.stack_depth] = 0xff;
                ++t.stack_depth;
                t.pc = off;
            }
            break;
        }
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6:
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: {
            const int idx = t.base[t.pc++];
            const s32 v = arg(ARG_S16);
            if (!run) break;
            s16 *var = var_ptr(t.player, idx);
            if (!var) break;
            s32 x = *var;
            switch (op) {
            case 0xB0: x = v; break;
            case 0xB1: x += v; break;
            case 0xB2: x -= v; break;
            case 0xB3: x *= v; break;
            case 0xB4: if (v) x /= v; break;
            case 0xB5: x = v >= 0 ? x << v : x >> -v; break;
            case 0xB6: {
                const s32 r = (s32)(rnd() % (u32)((v < 0 ? -v : v) + 1));
                x = v < 0 ? -r : r;
                break;
            }
            case 0xB8: t.cmp_flag = x == v; break;
            case 0xB9: t.cmp_flag = x >= v; break;
            case 0xBA: t.cmp_flag = x > v; break;
            case 0xBB: t.cmp_flag = x <= v; break;
            case 0xBC: t.cmp_flag = x < v; break;
            case 0xBD: t.cmp_flag = x != v; break;
            }
            if (op < 0xB8) *var = (s16)x;
            break;
        }
        case 0xC0: { const int v = arg(ARG_U8); if (run) t.pan = v; break; }
        case 0xC1: { const int v = arg(ARG_U8); if (run) t.volume = (u8)v; break; }
        case 0xC2: { const int v = arg(ARG_U8); if (run) g.pl[t.player].volume = (u8)v; break; }
        case 0xC3: { const int v = arg(ARG_U8); if (run) t.transpose = (s8)v; break; }
        case 0xC4: { const int v = arg(ARG_U8); if (run) t.pitch_bend = (s8)v; break; }
        case 0xC5: { const int v = arg(ARG_U8); if (run) t.bend_range = (u8)v; break; }
        case 0xC6: { const int v = arg(ARG_U8); if (run) t.prio = (u8)v; break; }
        case 0xC7: { const int v = arg(ARG_U8); if (run) t.note_wait = v != 0; break; }
        case 0xC8: {
            const int v = arg(ARG_U8);
            if (!run) break;
            t.tie = v != 0;
            release_track_channels(ti, false);
            t.last_channel = -1;
            break;
        }
        case 0xC9: { const int v = arg(ARG_U8); if (run) { t.porta_key = (u8)(v + t.transpose); t.porta = true; } break; }
        case 0xCA: { const int v = arg(ARG_U8); if (run) t.lfo.depth = (u8)v; break; }
        case 0xCB: { const int v = arg(ARG_U8); if (run) t.lfo.speed = (u8)v; break; }
        case 0xCC: { const int v = arg(ARG_U8); if (run) t.lfo.target = (u8)v; break; }
        case 0xCD: { const int v = arg(ARG_U8); if (run) t.lfo.range = (u8)v; break; }
        case 0xCE: { const int v = arg(ARG_U8); if (run) t.porta = v != 0; break; }
        case 0xCF: { const int v = arg(ARG_U8); if (run) t.porta_time = (u8)v; break; }
        case 0xD0: { const int v = arg(ARG_U8); if (run) t.attack = (u8)v; break; }
        case 0xD1: { const int v = arg(ARG_U8); if (run) t.decay = (u8)v; break; }
        case 0xD2: { const int v = arg(ARG_U8); if (run) t.sustain = (u8)v; break; }
        case 0xD3: { const int v = arg(ARG_U8); if (run) t.release = (u8)v; break; }
        case 0xD4: {                // loop start: count (0 for ever)
            const int v = arg(ARG_U8);
            if (run && t.stack_depth < 3) {
                t.call_stack[t.stack_depth] = t.pc;
                t.loop_count[t.stack_depth] = (u8)v;
                ++t.stack_depth;
            }
            break;
        }
        case 0xD5: { const int v = arg(ARG_U8); if (run) t.volume2 = (u8)v; break; }
        case 0xD6: { const int v = arg(ARG_U8); (void)v; break; }   // print a variable: debug
        case 0xE0: { const int v = arg(ARG_S16); if (run) t.lfo.delay = (u16)v; break; }
        case 0xE1: { const int v = arg(ARG_S16); if (run) g.pl[t.player].tempo = (u16)v; break; }
        case 0xE3: { const int v = arg(ARG_S16); if (run) t.sweep_pitch = (s16)v; break; }
        case 0xFC: {                // loop end
            if (!run || t.stack_depth == 0) break;
            const int d = t.stack_depth - 1;
            if (t.loop_count[d] == 0xff) break;     // a call's frame, not a loop
            if (t.loop_count[d] == 0) { t.pc = t.call_stack[d]; break; }
            if (--t.loop_count[d] == 0) { --t.stack_depth; break; }
            t.pc = t.call_stack[d];
            break;
        }
        case 0xFD: {                // return
            if (!run) break;
            while (t.stack_depth > 0 && t.loop_count[t.stack_depth - 1] != 0xff) --t.stack_depth;
            if (t.stack_depth == 0) break;
            --t.stack_depth;
            t.pc = t.call_stack[t.stack_depth];
            break;
        }
        case 0xFE: t.pc += 2; break;  // the tracks a sequence uses: tracks open on 0x93
        case 0xFF:                    // end of the track
            if (!run) break;
            t.active = false;
            break;
        default: {
            if (g.unknown_ops < 8) g.unknown_op_seen[g.unknown_ops] = op;
            ++g.unknown_ops;
            t.active = false;       // an unknown command: stop rather than run garbage
            break;
        }
        }
    }
}

/* The ticks of one driver frame for every player, by its tempo. */
void step_players()
{
    for (int p = 0; p < PLAYERS; ++p) {
        Player &pl = g.pl[p];
        if (!pl.active || pl.paused) continue;
        pl.tempo_counter += (u32)pl.tempo * pl.tempo_ratio / 256;
        while (pl.tempo_counter >= 240) {
            pl.tempo_counter -= 240;
            ++pl.ticks;
            // the channels' note lengths
            for (int i = 0; i < CHANNELS; ++i) {
                Channel &c = g.ch[i];
                if (!c.active || !c.seq || c.track < 0 || g.tr[c.track].player != p) continue;
                if (c.length_ticks > 0 && --c.length_ticks == 0 && c.env_state != ENV_RELEASE)
                    c.env_state = ENV_RELEASE;
            }
            bool any = false;
            for (int k = 0; k < 16; ++k) {
                const int ti = pl.tracks[k];
                if (ti < 0) continue;
                Track &t = g.tr[ti];
                if (t.active) {
                    if (t.wait > 0) --t.wait;
                    if (t.wait <= 0) run_track(ti);
                }
                if (t.active) any = true;
            }
            if (!any) {
                // every track has ended: the player stops once its notes have
                bool sounding = false;
                for (int i = 0; i < CHANNELS; ++i)
                    if (g.ch[i].active && g.ch[i].seq && g.ch[i].track >= 0 && g.tr[g.ch[i].track].player == p)
                        sounding = true;
                if (!sounding) {
                    int nt = 0;
                    for (int k = 0; k < 16; ++k) nt += pl.tracks[k] >= 0;
                    if (nt > 2 && g_end_logs < 50) {
                        ++g_end_logs;
                        printf("  [sound] player %d's sequence ended by itself at tick %u (%d tracks)\n", p,
                               (unsigned)pl.ticks, nt);
                    }
                    stop_player(p);
                    break;
                }
            }
        }
    }
}

/* A frame's envelope, pitch, volume and pan for every sequence channel. */
void update_channels()
{
    for (int i = 0; i < CHANNELS; ++i) {
        Channel &c = g.ch[i];
        if (!c.active) continue;
        if (!c.seq) {
            // set up directly: its own volume and pan
            static const float kShift[4] = {1.0f, 0.5f, 0.25f, 1.0f / 16};
            const float v = c.direct_vol / 128.0f * kShift[c.direct_shift & 3];
            c.gain_l = v * (128 - c.direct_pan) / 128.0f;
            c.gain_r = v * c.direct_pan / 128.0f;
            continue;
        }
        // the envelope
        switch (c.env_state) {
        case ENV_ATTACK:
            c.env = (s32)(((int64_t)c.attack * c.env) >> 8);
            if (c.env >= -128 || c.attack == 0) { c.env = 0; c.env_state = ENV_DECAY; }
            break;
        case ENV_DECAY:
            c.env -= c.decay;
            if (c.env <= c.sustain_level) { c.env = c.sustain_level; c.env_state = ENV_SUSTAIN; }
            break;
        case ENV_SUSTAIN: break;
        case ENV_RELEASE:
            c.env -= c.release;
            if (c.env <= ENV_MIN) { stop_channel(i); continue; }
            break;
        }
        if (c.track < 0) { stop_channel(i); continue; }
        const Track &t = g.tr[c.track];
        const Player &pl = g.pl[t.player];
        // the LFO
        float lfo = 0;
        ++c.lfo_count;
        if (t.lfo.depth && c.lfo_count > t.lfo.delay) {
            c.lfo_phase += (u32)t.lfo.speed << 6;
            lfo = sinf((c.lfo_phase & 0x7fff) / 32768.0f * 6.2831853f) * t.lfo.depth / 128.0f * t.lfo.range;
        }
        // pitch, 1/64 semitone
        int pitch = (c.key - c.orig_key) * 64 + t.pitch_bend * t.bend_range * 64 / 128 + t.ext_pitch;
        if (c.sweep_pitch && c.sweep_len > 0) {
            if (c.sweep_count < c.sweep_len) {
                pitch += (int)((int64_t)c.sweep_pitch * (c.sweep_len - c.sweep_count) / c.sweep_len);
                ++c.sweep_count;
            }
        } else if (c.sweep_pitch && c.sweep_len == 0) {
            pitch += c.sweep_pitch;
        }
        if (t.lfo.target == 0) pitch += (int)(lfo * 64);
        c.timer = c.base_timer * pow(2.0, -pitch / 768.0);
        if (c.timer < 16) c.timer = 16;
        // volume, tenths of a dB
        int db = g_db_sq[c.velocity] + g_db_sq[t.volume] + g_db_sq[t.volume2] + g_db_sq[pl.volume] + t.ext_fader +
                 pl.ext_fader + (c.env >> 7);
        if (t.lfo.target == 1) db += (int)(lfo * 60);
        if (db < -723) db = -723;
        if (db > 0) db = 0;
        // pan, 0..127
        int pan = t.pan + t.ext_pan + (c.inst_pan - 64);
        if (t.lfo.target == 2) pan += (int)(lfo * 64);
        if (pan < 0) pan = 0;
        if (pan > 127) pan = 127;
        const float gain = db_to_gain(db);
        c.gain_l = gain * (128 - pan) / 128.0f;
        c.gain_r = gain * pan / 128.0f;
    }
}

void write_status()
{
    if (!g.shared) return;
    u32 ps = 0;
    u16 cs = 0;
    for (int p = 0; p < PLAYERS; ++p)
        if (g.pl[p].active) ps |= 1u << p;
    for (int i = 0; i < CHANNELS; ++i)
        if (g.ch[i].active) cs |= (u16)(1u << i);
    u8 *sw = reinterpret_cast<u8 *>(g.shared);
    memcpy(sw + 4, &ps, 4);
    memcpy(sw + 8, &cs, 2);
    for (int p = 0; p < PLAYERS; ++p) memcpy(sw + 0x20 + p * 0x24 + 0x20, &g.pl[p].ticks, 4);
}

void driver_frame()
{
    step_players();
    update_channels();
    write_status();
    ++g.frames;
}

// --- commands ---
enum {
    CMD_START_SEQ = 0x00, CMD_STOP_SEQ = 0x01, CMD_PAUSE_SEQ = 0x02, CMD_PLAYER_PARAM = 0x03,
    CMD_TRACK_PARAM = 0x04, CMD_MUTE_TRACK = 0x05, CMD_START_TIMER = 0x09, CMD_STOP_TIMER = 0x0a,
    CMD_SETUP_CHANNEL_PCM = 0x0b, CMD_SETUP_CAPTURE = 0x0e, CMD_SETUP_ALARM = 0x0f, CMD_CHANNEL_VOLUME = 0x11,
    CMD_SURROUND_DECAY = 0x13, CMD_OUTPUT_SELECTOR = 0x15, CMD_LOCK_CHANNEL = 0x16, CMD_UNLOCK_CHANNEL = 0x17,
    CMD_SET_SHARED_WORK = 0x19,
};

void start_seq(int p, const u8 *base, u32 offset, const u8 *bank)
{
    if (p < 0 || p >= PLAYERS || !base) return;
    stop_player(p);
    Player &pl = g.pl[p];
    pl.active = true;
    pl.paused = false;
    pl.volume = 127;
    pl.ext_fader = 0;
    pl.tempo = 120;
    pl.tempo_ratio = 256;
    pl.tempo_counter = 240;     // the first tick at once
    pl.bank = bank;
    pl.ticks = 0;
    if (pl.prio == 0) pl.prio = 64;
    for (int k = 0; k < 16; ++k) pl.tracks[k] = -1;
    const int ti = alloc_track();
    if (ti < 0) { ++g_track_fail; pl.active = false; return; }
    init_track(g.tr[ti], p, base, offset);
    pl.tracks[0] = (s8)ti;
    ++g.seqs_started;
}

void player_param(int p, u32 off, u32 data, int size)
{
    if (p < 0 || p >= PLAYERS) return;
    Player &pl = g.pl[p];
    (void)size;
    switch (off) {
    case 0x04: pl.prio = (u8)data; break;
    case 0x05: pl.volume = (u8)data; break;
    case 0x06: pl.ext_fader = (s16)data; break;
    case 0x18: pl.tempo = (u16)data; break;
    case 0x1a: pl.tempo_ratio = (u16)data; break;
    default: break;
    }
}

void track_param(int p, u32 mask, u32 off, u32 data)
{
    if (p < 0 || p >= PLAYERS) return;
    for (int k = 0; k < 16; ++k) {
        if (!(mask & (1u << k)) || g.pl[p].tracks[k] < 0) continue;
        Track &t = g.tr[g.pl[p].tracks[k]];
        switch (off) {
        case 0x09: t.ext_pan = (s8)data; break;
        case 0x0a: t.ext_fader = (s16)data; break;
        case 0x0c: t.ext_pitch = (s16)data; break;
        case 0x18: t.lfo.target = (u8)data; break;
        case 0x19: t.lfo.speed = (u8)data; break;
        case 0x1a: t.lfo.depth = (u8)data; break;
        case 0x1b: t.lfo.range = (u8)data; break;
        default: break;
        }
    }
}

void command(int id, const u32 *a)
{
    switch (id) {
    case CMD_START_SEQ:
        if (g_start_logs < 120) {
            ++g_start_logs;
            printf("  [sound] player %u starts a sequence (data %08x + %x, bank %08x)%s\n", (unsigned)a[0],
                   (unsigned)a[1], (unsigned)a[2], (unsigned)a[3],
                   a[0] < PLAYERS && g.pl[a[0]].active ? ", replacing the one playing" : "");
        }
        start_seq((int)a[0], ptr<const u8>(a[1]), a[2], ptr<const u8>(a[3]));
        break;
    case CMD_STOP_SEQ:
        if (a[0] < PLAYERS) {
            int nt = 0;
            for (int k = 0; k < 16; ++k) nt += g.pl[a[0]].tracks[k] >= 0;
            if (nt > 2 && g_end_logs < 50) {
                ++g_end_logs;
                printf("  [sound] player %u's sequence stopped by the game at tick %u (%d tracks)\n",
                       (unsigned)a[0], (unsigned)g.pl[a[0]].ticks, nt);
            }
            stop_player((int)a[0]);
        }
        break;
    case CMD_PAUSE_SEQ:
        if (a[0] < PLAYERS) {
            g.pl[a[0]].paused = a[1] != 0;
            if (a[1])     // paused: its notes go quiet
                for (int k = 0; k < 16; ++k)
                    if (g.pl[a[0]].tracks[k] >= 0) release_track_channels(g.pl[a[0]].tracks[k], false);
        }
        break;
    case CMD_PLAYER_PARAM: player_param((int)a[0], a[1], a[2], (int)a[3]); break;
    case CMD_TRACK_PARAM: track_param((int)(a[0] & 0xffffff), a[1], a[2], a[3]); break;
    case CMD_MUTE_TRACK:
        if (a[0] < PLAYERS)
            for (int k = 0; k < 16; ++k) {
                const int ti = g.pl[a[0]].tracks[k];
                if (!(a[1] & (1u << k)) || ti < 0) continue;
                g.tr[ti].muted = a[2] != 0;
                if (a[2] >= 2) release_track_channels(ti, a[2] == 3);
            }
        break;
    case CMD_SETUP_CHANNEL_PCM: {
        const int ci = (int)(a[0] & 0xffff);
        if (ci >= CHANNELS) break;
        Channel &c = g.ch[ci];
        memset(&c, 0, sizeof c);
        c.track = -1;
        c.timer = c.base_timer = (a[0] >> 16) ? (a[0] >> 16) : 1;
        c.data = ptr<const u8>(a[1]);
        c.direct_vol = (int)(a[2] >> 24) & 0x7f;
        c.direct_shift = (int)(a[2] >> 22) & 3;
        const u32 len_w = a[2] & 0x3fffff;
        const int loop_mode = (int)(a[3] >> 26) & 3, fmt = (int)(a[3] >> 24) & 3;
        c.direct_pan = (int)(a[3] >> 16) & 0x7f;
        const u32 loop_w = a[3] & 0xffff;
        c.type = fmt == 0 ? CH_PCM8 : fmt == 1 ? CH_PCM16 : CH_ADPCM;
        const u32 per_word = c.type == CH_PCM8 ? 4 : c.type == CH_PCM16 ? 2 : 8;
        c.loop = loop_mode == 1;
        c.loop_start = loop_w * per_word;
        c.length = (loop_w + len_w) * per_word;
        if (c.type == CH_ADPCM && c.data) {
            c.ad_pred = (s16)rd16(c.data);
            c.ad_index = c.data[2] > 88 ? 88 : c.data[2];
            c.ad_cur = c.ad_prev = (s16)c.ad_pred;
            c.data += 4;
        }
        c.direct_set = true;
        break;
    }
    case CMD_START_TIMER:
        for (int i = 0; i < CHANNELS; ++i)
            if ((a[0] & (1u << i)) && g.ch[i].direct_set && g.ch[i].data) {
                Channel &c = g.ch[i];
                // a capture unit's buffer: the effect is not made, so nothing to play
                if (c.data == g.capture_buf[0] || c.data == g.capture_buf[1]) continue;
                c.pos = 0;
                c.seq = false;
                c.active = true;
            }
        break;
    case CMD_STOP_TIMER:
        for (int i = 0; i < CHANNELS; ++i)
            if ((a[0] & (1u << i)) && !g.ch[i].seq) stop_channel(i);
        break;
    case CMD_SETUP_CAPTURE: {
        const int unit = (int)(a[2] >> 31) & 1;
        g.capture_buf[unit] = ptr<const u8>(a[0]);
        break;
    }
    case CMD_CHANNEL_VOLUME:
        for (int i = 0; i < CHANNELS; ++i)
            if (a[0] & (1u << i)) {
                g.ch[i].direct_vol = (int)a[1] & 0x7f;
                g.ch[i].direct_shift = (int)a[2] & 3;
            }
        break;
    case CMD_LOCK_CHANNEL:
        g.locked |= (u16)a[0];
        for (int i = 0; i < CHANNELS; ++i)
            if ((a[0] & (1u << i)) && g.ch[i].seq) stop_channel(i);
        break;
    case CMD_UNLOCK_CHANNEL: g.locked &= (u16)~a[0]; break;
    case CMD_SET_SHARED_WORK: g.shared = ptr<u32>(a[0]); break;
    default: break;     // alarms, surround, output selector, invalidations: nothing to do here
    }
}

/* `n` output frames (stereo s16) of the mix, the driver stepped as it goes. */
void mix(s16 *out, int n)
{
    std::lock_guard<std::mutex> hold(g_lock);
    const double frame_samples = FRAME_SECONDS * OUT_RATE;
    for (int s = 0; s < n; ++s) {
        g.frame_acc += 1.0;
        if (g.frame_acc >= frame_samples) {
            g.frame_acc -= frame_samples;
            driver_frame();
        }
        float l = 0, r = 0;
        for (int i = 0; i < CHANNELS; ++i) {
            Channel &c = g.ch[i];
            if (!c.active) continue;
            float v;
            if (!channel_sample(c, v)) { stop_channel(i); continue; }
            l += v * c.gain_l;
            r += v * c.gain_r;
        }
        // the DS's speakers clip; keep headroom for many channels
        l *= 0.7f;
        r *= 0.7f;
        if (l > 1) l = 1;
        if (l < -1) l = -1;
        if (r > 1) r = 1;
        if (r < -1) r = -1;
        out[s * 2] = (s16)(l * 32767);
        out[s * 2 + 1] = (s16)(r * 32767);
    }
}

#if defined(__vita__)
int audio_thread(SceSize, void *)
{
    constexpr int GRAIN = 512;      // ~10.7 ms a buffer
    const int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        printf("  sound: no audio port (%#x): silent\n", (unsigned)port);
        return 0;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    printf("  sound: playing at %d Hz on the third core\n", OUT_RATE);
    static s16 buf[2][GRAIN * 2];
    int which = 0;
    unsigned report = 0;
    for (;;) {
        mix(buf[which], GRAIN);
        sceAudioOutOutput(port, buf[which]);
        which ^= 1;
        if (++report % (OUT_RATE / GRAIN * 30) == 0) {     // every half minute
            std::lock_guard<std::mutex> hold(g_lock);
            int active = 0;
            for (const Channel &c : g.ch) active += c.active;
            printf("  [sound] %u sequences started, %u notes, %d channels playing, %u unknown commands",
                   g.seqs_started, g.notes, active, g.unknown_ops);
            for (unsigned k = 0; k < g.unknown_ops && k < 8; ++k) printf(" %02x", (unsigned)g.unknown_op_seen[k]);
            // the players, for "the music stopped" reports
            int used = 0;
            for (const Track &t : g.tr) used += t.allocated;
            printf("; tracks in use %d/%d, locked channels %04x, failed track opens %u\n", used, TRACKS,
                   (unsigned)g.locked, g_track_fail);
            for (int p = 0; p < PLAYERS; ++p) {
                const Player &pl = g.pl[p];
                if (!pl.active) continue;
                int nt = 0, live = 0;
                for (int k = 0; k < 16; ++k)
                    if (pl.tracks[k] >= 0) { ++nt; live += g.tr[pl.tracks[k]].active; }
                printf("    player %d: %d tracks (%d running)%s, volume %u fader %d, tempo %u, tick %u\n", p, nt, live,
                       pl.paused ? ", PAUSED" : "", pl.volume, pl.ext_fader, pl.tempo, (unsigned)pl.ticks);
            }
        }
    }
    return 0;
}
#endif
}  // namespace

/* The driver's whole state, for save states (port/vita/savestate_host.cpp):
   its channels, players and tracks point into the game's memory, which is
   saved and restored with it. */
extern "C" unsigned sm64ds_snd_state_size(void) { return sizeof g; }
extern "C" void sm64ds_snd_state_save(void *out)
{
    std::lock_guard<std::mutex> lock(g_lock);
    memcpy(out, &g, sizeof g);
}
extern "C" void sm64ds_snd_state_load(const void *in)
{
    std::lock_guard<std::mutex> lock(g_lock);
    memcpy(&g, in, sizeof g);
}

extern "C" {

/* The ARM9's command list (port/vita/arm7_host.cpp): applied now, in order. */
void sm64ds_snd_commands(const void *list)
{
    struct Cmd { const Cmd *next; int32_t id; u32 arg[4]; };
    std::lock_guard<std::mutex> hold(g_lock);
    unsigned n = 0;
    for (const Cmd *c = static_cast<const Cmd *>(list); c && n < 4096; c = c->next, ++n) command(c->id, c->arg);
    // The status the game reads (which players and channels are active) as
    // these commands leave it, before arm7_host.cpp counts the list as done.
    // On the DS the ARM7 writes it in the same pass; here the output thread
    // only wrote it at its next sequencer tick, and a game that started a
    // sequence and looked straight away saw the player idle -- and could
    // start the music a second time over the first (the menu "canon").
    write_status();
}

/* Once, at start-up: the driver, and its output thread on the third core. */
void sm64ds_snd_init(void)
{
    static bool done;
    if (done) return;
    done = true;
    init_tables();
    init_adpcm();
    for (Player &p : g.pl)
        for (s8 &t : p.tracks) t = -1;
    for (Channel &c : g.ch) c.track = -1;
#if defined(__vita__)
    const SceUID th = sceKernelCreateThread("sm64ds_sound", audio_thread, 0x10000100 - 8, 64 * 1024, 0,
                                            0x40000 /* CPU_MASK_USER_2 */, nullptr);
    if (th >= 0) sceKernelStartThread(th, 0, nullptr);
    else printf("  sound: no audio thread (%#x)\n", (unsigned)th);
#endif
}

}  // extern "C"
