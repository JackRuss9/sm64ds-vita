// Fill the ROM-constant tables from the ARM9 inside the user's .nds.
// See romdata_rom.h. Same facts and same anchor check as port/tools/romdata.py,
// which reads them from extracted/arm9_dec.bin on PC.
#include "romdata_rom.h"

#include "romfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SM64DS_ROM_PATH
#define SM64DS_ROM_PATH "ux0:data/sm64ds/sm64ds.nds"
#endif

namespace {
// romdata.py's anchor: Copy36Bytes starts with `ldmia r0!, {r2, r3, ip}` here.
constexpr uint32_t ANCHOR_ADDR = 0x0205A548u;
const uint8_t ANCHOR_BYTES[4] = {0x0c, 0x10, 0xb0, 0xe8};
}  // namespace

bool sm64ds_romdata_load(const char *rom_path) {
    if (!rom_path) rom_path = getenv("SM64DS_ROM");
    if (!rom_path) rom_path = SM64DS_ROM_PATH;
    uint32_t base = 0;
    size_t len = 0;
    uint8_t *arm9 = romfs::read_arm9(rom_path, &base, &len);
    if (!arm9) {
        fprintf(stderr, "romdata: could not read the ARM9 from %s\n", rom_path);
        return false;
    }
    bool ok = true;
    if (ANCHOR_ADDR < base || ANCHOR_ADDR - base + 4 > len ||
        memcmp(arm9 + (ANCHOR_ADDR - base), ANCHOR_BYTES, 4) != 0) {
        fprintf(stderr, "romdata: ARM9 failed the Copy36Bytes anchor at 0x%08x "
                        "(wrong game or region?)\n", (unsigned)ANCHOR_ADDR);
        ok = false;
    }
    for (size_t i = 0; ok && i < sm64ds_romdata_count; ++i) {
        const RomdataTable &t = sm64ds_romdata_tables[i];
        if (t.addr < base || t.addr + t.length > sm64ds_romdata_bss_start ||
            t.addr - base + t.length > len) {
            fprintf(stderr, "romdata: table 0x%08x+0x%x is outside the ARM9 image\n",
                    (unsigned)t.addr, (unsigned)t.length);
            ok = false;
            break;
        }
        memcpy(t.storage, arm9 + (t.addr - base), t.length);
    }
    free(arm9);
    return ok;
}

// The launcher's pre-test hook (port/vita/vita_main.cpp): the tables must be in
// place before any game code runs.
extern "C" bool sm64ds_vita_preinit(void) {
    return sm64ds_romdata_load(nullptr);
}
