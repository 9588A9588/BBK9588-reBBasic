#ifndef RE_BBASIC_BBVM_H
#define RE_BBASIC_BBVM_H

/* Platform-neutral BBASIC bytecode interpreter.  It is deliberately
 * freestanding: the 9588 build does not have a C runtime. */
typedef unsigned char bb_u8;
typedef unsigned short bb_u16;
typedef unsigned int bb_u32;
typedef signed int bb_s32;

#define BBVM_MEMORY_SIZE (2u * 1024u * 1024u)
#define BBVM_STACK_SIZE 1000u
#define BBVM_STRING_SLOTS 256u

enum bbvm_register {
    BB_RP = 0, BB_RF = 1, BB_RS = 2, BB_RB = 3,
    BB_R0 = 4, BB_R1 = 5, BB_R2 = 6, BB_R3 = 7
};

enum bbvm_error {
    BBVM_OK = 0,
    BBVM_ERR_IMAGE = 1,
    BBVM_ERR_MEMORY = 2,
    BBVM_ERR_OPCODE = 3,
    BBVM_ERR_OPERAND = 4,
    BBVM_ERR_DIV_ZERO = 5,
    BBVM_ERR_PORT = 6,
    BBVM_ERR_HOST = 7,
    BBVM_ERR_LIMIT = 8
};

struct bbvm;

typedef void *(*bbvm_alloc_fn)(void *opaque, bb_u32 size);
typedef void (*bbvm_free_fn)(void *opaque, void *pointer);
typedef bb_u32 (*bbvm_tick_fn)(void *opaque);
typedef int (*bbvm_out_fn)(void *opaque, struct bbvm *vm, bb_u32 port,
                           bb_u32 argument);
typedef bb_u32 (*bbvm_in_fn)(void *opaque, struct bbvm *vm, bb_u32 port);

typedef struct bbvm_host {
    void *opaque;
    bbvm_alloc_fn alloc;
    bbvm_free_fn free;
    bbvm_tick_fn tick_ms;
    bbvm_out_fn out_port;
    bbvm_in_fn in_port;
} bbvm_host_t;

typedef struct bbvm_string {
    char *data;
    bb_u32 length;
    bb_u32 capacity;
} bbvm_string_t;

typedef struct bbvm {
    bb_u8 *memory;
    bb_u32 memory_size;
    bb_u32 image_size;
    bb_u32 data_ptr;
    bb_u32 reg[8];
    bb_u32 random_state;
    bb_u32 steps;
    int running;
    int error;
    int image_version;
    bbvm_host_t host;
    bbvm_string_t strings[BBVM_STRING_SLOTS];
} bbvm_t;

void bbvm_init(bbvm_t *vm, bb_u8 *memory, bb_u32 memory_size,
               const bbvm_host_t *host);
void bbvm_destroy(bbvm_t *vm);
/* image may equal vm->memory; a BBE header is compacted safely in place. */
int bbvm_load(bbvm_t *vm, const bb_u8 *image, bb_u32 size);
int bbvm_step(bbvm_t *vm);
int bbvm_run(bbvm_t *vm, bb_u32 instruction_budget);
const char *bbvm_error_string(int error);

bb_u32 bbvm_read_u32(bbvm_t *vm, bb_u32 address);
int bbvm_write_u32(bbvm_t *vm, bb_u32 address, bb_u32 value);
const char *bbvm_get_string(bbvm_t *vm, bb_u32 value);
int bbvm_set_string(bbvm_t *vm, bb_u32 handle, const char *value);
bb_u32 bbvm_new_string(bbvm_t *vm);
void bbvm_free_string(bbvm_t *vm, bb_u32 handle);
bb_u32 bbvm_in_port(bbvm_t *vm, bb_u32 port);

#endif
