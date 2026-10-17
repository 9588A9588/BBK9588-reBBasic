#ifndef BBVM_AUDIO_H
#define BBVM_AUDIO_H
#include "bbvm.h"

/* No heap, firmware addresses, interrupts, or platform headers in this engine. */
#define BBVM_AUDIO_BLOCK 1024u
#define BBVM_AUDIO_FRAMES 512u
#define BBVM_AUDIO_VERSION 101u
enum { BB_AUDIO_OK=0, BB_AUDIO_PATH=-1, BB_AUDIO_OPEN=-2, BB_AUDIO_WAV=-3,
       BB_AUDIO_FORMAT=-4, BB_AUDIO_IO=-5, BB_AUDIO_ARGUMENT=-6,
       BB_AUDIO_ABI=-7, BB_AUDIO_TIMEOUT=-8, BB_AUDIO_LIBRARY=-9,
       BB_AUDIO_RESOURCE=-10 };

typedef struct bbvm_audio_io {
    int (*available)(void);
    int (*file_open)(const char *path);
    int (*file_close)(int file);
    int (*file_read)(int file, void *buffer, bb_u32 bytes);
    int (*file_seek)(int file, bb_s32 offset, int origin);
    int (*pcm_open)(void);
    int (*pcm_ready)(void);
    int (*pcm_write)(void *buffer, bb_u32 bytes);
    void (*pcm_stop)(void);
    bb_u32 (*tick_25ms)(void);
} bbvm_audio_io_t;

typedef struct bbvm_audio {
    bbvm_audio_io_t io;
    int file, opened, paused, loop, error;
    bb_u32 data_start, data_bytes, left, bits;
    bb_u32 pending, pending_offset, buffer_index;
    bb_u32 tone_left, phase, phase_step;
    bb_u32 draining, drain_start, last_tick, stall_tick;
    bb_u32 volume, submitted, polls, reads;
    bb_u32 queue_samples, queue_tick, queue_frac;
    bb_u8 input[BBVM_AUDIO_BLOCK];
    signed short pcm[2][BBVM_AUDIO_FRAMES];
} bbvm_audio_t;

void bbvm_audio_init(bbvm_audio_t *s, const bbvm_audio_io_t *io);
int bbvm_audio_play(bbvm_audio_t *s, const char *path, int loop);
/* BBAL v1 indexed library. WAV parser and stream reads stay inside the member. */
int bbvm_audio_play_resource(bbvm_audio_t *s, const char *path, int id, int loop);
int bbvm_audio_tone(bbvm_audio_t *s, int hz, int milliseconds);
void bbvm_audio_stop(bbvm_audio_t *s);
void bbvm_audio_poll(bbvm_audio_t *s);
int bbvm_audio_volume(bbvm_audio_t *s, int volume);
void bbvm_audio_pause(bbvm_audio_t *s, int paused);
int bbvm_audio_out(bbvm_audio_t *s, bbvm_t *vm, bb_u32 port);
bb_u32 bbvm_audio_in(bbvm_audio_t *s, bbvm_t *vm, bb_u32 port);

/* Firmware adapter; only bda_audio_backend.c implements these. */
void bbvm_bda_audio_init(void);
void bbvm_bda_audio_poll(void);
void bbvm_bda_audio_stop(void);
int bbvm_bda_audio_out(bbvm_t *vm, bb_u32 port);
bb_u32 bbvm_bda_audio_in(void *opaque, bbvm_t *vm, bb_u32 port);
#endif
