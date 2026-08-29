#ifndef RE_BBASIC_BBVM_LIB_H
#define RE_BBASIC_BBVM_LIB_H

#include "bbvm.h"

/* A LIB record's length excludes its own u32 length field.  It includes the
 * width/height pair, two reserved u32 values and the RGB565 pixel payload. */
#define BBVM_LIB_RECORD_METADATA_SIZE 12u

int bbvm_lib_table_position(bb_u32 count, bb_u32 resource_id,
                            bb_u32 *position);
int bbvm_lib_pixel_size(bb_u32 record_length, bb_u32 width, bb_u32 height,
                        bb_u32 *pixel_size);

#endif
