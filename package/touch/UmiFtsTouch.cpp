// SPDX-License-Identifier: BSD-2-Clause-Patent
// Mi 10 ST FTM5 @ I2C 0x49. Dedicated worker owns transport, power and FIFO.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <stdarg.h>
#include "UmiTouchPlatform.h"
#include "UmiFtsCore.h"
#include "UmiFtsStartup.h"
#include "UmiFtsIdentity.h"
#include "UmiBoardBoost.h"
#include "UmiFtsRetry.h"
#include "UmiFtsSensing.h"
#include "UmiFtsSysInfo.h"
#include "UmiFtsFrameSample.h"
#include "UmiFtsPowerSequence.h"
#include "UmiFtsHardwareProbe.h"
#include "UmiFtsScanProbe.h"
#include "UmiFtsDataProbe.h"
#include "UmiFtsGpiCore.h"
extern "C" uint64_t gPhysBase,gPhysSize;
#include "UmiFtsLogLimits.h"
#include "UmiTouchLog.h"
extern "C" {
volatile uint32_t native_touch_seq,native_touch_count,native_touch_hid_reports,native_touch_loop_step;
volatile int native_touch_xs[12],native_touch_ys[12],native_touch_ids[12];
volatile unsigned native_touch_stage_req;
char native_touch_rows[4][120];
static volatile unsigned umiFtsBadEvents;static uint8_t umiFtsFirstBad[8];
extern char boot_progress_ext[8][48];
extern volatile uint32_t native_hid_stage,native_hid_resource_found,native_hid_input_events[8],native_touch_edge_stats[4];
extern volatile int native_hid_load_status,native_hid_matching_load_status,native_hid_input_results[8];
}
static uint64_t epoch;
static unsigned stage,failure,rawCount,pointCount,resetCount,chipErrors,queueDrops,transportErrors,lastResult,lastAck;
static uint8_t lastRaw[8];
static unsigned sensingRuns,sensingCompleted,sensingFailure,irqLowPolls,irqHighPolls,fifoEmpty,fifoNonempty;
static uint64_t sensingElapsedNs,maxReadNs;
static unsigned irqTransitions;static bool irqObserved,previousIrq;
static unsigned idleFifoReads;
static unsigned chargeAttempted,chargeResult;
static unsigned teHighPolls,teLowPolls,teTransitions;
static bool teObserved,tePrevious;
static UmiFtsSysInfo controllerInfo;
static UmiFtsFrameSample frameSample;
static unsigned seconds(void){return frequency?(unsigned)((GetPerformanceCounter()-epoch)/frequency):0;}
static UmiTouchLog debugLog;
static void touchLogLine(const char *line){IOLog("%s",line);}
static void touchFlush(void) {
    debugLog.flush(ml_get_interrupts_enabled() && !ml_at_interrupt_context(),touchLogLine);
}
static void touchDebug(unsigned,const char *format,...) {
    va_list a;va_start(a,format);debugLog.append(format,a);va_end(a);touchFlush();
}
struct Region {uintptr_t physical;unsigned length;IOMemoryMap *mapping;};
static Region boardSpmiWrite={0,0x1000,nullptr};
static Region regions[]={
 {0x00a94000,0x1000,nullptr},{0x0f500000,0x46000,nullptr},
 {0x0c440000,0x2000,nullptr},{0x0c40a000,0x1000,nullptr},
 {0x0e600000,0x10000,nullptr},{0x0c3f0000,0x1000,nullptr},
 {0x80860000,0x20000,nullptr},{0x18220000,0x3000,nullptr},
 {0x00a00000,0x70000,nullptr},{0x15000000,0xd0000,nullptr},
 {0x00118000,0x1000,nullptr},{0x00123000,0x1000,nullptr},{0x00152000,0x1000,nullptr},
 {0x0f942000,0x1000,nullptr}}; // Kona GPIO66 is NORTH: primary panel TE input.
