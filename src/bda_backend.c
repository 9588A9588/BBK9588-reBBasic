#include "bda_sdk.h"
#include "bbvm_bda.h"
#include "bbvm_lib.h"

#define SCREEN_MAX_WIDTH 240
#define SCREEN_MAX_HEIGHT 320
#define PAGE_COUNT 9
#define PICTURE_COUNT 64
#define FILE_COUNT 16
#define KEY_QUEUE_SIZE 32
#define GUI_GET_PIXEL_RGB 0x374u

typedef struct picture {
    u16 *pixels;
    u32 width;
    u32 height;
} picture_t;

typedef struct backend {
    bbvm_t *vm;
    bda_handle_t frame;
    bda_handle_t draw;
    bda_handle_t draw_owner;
    bda_handle_t pages[PAGE_COUNT];
    u16 *page_pixels[PAGE_COUNT];
    u16 *visible_pixels;
    u16 *blit_pixels;
    void *draw_object;
    picture_t pictures[PICTURE_COUNT];
    int files[FILE_COUNT];
    u32 queue[KEY_QUEUE_SIZE];
    u32 queue_read;
    u32 queue_write;
    u32 old_keys;
    int screen_width;
    int screen_height;
    int cursor_x;
    int cursor_y;
    int font_width;
    int font_height;
    int text_transparent;
    u32 foreground;
    u32 background;
    u32 pen_color[PAGE_COUNT + 1];
    int pen_x[PAGE_COUNT + 1];
    int pen_y[PAGE_COUNT + 1];
    int detached;
    int timer_started;
} backend_t;

static backend_t g;

static void zero_bytes(void *pointer, u32 size) {
    u8 *p = (u8 *)pointer;
    while (size-- != 0u) *p++ = 0;
}

static u32 read32(bbvm_t *vm, u32 address) {
    return bbvm_read_u32(vm, address);
}

static void color_parts(u32 bgr, u32 *red, u32 *green, u32 *blue) {
    *red = bgr & 0xffu;
    *green = (bgr >> 8) & 0xffu;
    *blue = (bgr >> 16) & 0xffu;
}

static u16 color_565(u32 bgr) {
    u32 red, green, blue;
    color_parts(bgr, &red, &green, &blue);
    return (u16)(((red & 0xf8u) << 8) | ((green & 0xfcu) << 3) |
                 (blue >> 3));
}

static void queue_value(u32 value) {
    u32 next = (g.queue_write + 1u) % KEY_QUEUE_SIZE;
    if (next == g.queue_read) g.queue_read = (g.queue_read + 1u) % KEY_QUEUE_SIZE;
    g.queue[g.queue_write] = value;
    g.queue_write = next;
}

static u32 dequeue_value(void) {
    u32 value;
    if (g.queue_read == g.queue_write) return 0;
    value = g.queue[g.queue_read];
    g.queue_read = (g.queue_read + 1u) % KEY_QUEUE_SIZE;
    return value;
}

static u32 packet_mask(const bda_gui_input_packet_t *packet) {
    u32 mask = 0;
    u32 i;
    for (i = 0; i < BDA_GUI_INPUT_PACKET_SIZE; ++i)
        if (packet->bytes[i] == 1u) mask |= 1u << i;
    return mask;
}

static u32 bb_key_from_packet_index(u32 index) {
    static const u8 keys[6] = {39, 37, 40, 38, 27, 13};
    return index < 6u ? keys[index] : 0u;
}

static void sample_keys(void) {
    bda_gui_input_packet_t packet;
    u32 now, down, i;
    (void)bda_gui_input_packet(&packet);
    now = packet_mask(&packet);
    down = now & ~g.old_keys;
    g.old_keys = now;
    for (i = 0; i < 6u; ++i)
        if ((down & (1u << i)) != 0u) queue_value(bb_key_from_packet_index(i));
}

static int bb_key_pressed(u32 key) {
    u32 native = 0;
    if (key == 37u) native = BDA_KEY_LEFT;
    else if (key == 38u) native = BDA_KEY_UP;
    else if (key == 39u) native = BDA_KEY_RIGHT;
    else if (key == 40u) native = BDA_KEY_DOWN;
    else if (key == 27u) native = BDA_KEY_ESCAPE;
    else if (key == 13u) native = BDA_KEY_ENTER;
    return native != 0u ? bda_gui_key_pressed(native) : 0;
}

static int backend_window_proc(bda_handle_t handle, u32 message,
                               u32 wparam, u32 lparam) {
    (void)wparam;
    if (message == BDA_MSG_DRAW_CONTEXT_ATTACH) {
        g.frame = handle;
        if (g.draw == 0) {
            g.draw = bda_gui_current_draw(handle);
            g.draw_owner = handle;
        }
    } else if (message == BDA_MSG_DRAW_CONTEXT_DETACH) {
        if (g.draw != 0 && (g.draw_owner == 0 || g.draw_owner == handle)) {
            bda_gui_end_draw(g.draw);
            g.draw = 0;
            g.draw_owner = 0;
        }
        g.detached = 1;
    } else if (message == BDA_MSG_TOUCH_COORDINATE) {
        queue_value(0x80000000u | lparam);
    }
    return bda_gui_default_proc(handle, message, wparam, lparam);
}

static void pump_once(void) {
    bda_gui_message_t message;
    zero_bytes(&message, sizeof(message));
    if (g.frame != 0) (void)bda_gui_event_pump_frame_once(&message, g.frame);
    sample_keys();
}

static u32 wait_key(void) {
    u32 key;
    for (;;) {
        pump_once();
        key = dequeue_value();
        if (key != 0u || g.detached) return key;
        bda_sys_delay(1u);
    }
}

