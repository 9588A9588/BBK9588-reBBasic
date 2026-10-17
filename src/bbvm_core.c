#include "bbvm.h"

static const bb_u8 command_length[16] = {
    1, 10, 5, 5, 10, 10, 5, 6, 5, 1, 10, 10, 0, 0, 0, 1
};
static const bb_u8 type_size[5] = {4, 2, 1, 4, 4};

static void bytes_zero(void *pointer, bb_u32 size) {
    bb_u8 *p = (bb_u8 *)pointer;
    /* VM startup clears 2 MiB. Word stores cut loop/branch overhead without
     * libc, unaligned accesses, or writing beyond a short destination.
     * may_alias is required because this also initializes mixed-type structs. */
    typedef bb_u32 bb_zero_word __attribute__((__may_alias__));
    while (size != 0u && (((__UINTPTR_TYPE__)p) & 3u) != 0u) {
        *p++ = 0; --size;
    }
    while (size >= 16u) {
        bb_zero_word *w = (bb_zero_word *)p;
        w[0] = 0u; w[1] = 0u; w[2] = 0u; w[3] = 0u;
        p += 16; size -= 16u;
    }
    while (size >= 4u) {
        *(bb_zero_word *)p = 0u; p += 4; size -= 4u;
    }
    while (size-- != 0u) *p++ = 0;
}

static void bytes_copy(void *destination, const void *source, bb_u32 size) {
    bb_u8 *d = (bb_u8 *)destination;
    const bb_u8 *s = (const bb_u8 *)source;
    while (size-- != 0u) *d++ = *s++;
}

static int fail(bbvm_t *vm, int error) {
    vm->error = error;
    vm->running = 0;
    return 0;
}

static int range_ok(bbvm_t *vm, bb_u32 address, bb_u32 size) {
    return address <= vm->memory_size && size <= vm->memory_size - address;
}

static inline bb_u32 read_le32(const bb_u8 *p) {
    /* BBASIC bytecode is unaligned. Never cast it to a MIPS word pointer. */
    return (bb_u32)p[0] | ((bb_u32)p[1] << 8) |
           ((bb_u32)p[2] << 16) | ((bb_u32)p[3] << 24);
}

static bb_u32 raw_read(bbvm_t *vm, bb_u32 address, bb_u32 size) {
    bb_u32 value = 0;
    if (!range_ok(vm, address, size)) {
        fail(vm, BBVM_ERR_MEMORY);
        return 0;
    }
    if (size >= 1u) value |= vm->memory[address];
    if (size >= 2u) value |= (bb_u32)vm->memory[address + 1u] << 8;
    if (size >= 4u) {
        value |= (bb_u32)vm->memory[address + 2u] << 16;
        value |= (bb_u32)vm->memory[address + 3u] << 24;
    }
    return value;
}

static int raw_write(bbvm_t *vm, bb_u32 address, bb_u32 value, bb_u32 size) {
    if (!range_ok(vm, address, size)) return fail(vm, BBVM_ERR_MEMORY);
    vm->memory[address] = (bb_u8)value;
    if (size >= 2u) vm->memory[address + 1u] = (bb_u8)(value >> 8);
    if (size >= 4u) {
        vm->memory[address + 2u] = (bb_u8)(value >> 16);
        vm->memory[address + 3u] = (bb_u8)(value >> 24);
    }
    return 1;
}

bb_u32 bbvm_read_u32(bbvm_t *vm, bb_u32 address) {
    return raw_read(vm, address, 4u);
}

int bbvm_write_u32(bbvm_t *vm, bb_u32 address, bb_u32 value) {
    return raw_write(vm, address, value, 4u);
}

static inline __attribute__((always_inline)) bb_u32 get_data(bbvm_t *vm, bb_u32 mode, bb_u32 argument) {
    switch (mode) {
        case 0: return argument < 8u ? vm->reg[argument]
                                    : (fail(vm, BBVM_ERR_OPERAND), 0u);
        case 1: return argument < 8u ? raw_read(vm, vm->reg[argument], 4u)
                                    : (fail(vm, BBVM_ERR_OPERAND), 0u);
        case 2: return argument;
        case 3: return raw_read(vm, argument, 4u);
        default: fail(vm, BBVM_ERR_OPERAND); return 0;
    }
}

