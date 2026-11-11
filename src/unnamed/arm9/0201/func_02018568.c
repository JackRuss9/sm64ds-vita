/* "LZ77", the tag in front of an LZ-compressed game file. */
extern int data_0208edf8[];
extern int func_0206e28c(int a, void *b, int n);

/* Whether the file bytes at a open with the "LZ77" tag (func_0206e28c
   compares 4 bytes, memcmp-style). The loaders then read the LZ header at
   a + 4 (func_02018ac4) and decompress from there. */
#if defined(SM64DS_PLATFORM_PC)
extern void *ntr_host_ptr(unsigned int ds_address);     /* port/ntr/io.cpp */
#endif
int func_02018568(int a) {
#if defined(SM64DS_PLATFORM_PC)
    /* Host: a file read straight into palette or VRAM (LoadFileAt) arrives
       here as its DS address; host_ptr passes host pointers through. */
    return func_0206e28c((int)ntr_host_ptr((unsigned int)a), data_0208edf8, 4) == 0;
#else
    return func_0206e28c(a, data_0208edf8, 4) == 0;
#endif
}