static int context_index(int page) {
    return page < 0 ? PAGE_COUNT : page;
}

static u16 *page_surface(int page) {
    if (page < 0) return g.visible_pixels;
    if (page >= PAGE_COUNT) return 0;
    return g.page_pixels[page];
}

static void draw_begin(int page) {
    if (page < 0) (void)bda_gui_draw_guard_begin();
}

static void draw_end(int page) {
    if (page < 0) (void)bda_gui_draw_guard_end();
}

static void select_object(bda_handle_t context) {
    if (context != 0 && g.draw_object != 0)
        (void)bda_gui_select_draw_object(context, g.draw_object);
}

static void render_visible_region(int x, int y, int width, int height) {
    bda_gui_picture_t picture;
    u16 *source;
    int row, column;
    if (g.draw == 0 || g.visible_pixels == 0 || width <= 0 || height <= 0)
        return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > g.screen_width) width = g.screen_width - x;
    if (y + height > g.screen_height) height = g.screen_height - y;
    if (width <= 0 || height <= 0) return;

    if (x == 0 && width == g.screen_width) {
        source = g.visible_pixels + y * g.screen_width;
    } else {
        if (g.blit_pixels == 0) return;
        source = g.blit_pixels;
        for (row = 0; row < height; ++row)
            for (column = 0; column < width; ++column)
                source[row * width + column] =
                    g.visible_pixels[(y + row) * g.screen_width + x + column];
    }

    zero_bytes(&picture, sizeof(picture));
    picture.width = (u32)width;
    picture.height = (u32)height;
    picture.source_pixels = source;
    picture.selected_index = -1;
    draw_begin(-1);
    select_object(g.draw);
    (void)bda_gui_render_picture(g.draw, x, y, width, height, &picture);
    draw_end(-1);
}

static void put_pixel(int page, int x, int y, u32 color) {
    u16 *surface = page_surface(page);
    if (surface == 0 || x < 0 || y < 0 || x >= g.screen_width ||
        y >= g.screen_height) return;
    surface[y * g.screen_width + x] = color_565(color);
    if (page < 0) render_visible_region(x, y, 1, 1);
}

static void fill_rect(int page, int x, int y, int width, int height, u32 color) {
    u16 *surface = page_surface(page);
    u16 converted;
    int row, column;
    if (surface == 0 || width <= 0 || height <= 0) return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x + width > g.screen_width) width = g.screen_width - x;
    if (y + height > g.screen_height) height = g.screen_height - y;
    if (width <= 0 || height <= 0) return;
    converted = color_565(color);
    for (row = 0; row < height; ++row)
        for (column = 0; column < width; ++column)
            surface[(y + row) * g.screen_width + x + column] = converted;
    if (page < 0) render_visible_region(x, y, width, height);
}

static void surface_pixel_565(u16 *surface, int x, int y, u16 color) {
    if (surface != 0 && x >= 0 && y >= 0 && x < g.screen_width &&
        y < g.screen_height)
        surface[y * g.screen_width + x] = color;
}

