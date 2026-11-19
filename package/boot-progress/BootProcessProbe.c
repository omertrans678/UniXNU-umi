// SPDX-License-Identifier: BSD-2-Clause-Patent
// Process traversal can wait in proc_iterate despite the bounded thread scan.
// Run capture only in its separate worker. The display takes a nonblocking copy
// of the last complete result; never expose an in-progress row to the renderer.
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/simple_lock.h>
#include <machine/machine_routines.h>
#include <arm/thread.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <kern/kcdata.h>
#include <kern/block_hint.h>
// Port-send waits: the stackshot helper resolves the full queue's receiver pid and launchd label.
extern int ipc_mqueue_full;   /* IPC_MQUEUE_FULL; IPC_MQUEUE_RECEIVE is NO_EVENT64 */
struct umi_ispl_view { void *sblabel; uint64_t context; uint32_t name; uint16_t flags; uint8_t domain; const char *service_name; };
// User stack of another task, read after every lock is dropped (may fault in pages).
extern vm_map_t get_task_map_reference(task_t);
extern kern_return_t vm_map_read_user(vm_map_t, vm_map_address_t, void *, vm_size_t);
extern void vm_map_deallocate(vm_map_t);
extern void *volatile umi_sys_target_task;   /* sleh.c per-task syscall/fault profile */
#include <kern/turnstile.h>
struct proc;
extern char *proc_best_name(struct proc *);
extern int proc_pid(struct proc *);
extern task_t get_threadtask(thread_t);
extern void *get_bsdtask_info(task_t);
// Synchronous IPC (special reply port): follow the turnstile inheritor chain to its end, read-only.
// (kdp_turnstile_fill_tsinfo asserts debugger context on DEVELOPMENT kernels, so walk it here.)
// Final inheritor: a thread -> its process name/pid; a workqueue -> its address.
static void resolve_turnstile(thread_t thread,char *out,size_t len) {
 out[0]=0;
 struct turnstile *ts=thread_get_waiting_turnstile(thread);
 unsigned hops=0;
 while(ts && hops<8) {
  ++hops;
  const unsigned f=(unsigned)ts->ts_inheritor_flags;
  void *inh=ts->ts_inheritor;
  if(!inh){snprintf(out,len," ts%u none",hops);return;}
  if(f&TURNSTILE_INHERITOR_TURNSTILE){ts=(struct turnstile *)inh;continue;}
  if(f&TURNSTILE_INHERITOR_THREAD) {
   thread_t t=(thread_t)inh;task_t task=get_threadtask(t);
   struct proc *p=task?(struct proc *)get_bsdtask_info(task):NULL;
   snprintf(out,len," ts%u thr%llx %s/%d",hops,(unsigned long long)t->thread_id,p?proc_best_name(p):"?",p?proc_pid(p):-1);
   return;
  }
  if(f&TURNSTILE_INHERITOR_WORKQ){snprintf(out,len," ts%u workq%llx",hops,(unsigned long long)(uintptr_t)inh);return;}
  snprintf(out,len," ts%u f%x",hops,f);return;
 }
 if(hops)snprintf(out,len," ts%u loop",hops);
}
// Caller holds the thread's sched lock. Mach port waits only: owner (send: receiver pid; receive:
// receiver-side port name), special-reply flag and launchd service label.
static void resolve_port_wait(thread_t thread,unsigned flags,uint64_t *owner,unsigned *wflags,char *service,size_t len) {
 *owner=0;*wflags=0;service[0]=0;
 if(!(flags&TH_WAIT) || !thread->waitq.wq_q)return;
 thread_waitinfo_v2_t wi={0};struct ipc_service_port_label *ispl=NULL;
 if(thread->block_hint==kThreadWaitPortSend && thread->wait_event==CAST_EVENT64_T(&ipc_mqueue_full)) {
  wi.wait_type=kThreadWaitPortSend;kdp_mqueue_send_find_owner(thread->waitq.wq_q,thread->wait_event,&wi,&ispl);
 } else if(thread->block_hint==kThreadWaitPortReceive && thread->wait_event==NO_EVENT64) {
  wi.wait_type=kThreadWaitPortReceive;kdp_mqueue_recv_find_owner(thread->waitq.wq_q,thread->wait_event,&wi,&ispl);
 } else return;
 *owner=wi.owner;*wflags=wi.wait_flags;
 const struct umi_ispl_view *v=(const struct umi_ispl_view *)ispl;
 if(v && (uint64_t)v->service_name>=0xfffffff000000000ULL) {
  size_t i=0;for(;i+1<len && v->service_name[i];++i)service[i]=v->service_name[i];service[i]=0;
 }
}
// Exact private process ABI from this source revision's proc.h/proc_internal.h.
// Do not mix BSD and Mach-private header type universes in one translation unit.
struct proc;typedef struct proc *proc_t;
char *proc_best_name(proc_t);
int proc_pid(proc_t);
task_t proc_task(proc_t);
void proc_iterate(unsigned,int (*)(proc_t,void *),void *,int (*)(proc_t,void *),void *);
#define PROC_RETURNED 0
#define PROC_ALLPROCLIST (1U<<0)
#define PROC_NOWAITTRANS (1U<<2)
char boot_process_rows[10][120];
extern void boot_ui_trace_begin(void) __attribute__((weak_import));
extern void boot_ui_trace_task(task_t,unsigned,int,const char *,unsigned) __attribute__((weak_import));
extern void boot_ui_trace_publish(void) __attribute__((weak_import));

