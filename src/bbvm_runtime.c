#include "bbvm.h"

static bb_u32 string_length(const char *text) {
    bb_u32 n = 0;
    if (text != 0) while (text[n] != '\0') ++n;
    return n;
}

static int string_compare(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) { ++a; ++b; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static const char *string_find(const char *text, const char *needle) {
    const char *a, *b;
    if (*needle == '\0') return text;
    while (*text != '\0') {
        a = text; b = needle;
        while (*a != '\0' && *b != '\0' && *a == *b) { ++a; ++b; }
        if (*b == '\0') return text;
        ++text;
    }
    return 0;
}

static bb_s32 text_to_int(const char *text) {
    bb_s32 sign = 1, value = 0;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text == '-') { sign = -1; ++text; }
    else if (*text == '+') ++text;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (*text - '0');
        ++text;
    }
    return value * sign;
}

static float text_to_float(const char *text) {
    float value = 0.0f, scale = 0.1f;
    int sign = 1;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text == '-') { sign = -1; ++text; }
    else if (*text == '+') ++text;
    while (*text >= '0' && *text <= '9') {
        value = value * 10.0f + (float)(*text++ - '0');
    }
    if (*text == '.') {
        ++text;
        while (*text >= '0' && *text <= '9') {
            value += (float)(*text++ - '0') * scale;
            scale *= 0.1f;
        }
    }
    return sign < 0 ? -value : value;
}

static void int_to_text(bb_s32 value, char *out) {
    char reverse[16];
    bb_u32 magnitude, n = 0, i = 0;
    if (value < 0) {
        *out++ = '-';
        magnitude = 0u - (bb_u32)value;
    } else magnitude = (bb_u32)value;
    do { reverse[n++] = (char)('0' + magnitude % 10u); magnitude /= 10u; }
    while (magnitude != 0u);
    while (n != 0u) out[i++] = reverse[--n];
    out[i] = '\0';
}

static void float_to_text(float number, char *out) {
    bb_s32 whole;
    bb_u32 fraction, n;
    char *tail;
    if (number < 0.0f) { *out++ = '-'; number = -number; }
    whole = (bb_s32)number;
    int_to_text(whole, out);
    n = string_length(out);
    tail = out + n;
    *tail++ = '.';
    fraction = (bb_u32)((number - (float)whole) * 1000000.0f + 0.5f);
    if (fraction >= 1000000u) fraction = 999999u;
    for (n = 0; n < 6u; ++n) {
        bb_u32 divisor = n == 0u ? 100000u :
                           (n == 1u ? 10000u : (n == 2u ? 1000u :
                           (n == 3u ? 100u : (n == 4u ? 10u : 1u))));
        *tail++ = (char)('0' + (fraction / divisor) % 10u);
    }
    while (tail[-1] == '0' && tail[-2] != '.') --tail;
    if (tail[-1] == '.') *tail++ = '0';
    *tail = '\0';
}

static bb_u32 float_bits(float value) {
    union { bb_u32 u; float f; } c;
    c.f = value; return c.u;
}

static float bits_float(bb_u32 value) {
    union { bb_u32 u; float f; } c;
    c.u = value; return c.f;
}

static float reduce_angle(float x) {
    const float pi2 = 6.28318530718f;
    while (x > 3.14159265359f) x -= pi2;
    while (x < -3.14159265359f) x += pi2;
    return x;
}

static float fast_sin(float x) {
    float x2;
    x = reduce_angle(x); x2 = x * x;
    return x * (1.0f - x2 / 6.0f + x2 * x2 / 120.0f -
                x2 * x2 * x2 / 5040.0f);
}

static float fast_cos(float x) { return fast_sin(x + 1.57079632679f); }

static float fast_sqrt(float x) {
    float guess;
    int i;
    if (x <= 0.0f) return 0.0f;
    guess = x > 1.0f ? x : 1.0f;
    for (i = 0; i < 12; ++i) guess = 0.5f * (guess + x / guess);
    return guess;
}

static int valid_handle(bbvm_t *vm, bb_u32 handle, bb_u32 *index) {
    bb_u32 i = 0u - handle - 1u;
    if ((bb_s32)handle >= 0 || i >= BBVM_STRING_SLOTS ||
        vm->strings[i].data == 0) return 0;
    if (index != 0) *index = i;
    return 1;
}