static void line_surface(int page, int x0, int y0, int x1, int y1,
                         u32 color) {
    u16 *surface = page_surface(page);
    u16 converted = color_565(color);
    int min_x = x0 < x1 ? x0 : x1;
    int min_y = y0 < y1 ? y0 : y1;
    int max_x = x0 > x1 ? x0 : x1;
    int max_y = y0 > y1 ? y0 : y1;
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy_abs = y1 > y0 ? y1 - y0 : y0 - y1;
    int dy = -dy_abs;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    if (surface == 0) return;
    for (;;) {
        surface_pixel_565(surface, x0, y0, converted);
        if (x0 == x1 && y0 == y1) break;
        { int twice = error * 2;
          if (twice >= dy) { error += dy; x0 += sx; }
          if (twice <= dx) { error += dx; y0 += sy; } }
    }
    if (page < 0)
        render_visible_region(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
}

static void circle_surface(int page, int cx, int cy, int radius, u32 color) {
    u16 *surface = page_surface(page);
    u16 converted = color_565(color);
    int x = radius, y = 0, error = 1 - radius;
    if (surface == 0 || radius < 0) return;
    while (x >= y) {
        surface_pixel_565(surface, cx + x, cy + y, converted);
        surface_pixel_565(surface, cx + y, cy + x, converted);
        surface_pixel_565(surface, cx - y, cy + x, converted);
        surface_pixel_565(surface, cx - x, cy + y, converted);
        surface_pixel_565(surface, cx - x, cy - y, converted);
        surface_pixel_565(surface, cx - y, cy - x, converted);
        surface_pixel_565(surface, cx + y, cy - x, converted);
        surface_pixel_565(surface, cx + x, cy - y, converted);
        ++y;
        if (error < 0) error += 2 * y + 1;
        else { --x; error += 2 * (y - x) + 1; }
    }
    if (page < 0)
        render_visible_region(cx - radius, cy - radius,
                              radius * 2 + 1, radius * 2 + 1);
}

static u32 color_from_565(u16 pixel) {
    u32 red = (pixel >> 11) & 0x1fu;
    u32 green = (pixel >> 5) & 0x3fu;
    u32 blue = pixel & 0x1fu;
    red = (red << 3) | (red >> 2);
    green = (green << 2) | (green >> 4);
    blue = (blue << 3) | (blue >> 2);
    return red | (green << 8) | (blue << 16);
}

static u16 native_pixel_565(bda_handle_t context, int x, int y) {
    u32 red = 0, green = 0, blue = 0;
    typedef int (*fn_t)(u32, u32, u32, u32 *, u32 *, u32 *);
    fn_t fn;
    if (context == 0) return 0;
    fn = (fn_t)bda_sdk_internal_api(bda_sdk_internal_gui(), GUI_GET_PIXEL_RGB);
    (void)fn((u32)context, (u32)x, (u32)y, &red, &green, &blue);
    return (u16)(((red & 0xf8u) << 8) | ((green & 0xfcu) << 3) |
                 ((blue & 0xffu) >> 3));
}

static u32 get_pixel(int page, int x, int y) {
    u16 *surface = page_surface(page);
    if (surface == 0 || x < 0 || y < 0 || x >= g.screen_width ||
        y >= g.screen_height) return 0;
    return color_from_565(surface[y * g.screen_width + x]);
}

static void copy_context(int source, int destination, int sx, int sy,
                         int width, int height, int dx, int dy) {
    u16 *src = page_surface(source), *dst = page_surface(destination);
    int row, column;
    if (src == 0 || dst == 0 || g.blit_pixels == 0 ||
        width <= 0 || height <= 0) return;
    if (sx < 0) { dx -= sx; width += sx; sx = 0; }
    if (sy < 0) { dy -= sy; height += sy; sy = 0; }
    if (dx < 0) { sx -= dx; width += dx; dx = 0; }
    if (dy < 0) { sy -= dy; height += dy; dy = 0; }
    if (sx + width > g.screen_width) width = g.screen_width - sx;
    if (sy + height > g.screen_height) height = g.screen_height - sy;
    if (dx + width > g.screen_width) width = g.screen_width - dx;
    if (dy + height > g.screen_height) height = g.screen_height - dy;
    if (width <= 0 || height <= 0) return;

    /* Stage the source first so overlapping copies on one BB page have
     * memmove semantics.  BB page blits always use RGB565 magenta as the
     * transparent color, matching the historical renderer. */
    for (row = 0; row < height; ++row)
        for (column = 0; column < width; ++column)
            g.blit_pixels[row * width + column] =
                src[(sy + row) * g.screen_width + sx + column];
    for (row = 0; row < height; ++row)
        for (column = 0; column < width; ++column) {
            u16 pixel = g.blit_pixels[row * width + column];
            if (pixel != BDA_GUI_COLOR_KEY_MAGENTA_RGB565)
                dst[(dy + row) * g.screen_width + dx + column] = pixel;
        }
    if (destination < 0) render_visible_region(dx, dy, width, height);
}

static void int_text(s32 value, char *out) {
    char reverse[16];
    u32 magnitude, n = 0, i = 0;
    if (value < 0) { *out++ = '-'; magnitude = 0u - (u32)value; }
    else magnitude = (u32)value;
    do { reverse[n++] = (char)('0' + magnitude % 10u); magnitude /= 10u; }
    while (magnitude != 0u);
    while (n != 0u) out[i++] = reverse[--n];
    out[i] = '\0';
}

static void float_text(u32 bits, char *out) {
    union { u32 u; float f; } value;
    s32 whole;
    u32 fraction, n;
    char *p;
    value.u = bits;
    if (value.f < 0.0f) { *out++ = '-'; value.f = -value.f; }
    whole = (s32)value.f;
    int_text(whole, out);
    p = out; while (*p != '\0') ++p;
    *p++ = '.';
    fraction = (u32)((value.f - (float)whole) * 1000000.0f + 0.5f);
    for (n = 100000u; n != 0u; n /= 10u) *p++ = (char)('0' + fraction / n % 10u);
    while (p[-1] == '0' && p[-2] != '.') --p;
    if (p[-1] == '.') *p++ = '0';
    *p = '\0';
}

static int display_width(const char *text) {
    int width = 0;
    while (*text != '\0') {
        if ((u8)*text >= 0x80u && text[1] != '\0') {
            width += g.font_width * 2; text += 2;
        } else { width += g.font_width; ++text; }
    }
    return width;
}

static void newline(void) {
    g.cursor_x = 0;
    g.cursor_y += g.font_height;
    if (g.cursor_y + g.font_height > g.screen_height) {
        fill_rect(-1, 0, 0, g.screen_width, g.screen_height, g.background);
        g.cursor_y = 0;
    }
}

static void display_text(const char *text, int add_newline) {
    char segment[128];
    u32 n;
    while (*text != '\0') {
        n = 0;
        while (text[n] != '\0' && text[n] != '\n' && n + 1u < sizeof(segment)) {
            segment[n] = text[n]; ++n;
        }
        segment[n] = '\0';
        if (n != 0u && g.draw != 0) {
            int width = display_width(segment);
            int text_x, text_y, row, column;
            if (g.cursor_x + width > g.screen_width) newline();
            if (!g.text_transparent)
                fill_rect(-1, g.cursor_x, g.cursor_y, width, g.font_height,
                          g.background);
            text_x = g.cursor_x;
            text_y = g.cursor_y;
            (void)bda_gui_draw_guard_begin();
            select_object(g.draw);
            (void)bda_gui_set_text_mode(g.draw, g.text_transparent ? 1u : 2u);
            { u32 red, green, blue, converted;
              color_parts(g.foreground, &red, &green, &blue);
              converted = (u32)bda_gui_rgb(g.draw, red, green, blue);
              (void)bda_gui_set_text_color(g.draw, converted); }
            (void)bda_gui_draw_text(g.draw, g.cursor_x, g.cursor_y, segment, -1);
            (void)bda_gui_draw_guard_end();
            /* Text is rasterized by the firmware.  Mirror the affected cells
             * back into the visible BB surface so a later transparent sprite
             * or page copy does not restore stale pixels over the glyphs. */
            if (g.visible_pixels != 0) {
                for (row = 0; row < g.font_height && text_y + row < g.screen_height;
                     ++row)
                    for (column = 0; column < width &&
                         text_x + column < g.screen_width; ++column)
                        if (text_x + column >= 0 && text_y + row >= 0)
                            g.visible_pixels[(text_y + row) * g.screen_width +
                                             text_x + column] =
                                native_pixel_565(g.draw, text_x + column,
                                                 text_y + row);
            }
            g.cursor_x += width;
        }
        text += n;
        if (*text == '\n') { newline(); ++text; }
        else if (n == sizeof(segment) - 1u) continue;
    }
    if (add_newline) newline();
}

static int new_page(void) {
    int i;
    for (i = 0; i < PAGE_COUNT; ++i) {
        if (g.pages[i] == 0) {
            g.pages[i] = bda_gui_compatible_context_create(g.draw);
            if (g.pages[i] == 0 || (s32)(u32)g.pages[i] == -1) {
                g.pages[i] = 0; return -1;
            }
            g.page_pixels[i] = (u16 *)bda_alloc(
                (u32)SCREEN_MAX_WIDTH * (u32)SCREEN_MAX_HEIGHT * 2u);
            if (g.page_pixels[i] == 0) {
                bda_gui_compatible_context_free(g.pages[i]);
                g.pages[i] = 0;
                return -1;
            }
            select_object(g.pages[i]);
            fill_rect(i, 0, 0, g.screen_width, g.screen_height, 0u);
            return i;
        }
    }
    return -1;
}

static void free_picture(int handle) {
    if (handle >= 0 && handle < PICTURE_COUNT && g.pictures[handle].pixels != 0) {
        bda_free(g.pictures[handle].pixels);
        zero_bytes(&g.pictures[handle], sizeof(g.pictures[handle]));
    }
}

static int load_picture(const char *path, u32 wanted) {
    int file, slot;
    u32 count, offset, record_length, table_position, pixel_size;
    u16 dimensions[2];
    for (slot = 0; slot < PICTURE_COUNT && g.pictures[slot].pixels != 0; ++slot) {}
    if (slot == PICTURE_COUNT) return -1;
    file = bda_fs_fopen_raw(path, "rb");
    if (!bda_fs_file_is_valid(file)) return -1;
    /* BBASIC LIB resource ids are one-based.  The offset table starts at
     * byte 4, so id 1 selects its first entry. */
    if (bda_fs_read_raw(file, &count, 4u) != 4 ||
        !bbvm_lib_table_position(count, wanted, &table_position) ||
        bda_fs_seek_raw(file, (s32)table_position, BDA_SEEK_SET) < 0 ||
        bda_fs_read_raw(file, &offset, 4u) != 4 ||
        bda_fs_seek_raw(file, (s32)offset, BDA_SEEK_SET) < 0 ||
        bda_fs_read_raw(file, &record_length, 4u) != 4 ||
        bda_fs_read_raw(file, dimensions, 4u) != 4 ||
        /* The width/height pair is followed by two reserved u32 fields.
         * Pixel data starts 16 bytes after the record offset. */
        bda_fs_seek_raw(file, (s32)(BBVM_LIB_RECORD_METADATA_SIZE - 4u),
                        BDA_SEEK_CUR) < 0) {
        (void)bda_fs_close_raw(file); return -1;
    }
    g.pictures[slot].width = dimensions[0];
    g.pictures[slot].height = dimensions[1];
    if (!bbvm_lib_pixel_size(record_length, dimensions[0], dimensions[1],
                             &pixel_size)) {
        zero_bytes(&g.pictures[slot], sizeof(g.pictures[slot]));
        (void)bda_fs_close_raw(file); return -1;
    }
    g.pictures[slot].pixels = (u16 *)bda_alloc(pixel_size);
    if (g.pictures[slot].pixels == 0 ||
        bda_fs_read_raw(file, g.pictures[slot].pixels, pixel_size) !=
                        (int)pixel_size) {
        free_picture(slot); (void)bda_fs_close_raw(file); return -1;
    }
    (void)bda_fs_close_raw(file);
    return slot;
}

static void draw_picture(int page, int picture, int dx, int dy, int width,
                         int height, int sx, int sy, int mode) {
    picture_t *pic;
    u16 *surface;
    int x, y;
    (void)mode;
    if (picture < 0 || picture >= PICTURE_COUNT ||
        g.pictures[picture].pixels == 0) return;
    pic = &g.pictures[picture];
    surface = page_surface(page);
    if (surface == 0) return;
    if (width <= 0) width = (int)pic->width;
    if (height <= 0) height = (int)pic->height;

    /* Clip the requested source rectangle and keep the destination aligned
     * with the surviving pixels. */
    if (sx < 0) { dx -= sx; width += sx; sx = 0; }
    if (sy < 0) { dy -= sy; height += sy; sy = 0; }
    if (dx < 0) { sx -= dx; width += dx; dx = 0; }
    if (dy < 0) { sy -= dy; height += dy; dy = 0; }
    if (sx + width > (int)pic->width) width = (int)pic->width - sx;
    if (sy + height > (int)pic->height) height = (int)pic->height - sy;
    if (dx + width > g.screen_width) width = g.screen_width - dx;
    if (dy + height > g.screen_height) height = g.screen_height - dy;
    if (width <= 0 || height <= 0) return;

    /* Keep BB pages as CPU-side RGB565 surfaces.  Native compatible contexts
     * do not preserve the legacy VM's page semantics reliably when hundreds
     * of small transparent resources are composed.  In-memory color-keying
     * is deterministic, and the visible result can be submitted in one bulk
     * picture call. */
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) {
            u16 pixel = pic->pixels[(sy + y) * pic->width + sx + x];
            if (pixel != BDA_GUI_COLOR_KEY_MAGENTA_RGB565)
                surface[(dy + y) * g.screen_width + dx + x] = pixel;
        }
    if (page < 0) render_visible_region(dx, dy, width, height);
}

