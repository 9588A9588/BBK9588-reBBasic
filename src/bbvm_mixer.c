#include "bbvm_mixer.h"
#include "nobi_sound_index.h"
static bb_u32 u16(const bb_u8 *p) { return p[0]|((bb_u32)p[1]<<8); }
static bb_u32 u32(const bb_u8 *p) { return u16(p)|(u16(p+2)<<16); }
static int tag(const bb_u8 *p,const char *s) { return p[0]==s[0]&&p[1]==s[1]&&p[2]==s[2]&&p[3]==s[3]; }
static bb_u32 path_key(const char *path,bb_u32 *length) {
    bb_u32 i,h=2166136261u;
    for(i=0;i<256u&&path[i];++i) h=(h^(bb_u8)path[i])*16777619u;
    *length=i;
    return h^(i*16777619u);
}
static void close_channel(bbvm_mixer_t *m,int n) {
    bbvm_mix_channel_t *c=&m->channel[n];
    if (c->file && c->file!=-1) (void)m->io.file_close(c->file);
    c->file=0;c->file_hash=c->file_path_len=c->file_total=0;
    c->active=0;c->buffered=c->position=c->have_sample=0;c->fade_ticks=0;
}
/* Stop decoding a channel but retain its already-open library handle.  The
 * game replaces short sound effects frequently; reopening the 169 MiB
 * sound.lib on every hit makes the firmware filesystem latency visible in
 * gameplay.  Explicit mixer stop still closes both handles. */
