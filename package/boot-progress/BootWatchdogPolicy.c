// SPDX-License-Identifier: MIT
// Explicit temporary bring-up policy for an unavailable watchdog backend.
// Runs after writable scratch Data mounts, before the unchanged launchd exec.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <pexpert/pexpert.h>

static const char policy_head[] =
 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
 "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
 "<plist version=\"1.0\"><dict>";
static const char policy_watchdogd[]="<key>com.apple.watchdogd</key><true/>";
// Bring-up isolation: hardware stacks show identityservicesd, contextstored and locationd in a
// cycle of synchronous XPC waits. A disabled job makes its Mach service lookups fail instead of hang.
// sheng-disable-jobs=<label>[,<label>...] (restricted label characters, bounded length).
// A label written as ~name stands for com.apple.name (keeps the kernel arguments under their limit).
static const char policy_tail[]="</dict></plist>\n";
static char policy[768],jobs[320];
volatile int sheng_watchdog_policy_status=-1;
// umi-skip-setup=1: boot to SpringBoard instead of Setup, as unixnu.com/guide/skip-setup does with
// scripts/seed-data-skip-setup.py (which edits the Data seed on a Mac): the same preference domains and container
// directories, written here on the Data volume before launchd. umi has no Mac seed editor, so each domain file is
// rewritten on every boot (not merged). --no-auto-dim keys included: no working idle timer on umi.
// The /chosen identity half of the guide is in umi-work/tools/make_umi_payload.py.
volatile int umi_skip_setup_status=-1;
#define UMI_PLIST_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
#define UMI_PLIST_TAIL "</dict></plist>\n"
static const struct { const char *domain, *body; } umi_setup_prefs[]={
 {"com.apple.migration",
  "<key>LastSystemVersion</key><string>21H461</string>"
  "<key>DMUserDataDisposition</key><dict><key>SyntheticDidUpgrade</key><false/><key>buildVersion</key><string>21H461</string></dict>"},
 {"com.apple.springboard",
  "<key>BKNoWatchdogs</key><true/>"
  "<key>FBLaunchWatchdogExceptions</key><dict><key>com.apple.purplebuddy</key><real>100</real></dict>"
  "<key>MCForceShowCloudConfigurationUI</key><false/>"
  "<key>SBDisableAutoDim</key><true/><key>SBDontDimOrLockOnAC</key><true/><key>SBDisableIdleSleep</key><true/>"},
 {"com.apple.purplebuddy",
  "<key>SetupDone</key><true/><key>SetupFinishedAllSteps</key><true/>"
  "<key>SetupVersion</key><integer>11</integer><key>daemonizeNeedsToRun</key><false/>"},
 {"com.apple.purplebuddy.notbackedup",
  "<key>ShouldRunDiagnosticsMiniBuddy</key><false/><key>ShouldRunHSA2MiniBuddy</key><false/>"
  "<key>ShouldRunKeychainSyncMiniBuddy</key><false/><key>ShouldRunMesaMiniBuddy</key><false/>"
  "<key>ShouldRunPaymentMiniBuddy</key><false/><key>ShouldRunStandardMiniBuddy</key><false/>"
  "<key>SilentICDPUpgrade</key><false/>"},
 {"com.apple.managedconfiguration.notbackedup",
  "<key>LockdownActivationIndicatesCloudConfigurationAvailable</key><false/>"},
};
static int umi_mkdir_owned(proc_t p,uint64_t scratch,const char *path,int mode,int uid) {
 int error,result=0;
 if((error=copyout(path,scratch,strlen(path)+1)))return error;
 struct mkdir_args md={.path=scratch,.mode=mode};
 error=mkdir(p,&md,&result);
 if(error==EEXIST)return 0;
 if(error)return error;
 if(uid<0)return 0;
 struct chown_args co={.path=scratch,.uid=uid,.gid=uid};
 return chown(p,&co,&result);
}
// Write text through the user scratch window at +0x200 (below the preserved +0x400 slots), in pieces.
static int umi_write_text(proc_t p,uint64_t scratch,int fd,const char *text) {
 size_t n=strlen(text);
 while(n) {
  size_t k=n>0x1f0?0x1f0:n;int error;
  if((error=copyout(text,scratch+0x200,k)))return error;
  struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=k};
  user_ssize_t written=0;
  if((error=write(p,&wr,&written)))return error;
  if(written!=(user_ssize_t)k)return EIO;
  text+=k;n-=k;
 }
 return 0;
}
static int umi_write_pref(proc_t p,uint64_t scratch,const char *domain,const char *body) {
 char path[160];int error,result=0;
 snprintf(path,sizeof(path),"/private/var/mobile/Library/Preferences/%s.plist",domain);
 if((error=copyout(path,scratch,strlen(path)+1)))return error;
 struct open_args op={.path=scratch,.flags=O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,.mode=0600};
 if((error=open(p,&op,&result)))return error;
 int fd=result;
 error=umi_write_text(p,scratch,fd,UMI_PLIST_HEAD);
 if(!error)error=umi_write_text(p,scratch,fd,body);
 if(!error)error=umi_write_text(p,scratch,fd,UMI_PLIST_TAIL);
 if(!error){struct fchown_args owner={.fd=fd,.uid=501,.gid=501};error=fchown(p,&owner,&result);}
 if(!error){struct fchmod_args mode={.fd=fd,.mode=0600};error=fchmod(p,&mode,&result);}
 if(!error){struct fsync_args sync={.fd=fd};error=fsync(p,&sync,&result);}
 struct close_args cl={.fd=fd};int e=sys_close(p,&cl,&result);if(!error)error=e;
 return error;
}
// unixnu.com/guide/mobilegestalt: the cached answers are keyed by build and platform UUID and keep old identity
// answers after an identity change ("Delete the file or rebuild the Data volume after an identity change").
// Removed on every boot here, so MobileGestalt answers from the current ADT (including the /chosen activation
// identity of the skip-setup guide). Count of removed files in umi_mg_cache_removed.
volatile int umi_mg_cache_removed=-1;
static const char *const umi_mg_cache[]={
 "/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist",
 "/private/var/mobile/Library/Preferences/com.apple.MobileGestalt.plist",
 "/private/var/mobile/Library/Preferences/com.apple.mobilegestalt.plist"};
