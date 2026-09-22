#include "bda_sdk.h"
#include "bbvm.h"
#include "bbvm_bda.h"

static int read_program(const char *path, u8 **out_data, u32 *out_size) {
    int file, size, got;
    u8 *data;
    file = bda_fs_fopen_raw(path, "rb");
    if (!bda_fs_file_is_valid(file)) return 0;
    size = bda_fs_seek_raw(file, 0, BDA_SEEK_END);
    if (size <= 0 || bda_fs_seek_raw(file, 0, BDA_SEEK_SET) < 0) {
        (void)bda_fs_close_raw(file); return 0;
    }
    data = (u8 *)bda_alloc((u32)size);
    if (data == 0) { (void)bda_fs_close_raw(file); return 0; }
    got = bda_fs_read_raw(file, data, (u32)size);
    (void)bda_fs_close_raw(file);
    if (got != size) { bda_free(data); return 0; }
    *out_data = data; *out_size = (u32)size; return 1;
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
    u8 *image = 0, *memory = 0;
    u32 image_size = 0;
    int selected, result = 0;

    bda_memset(&selector, 0, sizeof(selector));
    selected = bda_gui_select_file(&selector, "A:\\BBasic\\", "bin", "Select BB program");
    if (selected != BDA_FILE_SELECTOR_SELECTED) return 0;
    if (!read_program(selector.path, &image, &image_size)) {
        bda_msgbox("BBVM", "Cannot read selected program"); return 1;
    }
    memory = (u8 *)bda_alloc(BBVM_MEMORY_SIZE);
    if (memory == 0) {
        bda_free(image); bda_msgbox("BBVM", "Not enough memory for 2 MiB VM"); return 2;
    }
    bda_memset(&host, 0, sizeof(host));
    host.alloc = bbvm_bda_alloc; host.free = bbvm_bda_free;
    host.tick_ms = bbvm_bda_tick; host.out_port = bbvm_bda_out;
    bbvm_init(&vm, memory, BBVM_MEMORY_SIZE, &host);
    if (!bbvm_load(&vm, image, image_size)) {
        bda_msgbox("BBVM", bbvm_error_string(vm.error)); result = 3; goto cleanup;
    }
    bda_free(image); image = 0;
    select_working_directory(selector.path);
    if (!bbvm_bda_start(&vm)) {
        bda_msgbox("BBVM", "Cannot create BBVM window"); result = 4; goto cleanup;
    }
    while (vm.running) {
        (void)bbvm_run(&vm, 512u);
        bbvm_bda_pump(&vm);
    }
    bbvm_bda_stop();
    if (vm.error != BBVM_OK) {
        bda_msgbox("BBVM", bbvm_error_string(vm.error)); result = 5;
    }
cleanup:
    bbvm_destroy(&vm);
    bda_free(memory);
    if (image != 0) bda_free(image);
    return result;
}