static void deactivate_channel(bbvm_mixer_t *m,int n) {
    bbvm_mix_channel_t *c=&m->channel[n];
    c->active=0;c->buffered=c->position=c->have_sample=0;c->fade_ticks=0;
}
static bbvm_mix_metadata_t *metadata_find(bbvm_mixer_t *m,bb_u32 path_hash,
                                          bb_u32 path_len,bb_u32 total,bb_u32 id) {
    bb_u32 i;
    for(i=0;i<BBVM_MIX_METADATA_SLOTS;++i) {
        bbvm_mix_metadata_t *x=&m->metadata[i];
        if(x->valid&&x->path_hash==path_hash&&x->path_len==path_len&&
           x->total==total&&x->id==id) {
            x->age=++m->metadata_clock;
            return x;
        }
    }
    return 0;
}
static void metadata_store(bbvm_mixer_t *m,bb_u32 path_hash,bb_u32 path_len,
                           bb_u32 total,bb_u32 id,bb_u32 base,bb_u32 length,
                           bb_u32 data_start,bb_u32 data_bytes,bb_u32 bits) {
    bb_u32 i,slot=0;bb_u32 oldest=0xffffffffu;
    for(i=0;i<BBVM_MIX_METADATA_SLOTS;++i) {
        if(!m->metadata[i].valid) {slot=i;break;}
        if(m->metadata[i].age<oldest) {oldest=m->metadata[i].age;slot=i;}
    }
    m->metadata[slot].valid=1;
    m->metadata[slot].path_hash=path_hash;
    m->metadata[slot].path_len=path_len;
    m->metadata[slot].total=total;
    m->metadata[slot].id=id;
    m->metadata[slot].base=base;
    m->metadata[slot].length=length;
    m->metadata[slot].data_start=data_start;
    m->metadata[slot].data_bytes=data_bytes;
    m->metadata[slot].bits=bits;
    m->metadata[slot].age=++m->metadata_clock;
}
void bbvm_mixer_stop(bbvm_mixer_t *m,int n) {
    if (n<0) { close_channel(m,0);close_channel(m,1);m->channel[0].played_once=m->channel[1].played_once=0; }
    else if (n<2) {close_channel(m,n);m->channel[n].played_once=0;}
    if (n<0) {
        if (m->opened) m->io.pcm_stop();
        m->opened=m->pending=m->offset=m->queue_samples=m->queue_frac=m->draining=0;
    }
}
void bbvm_mixer_init(bbvm_mixer_t *m,const bbvm_audio_io_t *io) {
    bb_u32 i;bb_u8 *p=(bb_u8 *)m;
    for (i=0;i<(bb_u32)sizeof(*m);++i)p[i]=0;
    m->io.available=io->available;m->io.file_open=io->file_open;
    m->io.file_close=io->file_close;m->io.file_read=io->file_read;m->io.file_seek=io->file_seek;
    m->io.pcm_open=io->pcm_open;m->io.pcm_ready=io->pcm_ready;m->io.pcm_write=io->pcm_write;
    m->io.pcm_stop=io->pcm_stop;m->io.tick_25ms=io->tick_25ms;
}
static int fail_channel(bbvm_mixer_t *m,int n,int error) {
    close_channel(m,n);m->channel[n].error=error;m->error=error;return error;
}
typedef struct mix_metadata_window {
    bb_u32 total,start,valid;
    bb_u8 bytes[64];
} mix_metadata_window_t;
static int read_at(bbvm_mixer_t *m,int file,mix_metadata_window_t *window,
                   bb_u32 at,void *dest,bb_u32 bytes) {
    bb_u32 i,n;bb_u8 *out=(bb_u8 *)dest;
    if(at>window->total||bytes>window->total-at||bytes>64u)return 0;
    if(at<window->start||at-window->start>window->valid||
       bytes>window->valid-(at-window->start)) {
        window->valid=0;window->start=at;
        n=window->total-at;if(n>64u)n=64u;
        if(m->io.file_seek(file,(bb_s32)at,0)!=(int)at||
           m->io.file_read(file,window->bytes,n)!=(int)n)return 0;
        window->valid=n;
    }
    for(i=0;i<bytes;++i)out[i]=window->bytes[at-window->start+i];
    return 1;
}
int bbvm_mixer_play(bbvm_mixer_t *m,const char *path,int id,int n,int loop,int volume,int pitch) {
    bb_u8 h[16];bb_u32 count,total,base,length,pos,limit,k,fmt=0,data=0,now;
    bb_u32 path_hash,path_len;bbvm_mix_metadata_t *cached;
    const nobi_sound_index_entry_t *preindexed=0;
    mix_metadata_window_t window;
    bbvm_mix_channel_t *c;int end,reuse;
    if (n<0||n>1||id<1||id>65535||(loop!=0&&loop!=1)||volume<0||volume>256||pitch<50||pitch>200)
        return m->error=BB_AUDIO_ARGUMENT;
    if (!path||!path[0])return m->error=BB_AUDIO_PATH;
    for (k=0;k<256u&&path[k];++k) {}
    if(k==256u)return m->error=BB_AUDIO_PATH;
    if(!m->io.available())return m->error=BB_AUDIO_ABI;
    c=&m->channel[n];path_hash=path_key(path,&path_len);
    reuse=c->file!=0&&c->file!=-1&&c->file_hash==path_hash&&c->file_path_len==path_len;
    if(reuse) deactivate_channel(m,n); else close_channel(m,n);
    c=&m->channel[n];c->played_once=0;
    if(!reuse) {
        c->file=m->io.file_open(path);
        if(!c->file||c->file==-1)return fail_channel(m,n,BB_AUDIO_OPEN);
        end=m->io.file_seek(c->file,0,2);
        if(end<0)return fail_channel(m,n,BB_AUDIO_LIBRARY);
        c->file_hash=path_hash;c->file_path_len=path_len;c->file_total=(bb_u32)end;
    }
    total=c->file_total;
    cached=metadata_find(m,path_hash,path_len,total,(bb_u32)id);
    if(cached) {
        base=cached->base;length=cached->length;c->data_start=cached->data_start;
        c->data_bytes=cached->data_bytes;c->bits=cached->bits;
    } else {
        /* R229: the game uses this exact immutable library.  Validate the
         * outer BBAL header once per uncached resource, then use the offline
         * member index and skip the SD-card WAV chunk walk.  Any mismatch
         * deliberately falls through to the R228 parser below. */
        if(path_len==11u&&path[0]=='.'&&path[1]=='/'&&path[2]=='s'&&path[3]=='o'&&
           path[4]=='u'&&path[5]=='n'&&path[6]=='d'&&path[7]=='.'&&path[8]=='l'&&
           path[9]=='i'&&path[10]=='b'&&c->file_total==NOBI_SOUND_INDEX_TOTAL&&
           (bb_u32)id<=NOBI_SOUND_INDEX_COUNT) {
            window.total=c->file_total;window.start=window.valid=0;
            if(read_at(m,c->file,&window,0,h,16)&&tag(h,"BBAL")&&u32(h+4)==1u&&
               u32(h+8)==NOBI_SOUND_INDEX_COUNT&&u32(h+12)==NOBI_SOUND_INDEX_TOTAL)
                preindexed=&nobi_sound_index[(bb_u32)id-1u];
        }
        if(preindexed) {
            base=preindexed->base;length=preindexed->length;
            c->data_start=preindexed->data_start;c->data_bytes=preindexed->data_bytes;
            c->bits=preindexed->bits;
        } else {
            window.total=total;window.start=window.valid=0;
            if(total<16u||!read_at(m,c->file,&window,0,h,16))return fail_channel(m,n,BB_AUDIO_LIBRARY);
            count=u32(h+8);total=u32(h+12);
            if(!tag(h,"BBAL")||u32(h+4)!=1u||count<1u||count>65535u||total!=c->file_total||count>(total-16u)/8u)
                return fail_channel(m,n,BB_AUDIO_LIBRARY);
            if((bb_u32)id>count)return fail_channel(m,n,BB_AUDIO_RESOURCE);
            if(!read_at(m,c->file,&window,16u+((bb_u32)id-1u)*8u,h,8))return fail_channel(m,n,BB_AUDIO_IO);
            base=u32(h);length=u32(h+4);
            if((base&3u)||base<16u+count*8u||base>total||length<44u||length>total-base)
                return fail_channel(m,n,BB_AUDIO_LIBRARY);
            if(!read_at(m,c->file,&window,base,h,12)||!tag(h,"RIFF")||!tag(h+8,"WAVE"))return fail_channel(m,n,BB_AUDIO_WAV);
            limit=u32(h+4);
            if(limit<36u||limit>length-8u)return fail_channel(m,n,BB_AUDIO_WAV);
            limit+=8u;pos=12u;
            for(k=0;k<64u&&pos<=limit&&limit-pos>=8u;++k) {
                bb_u32 size,next;
                if(!read_at(m,c->file,&window,base+pos,h,8))return fail_channel(m,n,BB_AUDIO_IO);
                size=u32(h+4);
                if(size>limit-pos-8u)return fail_channel(m,n,BB_AUDIO_WAV);
                next=pos+8u+size;
                if(size&1u){if(next==limit)return fail_channel(m,n,BB_AUDIO_WAV);++next;}
                if(tag(h,"fmt ")) {
                    if(fmt||size<16u||!read_at(m,c->file,&window,base+pos+8u,h,16))return fail_channel(m,n,BB_AUDIO_WAV);
                    c->bits=u16(h+14);
                    if(u16(h)!=1u||u16(h+2)!=1u||u32(h+4)!=22050u||(c->bits!=8u&&c->bits!=16u)||
                       u16(h+12)!=c->bits/8u||u32(h+8)!=22050u*(c->bits/8u))return fail_channel(m,n,BB_AUDIO_FORMAT);
                    fmt=1;
                } else if(tag(h,"data")&&!data) {c->data_start=base+pos+8u;c->data_bytes=size;data=1;}
                if(fmt&&data)break;
                pos=next;
            }
            if(!fmt||!data||!c->data_bytes||c->data_bytes%(c->bits/8u))return fail_channel(m,n,BB_AUDIO_WAV);
            metadata_store(m,path_hash,path_len,c->file_total,(bb_u32)id,base,length,
                           c->data_start,c->data_bytes,c->bits);
        }
    }
    if(m->io.file_seek(c->file,(bb_s32)c->data_start,0)!=(int)c->data_start)return fail_channel(m,n,BB_AUDIO_IO);
    c->left=c->data_bytes;c->loop=loop;c->volume=(bb_u32)volume;
    c->step=(bb_u32)pitch*256u/100u;c->phase=0;c->active=1;c->error=0;
    c->buffered=c->position=c->have_sample=c->fade_ticks=0;
    now=m->io.tick_25ms();
    if(!m->opened) {
        if(!m->io.pcm_open())return fail_channel(m,n,BB_AUDIO_ABI);
        m->opened=1;m->last_tick=now-1u;m->queue_tick=now;
        m->queue_samples=m->queue_frac=m->pending=m->offset=0;m->buffer_index=0;
    }
    m->stall_tick=now;m->draining=0;return m->error=0;
}
static int next_sample(bbvm_mixer_t *m,int n) {
    bbvm_mix_channel_t *c=&m->channel[n];bb_u32 bytes;
    if(!c->active)return 0;
    if(c->position>=c->buffered) {
        if(!c->left) {
            c->played_once=1;
            if(!c->loop){deactivate_channel(m,n);return 0;}
            if(m->io.file_seek(c->file,(bb_s32)c->data_start,0)!=(int)c->data_start) {
                (void)fail_channel(m,n,BB_AUDIO_IO);return 0;
            }
            c->left=c->data_bytes;
        }
        bytes=c->left;if(bytes>1024u)bytes=1024u;
        if(m->io.file_read(c->file,c->input,bytes)!=(int)bytes){(void)fail_channel(m,n,BB_AUDIO_IO);return 0;}
        c->left-=bytes;c->position=0;c->buffered=bytes;
    }
    c->sample=c->bits==16u?(signed short)u16(c->input+c->position):((int)c->input[c->position]-128)*256;
    c->position+=c->bits/8u;c->have_sample=1;return 1;
}
static int sample(bbvm_mixer_t *m,int n) {
    bbvm_mix_channel_t *c=&m->channel[n];
    if(!c->active)return 0;
    if(!c->have_sample&&!next_sample(m,n))return 0;
    while(c->phase>=256u) {c->phase-=256u;if(!next_sample(m,n))return 0;}
    c->phase+=c->step;return c->sample*(int)c->volume/256;
}

