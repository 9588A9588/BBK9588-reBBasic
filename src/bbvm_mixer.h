#ifndef BBVM_MIXER_H
#define BBVM_MIXER_H
#include "bbvm_audio.h"
#define BBVM_MIX_VERSION 100u
#define BBVM_MIX_METADATA_SLOTS 16u
typedef struct bbvm_mix_channel {
    int file,loop,active,error;
    bb_u32 data_start,data_bytes,left,bits,volume,step,phase;
    bb_u32 buffered,position,have_sample,fade_start,fade_ticks,fade_volume;
    bb_u32 file_hash,file_path_len,file_total;
    bb_u32 played_once;
    int sample,fade_in;
    bb_u8 input[1024];
} bbvm_mix_channel_t;
typedef struct bbvm_mix_metadata {
    bb_u32 valid,path_hash,path_len,total,id,base,length,data_start,data_bytes,bits,age;
} bbvm_mix_metadata_t;
typedef struct bbvm_mixer {
    bbvm_audio_io_t io;
    bbvm_mix_channel_t channel[2];
    bbvm_mix_metadata_t metadata[BBVM_MIX_METADATA_SLOTS];
    bb_u32 metadata_clock;
    signed short pcm[2][512];
    bb_u32 opened,last_tick,queue_tick,queue_samples,queue_frac;
    bb_u32 pending,offset,buffer_index,stall_tick,draining,drain_start;
    int error;
} bbvm_mixer_t;
void bbvm_mixer_init(bbvm_mixer_t *m,const bbvm_audio_io_t *io);
void bbvm_mixer_stop(bbvm_mixer_t *m,int channel);
int bbvm_mixer_play(bbvm_mixer_t *m,const char *path,int id,int channel,int loop,int volume,int pitch);
int bbvm_mixer_fade(bbvm_mixer_t *m,int channel,int milliseconds);
void bbvm_mixer_poll(bbvm_mixer_t *m);
int bbvm_mixer_out(bbvm_mixer_t *m,bbvm_t *vm,bb_u32 address,const char *path);
#endif