volatile unsigned boot_process_passes,boot_process_seen;
volatile unsigned boot_process_attempts,boot_process_busy;
static char capture_rows[10][120],published_rows[10][120];
// Blocked-thread evidence for the sampled thread of each slot: block_hint, user LR, and the
// kernel return addresses of its switched-out stack (fp chain from the saved kernel state).
char boot_process_bt_rows[10][2048];
static char capture_bt[10][2048],published_bt[10][2048];
static unsigned capture_seen,capture_passes,published_passes,published_seen,publish_lock;
// backboardd main-thread history, one entry per capture pass (~2 s): progress vs. a loop shows as
// moving PC/LR and growing syscall counts vs. a fixed PC with counts racing.
char boot_process_hist_rows[8][120];
volatile unsigned boot_process_hist_count;
static unsigned chrono_focus_initialized;
static char focus_name[32],focus2_name[32],focus3_name[32],focus4_name[32];
static unsigned trace_slot[10]={0,1,1};
static const char *targets[]={"launchd","backboardd","SpringBoard","securityd","runningboardd","lsd","keybagd","cfprefsd","containermanagerd","kernel_task"};
static int selected(proc_t p,void *arg) {
 (void)arg;
 const char *name=proc_best_name(p);
 for(unsigned i=0;i<10;++i)if(!strcmp(name,targets[i]))return 1;
 return proc_pid(p)==0;
}
static int capture(proc_t p,void *arg) {
 (void)arg;
 const char *name=proc_best_name(p);unsigned index=10;
 for(unsigned i=0;i<10;++i)if(!strcmp(name,targets[i])){index=i;break;}
 if(proc_pid(p)==0)index=9;
 if(index==10)return PROC_RETURNED;
 task_t task=proc_task(p);
 if(!task || !task_lock_try(task)) {
  snprintf(capture_rows[index],120,"%.18s p%d task unavailable/busy",name,proc_pid(p));return PROC_RETURNED;
 }
 unsigned count=0,running=0,waiting=0,busy=0,state=0,sampled=0;int priority=0;
 uint64_t wait=0,continuation=0,userPC=0,userLR=0,userFP=0,frames[8]={0},owner=0;
 unsigned hint=0,nframes=0,wflags=0;
 char service[48]={0},extra[900]={0};
 // backboardd and focus3 slots: every thread (up to 8). focus3 also gets the user frame chains of
 // its first 4 threads (read after the locks are dropped).
 const unsigned detail_from=1,detail_to=8;
 // SpringBoard (slot 2): every thread, user frame chains of up to 8 threads, each thread's mach port
 // name (TSD slot 3) and, for os_unfair_lock waiters, the ulock address/value (owner port name).
 // lsd (slot 5) and containermanagerd (slot 8) likewise; for both deep slots the 8 traced threads skip parked workqueue threads.
 const bool deep=index==2 || index==5 || index==8;
 const bool detail=index==1 || deep || (index==3 && trace_slot[3]);
 uint64_t tfp[8]={0},tself[8]={0},lka[8]={0};uint32_t lkv[8]={0};unsigned picked=0;
 uint32_t sysU=0,sysM=0;
 thread_t thread;
 queue_iterate(&task->threads,thread,thread_t,task_threads){
  if(count++>=128)break;
  bool irq=ml_set_interrupts_enabled(false);
  if(!simple_lock_try(&thread->sched_lock,&thread_lck_grp)){++busy;ml_set_interrupts_enabled(irq);continue;}
  unsigned flags=(unsigned)thread->state;
  running+=(flags&TH_RUN)!=0;waiting+=(flags&TH_WAIT)!=0;
  bool show=detail && count>=detail_from && count<=detail_to;
  unsigned slot=count-1;
  if(deep){show=picked<8 && thread->block_hint!=kThreadWaitParkedWorkQueue;slot=picked;}
  if(show) {
   if(deep)++picked;
   uint64_t o=0;unsigned wf=0;char sv[40];
   resolve_port_wait(thread,flags,&o,&wf,sv,sizeof(sv));
   size_t at=strlen(extra);
   if(at<sizeof(extra)-1)snprintf(extra+at,sizeof(extra)-at," |%u s%x h%x W%llx U%llx L%llx",count,flags,(unsigned)thread->block_hint,
     (unsigned long long)thread->wait_event,
     (unsigned long long)(thread->machine.upcb?(uint64_t)get_saved_state_pc(thread->machine.upcb):0),
     (unsigned long long)(thread->machine.upcb?(uint64_t)get_saved_state_lr(thread->machine.upcb):0));
   at=strlen(extra);
   if((o||wf||sv[0]) && at<sizeof(extra)-1)snprintf(extra+at,sizeof(extra)-at," o%llx f%x %s",(unsigned long long)o,wf,sv[0]?sv:"-");
   if(wf&STACKSHOT_WAITINFO_FLAGS_SPECIALREPLY) {
    char ts[96];resolve_turnstile(thread,ts,sizeof(ts));
    at=strlen(extra);
    if(ts[0] && at<sizeof(extra)-1)snprintf(extra+at,sizeof(extra)-at,"%s",ts);
   }
   at=strlen(extra);
   if(at<sizeof(extra)-1)snprintf(extra+at,sizeof(extra)-at," t%llx",(unsigned long long)thread->thread_id);
   if((deep || ((index==3 || (index==1 && count>=2)) && count<=4)) && thread->machine.upcb)
    tfp[slot]=(uint64_t)get_saved_state_fp(thread->machine.upcb);
   if(deep) {
    tself[slot]=(uint64_t)thread->machine.cthread_self;
    if(thread->block_hint==kThreadWaitUserLock && thread->machine.upcb) {
     lka[slot]=(uint64_t)get_saved_state_reg(thread->machine.upcb,1);
     lkv[slot]=(uint32_t)get_saved_state_reg(thread->machine.upcb,2);
    }
   }
  }
  bool take=count==1;
  if(index==9){char tn[64]={};thread_get_thread_name(thread,tn);take=!strcmp(tn,"VM_pageout_scan");}
  if(take){
   sampled=1;state=flags;priority=thread->sched_pri;wait=thread->wait_event;
   continuation=(uint64_t)thread->continuation;
   hint=(unsigned)thread->block_hint;
   if(hint==kThreadWaitPortSend)resolve_port_wait(thread,flags,&owner,&wflags,service,sizeof(service));
   if(thread->machine.upcb) {
    userPC=(uint64_t)get_saved_state_pc(thread->machine.upcb);
    userLR=(uint64_t)get_saved_state_lr(thread->machine.upcb);
    userFP=(uint64_t)get_saved_state_fp(thread->machine.upcb);
   }
   sysU=thread->syscalls_unix;sysM=thread->syscalls_mach;
   // A waiting thread without a continuation keeps its kernel stack; with its sched lock held
   // it cannot be resumed, so the saved fp/lr and the frames below them are stable. Stay
   // strictly inside this thread's own kernel stack.
   if((flags&TH_WAIT) && !(flags&TH_RUN) && !thread->continuation &&
      thread->kernel_stack && thread->machine.kstackptr) {
    const struct thread_kernel_state *iks=(const struct thread_kernel_state *)thread->machine.kstackptr;
    const uint64_t lo=(uint64_t)thread->kernel_stack,hi=lo+(uint64_t)kernel_stack_size;
    frames[nframes++]=iks->machine.ss.lr;
    uint64_t fp=iks->machine.ss.fp;
    while(nframes<8 && fp>=lo && fp+16<=hi && !(fp&7)) {
     const uint64_t *f=(const uint64_t *)fp;
     const uint64_t next=f[0],ret=f[1];
     if(!ret)break;
     frames[nframes++]=ret;
     if(next<=fp)break;
     fp=next;
    }
   }
  }
  thread_unlock(thread);ml_set_interrupts_enabled(irq);
 }
 unsigned total=(unsigned)task->thread_count,slide=task->task_shared_region_slide;task_unlock(task);
 if(index==5)umi_sys_target_task=(void *)task;   /* lsd (was backboardd); compared only, never dereferenced */
 // backboardd: user frame chain of the sampled (main) thread, now that no lock is held.
 uint64_t ustack[12];unsigned nustack=0;
 if(index==1 && userFP && !(userFP&7)) {
  vm_map_t map=get_task_map_reference(task);
  if(map) {
   uint64_t fp=userFP;
   while(nustack<12 && fp && !(fp&7) && fp<0x0000800000000000ULL) {
    uint64_t fr[2]={0,0};
    if(vm_map_read_user(map,(vm_map_address_t)fp,fr,sizeof(fr))!=KERN_SUCCESS || !fr[1])break;
    ustack[nustack++]=fr[1];
    if(fr[0]<=fp)break;
    fp=fr[0];
   }
   vm_map_deallocate(map);
  }
 }
 if(index==1) {
  uint64_t now=mach_absolute_time();
  unsigned h=__atomic_fetch_add(&boot_process_hist_count,1,__ATOMIC_RELAXED);
  snprintf(boot_process_hist_rows[h%8],120,"t%llums pass%u s%x h%x U%llx L%llx c%llx/%llx su%u sm%u +%x",
   (unsigned long long)(now/19200ULL),capture_passes,state,hint,(unsigned long long)userPC,(unsigned long long)userLR,
   (unsigned long long)(nustack>2?ustack[2]:0),(unsigned long long)(nustack>5?ustack[5]:0),sysU,sysM,slide);
 }
 char ustr[1100]={0};
 if((index==3 || index==1 || deep) && (tfp[0]|tfp[1]|tfp[2]|tfp[3]|tfp[4]|tfp[5]|tfp[6]|tfp[7])) {
  vm_map_t map=get_task_map_reference(task);
  if(map) {
   for(unsigned t=0;t<8;++t) {
    uint64_t fp=tfp[t];
    if(deep && (tself[t]|lka[t])) {
     uint64_t port=0;uint32_t now=0;size_t at=strlen(ustr);
     if(tself[t])(void)vm_map_read_user(map,(vm_map_address_t)(tself[t]+3*8),&port,sizeof(port));
     if(lka[t])(void)vm_map_read_user(map,(vm_map_address_t)lka[t],&now,sizeof(now));
     if(at<sizeof(ustr)-1)snprintf(ustr+at,sizeof(ustr)-at," T%u mp%x",t+1,(unsigned)port);
     at=strlen(ustr);
     if(lka[t] && at<sizeof(ustr)-1)snprintf(ustr+at,sizeof(ustr)-at," lk%llx v%x/%x",(unsigned long long)lka[t],lkv[t],now);
    }
    if(!fp)continue;
    size_t at=strlen(ustr);
    if(at<sizeof(ustr)-1)snprintf(ustr+at,sizeof(ustr)-at," us%u",t+1);
    for(unsigned k=0;k<(deep?10u:8u) && fp && !(fp&7) && fp<0x0000800000000000ULL;++k) {
     uint64_t fr[2]={0,0};
     if(vm_map_read_user(map,(vm_map_address_t)fp,fr,sizeof(fr))!=KERN_SUCCESS || !fr[1])break;
     at=strlen(ustr);
     if(at<sizeof(ustr)-1)snprintf(ustr+at,sizeof(ustr)-at," %llx",(unsigned long long)fr[1]);
     if(fr[0]<=fp)break;
     fp=fr[0];
    }
   }
   vm_map_deallocate(map);
  }
 }
 snprintf(capture_rows[index],120,"%.15s p%d n%u r%u w%u b%u t%u s%x/%d W%llx C%llx U%llx +%x",
  targets[index],proc_pid(p),total,running,waiting,busy,sampled,state,priority,wait,continuation,userPC,slide);
 {
  const int cap=(int)sizeof(capture_bt[0]);
  int n=service[0]||owner ?
   snprintf(capture_bt[index],cap,"%.15s p%d h%x own%lld svc %.47s uLR%llx k",targets[index],proc_pid(p),hint,(long long)owner,
            service[0]?service:"-",userLR) :
   snprintf(capture_bt[index],cap,"%.15s p%d h%x uLR%llx k",targets[index],proc_pid(p),hint,userLR);
  for(unsigned i=0;i<nframes && n>0 && n<cap;++i)n+=snprintf(capture_bt[index]+n,(size_t)(cap-n)," %llx",frames[i]);
  if(nustack && n>0 && n<cap)n+=snprintf(capture_bt[index]+n,(size_t)(cap-n)," us");
  for(unsigned i=0;i<nustack && n>0 && n<cap;++i)n+=snprintf(capture_bt[index]+n,(size_t)(cap-n)," %llx",ustack[i]);
  if(extra[0] && n>0 && n<cap)n+=snprintf(capture_bt[index]+n,(size_t)(cap-n),"%s",extra);
  if(ustr[0] && n>0 && n<cap)snprintf(capture_bt[index]+n,(size_t)(cap-n),"%s",ustr);
 }
 if(trace_slot[index] && boot_ui_trace_task)boot_ui_trace_task(task,index,proc_pid(p),name,slide);
 capture_seen=capture_seen+1;return PROC_RETURNED;
}
void boot_process_capture(void) {
 // Exactly one capture worker. Refuse reentry without waiting or modifying its
 // partial results, including in host failure-injection checks.
 if(__atomic_exchange_n(&boot_process_busy,1,__ATOMIC_ACQUIRE))return;
 __atomic_fetch_add(&boot_process_attempts,1,__ATOMIC_RELAXED);
 if(!chrono_focus_initialized) {
  unsigned focus=0;PE_parse_boot_argn("boot-ui-chrono",&focus,sizeof(focus));
  if(focus==1){targets[1]="chronod";targets[3]="backboardd";}
  // boot-ui-focus=<process> replaces the sampled second slot; the chronod flag above stays as a fallback.
  if(PE_parse_boot_argn("boot-ui-focus",focus_name,sizeof(focus_name)) && focus_name[0]) {
   focus_name[sizeof(focus_name)-1]=0;targets[1]=focus_name;targets[3]="backboardd";
  }
  // boot-ui-focus2=<process> likewise replaces the third slot (normally SpringBoard, role S).
  if(PE_parse_boot_argn("boot-ui-focus2",focus2_name,sizeof(focus2_name)) && focus2_name[0]) {
   focus2_name[sizeof(focus2_name)-1]=0;targets[2]=focus2_name;
  }
  // boot-ui-focus3/4 add two more traced slots (replacing the PreBoard/runningboardd rows).
  if(PE_parse_boot_argn("boot-ui-focus3",focus3_name,sizeof(focus3_name)) && focus3_name[0]) {
   focus3_name[sizeof(focus3_name)-1]=0;targets[3]=focus3_name;trace_slot[3]=1;
  }
  if(PE_parse_boot_argn("boot-ui-focus4",focus4_name,sizeof(focus4_name)) && focus4_name[0]) {
   focus4_name[sizeof(focus4_name)-1]=0;targets[4]=focus4_name;trace_slot[4]=1;
  }
  chrono_focus_initialized=1;
 }
 for(unsigned i=0;i<10;++i)snprintf(capture_rows[i],120,"%.17s: absent from this process pass",targets[i]);
 for(unsigned i=0;i<10;++i)capture_bt[i][0]=0;
 capture_seen=0;
 if(boot_ui_trace_begin)boot_ui_trace_begin();
 // No waiting for exec/exit transitions; callouts run outside proc_list_lock.
 proc_iterate(PROC_ALLPROCLIST|PROC_NOWAITTRANS,capture,NULL,selected,NULL);
 if(boot_ui_trace_publish)boot_ui_trace_publish();
 ++capture_passes;
 if(!__atomic_exchange_n(&publish_lock,1,__ATOMIC_ACQUIRE)) {
  memcpy(published_rows,capture_rows,sizeof(published_rows));
  memcpy(published_bt,capture_bt,sizeof(published_bt));
  published_passes=capture_passes;published_seen=capture_seen;
  __atomic_store_n(&publish_lock,0,__ATOMIC_RELEASE);
 }
 __atomic_store_n(&boot_process_busy,0,__ATOMIC_RELEASE);
}
void boot_process_snapshot(void) {
 // One reader: the diagnostic monitor. Never allocate, walk processes or wait
 // for the worker; leave the previous complete display snapshot if it is busy.
 if(__atomic_exchange_n(&publish_lock,1,__ATOMIC_ACQUIRE))return;
 memcpy(boot_process_rows,published_rows,sizeof(boot_process_rows));
 memcpy(boot_process_bt_rows,published_bt,sizeof(boot_process_bt_rows));
 boot_process_passes=published_passes;boot_process_seen=published_seen;
 __atomic_store_n(&publish_lock,0,__ATOMIC_RELEASE);
}
