// SPDX-License-Identifier: MIT
// Diagnostic worker and bounded syscall observers; never change a result or policy.
#include <stdint.h>
#include <string.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/vnode.h>
#include <sys/mount_internal.h>
#include <sys/errno.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#if UMI_RUNTIME_TRACE
#include "UmiUserWire.h"
extern unsigned long umi_trace_console_address;
extern uint64_t boot_progress_epoch,boot_progress_frequency;
extern volatile int native_owner_data_stage;
extern volatile uint32_t native_data_persist_state,native_data_persist_header,native_data_persist_gpt,native_data_persist_gpt_after;
extern volatile uint64_t native_data_persist_pages,native_data_persist_loaded,native_data_persist_writes,native_data_persist_syncs,native_data_persist_errors,native_data_persist_present,native_data_persist_first_lba;
extern volatile uint64_t native_ram_block_bytes,native_ram_block_resident_bytes;
extern char boot_progress_ext_wide[2][112];
extern int umi_ui_wait_snapshot(unsigned,unsigned,uint64_t [8][64]);
#if UMI_FTS_TOUCH
extern void umi_touch_snapshot(uint64_t [32]);
extern unsigned umi_touch_log_peek(uint64_t [16]);
extern void umi_touch_log_commit(unsigned);
#endif
unsigned umi_console_lock;
static unsigned enabled,started,lock,events,flushed,dropped;
static uint64_t records[96][48];
static unsigned quota[16][10],errors[4];
static const char *names[]={"usermanagerd","containermanagerd","pkd","SpringBoard","backboardd","keybagd","lsd","cfprefsd","runningboardd","cryptexd","xpcproxy","launchd","PerfPowerServices","mDNSResponder","WindowServer","chronod"};
static unsigned now(void){uint64_t c;__asm__ volatile("isb\n\tmrs %0,cntpct_el0":"=r"(c));return boot_progress_frequency?(unsigned)((c-boot_progress_epoch)/boot_progress_frequency):0;}
static int selected(const char *name){for(unsigned i=0;i<16;++i)if(!strcmp(name,names[i]))return (int)i;return -1;}
static int contains(const char *s,const char *needle) {
  for(;*s;++s){const char *a=s,*b=needle;while(*a&&*b){char c=*a;if(c>='A'&&c<='Z')c+=32;if(c!=*b)break;++a;++b;}if(!*b)return 1;}return 0;
}
static void record(unsigned kind,int pid,const char *name,int error,uint64_t aux,const char *text,int copy_error) {
  if(!enabled || kind>4)return;int target=selected(name);if(target<0)return;
  if(__atomic_exchange_n(&lock,1,__ATOMIC_ACQUIRE)){__atomic_fetch_add(&dropped,1,__ATOMIC_RELAXED);return;}
  // Reserve the last 24 records for actual process failures.
  unsigned bucket=kind+(error?5:0),limit=kind==1?6:kind==4?4:error?6:2;
  if(events>=96 || (kind!=4 && events>=72) || quota[target][bucket]>=limit){++dropped;__atomic_store_n(&lock,0,__ATOMIC_RELEASE);return;}
  ++quota[target][bucket];unsigned slot=events++;uint64_t *v=records[slot];
  v[0]=1;v[1]=slot+1;v[2]=now();v[3]=kind;v[4]=(uint32_t)pid;v[5]=(uint32_t)error;v[6]=aux;v[7]=(uint32_t)copy_error;
  memcpy(v+8,name,strnlen(name,31));memcpy(v+12,text,strnlen(text,255));
  __atomic_store_n(&lock,0,__ATOMIC_RELEASE);
}
void umi_user_write_observe(int fd,uint64_t buffer,uint64_t bytes,int error) {
  if(!enabled || (fd!=1 && fd!=2) || !bytes)return;
  char name[32]={0};proc_selfname(name,sizeof(name));if(selected(name)<0)return;
  char text[256]={0};size_t n=bytes<255?(size_t)bytes:255;int e=copyin((user_addr_t)buffer,text,n);
  if(e)return;
  if(!contains(text,"error")&&!contains(text,"fail")&&!contains(text,"persona")&&!contains(text,"container")&&!contains(text,"keybag")&&!contains(text,"foreground")&&!contains(text,"default user")&&!contains(text,"fatal"))return;
  record(1,proc_selfpid(),name,error,bytes,text,0);
}
void umi_user_path_observe(unsigned operation,uint64_t path,int error,int fd) {
  if(!enabled || error==EEXIST || (operation==2 && !error))return;
  char name[32]={0};proc_selfname(name,sizeof(name));if(selected(name)<0)return;
  char text[256]={0};size_t n=0;int e=copyinstr((user_addr_t)path,text,sizeof(text),&n);if(e)return;
  if(error==ENOSPC)__atomic_fetch_add(&errors[0],1,__ATOMIC_RELAXED);
  if(error==EACCES||error==EPERM)__atomic_fetch_add(&errors[1],1,__ATOMIC_RELAXED);
  if(error==EROFS)__atomic_fetch_add(&errors[2],1,__ATOMIC_RELAXED);
  if(error==EDQUOT)__atomic_fetch_add(&errors[3],1,__ATOMIC_RELAXED);
  if(error!=ENOSPC && error!=EACCES && error!=EPERM && error!=EROFS && error!=EDQUOT &&
     !contains(text,"container")&&!contains(text,"persona")&&!contains(text,"keybag")&&!contains(text,"dslocal"))return;
  record(operation==2?2:3,proc_selfpid(),name,error,(uint32_t)fd,text,0);
}
void umi_user_exit_observe(int pid,const char *name,int status,unsigned ns,uint64_t code,const char *reason) {
  if(!enabled || !(status&0xffff))return;
  char text[256]={0};snprintf(text,sizeof(text),"namespace=%u code=%llu %s",ns,code,reason?reason:"");
  record(4,pid,name,status,code,text,0);
}
static int emit(const char *tag,unsigned seq,const uint64_t *v,unsigned fields) {
  if(__atomic_exchange_n(&umi_console_lock,1,__ATOMIC_ACQUIRE))return 0;
  uint64_t daif;__asm__ volatile("mrs %0,daif\n\tmsr daifset,#3":"=r"(daif)::"memory");
  __asm__ volatile("dsb sy":::"memory");
  int ok=(!memcmp(tag,"WAIT",4)||!memcmp(tag,"TOUC",4)||!memcmp(tag,"TCHX",4))?umi_user_wire_repeat((volatile uint32_t *)umi_trace_console_address,tag,seq,v,fields,3):umi_user_wire((volatile uint32_t *)umi_trace_console_address,tag,seq,v,fields);
  __asm__ volatile("dsb sy\n\tmsr daif,%0"::"r"(daif):"memory");__atomic_store_n(&umi_console_lock,0,__ATOMIC_RELEASE);return ok;
}
static void snapshot(unsigned seq) {
  uint64_t v[64]={1,seq,now(),(uint32_t)-1};
  if(native_owner_data_stage==4){
    vnode_t vp=NULLVP;int error=vnode_lookup("/private/var",0,&vp,vfs_context_kernel());
    if(!error){
      struct vfs_attr a={0};VFSATTR_INIT(&a);
      VFSATTR_WANTED(&a,f_bsize);VFSATTR_WANTED(&a,f_blocks);VFSATTR_WANTED(&a,f_bfree);VFSATTR_WANTED(&a,f_bavail);VFSATTR_WANTED(&a,f_files);VFSATTR_WANTED(&a,f_ffree);
      mount_t mp=vnode_mount(vp);error=vfs_getattr(mp,&a,vfs_context_kernel());
      if(!error){v[4]=a.f_bsize;v[5]=a.f_blocks;v[6]=a.f_bfree;v[7]=a.f_bavail;v[8]=a.f_files;v[9]=a.f_ffree;v[10]=a.f_supported;}
      v[11]=(vfs_flags(mp)&MNT_RDONLY)!=0;vnode_put(vp);
    }v[3]=(uint32_t)error;
    const char *paths[]={"/private/var/containers","/private/var/containers/Data/System","/private/var/mobile/Containers","/private/var/db/dslocal/nodes/Default/users","/private/var/PersonaVolumes","/private/var/mobile/Library/Preferences","/private/var/containers/Bundle","/private/var/containers/Temp"};
    for(unsigned i=0;i<8;++i){vp=NULLVP;int e=vnode_lookup(paths[i],0,&vp,vfs_context_kernel());
      if(!e){struct vnode_attr a={0};VATTR_INIT(&a);VATTR_WANTED(&a,va_uid);VATTR_WANTED(&a,va_gid);VATTR_WANTED(&a,va_mode);e=vnode_getattr(vp,&a,vfs_context_kernel());if(!e){v[33+i*4]=a.va_uid;v[34+i*4]=a.va_gid;v[35+i*4]=a.va_mode;}vnode_put(vp);}v[32+i*4]=(uint32_t)e;}
  }
  const uint64_t ram_bytes=&native_ram_block_bytes?native_ram_block_bytes:0;
  const uint64_t ram_res=&native_ram_block_resident_bytes?native_ram_block_resident_bytes:0;
  const uint64_t data[]={native_data_persist_state,native_data_persist_header,native_data_persist_gpt,native_data_persist_gpt_after,
    native_data_persist_pages,native_data_persist_loaded,native_data_persist_writes,native_data_persist_syncs,native_data_persist_errors,native_data_persist_present,native_data_persist_first_lba,
    ram_bytes,ram_res,events,dropped,errors[0],errors[1],errors[2],errors[3],(unsigned)nprocs};memcpy(v+12,data,sizeof(data));
  // Existing diagnostic extension slots; no display ownership or drawing changes.
  snprintf(boot_progress_ext_wide[0],112,"DATA MiB=%llu free=%llu avail=%llu fs=%u persist=%u writes=%llu errors=%llu",ram_bytes>>20,(v[6]*v[4])>>20,(v[7]*v[4])>>20,(unsigned)v[3],native_data_persist_state,native_data_persist_writes,native_data_persist_errors);
  snprintf(boot_progress_ext_wide[1],112,"USER records=%u dropped=%u ENOSPC=%u access=%u rofs=%u quota=%u",events,dropped,errors[0],errors[1],errors[2],errors[3]);
  emit("DATA",seq,v,64);
}
static void worker(void *arg,wait_result_t wr) {
  (void)arg;(void)wr;thread_set_thread_name(current_thread(),"umi_userspace");unsigned sample=0;const unsigned deadlines[]={10,30,60,90,120,180,240,300,480,600};
  unsigned wait_sample=0,wait_next=8;const unsigned wait_deadlines[]={120,300,600};
  static uint64_t wait_rows[8][64];
#if UMI_FTS_TOUCH
  unsigned touch_sample=0;
#endif
  for(;;){
    for(unsigned i=0;i<4;++i){uint64_t v[48];unsigned id=0;
      if(!__atomic_exchange_n(&lock,1,__ATOMIC_ACQUIRE)){if(flushed<events){id=flushed+1;memcpy(v,records[flushed],sizeof(v));}__atomic_store_n(&lock,0,__ATOMIC_RELEASE);}
      if(!id||!emit("USER",id,v,48))break;++flushed;
    }
    if(sample<sizeof(deadlines)/sizeof(deadlines[0]) && now()>=deadlines[sample])snapshot(++sample);
    if(wait_next==8 && wait_sample<3 && now()>=wait_deadlines[wait_sample] &&
       umi_ui_wait_snapshot(wait_sample*8+1,now(),wait_rows)){++wait_sample;wait_next=0;}
    if(wait_next<8 && emit("WAIT",(unsigned)wait_rows[wait_next][1],wait_rows[wait_next],64))++wait_next;
#if UMI_FTS_TOUCH
    // 30 summaries + 96 raw events, three CRC copies: <=314496 bytes.
    // Alongside the retained WAIT/DATA/USER/LIVE records this fits the 2 MiB console.
    if(touch_sample<30 && now()>=2+touch_sample*10) {
      uint64_t v[32];umi_touch_snapshot(v);if(emit("TOUC",touch_sample+1,v,32))++touch_sample;
    }
    for(unsigned i=0;i<4;++i){uint64_t v[16];unsigned id=umi_touch_log_peek(v);
      if(!id||!emit("TCHX",id,v,16))break;umi_touch_log_commit(id);}
#endif
    assert_wait_timeout((event_t)&enabled,THREAD_UNINT,1000,NSEC_PER_MSEC);thread_block(THREAD_CONTINUE_NULL);
  }
}
void umi_user_start(void) {
  unsigned opt=0;PE_parse_boot_argn("umi-user-log",&opt,sizeof(opt));if(opt!=1||started)return;
  started=1;enabled=1;thread_t t;
  if(kernel_thread_start(worker,NULL,&t)==KERN_SUCCESS)thread_deallocate(t);else enabled=0;
}

#else
unsigned umi_console_lock;
void umi_user_start(void) {}
void umi_user_write_observe(int fd,uint64_t buffer,uint64_t bytes,int error) {(void)fd;(void)buffer;(void)bytes;(void)error;}
void umi_user_path_observe(unsigned operation,uint64_t path,int error,int fd) {(void)operation;(void)path;(void)error;(void)fd;}
void umi_user_exit_observe(int pid,const char *name,int status,unsigned ns,uint64_t code,const char *reason) {(void)pid;(void)name;(void)status;(void)ns;(void)code;(void)reason;}
#endif