static void umi_clear_mg_cache(proc_t p,uint64_t scratch) {
 int removed=0,result=0;
 for(unsigned i=0;i<sizeof(umi_mg_cache)/sizeof(umi_mg_cache[0]);++i) {
  if(copyout(umi_mg_cache[i],scratch,strlen(umi_mg_cache[i])+1))continue;
  struct unlink_args ua={.path=scratch};
  if(!unlink(p,&ua,&result))++removed;
 }
 umi_mg_cache_removed=removed;
}
// mobileactivationd's data migration traps on "Failed to query keychain": the NativeKeyStoreEvents AppleKeyStore has no key
// operations, and umi keeps a persistent Data volume whose keychain database was written on earlier boots. Move it aside once
// (never deleted; a later boot finds Keychains.umi-old and leaves the new one alone). umi_keychain_moved: 0 moved, else errno.
volatile int umi_keychain_moved=-1;
static void umi_move_keychain(proc_t p,uint64_t scratch) {
 static const char from[]="/private/var/Keychains",to[]="/private/var/Keychains.umi-old";
 int result=0;
 if(copyout(from,scratch,sizeof(from)) || copyout(to,scratch+0x100,sizeof(to))){umi_keychain_moved=EFAULT;return;}
 struct rename_args ra={.from=scratch,.to=scratch+0x100};
 umi_keychain_moved=rename(p,&ra,&result);
}
// mobileactivationd (performMigration, data_migration_support.m) compares MobileGestalt BuildVersion with its data ark's
// "-BuildVersion" (domain nil -> "-%@"); on a mismatch ("Upgrade from ... detected.") it runs the keychain-based data migration,
// which traps ("Failed to query keychain": no key operations in the NativeKeyStoreEvents AppleKeyStore). The ark is
// <own system container>/Library/internal/data_ark.plist (container_system_path_for_identifier(NULL) for its
// com.apple.security.system-container entitlement; it runs as root). Find that container by the identifier in its
// .com.apple.mobile_container_manager.metadata.plist and record the booted build there, as com.apple.migration
// LastSystemVersion does for the system migrator. Once only: an existing ark is kept as data_ark.plist.umi-old, which also
// marks the seed (mobileactivationd owns the ark afterwards). umi_data_ark_status: 0 seeded, EEXIST already seeded, else errno
// (ENOENT: no container yet). An earlier umi build wrote /private/var/mobile/Library/mad/data_ark.plist (the legacy ark,
// a migration source): that is undone first.
volatile int umi_data_ark_status=-1;
static const char umi_data_ark_body[]=
 "<key>-BuildVersion</key><string>21H461</string><key>-ProductVersion</key><string>17.7.11</string>";