static int file_index(u32 handle) { return handle < FILE_COUNT ? (int)handle : -1; }

static void close_file(u32 handle) {
    int index = file_index(handle);
    if (index >= 0 && bda_fs_file_is_valid(g.files[index])) {
        (void)bda_fs_close_raw(g.files[index]); g.files[index] = 0;
    }
}

static void open_file(const char *path, u32 handle) {
    int index = file_index(handle), file;
    if (index < 0) return;
    close_file(handle);
    file = bda_fs_fopen_raw(path, "rb+");
    if (!bda_fs_file_is_valid(file)) file = bda_fs_fopen_raw(path, "wb+");
    g.files[index] = bda_fs_file_is_valid(file) ? file : 0;
}

static int seek_relative(int file, u32 encoded) {
    s32 delta = (s32)(encoded - 0x7fffffffu);
    return bda_fs_seek_raw(file, delta, BDA_SEEK_CUR) >= 0;
}

static u32 file_size(int file) {
    int position, end;
    position = bda_fs_tell_raw(file);
    end = bda_fs_seek_raw(file, 0, BDA_SEEK_END);
    (void)bda_fs_seek_raw(file, position, BDA_SEEK_SET);
    return end < 0 ? 0u : (u32)end;
}

static void draw_label(int x, int y, const char *text, u32 color) {
    u32 red, green, blue, converted;
    if (g.draw == 0) return;
    color_parts(color, &red, &green, &blue);
    converted = (u32)bda_gui_rgb(g.draw, red, green, blue);
    select_object(g.draw);
    (void)bda_gui_set_text_mode(g.draw, 1u);
    (void)bda_gui_set_text_color(g.draw, converted);
    (void)bda_gui_draw_text(g.draw, x, y, text, -1);
}