static inline __attribute__((always_inline)) int put_data(bbvm_t *vm, bb_u32 mode, bb_u32 argument,
                    bb_u32 value, bb_u32 data_type) {
    bb_u32 size;
    bb_u32 mask;
    if (data_type >= 5u) return fail(vm, BBVM_ERR_OPERAND);
    size = type_size[data_type];
    mask = size == 1u ? 0xffu : (size == 2u ? 0xffffu : 0xffffffffu);
    switch (mode) {
        case 0:
            if (argument >= 8u) return fail(vm, BBVM_ERR_OPERAND);
            vm->reg[argument] = (vm->reg[argument] & ~mask) | (value & mask);
            return 1;
        case 1:
            if (argument >= 8u) return fail(vm, BBVM_ERR_OPERAND);
            return raw_write(vm, vm->reg[argument], value, size);
        case 2: /* The historical VM silently discards writes to immediates. */
            return 1;
        case 3:
            return raw_write(vm, argument, value, size);
        default:
            return fail(vm, BBVM_ERR_OPERAND);
    }
}

static int push(bbvm_t *vm, bb_u32 value) {
    if (!raw_write(vm, vm->reg[BB_RS], value, 4u)) return 0;
    vm->reg[BB_RS] -= 4u;
    return 1;
}

static bb_u32 pop(bbvm_t *vm) {
    vm->reg[BB_RS] += 4u;
    return raw_read(vm, vm->reg[BB_RS], 4u);
}

static int test_flag(bb_u32 flags, bb_u32 condition) {
    switch (condition) {
        case 1: return (flags & 1u) != 0u;       /* Z */
        case 2: return (flags & 2u) != 0u;       /* B */
        case 3: return (flags & 3u) != 0u;       /* BE */
        case 4: return (flags & 4u) != 0u;       /* A */
        case 5: return (flags & 5u) != 0u;       /* AE */
        case 6: return (flags & 1u) == 0u;       /* NZ */
        default: return 0;
    }
}

static float bits_float(bb_u32 bits) {
    union { bb_u32 u; float f; } value;
    value.u = bits;
    return value.f;
}

static bb_u32 float_bits(float number) {
    union { bb_u32 u; float f; } value;
    value.f = number;
    return value.u;
}

static bb_s32 narrowed_signed(bb_u32 value, bb_u32 type) {
    if (type == 2u) return (bb_s32)(value & 0xffu);
    if (type == 1u) return (bb_s32)(value & 0xffffu);
    return (bb_s32)value;
}

void bbvm_init(bbvm_t *vm, bb_u8 *memory, bb_u32 memory_size,
               const bbvm_host_t *host) {
    bytes_zero(vm, (bb_u32)sizeof(*vm));
    vm->memory = memory;
    vm->memory_size = memory_size;
    vm->random_state = 1u;
    if (host != 0) {
        vm->host.opaque = host->opaque;
        vm->host.alloc = host->alloc;
        vm->host.free = host->free;
        vm->host.tick_ms = host->tick_ms;
        vm->host.out_port = host->out_port;
        vm->host.in_port = host->in_port;
    }
    if (memory != 0) bytes_zero(memory, memory_size);
}

void bbvm_destroy(bbvm_t *vm) {
    bb_u32 i;
    if (vm->host.free != 0) {
        for (i = 0; i < BBVM_STRING_SLOTS; ++i) {
            if (vm->strings[i].data != 0)
                vm->host.free(vm->host.opaque, vm->strings[i].data);
        }
    }
    bytes_zero(vm->strings, (bb_u32)sizeof(vm->strings));
}

