#include "bbvm_audio.h"

/* Shared by the actual BDA and host tests, with the BB2.0++ calling convention. */
int bbvm_audio_out(bbvm_audio_t *s, bbvm_t *vm, bb_u32 port) {
    int result=0;
    switch (port) {
        case 96: result=bbvm_audio_play(s,bbvm_get_string(vm,vm->reg[BB_R3]),
                                       (int)vm->reg[BB_R2]); break;
        case 97: bbvm_audio_stop(s); break;
        case 98: result=bbvm_audio_volume(s,(int)vm->reg[BB_R3]); break;
        case 99: bbvm_audio_poll(s); break;
        case 100: result=bbvm_audio_tone(s,(int)vm->reg[BB_R3],
                                        (int)vm->reg[BB_R2]); break;
        case 101: bbvm_audio_pause(s,1); break;
        case 102: bbvm_audio_pause(s,0); break;
        case 103: result=bbvm_audio_play_resource(s,bbvm_get_string(vm,vm->reg[BB_R3]),
                                                (int)vm->reg[BB_R2],
                                                (int)vm->reg[BB_R1]); break;
        default: return 0;
    }
    vm->reg[BB_R3]=(bb_u32)result;
    return 1;
}
bb_u32 bbvm_audio_in(bbvm_audio_t *s, bbvm_t *vm, bb_u32 port) {
    switch (port) {
        case 96: return (bb_u32)s->opened;
        case 97: return (bb_u32)s->error;
        case 98: return BBVM_AUDIO_VERSION;
        default: vm->error=BBVM_ERR_PORT; vm->running=0; return 0;
    }
}
