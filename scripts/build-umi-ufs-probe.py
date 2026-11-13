#!/usr/bin/env python3
"""Build the gated umi storage bring-up using the official production transport."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'build/umi-ufs-probe'
out.mkdir(parents=True,exist_ok=True)
source=ROOT/'kits/umi/UmiUfsProbe.cpp'
upstream=ROOT/'drivers/NativeUfs'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
original=(upstream/'NativeUfs.cpp').read_text()
start=original.index('struct UfsHost {')
end=original.index('// Bring the live controller up for reading:',start)
(out/'UmiUfsTransport.inc').write_text(
    '// SPDX-License-Identifier: BSD-2-Clause-Patent\n'
    '#include "RootTransportLayout.h"\n'+original[start:end])
commands=(upstream/'RootCommands.h').read_text()
boundary=commands.index('// ---- Persistent store window')
(out/'RootCommands.h').write_text(commands[:boundary])
assert 'hostWrite4k (' not in (out/'RootCommands.h').read_text()
for name in ('RootTransportLayout.h','RootDiagnostics.h','RootQueue.h',
             'TransferPoll.h','RootLayout.h','LICENSE'):
    shutil.copyfile(upstream/name,out/name)
# Exercise the actual two-stream planner before compiling a phone artifact.
host_test=ROOT/'kits/umi/ufs-tests/UmiSmmuTakeover.cpp'
subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                str(host_test),'-o',str(out/'smmu-host-test')],check=True)
with (out/'smmu-host-test.log').open('w') as log:
    subprocess.run([str(out/'smmu-host-test')],stdout=log,stderr=subprocess.STDOUT,check=True)
record=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
phone=os.environ.get('UMI_EARLY_TRACE','0')
assert phone in ('0','1') and os.environ.get('UMI_UFS_PROBE')=='1'
args=record['arguments']
command=[]
i=0
obj=out/'UmiUfsProbe.o'
while i<len(args):
    v=args[i]
    if v==record['file']: command.append(str(source))
    elif v=='-o': command.extend(['-o',str(obj)]); i+=1
    elif v in ('-MF','-MJ'): i+=1
    elif v=='-Xclang' and args[i+1].startswith('-W'): i+=1
    elif not v.startswith('-W') and v not in ('-MD','-MP'): command.append(v)
    i+=1
command+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter',
 '-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion',
 '-mgeneral-regs-only',f'-DUMI_UFS_PHONE={phone}','-I',str(ROOT/'.cache/xnu-work/osfmk'),
 '-I',str(out)]
(out/'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
r=subprocess.run(command,cwd=record['directory'],text=True,capture_output=True)
(out/'compile.log').write_text(r.stdout+r.stderr)
if r.returncode: raise SystemExit(r.stdout+r.stderr)
text=subprocess.check_output(['/usr/lib/llvm-18/bin/llvm-objdump','-d',str(obj)],text=True)
(out/'disassembly.txt').write_text(text)
operands='\n'.join(re.findall(r'^[ \t]*[0-9a-f]+:[ \t]+[0-9a-f]{8}[ \t]+\w+[ \t]+([^\n]+)',text,re.M))
assert not re.search(r'\b(?:[vqds][0-9]+)\b',operands), 'Unexpected FP/SIMD register'
assert not re.search(r'\bmsr\s+(?:tcr|ttbr|sctlr)',operands,re.I), 'Translation mutation'
archive=out/'libUmiUfsProbe.a'
archive.unlink(missing_ok=True)
subprocess.run(['/usr/lib/llvm-18/bin/llvm-ar','rcs',str(archive),str(obj)],check=True)
report={'phone':phone=='1','source_sha256':sha(source),'object_sha256':sha(obj),
 'archive_sha256':sha(archive),'script_sha256':sha(Path(__file__)),
 'actual_disassembly_checked':True,'disk_write_path':False,'dma_submission_path':phone=='1',
 'contract_version':2,'smmu_host_test_passed':True,
 'sources':{name:sha(ROOT/'kits/umi'/name) for name in
   ('UmiUfsProbe.cpp','UmiUfsRead.inc','UmiSmmuTakeover.h','ufs-tests/UmiSmmuTakeover.cpp')},
 'upstream_sources':{name:sha(upstream/name) for name in
   ('NativeUfs.cpp','RootCommands.h','RootTransportLayout.h','RootDiagnostics.h',
    'RootQueue.h','TransferPoll.h','RootLayout.h','LICENSE')},
 'generated_sources':{name:sha(out/name) for name in
   ('UmiUfsTransport.inc','RootCommands.h')},
 'qemu_hardware_probe':'skipped, never emulated','hardware_tested':False}
(out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
