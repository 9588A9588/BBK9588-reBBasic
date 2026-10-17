#include "bbvm_audio.h"

static bb_u32 le16(const bb_u8 *p) { return p[0] | ((bb_u32)p[1] << 8); }
static bb_u32 le32(const bb_u8 *p) { return le16(p) | (le16(p+2) << 16); }
static int tag(const bb_u8 *p, const char *v) {
    return p[0]==v[0] && p[1]==v[1] && p[2]==v[2] && p[3]==v[3];
}
void bbvm_audio_init(bbvm_audio_t *s, const bbvm_audio_io_t *io) {
    bb_u32 i;
    bb_u8 *p=(bb_u8 *)s;
    for (i=0; i<(bb_u32)sizeof(*s); ++i) p[i]=0;
    s->io.available=io->available; s->io.file_open=io->file_open;
    s->io.file_close=io->file_close; s->io.file_read=io->file_read;
    s->io.file_seek=io->file_seek; s->io.pcm_open=io->pcm_open;
    s->io.pcm_ready=io->pcm_ready; s->io.pcm_write=io->pcm_write;
    s->io.pcm_stop=io->pcm_stop; s->io.tick_25ms=io->tick_25ms;
    s->volume=256u;
}
static void close_file(bbvm_audio_t *s) {
    if (s->file!=0 && s->file!=-1) (void)s->io.file_close(s->file);
    s->file=0;
}
void bbvm_audio_stop(bbvm_audio_t *s) {
    close_file(s);
    if (s->opened) s->io.pcm_stop();
    s->opened=s->paused=0; s->draining=0;
    s->pending=s->pending_offset=s->left=s->tone_left=0;
    s->queue_samples=s->queue_frac=0;
}
static int fail(bbvm_audio_t *s, int error) {
    bbvm_audio_stop(s); s->error=error; return error;
}
static int read_exact(bbvm_audio_t *s, void *p, bb_u32 n) {
    ++s->reads;
    return s->io.file_read(s->file,p,n)==(int)n;
}
static int parse_wav(bbvm_audio_t *s, bb_u32 base, bb_u32 length) {
    bb_u8 h[16];
    bb_u32 limit, pos=12, i, have_fmt=0, have_data=0;
    if (length<44u) return BB_AUDIO_WAV;
    if (s->io.file_seek(s->file,(bb_s32)base,0)!=(int)base || !read_exact(s,h,12))
        return BB_AUDIO_IO;
    if (!tag(h,"RIFF") || !tag(h+8,"WAVE")) return BB_AUDIO_WAV;
    limit=le32(h+4);
    if (limit<36u || limit>length-8u) return BB_AUDIO_WAV;
    limit+=8u;
    for (i=0; i<64u && pos<=limit && limit-pos>=8u; ++i) {
        bb_u32 n, next;
        if (s->io.file_seek(s->file,(bb_s32)(base+pos),0)!=(int)(base+pos) || !read_exact(s,h,8))
            return BB_AUDIO_IO;
        n=le32(h+4);
        if (n>limit-pos-8u) return BB_AUDIO_WAV;
        next=pos+8u+n;
        if (n&1u) { if (next==limit) return BB_AUDIO_WAV; ++next; }
        if (tag(h,"fmt ")) {
            bb_u32 align;
            if (have_fmt || n<16u || !read_exact(s,h,16)) return BB_AUDIO_WAV;
            s->bits=le16(h+14); align=s->bits/8u;
            if (le16(h)!=1u || le16(h+2)!=1u || le32(h+4)!=22050u ||
                (s->bits!=8u && s->bits!=16u) || le16(h+12)!=align ||
                le32(h+8)!=22050u*align) return BB_AUDIO_FORMAT;
            have_fmt=1;
        } else if (tag(h,"data") && !have_data) {
            s->data_start=base+pos+8u; s->data_bytes=n; have_data=1;
        }
        if (have_fmt && have_data) break;
        pos=next;
    }
    if (!have_fmt || !have_data || !s->data_bytes || s->data_bytes%(s->bits/8u))
        return BB_AUDIO_WAV;
    if (s->io.file_seek(s->file,(bb_s32)s->data_start,0)!=(int)s->data_start)
        return BB_AUDIO_IO;
    s->left=s->data_bytes; return BB_AUDIO_OK;
}
static int open_pcm(bbvm_audio_t *s) {
    bb_u32 now;
    if (!s->io.pcm_open()) return fail(s,BB_AUDIO_ABI);
    s->opened=1; s->paused=0; s->draining=0;
    s->pending=s->pending_offset=0;
    now=s->io.tick_25ms(); s->last_tick=now-1u; s->stall_tick=now;
    s->error=BB_AUDIO_OK;
    s->queue_samples=s->queue_frac=0; s->queue_tick=now;
    return BB_AUDIO_OK;
}
static int begin_file(bbvm_audio_t *s, const char *path, int loop) {
    bb_u32 n;
    if (loop!=0 && loop!=1) return s->error=BB_AUDIO_ARGUMENT;
    if (path==0 || path[0]=='\0') return s->error=BB_AUDIO_PATH;
    for (n=0; n<256u && path[n]!='\0'; ++n) {}
    if (n==256u) return s->error=BB_AUDIO_PATH;
    if (!s->io.available()) return s->error=BB_AUDIO_ABI;
    bbvm_audio_stop(s);
    s->file=s->io.file_open(path);
    if (s->file==0 || s->file==-1) { s->file=0; return s->error=BB_AUDIO_OPEN; }
    s->loop=loop;
    return BB_AUDIO_OK;
}
int bbvm_audio_play(bbvm_audio_t *s, const char *path, int loop) {
    int end, result=begin_file(s,path,loop);
    if (result) return result;
    end=s->io.file_seek(s->file,0,2);
    if (end<0) return fail(s,BB_AUDIO_IO);
    result=parse_wav(s,0u,(bb_u32)end);
    if (result) return fail(s,result);
    return open_pcm(s);
}
int bbvm_audio_play_resource(bbvm_audio_t *s, const char *path, int id, int loop) {
    bb_u8 h[16];
    bb_u32 count, total, index_end, pos, base, length;
    int end, result;
    if (id<1 || id>65535) return s->error=BB_AUDIO_RESOURCE;
    result=begin_file(s,path,loop);
    if (result) return result;
    end=s->io.file_seek(s->file,0,2);
    if (end<0) return fail(s,BB_AUDIO_IO);
    if (end<16) return fail(s,BB_AUDIO_LIBRARY);
    if (s->io.file_seek(s->file,0,0)!=0 || !read_exact(s,h,16))
        return fail(s,BB_AUDIO_IO);
    count=le32(h+8); total=le32(h+12);
    if (!tag(h,"BBAL") || le32(h+4)!=1u || count<1u || count>65535u ||
        total!=(bb_u32)end || count>(total-16u)/8u)
        return fail(s,BB_AUDIO_LIBRARY);
    if ((bb_u32)id>count) return fail(s,BB_AUDIO_RESOURCE);
    index_end=16u+count*8u;
    pos=16u+((bb_u32)id-1u)*8u;
    if (s->io.file_seek(s->file,(bb_s32)pos,0)!=(int)pos || !read_exact(s,h,8))
        return fail(s,BB_AUDIO_IO);
    base=le32(h); length=le32(h+4);
    /* Subtraction bounds avoid offset+length overflow. Never scan the index or
     * allocate it: lookup is one 8-byte read regardless of library size. */
    if ((base&3u) || base<index_end || base>total || length<44u || length>total-base)
        return fail(s,BB_AUDIO_LIBRARY);
    result=parse_wav(s,base,length);
    if (result) return fail(s,result);
    return open_pcm(s);
}