static void render_input_keyboard(const char *buffer, int lowercase) {
    static const char upper[4][11] = {
        "1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM.-+"
    };
    static const char lower[4][11] = {
        "1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-+"
    };
    const char (*keys)[11] = lowercase ? lower : upper;
    int row, column;
    char label[2];
    fill_rect(-1, 0, 0, g.screen_width, 32, 0x00202020u);
    fill_rect(-1, 0, 140, g.screen_width, 180, 0x00202020u);
    (void)bda_gui_draw_guard_begin();
    draw_label(2, 4, "INPUT:", 0x0000ffffu);
    draw_label(44, 4, buffer, 0x00ffffffu);
    label[1] = '\0';
    for (row = 0; row < 4; ++row) {
        for (column = 0; column < 10; ++column) {
            label[0] = keys[row][column];
            draw_label(column * 24 + 8, 145 + row * 30, label, 0x00ffffffu);
        }
    }
    draw_label(8, 293, "BACK", 0x0000ffffu);
    draw_label(67, 293, "SPACE", 0x0000ffffu);
    draw_label(130, 293, lowercase ? "ABC" : "abc", 0x0000ffffu);
    draw_label(200, 293, "OK", 0x0000ffffu);
    (void)bda_gui_draw_guard_end();
}

static void read_line_editor(char *buffer, u32 capacity) {
    static const char upper[4][11] = {
        "1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM.-+"
    };
    static const char lower[4][11] = {
        "1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-+"
    };
    u32 length = 0, key;
    int lowercase = 0;
    buffer[0] = '\0';
    render_input_keyboard(buffer, lowercase);
    for (;;) {
        key = wait_key();
        if (key == 13u || g.detached) break;
        if ((key & 0x80000000u) != 0u) {
            int x = (s16)(key & 0xffffu);
            int y = (s16)((key >> 16) & 0x7fffu);
            if (y >= 140 && y < 260 && x >= 0 && x < 240) {
                int row = (y - 140) / 30;
                int column = x / 24;
                if (length + 1u < capacity) {
                    buffer[length++] = lowercase ? lower[row][column]
                                                 : upper[row][column];
                    buffer[length] = '\0';
                }
            } else if (y >= 280 && y < 320) {
                if (x < 60 && length != 0u) buffer[--length] = '\0';
                else if (x < 120 && length + 1u < capacity) {
                    buffer[length++] = ' '; buffer[length] = '\0';
                } else if (x < 180) lowercase = !lowercase;
                else break;
            }
            render_input_keyboard(buffer, lowercase);
        } else if (key == 37u && length != 0u) buffer[--length] = '\0';
        else if ((key == 38u || key == 40u) && length + 1u < capacity) {
            char c = length == 0u ? '0' : buffer[length - 1u];
            if (c < '0' || c > '9') c = '0';
            if (length == 0u || (length != 0u && buffer[length - 1u] != c))
                buffer[length++] = c;
            if (key == 38u) buffer[length - 1u] = c == '9' ? '0' : c + 1;
            else buffer[length - 1u] = c == '0' ? '9' : c - 1;
            buffer[length] = '\0';
        } else if (key == 39u && length + 1u < capacity) {
            buffer[length++] = '0'; buffer[length] = '\0';
        } else if (key == 27u) { length = 0; buffer[0] = '\0'; }
        render_input_keyboard(buffer, lowercase);
    }
    display_text(buffer, 1);
}

void *bbvm_bda_alloc(void *opaque, bb_u32 size) {
    (void)opaque; return bda_alloc(size);
}

void bbvm_bda_free(void *opaque, void *pointer) {
    (void)opaque; bda_free(pointer);
}

bb_u32 bbvm_bda_tick(void *opaque) {
    (void)opaque;
    return g.timer_started ? bda_gui_millisecond_count()
                           : bda_gui_tick_count_25ms() * 25u;
}

