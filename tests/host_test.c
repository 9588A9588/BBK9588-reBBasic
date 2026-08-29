#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bbvm.h"
#include "bbvm_lib.h"

static void *host_alloc(void *opaque, bb_u32 size) { (void)opaque; return malloc(size); }
static void host_free(void *opaque, void *p) { (void)opaque; free(p); }
static bb_u32 host_tick(void *opaque) { (void)opaque; return 123456u; }

static int output_count;
static bb_u32 last_port, last_argument;
static int host_out(void *opaque, bbvm_t *vm, bb_u32 port, bb_u32 argument) {
    (void)opaque; (void)vm; ++output_count; last_port = port; last_argument = argument;
    return 1;
}

static void put32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void expect(int truth, const char *message) {
    if (!truth) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

int main(void) {
    unsigned char *memory = (unsigned char *)malloc(BBVM_MEMORY_SIZE);
    unsigned char code[96];
    bbvm_host_t host = {0, host_alloc, host_free, host_tick, host_out};
    bbvm_t vm;
    unsigned int p = 0, handle, value;
    memset(code, 0, sizeof(code));

    /* LD.I r0, 7 */
    code[p] = 0x14; code[p + 1] = 0x02; put32(code + p + 2, BB_R0);
    put32(code + p + 6, 7); p += 10;
    /* CAL.I r0, 5 (add) */
    code[p] = 0xb4; code[p + 1] = 0x02; put32(code + p + 2, BB_R0);
    put32(code + p + 6, 5); p += 10;
    /* PUSH r0; POP r1 */
    code[p] = 0x20; put32(code + p + 1, BB_R0); p += 5;
    code[p] = 0x30; put32(code + p + 1, BB_R1); p += 5;
    /* CMP.I r1, 12; JPC.Z target */
    code[p] = 0xa4; code[p + 1] = 0x02; put32(code + p + 2, BB_R1);
    put32(code + p + 6, 12); p += 10;
    code[p] = 0x71; code[p + 1] = 2; put32(code + p + 2, p + 16); p += 6;
    /* wrong path: OUT 99, 1 */
    code[p] = 0x50; code[p + 1] = 0x0a; put32(code + p + 2, 99);
    put32(code + p + 6, 1); p += 10;
    /* target: OUT 42, r1; EXIT */
    code[p] = 0x50; code[p + 1] = 0x08; put32(code + p + 2, 42);
    put32(code + p + 6, BB_R1); p += 10; code[p++] = 0xf0;

    bbvm_init(&vm, memory, BBVM_MEMORY_SIZE, &host);
    expect(bbvm_load(&vm, code, p), "load raw bytecode");
    expect(bbvm_run(&vm, 100), "execute arithmetic/control program");
    expect(vm.error == BBVM_OK && !vm.running, "normal EXIT");
    expect(output_count == 1 && last_port == 42 && last_argument == 12,
           "branch and OUT operands");
    expect(vm.reg[BB_R0] == 12 && vm.reg[BB_R1] == 12, "CAL/PUSH/POP result");

    handle = bbvm_new_string(&vm);
    expect((int)handle < 0, "negative string handle");
    expect(bbvm_set_string(&vm, handle, "9588"), "set string");
    vm.reg[BB_R3] = handle;
    expect(bbvm_in_port(&vm, 33) == 9588, "VAL/string-to-int port");
    expect(bbvm_in_port(&vm, 15) == 123456, "millisecond tick port");
    expect(bbvm_in_port(&vm, 25) == 0x4612b000u,
           "legacy 9388 environment compatibility port");
    expect(bbvm_lib_table_position(180, 1, &value) && value == 4,
           "LIB resource ids start at one");
    expect(bbvm_lib_table_position(180, 180, &value) && value == 720,
           "LIB final offset-table entry");
    expect(!bbvm_lib_table_position(180, 0, &value) &&
           !bbvm_lib_table_position(180, 181, &value),
           "reject out-of-range LIB resource ids");
    expect(bbvm_lib_pixel_size(153612, 240, 320, &value) && value == 153600,
           "LIB skips 12-byte record metadata before RGB565 pixels");
    expect(!bbvm_lib_pixel_size(153611, 240, 320, &value),
           "reject truncated LIB picture records");
    bbvm_destroy(&vm); free(memory);
    puts("host_test: all checks passed");
    return 0;
}
