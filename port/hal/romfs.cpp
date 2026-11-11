// Read game assets straight out of the user's .nds dump. See romfs.h.
//
// Formats are from GBATEK ("DS Cartridge Header", "DS Cartridge NitroROM and
// NitroARC File Systems", "DS Encryption/Compression"), cross-checked against
// ndspy, the library tools/asset_catalog.py uses. No SDK or emulator code.
#include "romfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace romfs {
namespace {

// The game's handle table, as tools/asset_catalog.py names it: visible in the
// target image and in its matched initializer, InitializeAssetHandleTable.
constexpr uint32_t OV000_HANDLE_TABLE_ADDRESS = 0x020BD4B8u;
constexpr uint32_t OV000_HANDLE_COUNT = 0x80Au;

// Cartridge header words (GBATEK "DS Cartridge Header").
constexpr uint32_t HDR_GAME_CODE = 0x0C;
constexpr uint32_t HDR_FNT_OFF = 0x40, HDR_FNT_SIZE = 0x44;
constexpr uint32_t HDR_FAT_OFF = 0x48, HDR_FAT_SIZE = 0x4C;
constexpr uint32_t HDR_OVT9_OFF = 0x50, HDR_OVT9_SIZE = 0x54;

FILE *g_rom = nullptr;
long g_rom_size = 0;
uint8_t *g_fat = nullptr;     // 8 bytes per file: start, end (absolute)
uint32_t g_file_count = 0;

inline uint32_t rd32(const uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

}  // namespace
// Reads of game files, for the log's slow-frame lines: count, bytes, time.
unsigned g_span_reads, g_span_bytes;
unsigned long long g_span_us;
namespace {
unsigned long long now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000u + (unsigned long long)ts.tv_nsec / 1000u;
}
uint8_t *read_span_timed(uint32_t off, uint32_t len);
// Read [off, off+len) of the image, malloc'd; nullptr if out of range.
uint8_t *read_span(uint32_t off, uint32_t len) {
    const unsigned long long t0 = now_us();
    uint8_t *r = read_span_timed(off, len);
    ++g_span_reads;
    g_span_bytes += len;
    g_span_us += now_us() - t0;
    return r;
}
uint8_t *read_span_timed(uint32_t off, uint32_t len) {
    if ((long)off > g_rom_size || (long)len > g_rom_size - (long)off) return nullptr;
    uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
    if (!buf) return nullptr;
    if (fseek(g_rom, (long)off, SEEK_SET) != 0 || fread(buf, 1, len, g_rom) != len) {
        free(buf);
        return nullptr;
    }
    return buf;
}

// Walk one FNT directory, reporting every file under its full path.
// GBATEK: the main table has 8 bytes per directory (u32 sub-table offset,
// u16 first file id, u16 parent id); a sub-table entry is a type/length byte
// -- 0 ends it, 1..0x7F a file name of that length, 0x81..0xFF a directory
// name of length-0x80 followed by its u16 id (0xF000 | index).
bool walk_dir(const uint8_t *fnt, uint32_t fnt_size, unsigned dir_index,
              const char *prefix, int depth,
              void (*on_file)(unsigned, const char *, void *), void *ctx) {
    if (depth > 32 || (dir_index + 1) * 8 > fnt_size) return false;
    const uint8_t *e = fnt + dir_index * 8;
    uint32_t sub = rd32(e);
    unsigned file_id = rd16(e + 4);
    char path[512];
    while (sub < fnt_size) {
        uint8_t type = fnt[sub++];
        if (type == 0) return true;
        unsigned len = type & 0x7F;
        if (len == 0 || sub + len > fnt_size) return false;
        snprintf(path, sizeof path, "%s%.*s", prefix, (int)len, (const char *)fnt + sub);
        sub += len;
        if (type & 0x80) {
            if (sub + 2 > fnt_size) return false;
            unsigned child = rd16(fnt + sub) & 0x0FFF;
            sub += 2;
            size_t n = strlen(path);
            if (n + 1 >= sizeof path) return false;
            path[n] = '/'; path[n + 1] = 0;
            if (!walk_dir(fnt, fnt_size, child, path, depth + 1, on_file, ctx)) return false;
        } else {
            on_file(file_id++, path, ctx);
        }
    }
    return false;   // ran off the table without a terminator
}

// Path -> file id lookup for the handle table, built during the FNT walk.
struct PathIndex {
    char **paths = nullptr;  // by file id
    unsigned count = 0;
};

void index_file(unsigned id, const char *path, void *ctx) {
    PathIndex *ix = (PathIndex *)ctx;
    if (id < ix->count && !ix->paths[id]) ix->paths[id] = strdup(path);
}

void close_all() {
    if (g_rom) fclose(g_rom);
    g_rom = nullptr;
    free(g_fat);
    g_fat = nullptr;
    g_file_count = 0;
}

}  // namespace