int bbvm_load(bbvm_t *vm, const bb_u8 *image, bb_u32 size) {
    bb_u32 offset = 0;
    bb_u32 payload;
    if (vm->memory == 0 || image == 0 || size == 0u)
        return fail(vm, BBVM_ERR_IMAGE);
    if (size >= 16u && image[0] == 'B' && image[1] == 'B' && image[2] == 'E') {
        offset = 16u;
        vm->image_version = image[7] == 0x40u ? 19 : 20;
    } else {
        vm->image_version = 19;
    }
    payload = size - offset;
    if (vm->memory_size < BBVM_STACK_SIZE + 4u ||
        payload > vm->memory_size - BBVM_STACK_SIZE - 4u)
        return fail(vm, BBVM_ERR_IMAGE);
    if (image == vm->memory) {
        /* Copy left before clearing its tail: safe for the BBE header overlap. */
        if (offset != 0u) bytes_copy(vm->memory, image + offset, payload);
        bytes_zero(vm->memory + payload, vm->memory_size - payload);
    } else {
        bytes_zero(vm->memory, vm->memory_size);
        bytes_copy(vm->memory, image + offset, payload);
    }
    vm->image_size = payload;
    vm->data_ptr = 0;
    bytes_zero(vm->reg, (bb_u32)sizeof(vm->reg));
    vm->reg[BB_RB] = payload;
    vm->reg[BB_RS] = payload + BBVM_STACK_SIZE;
    vm->steps = 0;
    vm->error = BBVM_OK;
    vm->running = 1;
    return 1;
}