int bbvm_bda_out(void *opaque, bbvm_t *vm, bb_u32 port, bb_u32 argument) {
    char text[1024];
    u32 address, value, page, type, handle;
    int file, index;
    (void)opaque;
    switch (port) {
        case 0: int_text((s32)argument, text); display_text(text, 1); break;
        case 1: display_text(bbvm_get_string(vm, argument), 1); break;
        case 2: display_text(bbvm_get_string(vm, argument), 0); break;
        case 3: int_text((s32)argument, text); display_text(text, 0); break;
        case 4: text[0] = (char)argument; text[1] = '\0'; display_text(text, 0); break;
        case 5: float_text(argument, text); display_text(text, 0); break;
        case 10:
            read_line_editor(text, sizeof(text));
            vm->reg[BB_R3] = (u32)(s32)0;
            { char *p = text; int sign = 1; s32 number = 0;
              if (*p == '-') { sign = -1; ++p; }
              while (*p >= '0' && *p <= '9') number = number * 10 + (*p++ - '0');
              vm->reg[BB_R3] = (u32)(number * sign); }
            break;
        case 11: read_line_editor(text, sizeof(text));
                 (void)bbvm_set_string(vm, vm->reg[BB_R3], text); break;
        case 12: read_line_editor(text, sizeof(text));
                 { bb_u32 temporary = bbvm_new_string(vm);
                   union { u32 u; float f; } c;
                   (void)bbvm_set_string(vm, temporary, text);
                   vm->reg[BB_R3] = temporary;
                   c.f = 0.0f;
                   /* Reuse the VM's string-to-float IN port. */
                   c.u = bbvm_in_port(vm, 11u);
                   vm->reg[BB_R3] = c.u; bbvm_free_string(vm, temporary); }
                 break;
        case 13: vm->reg[BB_R3] = read32(vm, vm->data_ptr); vm->data_ptr += 4u; break;
        case 14:
            (void)bbvm_set_string(vm, vm->reg[BB_R3],
                                  bbvm_get_string(vm, vm->data_ptr));
            { const char *p = bbvm_get_string(vm, vm->data_ptr);
              while (*p++ != '\0') ++vm->data_ptr;
              ++vm->data_ptr; }
            break;
        case 15: vm->reg[BB_R3] = read32(vm, vm->data_ptr); vm->data_ptr += 4u; break;
        case 16:
            if (vm->reg[BB_R2] > 0u && vm->reg[BB_R2] <= SCREEN_MAX_WIDTH)
                g.screen_width = (int)vm->reg[BB_R2];
            if (vm->reg[BB_R3] > 0u && vm->reg[BB_R3] <= SCREEN_MAX_HEIGHT)
                g.screen_height = (int)vm->reg[BB_R3];
            break;
        case 17: vm->reg[BB_R3] = (u32)new_page(); break;
        case 18:
            index = (int)vm->reg[BB_R3];
            if (index >= 0 && index < PAGE_COUNT && g.pages[index] != 0) {
                bda_gui_compatible_context_free(g.pages[index]); g.pages[index] = 0;
                if (g.page_pixels[index] != 0) {
                    bda_free(g.page_pixels[index]); g.page_pixels[index] = 0;
                }
            }
            break;
        case 19: vm->reg[BB_R3] = (u32)load_picture(
                     bbvm_get_string(vm, vm->reg[BB_R3]), vm->reg[BB_R2]); break;
        case 20:
            address = vm->reg[BB_R3];
            draw_picture((int)read32(vm, address + 32u), (int)read32(vm, address + 28u),
                         (int)read32(vm, address + 24u), (int)read32(vm, address + 20u),
                         (int)read32(vm, address + 16u), (int)read32(vm, address + 12u),
                         (int)read32(vm, address + 8u), (int)read32(vm, address + 4u),
                         (int)read32(vm, address)); break;
        case 21: copy_context((int)vm->reg[BB_R3], -1, 0, 0,
                              g.screen_width, g.screen_height, 0, 0); break;
        case 22: copy_context((int)vm->reg[BB_R3], (int)vm->reg[BB_R2], 0, 0,
                              g.screen_width, g.screen_height, 0, 0); break;
        case 23:
            address = vm->reg[BB_R3];
            fill_rect((int)read32(vm, address + 20u), (int)read32(vm, address + 16u),
                      (int)read32(vm, address + 12u), (int)read32(vm, address + 8u),
                      (int)read32(vm, address + 4u), read32(vm, address)); break;
        case 24:
            address = vm->reg[BB_R3]; put_pixel((int)read32(vm, address + 12u),
                (int)read32(vm, address + 8u), (int)read32(vm, address + 4u),
                read32(vm, address)); break;
        case 25:
            address = vm->reg[BB_R3]; vm->reg[BB_R3] = get_pixel(
                (int)read32(vm, address + 8u), (int)read32(vm, address + 4u),
                (int)read32(vm, address)); break;
        case 26: free_picture((int)vm->reg[BB_R3]); break;
        case 27: {
            u32 start = bbvm_bda_tick(0), duration = vm->reg[BB_R3];
            while (bbvm_bda_tick(0) - start < duration && !g.detached) {
                pump_once(); bda_sys_delay(1u);
            } break;
        }
        case 32: vm->random_state = vm->reg[BB_R3] | 1u; break;
        case 33:
            vm->random_state = vm->random_state * 1664525u + 1013904223u;
            vm->reg[BB_R3] = vm->reg[BB_R3] == 0u ? 0u :
                             vm->random_state % vm->reg[BB_R3]; break;
        case 34: vm->reg[BB_R3] = bb_key_pressed(vm->reg[BB_R3]) ? 1u : 0u; break;
        case 35: fill_rect(-1, 0, 0, g.screen_width, g.screen_height, 0u);
                 g.cursor_x = g.cursor_y = 0; break;
        case 36: g.cursor_x = ((int)vm->reg[BB_R3] - 1) * g.font_width;
                 g.cursor_y = ((int)vm->reg[BB_R2] - 1) * g.font_height; break;
        case 37: address = vm->reg[BB_R3]; g.foreground = read32(vm, address + 8u);
                 g.background = read32(vm, address + 4u); break;
        case 38: {
            /* FONT ids are grouped by size, with the low two bits selecting
             * the face: 0..2 = 12 px, 3..5 = 16 px, 6..8 = 24 px.  Treating
             * id % 3 as the size makes LOCATE use double-height rows for
             * FONT 2; Rich then overprints its menu and eventually wraps far
             * enough to clear the playfield. */
            static const u8 widths[9] = {6, 6, 6, 8, 8, 8, 12, 12, 12};
            static const u8 heights[9] = {12, 12, 12, 16, 16, 16, 24, 24, 24};
            u32 id = vm->reg[BB_R3]; if (id > 8u) id = 0u;
            g.font_width = widths[id]; g.font_height = heights[id]; break;
        }
        case 39: vm->reg[BB_R3] = wait_key(); break;
        case 40: handle = vm->reg[BB_R3]; vm->reg[BB_R3] =
                 handle < PICTURE_COUNT ? g.pictures[handle].width : 0u; break;
        case 41: handle = vm->reg[BB_R3]; vm->reg[BB_R3] =
                 handle < PICTURE_COUNT ? g.pictures[handle].height : 0u; break;
        case 42: g.cursor_x = (int)vm->reg[BB_R2]; g.cursor_y = (int)vm->reg[BB_R3]; break;
        case 43: address = vm->reg[BB_R3]; copy_context(
                 (int)read32(vm, address), (int)read32(vm, address + 4u), 0, 0,
                 g.screen_width, g.screen_height, (int)read32(vm, address + 12u),
                 (int)read32(vm, address + 8u)); break;
        case 44: g.text_transparent = vm->reg[BB_R3] == 1u; break;
        case 45: value = wait_key(); text[0] = (char)value; text[1] = '\0';
                 (void)bbvm_set_string(vm, vm->reg[BB_R3], text); break;
        case 46: pump_once(); vm->reg[BB_R3] = dequeue_value(); break;
        case 48: open_file(bbvm_get_string(vm, vm->reg[BB_R3]), vm->reg[BB_R1]); break;
        case 49: close_file(argument); break;
        case 50:
            type = argument; index = file_index(vm->reg[BB_R1]);
            if (index < 0 || !bda_fs_file_is_valid(g.files[index])) break;
            file = g.files[index]; (void)seek_relative(file, vm->reg[BB_R2]);
            if (type == 16u || type == 17u) {
                value = 0; (void)bda_fs_read_raw(file, &value, 4u); vm->reg[BB_R3] = value;
            } else if (type == 18u) {
                u32 n = 0; char c = 0;
                do { if (bda_fs_read_raw(file, &c, 1u) != 1) c = 0;
                     if (n + 1u < sizeof(text)) text[n++] = c; }
                while (c != '\0' && n + 1u < sizeof(text));
                text[n] = '\0'; (void)bbvm_set_string(vm, vm->reg[BB_R3], text);
            } break;
        case 51:
            type = argument; index = file_index(vm->reg[BB_R1]);
            if (index < 0 || !bda_fs_file_is_valid(g.files[index])) break;
            file = g.files[index]; (void)seek_relative(file, vm->reg[BB_R2]);
            if (type == 16u || type == 17u)
                (void)bda_fs_write_raw(file, &vm->reg[BB_R3], 4u);
            else if (type == 18u) {
                const char *p = bbvm_get_string(vm, vm->reg[BB_R3]); u32 n = 0;
                while (p[n++] != '\0') {}
                (void)bda_fs_write_raw(file, p, n);
            } break;
        case 52:
            index = file_index(vm->reg[BB_R3]);
            vm->reg[BB_R3] = index < 0 || !bda_fs_file_is_valid(g.files[index]) ||
                (u32)bda_fs_tell_raw(g.files[index]) >= file_size(g.files[index]); break;
        case 53: index = file_index(vm->reg[BB_R3]); vm->reg[BB_R3] =
                 index >= 0 && bda_fs_file_is_valid(g.files[index]) ?
                 file_size(g.files[index]) : 0u; break;
        case 54: index = file_index(vm->reg[BB_R3]); vm->reg[BB_R3] =
                 index >= 0 && bda_fs_file_is_valid(g.files[index]) ?
                 (u32)bda_fs_tell_raw(g.files[index]) : 0u; break;
        case 55:
            index = file_index(vm->reg[BB_R2]);
            if (index >= 0 && bda_fs_file_is_valid(g.files[index])) {
                (void)bda_fs_seek_raw(g.files[index], (s32)vm->reg[BB_R3], BDA_SEEK_SET);
            }
            break;
        case 64:
            address = vm->reg[BB_R3]; page = read32(vm, address + 12u);
            index = context_index((int)page);
            if (index >= 0 && index <= PAGE_COUNT)
                g.pen_color[index] = read32(vm, address);
            break;
        case 65: break;
        case 66:
            index = context_index((int)vm->reg[BB_R1]); if (index >= 0 && index <= PAGE_COUNT) {
                g.pen_x[index] = (int)vm->reg[BB_R2]; g.pen_y[index] = (int)vm->reg[BB_R3];
            } break;
        case 67: {
            int target = (int)vm->reg[BB_R1];
            index = context_index(target);
            if (page_surface(target) != 0 && index >= 0 && index <= PAGE_COUNT) {
                line_surface(target, g.pen_x[index], g.pen_y[index],
                             (int)vm->reg[BB_R2], (int)vm->reg[BB_R3],
                             g.pen_color[index]);
                g.pen_x[index] = (int)vm->reg[BB_R2];
                g.pen_y[index] = (int)vm->reg[BB_R3];
            } break;
        }
        case 68: {
            int target; address = vm->reg[BB_R3];
            target = (int)read32(vm, address + 16u);
            index = context_index(target); if (page_surface(target) != 0 &&
                index >= 0 && index <= PAGE_COUNT) {
                int left = (int)read32(vm, address + 12u);
                int top = (int)read32(vm, address + 8u);
                int right = (int)read32(vm, address + 4u);
                int bottom = (int)read32(vm, address);
                line_surface(target, left, top, right, top, g.pen_color[index]);
                line_surface(target, right, top, right, bottom, g.pen_color[index]);
                line_surface(target, right, bottom, left, bottom, g.pen_color[index]);
                line_surface(target, left, bottom, left, top, g.pen_color[index]);
            } break;
        }
        case 69: {
            int target; address = vm->reg[BB_R3];
            target = (int)read32(vm, address + 12u);
            index = context_index(target); if (page_surface(target) != 0 &&
                index >= 0 && index <= PAGE_COUNT) {
                circle_surface(target, (int)read32(vm, address + 8u),
                    (int)read32(vm, address + 4u), (int)read32(vm, address),
                    g.pen_color[index]);
            } break;
        }
        case 80: address = vm->reg[BB_R3]; copy_context(
                 (int)read32(vm, address), (int)read32(vm, address + 4u),
                 (int)read32(vm, address + 12u), (int)read32(vm, address + 8u),
                 (int)read32(vm, address + 20u), (int)read32(vm, address + 16u),
                 (int)read32(vm, address + 28u), (int)read32(vm, address + 24u)); break;
        case 255: break;
        default: return 0;
    }
    return vm->error == BBVM_OK && !g.detached;
}