static uintptr_t virtualAddress(uintptr_t physical,unsigned bytes) {
    if(boardSpmiWrite.mapping && physical>=boardSpmiWrite.physical &&
       bytes<=boardSpmiWrite.length && physical-boardSpmiWrite.physical<=boardSpmiWrite.length-bytes)
        return boardSpmiWrite.mapping->getVirtualAddress()+physical-boardSpmiWrite.physical;
    for(auto &r:regions)if(r.mapping && physical>=r.physical && physical-r.physical<=r.length-bytes)
        return r.mapping->getVirtualAddress()+physical-r.physical;
    panic("umi FTS unqualified MMIO %lx",(unsigned long)physical);
}
static uint32_t MmioRead32(uintptr_t address) {
    uint64_t value;uintptr_t mapped=virtualAddress(address,4);
    __asm__ volatile("ldr %w0,[%1]\n\tuxtw %0,%w0":"=&r"(value):"r"(mapped):"memory");return (uint32_t)value;
}
static uint16_t MmioRead16(uintptr_t address) {return (uint16_t)(MmioRead32(address&~(uintptr_t)3)>>((address&2)*8));}
static void MmioWrite32(uintptr_t address,uint32_t value) {
    uintptr_t mapped=virtualAddress(address,4);
    __asm__ volatile("dsb sy\n\tstr %w0,[%1]\n\tdsb sy"::"r"(value),"r"(mapped):"memory");
}
static bool prepareBoardSpmiWrite(unsigned sid,unsigned address,unsigned value,uintptr_t channel) {
    if(sid!=2 || address!=0xc444 || value!=0x80 || channel<0x0c600000 ||
       channel>=0x0e600000 || (channel&0xffff))return false;
    if(boardSpmiWrite.mapping)return channel==boardSpmiWrite.physical;
    IODeviceMemory *mem=IODeviceMemory::withRange(channel,boardSpmiWrite.length);
    if(!mem)return false;
    boardSpmiWrite.mapping=mem->map(kIOMapAnywhere|kIOMapInhibitCache);mem->release();
    if(!boardSpmiWrite.mapping)return false;
    boardSpmiWrite.physical=channel;return true;
}
#define UMI_SPMI_PREPARE_WRITE(sid,address,value,channel) prepareBoardSpmiWrite(sid,address,value,channel)
namespace spmi {
#include "UmiSpmi.inc"
}
#undef UMI_SPMI_PREPARE_WRITE
using spmi::UmiSpmiRead8;
namespace rpmh {
#include "UmiRpmh.inc"
}
using rpmh::UmiRpmhTouchEnable;using rpmh::UmiRpmhTouchDisable;using rpmh::UmiRpmhTouchOutstanding;
using rpmh::UmiRpmhTouchVoteSettled;
static void umiGpiDmaSync(void *data,unsigned bytes) {
    uintptr_t a=(uintptr_t)data&~uintptr_t(63),end=(uintptr_t)data+bytes;
    for(;a<end;a+=64)__asm__ volatile("dc civac,%0"::"r"(a):"memory");
    __asm__ volatile("dsb sy":::"memory");
}
#define UMI_FTS_NATIVE_GPI 1
namespace gpi {
#include "UmiFtsGpi.inc"
}
namespace bus {
#include "UmiFtsBus.inc"
}
// SPSC frame queue: producer publishes a complete frame; HID consumes in order.
// Overflow stops the controller and queues cancellation after all accepted frames.
static UmiTouchFrame frames[64],state;
static unsigned producer,consumer,cancelPending;
static UmiTouchFrame cancelFrame;
static bool queue(const UmiTouchFrame &f) {
    unsigned p=__atomic_load_n(&producer,__ATOMIC_RELAXED),c=__atomic_load_n(&consumer,__ATOMIC_ACQUIRE);
    if(p-c>=64){++queueDrops;return false;}
    frames[p%64]=f;__atomic_store_n(&producer,p+1,__ATOMIC_RELEASE);return true;
}
extern "C" bool umi_touch_next(UmiTouchFrame *frame) {
    unsigned c=__atomic_load_n(&consumer,__ATOMIC_RELAXED),p=__atomic_load_n(&producer,__ATOMIC_ACQUIRE);
    if(c==p){if(!__atomic_exchange_n(&cancelPending,0,__ATOMIC_ACQUIRE))return false;*frame=cancelFrame;return true;}
    *frame=frames[c%64];__atomic_store_n(&consumer,c+1,__ATOMIC_RELEASE);return true;
}
static bool publish(void) {
    ++state.sequence;
    uint32_t seq=__atomic_load_n(&native_touch_seq,__ATOMIC_RELAXED);
    __atomic_store_n(&native_touch_seq,seq+1,__ATOMIC_SEQ_CST);
    unsigned n=0;for(unsigned i=0;i<10;++i)if(state.points[i].active){
        native_touch_ids[n]=i;native_touch_xs[n]=state.points[i].x;native_touch_ys[n]=state.points[i].y;++n;
    }native_touch_count=n;__atomic_store_n(&native_touch_seq,seq+2,__ATOMIC_RELEASE);
    return queue(state);
}
// umi-168 unattended touch script (umi-touch-script=<driver seconds to start>, 0 = off): synthetic contacts on reserved
// ID 9 through the same publish() queue as the panel, plus Home presses through the HID mailbox. Steps: swipe up (unlock),
// tap the dock centre, Home, then three taps across the first icon row, each followed by Home. Results: umi_touch_script
// [0] steps done, [1] frames injected, [2] last step (ms since driver start), [3] Home presses asked.
extern "C" {volatile uint32_t umi_touch_script[4];extern volatile uint32_t native_hid_input_command;}
struct UmiScriptStep {uint32_t at_ms; uint8_t kind; uint16_t x0,y0,x1,y1; uint16_t dur_ms;};   // kind 1 swipe, 2 tap, 3 Home
static const UmiScriptStep kScript[]={
    {0,1,540,2320,540,1100,400},{6000,2,540,2200,0,0,80},{14000,3,0,0,0,0,0},
    {20000,2,200,350,0,0,80},{28000,3,0,0,0,0,0},{34000,2,540,350,0,0,80},{42000,3,0,0,0,0,0},
    {48000,2,880,350,0,0,80},{56000,3,0,0,0,0,0}};