static const char umi_ma_container_base[]="/private/var/containers/Data/System";
static const char umi_ma_identifier[]="com.apple.mobileactivationd";
static char umi_dirbuf[0x600],umi_meta[0x600];
static int umi_path_open(proc_t p,uint64_t scratch,const char *path,int flags,int mode,int *fd) {
 int error,result=0;
 if((error=copyout(path,scratch,strlen(path)+1)))return error;
 struct open_args op={.path=scratch,.flags=flags,.mode=mode};
 if((error=open(p,&op,&result)))return error;
 *fd=result;return 0;
}
static void umi_fd_close(proc_t p,int fd){int result=0;struct close_args cl={.fd=fd};(void)sys_close(p,&cl,&result);}
// Size of an open file through the fstat64 window at +0x800 (as the policy check above).
static int64_t umi_fd_size(proc_t p,uint64_t scratch,int fd) {
 int result=0;struct fstat64_args sa={.fd=fd,.ub=scratch+0x800};struct stat64 st={0};
 if(sys_fstat64(p,&sa,&result) || copyin(sa.ub,&st,sizeof(st)))return -1;
 return st.st_size;
}
static void umi_undo_legacy_ark(proc_t p,uint64_t scratch) {
 static const char ark[]="/private/var/mobile/Library/mad/data_ark.plist",old[]="/private/var/mobile/Library/mad/data_ark.plist.umi-old";
 int fd,result=0;
 if(umi_path_open(p,scratch,old,O_RDONLY|O_NOFOLLOW,0,&fd))return;   // never seeded there
 int64_t size=umi_fd_size(p,scratch,fd);umi_fd_close(p,fd);
 if(copyout(ark,scratch,sizeof(ark)) || copyout(old,scratch+0x100,sizeof(old)))return;
 if(size>0){struct rename_args ra={.from=scratch+0x100,.to=scratch};(void)rename(p,&ra,&result);return;}   // restore the original
 struct unlink_args ua={.path=scratch};(void)unlink(p,&ua,&result);
 struct unlink_args um={.path=scratch+0x100};(void)unlink(p,&um,&result);
}
static int umi_has_identifier(const char *buf,size_t n) {
 const size_t l=sizeof(umi_ma_identifier)-1;
 for(size_t i=0;i+l<=n;++i) {
  if(memcmp(buf+i,umi_ma_identifier,l))continue;
  char c=i+l<n?buf[i+l]:0;
  if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-'||c=='_'))return 1;
 }
 return 0;
}
// The UUID directory name of mobileactivationd's data container, or ENOENT.
static int umi_find_ma_container(proc_t p,uint64_t scratch,char *uuid,size_t cap) {
 int dir,error;
 if((error=umi_path_open(p,scratch,umi_ma_container_base,O_RDONLY|O_DIRECTORY,0,&dir)))return error;
 error=ENOENT;
 for(unsigned round=0;round<64 && error==ENOENT;++round) {
  user_ssize_t got=0;
  struct getdirentries64_args ga={.fd=dir,.buf=scratch+0x900,.bufsize=sizeof(umi_dirbuf),.position=scratch+0x8f0};
  if(getdirentries64(p,&ga,&got) || got<=0 || got>(user_ssize_t)sizeof(umi_dirbuf))break;
  if(copyin(scratch+0x900,umi_dirbuf,(size_t)got))break;
  for(size_t off=0;off+21<=(size_t)got && error==ENOENT;) {
   uint16_t reclen,namlen;uint8_t type;
   memcpy(&reclen,umi_dirbuf+off+16,2);memcpy(&namlen,umi_dirbuf+off+18,2);type=(uint8_t)umi_dirbuf[off+20];
   if(!reclen || off+reclen>(size_t)got)break;
   const char *name=umi_dirbuf+off+21;
   if(type==4 && namlen && namlen<64 && name[0]!='.') {
    char path[200];int meta;
    snprintf(path,sizeof(path),"%s/%.*s/.com.apple.mobile_container_manager.metadata.plist",umi_ma_container_base,(int)namlen,name);
    if(!umi_path_open(p,scratch,path,O_RDONLY|O_NOFOLLOW,0,&meta)) {
     int result=0;user_ssize_t n=0;
     struct read_args ra={.fd=meta,.cbuf=scratch+0x200,.nbyte=0x1f0};   // below the preserved +0x400 slots
     size_t have=0;
     while(have+0x1f0<=sizeof(umi_meta) && !read(p,&ra,&n) && n>0) {
      if(copyin(scratch+0x200,umi_meta+have,(size_t)n))break;
      have+=(size_t)n;
     }
     (void)result;umi_fd_close(p,meta);
     if(umi_has_identifier(umi_meta,have)){snprintf(uuid,cap,"%.*s",(int)namlen,name);error=0;}
    }
   }
   off+=reclen;
  }
 }
 umi_fd_close(p,dir);
 return error;
}
static int umi_write_data_ark(proc_t p,uint64_t scratch) {
 int error,result=0,fd;
 umi_undo_legacy_ark(p,scratch);
 char uuid[64]={0},dir[160],internal[180],ark[200],old[210];
 if((error=umi_find_ma_container(p,scratch,uuid,sizeof(uuid))))return error;
 snprintf(dir,sizeof(dir),"%s/%s/Library",umi_ma_container_base,uuid);
 snprintf(internal,sizeof(internal),"%s/internal",dir);
 snprintf(ark,sizeof(ark),"%s/data_ark.plist",internal);
 snprintf(old,sizeof(old),"%s/data_ark.plist.umi-old",internal);
 if(!umi_path_open(p,scratch,old,O_RDONLY|O_NOFOLLOW,0,&fd)){umi_fd_close(p,fd);return EEXIST;}
 if((error=umi_mkdir_owned(p,scratch,dir,0755,-1)) || (error=umi_mkdir_owned(p,scratch,internal,0755,-1)))return error;
 if(copyout(ark,scratch,strlen(ark)+1) || copyout(old,scratch+0x200,strlen(old)+1))return EFAULT;
 struct rename_args ra={.from=scratch,.to=scratch+0x200};
 if(rename(p,&ra,&result) && !umi_path_open(p,scratch,old,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600,&fd))umi_fd_close(p,fd);
 if((error=umi_path_open(p,scratch,ark,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600,&fd)))return error;
 error=umi_write_text(p,scratch,fd,UMI_PLIST_HEAD);
 if(!error)error=umi_write_text(p,scratch,fd,umi_data_ark_body);
 if(!error)error=umi_write_text(p,scratch,fd,UMI_PLIST_TAIL);
 if(!error){struct fchmod_args mode={.fd=fd,.mode=0600};error=fchmod(p,&mode,&result);}
 if(!error){struct fsync_args sync={.fd=fd};error=fsync(p,&sync,&result);}
 umi_fd_close(p,fd);
 return error;
}
// Status: 0, or 100*step+errno (step 1 mobile dirs, 2+i preference i, 8 container dirs).
static int umi_skip_setup(proc_t p,uint64_t scratch) {
 int error=0;
 umi_clear_mg_cache(p,scratch);
 umi_move_keychain(p,scratch);
 static const char *const dirs[]={"/private/var/mobile","/private/var/mobile/Library","/private/var/mobile/Library/Preferences"};
 for(unsigned i=0;i<3;++i)if((error=umi_mkdir_owned(p,scratch,dirs[i],0755,501)))return 100+error;
 for(unsigned i=0;i<sizeof(umi_setup_prefs)/sizeof(umi_setup_prefs[0]);++i)
  if((error=umi_write_pref(p,scratch,umi_setup_prefs[i].domain,umi_setup_prefs[i].body)))return 100*(2+(int)i)+error;
 umi_data_ark_status=umi_write_data_ark(p,scratch);
 static const char *const containers[]={"/private/var/containers","/private/var/containers/Bundle","/private/var/containers/Data",
  "/private/var/containers/Data/System","/private/var/containers/Shared","/private/var/containers/Shared/SystemGroup",
  "/private/var/containers/Temp"};
 for(unsigned i=0;i<sizeof(containers)/sizeof(containers[0]);++i)
  if((error=umi_mkdir_owned(p,scratch,containers[i],0755,-1)))return 800+error;
 return 0;
}

