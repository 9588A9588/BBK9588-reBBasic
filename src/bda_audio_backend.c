#include "bda_sdk.h"
#include "bbvm_audio.h"
#include "bbvm_mixer.h"

static bbvm_audio_t audio;
static bbvm_mixer_t mixer;
static int ram_pointer(u32 p) { return p>=0x80000000u && p<0x82000000u; }
static int available(void) {
    void *sys=bda_sdk_internal_sys();
    static const u32 slots[4]={0x6cu,0x74u,0x78u,0xa0u};
    u32 i;
    if (!ram_pointer((u32)sys)) return 0;
    for (i=0; i<4u; ++i)
        if (!ram_pointer((u32)bda_sdk_internal_api(sys,slots[i]))) return 0;
    return 1;
}
static int file_open(const char *path) { return bda_fs_fopen_raw(path,"rb"); }
static int file_close(int file) { return bda_fs_close_raw(file); }
static int file_read(int file, void *buffer, bb_u32 bytes) {
    return bda_fs_read_raw(file,buffer,bytes);
}
static int file_seek(int file, bb_s32 offset, int origin) {
    return bda_fs_seek_raw(file,offset,origin);
}
static int pcm_open(void) { bda_audio_open_pcm(22050u,16u,1u); return 1; }
static int pcm_ready(void) { return bda_audio_ready(); }
static int pcm_write(void *buffer, bb_u32 bytes) { return bda_audio_write(buffer,bytes); }
static void pcm_stop(void) { bda_audio_stop(); }
static bb_u32 tick(void) { return bda_gui_tick_count_25ms(); }

void bbvm_bda_audio_init(void) {
    static const bbvm_audio_io_t io={available,file_open,file_close,file_read,file_seek,
                                    pcm_open,pcm_ready,pcm_write,pcm_stop,tick};
    bbvm_audio_init(&audio,&io);
    bbvm_mixer_init(&mixer,&io);
}
void bbvm_bda_audio_poll(void) { bbvm_audio_poll(&audio);bbvm_mixer_poll(&mixer); }
void bbvm_bda_audio_stop(void) { bbvm_audio_stop(&audio);bbvm_mixer_stop(&mixer,-1); }
int bbvm_bda_audio_out(bbvm_t *vm, bb_u32 port) {
    if(port==108u) {
        bbvm_audio_stop(&audio);
        vm->reg[BB_R3]=(bb_u32)bbvm_mixer_out(&mixer,vm,vm->reg[BB_R3],bbvm_get_string(vm,vm->reg[BB_R2]));return 1;
    }
    if(port==109u) {vm->reg[BB_R3]=BBVM_MIX_VERSION;return 1;}
    if(port==96u||port==100u||port==103u)bbvm_mixer_stop(&mixer,-1);
    if(port==97u)bbvm_mixer_stop(&mixer,-1);
    return bbvm_audio_out(&audio,vm,port);
}
bb_u32 bbvm_bda_audio_in(void *opaque, bbvm_t *vm, bb_u32 port) {
    (void)opaque;
    return bbvm_audio_in(&audio,vm,port);
}
