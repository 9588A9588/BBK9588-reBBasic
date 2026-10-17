#include "bda_sdk.h"
#include "bbvm.h"
#include "bbvm_bda.h"
#include "bbvm_audio.h"

/* Load into the final VM allocation, not a second full-size BIN buffer. */
#define PROGRAM_READ_CHUNK 4096u
enum program_error { PROGRAM_OK, PROGRAM_OPEN, PROGRAM_SIZE, PROGRAM_SEEK,
                     PROGRAM_MEMORY, PROGRAM_READ, PROGRAM_IMAGE };
typedef struct program_load_info {
    int error, file_size, last_read;
    u32 bytes_read;
} program_load_info_t;

static int read_program(const char *path, bbvm_t *vm, program_load_info_t *info) {
    int file, size, got;
    u32 request, calls = 0u, max_calls;
    info->error = PROGRAM_OPEN;
    file = bda_fs_fopen_raw(path, "rb");
    if (!bda_fs_file_is_valid(file)) return 0;
    info->error = PROGRAM_SIZE;
    size = bda_fs_seek_raw(file, 0, BDA_SEEK_END);
    info->file_size = size;
    if (size <= 0 || (u32)size > BBVM_MEMORY_SIZE - BBVM_STACK_SIZE - 4u + 16u)
        goto close_file;
    info->error = PROGRAM_SEEK;
    if (bda_fs_seek_raw(file, 0, BDA_SEEK_SET) != 0) goto close_file;
    info->error = PROGRAM_MEMORY;
    vm->memory = (bb_u8 *)bda_alloc(vm->memory_size);
    if (vm->memory == 0) goto close_file;
    info->error = PROGRAM_READ;
    /* Positive short reads advance; zero/error stops without retrying forever. */
    max_calls = ((u32)size + 255u) / 256u + 16u;
    while (info->bytes_read < (u32)size) {
        request = (u32)size - info->bytes_read;
        if (request > PROGRAM_READ_CHUNK) request = PROGRAM_READ_CHUNK;
        if (++calls > max_calls) goto close_file;
        got = bda_fs_read_raw(file, vm->memory + info->bytes_read, request);
        info->last_read = got;
        if (got <= 0 || (u32)got > request) goto close_file;
        info->bytes_read += (u32)got;
    }
    info->error = PROGRAM_IMAGE;
    if (!bbvm_load(vm, vm->memory, (u32)size)) goto close_file;
    info->error = PROGRAM_OK;
close_file:
    (void)bda_fs_close_raw(file);
    return info->error == PROGRAM_OK;
}

static char *append_text(char *out, char *end, const char *text) {
    while (out < end && *text != '\0') *out++ = *text++;
    *out = '\0';
    return out;
}

static char *append_unsigned(char *out, char *end, u32 value) {
    char digits[10];
    u32 n = 0u;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value != 0u);
    while (n != 0u && out < end) *out++ = digits[--n];
    *out = '\0';
    return out;
}

static void show_program_error(const char *path, const program_load_info_t *info) {
    char message[512], *out = message, *end = message + sizeof(message) - 1u;
    const char *reason;
    switch (info->error) {
        case PROGRAM_OPEN: reason = "L1: Cannot open BIN"; break;
        case PROGRAM_SIZE: reason = "L2: Empty, oversized or unreadable BIN length"; break;
        case PROGRAM_SEEK: reason = "L3: Cannot rewind BIN"; break;
        case PROGRAM_MEMORY: reason = "L4: Cannot allocate 2 MiB VM memory"; break;
        case PROGRAM_READ: reason = "L5: BIN read stopped before completion"; break;
        default: reason = "L6: Invalid BIN image"; break;
    }
    out = append_text(out, end, reason);
    out = append_text(out, end, "\nPath: ");
    out = append_text(out, end, path);
    out = append_text(out, end, "\nSize: ");
    if (info->file_size < 0) out = append_text(out, end, "error");
    else out = append_unsigned(out, end, (u32)info->file_size);
    out = append_text(out, end, "  Read: ");
    out = append_unsigned(out, end, info->bytes_read);
    out = append_text(out, end, "  Last: ");
    if (info->last_read < 0) (void)append_text(out, end, "error");
    else (void)append_unsigned(out, end, (u32)info->last_read);
    bda_msgbox("BBVM R144", message);
}

static void select_working_directory(char *path) {
    char *p = path, *last = 0;
    while (*p != '\0') { if (*p == '\\' || *p == '/') last = p; ++p; }
    if (last != 0) { char saved = last[1]; last[1] = '\0';
        (void)bda_fs_chdir(path); last[1] = saved; }
}

__attribute__((section(".text.bda_main")))
int bda_main(void) {
    bda_file_selector_t selector;
    bbvm_host_t host;
    bbvm_t vm;
    program_load_info_t load_info;
    u32 batch = 0;
    int selected, result = 0;

    bda_memset(&selector, 0, sizeof(selector));
    /* Device B: is the SD card; this is only the initial browser directory. */
    selected = bda_gui_select_file(&selector, "B:\\BBasic\\", "bin", "Select BB program");
    if (selected != BDA_FILE_SELECTOR_SELECTED) return 0;
    bda_memset(&host, 0, sizeof(host));
    host.alloc = bbvm_bda_alloc; host.free = bbvm_bda_free;
    host.tick_ms = bbvm_bda_tick; host.out_port = bbvm_bda_out;
    host.in_port = bbvm_bda_audio_in;
    bbvm_init(&vm, 0, BBVM_MEMORY_SIZE, &host);
    bda_memset(&load_info, 0, sizeof(load_info));
    if (!read_program(selector.path, &vm, &load_info)) {
        show_program_error(selector.path, &load_info);
        result = load_info.error == PROGRAM_MEMORY ? 2 : 1;
        goto cleanup;
    }
    select_working_directory(selector.path);
    if (!bbvm_bda_start(&vm)) {
        bbvm_bda_stop();
        bda_msgbox("BBVM", "Cannot create BBVM window"); result = 4; goto cleanup;
    }
    while (vm.running) {
        (void)bbvm_run(&vm, 256u);
        bbvm_bda_audio_poll();
        /* Bounded nonblocking input/GUI service; never wait for user events. */
        if ((++batch & 1u) == 0u || !vm.running) bbvm_bda_pump(&vm);
    }
    bbvm_bda_stop();
    if (vm.error != BBVM_OK) {
        bda_msgbox("BBVM", bbvm_error_string(vm.error)); result = 5;
    }
cleanup:
    bbvm_destroy(&vm);
    if (vm.memory != 0) bda_free(vm.memory);
    return result;
}