int bbvm_step(bbvm_t *vm) {
    bb_u32 ip, command, length, first_mode = 0, second_mode = 0;
    bb_u32 first = 0, second = 0, type = 0, operation = 0, flag = 0;
    bb_u32 left, right, result;
    if (!vm->running) return 0;
    ip = vm->reg[BB_RP];
    if (!range_ok(vm, ip, 1u)) return fail(vm, BBVM_ERR_MEMORY);
    command = vm->memory[ip] >> 4;
    length = command_length[command];
    if (length == 0u || !range_ok(vm, ip, length))
        return fail(vm, BBVM_ERR_OPCODE);
    if (length == 5u) {
        first_mode = vm->memory[ip] & 15u;
        first = read_le32(vm->memory + ip + 1u);
    } else if (length == 6u) {
        flag = vm->memory[ip] & 15u;
        first_mode = vm->memory[ip + 1u];
        first = read_le32(vm->memory + ip + 2u);
    } else if (length == 10u) {
        type = vm->memory[ip] & 15u;
        if (type > 4u) return fail(vm, BBVM_ERR_OPERAND);
        operation = vm->memory[ip + 1u] >> 4;
        first_mode = (vm->memory[ip + 1u] & 15u) >> 2;
        second_mode = vm->memory[ip + 1u] & 3u;
        first = read_le32(vm->memory + ip + 2u);
        second = read_le32(vm->memory + ip + 6u);
    }
    if (!vm->running) return 0;
    switch (command) {
        case 0x0:
            vm->reg[BB_RP] += length;
            break;
        case 0x1:
            result = get_data(vm, second_mode, second);
            if (!put_data(vm, first_mode, first, result, type)) return 0;
            vm->reg[BB_RP] += length;
            break;
        case 0x2:
            if ((vm->memory[ip] & 15u) == 15u) {
                bb_u32 r;
                for (r = BB_RF; r <= BB_R3; ++r)
                    if (!push(vm, vm->reg[r])) return 0;
            } else if (!push(vm, get_data(vm, first_mode, first))) return 0;
            vm->reg[BB_RP] += length;
            break;
        case 0x3:
            if ((vm->memory[ip] & 15u) == 15u) {
                bb_s32 r;
                for (r = BB_R3; r >= BB_RF; --r) vm->reg[r] = pop(vm);
            } else if (!put_data(vm, first_mode, first, pop(vm), type)) return 0;
            vm->reg[BB_RP] += length;
            break;
        case 0x4:
            right = get_data(vm, second_mode, second);
            result = bbvm_in_port(vm, right);
            if (!vm->running || !put_data(vm, first_mode, first, result, type)) return 0;
            vm->reg[BB_RP] += length;
            break;
        case 0x5:
            left = get_data(vm, first_mode, first);
            right = get_data(vm, second_mode, second);
            if (vm->host.out_port == 0 ||
                !vm->host.out_port(vm->host.opaque, vm, left, right))
                return fail(vm, BBVM_ERR_PORT);
            vm->reg[BB_RP] += length;
            break;
        case 0x6:
            vm->reg[BB_RP] = get_data(vm, first_mode, first);
            break;
        case 0x7:
            if (test_flag(vm->reg[BB_RF], flag))
                vm->reg[BB_RP] = get_data(vm, first_mode, first);
            else vm->reg[BB_RP] += length;
            break;
        case 0x8:
            if (!push(vm, vm->reg[BB_RP] + length)) return 0;
            vm->reg[BB_RP] = get_data(vm, first_mode, first);
            break;
        case 0x9:
            vm->reg[BB_RP] = pop(vm);
            break;
        case 0xa:
            left = get_data(vm, first_mode, first);
            right = get_data(vm, second_mode, second);
            if (type == 3u) {
                float a = bits_float(left), b = bits_float(right);
                vm->reg[BB_RF] = a == b ? 1u : (a < b ? 2u : 4u);
            } else {
                bb_s32 a = narrowed_signed(left, type);
                bb_s32 b = narrowed_signed(right, type);
                vm->reg[BB_RF] = a == b ? 1u : (a < b ? 2u : 4u);
            }
            vm->reg[BB_RP] += length;
            break;
        case 0xb:
            left = get_data(vm, first_mode, first);
            right = get_data(vm, second_mode, second);
            if (type == 3u) {
                float a = bits_float(left), b = bits_float(right), c;
                if ((operation == 3u || operation == 4u) && b == 0.0f)
                    return fail(vm, BBVM_ERR_DIV_ZERO);
                if (operation == 0u) c = a + b;
                else if (operation == 1u) c = a - b;
                else if (operation == 2u) c = a * b;
                else if (operation == 3u) c = a / b;
                else return fail(vm, BBVM_ERR_OPERAND);
                result = float_bits(c);
            } else {
                bb_s32 a = narrowed_signed(left, type);
                bb_s32 b = narrowed_signed(right, type);
                if ((operation == 3u || operation == 4u) && b == 0)
                    return fail(vm, BBVM_ERR_DIV_ZERO);
                if (operation == 0u) result = (bb_u32)a + (bb_u32)b;
                else if (operation == 1u) result = (bb_u32)a - (bb_u32)b;
                else if (operation == 2u) result = (bb_u32)a * (bb_u32)b;
                else if (operation == 3u)
                    result = a == (-2147483647-1) && b == -1 ? 0x80000000u : (bb_u32)(a / b);
                else if (operation == 4u)
                    result = a == (-2147483647-1) && b == -1 ? 0u : (bb_u32)(a % b);
                else return fail(vm, BBVM_ERR_OPERAND);
            }
            if (!put_data(vm, first_mode, first, result, type)) return 0;
            vm->reg[BB_RP] += length;
            break;
        case 0xf:
            vm->running = 0;
            break;
        default:
            return fail(vm, BBVM_ERR_OPCODE);
    }
    ++vm->steps;
    return vm->running;
}

int bbvm_run(bbvm_t *vm, bb_u32 instruction_budget) {
    while (vm->running && instruction_budget-- != 0u) (void)bbvm_step(vm);
    return vm->error == BBVM_OK;
}

const char *bbvm_error_string(int error) {
    switch (error) {
        case BBVM_OK: return "ok";
        case BBVM_ERR_IMAGE: return "invalid or oversized BB program";
        case BBVM_ERR_MEMORY: return "virtual memory access outside 2 MiB";
        case BBVM_ERR_OPCODE: return "unknown or truncated bytecode";
        case BBVM_ERR_OPERAND: return "invalid operand";
        case BBVM_ERR_DIV_ZERO: return "division by zero";
        case BBVM_ERR_PORT: return "unsupported I/O port";
        case BBVM_ERR_HOST: return "9588 host operation failed";
        case BBVM_ERR_LIMIT: return "resource limit reached";
        default: return "unknown VM error";
    }
}