int bbvm_bda_start(bbvm_t *vm) {
    bda_frame_desc_t descriptor;
    int i;
    zero_bytes(&g, sizeof(g));
    g.vm = vm; g.screen_width = SCREEN_MAX_WIDTH; g.screen_height = SCREEN_MAX_HEIGHT;
    g.font_width = 6; g.font_height = 12; g.foreground = 0x00ffffffu;
    g.background = 0; g.text_transparent = 1;
    for (i = 0; i <= PAGE_COUNT; ++i) g.pen_color[i] = 0x00ffffffu;
    zero_bytes(&descriptor, sizeof(descriptor));
    descriptor.style = 0x08000000u;
    descriptor.title = "BBVM";
    descriptor.wndproc = backend_window_proc;
    descriptor.height = SCREEN_MAX_WIDTH;
    descriptor.width = SCREEN_MAX_HEIGHT;
    descriptor.surface = (u32)bda_gui_draw_object_create(15u);
    g.frame = bda_gui_register_frame_desc(&descriptor);
    if (g.frame == 0 || (s32)(u32)g.frame == -1) return 0;
    (void)bda_gui_frame_activate(g.frame, 0x100u);
    g.draw = bda_gui_current_draw(g.frame); g.draw_owner = g.frame;
    g.draw_object = bda_gui_draw_object_create(7u);
    if (g.draw == 0 || g.draw_object == 0 || (s32)(u32)g.draw_object == -1) return 0;
    g.visible_pixels = (u16 *)bda_alloc(
        (u32)SCREEN_MAX_WIDTH * (u32)SCREEN_MAX_HEIGHT * 2u);
    g.blit_pixels = (u16 *)bda_alloc(
        (u32)SCREEN_MAX_WIDTH * (u32)SCREEN_MAX_HEIGHT * 2u);
    if (g.visible_pixels == 0 || g.blit_pixels == 0) {
        if (g.visible_pixels != 0) bda_free(g.visible_pixels);
        if (g.blit_pixels != 0) bda_free(g.blit_pixels);
        g.visible_pixels = g.blit_pixels = 0;
        return 0;
    }
    select_object(g.draw);
    bda_gui_millisecond_timer_start(); g.timer_started = 1;
    fill_rect(-1, 0, 0, g.screen_width, g.screen_height, 0u);
    return 1;
}