const char *bbvm_get_string(bbvm_t *vm, bb_u32 value) {
    bb_u32 index;
    if (valid_handle(vm, value, &index)) return vm->strings[index].data;
    if (value < vm->memory_size) return (const char *)(vm->memory + value);
    return "";
}

static int ensure_capacity(bbvm_t *vm, bbvm_string_t *entry, bb_u32 needed) {
    char *replacement;
    bb_u32 capacity, i;
    if (needed <= entry->capacity) return 1;
    if (vm->host.alloc == 0 || vm->host.free == 0) return 0;
    capacity = entry->capacity == 0u ? 32u : entry->capacity;
    while (capacity < needed) {
        if (capacity > 65536u) { capacity = needed; break; }
        capacity *= 2u;
    }
    replacement = (char *)vm->host.alloc(vm->host.opaque, capacity);
    if (replacement == 0) return 0;
    for (i = 0; i <= entry->length && entry->data != 0; ++i)
        replacement[i] = entry->data[i];
    if (entry->data != 0) vm->host.free(vm->host.opaque, entry->data);
    entry->data = replacement;
    entry->capacity = capacity;
    return 1;
}

bb_u32 bbvm_new_string(bbvm_t *vm) {
    bb_u32 i;
    for (i = 0; i < BBVM_STRING_SLOTS; ++i) {
        if (vm->strings[i].data == 0) {
            if (!ensure_capacity(vm, &vm->strings[i], 1u)) {
                vm->error = BBVM_ERR_LIMIT; vm->running = 0; return 0;
            }
            vm->strings[i].data[0] = '\0';
            vm->strings[i].length = 0;
            return 0u - i - 1u;
        }
    }
    vm->error = BBVM_ERR_LIMIT; vm->running = 0; return 0;
}

void bbvm_free_string(bbvm_t *vm, bb_u32 handle) {
    bb_u32 index;
    if (valid_handle(vm, handle, &index)) {
        vm->host.free(vm->host.opaque, vm->strings[index].data);
        vm->strings[index].data = 0;
        vm->strings[index].length = vm->strings[index].capacity = 0;
    }
}

int bbvm_set_string(bbvm_t *vm, bb_u32 handle, const char *value) {
    bb_u32 index, length, i;
    if (!valid_handle(vm, handle, &index)) return 0;
    length = string_length(value);
    if (!ensure_capacity(vm, &vm->strings[index], length + 1u)) return 0;
    for (i = 0; i <= length; ++i) vm->strings[index].data[i] = value[i];
    vm->strings[index].length = length;
    return 1;
}

static int append_string(bbvm_t *vm, bb_u32 handle, const char *value) {
    bb_u32 index, add, i;
    if (!valid_handle(vm, handle, &index)) return 0;
    add = string_length(value);
    if (!ensure_capacity(vm, &vm->strings[index],
                         vm->strings[index].length + add + 1u)) return 0;
    for (i = 0; i <= add; ++i)
        vm->strings[index].data[vm->strings[index].length + i] = value[i];
    vm->strings[index].length += add;
    return 1;
}

static int set_string_slice(bbvm_t *vm, bb_u32 handle, const char *value,
                            bb_u32 offset, bb_u32 count) {
    bb_u32 index, length, i;
    if (!valid_handle(vm, handle, &index)) return 0;
    length = string_length(value);
    if (offset > length) offset = length;
    if (count > length - offset) count = length - offset;
    if (!ensure_capacity(vm, &vm->strings[index], count + 1u)) return 0;
    for (i = 0; i < count; ++i) vm->strings[index].data[i] = value[offset + i];
    vm->strings[index].data[count] = '\0';
    vm->strings[index].length = count;
    return 1;
}

