/* Room between the code and the data segment for vita-elf-create, which
 * appends the module's SCE data (module info, import stubs: ~3.5 KB) to the
 * end of the code segment. The linker can start the data segment only a few
 * hundred bytes after the code, depending on where the code ends in its
 * page, and then vita-elf-create stops: "Cannot allocate 3548 bytes for SCE
 * data at end of segment 0; segment 1 overlaps". .tdata is the first section
 * of the data segment, so the gap goes before it. */
SECTIONS
{
    . = . + 0x2000;
}
INSERT BEFORE .tdata;