uint8_t *blz_decompress(const uint8_t *src, size_t src_len, size_t *out_len) {
    // Footer (last 8 bytes): u32 enc_len | hdr_len << 24, u32 inc_len.
    // The first (src_len - enc_len) bytes are stored plain; the compressed
    // stream is read BACKWARDS from (src_len - hdr_len) down to that point,
    // and the output is written backwards from its end.
    if (src_len < 8) return nullptr;
    uint32_t inc_len = rd32(src + src_len - 4);
    if (inc_len == 0) {              // "not actually compressed"
        uint8_t *out = (uint8_t *)malloc(src_len);
        if (!out) return nullptr;
        memcpy(out, src, src_len);
        *out_len = src_len;
        return out;
    }
    uint32_t word = rd32(src + src_len - 8);
    uint32_t hdr_len = word >> 24, enc_len = word & 0xFFFFFF;
    if (enc_len > src_len || hdr_len < 8 || hdr_len > enc_len) return nullptr;
    size_t dec_start = src_len - enc_len;     // plain prefix length
    size_t raw_len = src_len + inc_len;
    uint8_t *out = (uint8_t *)malloc(raw_len);
    if (!out) return nullptr;
    memcpy(out, src, src_len);                 // prefix (and scratch) in place

    size_t in = src_len - hdr_len;             // read cursor, moves down
    size_t w = raw_len;                        // write cursor, moves down
    while (w > dec_start && in > dec_start) {
        uint8_t flags = src[--in];
        for (int bit = 0; bit < 8 && w > dec_start; ++bit, flags <<= 1) {
            if (!(flags & 0x80)) {
                if (in <= dec_start) break;
                out[--w] = src[--in];
                continue;
            }
            if (in < dec_start + 2) { free(out); return nullptr; }
            uint32_t pair = (uint32_t)src[--in] << 8;
            pair |= src[--in];
            uint32_t len = (pair >> 12) + 3;
            uint32_t disp = (pair & 0x0FFF) + 3;
            while (len-- && w > dec_start) {
                --w;
                if (w + disp >= raw_len) { free(out); return nullptr; }
                out[w] = out[w + disp];
            }
        }
    }
    *out_len = raw_len;
    return out;
}

