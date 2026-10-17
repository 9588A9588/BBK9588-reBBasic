#include "bbvm.h"
#include "bbvm_audio.h"
unsigned int test_size_vm(void) { return (unsigned int)sizeof(bbvm_t); }
unsigned int test_size_audio(void) { return (unsigned int)sizeof(bbvm_audio_t); }

#include "bbvm_mixer.h"
unsigned int test_size_mixer(void) { return (unsigned int)sizeof(bbvm_mixer_t); }
