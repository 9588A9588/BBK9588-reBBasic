#include "bda_sdk.h"
#include "bbvm_bda.h"
#include "bbvm_lib.h"
#include "bbvm_pixels.h"
#include "bbvm_audio.h"
#include "bbvm_fx.h"

#define SCREEN_MAX_WIDTH 240
#define SCREEN_MAX_HEIGHT 320
#define PAGE_COUNT 9
#define PICTURE_COUNT 64
#define FILE_COUNT 16
#define KEY_QUEUE_SIZE 32
#define GUI_GET_PIXEL_RGB 0x374u
#define RESOURCE_CACHE_COUNT 4u
#define RESOURCE_WINDOW_COUNT 48u
#define RESOURCE_CACHE_BYTES 2048u
#define BBVM_IDLE_MESSAGE 0x7f10u

typedef struct resource_cache {
    char path[512];
    int file;
    u32 size, age;
} resource_cache_t;

typedef struct resource_window {
    resource_cache_t *owner;
    u32 offset, valid, age;
    u8 *bytes;
} resource_window_t;

typedef int (*get_pixel_rgb_fn)(u32, u32, u32, u32 *, u32 *, u32 *);

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
    get_pixel_rgb_fn get_pixel_rgb;
    resource_cache_t resources[RESOURCE_CACHE_COUNT];
    u32 resource_age;
    resource_window_t windows[RESOURCE_WINDOW_COUNT];
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
    zero_bytes(&packet, sizeof(packet));
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
    if (message == BBVM_IDLE_MESSAGE) return 0;
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

/* The stock GUI+0x30 is GetMessage, NOT a nonblocking poll. On 9688 its
 * queue+0x0c callback enters a blocking RTOS input receive, regardless of the
 * nominal wait argument. Calling it every 512 bytecodes stalls game progress
 * until a touch/key/system event arrives.
 *
 * Keep the actual window/paint/lifecycle dispatcher, but scope a nowait idle
 * callback to this application's own frame queue. It posts one private wake
 * only if GetMessage actually reaches its empty-queue branch (after paints).
 * Restore the firmware callback BEFORE dispatching a message, so nested GUI
 * dialogs and the system launcher retain their normal blocking behavior.
 * Input uses the same bounded, nonblocking raw API as factory GAMEBOY.
 * Layout verified in kj409688: window tag=1, root type=17, root+0xf0=queue,
 * queue+0x0c=idle callback. No firmware code/global driver table is patched.
 */
static int idle_nowait(void *queue, int wait) {
    (void)queue; (void)wait;
    return bda_sdk_internal_call4(bda_sdk_internal_gui(), 0x03cu,
        (u32)g.frame, BBVM_IDLE_MESSAGE, 0u, 0u);
}

static volatile u32 *frame_idle_slot(void) {
    u8 *frame = (u8 *)g.frame;
    u32 root, queue;
    if (frame == 0 || (u32)frame == 0xffffffffu || *(u16 *)frame != 1u)
        return 0;
    root = *(u16 *)(frame + 2u) == 17u ? (u32)frame : *(u32 *)(frame + 0xccu);
    if (root == 0u || (root & 3u) != 0u) return 0;
    queue = *(u32 *)(root + 0xf0u);
    if (queue == 0u || (queue & 3u) != 0u) return 0;
    return (volatile u32 *)(queue + 0x0cu);
}

static void poll_raw_input(void) {
    bda_gui_raw_event_t event;
    u32 i;
    for (i=0u; i<4u; ++i) {
        u16 x=0u, y=0u;
        if (bda_gui_raw_event_fetch(&event) < 0) break;
        if (event.code == BDA_INPUT_EVENT_TOUCH_DOWN ||
            event.code == BDA_INPUT_EVENT_TOUCH_MOVE) {
            bda_gui_touch_position(&x, &y);
            if (x<SCREEN_MAX_WIDTH && y<SCREEN_MAX_HEIGHT)
                queue_value(0x80000000u | (u32)x | ((u32)y << 16));
        }
        /* Physical held state is sampled below. Raw key values use a
         * different namespace; never reinterpret them as BBASIC keycodes. */
    }
}