static void scriptTick(uint64_t nowMs) {
    static int configured=-1;static uint64_t startMs;static unsigned step;static bool down;static uint16_t lastX,lastY;
    if(configured<0){unsigned at=0;PE_parse_boot_argn("umi-touch-script",&at,sizeof(at));configured=at?1:0;startMs=uint64_t(at)*1000;}
    if(!configured || step>=ARRAY_SIZE(kScript) || nowMs<startMs)return;
    const UmiScriptStep &k=kScript[step];const uint64_t rel=nowMs-startMs;
    if(rel<k.at_ms)return;
    UmiTouchPoint &p=state.points[9];
    if(k.kind==3){native_hid_input_command=1;umi_touch_script[3]=umi_touch_script[3]+1;}
    else {
        const uint64_t t=rel-k.at_ms;
        if(t<k.dur_ms){
            uint16_t x=k.kind==1?uint16_t(k.x0+(int(k.x1)-int(k.x0))*int64_t(t)/k.dur_ms):k.x0;
            uint16_t y=k.kind==1?uint16_t(k.y0+(int(k.y1)-int(k.y0))*int64_t(t)/k.dur_ms):k.y0;
            if(!down || x!=lastX || y!=lastY){p.x=x;p.y=y;p.active=1;down=true;lastX=x;lastY=y;
                if(publish())umi_touch_script[1]=umi_touch_script[1]+1;}
            return;
        }
        if(k.kind==1 && (lastX!=k.x1 || lastY!=k.y1)){p.x=k.x1;p.y=k.y1;p.active=1;if(publish())umi_touch_script[1]=umi_touch_script[1]+1;}
        p.active=0;down=false;if(publish())umi_touch_script[1]=umi_touch_script[1]+1;
    }
    umi_touch_script[2]=uint32_t(nowMs);++step;umi_touch_script[0]=step;
}
// Bounded logs: protocol quota plus one reserved stop, and64 contacts,
// plus periodic summaries. Separate quotas keep startup status from hiding fingers.
static uint64_t logs[UMI_FTS_LOG_LIMIT][16];static unsigned logPublished,logConsumed,rawQuota,pointQuota;
static void record(const uint8_t e[8],unsigned result,bool diagnostic=false) {
    bool point=!diagnostic && umi_fts_point_event(e[0]);
    if((!e[0]&&!diagnostic) || (point?pointQuota>=UMI_FTS_POINT_LIMIT:rawQuota>=(diagnostic?UMI_FTS_STOP_LIMIT:UMI_FTS_PROTOCOL_LIMIT)))return;
    unsigned i=logPublished;if(i>=UMI_FTS_LOG_LIMIT)return;
    point?++pointQuota:++rawQuota;
    uint64_t *v=logs[i];v[0]=1;v[1]=i+1;v[2]=seconds();v[3]=result;
    memcpy(&v[4],e,8);v[5]=lastAck;v[6]=stage;v[7]=state.sequence;v[8]=native_touch_count;
    for(unsigned id=0;id<10;++id)if(state.points[id].active)v[9]|=1U<<id;
    unsigned id=e[1]>>4;if(id<10){v[10]=id;v[11]=state.points[id].x;v[12]=state.points[id].y;}
    v[13]=transportErrors;v[14]=queueDrops;v[15]=native_touch_hid_reports;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
extern "C" unsigned umi_touch_log_peek(uint64_t values[16]) {
    unsigned n=__atomic_load_n(&logPublished,__ATOMIC_ACQUIRE);
    if(logConsumed>=n)return 0;memcpy(values,logs[logConsumed],sizeof(logs[0]));return logConsumed+1;
}
extern "C" void umi_touch_log_commit(unsigned id) {if(id==logConsumed+1)++logConsumed;}
static void recordBus(unsigned result,unsigned failures,bool retry,bool cancelled) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=2;v[1]=i+1;v[2]=seconds();v[3]=result;v[4]=bus::mLastPhase;v[5]=bus::mLastByte;
    v[6]=bus::mLastElapsedNs/1000;v[7]=bus::mLastStopResult;v[8]=bus::mLastLines;v[9]=lastAck;
    v[10]=bus::mLastGeni;v[11]=bus::mLastPinsValid;v[12]=failures;v[13]=cancelled;
    v[14]=bus::mLastMaxGapNs/1000;v[15]=retry;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordRecovery(unsigned failures,bool retry,bool cancelled) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=3;v[1]=i+1;v[2]=seconds();v[3]=bus::mRecoveryResult;v[4]=bus::mRecoveryPulses;
    v[5]=bus::mRecoveryElapsedNs/1000;v[6]=bus::mLastLines;v[7]=bus::mRecoveryLines;
    v[8]=bus::mLastPhase;v[9]=bus::mLastByte;v[10]=bus::High(39);
    v[11]=bus::mLastPinsValid;v[12]=bus::mLastGeni;v[13]=failures;v[14]=cancelled;v[15]=retry;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordSensing(void) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=4;v[1]=i+1;v[2]=seconds();v[3]=sensingRuns;v[4]=sensingCompleted;v[5]=sensingFailure;
    v[6]=sensingElapsedNs/1000;v[7]=bus::High(39);v[8]=irqLowPolls;v[9]=irqHighPolls;
    v[10]=fifoEmpty;v[11]=fifoNonempty;v[12]=bus::mLastElapsedNs/1000;v[13]=maxReadNs/1000;
    v[14]=MmioRead32(GPIO_CTL(39));v[15]=MmioRead32(GPIO_IO(39));
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    i=logPublished;++rawQuota;v=logs[i];
    v[0]=7;v[1]=i+1;v[2]=seconds();v[3]=irqTransitions;v[4]=irqLowPolls;v[5]=irqHighPolls;
    v[6]=bus::High(39);v[7]=bus::mSavedIrqCtl;v[8]=bus::mActiveIrqCtl;
    v[9]=MmioRead32(GPIO_CTL(39));v[10]=MmioRead32(SE13_BASE+0x40);v[11]=bus::PowerReady();
    v[12]=native_touch_loop_step;v[13]=fifoEmpty;v[14]=fifoNonempty;v[15]=maxReadNs/1000;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordController(const UmiFtsControllerRead &r,uint64_t elapsedNs) {
    // Retain only complete successful chunks; never log a partially read tail.
    for(unsigned offset=0;offset<r.bytesRead;offset+=88){
        if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
        unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
        unsigned length=r.bytesRead-offset;if(length>88)length=88;
        v[0]=5;v[1]=i+1;v[2]=seconds();v[3]=offset;v[4]=length;
        memcpy(v+5,r.bytes+offset,length);
        __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    }
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    UmiFtsSysInfo info=umi_fts_sysinfo(r.bytes,r.bytesRead);
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=6;v[1]=i+1;v[2]=seconds();
    v[3]=(r.bytesRead==208?1U:0U)|(info.valid?2U:0U)|(!r.crcResult?4U:0U);
    v[4]=r.sysResult;v[5]=r.bytesRead;v[6]=r.chunkMask;v[7]=r.crcResult;
    v[8]=r.crcResult?UINT64_MAX:r.crc;v[9]=bus::mSavedIrqCtl;
    v[10]=MmioRead32(GPIO_CTL(39));v[11]=irqTransitions;
    v[12]=info.valid?((uint64_t)info.config<<16)|info.fw:UINT64_MAX;
    v[13]=info.valid?info.svn:UINT64_MAX;
    v[14]=info.valid?((unsigned)info.panelAfe<<16)|((unsigned)info.cxAfe<<8)|info.cfgAfe:UINT64_MAX;
    v[15]=elapsedNs/1000;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordFrameSample(unsigned number,uint64_t elapsedNs) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=8;v[1]=i+1;v[2]=seconds();v[3]=number;v[4]=frameSample.result;v[5]=frameSample.validMask;
    for(unsigned n=0;n<3;++n){
        v[6+n]=frameSample.addresses[n];
        if(frameSample.validMask&(1U<<n))memcpy(v+9+n*2,frameSample.data[n],16);
    }
    v[15]=elapsedNs/1000;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordNativeBoard(unsigned phase,unsigned checkResult) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=23;v[1]=i+1;v[2]=seconds();v[3]=phase;v[4]=resetCount;v[5]=checkResult;
    // Native dsi_panel_mi.c drives GPIO50 output-high. Observe the inherited
    // state only: changing panel controls would confound the reset comparison.
    v[6]=MmioRead32(GPIO_CTL(50));v[7]=MmioRead32(GPIO_IO(50));
    v[8]=MmioRead32(GPIO_CTL(38));v[9]=MmioRead32(GPIO_IO(38));
    v[10]=MmioRead32(GPIO_CTL(69));v[11]=MmioRead32(GPIO_IO(69));
    v[12]=MmioRead32(GPIO_CTL(39));v[13]=MmioRead32(GPIO_IO(39));
    v[14]=unsigned(bus::mCycleEnabled)|(unsigned(bus::mFullCycleEnabled)<<1);
    v[15]=3; // Required READY count before the initial sensing commands.
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordBringup(unsigned phase,unsigned result,uint64_t elapsedNs) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    uint8_t value[4]={};unsigned valid=0;
    const uint16_t address[]={0x4046,0x4008,0x4042,0x4043};
    // Keep SPMI diagnostics out of the reset-release -> READY deadline.
    if((phase&255)!=2)for(unsigned n=0;n<4;++n)if(!UmiSpmiRead8(bus::mSupplySid,address[n],value+n))valid|=1U<<n;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=9;v[1]=i+1;v[2]=seconds();v[3]=phase;v[4]=resetCount;v[5]=result;v[6]=elapsedNs/1000;
    v[7]=valid;v[8]=value[0];v[9]=value[1];v[10]=unsigned(value[2])|(unsigned(value[3])<<8);
    v[11]=MmioRead32(GPIO_CTL(38));v[12]=MmioRead32(GPIO_IO(38));
    v[13]=MmioRead32(GPIO_CTL(69));v[14]=MmioRead32(GPIO_IO(69));v[15]=MmioRead32(GPIO_IO(39));
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordPowerCycle(void) {
    if(bus::mFullCycleRequested && rawQuota<UMI_FTS_PROTOCOL_LIMIT && logPublished<UMI_FTS_LOG_LIMIT){
        unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];auto &r=bus::mFullCycle;
        v[0]=21;v[1]=i+1;v[2]=seconds();v[3]=r.result;v[4]=r.flags;v[5]=bus::mFullCycleElapsedNs/1000;
        const UmiFtsSupplyState samples[]={r.before,r.off,r.on};
        for(unsigned n=0;n<3;++n){auto &s=samples[n];
            v[6+n]=s.enable|(uint64_t(s.status)<<8)|(uint64_t(s.mv)<<16)|(uint64_t(s.valid)<<32)|(uint64_t(s.result)<<40);}
        v[9]=bus::mSupplyEnable;v[10]=bus::mSupplySid;v[11]=MmioRead32(GPIO_CTL(38));
        v[12]=MmioRead32(GPIO_IO(38));v[13]=MmioRead32(GPIO_CTL(69));v[14]=MmioRead32(GPIO_IO(69));v[15]=UmiRpmhTouchVoteSettled();
        __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    }
    if(!bus::mCycleRequested || rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=12;v[1]=i+1;v[2]=seconds();v[3]=bus::mCycleRequested;
    v[4]=bus::mAvddCycle.result;v[5]=bus::mAvddCycle.flags;v[6]=bus::mCycleElapsedNs/1000;
    v[7]=bus::mPowerCtl[0];v[8]=bus::mPowerOut[0];v[9]=bus::mPowerCtl[1];v[10]=bus::mPowerOut[1];
    v[11]=bus::mAvddCycle.lowLevels;v[12]=bus::mAvddCycle.highLevels;
    v[13]=20;v[14]=15;v[15]=MmioRead32(SE13_BASE+0x40);
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static int transfer(const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn);
static void recordDataPath(unsigned sample) {
    // Attempt73 recovered the full engine image. Reserve those slots for the
    // two-rail restart and retain the paired raw/filter/baseline reads at30s.
    if(sample!=2)return;
    unsigned pages=sample==1?2:3;
    for(unsigned n=0;n<pages;++n){
        if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT || !bus::ControlReady())return;
        auto read=[](const uint8_t*w,unsigned wn,uint8_t*r,unsigned rn){return transfer(w,wn,r,rn);};
        uint64_t values[16]={};values[1]=logPublished+1;values[2]=seconds();
        if(sample==1){
            UmiFtsEnginePage p=umi_fts_engine_page(controllerInfo,n,read);
            values[0]=19;values[3]=p.address;values[4]=p.length|(uint64_t(p.result)<<32);
            if(!p.result)memcpy(values+5,p.data,p.length);
        }else{
            uint64_t started=GetPerformanceCounter();
            UmiFtsDataProbe p=umi_fts_data_probe(controllerInfo,n,read);
            values[0]=20;values[3]=n;values[4]=p.result;values[5]=p.validMask;
            values[6]=p.addresses[0]|(uint64_t(p.addresses[1])<<16)|(uint64_t(p.addresses[2])<<32);
            values[7]=GetTimeInNanoSecond(GetPerformanceCounter()-started)/1000;
            memcpy(values+8,p.data,sizeof(p.data));
        }
        unsigned i=logPublished;++rawQuota;memcpy(logs[i],values,sizeof(values));
        __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    }
}
static void recordScan(unsigned number) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT || !bus::ControlReady())return;
    uint64_t started=GetPerformanceCounter();
    UmiFtsScanProbe r=umi_fts_scan_probe(controllerInfo,
        [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);});
    uint8_t mode=0;unsigned modeValid=UmiSpmiRead8(bus::mSupplySid,0x4045,&mode)==0;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=18;v[1]=i+1;v[2]=seconds();v[3]=number;v[4]=r.result;v[5]=r.validMask;
    for(unsigned n=0;n<4;++n)memcpy(v+6+n,r.data[n],umi_fts_scan_lengths[n]);
    memcpy(v+10,r.data[4],16);
    v[12]=MmioRead32(0x0f942000)|(uint64_t(MmioRead32(0x0f942004))<<32);
    v[13]=teTransitions;v[14]=teHighPolls|(uint64_t(teLowPolls)<<32);
    v[15]=mode|(modeValid<<8)|((GetTimeInNanoSecond(GetPerformanceCounter()-started)/1000)<<16);
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordHardware(unsigned number) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT || !bus::ControlReady())return;
    uint64_t started=GetPerformanceCounter();
    UmiFtsHardwareProbe r=umi_fts_hardware_probe(
        [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);});
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=13;v[1]=i+1;v[2]=seconds();v[3]=number;
    // Read-only GENI mode/capability registers from qcom-geni-se.h.
    // A GSI-only engine cannot be replaced with an assumed FIFO driver.
    v[4]=MmioRead32(SE13_BASE+0x40);v[5]=MmioRead32(SE13_BASE+0x64);
    v[6]=MmioRead32(SE13_BASE+0x68);v[7]=MmioRead32(SE13_BASE+0xe24);
    v[8]=MmioRead32(SE13_BASE+0xe28);v[9]=MmioRead32(SE13_BASE+0x60);
    v[10]=MmioRead32(SE13_BASE+0x7c);v[11]=r.result;v[12]=r.validMask;
    memcpy(v+13,r.values,8);v[14]=bus::mLastMaxGapNs/1000;
    v[15]=GetTimeInNanoSecond(GetPerformanceCounter()-started)/1000;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
