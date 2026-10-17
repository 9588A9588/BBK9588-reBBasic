/* SPDX-License-Identifier: GPL-3.0-or-later
 * Low-memory wave screen transition for native, pre-rotated RGB565 pages.
 * RenderFX 9: op,page,x,y,w,h,darkness0..256,phase0..65535; rest zero.
 * One 640-byte column scratch, fixed-point LUT, no allocations or IO.
 * Hardware-adapted wave+fade, not a claim of RPG_RT frame-exact equivalence. */
static s32 bbvm_screen_wave(const s32 *a) {
    u16 column[320],*dst;
    s32 x,y,w=a[4],h=a[5],shift,sy;
    u32 i,visible;
    if(a[1]<-1||a[1]>=PAGE_COUNT||(dst=page_surface(a[1]))==0)return -2;
    if(a[2]<0||a[3]<0||w<1||h<1||h>320||a[6]<0||a[6]>256||
        a[7]<0||a[7]>65535||a[2]>g.screen_width||w>g.screen_width-a[2]||
        a[3]>g.screen_height||h>g.screen_height-a[3])return -3;
    for(i=8u;i<21u;++i)if(a[i]!=0)return -3;
    visible=256u-(u32)a[6];
    for(x=0;x<w;++x) {
        for(y=0;y<h;++y)column[y]=dst[(a[3]+y)*g.screen_width+a[2]+x];
        shift=bbvm_pic_sin((u32)a[7]+(u32)(w-1-x)*1024u)*a[6]/131072;
        for(y=0;y<h;++y) {
            sy=y-shift;
            dst[(a[3]+y)*g.screen_width+a[2]+x]=sy<0||sy>=h?0:
                bbvm_fx_blend(column[sy],0,visible);
        }
    }
    if(a[1]<0)render_visible_region(a[2],a[3],w,h);
    return 0;
}