/* R271: consume only samples already in the 1 KiB channel buffers.
 * No IO, allocations, persistent guest pointers, or changed resampling.
 * The common 100% pitch has phase=256 between samples. Advance an entire
 * bounded span, leaving exactly the state of repeated sample() calls.
 * MIPS uses explicit little-endian bytes: input alignment is not assumed.
 */
static int steady_channel(const bbvm_mix_channel_t *c) {
    return !c->active || (c->data_bytes>=32u && c->step==256u &&
        (c->phase==0u || c->phase==256u) && (c->bits==8u || c->bits==16u));
}
static bb_u32 buffered_mix(bbvm_mixer_t *m,signed short *pcm,bb_u32 count) {
    bbvm_mix_channel_t *a=&m->channel[0], *b=&m->channel[1];
    bb_u32 i,n=count,wa=0u,wb=0u,va=0u,vb=0u;
    const bb_u8 *pa=a->input, *pb=b->input;
    int sa=0,sb=0;
    if(a->active) {
        if(!a->have_sample || a->phase!=256u || a->position>a->buffered ||
            a->buffered>1024u)return 0u;
        wa=a->bits/8u; va=a->volume; pa+=a->position;
        i=(a->buffered-a->position)/wa; if(i<n)n=i;
    }
    if(b->active) {
        if(!b->have_sample || b->phase!=256u || b->position>b->buffered ||
            b->buffered>1024u)return 0u;
        wb=b->bits/8u; vb=b->volume; pb+=b->position;
        i=(b->buffered-b->position)/wb; if(i<n)n=i;
    }
    if(n<4u)return 0u;
    for(i=0u;i<n;++i) {
        int v;
        if(wa==2u)sa=(signed short)u16(pa);
        else if(wa==1u)sa=((int)*pa-128)*256;
        if(wb==2u)sb=(signed short)u16(pb);
        else if(wb==1u)sb=((int)*pb-128)*256;
        /* Signed division must truncate toward zero, not a signed shift. */
        v=sa*(int)va/256+sb*(int)vb/256;
        if(v>32767)v=32767;
        if(v< -32768)v= -32768;
        pcm[i]=(signed short)v; pa+=wa; pb+=wb;
    }
    if(wa){a->position+=n*wa; a->sample=sa;}
    if(wb){b->position+=n*wb; b->sample=sb;}
    return n;
}