/* Cooperative bounded service: at most two 1024-byte blocks per 25 ms tick.
 * ready is free queue capacity, NOT a playback-complete indication. */
void bbvm_audio_poll(bbvm_audio_t *s) {
    bb_u32 now, elapsed, blocks;
    if (!s->opened || s->paused) return;
    now=s->io.tick_25ms();
    if (now==s->last_tick) return;
    s->last_tick=now; ++s->polls;
    elapsed=now-s->queue_tick;
    if (elapsed>=40u) { s->queue_samples=0; s->queue_frac=0; }
    else {
        bb_u32 total=elapsed*22050u+s->queue_frac;
        bb_u32 consumed=total/40u; s->queue_frac=total%40u;
        s->queue_samples=consumed>=s->queue_samples ? 0u : s->queue_samples-consumed;
    }
    s->queue_tick=now;
    if (s->draining) {
        if (now-s->drain_start>=4u) bbvm_audio_stop(s);
        return;
    }
    for (blocks=0; blocks<2u; ++blocks) {
        int ready, written;
        if (s->queue_samples>BBVM_AUDIO_FRAMES) return;
        ready=s->io.pcm_ready();
        if (ready<0) { (void)fail(s,BB_AUDIO_IO); return; }
        if (!ready) {
            if (now-s->stall_tick>200u) (void)fail(s,BB_AUDIO_TIMEOUT);
            return;
        }
        if (!s->pending) {
            bb_u32 i, frames=BBVM_AUDIO_FRAMES, got=0;
            signed short *pcm=s->pcm[s->buffer_index];
            if (s->file && !s->left) {
                if (s->loop) {
                    if (s->io.file_seek(s->file,(bb_s32)s->data_start,0)!=(int)s->data_start) {
                        (void)fail(s,BB_AUDIO_IO); return;
                    }
                    s->left=s->data_bytes;
                } else close_file(s);
            }
            if (s->file) {
                got=s->left;
                if (got>BBVM_AUDIO_FRAMES*(s->bits/8u)) got=BBVM_AUDIO_FRAMES*(s->bits/8u);
                if (!read_exact(s,s->input,got)) { (void)fail(s,BB_AUDIO_IO); return; }
                s->left-=got; frames=got/(s->bits/8u);
            } else if (s->tone_left) {
                if (frames>s->tone_left) frames=s->tone_left;
            } else { s->draining=1; s->drain_start=now; return; }
            for (i=0; i<BBVM_AUDIO_FRAMES; ++i) {
                int sample=0;
                if (i<frames && got)
                    sample=s->bits==16u ? (signed short)le16(s->input+2u*i)
                                               : ((int)s->input[i]-128)*256;
                if (i<frames && s->tone_left) {
                    sample+=(s->phase&0x80000000u) ? 6000 : -6000;
                    s->phase+=s->phase_step; --s->tone_left;
                }
                if (sample>32767) sample=32767;
                if (sample< -32768) sample= -32768;
                pcm[i]=(signed short)(sample*(int)s->volume/256);
            }
            s->pending=BBVM_AUDIO_BLOCK; s->pending_offset=0;
        }
        /* Firmware may modify its input; this is always writable RAM. */
        written=s->io.pcm_write((bb_u8 *)s->pcm[s->buffer_index]+s->pending_offset,
                                s->pending-s->pending_offset);
        if (written<0 || (bb_u32)written>s->pending-s->pending_offset || (written&1)) {
            (void)fail(s,BB_AUDIO_IO); return;
        }
        if (!written) {
            if (now-s->stall_tick>200u) (void)fail(s,BB_AUDIO_TIMEOUT);
            return;
        }
        s->stall_tick=now; s->submitted+=(bb_u32)written;
        s->pending_offset+=(bb_u32)written; s->queue_samples+=(bb_u32)written/2u;
        if (s->pending_offset==s->pending) { s->pending=0; s->buffer_index^=1u; }
        else return;
    }
}
int bbvm_audio_tone(bbvm_audio_t *s, int hz, int ms) {
    if (hz<40 || hz>10000 || ms<1 || ms>60000) return s->error=BB_AUDIO_ARGUMENT;
    if (!s->io.available()) return s->error=BB_AUDIO_ABI;
    if (!s->opened && open_pcm(s)!=0) return s->error;
    s->tone_left=(bb_u32)ms*22050u/1000u;
    s->phase_step=(((bb_u32)hz<<16)/22050u)<<16; s->phase=0;
    s->draining=0; return s->error=BB_AUDIO_OK;
}
int bbvm_audio_volume(bbvm_audio_t *s, int volume) {
    if (volume<0 || volume>256) return s->error=BB_AUDIO_ARGUMENT;
    s->volume=(bb_u32)volume; return s->error=BB_AUDIO_OK;
}
void bbvm_audio_pause(bbvm_audio_t *s, int paused) {
    s->paused=paused!=0;
    /* Pausing suspends enqueue, not the firmware's already queued <=46 ms. */
    if (!s->paused && s->opened) {
        s->stall_tick=s->io.tick_25ms(); s->last_tick=s->stall_tick-1u;
    }
}