bool open(const char *rom_path,
          void (*on_file)(unsigned, const char *, void *),
          void (*on_handle)(unsigned, unsigned, void *),
          void *ctx) {
    close_all();
    g_rom = fopen(rom_path, "rb");
    if (!g_rom) {
        fprintf(stderr, "romfs: cannot open ROM: %s\n", rom_path);
        return false;
    }
    fseek(g_rom, 0, SEEK_END);
    g_rom_size = ftell(g_rom);

    uint8_t *hdr = read_span(0, 0x200);
    if (!hdr) {
        fprintf(stderr, "romfs: %s is too small to be a DS ROM\n", rom_path);
        close_all();
        return false;
    }
    char code[5] = {0};
    memcpy(code, hdr + HDR_GAME_CODE, 4);
    if (memcmp(code, "ASM", 3) != 0) {
        fprintf(stderr, "romfs: game code %s is not Super Mario 64 DS (ASM*)\n", code);
    } else if (code[3] != 'P') {
        fprintf(stderr, "romfs: game code %s: this port is verified against the EUR "
                        "build (ASMP); other regions may not match\n", code);
    }
    const uint32_t fnt_off = rd32(hdr + HDR_FNT_OFF), fnt_size = rd32(hdr + HDR_FNT_SIZE);
    const uint32_t fat_off = rd32(hdr + HDR_FAT_OFF), fat_size = rd32(hdr + HDR_FAT_SIZE);
    const uint32_t ovt_off = rd32(hdr + HDR_OVT9_OFF), ovt_size = rd32(hdr + HDR_OVT9_SIZE);
    free(hdr);

    g_fat = read_span(fat_off, fat_size);
    uint8_t *fnt = read_span(fnt_off, fnt_size);
    uint8_t *ovt = read_span(ovt_off, ovt_size);
    if (!g_fat || !fnt || !ovt || fat_size % 8 || ovt_size < 32) {
        fprintf(stderr, "romfs: header tables out of range (FNT/FAT/OVT)\n");
        free(fnt); free(ovt); close_all();
        return false;
    }
    g_file_count = fat_size / 8;

    // Names: report every file, and keep an index for the handle table.
    PathIndex ix;
    ix.count = g_file_count;
    ix.paths = (char **)calloc(g_file_count, sizeof(char *));
    struct Both { PathIndex *ix; void (*cb)(unsigned, const char *, void *); void *ctx; } both{&ix, on_file, ctx};
    auto report = [](unsigned id, const char *path, void *c) {
        Both *b = (Both *)c;
        index_file(id, path, b->ix);
        if (b->cb) b->cb(id, path, b->ctx);
    };
    bool ok = walk_dir(fnt, fnt_size, 0, "", 0, report, &both);
    free(fnt);
    if (!ok) fprintf(stderr, "romfs: malformed file name table\n");

    // Overlay 0: the first 32-byte OVT record. ram address at +4, file id at
    // +0x18, u32 at +0x1C = compressed size (low 24 bits) | flags << 24 with
    // bit 0 = BLZ-compressed.
    if (ok) {
        const uint32_t ov_ram = rd32(ovt + 4), ov_file = rd32(ovt + 0x18);
        const uint32_t ov_flags = rd32(ovt + 0x1C) >> 24;
        const uint32_t ov_comp_size = rd32(ovt + 0x1C) & 0xFFFFFF;
        size_t ov_len = 0;
        uint8_t *ov = read_file(ov_file, &ov_len);
        if (ov && (ov_flags & 1)) {
            // The OVT's compressed size bounds the stream: the FAT entry can
            // carry alignment padding after the BLZ footer.
            size_t stream = (ov_comp_size && ov_comp_size <= ov_len) ? ov_comp_size : ov_len;
            size_t dec_len = 0;
            uint8_t *dec = blz_decompress(ov, stream, &dec_len);
            free(ov);
            ov = dec;
            ov_len = dec_len;
        }
        const uint32_t table = OV000_HANDLE_TABLE_ADDRESS - ov_ram;
        if (!ov || OV000_HANDLE_TABLE_ADDRESS < ov_ram ||
            (size_t)table + OV000_HANDLE_COUNT * 4 > ov_len) {
            fprintf(stderr, "romfs: overlay 0 does not hold the asset-handle table "
                            "(wrong game or region?)\n");
            ok = false;
        }
        for (uint32_t h = 0; ok && h < OV000_HANDLE_COUNT; ++h) {
            uint32_t ptr = rd32(ov + table + h * 4);
            if (ptr < ov_ram || ptr - ov_ram >= ov_len) {
                fprintf(stderr, "romfs: handle 0x%x has an invalid path pointer\n", h);
                ok = false;
                break;
            }
            const char *name = (const char *)ov + (ptr - ov_ram);
            size_t room = ov_len - (ptr - ov_ram);
            if (!memchr(name, 0, room)) { ok = false; break; }
            unsigned id = 0;
            while (id < ix.count && !(ix.paths[id] && strcmp(ix.paths[id], name) == 0)) ++id;
            if (id == ix.count) {
                fprintf(stderr, "romfs: handle 0x%x names a missing file: %s\n", h, name);
                ok = false;
                break;
            }
            if (on_handle) on_handle(h, id, ctx);
        }
        free(ov);
    }
    free(ovt);
    for (unsigned i = 0; i < ix.count; ++i) free(ix.paths[i]);
    free(ix.paths);
    if (!ok) close_all();
    return ok;
}

bool read_ovt_entry(const char *rom_path, unsigned ov_id, uint8_t out[32]) {
    FILE *f = fopen(rom_path, "rb");
    if (!f) return false;
    uint8_t hdr[0x60];
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr;
    if (ok) {
        const uint32_t ovt_off = rd32(hdr + HDR_OVT9_OFF), ovt_size = rd32(hdr + HDR_OVT9_SIZE);
        ok = (ov_id + 1) * 0x20u <= ovt_size &&
             fseek(f, (long)(ovt_off + ov_id * 0x20u), SEEK_SET) == 0 &&
             fread(out, 1, 32, f) == 32;
    }
    fclose(f);
    return ok;
}