static void pump_once(void) {
    bda_gui_message_t message;
    volatile u32 *slot;
    u32 saved;
    bbvm_bda_audio_poll();
    poll_raw_input();
    zero_bytes(&message, sizeof(message));
    slot = frame_idle_slot();
    if (slot != 0) {
        int present;
        saved=*slot;
        *slot=(u32)idle_nowait;
        present=bda_sdk_internal_call2(bda_sdk_internal_gui(),
            BDA_SDK_INTERNAL_GUI_EVENT_POLL, (u32)&message, (u32)g.frame);
        *slot=saved;
        if (present>0) {
            (void)bda_sdk_internal_call1(bda_sdk_internal_gui(),
                BDA_SDK_INTERNAL_GUI_EVENT_STEP, (u32)&message);
            (void)bda_sdk_internal_call1(bda_sdk_internal_gui(),
                BDA_SDK_INTERNAL_GUI_EVENT_DISPATCH, (u32)&message);
        }
        if (message.message==0x140u) g.detached=1;
    } else if (g.frame != 0) {
        /* Reject an unrecognized queue layout; do not silently call the
         * blocking pump and recreate the input-dependent game clock. */
        g.detached=1;
    }
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
        for (row = 0; row < height; ++row) {
            u16 *dst = source + row * width;
            const u16 *src = g.visible_pixels + (y + row) * g.screen_width + x;
            for (column = 0; column < width; ++column) *dst++ = *src++;
        }
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
    if (context == 0) return 0;
    (void)g.get_pixel_rgb((u32)context, (u32)x, (u32)y, &red, &green, &blue);
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
    if (src == 0 || dst == 0 ||
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

    bbvm_blit_pixels(dst, src, g.screen_width, sx, sy, width, height,
                     dx, dy, BDA_GUI_COLOR_KEY_MAGENTA_RGB565);
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
        int width = 0;
        n = 0;
        while (text[n] != '\0' && text[n] != '\n') {
            u32 bytes = (u8)text[n] >= 0x80u && text[n+1u] != '\0' ? 2u : 1u;
            int advance = g.font_width * (int)bytes;
            if (n + bytes >= sizeof(segment)) break;
            if (g.cursor_x + width + advance > g.screen_width) {
                if (n != 0u) break;
                newline();
            }
            segment[n] = text[n];
            if (bytes == 2u) segment[n+1u] = text[n+1u];
            n += bytes;
            width += advance;
        }
        segment[n] = '\0';
        if (n != 0u && g.draw != 0) {
            int text_x, text_y, row, column;
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
        /* The next iteration wraps before a complete glyph; never split a
         * GBK pair at the old 127-byte boundary or discard a long line. */
    }
    if (add_newline) newline();
}

static int new_page(void) {
    int i;
    for (i = 0; i < PAGE_COUNT; ++i) {
        if (g.page_pixels[i] == 0) {
            g.page_pixels[i] = (u16 *)bda_alloc(
                (u32)SCREEN_MAX_WIDTH * (u32)SCREEN_MAX_HEIGHT * 2u);
            if (g.page_pixels[i] == 0) {
                return -1;
            }
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

#include "bbvm_picture_fx.h"
#include "bbvm_map_tiles.h"
#include "bbvm_screen_wave.h"

/* A separate packet port leaves all historical SHOWPIC mode values unchanged.
 * Packet coordinates are native; offline assets have already been rotated. */
/* A separate packet port leaves all historical SHOWPIC mode values unchanged.
 * Packet coordinates are native; offline assets have already been rotated. */
static s32 render_fx_packet(bbvm_t *vm, const s32 *a, int present) {
    s32 x0, y0, x1, y1, x, y, src_y;
    (void)vm;
    u32 i, r, green, b, sat, alpha, phase, delta, step_remainder, error, start_x;
    u16 *surface;
    picture_t *pic = 0;
    if ((a[0] < 1 || a[0] > 7) || a[1] < -1 || a[1] >= PAGE_COUNT)
        return -2;
    surface=page_surface(a[1]);
    if (surface == 0) return -2;
    for (i=3u;i<=6u;++i) if (a[i] < -65535 || a[i] > 65535) return -3;
    for (i=17u;i<=20u;++i) if (a[i] < -65535 || a[i] > 65535) return -3;
    if (a[5] < 1 || a[6] < 1 || a[19] < 1 || a[20] < 1 ||
        a[11] < 0 || a[11] > 256 || a[12] < 0 || a[12] > 1) return -3;
    for (i=13u;i<=16u;++i) {
        s32 maximum = (a[0] == 4 || a[0] == 5) && i <= 15u ? 31 : 200;
        if (a[i] < 0 || a[i] > maximum) return -3;
    }
    if(a[0]==6 || a[0]==7) {
        s32 result;
        if(a[2]<0 || a[2]>=PICTURE_COUNT || g.pictures[a[2]].pixels==0)return -4;
        pic=&g.pictures[a[2]];
        result=bbvm_pic_transform(surface,g.screen_width,g.screen_height,
            pic->pixels,(s32)pic->width,(s32)pic->height,a);
        if(result==0 && a[1]<0)render_visible_region(0,0,g.screen_width,g.screen_height);
        return result;
    }
    if (a[0] == 1 || a[0] == 4) {
        if (a[2] < 0 || a[2] >= PICTURE_COUNT || g.pictures[a[2]].pixels == 0)
            return -4;
        pic=&g.pictures[a[2]];
        if (a[7] < 0 || a[8] < 0 || a[9] < 1 || a[9] > 65535 || a[10] < 1 || a[10] > 65535 ||
            (u32)a[7] > pic->width || (u32)a[9] > pic->width-(u32)a[7] ||
            (u32)a[8] > pic->height || (u32)a[10] > pic->height-(u32)a[8]) return -3;
    }
    x0=a[3] > a[17] ? a[3] : a[17]; if (x0<0) x0=0;
    y0=a[4] > a[18] ? a[4] : a[18]; if (y0<0) y0=0;
    x1=a[3]+a[5]; if (x1>a[17]+a[19]) x1=a[17]+a[19];
    y1=a[4]+a[6]; if (y1>a[18]+a[20]) y1=a[18]+a[20];
    if (x1>g.screen_width) x1=g.screen_width;
    if (y1>g.screen_height) y1=g.screen_height;
    if (a[0]==3) {
        const u16 *source;
        if (a[2]<-1 || a[2]>=PAGE_COUNT || (source=page_surface(a[2]))==0)
            return -4;
        if (source==surface || a[7]<0 || a[8]<0 || a[9]!=a[5] || a[10]!=a[6] ||
            a[7]>g.screen_width || a[5]>g.screen_width-a[7] ||
            a[8]>g.screen_height || a[6]>g.screen_height-a[8] ||
            a[11]!=256 || a[12]!=0 || a[13]!=100 || a[14]!=100 || a[15]!=100 || a[16]!=100)
            return -3;
        if (x1<=x0 || y1<=y0) return 0;
        /* Explicit opaque presentation, unlike legacy color-keyed FLIPPAGE.
         * No allocation, scaling, blending, or firmware per-pixel calls. */
        for (y=y0;y<y1;++y) {
            u16 *d=surface+y*g.screen_width+x0;
            const u16 *s=source+(a[8]+y-a[4])*g.screen_width+a[7]+x0-a[3];
            for (x=x0;x+4<=x1;x+=4) {
                d[0]=s[0];d[1]=s[1];d[2]=s[2];d[3]=s[3];d+=4;s+=4;
            }
            for (;x<x1;++x) *d++=*s++;
        }
        if (a[1]<0 && present) render_visible_region(x0,y0,x1-x0,y1-y0);
        return 0;
    }
    if (x1<=x0 || y1<=y0 || ((a[0]==1 || a[0]>=4) && a[11]==0)) return 0;
    /* Common actor/glyph draw: already rotated, 1:1, opaque, neutral tone.
     * Avoid per-row divisions and a tone-function call per sprite pixel. */
    if (a[0]==1 && a[5]==a[9] && a[6]==a[10] && a[11]==256 &&
        a[13]==100 && a[14]==100 && a[15]==100 && a[16]==100) {
        for (y=y0;y<y1;++y) {
            u16 *d=surface+y*g.screen_width+x0;
            const u16 *s=pic->pixels+(a[8]+y-a[4])*pic->width+a[7]+x0-a[3];
            for (x=x0;x<x1;++x) {
                u16 pixel=*s++;
                if (!a[12] || pixel!=BDA_GUI_COLOR_KEY_MAGENTA_RGB565) *d=pixel;
                ++d;
            }
        }
        if (a[1]<0 && present) render_visible_region(x0,y0,x1-x0,y1-y0);
        return 0;
    }
    if (a[0]==2 && a[13]==100 && a[14]==100 && a[15]==100 && a[16]==100)
        return 0;
    /* Flash overlays have a constant source color. Hoist its RGB products
     * out of the pixel loop and bypass all scaling for the common 1:1 mask.
     * Each channel retains the original +128 then >>8 rounding. */
    if(a[0]==5 || (a[0]==4 && a[5]==a[9] && a[6]==a[10])) {
        u16 flash=(u16)(((u32)a[13]<<11)|(((u32)a[14]*63u/31u)<<5)|(u32)a[15]);
        u32 inverse=256u-(u32)a[11];
        u32 fr=(flash>>11)*(u32)a[11]+128u;
        u32 fg=((flash>>5)&63u)*(u32)a[11]+128u;
        u32 fb=(flash&31u)*(u32)a[11]+128u;
        for(y=y0;y<y1;++y) {
            u16 *d=surface+y*g.screen_width+x0;
            const u16 *s=pic!=0?pic->pixels+(a[8]+y-a[4])*pic->width+a[7]+x0-a[3]:0;
            for(x=x0;x<x1;++x) {
                int draw=s==0 || !a[12] || *s!=BDA_GUI_COLOR_KEY_MAGENTA_RGB565;
                if(draw) {
                    if(a[11]==256)*d=flash;
                    else {
                        u32 old=*d;
                        *d=(u16)((((fr+(old>>11)*inverse)>>8)<<11)|
                            (((fg+((old>>5)&63u)*inverse)>>8)<<5)|
                            ((fb+(old&31u)*inverse)>>8));
                    }
                }
                ++d;if(s!=0)++s;
            }
        }
        if(a[1]<0)render_visible_region(x0,y0,x1-x0,y1-y0);
        return 0;
    }
    /* Fixed-tone 1:1 images and screen tone need only 32+64+32 channel
     * transforms, not three hard-light divisions for every pixel. Only
     * neutral saturation is separable; other saturation/scaling retain the
     * full compatibility path. Small rectangles avoid table setup overhead. */
    if(a[16]==100 && ((a[0]==1 && a[5]==a[9] && a[6]==a[10]) || a[0]==2) &&
       (u32)(x1-x0)*(u32)(y1-y0)>=256u) {
        u16 rt[32],gt[64],bt[32];
        u32 red=(u32)a[13]*128u/100u,gr=(u32)a[14]*128u/100u,blue=(u32)a[15]*128u/100u;
        if(red>255u)red=255u;
        if(gr>255u)gr=255u;
        if(blue>255u)blue=255u;
        for(i=0u;i<32u;++i) {
            u32 v=(i<<3)|(i>>2);
            rt[i]=(u16)((bbvm_fx_light(red,v)>>3)<<11);
            bt[i]=(u16)(bbvm_fx_light(blue,v)>>3);
        }
        for(i=0u;i<64u;++i)gt[i]=(u16)((bbvm_fx_light(gr,(i<<2)|(i>>4))>>2)<<5);
        for(y=y0;y<y1;++y) {
            u16 *d=surface+y*g.screen_width+x0;
            const u16 *s=pic!=0?pic->pixels+(a[8]+y-a[4])*pic->width+a[7]+x0-a[3]:0;
            for(x=x0;x<x1;++x) {
                u16 pixel=s!=0?*s++:*d;
                if(s==0 || !a[12] || pixel!=BDA_GUI_COLOR_KEY_MAGENTA_RGB565) {
                    pixel=(u16)(rt[pixel>>11]|gt[(pixel>>5)&63u]|bt[pixel&31u]);
                    *d=(s!=0 && a[11]!=256)?bbvm_fx_blend(pixel,*d,(u32)a[11]):pixel;
                }
                ++d;
            }
        }
        if(a[1]<0)render_visible_region(x0,y0,x1-x0,y1-y0);
        return 0;
    }
    r=(u32)a[13]*128u/100u; if(r>255u)r=255u;
    green=(u32)a[14]*128u/100u; if(green>255u)green=255u;
    b=(u32)a[15]*128u/100u; if(b>255u)b=255u;
    sat=(u32)a[16]*128u/100u; if(sat>255u)sat=255u;
    alpha=(u32)a[11];
    delta=pic!=0 ? (u32)a[9]/(u32)a[5] : 0u;
    step_remainder=pic!=0 ? (u32)a[9]%(u32)a[5] : 0u;
    start_x=pic!=0 ? (u32)(x0-a[3])*(u32)a[9] : 0u;
    for (y=y0;y<y1;++y) {
        u16 *d=surface+y*g.screen_width;
        const u16 *s=0;
        if (pic != 0) {
            src_y=a[8]+(s32)((u32)(y-a[4])*(u32)a[10]/(u32)a[6]);
            s=pic->pixels+(u32)src_y*pic->width+(u32)a[7];
        }
        phase=start_x/(u32)a[5]; error=start_x%(u32)a[5];
        for (x=x0;x<x1;++x) {
            u16 pixel=s != 0 ? s[phase] : d[x];
            phase+=delta; error+=step_remainder;
            if (error>=(u32)a[5]) { ++phase; error-=(u32)a[5]; }
            if (s != 0 && a[12] != 0 && pixel == BDA_GUI_COLOR_KEY_MAGENTA_RGB565)
                continue;
            if (a[0]>=4) {
                /* Flash is a constant RGB overlay through the source's
                 * transparency mask, not a tone or a filled sprite rectangle. */
                pixel=(u16)(((u32)a[13]<<11)|(((u32)a[14]*63u/31u)<<5)|(u32)a[15]);
            } else pixel=bbvm_fx_tone(pixel,r,green,b,sat);
            d[x]=((s != 0 || a[0]==5) && alpha != 256u) ? bbvm_fx_blend(pixel,d[x],alpha) : pixel;
        }
    }
    if (a[1]<0 && present) render_visible_region(x0,y0,x1-x0,y1-y0);
    return 0;
}


/* Operation 11: read-only view of BBASIC actor arrays, no persistent pointers.
 * [11,page,picture,entity,gx,gy,facing,phase,lib,trans,jump,progress,
 *  flash_power,flash_life,flash_left,red,green,blue,camx,camy,0].
 * Array fields are bytecode offsets, including unaligned BB2.0++ globals.
 * Only presentation arithmetic moves here; routes, damage and timing stay BASIC. */
static s32 render_fx_actor(bbvm_t *vm, const s32 *a) {
    s32 v[14], one[21], i, ph, sx, sy, height=0, rem, r, trans, power;
    u32 entity;
    if (a[3]<0 || a[3]>=129 || a[20]!=0) return -3;
    entity=(u32)a[3];
    for (i=0;i<14;++i) {
        u32 base=(u32)a[4+i];
        if (a[4+i]<0 || base>vm->memory_size ||
            129u*4u>vm->memory_size-base) return -1;
        v[i]=(s32)read32(vm,base+entity*4u);
    }
    /* Bound products before evaluating them. Valid game coordinates retain
     * the exact signed integer division order of the original BASIC code. */
    if (v[0]<-65535 || v[0]>65535 || v[1]<-65535 || v[1]>65535 ||
        a[18]<-65535 || a[18]>65535 || a[19]<-65535 || a[19]>65535 ||
        v[2]<0 || v[2]>3 || v[5]<0 || v[5]>8 ||
        v[7]<0 || v[7]>256 || v[8]<0 || v[8]>31 ||
        v[9]<0 || v[9]>2147483 || v[10]<0 || v[10]>2147483 ||
        v[11]<0 || v[11]>31 || v[12]<0 || v[12]>31 ||
        v[13]<0 || v[13]>31) return -3;
    ph=v[3]; if(ph<0 || ph>2)ph=1;
    sx=84-v[2]*28; sy=ph*21;
    if(v[4]==0){sx=0;sy=0;}
    if(v[6]!=0) {
        rem=256-v[7];
        /* Preserve installed BB2.0++ bytecode, not its misleading source:
         * REM=256-REM emits store 256 then subtract that same slot -> 0.
         * Changing jump animation is outside this performance-only patch. */
        if(rem>128)rem=0;
        height=rem/8;
        if(height<5)height*=2;
        else if(height<13)height+=4;
        else height=16;
        height=height*7/8;
    }
    trans=8-v[5];
    for(i=0;i<21;++i)one[i]=0;
    one[0]=1;one[1]=a[1];one[2]=a[2];
    one[3]=211-v[1]+a[19]+height;one[4]=37+v[0]-a[18];
    one[5]=28;one[6]=21;one[7]=sx;one[8]=sy;
    one[9]=28;one[10]=21;one[11]=trans*32;one[12]=1;
    one[13]=100;one[14]=100;one[15]=100;one[16]=100;
    one[17]=15;one[18]=40;one[19]=210;one[20]=280;
    r=render_fx_packet(vm,one,1);if(r<0)return r;
    if(v[8]>0) {
        rem=v[8];if(v[9]>0)rem=rem*v[10]/v[9];
        power=rem*256/31;power=power*trans/8;
        one[0]=4;one[11]=power;
        one[13]=v[11];one[14]=v[12];one[15]=v[13];
        return render_fx_packet(vm,one,1);
    }
    return 0;
}


/* Operation 12: exact GPREPAREDRAW visibility and stable Y ordering.
 * [12,order(387),counts(3),entities,layer,lib,pageid,char,trans,x,y,
 *  hidden,camx,camy,tilebase,0...]. No heap or cross-frame state. */
static s32 render_fx_actor_prepare(bbvm_t *vm,const s32 *a) {
    u32 ptr[10], lengths[10], i,j,k,entity,order,counts,base,count,other;
    s32 layer,lib,x,y,charid;
    int eligible;
    if(a[3]<0 || a[3]>129)return -3;
    for(i=15;i<21;++i)if(a[i]!=0)return -3;
    ptr[0]=(u32)a[1];lengths[0]=387u*4u;
    ptr[1]=(u32)a[2];lengths[1]=3u*4u;
    for(i=2;i<9;++i){ptr[i]=(u32)a[i+2];lengths[i]=129u*4u;}
    /* Seven read-only arrays: layer,lib,pageid,char,trans,x,y. */
    for(i=0;i<9;++i)
        if(ptr[i]>vm->memory_size || lengths[i]>vm->memory_size-ptr[i])return -1;
    for(i=0;i<2;++i)for(j=i+1;j<9;++j)
        if(ptr[i]<ptr[j]+lengths[j] && ptr[j]<ptr[i]+lengths[i])return -1;
    order=ptr[0];counts=ptr[1];
    for(i=0;i<3;++i)bbvm_write_u32(vm,counts+i*4u,0u);
    for(entity=0;entity<(u32)a[3];++entity) {
        layer=(s32)read32(vm,ptr[2]+entity*4u);
        if(layer<0 || layer>2)continue;
        lib=(s32)read32(vm,ptr[3]+entity*4u);
        charid=(s32)read32(vm,ptr[5]+entity*4u);
        eligible=lib>0;
        if(entity>0 && lib==0 && (s32)read32(vm,ptr[4]+entity*4u)>0 &&
           charid>0 && charid<144 && a[14]>0)eligible=1;
        if(!eligible || (s32)read32(vm,ptr[6]+entity*4u)>=8 ||
           (entity==0 && a[11]!=0))continue;
        x=(s32)(read32(vm,ptr[7]+entity*4u)-(u32)a[12]);
        y=(s32)(read32(vm,ptr[8]+entity*4u)-(u32)a[13]);
        if(x< -18 || x>=283 || y< -14 || y>=238)continue;
        count=read32(vm,counts+(u32)layer*4u);j=count;
        base=order+(u32)layer*129u*4u;
        while(j>0) {
            other=read32(vm,base+(j-1u)*4u);
            if((s32)read32(vm,ptr[8]+other*4u)<=
               (s32)read32(vm,ptr[8]+entity*4u))break;
            bbvm_write_u32(vm,base+j*4u,other);
            --j;
        }
        bbvm_write_u32(vm,base+j*4u,entity);
        k=counts+(u32)layer*4u;
        bbvm_write_u32(vm,k,count+1u);
    }
    return 0;
}


/* Pure, allocation-free hot-path helpers. No guest pointer survives the call.
 * Shared 21-word packet; reserved words must remain zero. All guest words
 * are unaligned bytecode data. Return -2 for a malformed packet. */
static int lookup_range(bbvm_t *vm,s32 base,u32 words) {
    return base>=0 && (u32)base<=vm->memory_size &&
        words<=(vm->memory_size-(u32)base)/4u;
}
static s32 lookup_hot(bbvm_t *vm,const s32 *a) {
    u32 i,slot=0u; s32 oldest=2147483647,value,flags,n,v,b,op,ok;
    u32 used=a[0]==13?4u:(a[0]==14?4u:8u);
    for(i=used;i<21u;++i)if(a[i]!=0)return -2;
    if(a[0]==13) {
        /* [13, ids(16), ages(16), wanted]. Same early hit and signed LRU. */
        if(!lookup_range(vm,a[1],16u)||!lookup_range(vm,a[2],16u))return -2;
        for(i=0u;i<16u;++i) {
            if((s32)read32(vm,(u32)a[1]+i*4u)==a[3])return (s32)i;
            value=(s32)read32(vm,(u32)a[2]+i*4u);
            if(value<oldest){oldest=value;slot=i;}
        }
        return 16+(s32)slot;
    }
    if(a[0]==14) {
        /* [14, map_event_ids(count), count, wanted] -> 1-based entity. */
        if(a[2]<0 || a[2]>128 || !lookup_range(vm,a[1],(u32)a[2]))return -2;
        for(i=0u;i<(u32)a[2];++i)
            if((s32)read32(vm,(u32)a[1]+i*4u)==a[3])return (s32)i+1;
        return -1;
    }
    /* [15, page(32), switches(8192), vars(8192), items(2048),
     *  party(256), timer1, timer2]. Exact installed EVCONDITION semantics. */
    if(a[0]!=15 || !lookup_range(vm,a[1],32u) ||
       !lookup_range(vm,a[2],8192u)||!lookup_range(vm,a[3],8192u)||
       !lookup_range(vm,a[4],2048u)||!lookup_range(vm,a[5],256u))return -2;
    flags=(s32)read32(vm,(u32)a[1]+16u*4u);
    if(flags<0 || flags>127)return -5;
    for(i=0u;i<7u;++i) {
        if(flags&1) {
            ok=1;
            if(i<2u) {
                n=(s32)read32(vm,(u32)a[1]+(17u+i)*4u);
                ok=n>=1&&n<=8191?(s32)read32(vm,(u32)a[2]+(u32)n*4u):0;
            } else if(i==2u) {
                n=(s32)read32(vm,(u32)a[1]+19u*4u);ok=0;
                if(n>=1&&n<=8191) {
                    v=(s32)read32(vm,(u32)a[3]+(u32)n*4u);
                    b=(s32)read32(vm,(u32)a[1]+20u*4u);
                    op=(s32)read32(vm,(u32)a[1]+25u*4u);
                    ok=(op==0&&v==b)||(op==1&&v>=b)||(op==2&&v<=b)||
                       (op==3&&v>b)||(op==4&&v<b)||(op==5&&v!=b);
                }
            } else if(i==3u) {
                n=(s32)read32(vm,(u32)a[1]+21u*4u);ok=0;
                if(n>=1&&n<=2047)ok=(s32)read32(vm,(u32)a[4]+(u32)n*4u)>=1;
            } else if(i==4u) {
                n=(s32)read32(vm,(u32)a[1]+22u*4u);
                ok=n>=1&&n<=255?(s32)read32(vm,(u32)a[5]+(u32)n*4u):0;
            } else if(i==5u) {
                if(a[6]>(s32)read32(vm,(u32)a[1]+23u*4u))ok=0;
            } else {
                if(a[7]>(s32)read32(vm,(u32)a[1]+24u*4u))ok=0;
            }
            if(!ok)return 0;
        }
        flags/=2;
    }
    return 1;
}

static s32 render_fx(bbvm_t *vm, u32 address) {
    s32 a[21];
    u32 i;
    /* BB2.0++ packs globals without guaranteeing four-byte alignment. */
    if (address > vm->memory_size || 84u > vm->memory_size-address) return -1;
    for (i=0u;i<21u;++i) a[i]=(s32)read32(vm,address+i*4u);
    if (a[0]>=13 && a[0]<=15) return lookup_hot(vm,a);
    if (a[0]==11) return render_fx_actor(vm,a);
    if (a[0]==12) return render_fx_actor_prepare(vm,a);
    if (a[0]==8) return bbvm_map_tiles(vm,a);
    if (a[0]==9) return bbvm_screen_wave(a);
    return render_fx_packet(vm,a,1);
}

static void clear_resource_cache(void) {
    u32 i;
    for (i=0u;i<RESOURCE_WINDOW_COUNT;++i) {
        resource_window_t *w=&g.windows[i];
        if (w->bytes!=0) bda_free(w->bytes);
        zero_bytes(w,sizeof(*w));
    }
    for (i=0u;i<RESOURCE_CACHE_COUNT;++i) {
        resource_cache_t *c=&g.resources[i];
        if (bda_fs_file_is_valid(c->file)) (void)bda_fs_close_raw(c->file);
        zero_bytes(c,sizeof(*c));
    }
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
    clear_resource_cache();
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

/* Immutable resources are not opened through legacy OPEN: its rb+ -> wb+
 * fallback can create an empty file when an asset is missing. One bounded
 * read avoids thousands of interpreted GET operations per map/event page. */
static s32 read_resource_block(bbvm_t *vm, const char *path, u32 offset,
                               u32 destination, u32 length) {
    int file, got;
    s32 end;
    u32 position;
    if (path == 0 || path[0] == '\0') return -1;
    position = 0;
    while (position < 512u && path[position] != '\0') ++position;
    if (position == 512u || length > 65536u ||
        destination > vm->memory_size ||
        length > vm->memory_size - destination || offset > 0x7fffffffu)
        return -2;
    if (length == 0u) return 0;
    /* Only immutable LIB/IDX files are cached. Save/data reads retain their
     * exact legacy IO semantics. Forty-eight 2-KiB windows cap cache heap at 96 KiB; file owners stay four. */
    if (length<=RESOURCE_CACHE_BYTES && position>=4u &&
        path[position-4u]=='.' &&
        (((path[position-3u]|32)=='l' && (path[position-2u]|32)=='i' &&
          (path[position-1u]|32)=='b') ||
         ((path[position-3u]|32)=='i' && (path[position-2u]|32)=='d' &&
          (path[position-1u]|32)=='x'))) {
        resource_cache_t *c=0;
        resource_window_t *w=0;
        u32 i, j, victim=0u, oldest=0xffffffffu;
        /* File ownership is independent of window replacement: a hot LIB
         * can occupy several windows without reopening it on each miss. */
        for (i=0u;i<RESOURCE_CACHE_COUNT;++i) {
            resource_cache_t *entry=&g.resources[i];
            for (j=0u;j<=position && entry->path[j]==path[j];++j) {}
            if (j>position) { c=entry;break; }
            if (entry->age<oldest) { oldest=entry->age;victim=i; }
        }
        if (c==0) {
            c=&g.resources[victim];
            if (bda_fs_file_is_valid(c->file)) (void)bda_fs_close_raw(c->file);
            for (i=0u;i<RESOURCE_WINDOW_COUNT;++i)
                if (g.windows[i].owner==c) {
                    g.windows[i].owner=0;g.windows[i].valid=0u;g.windows[i].age=0u;
                }
            c->file=0;c->path[0]='\0';c->age=0u;
            file=bda_fs_fopen_raw(path,"rb");
            if (!bda_fs_file_is_valid(file)) return -1;
            end=bda_fs_seek_raw(file,0,BDA_SEEK_END);
            if (end<0) { (void)bda_fs_close_raw(file);return -3; }
            c->file=file;c->size=(u32)end;
            for (j=0u;j<=position;++j) c->path[j]=path[j];
        }
        c->age=++g.resource_age;
        if (offset>c->size || length>c->size-offset) return -3;
        oldest=0xffffffffu;victim=0u;
        for (i=0u;i<RESOURCE_WINDOW_COUNT;++i) {
            resource_window_t *entry=&g.windows[i];
            if (entry->owner==c && offset>=entry->offset &&
                offset-entry->offset<=entry->valid &&
                length<=entry->valid-(offset-entry->offset)) { w=entry;break; }
            if (entry->age<oldest) { oldest=entry->age;victim=i; }
        }
        if (w==0) {
            u32 base=offset & ~(RESOURCE_CACHE_BYTES-1u);
            u32 want;
            w=&g.windows[victim];
            if (w->bytes==0) w->bytes=(u8 *)bda_alloc(RESOURCE_CACHE_BYTES);
            if (w->bytes==0) {
                /* No cache allocation: preserve exact read/error semantics
                 * using the already open, validated resource handle. */
                if (bda_fs_seek_raw(c->file,(s32)offset,BDA_SEEK_SET)<0) return -3;
                got=bda_fs_read_raw(c->file,vm->memory+destination,length);
                return got==(int)length?got:-4;
            }
            if (length>RESOURCE_CACHE_BYTES-(offset-base)) base=offset;
            want=c->size-base;if(want>RESOURCE_CACHE_BYTES)want=RESOURCE_CACHE_BYTES;
            w->owner=c;w->offset=base;w->valid=0u;w->age=++g.resource_age;
            if (bda_fs_seek_raw(c->file,(s32)base,BDA_SEEK_SET)<0) return -3;
            got=bda_fs_read_raw(c->file,w->bytes,want);
            if(got>0 && (u32)got<=want)w->valid=(u32)got;
            if(w->valid<offset-base || length>w->valid-(offset-base))return -4;
        }
        w->age=++g.resource_age;
        for(i=0u;i<length;++i)vm->memory[destination+i]=w->bytes[offset-w->offset+i];
        return (s32)length;
    }
    file = bda_fs_fopen_raw(path, "rb");
    if (!bda_fs_file_is_valid(file)) return -1;
    end = bda_fs_seek_raw(file, 0, BDA_SEEK_END);
    if (end < 0 || offset > (u32)end || length > (u32)end - offset) {
        (void)bda_fs_close_raw(file); return -3;
    }
    if (bda_fs_seek_raw(file, (s32)offset, BDA_SEEK_SET) < 0) {
        (void)bda_fs_close_raw(file); return -3;
    }
    got = bda_fs_read_raw(file, vm->memory + destination, length);
    (void)bda_fs_close_raw(file);
    return got == (int)length ? got : -4;
}

static s32 write_save_block(bbvm_t *vm, const char *path, u32 offset,
                            u32 source, u32 length) {
    int file, wrote;
    u32 n=0;
    if (path==0 || path[0]=='\0') return -1;
    while (n<512u && path[n]!='\0') ++n;
    if (n==512u || length>65536u || source>vm->memory_size ||
        length>vm->memory_size-source || offset>0x7fffffffu ||
        length>0x7fffffffu-offset) return -2;
    if (length==0u) return 0;
    /* .sav is never in the immutable LIB/IDX cache. Do not evict unrelated
     * event, map and font windows when committing a save. Other writes retain
     * conservative invalidation (including alternate LIB/IDX path spellings). */
    if (!(n>=4u && path[n-4u]=='.' && (path[n-3u]|32)=='s' &&
          (path[n-2u]|32)=='a' && (path[n-1u]|32)=='v'))
        clear_resource_cache();
    file=bda_fs_fopen_raw(path,"rb+");
    if (!bda_fs_file_is_valid(file)) file=bda_fs_fopen_raw(path,"wb+");
    if (!bda_fs_file_is_valid(file)) return -1;
    if (bda_fs_seek_raw(file,(s32)offset,BDA_SEEK_SET)<0) {
        (void)bda_fs_close_raw(file);return -3;
    }
    wrote=bda_fs_write_raw(file,vm->memory+source,length);
    (void)bda_fs_close_raw(file);
    return wrote==(int)length?wrote:-4;
}

/* Incremental IEEE CRC state (unfinalized), bytewise and unaligned-safe.
 * ROM table costs 1024 bytes, no heap; identical polynomial/initial/final state. */
static const u32 save_crc_table[256]={
    0x00000000u,0x77073096u,0xee0e612cu,0x990951bau,0x076dc419u,0x706af48fu,0xe963a535u,0x9e6495a3u,
    0x0edb8832u,0x79dcb8a4u,0xe0d5e91eu,0x97d2d988u,0x09b64c2bu,0x7eb17cbdu,0xe7b82d07u,0x90bf1d91u,
    0x1db71064u,0x6ab020f2u,0xf3b97148u,0x84be41deu,0x1adad47du,0x6ddde4ebu,0xf4d4b551u,0x83d385c7u,
    0x136c9856u,0x646ba8c0u,0xfd62f97au,0x8a65c9ecu,0x14015c4fu,0x63066cd9u,0xfa0f3d63u,0x8d080df5u,
    0x3b6e20c8u,0x4c69105eu,0xd56041e4u,0xa2677172u,0x3c03e4d1u,0x4b04d447u,0xd20d85fdu,0xa50ab56bu,
    0x35b5a8fau,0x42b2986cu,0xdbbbc9d6u,0xacbcf940u,0x32d86ce3u,0x45df5c75u,0xdcd60dcfu,0xabd13d59u,
    0x26d930acu,0x51de003au,0xc8d75180u,0xbfd06116u,0x21b4f4b5u,0x56b3c423u,0xcfba9599u,0xb8bda50fu,
    0x2802b89eu,0x5f058808u,0xc60cd9b2u,0xb10be924u,0x2f6f7c87u,0x58684c11u,0xc1611dabu,0xb6662d3du,
    0x76dc4190u,0x01db7106u,0x98d220bcu,0xefd5102au,0x71b18589u,0x06b6b51fu,0x9fbfe4a5u,0xe8b8d433u,
    0x7807c9a2u,0x0f00f934u,0x9609a88eu,0xe10e9818u,0x7f6a0dbbu,0x086d3d2du,0x91646c97u,0xe6635c01u,
    0x6b6b51f4u,0x1c6c6162u,0x856530d8u,0xf262004eu,0x6c0695edu,0x1b01a57bu,0x8208f4c1u,0xf50fc457u,
    0x65b0d9c6u,0x12b7e950u,0x8bbeb8eau,0xfcb9887cu,0x62dd1ddfu,0x15da2d49u,0x8cd37cf3u,0xfbd44c65u,
    0x4db26158u,0x3ab551ceu,0xa3bc0074u,0xd4bb30e2u,0x4adfa541u,0x3dd895d7u,0xa4d1c46du,0xd3d6f4fbu,
    0x4369e96au,0x346ed9fcu,0xad678846u,0xda60b8d0u,0x44042d73u,0x33031de5u,0xaa0a4c5fu,0xdd0d7cc9u,
    0x5005713cu,0x270241aau,0xbe0b1010u,0xc90c2086u,0x5768b525u,0x206f85b3u,0xb966d409u,0xce61e49fu,
    0x5edef90eu,0x29d9c998u,0xb0d09822u,0xc7d7a8b4u,0x59b33d17u,0x2eb40d81u,0xb7bd5c3bu,0xc0ba6cadu,
    0xedb88320u,0x9abfb3b6u,0x03b6e20cu,0x74b1d29au,0xead54739u,0x9dd277afu,0x04db2615u,0x73dc1683u,
    0xe3630b12u,0x94643b84u,0x0d6d6a3eu,0x7a6a5aa8u,0xe40ecf0bu,0x9309ff9du,0x0a00ae27u,0x7d079eb1u,
    0xf00f9344u,0x8708a3d2u,0x1e01f268u,0x6906c2feu,0xf762575du,0x806567cbu,0x196c3671u,0x6e6b06e7u,
    0xfed41b76u,0x89d32be0u,0x10da7a5au,0x67dd4accu,0xf9b9df6fu,0x8ebeeff9u,0x17b7be43u,0x60b08ed5u,
    0xd6d6a3e8u,0xa1d1937eu,0x38d8c2c4u,0x4fdff252u,0xd1bb67f1u,0xa6bc5767u,0x3fb506ddu,0x48b2364bu,
    0xd80d2bdau,0xaf0a1b4cu,0x36034af6u,0x41047a60u,0xdf60efc3u,0xa867df55u,0x316e8eefu,0x4669be79u,
    0xcb61b38cu,0xbc66831au,0x256fd2a0u,0x5268e236u,0xcc0c7795u,0xbb0b4703u,0x220216b9u,0x5505262fu,
    0xc5ba3bbeu,0xb2bd0b28u,0x2bb45a92u,0x5cb36a04u,0xc2d7ffa7u,0xb5d0cf31u,0x2cd99e8bu,0x5bdeae1du,
    0x9b64c2b0u,0xec63f226u,0x756aa39cu,0x026d930au,0x9c0906a9u,0xeb0e363fu,0x72076785u,0x05005713u,
    0x95bf4a82u,0xe2b87a14u,0x7bb12baeu,0x0cb61b38u,0x92d28e9bu,0xe5d5be0du,0x7cdcefb7u,0x0bdbdf21u,
    0x86d3d2d4u,0xf1d4e242u,0x68ddb3f8u,0x1fda836eu,0x81be16cdu,0xf6b9265bu,0x6fb077e1u,0x18b74777u,
    0x88085ae6u,0xff0f6a70u,0x66063bcau,0x11010b5cu,0x8f659effu,0xf862ae69u,0x616bffd3u,0x166ccf45u,
    0xa00ae278u,0xd70dd2eeu,0x4e048354u,0x3903b3c2u,0xa7672661u,0xd06016f7u,0x4969474du,0x3e6e77dbu,
    0xaed16a4au,0xd9d65adcu,0x40df0b66u,0x37d83bf0u,0xa9bcae53u,0xdebb9ec5u,0x47b2cf7fu,0x30b5ffe9u,
    0xbdbdf21cu,0xcabac28au,0x53b39330u,0x24b4a3a6u,0xbad03605u,0xcdd70693u,0x54de5729u,0x23d967bfu,
    0xb3667a2eu,0xc4614ab8u,0x5d681b02u,0x2a6f2b94u,0xb40bbe37u,0xc30c8ea1u,0x5a05df1bu,0x2d02ef8du,
};
static u32 save_crc(bbvm_t *vm,u32 source,u32 length,u32 state) {
    u32 i;
    if (source>vm->memory_size || length>vm->memory_size-source) return 0u;
    for (i=0;i<length;++i)
        state=(state>>8)^save_crc_table[(state^vm->memory[source+i])&255u];
    return state;
}

int bbvm_bda_out(void *opaque, bbvm_t *vm, bb_u32 port, bb_u32 argument) {
    char text[1024];
    u32 address, value, page, type, handle;
    int file, index;
    (void)opaque;
    if (port >= 96u && port <= 103u) return bbvm_bda_audio_out(vm, port);
    if (port == 108u || port == 109u) return bbvm_bda_audio_out(vm, port);
    switch (port) {
        case 113: {
            u32 src=vm->reg[BB_R3],dst=vm->reg[BB_R2],len=vm->reg[BB_R1],i;
            if (src>vm->memory_size || dst>vm->memory_size ||
                len>vm->memory_size-src || len>vm->memory_size-dst) {
                vm->reg[BB_R3]=(u32)-2;break;
            }
            if (dst>src && dst-src<len) {
                for (i=len;i>0u;--i) vm->memory[dst+i-1u]=vm->memory[src+i-1u];
            } else {
                for (i=0;i<len;++i) vm->memory[dst+i]=vm->memory[src+i];
            }
            vm->reg[BB_R3]=len;break;
        }
        case 112: vm->reg[BB_R3]=100u;break;
        case 110:
            vm->reg[BB_R3]=(u32)write_save_block(vm,
                bbvm_get_string(vm,vm->reg[BB_R3]),vm->reg[BB_R2],
                vm->reg[BB_R1],vm->reg[BB_R0]); break;
        case 111:
            vm->reg[BB_R3]=save_crc(vm,vm->reg[BB_R3],vm->reg[BB_R2],vm->reg[BB_R1]);break;
        case 104:
            vm->reg[BB_R3] = (u32)read_resource_block(vm,
                bbvm_get_string(vm, vm->reg[BB_R3]), vm->reg[BB_R2],
                vm->reg[BB_R1], vm->reg[BB_R0]); break;
        case 105: vm->reg[BB_R3] = 100u; break;
        case 106: vm->reg[BB_R3] = (u32)render_fx(vm, vm->reg[BB_R3]); break;
        case 107: vm->reg[BB_R3] = 100u; break;
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
            if (index >= 0 && index < PAGE_COUNT) {
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
            clear_resource_cache();
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
    bbvm_bda_audio_init();
    g.get_pixel_rgb = (get_pixel_rgb_fn)bda_sdk_internal_api(
        bda_sdk_internal_gui(), GUI_GET_PIXEL_RGB);
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
    if (g.frame == 0 || (s32)(u32)g.frame == -1) { g.frame = 0; return 0; }
    (void)bda_gui_frame_activate(g.frame, 0x100u);
    g.draw = bda_gui_current_draw(g.frame); g.draw_owner = g.frame;
    g.draw_object = bda_gui_draw_object_create(7u);
    if ((s32)(u32)g.draw == -1) g.draw = 0;
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
    clear_resource_cache();
    bbvm_bda_audio_stop();
    if (g.timer_started) { bda_gui_millisecond_timer_stop(); g.timer_started = 0; }
    for (i = 0; i < FILE_COUNT; ++i) close_file((u32)i);
    for (i = 0; i < PICTURE_COUNT; ++i) free_picture(i);
    for (i = 0; i < PAGE_COUNT; ++i) {
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