void bbvm_bda_pump(bbvm_t *vm) {
    pump_once();
    if (g.detached) vm->running = 0;
}

void bbvm_bda_stop(void) {
    int i;
    if (g.timer_started) { bda_gui_millisecond_timer_stop(); g.timer_started = 0; }
    for (i = 0; i < FILE_COUNT; ++i) close_file((u32)i);
    for (i = 0; i < PICTURE_COUNT; ++i) free_picture(i);
    for (i = 0; i < PAGE_COUNT; ++i) {
        if (g.pages[i] != 0) {
            bda_gui_compatible_context_free(g.pages[i]); g.pages[i] = 0;
        }
        if (g.page_pixels[i] != 0) {
            bda_free(g.page_pixels[i]); g.page_pixels[i] = 0;
        }
    }
    if (g.blit_pixels != 0) { bda_free(g.blit_pixels); g.blit_pixels = 0; }
    if (g.visible_pixels != 0) {
        bda_free(g.visible_pixels); g.visible_pixels = 0;
    }
    if (g.frame != 0) {
        (void)bda_gui_frame_stop(g.frame);
        (void)bda_gui_frame_release(g.frame);
        for (i = 0; i < 64 && !g.detached; ++i) { pump_once(); bda_sys_delay(1u); }
    }
    if (g.draw != 0) { bda_gui_end_draw(g.draw); g.draw = 0; }
    if (g.frame != 0) bda_gui_close_frame(g.frame);
    g.frame = 0;
}