extern "C" void umi_touch_snapshot(uint64_t v[32]) {
    memset(v,0,32*sizeof(*v));v[0]=1;v[1]=stage;v[2]=failure;v[3]=rawCount;v[4]=pointCount;v[5]=resetCount;
    v[6]=chipErrors;v[7]=transportErrors;v[8]=lastResult;v[9]=lastAck;memcpy(v+10,lastRaw,8);
    v[11]=native_touch_count;v[12]=native_touch_seq;v[13]=queueDrops;v[14]=native_hid_stage;
    v[15]=(uint32_t)native_hid_load_status;v[16]=(uint32_t)native_hid_matching_load_status;v[17]=native_hid_resource_found;
    v[18]=native_hid_input_events[2];v[19]=native_hid_input_events[6];v[20]=(uint32_t)native_hid_input_results[6];
    v[21]=native_touch_hid_reports;v[22]=native_touch_loop_step;v[23]=producer-consumer;
    for(unsigned i=0;i<4;++i)v[24+i]=native_touch_edge_stats[i];
    v[28]=state.sequence;v[29]=logPublished;v[30]=seconds();v[31]=native_hid_input_events[7];
    snprintf(boot_progress_ext[4],48,"FTS s=%u e=%u raw=%u pt=%u bad=%u %02x%02x%02x%02x",stage,failure,rawCount,pointCount,
             umiFtsBadEvents,umiFtsFirstBad[0],umiFtsFirstBad[1],umiFtsFirstBad[2],umiFtsFirstBad[3]);
    snprintf(boot_progress_ext[5],48,"TOUCH n=%u q=%u drop=%u ack=%x",native_touch_count,producer-consumer,queueDrops,lastAck);
    snprintf(boot_progress_ext[6],48,"HID st=%u start=%u rpt=%u rc=%x",native_hid_stage,native_hid_input_events[2],native_touch_hid_reports,(unsigned)native_hid_input_results[6]);
    snprintf(boot_progress_ext[7],48,"EDGE on=%u start=%u edge=%u",native_touch_edge_stats[0],native_touch_edge_stats[1],native_touch_edge_stats[2]);
    snprintf(native_touch_rows[0],120,"FTS stage=%u failure=%u raw=%u points=%u I2C errors=%u last=%02x/%02x",stage,failure,rawCount,pointCount,transportErrors,lastRaw[0],lastRaw[1]);
}
static bool isUmi(void) {
    IORegistryEntry *root=IORegistryEntry::fromPath("/product",gIODTPlane);if(!root)return false;
    OSData *model=OSDynamicCast(OSData,root->getProperty("model"));const char expected[]="Qualcomm Technologies, Inc. xiaomi umi";
    bool match=model && model->getLength()==sizeof(expected) && !memcmp(model->getBytesNoCopy(),expected,sizeof(expected));root->release();return match;
}
static int transfer(const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn) {
    // Only this worker drives the bus. Do not hold an IRQ mask over I2C.
    int rc=bus::Transfer(w,wn,r,rn);lastResult=rc;lastAck=bus::mAckMask;if(rc)++transportErrors;return rc;
}
static void recordGpiClock(void) {
    if(!bus::mNative || rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=16;v[1]=i+1;v[2]=seconds();v[3]=gpi::clockStep;v[4]=gpi::result;v[5]=MmioRead32(0x152008);
    for(unsigned n=0;n<2;++n)v[6+n]=uint64_t(gpi::clockBranches[2*n])|(uint64_t(gpi::clockBranches[2*n+1])<<32);
    for(unsigned n=0;n<4;++n)v[8+n]=uint64_t(gpi::clockLevels[2*n])|(uint64_t(gpi::clockLevels[2*n+1])<<32);
    v[12]=MmioRead32(0x118600);v[13]=MmioRead32(0x118604);v[14]=MmioRead32(0x118614);v[15]=gpi::clockLevel;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordGpiSmmu(void) {
    if(!bus::mNative || gpi::phase<4 || rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];const auto &p=gpi::smmuPlan;
    v[0]=17;v[1]=i+1;v[2]=seconds();v[3]=gpi::smmuResult;
    for(unsigned n=0;n<3;++n)v[4+n]=p.id[n];
    v[7]=p.bank;v[8]=p.oldSctlr;
    for(unsigned n=0;n<2;++n){v[9+n]=p.usedMask[n];v[11+n]=p.enabledMask[n];v[13+n]=p.nonzeroMask[n];}
    v[15]=uint64_t(p.slot[0])|(uint64_t(p.slot[1])<<32);
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordGpi(unsigned reason) {
    if(!bus::mNative || rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=14;v[1]=i+1;v[2]=seconds();v[3]=reason;v[4]=gpi::phase;v[5]=gpi::result;
    v[6]=gpi::instance;v[7]=gpi::physical;v[8]=gpi::smmuResult;
    v[9]=(gpi::smmuPlan.bank&0xffff)|(uint64_t(gpi::smmuPlan.slot[0]&0xffff)<<16)|(uint64_t(gpi::smmuPlan.slot[1]&0xffff)<<32);
    v[10]=gpi::clockLevel;v[11]=gpi::transactions;v[12]=gpi::completed;
    v[13]=uint64_t(gpi::lastEvent.d[0])|(uint64_t(gpi::lastEvent.d[1])<<32);
    v[14]=uint64_t(gpi::lastEvent.d[2])|(uint64_t(gpi::lastEvent.d[3])<<32);
    v[15]=gpi::base?(gpi::state(0)|(gpi::state(0x80)<<4)|(gpi::state(0x1000)<<8)):0;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    touchDebug(0,"UMI GPI reason=%u phase=%u result=%u instance=%u dma=%llx smmu=%u bank=%u slots=%u/%u level=%u txns=%u completed=%x error=%x global=%x general=%x se_irq=%x\n",
        reason,gpi::phase,gpi::result,gpi::instance,gpi::physical,gpi::smmuResult,gpi::smmuPlan.bank,
        gpi::smmuPlan.slot[0],gpi::smmuPlan.slot[1],gpi::clockLevel,gpi::transactions,gpi::completed,
        gpi::errorLog,gpi::globalIrq,gpi::generalIrq,gpi::seIrq);
    if((reason || !gpi::transactions) && rawQuota<UMI_FTS_PROTOCOL_LIMIT && logPublished<UMI_FTS_LOG_LIMIT){
        i=logPublished;++rawQuota;v=logs[i];v[0]=15;v[1]=i+1;v[2]=seconds();
        v[3]=MmioRead32(SE13_BASE+0x40);v[4]=gpi::seIrq;v[5]=gpi::errorLog;
        v[6]=gpi::globalIrq;v[7]=gpi::generalIrq;v[8]=MmioRead32(0x15000048);
        v[9]=gpi::smmuPlan.pages && gpi::smmuPlan.bank<gpi::smmuPlan.banks?MmioRead32(0x15000000+(gpi::smmuPlan.pages+gpi::smmuPlan.bank)*4096+0x58):UINT64_MAX;
        v[10]=MmioRead32(0x152008);v[11]=MmioRead32(0x118604);v[12]=MmioRead32(0x118614);
        v[13]=MmioRead32(0x1185fc);v[14]=0;v[15]=0;
        if(gpi::phase>=2)for(unsigned n=0;n<6;++n){uintptr_t b=0xa1a000+n*0x4000;
            v[14]|=uint64_t(((MmioRead32(b)>>20)&15)|(((MmioRead32(b+0x80)>>20)&15)<<4))<<(n*8);
            v[15]|=uint64_t((MmioRead32(b+0x1000)>>20)&15)<<(n*4);}
        __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    }
}
static void stop(unsigned why) {
    recordGpi(why);
    failure=why;stage=90;touchDebug(0,"UMI FTS stopped reason=%u transfer=%u ack=%x\n",why,lastResult,lastAck);
    // Preserve the actual last FIFO bytes even when no FIFO read succeeded.
    // Result 0x100|reason denotes a driver stop, not a fabricated controller event.
    record(lastRaw,0x100|why,true);
    for(auto &p:state.points)p.active=0;
    // A separate terminal mailbox survives a full queue and a late HID attach.
    // Consumer drains accepted frames first, then the explicit all-up state.
    if(!publish()){cancelFrame=state;__atomic_store_n(&cancelPending,1,__ATOMIC_RELEASE);}
    if(!bus::mNative || gpi::shutdown()){bus::ReleasePins();bus::RestorePower();}
    touchFlush();
}
static bool recordModes(unsigned phase) {
    uint64_t started=GetPerformanceCounter();
    UmiFtsModes modes=umi_fts_read_modes(
        [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);});
    if(rawQuota<UMI_FTS_PROTOCOL_LIMIT && logPublished<UMI_FTS_LOG_LIMIT){
        unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
        v[0]=24;v[1]=i+1;v[2]=seconds();v[3]=phase;
        v[4]=modes.modeResult;v[5]=modes.gripResult;v[6]=modes.validMask;
        memcpy(v+7,modes.mode,8);memcpy(v+8,modes.grip,8);
        v[9]=chargeAttempted;v[10]=chargeAttempted?chargeResult:UINT64_MAX;
        v[11]=0; // Observed native NOT_CHARGING default, not a PMIC measurement.
        v[12]=GetTimeInNanoSecond(GetPerformanceCounter()-started)/1000;
        v[13]=pointCount;v[14]=native_touch_hid_reports;v[15]=bus::High(39);
        __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    }
    return !modes.modeResult && !modes.gripResult;
}
static void recordIdentity(const UmiFtsIdentity &r,uint64_t elapsedNs) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=25;v[1]=i+1;v[2]=seconds();v[3]=r.result;v[4]=r.flags;
    memcpy(v+5,r.before,8);memcpy(v+6,r.after,8);memcpy(v+7,r.identity,8);memcpy(v+8,r.event,8);
    v[9]=r.writeResult;v[10]=r.readResult;v[11]=r.polls;v[12]=elapsedNs/1000;
    v[13]=0x70;v[14]=resetCount;v[15]=0; // Zero NVM writes.
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static void recordBoardRail(unsigned phase) {
    if(rawQuota>=UMI_FTS_PROTOCOL_LIMIT || logPublished>=UMI_FTS_LOG_LIMIT)return;
    // xiaomi-sm8250-common.dtsi: always-on vdd_boost_vreg, PM8150B GPIO5.
    // Its connection to FTS AVDD is unknown. This capture itself is read-only.
    // Registers follow pinctrl-spmi-gpio.c, including LV/MV output-source.
    const uint16_t addresses[]={0x0104,0x0105,0xc404,0xc405,0xc410,0xc440,
                               0xc441,0xc442,0xc443,0xc444,0xc445,0xc446};
    uint8_t bytes[16]={};unsigned valid=0,result=0;uint64_t start=GetPerformanceCounter();
    for(unsigned n=0;n<ARRAY_SIZE(addresses);++n){uint8_t value=0;
        unsigned rc=UmiSpmiRead8(2,addresses[n],&value);
        if(!rc){bytes[n]=value;valid|=1U<<n;}else if(!result)result=rc;}
    unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
    v[0]=26;v[1]=i+1;v[2]=seconds();v[3]=result;v[4]=valid;v[5]=2;v[6]=5;v[7]=0xc400;
    memcpy(v+8,bytes,16);v[10]=ARRAY_SIZE(addresses);v[11]=GetTimeInNanoSecond(GetPerformanceCounter()-start)/1000;
    v[12]=phase;v[13]=MmioRead32(GPIO_CTL(69));v[14]=MmioRead32(GPIO_IO(69));v[15]=0;
    __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
}
static bool enableBoardBoost(void) {
    uint64_t start=GetPerformanceCounter();
    UmiBoardBoost r=umi_board_boost_enable(
        [](uint8_t sid,uint16_t addr,uint8_t *v){return UmiSpmiRead8(sid,addr,v);},
        [](){return spmi::UmiSpmiBoardBoostHigh();},
        [](unsigned us){MicroSecondDelay(us);});
    if(rawQuota<UMI_FTS_PROTOCOL_LIMIT && logPublished<UMI_FTS_LOG_LIMIT){
        unsigned i=logPublished;++rawQuota;uint64_t *v=logs[i];
        v[0]=27;v[1]=i+1;v[2]=seconds();v[3]=r.result;v[4]=r.readResult;v[5]=r.writeResult;
        v[6]=r.writes;v[7]=r.before;v[8]=r.after;v[9]=r.flags;
        v[10]=GetTimeInNanoSecond(GetPerformanceCounter()-start)/1000;
        v[11]=spmi::mBoardWriteApid;v[12]=spmi::mBoardWriteOwner;v[13]=spmi::mBoardWriteChannel;
        v[14]=r.delayUs;v[15]=0;
        __atomic_store_n(&logPublished,i+1,__ATOMIC_RELEASE);
    }
    return r.result==0;
}
static void worker(void *,wait_result_t) {
    thread_set_thread_name(current_thread(),"umi_fts");stage=2;
    if(!isUmi()){failure=1;stage=90;return;}
    for(auto &r:regions){IODeviceMemory *mem=IODeviceMemory::withRange(r.physical,r.length);
        if(mem){r.mapping=mem->map(kIOMapAnywhere|kIOMapInhibitCache);mem->release();}
        if(!r.mapping){failure=2;stage=90;return;}}
    // Ordinary native boot, not the previous power-cycle/software-reset trials.
    bus::mCycleEnabled=false;bus::mFullCycleEnabled=false;
    recordNativeBoard(0,0); // Before touch pin/power ownership.
    recordBoardRail(0);
    // Native board regulator is always-on, enabled before FTS supply/reset.
    // Retain on touch failure too; never change its inherited VIN or drive mode.
    if(!enableBoardBoost()){stop(24);return;}
    unsigned native=0;
    bus::mNative=PE_parse_boot_argn("umi-fts-gpi",&native,sizeof(native)) && native==1;
    stage=3;
    if(bus::mNative){bool ok=gpi::initialize();gpi::capture();recordGpiClock();recordGpiSmmu();recordGpi(0);if(!ok){stop(19);return;}}
    bus::Snapshot();int rc=bus::AcquirePins();lastResult=rc;recordPowerCycle();touchFlush();
    if(rc){stop(3);return;}stage=4;
    uint8_t command=0x86,e[8];bool scan=false;unsigned consecutiveFailures=0,slowRecords=0,timingRecords=0;
    uint64_t lastFifo=GetPerformanceCounter();
    uint64_t sensingStarted=0,chargeStarted=0;bool chargeObserved=false;
    unsigned healthRecord=0;const unsigned healthTimes[]={10,60,120,180,240};
    unsigned sampleNumber=0;const unsigned sampleTimes[]={5,30,90};uint64_t sampleStarted=0;
    for(;;){
        unsigned sleepMs=2;
        native_touch_loop_step=native_touch_loop_step+1;
        // Passive, approximately 2 ms sampling. Edges prove sampled activity;
        // zero edges cannot establish that narrow TE pulses are absent.
        if(stage==6){bool te=(MmioRead32(0x0f942004)&1)!=0;
            if(teObserved && te!=tePrevious)++teTransitions;
            teObserved=true;tePrevious=te;if(te)++teHighPolls;else ++teLowPolls;}
        bool irq=bus::High(39);
        if(irqObserved && irq!=previousIrq)++irqTransitions;irqObserved=true;previousIrq=irq;
        if(irq)++irqHighPolls;else ++irqLowPolls;
        // At most eight records per pass. Keep the complete FIFO order and
        // yield between batches; a busy touch source cannot monopolize a CPU.
        // Original low-level IRQ handler reads FIFO only when events are ready.
        // A bounded 100 ms fallback distinguishes a quiet IRQ from a quiet FIFO.
        bool idleRead=stage==6 && irq && GetTimeInNanoSecond(GetPerformanceCounter()-lastFifo)>=100000000;
        bool readFifo=stage!=6 || !irq || idleRead;
        if(readFifo){lastFifo=GetPerformanceCounter();if(idleRead)++idleFifoReads;}
        for(unsigned n=0;readFifo && n<8;++n){
            if(stage==4 && umi_fts_ready_expired(GetTimeInNanoSecond(GetPerformanceCounter()-bus::mReadyStart))){stop(11);return;}
            rc=transfer(&command,1,e,8);
            if(rc){
                if(stage==4 && rc==EFI_NO_RESPONSE &&
                   !umi_fts_ready_expired(GetTimeInNanoSecond(GetPerformanceCounter()-bus::mReadyStart)))break;
                ++consecutiveFailures;
                bool recoveryAttempted=stage==6 && consecutiveFailures<3 && bus::RecoveryEligible(rc);
                bool recovered=recoveryAttempted && bus::RecoverRead(rc)==EFI_SUCCESS;
                bool retry=!bus::mNative && stage==6 && umi_fts_retry_read(rc,consecutiveFailures,
                    recovered || bus::mLastStopResult==EFI_SUCCESS,recovered || bus::mLastLines==3,
                    bus::mLastPinsValid,(bus::mLastGeni & GENI_BUSY_MASK)==0);
                bool cancelled=false;
                if(retry) {
                    // A partial FIFO read may consume a contact edge. Release
                    // existing contacts before accepting any subsequent event.
                    for(auto &point:state.points)if(point.active){point.active=0;cancelled=true;}
                    if(cancelled && !publish()){recordBus(rc,consecutiveFailures,false,true);stop(6);return;}
                }
                recordBus(rc,consecutiveFailures,retry,cancelled);
                if(recoveryAttempted)recordRecovery(consecutiveFailures,retry,cancelled);
                if(retry){sleepMs=2;break;}
                stop(4);return;
            }
            consecutiveFailures=0;
            if(bus::mLastElapsedNs>maxReadNs)maxReadNs=bus::mLastElapsedNs;
            if(stage==6 && timingRecords<2){recordBus(0,0,false,false);++timingRecords;}
            if(e[0])++fifoNonempty;else ++fifoEmpty;
            // Preserve a few successful transactions above the old 8 ms limit.
            if(bus::mLastElapsedNs>8000000 && slowRecords<4){recordBus(0,0,false,false);++slowRecords;}
            memcpy(lastRaw,e,8);if(e[0])++rawCount;
            unsigned decoded=umi_fts_startup_event(&state,e,stage==4,
                GetTimeInNanoSecond(GetPerformanceCounter()-bus::mReadyStart));
            if(decoded==UMI_FTS_READY_TIMEOUT){record(e,decoded);stop(11);return;}
            if(decoded==UMI_FTS_WAIT_READY){record(e,decoded);break;}
            if(decoded==UMI_FTS_RESET){++resetCount;if(resetCount>3){record(e,decoded);stop(5);return;}scan=true;stage=5;publish();}
            if(decoded==UMI_FTS_POINT){++pointCount;if(!publish()){record(e,decoded);stop(6);return;}}
            if(decoded==UMI_FTS_FAULT){++chipErrors;record(e,decoded);stop(7);return;}
            // umi-156: an event this decoder does not know (status, gesture, out-of-range contact) used to stop the driver for good
            // after the unlock swipe; count it, keep the first one's bytes, and go on reading.
            if(decoded==UMI_FTS_BAD){if(!umiFtsBadEvents)memcpy(umiFtsFirstBad,e,8);++umiFtsBadEvents;}
            record(e,decoded);
            if(scan){
                if(resetCount<3){
                    uint64_t started=GetPerformanceCounter();
                    if(resetCount==1){
                        UmiFtsControllerRead controller=umi_fts_read_controller(
                            [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);});
                        controllerInfo=umi_fts_sysinfo(controller.bytes,controller.bytesRead);
                        recordController(controller,GetTimeInNanoSecond(GetPerformanceCounter()-started));
                        unsigned check=umi_fts_native_identity_check(controller,controllerInfo);
                        recordNativeBoard(1,check);
                        if(check){stop(20);return;}
                        if(bus::mLastLines!=3){stop(15);return;}
                        recordBringup(1,0,0);
                        // Native probe reads factory module record before the
                        // CRC-check and post-version GPIO resets. It loads a
                        // read-only record into host RAM, not a flash update.
                        started=GetPerformanceCounter();
                        UmiFtsIdentity identity=umi_fts_read_identity(
                            [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);},
                            [](unsigned ms){IOSleep(ms);},
                            [](){return GetTimeInNanoSecond(GetPerformanceCounter());});
                        recordIdentity(identity,GetTimeInNanoSecond(GetPerformanceCounter()-started));
                        if(identity.result){stop(23);return;}
                    }
                    // READY #1 -> CRC-check reset; READY #2 -> post-version
                    // reset. Both use umi's actual GPIO, never FA software reset.
                    started=GetPerformanceCounter();
                    unsigned resetResult=bus::ResetController();
                    recordBringup(2,resetResult,GetTimeInNanoSecond(GetPerformanceCounter()-started));
                    if(resetResult){stop(16);return;}
                    scan=false;stage=4;break;
                }
                if(!sensingRuns){
                    recordBringup(3,0,GetTimeInNanoSecond(GetPerformanceCounter()-bus::mReadyStart));
                    recordNativeBoard(3,0);
                }
                ++sensingRuns;uint64_t started=GetPerformanceCounter();
                sensingFailure=umi_fts_start_sensing(sensingRuns==1,
                    [](const uint8_t *w,unsigned n){return transfer(w,n,nullptr,0);},
                    [](unsigned ms){IOSleep(ms);},&sensingCompleted);
                sensingElapsedNs=GetTimeInNanoSecond(GetPerformanceCounter()-started);
                recordSensing();
                if(sensingFailure){stop(sensingFailure);return;}scan=false;stage=6;
                sensingStarted=GetPerformanceCounter();
                if(!recordModes(1)){stop(21);return;}
            }
            if(!e[0] || !(e[7]&31))break;
        }
        // Native power-supply notification follows successful sensing startup.
        // Run once, serialized with FIFO, with before/after mode evidence.
        // Do not overwrite a later mode or disturb an active contact.
        if(stage==6 && !chargeAttempted && !native_touch_count && bus::High(39) &&
           GetTimeInNanoSecond(GetPerformanceCounter()-sensingStarted)>=500000000){
            if(!recordModes(2)){stop(21);return;}
            chargeAttempted=1;
            chargeResult=umi_fts_default_charge_mode(
                [](const uint8_t *w,unsigned n){return transfer(w,n,nullptr,0);});
            chargeStarted=GetPerformanceCounter();
            if(chargeResult){recordModes(3);stop(22);return;}
        }
        if(stage==6 && chargeAttempted && !chargeObserved &&
           GetTimeInNanoSecond(GetPerformanceCounter()-chargeStarted)>=50000000){
            chargeObserved=true;if(!recordModes(3)){stop(21);return;}
        }
        // Passive reads only: active LOAD_DATA diagnostics can block and
        // must not perturb normal input startup.
        if(stage==6 && !native_touch_count && bus::High(39) && sampleNumber<ARRAY_SIZE(sampleTimes)){
            bool ready=false;
            if(!frameSample.active && seconds()>=sampleTimes[sampleNumber]){
                sampleStarted=GetPerformanceCounter();
                if(!pointCount && !native_touch_hid_reports){
                    // Mode parity was captured before/after the startup command.
                    // Keep this slot for the board-boost enable/readback record.
                    recordScan(sampleNumber+1);
                    if(!bus::ControlReady()){stop(18);return;}
                }
                if(!frameSample.begin(controllerInfo))ready=true;
            }
            if(frameSample.active)ready=frameSample.step(
                [](const uint8_t *w,unsigned wn,uint8_t *r,unsigned rn){return transfer(w,wn,r,rn);});
            if(ready){recordFrameSample(++sampleNumber,GetTimeInNanoSecond(GetPerformanceCounter()-sampleStarted));
                if(!pointCount && !native_touch_hid_reports){
                    if(sampleNumber==1){recordHardware(sampleNumber);recordBoardRail(1);}
                    recordDataPath(sampleNumber);
                }
                if(bus::mLastLines!=3){stop(15);return;}}
        }
        if(stage==6 && healthRecord<ARRAY_SIZE(healthTimes) && seconds()>=healthTimes[healthRecord]){
            recordSensing();if(healthRecord<2)recordGpi(0);++healthRecord;
        }
        if(stage==6)scriptTick(GetTimeInNanoSecond(GetPerformanceCounter()-epoch)/1000000ull);
        IOSleep(sleepMs);
    }
}
extern "C" void native_touch_probe(void) {
    unsigned enabled=0;if(!PE_parse_boot_argn("umi-fts",&enabled,sizeof(enabled)) || enabled!=1 || stage)return;
    __asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));if(!frequency)return;epoch=GetPerformanceCounter();
    native_touch_stage_req=1;stage=1;thread_t t;
    if(kernel_thread_start(worker,nullptr,&t)==KERN_SUCCESS)thread_deallocate(t);else{stage=90;failure=12;}
}
