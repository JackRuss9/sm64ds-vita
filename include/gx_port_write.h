#ifndef GX_PORT_WRITE_H
#define GX_PORT_WRITE_H

/* A store through a pointer aimed at a 3D geometry port (0x04000400 ..
   0x040005FF): `port = (volatile int *)0x4000458; *port = v;`. On the DS that
   is the register write itself. A host keeps the I/O registers in memory, so
   there it goes to the port's handler (port/ntr/io.cpp ntr_gx_port_store),
   which works out the register from the pointer however it was spelled.
   The DS expansion is the original expression, unchanged. */
#if defined(SM64DS_PLATFORM_PC)
#ifdef __cplusplus
extern "C"
#endif
void ntr_gx_port_store(volatile void *port, unsigned int value);
#define GX_PORT_WRITE(p, v) ntr_gx_port_store((volatile void *)(p), (unsigned int)(v))
#else
#define GX_PORT_WRITE(p, v) (*(p) = (v))
#endif

#endif
