// The ARM7's side of the PXI FIFO for the full-game GCC build (PS Vita).
//
// On the DS the ARM9 game talks to the ARM7 -- which owns power management,
// the touch screen, sound mixing, the card backup chip, the GBA slot and reset
// -- through the PXI FIFO (src/engine/pxi/PXI_Fifo.cpp): 32-bit words tagged
// with a channel, a receive IRQ on each side, and a callback per tag. The host
// has no ARM7, so this file stands in for it at the FIFO's two ends:
//
//   PXI_InitFifo        same set-up, without the SUBPINTF nibble handshake
//                       (the ARM7 never answers it here), and the ARM7's
//                       listening tags marked ready in the shared system work
//                       area, as the ARM7's own PXI_Init would have;
//   PXI_SendWordByFifo  every word the game sends reaches this ARM7 stand-in,
//                       which answers the channels whose answers the ARM9
//                       waits for. A reply is the receive IRQ: it reaches the
//                       game's own callback for that tag in interrupt context
//                       (what PXIi_HandlerRecvFifoNotEmpty does on the DS) as
//                       soon as the ARM9 has interrupts enabled -- right after
//                       the send, or at the IRQ::Restore that re-enables them
//                       when the game sent with interrupts off (a request
//                       marks itself busy before its answer, as it relies on).
//
// Channels and what they get here:
//   PM (8)       power management: every packet acknowledged with result 0;
//                register reads answer 0 (no power-button, battery fine).
//   TP (6)       touch panel: every request acknowledged with success; the
//                one-shot read and, once auto-sampling is on, the samples
//                sent every frame carry the Vita's touch (sm64ds_arm7_touch,
//                from port/vita/input_host.cpp).
//   FS (0xb)     the card's backup (save) chip: identify, read, program and
//                verify run on a host file, ux0:data/sm64ds/sm64ds.sav, sized
//                by the backup type the game asks for; every request answers
//                success, the reply word carrying the PXI reply bit the card
//                callback (CARDi_OnFifoRecv) waits for.
//   SOUND (7)    command lists: carried out by the sound driver
//                (port/vita/snd_host.cpp), then counted as done (the shared
//                work's finished-command tag) -- the game's waits on its own
//                commands end.
//   others       logged once per command and left unanswered for now.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntr/mmio.h"
#include "ntr/rt.h"

typedef uint32_t u32;
typedef void (*PxiCallback)(u32 tag, u32 data, u32 reply);

extern "C" {
extern uint16_t data_020a7fc4;          /* the FIFO is initialised */
extern PxiCallback data_020a7fc8[];     /* the ARM9's callback per tag */
void PXIi_HandlerRecvFifoNotEmpty(void);
int __real_PXI_SendWordByFifo(unsigned tag, unsigned data, unsigned reply);
/* irq_host.cpp */
void sm64ds_irq_post(void (*fn)(uintptr_t, uintptr_t, uintptr_t), uintptr_t a, uintptr_t b,
                     uintptr_t c);
void sm64ds_irq_drain(void);
void sm64ds_hostlog(const char *what, uint32_t a, uint32_t b);
/* snd_host.cpp: the sound driver */
void sm64ds_snd_init(void);
void sm64ds_snd_commands(const void *list);
}
namespace IRQ {
u32 Disable(void);
u32 Restore(u32 saved);
void SetIRQHandler(u32 mask, void (*handler)(void));
u32 EnableIRQs(u32 mask);
}  // namespace IRQ