int bbvm_mixer_fade(bbvm_mixer_t *m,int n,int milliseconds) {
    bbvm_mix_channel_t *c;
    if(n<0||n>1||milliseconds<0||milliseconds>600000)return m->error=BB_AUDIO_ARGUMENT;
    if(!milliseconds){close_channel(m,n);return 0;}
    c=&m->channel[n];c->fade_start=m->io.tick_25ms();c->fade_ticks=((bb_u32)milliseconds+24u)/25u;
    c->fade_volume=c->volume;c->fade_in=0;return 0;
}
void bbvm_mixer_poll(bbvm_mixer_t *m) {
    bb_u32 now,elapsed,total,blocks,n;
    if(!m->opened)return;
    now=m->io.tick_25ms();if(now==m->last_tick)return;m->last_tick=now;
    elapsed=now-m->queue_tick;
    if(elapsed>=40u){m->queue_samples=0;m->queue_frac=0;}
    else {total=elapsed*22050u+m->queue_frac;elapsed=total/40u;m->queue_frac=total%40u;
        m->queue_samples=elapsed>=m->queue_samples?0u:m->queue_samples-elapsed;}
    m->queue_tick=now;
    for(n=0;n<2u;++n) {
        bbvm_mix_channel_t *c=&m->channel[n];
        if(c->active&&c->fade_ticks) {
            elapsed=now-c->fade_start;
            if(elapsed>=c->fade_ticks) {
                if(c->fade_in){c->volume=c->fade_volume;c->fade_ticks=0;c->fade_in=0;}
                else deactivate_channel(m,(int)n);
            } else c->volume=c->fade_volume*(c->fade_in?elapsed:c->fade_ticks-elapsed)/c->fade_ticks;
        }
    }
    if(m->draining) {
        if(now-m->drain_start>=4u) {
            /* Natural EOF retains played_once, unlike an explicit stop. */
            if(m->opened)m->io.pcm_stop();
            m->opened=m->pending=m->offset=m->queue_samples=m->queue_frac=m->draining=0;
        }
        return;
    }
    for(blocks=0;blocks<2u;++blocks) {
        int ready,written;
        if(m->queue_samples>512u)return;
        ready=m->io.pcm_ready();
        if(ready<0){m->error=BB_AUDIO_IO;bbvm_mixer_stop(m,-1);return;}
        if(!ready) {if(now-m->stall_tick>200u){m->error=BB_AUDIO_TIMEOUT;bbvm_mixer_stop(m,-1);}return;}
        if(!m->pending) {
            bb_u32 i;signed short *pcm=m->pcm[m->buffer_index];
            if(!m->channel[0].active&&!m->channel[1].active){m->draining=1;m->drain_start=now;return;}
            if(steady_channel(&m->channel[0]) && steady_channel(&m->channel[1])) {
                i=0u;
                while(i<512u) {
                    bb_u32 span=buffered_mix(m,pcm+i,512u-i);
                    if(span) i+=span;
                    else {
                        int v=sample(m,0)+sample(m,1);
                        if(v>32767)v=32767;
                        if(v< -32768)v= -32768;
                        pcm[i++]=(signed short)v;
                    }
                }
            } else {
                for(i=0;i<512u;++i) {
                    int v=sample(m,0)+sample(m,1);
                    if(v>32767)v=32767;
                    if(v< -32768)v= -32768;
                    pcm[i]=(signed short)v;
                }
            }
            m->pending=1024u;m->offset=0;
        }
        written=m->io.pcm_write((bb_u8 *)m->pcm[m->buffer_index]+m->offset,m->pending-m->offset);
        if(written<0||(bb_u32)written>m->pending-m->offset||(written&1)) {
            m->error=BB_AUDIO_IO;bbvm_mixer_stop(m,-1);return;
        }
        if(!written){if(now-m->stall_tick>200u){m->error=BB_AUDIO_TIMEOUT;bbvm_mixer_stop(m,-1);}return;}
        m->stall_tick=now;m->offset+=(bb_u32)written;m->queue_samples+=(bb_u32)written/2u;
        if(m->offset==m->pending){m->pending=m->offset=0;m->buffer_index^=1u;}else return;
    }
}
/* MIXCMD is an unaligned-safe 8-word packet in the BASIC VM, like RENDERFX.
 * [operation,channel,reserved,id,loop,volume0..256,pitch50..200,fade_ms]
 * Path is a live argument of MIXCMD, never a cached temporary string handle.
 */
