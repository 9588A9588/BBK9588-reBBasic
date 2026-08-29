#include "bbvm_lib.h"

int bbvm_lib_table_position(bb_u32 count, bb_u32 resource_id,
                            bb_u32 *position) {
    if (position == 0 || resource_id == 0u || resource_id > count)
        return 0;
    *position = 4u + (resource_id - 1u) * 4u;
    return 1;
}

int bbvm_lib_pixel_size(bb_u32 record_length, bb_u32 width, bb_u32 height,
                        bb_u32 *pixel_size) {
    bb_u32 pixels;
    if (pixel_size == 0 || record_length < BBVM_LIB_RECORD_METADATA_SIZE ||
        width == 0u || height == 0u ||
        width > 0xffffffffu / height)
        return 0;
    pixels = width * height;
    if (pixels > 0xffffffffu / 2u)
        return 0;
    pixels *= 2u;
    if (pixels > record_length - BBVM_LIB_RECORD_METADATA_SIZE)
        return 0;
    *pixel_size = pixels;
    return 1;
}
