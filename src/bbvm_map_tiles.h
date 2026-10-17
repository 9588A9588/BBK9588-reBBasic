/* SPDX-License-Identifier: MIT
 * RenderFX operation 8: bounded pre-rotated atlas batch, no IO/allocation.
 * Packet words: op,page,atlas,VM-records,count,camx,camy,AB,C,tile,
 * native viewport x,y,width,height,layer; remaining words reserved zero.
 * Records: world pixel x,y,atlas slot,kind(0=AB,1=C), four LE u32s.
 * Every animated cell has both a complete ground and a transparent high tile.
 * Validate the ENTIRE batch before writing the destination. */
static s32 bbvm_map_tiles(bbvm_t *vm, const s32 *a) {
    u32 base, count, n, stride, slots;
    s32 tile, left, top, right, bottom;
    picture_t *pic;
    u16 *dst;
    if (a[1]<-1 || a[1]>=PAGE_COUNT || (dst=page_surface(a[1]))==0) return -2;
    if (a[2]<0 || a[2]>=PICTURE_COUNT || g.pictures[a[2]].pixels==0) return -4;
    pic=&g.pictures[a[2]];
    tile=a[9];
    if (a[4]<0 || a[4]>512 || tile<1 || tile>32 ||
        a[5]<-65535 || a[5]>65535 || a[6]<-65535 || a[6]>65535 ||
        a[7]<0 || a[7]>2 || a[8]<0 || a[8]>3 ||
        a[10]<0 || a[11]<0 || a[12]<1 || a[13]<1 || a[14]<0 || a[14]>1 ||
        a[10]>g.screen_width || a[12]>g.screen_width-a[10] ||
        a[11]>g.screen_height || a[13]>g.screen_height-a[11] ||
        pic->height!=(u32)tile*4u || pic->width%(u32)tile!=0u) return -3;
    for (n=15u;n<21u;++n) if (a[n]!=0) return -3;
    base=(u32)a[3];count=(u32)a[4];
    if (base>vm->memory_size || count>(vm->memory_size-base)/16u) return -1;
    slots=pic->width/(u32)tile;
    for (n=0u;n<count;++n) {
        u32 p=base+n*16u;
        if (read32(vm,p)>65535u || read32(vm,p+4u)>65535u ||
            read32(vm,p+8u)>=slots || read32(vm,p+12u)>1u) return -3;
    }
    left=a[10];right=a[10]+a[12];
    top=a[11];bottom=a[11]+a[13];stride=(u32)g.screen_width;
    for (n=0u;n<count;++n) {
        u32 p=base+n*16u;
        u32 kind=read32(vm,p+12u);
        s32 dx=right-(s32)read32(vm,p+4u)+a[6]-tile;
        s32 dy=top+(s32)read32(vm,p)-a[5];
        s32 sx=(s32)pic->width-((s32)read32(vm,p+8u)+1)*tile;
        s32 sy=(kind&1u ? a[8] : a[7])*tile;
        s32 x0=dx>left ? dx : left, y0=dy>top ? dy : top;
        s32 x1=dx+tile<right ? dx+tile : right;
        s32 y1=dy+tile<bottom ? dy+tile : bottom;
        s32 x,y;
        if (x0>=x1 || y0>=y1) continue;
        for (y=y0;y<y1;++y) {
            const u16 *src=pic->pixels+(sy+y-dy)*pic->width+sx+x0-dx;
            u16 *out=dst+(u32)y*stride+(u32)x0;
            for (x=x0;x<x1;++x) {
                u16 pixel=*src++;
                if (pixel!=BDA_GUI_COLOR_KEY_MAGENTA_RGB565) *out=pixel;
                ++out;
            }
        }
    }
    if (a[1]<0) render_visible_region(left,top,right-left,bottom-top);
    return 0;
}