uint8_t *read_overlay(const char *rom_path, unsigned ov_id, uint32_t *ram_base, size_t *len) {
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "romfs: fopen(%s) failed, errno %d (%s)\n",
                rom_path, errno, strerror(errno));
        return nullptr;
    }
    uint8_t *out = nullptr;
    uint8_t hdr[0x60], ent[0x20], fat[8];
    do {
        if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) break;
        const uint32_t ovt_off = rd32(hdr + HDR_OVT9_OFF), ovt_size = rd32(hdr + HDR_OVT9_SIZE);
        const uint32_t fat_off = rd32(hdr + HDR_FAT_OFF), fat_size = rd32(hdr + HDR_FAT_SIZE);
        if ((ov_id + 1) * 0x20u > ovt_size) {
            fprintf(stderr, "romfs: overlay %u is not in the OVT\n", ov_id);
            break;
        }
        // GBATEK OVT record: id, ram address, ram size, bss size, sinit start,
        // sinit end, file id, then compressed size (24 bits) | flags << 24.
        if (fseek(f, (long)(ovt_off + ov_id * 0x20u), SEEK_SET) != 0 ||
            fread(ent, 1, sizeof ent, f) != sizeof ent) break;
        const uint32_t ram = rd32(ent + 4), ram_size = rd32(ent + 8);
        const uint32_t file_id = rd32(ent + 0x18), word = rd32(ent + 0x1C);
        if ((file_id + 1) * 8u > fat_size ||
            fseek(f, (long)(fat_off + file_id * 8u), SEEK_SET) != 0 ||
            fread(fat, 1, 8, f) != 8) break;
        const uint32_t start = rd32(fat), end = rd32(fat + 4);
        if (end < start || end - start >= (16u << 20)) break;
        uint32_t size = end - start;
        uint8_t *raw = (uint8_t *)malloc(size ? size : 1);
        if (!raw) break;
        if (fseek(f, (long)start, SEEK_SET) != 0 || fread(raw, 1, size, f) != size) {
            free(raw); break;
        }
        if ((word >> 24) & 1) {          // BLZ-compressed, stream bounded by the OVT
            const uint32_t comp = word & 0xFFFFFF;
            out = blz_decompress(raw, comp && comp <= size ? comp : size, len);
            free(raw);
        } else {
            out = raw;
            *len = size;
        }
        if (out && *len > ram_size && ram_size) *len = ram_size;
        if (out) *ram_base = ram;
    } while (0);
    if (!out) fprintf(stderr, "romfs: could not read overlay %u from %s\n", ov_id, rom_path);
    fclose(f);
    return out;
}

uint8_t *read_arm9(const char *rom_path, uint32_t *ram_base, size_t *len) {
    // Each failure says which step failed: "could not read the ARM9" alone
    // cannot tell a missing file from a short read or a full heap.
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "romfs: fopen(%s) failed, errno %d (%s)\n",
                rom_path, errno, strerror(errno));
        return nullptr;
    }
    uint8_t hdr[0x30];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fprintf(stderr, "romfs: header read failed (file shorter than 0x30?)\n");
        fclose(f); return nullptr;
    }
    // GBATEK header: 0x20 ARM9 ROM offset, 0x28 RAM address, 0x2C size.
    const uint32_t off = rd32(hdr + 0x20), ram = rd32(hdr + 0x28), size = rd32(hdr + 0x2C);
    if (size < 8 || size >= (64u << 20)) {
        fprintf(stderr, "romfs: implausible ARM9 size 0x%x in the header\n", (unsigned)size);
        fclose(f); return nullptr;
    }
    uint8_t *raw = (uint8_t *)malloc(size);
    if (!raw) {
        fprintf(stderr, "romfs: malloc(0x%x) for the ARM9 failed\n", (unsigned)size);
        fclose(f); return nullptr;
    }
    if (fseek(f, (long)off, SEEK_SET) != 0) {
        fprintf(stderr, "romfs: fseek to ARM9 offset 0x%x failed, errno %d\n",
                (unsigned)off, errno);
        free(raw); fclose(f); return nullptr;
    }
    const size_t got = fread(raw, 1, size, f);
    if (got != size) {
        fprintf(stderr, "romfs: ARM9 read got 0x%x of 0x%x bytes at 0x%x, errno %d\n",
                (unsigned)got, (unsigned)size, (unsigned)off, errno);
        free(raw); fclose(f); return nullptr;
    }
    fclose(f);
    // Like ndspy's code decompression: the BLZ footer may be followed by up to
    // 0x1C bytes of appended data. Take the first plausible footer position.
    size_t stream = 0;
    for (size_t extra = 0; extra < 0x20 && extra + 8 <= size; extra += 4) {
        const uint32_t word = rd32(raw + size - extra - 8);
        const uint32_t hdr_len = word >> 24, enc_len = word & 0xFFFFFF;
        if (hdr_len < 8 || enc_len > size) continue;
        stream = size - extra;
        break;
    }
    uint8_t *out = nullptr;
    if (stream) {
        out = blz_decompress(raw, stream, len);
        free(raw);
    } else {               // no footer at all: stored uncompressed
        out = raw;
        *len = size;
    }
    if (out) *ram_base = ram;
    else fprintf(stderr, "romfs: ARM9 BLZ decompression failed (stream 0x%x)\n",
                 (unsigned)stream);
    return out;
}

uint8_t *read_file(unsigned file_id, size_t *size) {
    if (!g_rom || file_id >= g_file_count) return nullptr;
    const uint32_t start = rd32(g_fat + file_id * 8), end = rd32(g_fat + file_id * 8 + 4);
    if (end < start) return nullptr;
    uint8_t *p = read_span(start, end - start);
    if (p) *size = end - start;
    return p;
}

}  // namespace romfs

extern "C" void sm64ds_romfs_stats(unsigned *reads, unsigned *bytes, unsigned long long *us)
{
    *reads = romfs::g_span_reads;
    *bytes = romfs::g_span_bytes;
    *us = romfs::g_span_us;
}
