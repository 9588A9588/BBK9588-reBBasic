/* SPDX-License-Identifier: GPL-3.0-or-later
 * Picture-local fixed-point transforms on offline rotated RGB565 assets. */
#ifndef BBVM_PICTURE_FX_H
#define BBVM_PICTURE_FX_H
#include "bbvm_fx.h"
static const short bbvm_pic_quarter[65]={0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,5139,5520,5897,6270,6639,7005,7366,7723,8076,8423,8765,9102,9434,9760,10080,10394,10702,11003,11297,11585,11866,12140,12406,12665,12916,13160,13395,13623,13842,14053,14256,14449,14635,14811,14978,15137,15286,15426,15557,15679,15791,15893,15986,16069,16143,16207,16261,16305,16340,16364,16379,16384};
static s32 bbvm_pic_sin(u32 phase) {
    u32 p=phase&65535u,t,i,f;
    s32 sign=1,v;
    if(p<16384u)t=p;
    else if(p<32768u)t=32768u-p;
    else if(p<49152u){t=p-32768u;sign=-1;}
    else {t=65536u-p;sign=-1;}
    i=t>>8;f=t&255u;
    v=bbvm_pic_quarter[i];
    if(i<64u)v+=(s32)((bbvm_pic_quarter[i+1u]-v)*(s32)f+128)/256;
    return v*sign;
}
static inline void bbvm_pic_put(u16 *d,u16 pixel,const s32 *a,const u32 *tone) {
    if(a[12] && pixel==BDA_GUI_COLOR_KEY_MAGENTA_RGB565)return;
    pixel=bbvm_fx_tone(pixel,tone[0],tone[1],tone[2],tone[3]);
    *d=a[11]==256?pixel:bbvm_fx_blend(pixel,*d,(u32)a[11]);
}
static s32 bbvm_pic_transform(u16 *surface,s32 stride,s32 rows,
                              const u16 *pixels,s32 sw,s32 sh,const s32 *a) {
    s32 zoom=a[0]==6?a[8]:(s32)((u32)a[8]>>16);
    s32 power=a[0]==7?(a[8]&65535):0;
    s32 w,h,bw,bh,x0,y0,x1,y1,x,y,c,s,ac,as,top,shift,depth;
    long long kx,ky,sx,sy,sx0,sy0,amp;
    u32 tone[4],i;
    if(a[7]<0 || a[7]>65535 || zoom<1 || zoom>10000 || power>51200 ||
        a[9]!=sw || a[10]!=sh)return -3;
    w=sw*zoom/100;h=sh*zoom/100;
    if(w<1 || h<1 || w>65535 || h>65535)return -3;
    for(i=0;i<4u;++i) {
        tone[i]=(u32)a[13+i]*128u/100u;
        if(tone[i]>255u)tone[i]=255u;
    }
    c=bbvm_pic_sin((u32)a[7]+16384u);s=bbvm_pic_sin((u32)a[7]);
    ac=c<0?-c:c;as=s<0?-s:s;
    if(a[0]==6) {
        bw=(s32)(((long long)ac*w+(long long)as*h+16383)/16384);
        bh=(s32)(((long long)as*w+(long long)ac*h+16383)/16384);
    } else {
        bw=w;depth=power/128;
        shift=(s32)((long long)zoom*depth*7/400)+1;
        bh=h+shift*2;
    }
    x0=a[3]-bw/2;y0=a[4]-bh/2;x1=x0+bw;y1=y0+bh;
    if(x0<a[17])x0=a[17];
    if(y0<a[18])y0=a[18];
    if(x1>a[17]+a[19])x1=a[17]+a[19];
    if(y1>a[18]+a[20])y1=a[18]+a[20];
    if(x0<0)x0=0;
    if(y0<0)y0=0;
    if(x1>stride)x1=stride;
    if(y1>rows)y1=rows;
    if(x1<=x0 || y1<=y0 || a[11]==0)return 0;
    if(a[0]==6) {
        /* Inverse Q16 coefficients: only adds/shifts in the visible-pixel loop. */
        kx=(long long)c*400/zoom;ky=(long long)s*400/zoom;
        sx0=(long long)(sw/2)*65536+(x0-a[3])*kx+(y0-a[4])*ky;
        sy0=(long long)(sh/2)*65536-(x0-a[3])*ky+(y0-a[4])*kx;
        for(y=y0;y<y1;++y) {
            sx=sx0;sy=sy0;
            for(x=x0;x<x1;++x) {
                if(sx>=0 && sy>=0 && sx<(long long)sw*65536 && sy<(long long)sh*65536)
                    bbvm_pic_put(surface+y*stride+x,pixels[(s32)(sy>>16)*sw+(s32)(sx>>16)],a,tone);
                sx+=kx;sy-=ky;
            }
            sx0+=ky;sy0+=kx;
        }
    } else {
        depth=power/128;
        amp=(long long)zoom*depth*7*256/400;
        top=a[3]+w-w/2;
        if(top>a[17]+a[19])top=a[17]+a[19];
        if(top>stride)top=stride;
        /* Original logical rows are native columns; wave along native Y. */
        for(x=x0;x<x1;++x) {
            u32 phase=(u32)a[7]+(u32)((long long)(top-1-x)*1638400/(7*zoom));
            s32 source_x=(x-(a[3]-w/2))*sw/w;
            s32 first,last,yy,source_y,delta=sh/h,remainder=sh%h,error;
            if(source_x<0 || source_x>=sw)continue;
            shift=(s32)(amp*bbvm_pic_sin(phase)/4194304);
            first=a[4]-h/2+shift;last=first+h;
            if(last>y1)last=y1;
            if(first<y0)first=y0;
            yy=first-(a[4]-h/2)-shift;
            source_y=yy*sh/h;error=yy*sh%h;
            for(y=first;y<last;++y) {
                bbvm_pic_put(surface+y*stride+x,pixels[source_y*sw+source_x],a,tone);
                source_y+=delta;error+=remainder;
                if(error>=h){++source_y;error-=h;}
            }
        }
    }
    return 0;
}
#endif
