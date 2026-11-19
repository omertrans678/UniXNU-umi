// SPDX-License-Identifier: BSD-2-Clause-Patent
// ST FTM5 wire format: LineageOS SM8250 71b13e62f057a649b77fe4062feb73ee72ad609c.
// Independent decoder. Coordinates and contact IDs stay in native portrait axes.
#pragma once
#include <stdint.h>
#include <string.h>
struct UmiTouchPoint { uint16_t x,y; uint8_t active; };
struct UmiTouchFrame { uint32_t sequence; UmiTouchPoint points[10]; };
enum { UMI_FTS_NONE=0, UMI_FTS_POINT=1, UMI_FTS_RESET=2, UMI_FTS_FAULT=3, UMI_FTS_BAD=4 };
// Native fts interrupt dispatch uses event[0] >> 4. The low nibble can
// carry contact-size bits; it is not a fixed 0x3 protocol signature.
static inline bool umi_fts_point_event(uint8_t code) {
    unsigned kind=code>>4;return kind>=1 && kind<=3;
}
static inline unsigned umi_fts_parse(UmiTouchFrame *f,const uint8_t e[8]) {
    if(!e[0])return UMI_FTS_NONE;
    if(e[0]==3 || e[0]==0xf3) {
        for(unsigned i=0;i<10;++i)f->points[i].active=0;
        return e[0]==3?UMI_FTS_RESET:UMI_FTS_FAULT;
    }
    if(!umi_fts_point_event(e[0]))return (e[0]&15)==3?UMI_FTS_NONE:UMI_FTS_BAD;
    unsigned id=e[1]>>4,type=e[1]&15;
    if(id>=10)return UMI_FTS_BAD;
    UmiTouchPoint *p=&f->points[id];
    if((e[0]>>4)==3 || (type!=1 && type!=2)) {
        unsigned changed=p->active;p->active=0;return changed?UMI_FTS_POINT:UMI_FTS_NONE;
    }
    unsigned x=((unsigned)(e[3]&15)<<8)|e[2],y=((unsigned)e[4]<<4)|(e[3]>>4);
    // umi-156: a contact at the panel edge reports one past the last pixel; clamp small overshoots, reject real garbage.
    if(x>1079+64 || y>2339+64)return UMI_FTS_BAD;
    if(x>1079)x=1079;
    if(y>2339)y=2339;
    unsigned changed=!p->active||p->x!=x||p->y!=y;
    p->x=(uint16_t)x;p->y=(uint16_t)y;p->active=1;return changed?UMI_FTS_POINT:UMI_FTS_NONE;
}
// Fixed HID collection per hardware ID: lifting ID 0 cannot overwrite ID 1's
// previous state. Every disappearing ID carries its own last coordinate.
static inline unsigned umi_fts_report(const UmiTouchFrame *previous,const UmiTouchFrame *next,uint8_t report[62]) {
    memset(report,0,62);report[0]=3;unsigned count=0;
    for(unsigned id=0;id<10;++id) {
        const UmiTouchPoint *p=next->points[id].active?&next->points[id]:&previous->points[id];
        uint8_t *out=report+1+id*6;out[1]=(uint8_t)id;
        if(!next->points[id].active && !previous->points[id].active)continue;
        unsigned x=(unsigned)p->x*32767/1079,y=(unsigned)p->y*32767/2339;
        // Bit 1 is TouchValid/confidence, including on the release report.
        out[0]=next->points[id].active?3:2;out[2]=(uint8_t)x;out[3]=(uint8_t)(x>>8);out[4]=(uint8_t)y;out[5]=(uint8_t)(y>>8);++count;
    }
    report[61]=(uint8_t)count;return count;
}