namespace {
constexpr u32 SYSTEM_WORK = 0x027ffc00, PXI_HANDLE_CHECKER = SYSTEM_WORK + 0x388;
constexpr u32 IRQ_RECV_FIFO_NOT_EMPTY = 0x40000;

enum : u32 { TAG_TP = 6, TAG_SOUND = 7, TAG_PM = 8, TAG_FS = 0xb, TAG_OS = 12, TAG_CTRDG = 13 };
// The channels the ARM7 listens on (PXI_IsCallbackReady(tag, ARM7)). Wireless
// (0xa) stays off: the game only uses it for download play and multiplayer.
constexpr u32 ARM7_TAGS = (1u << TAG_TP) | (1u << TAG_SOUND) | (1u << TAG_PM) |
                          (1u << TAG_FS) | (1u << TAG_OS) | (1u << TAG_CTRDG);

u32 *checker() { return static_cast<u32 *>(::ntr::host_ptr(PXI_HANDLE_CHECKER)); }

// A reply is the ARM9's receive interrupt: posted, it runs the tag's
// callback in interrupt context at the next point the DS would take it.
void deliver(uintptr_t tag, uintptr_t data, uintptr_t rep)
{
    if (PxiCallback cb = data_020a7fc8[tag]) cb((u32)tag, (u32)data, (u32)rep);
}
void reply(u32 tag, u32 data, u32 rep = 0)
{
    sm64ds_irq_post(deliver, tag, data & 0x3ffffff, rep);
}

// --- PM: packets of 1..3 words; bit 25 starts one, bit 24 ends it,
//     bits 8..14 of the first word are the command (src/engine/spi/PM.cpp).
u32 g_pm_cmd;
void pm(u32 data)
{
    if (data & 0x2000000) g_pm_cmd = (data >> 8) & 0x7f;
    if (!(data & 0x1000000)) return;
    u32 cmd = g_pm_cmd;
    u32 result = 0;
    if (cmd == 0x65) {                  // read register n -> reply 0x70 + n, value 0
        const u32 reg = data & 0xff;
        cmd = 0x70 + (reg & 3);
    }
    reply(TAG_PM, (cmd << 8) | result);
}

// --- TP: same packet framing; the answer to command c is c << 8 with the
//     end bit and result 0 (TPi_TpCallback, src/TPi_TpCallback.c). The
//     touch sample words sit in the shared work area at 0x027fffaa/ac.
u32 g_tp_cmd;
bool g_tp_auto;             // auto-sampling on (command 1 until command 2)
u32 g_tp_freq = 1;          // samples a frame
u32 g_tp_word;              // the latest sample: x:12 y:12 touch:1 validity:2

void tp_store()
{
    uint16_t *s = static_cast<uint16_t *>(::ntr::host_ptr(0x027fffaa));
    s[0] = static_cast<uint16_t>(g_tp_word);
    s[1] = static_cast<uint16_t>(g_tp_word >> 16);
}

void tp(u32 data)
{
    if (data & 0x2000000) {
        g_tp_cmd = (data >> 8) & 0x7f;
        if (g_tp_cmd == 1) g_tp_freq = (data & 0xff) ? (data & 0xff) : 1;
    }
    if (!(data & 0x1000000)) return;
    if (g_tp_cmd == 1) g_tp_auto = true;
    if (g_tp_cmd == 2) g_tp_auto = false;
    tp_store();
    reply(TAG_TP, (g_tp_cmd << 8) | 0x1000000);
}

// --- FS (card backup): the ARM9 sends a request type (CARD_REQ_*); the
//     first one, INIT, is followed by the address of its CARDiCommandArg,
//     where every later request's arguments and result live
//     (include/nitro/card/card_common.h). The full 32-bit word reaches this
//     wrap before the FIFO's 26-bit field would cut a host pointer.
#ifndef SM64DS_SAVE_PATH
#define SM64DS_SAVE_PATH "ux0:data/sm64ds/sm64ds.sav"
#endif
void log_unanswered_card(u32 req);
struct CardArg { int32_t result, type; u32 id, src, dest, len; };
CardArg *g_card_arg;
bool g_card_expect_arg;
unsigned char *g_save;
u32 g_save_size;

void save_load(u32 size)
{
    if (g_save && g_save_size == size) return;
    free(g_save);
    g_save = static_cast<unsigned char *>(malloc(size));
    memset(g_save, 0xff, size);         // an erased chip reads 0xff
    g_save_size = size;
    if (FILE *f = fopen(SM64DS_SAVE_PATH, "rb")) {
        const size_t n = fread(g_save, 1, size, f);
        fclose(f);
        printf("  arm7: save %s, %u of %u bytes\n", SM64DS_SAVE_PATH, (unsigned)n, (unsigned)size);
    } else {
        printf("  arm7: no save yet (%u-byte backup)\n", (unsigned)size);
    }
    fflush(stdout);
}
/* The game writes its save a page at a time -- hundreds of small writes
   when it saves -- and each rewrote the whole file on the memory card: the
   game stopped for seconds (a star collected, a door opened). Now a write
   only marks the copy in memory changed; the file is written once the
   writes have stopped for a few frames (save_frame, from sm64ds_arm7_frame). */
bool g_save_dirty;
unsigned g_save_quiet;                  // frames since the last write
void save_write_file()
{
    if (FILE *f = fopen(SM64DS_SAVE_PATH, "wb")) {
        fwrite(g_save, 1, g_save_size, f);
        fclose(f);
    }
    g_save_dirty = false;
}
void save_flush()
{
    g_save_dirty = true;
    g_save_quiet = 0;
}
void save_frame()
{
    if (g_save_dirty && ++g_save_quiet >= 20) save_write_file();
}
u32 clip(u32 off, u32 len) { return off >= g_save_size ? 0 : (len > g_save_size - off ? g_save_size - off : len); }

void card(u32 data)
{
    if (g_card_expect_arg) {
        g_card_expect_arg = false;
        g_card_arg = reinterpret_cast<CardArg *>(static_cast<uintptr_t>(data));
        g_card_arg->result = 0;
        reply(TAG_FS, 0, 1);
        return;
    }
    CardArg *a = g_card_arg;
    if (data == 0) {                    // CARD_REQ_INIT: the argument word follows
        g_card_expect_arg = true;
        return;
    }
    if (!a) { log_unanswered_card(data); return; }
    a->result = 0;
    switch (data) {
    case 2: {                           // IDENTIFY: size 1 << bits 8..15 of the type
        const u32 shift = ((u32)a->type >> 8) & 0xff;
        save_load(shift && shift < 24 ? (1u << shift) : 0x200);
        break;
    }
    case 6: {                           // READ_BACKUP: chip[src] -> memory[dest]
        if (!g_save) save_load(0x200);
        const u32 n = clip(a->src, a->len);
        memcpy(reinterpret_cast<void *>(static_cast<uintptr_t>(a->dest)), g_save + a->src, n);
        break;
    }
    case 7: case 8: {                   // WRITE / PROGRAM_BACKUP: memory[src] -> chip[dest]
        if (!g_save) save_load(0x200);
        const u32 n = clip(a->dest, a->len);
        memcpy(g_save + a->dest, reinterpret_cast<const void *>(static_cast<uintptr_t>(a->src)), n);
        save_flush();
        break;
    }
    case 9: {                           // VERIFY_BACKUP: memory[src] == chip[dest]
        const u32 n = clip(a->dest, a->len);
        if (!g_save || memcmp(g_save + a->dest, reinterpret_cast<const void *>(static_cast<uintptr_t>(a->src)), n))
            a->result = 1;              // CARD_RESULT_FAILURE
        break;
    }
    default:
        log_unanswered_card(data);      // logged, answered as success
        break;
    }
    reply(TAG_FS, 0, 1);
}

// --- SOUND: the ARM9 sends the address of a command list (SNDCommand: next,
//     id, arg[4]; src/engine/snd/SND_Command.cpp), or 0 to ask for processing.
struct SndCommand { SndCommand *next; int32_t id; u32 arg[4]; };
constexpr int32_t SND_CMD_SET_SHARED_WORK = 0x19;
u32 *g_snd_shared;                  // SNDSharedWork: finishCommandTag first
void sound(u32 data)
{
    if (!data) return;
    sm64ds_snd_init();
    unsigned n = 0;
    for (SndCommand *c = reinterpret_cast<SndCommand *>(static_cast<uintptr_t>(data)); c && n < 4096;
         c = c->next, ++n)
        if (c->id == SND_CMD_SET_SHARED_WORK)
            g_snd_shared = reinterpret_cast<u32 *>(static_cast<uintptr_t>(c->arg[0]));
    // the driver (port/vita/snd_host.cpp) carries them out
    sm64ds_snd_commands(reinterpret_cast<const void *>(static_cast<uintptr_t>(data)));
    if (g_snd_shared) ++g_snd_shared[0];   // this list's commands are done
}

bool g_seen[32][128];
void log_unanswered(u32 tag, u32 data)
{
    const u32 key = (data >> 8) & 0x7f;
    if (g_seen[tag][key]) return;
    g_seen[tag][key] = true;
    printf("  arm7: tag %u word 0x%07x not answered\n", (unsigned)tag, (unsigned)data);
    fflush(stdout);
}
void log_unanswered_card(u32 req)
{
    static bool seen[64];
    if (req < 64 && seen[req]) return;
    if (req < 64) seen[req] = true;
    printf("  arm7: card request %u treated as success\n", (unsigned)req);
    fflush(stdout);
}
}  // namespace

