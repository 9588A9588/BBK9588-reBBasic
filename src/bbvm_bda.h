#ifndef RE_BBASIC_BBVM_BDA_H
#define RE_BBASIC_BBVM_BDA_H

#include "bbvm.h"

void *bbvm_bda_alloc(void *opaque, bb_u32 size);
void bbvm_bda_free(void *opaque, void *pointer);
bb_u32 bbvm_bda_tick(void *opaque);
int bbvm_bda_out(void *opaque, bbvm_t *vm, bb_u32 port, bb_u32 argument);
int bbvm_bda_start(bbvm_t *vm);
void bbvm_bda_pump(bbvm_t *vm);
void bbvm_bda_stop(void);

#endif