int bbvm_mixer_out(bbvm_mixer_t *m,bbvm_t *vm,bb_u32 address,const char *path) {
    bb_u32 p[8],i;
    if(address>vm->memory_size||32u>vm->memory_size-address)return BB_AUDIO_ARGUMENT;
    for(i=0;i<8u;++i)p[i]=bbvm_read_u32(vm,address+i*4u);
    if(p[0]==1u) {
        int r;
        if(p[7]>600000u)return BB_AUDIO_ARGUMENT;
        r=bbvm_mixer_play(m,path,(int)p[3],(int)p[1],(int)p[4],(int)p[5],(int)p[6]);
        if(!r&&p[7]) {
            bbvm_mix_channel_t *c=&m->channel[p[1]];
            c->fade_volume=c->volume;c->volume=0;c->fade_ticks=(p[7]+24u)/25u;
            c->fade_start=m->io.tick_25ms();c->fade_in=1;
        }
        return r;
    }
    if(p[0]==2u){if(p[1]>1u&&p[1]!=0xffffffffu)return BB_AUDIO_ARGUMENT;bbvm_mixer_stop(m,(int)p[1]);return 0;}
    if(p[0]==3u)return bbvm_mixer_fade(m,(int)p[1],(int)p[7]);
    if(p[0]==4u){bbvm_mixer_poll(m);return m->error;}
    /* Query the next decoder position without IO. Cursor is the current
     * sample index; phase preserves pitch resampling's pending advance. */
    if(p[0]==5u) {
        bbvm_mix_channel_t *c;bb_u32 used,bytes;
        if(p[1]>1u)return BB_AUDIO_ARGUMENT;
        c=&m->channel[p[1]];bytes=c->bits/8u;
        used=c->active ? c->data_bytes-c->left-c->buffered+c->position : 0u;
        p[3]=(c->active&&bytes)?(used-(c->have_sample?bytes:0u))/bytes:0u;
        p[4]=c->active?c->phase:0u;p[5]=(bb_u32)c->active;
        p[6]=c->active?c->volume:0u;p[7]=(bb_u32)c->loop;
        for(i=3u;i<8u;++i)(void)bbvm_write_u32(vm,address+i*4u,p[i]);
        return 0;
    }
    /* Seek only inside the already validated active BBAL member. No reopen,
     * unbounded scan, allocation, or read beyond the current WAV slice. */
    if(p[0]==6u) {
        bbvm_mix_channel_t *c;bb_u32 bytes,at,offset;
        if(p[1]>1u||p[4]>767u||p[6]>256u)return BB_AUDIO_ARGUMENT;
        c=&m->channel[p[1]];bytes=c->bits/8u;
        if(!c->active||!bytes||p[3]>=c->data_bytes/bytes||p[4]>c->step+255u)return BB_AUDIO_ARGUMENT;
        offset=p[3]*bytes;at=c->data_start+offset;
        if(m->io.file_seek(c->file,(bb_s32)at,0)!=(int)at)return m->error=BB_AUDIO_IO;
        c->left=c->data_bytes-offset;c->buffered=c->position=c->have_sample=0;
        c->phase=p[4];c->volume=p[6];c->fade_ticks=0;
        m->pending=m->offset=m->queue_samples=m->queue_frac=m->draining=0;
        m->queue_tick=m->io.tick_25ms();m->last_tick=m->queue_tick-1u;
        return 0;
    }
    /* Playback-once latch for original RPG2000 conditional branch type 9.
     * IO-free: never seek/read to answer this per-frame query. Decoded EOF
     * may lead the hardware by the existing bounded PCM queue (<=1024 samples).
     * Does not claim a sample-accurate hardware playback-position query. */
    if(p[0]==7u) {
        bbvm_mix_channel_t *c;bb_u32 bytes;
        if(p[1]>1u)return BB_AUDIO_ARGUMENT;
        c=&m->channel[p[1]];bytes=c->bits/8u;
        p[3]=c->played_once;p[4]=bytes?c->data_bytes/bytes:0u;
        p[5]=(bb_u32)c->active;p[6]=c->active?c->volume:0u;p[7]=(bb_u32)c->loop;
        for(i=3u;i<8u;++i)(void)bbvm_write_u32(vm,address+i*4u,p[i]);
        return 0;
    }
    /* Separate restore operation leaves old op6's unused words compatible. */
    if(p[0]==8u) {
        if(p[1]>1u||p[3]>1u)return BB_AUDIO_ARGUMENT;
        m->channel[p[1]].played_once=p[3];return 0;
    }
    return BB_AUDIO_ARGUMENT;
}