bb_u32 bbvm_in_port(bbvm_t *vm, bb_u32 port) {
    bb_u32 r0 = vm->reg[BB_R0], r1 = vm->reg[BB_R1];
    bb_u32 r2 = vm->reg[BB_R2], r3 = vm->reg[BB_R3];
    const char *a, *b, *found;
    char buffer[40];
    float value, divisor;
    bb_u32 length, count;
    switch (port) {
        case 0: return (bb_u32)(bb_s32)bits_float(r3);
        case 1: return float_bits((float)(bb_s32)r3);
        case 2: return bbvm_new_string(vm);
        case 3: return (bb_u32)text_to_int(bbvm_get_string(vm, r3));
        case 4:
            int_to_text((bb_s32)r3, buffer); (void)bbvm_set_string(vm, r2, buffer);
            return r3;
        case 5: (void)bbvm_set_string(vm, r3, bbvm_get_string(vm, r2)); return r3;
        case 6: (void)append_string(vm, r3, bbvm_get_string(vm, r2)); return r3;
        case 7: return string_length(bbvm_get_string(vm, r3));
        case 8: bbvm_free_string(vm, r3); return r3;
        case 9: return (bb_u32)string_compare(bbvm_get_string(vm, r3),
                                              bbvm_get_string(vm, r2));
        case 10:
            float_to_text(bits_float(r3), buffer);
            (void)bbvm_set_string(vm, r2, buffer); return r3;
        case 11: return float_bits(text_to_float(bbvm_get_string(vm, r3)));
        case 12:
            a = bbvm_get_string(vm, r3); length = string_length(a);
            return r2 < length ? (bb_u32)(unsigned char)a[r2] : 0u;
        case 13: {
            bb_u32 index;
            if (valid_handle(vm, r3, &index) && r2 < vm->strings[index].length)
                vm->strings[index].data[r2] = (char)r1;
            return r3;
        }
        case 14: return 65536u;
        case 15: return vm->host.tick_ms != 0 ? vm->host.tick_ms(vm->host.opaque) : 0u;
        case 16: return float_bits(fast_sin(bits_float(r3)));
        case 17: return float_bits(fast_cos(bits_float(r3)));
        case 18:
            value = bits_float(r3); divisor = fast_cos(value);
            return float_bits(fast_sin(value) / divisor);
        case 19: return float_bits(fast_sqrt(bits_float(r3)));
        case 20: return (bb_s32)r3 < 0 ? 0u - r3 : r3;
        case 21: return r3 & 0x7fffffffu;
        case 22: vm->data_ptr = r2; return r3;
        case 23: return bbvm_read_u32(vm, r3);
        case 24: (void)bbvm_write_u32(vm, r3, r2); return r3;
        /* BBASIC's environment value identifies the emulated BB runtime,
         * not the physical host model.  Legacy color programs (including
         * the bundled Rich/Monopoly game) select their RGB565 resource path
         * only for the 9388-compatible environment; reporting the 9588 host
         * makes those programs abort with their own error 1002. */
        case 25: return float_bits(9388.0f);
        case 32:
            int_to_text((bb_s32)r1, buffer); (void)bbvm_set_string(vm, r3, buffer);
            return r3;
        case 33: return (bb_u32)text_to_int(bbvm_get_string(vm, r3));
        case 34: a = bbvm_get_string(vm, r3); return (bb_u32)(unsigned char)a[0];
        case 35:
            a = bbvm_get_string(vm, r2); length = string_length(a);
            count = r1 < length ? r1 : length;
            (void)set_string_slice(vm, r3, a, 0u, count); return r3;
        case 36:
            a = bbvm_get_string(vm, r2); length = string_length(a);
            count = r1 < length ? r1 : length;
            (void)set_string_slice(vm, r3, a, length - count, count); return r3;
        case 37:
            a = bbvm_get_string(vm, r2); length = string_length(a);
            if (r1 > length) r1 = length;
            count = r0 < length - r1 ? r0 : length - r1;
            (void)set_string_slice(vm, r3, a, r1, count); return r3;
        case 38:
            a = bbvm_get_string(vm, r3); b = bbvm_get_string(vm, r2);
            length = string_length(a); if (r1 > length) return 0xffffffffu;
            found = string_find(a + r1, b);
            return found == 0 ? 0xffffffffu : (bb_u32)(found - a);
        case 39: return string_length(bbvm_get_string(vm, r3));
        default:
            vm->error = BBVM_ERR_PORT; vm->running = 0; return 0;
    }
}