extern "C" {

void __wrap_PXI_InitFifo(void)
{
    const u32 state = IRQ::Disable();
    if (data_020a7fc4 == 0) {
        data_020a7fc4 = 1;
        checker()[0] = 0;
        for (int i = 0; i < 0x20; ++i) data_020a7fc8[i] = 0;
        IRQ::SetIRQHandler(IRQ_RECV_FIFO_NOT_EMPTY, PXIi_HandlerRecvFifoNotEmpty);
        IRQ::EnableIRQs(IRQ_RECV_FIFO_NOT_EMPTY);
        // The game writes 0xc408 (enable, receive IRQ, acknowledge error);
        // bit 14 is write-to-clear on the DS but would stick in host memory
        // and fail every send. What the register reads back instead: on,
        // receive IRQ on, both FIFOs empty, no error.
        NTR_MMIO(uint16_t, 0x04000184) = 0x8505;
        checker()[1] = ARM7_TAGS;
    }
    IRQ::Restore(state);
}

/* The touch screen now: DS dots (0..255, 0..191), pressed or not. The ARM7's
   reading is raw ADC values, which the game turns into dots through the
   calibration the firmware settings hold -- input_host.cpp writes one where
   raw = 16 x dot, so the sample is that. */
void sm64ds_arm7_touch(unsigned x, unsigned y, bool down)
{
    if (x > 255) x = 255;
    if (y > 191) y = 191;
    g_tp_word = down ? ((x * 16 + 8) | (y * 16 + 8) << 12 | 1u << 24) : 0;
}

/* Once a frame (the host's turn): the auto-sampling reports, as the ARM7
   sends them at its chosen V-counts. */
void sm64ds_arm7_frame(void)
{
    save_frame();
    if (!g_tp_auto || !data_020a7fc8[TAG_TP]) return;
    tp_store();
    const u32 n = g_tp_freq > 4 ? 4 : g_tp_freq;
    for (u32 i = 0; i < n; ++i) reply(TAG_TP, 0x1000);    // TP_SAMPLING: command 0x10
}

int __wrap_PXI_SendWordByFifo(unsigned tag, unsigned data, unsigned rep)
{
    const int r = __real_PXI_SendWordByFifo(tag, data, rep);
    if (r != 0) return r;
    sm64ds_hostlog("pxi", tag, data);
    switch (tag) {
    case TAG_PM: pm(data); break;
    case TAG_TP: tp(data); break;
    case TAG_FS: card(data); break;
    case TAG_SOUND: sound(data); break;
    default: log_unanswered(tag, data); break;
    }
    sm64ds_irq_drain();
    return 0;
}

}  // extern "C"

/* The save written now if it changed (the game ending). */
extern "C" void sm64ds_arm7_flush_save(void)
{
    if (g_save_dirty) save_write_file();
}
