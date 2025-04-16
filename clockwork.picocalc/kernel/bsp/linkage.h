#ifndef _LINUX_LINKAGE_H
#define _LINUX_LINKAGE_H

#define ALIGN   .align   4

#define LENTRY(name) \
        ALIGN; \
        name:

#define ENTRY(name) \
        .globl name ;   \
        LENTRY(name)

#define END(name) \
        .size name, .-name

#define ENDPROC(name) \
        .type name 2; \
        END(name)
#endif