int sheng_boot_watchdog_policy(void *process, uint64_t scratch) {
 proc_t p=process;
 int enabled=0,error=0,fd=-1;int32_t result=0;
 PE_parse_boot_argn("sheng-disable-watchdogd",&enabled,sizeof(enabled));
 if(!PE_parse_boot_argn("sheng-disable-jobs",jobs,sizeof(jobs)))jobs[0]=0;jobs[sizeof(jobs)-1]=0;
 if(enabled!=1)enabled=0;
 for(const char *c=jobs;*c;++c)if(!((*c>='a'&&*c<='z')||(*c>='A'&&*c<='Z')||(*c>='0'&&*c<='9')||*c=='.'||*c=='-'||*c=='_'||*c==','||
   (*c=='~' && (c==jobs || c[-1]==','))))return EINVAL;
 if(!enabled && !jobs[0])return 0;
 policy[0]=0;strlcat(policy,policy_head,sizeof(policy));
 if(enabled)strlcat(policy,policy_watchdogd,sizeof(policy));
 for(const char *c=jobs;*c;) {
  const char *e=c;while(*e && *e!=',')++e;
  if(e>c) {
   if(strlcat(policy,"<key>",sizeof(policy))>=sizeof(policy))return EINVAL;
   const char *name=c;
   if(*name=='~'){++name;if(strlcat(policy,"com.apple.",sizeof(policy))>=sizeof(policy))return EINVAL;}
   size_t at=strlen(policy);if(at+(size_t)(e-name)+1>=sizeof(policy))return EINVAL;
   memcpy(policy+at,name,(size_t)(e-name));policy[at+(size_t)(e-name)]=0;
   if(strlcat(policy,"</key><true/>",sizeof(policy))>=sizeof(policy))return EINVAL;
  }
  c=*e?e+1:e;
 }
 strlcat(policy,policy_tail,sizeof(policy));
 const char *directory="/private/var/db/com.apple.xpc.launchd";
 const char *path="/private/var/db/com.apple.xpc.launchd/disabled.plist";
 if((error=copyout(directory,scratch,strlen(directory)+1)))goto done;
 struct mkdir_args md={.path=scratch,.mode=0755};
 error=mkdir(p,&md,&result);
 if(error && error!=EEXIST)goto done;
 if((error=copyout(path,scratch,strlen(path)+1)))goto done;
 // With persistent Data the previous boot's file is there: this policy is rewritten in place on every boot (it must follow the
 // boot arguments). Symlinks are still refused (O_NOFOLLOW) and the fstat check below insists on a root-owned regular file.
 struct open_args op={.path=scratch,.flags=O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,.mode=0644};
 if((error=open(p,&op,&result)))goto done;
 fd=result;
 if((error=copyout(policy,scratch+0x200,strlen(policy))))goto done;
 struct write_args wr={.fd=fd,.cbuf=scratch+0x200,.nbyte=strlen(policy)};
 user_ssize_t written=0;
 if((error=write(p,&wr,&written)))goto done;
 if(written!=(user_ssize_t)(strlen(policy))){error=EIO;goto done;}
 struct fchown_args owner={.fd=fd,.uid=0,.gid=0};
 if((error=fchown(p,&owner,&result)))goto done;
 struct fchmod_args mode={.fd=fd,.mode=0644};
 if((error=fchmod(p,&mode,&result)))goto done;
 struct fsync_args sync={.fd=fd};
 if((error=fsync(p,&sync,&result)))goto done;
 // Preserve the cache-mount environment slots at +0x400, +0x500, +0x600.
 struct fstat64_args sa={.fd=fd,.ub=scratch+0x800};struct stat64 st={0};
 if((error=sys_fstat64(p,&sa,&result)) || (error=copyin(sa.ub,&st,sizeof(st))))goto done;
 if(st.st_uid!=0 || st.st_gid!=0 || !S_ISREG(st.st_mode) || (st.st_mode&0777)!=0644 || st.st_size!=(off_t)(strlen(policy)))error=EINVAL;
 done:
 if(fd>=0){struct close_args cl={.fd=fd};int e=sys_close(p,&cl,&result);if(!error)error=e;}
 // A real empty migration marker selects the qualified modern override loader.
 if(!error) {
  const char *marker="/private/var/db/com.apple.xpc.launchd/disabled.migrated";
  error=copyout(marker,scratch,strlen(marker)+1);
  if(!error) {
   struct open_args mark={.path=scratch,.flags=O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,.mode=0644};
   error=open(p,&mark,&result);
   if(!error){struct close_args cl={.fd=result};error=sys_close(p,&cl,&result);}
   else if(error==EEXIST)error=0;
  }
 }
 sheng_watchdog_policy_status=error;
 printf("sheng bring-up watchdogd launch policy: %d (hardware watchdog not implemented)\n",error);
 {
  int skip=0;
  if(PE_parse_boot_argn("umi-skip-setup",&skip,sizeof(skip)) && skip==1) {
   umi_skip_setup_status=umi_skip_setup(p,scratch);
   printf("umi skip setup (purplebuddy SetupDone): %d\n",umi_skip_setup_status);
  }
 }
 if(error==ENOSPC)return 0;
 return error;
}
