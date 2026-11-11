// Read game assets straight out of the user's .nds cartridge dump.
//
// The PC port reads files that tools/asset_catalog.py and dsd extracted
// beforehand (build/assets/*.tsv + extracted/dsd/files/). On a console that
// means copying thousands of loose files, so this backend opens the ROM itself
// and serves the same two things the catalog provides:
//
//   * FAT file id -> path and bytes      (cartridge header -> FNT + FAT)
//   * ov0 handle  -> FAT file id         (the game's own handle table in
//                                         overlay 0, BLZ-decompressed here)
//
// Same facts asset_catalog.py reads with ndspy, same validation spirit: a
// ROM that is not the EUR build fails loudly at open, not with plausible data.
#ifndef SM64DS_PORT_ROMFS_H
#define SM64DS_PORT_ROMFS_H

#include <stddef.h>
#include <stdint.h>

namespace romfs {

// Open the ROM and build both tables. Callbacks receive every named file and
// every handle; ids outside [0, max) are the caller's to reject. Returns false
// with a message on stderr if the image is missing or is not the expected game.
bool open(const char *rom_path,
          void (*on_file)(unsigned file_id, const char *path, void *ctx),
          void (*on_handle)(unsigned handle, unsigned file_id, void *ctx),
          void *ctx);

// Bytes of FAT file `file_id`, malloc'd (caller frees). nullptr on a bad id
// or a short read. *size receives the length.
uint8_t *read_file(unsigned file_id, size_t *size);

// The ARM9 main binary from `rom_path`, decompressed, malloc'd (caller frees).
// *ram_base receives its load address (0x02004000 in SM64DS). Opens the ROM on
// its own, so it works before (and independently of) open().
uint8_t *read_arm9(const char *rom_path, uint32_t *ram_base, size_t *len);

// ARM9 overlay `ov_id` from `rom_path` as it sits in RAM once loaded:
// decompressed, malloc'd (caller frees), *ram_base its load address, *len its
// initialized size (the OVT's bss follows in RAM and is not part of this).
// Opens the ROM on its own, like read_arm9. nullptr if the id or image is bad.
uint8_t *read_overlay(const char *rom_path, unsigned ov_id, uint32_t *ram_base, size_t *len);

// The 32-byte ARM9 OVT record of overlay `ov_id` (GBATEK: id, RAM address,
// RAM size, BSS size, sinit start/end, file id, compressed size | flags).
// Opens the ROM on its own. False if the id is out of range.
bool read_ovt_entry(const char *rom_path, unsigned ov_id, uint8_t out[32]);

// Nintendo's backward LZ ("BLZ"), the ARM9/overlay code compression. `src` is
// the whole compressed file (footer included); returns the decompressed image,
// malloc'd, or nullptr if the footer is inconsistent. Exposed for tests.
uint8_t *blz_decompress(const uint8_t *src, size_t src_len, size_t *out_len);

}  // namespace romfs

#endif
