#!/usr/bin/env python3
"""Read the actual helper's V1/V2/V3 snapshots without crossing port versions."""
from pathlib import Path
import hashlib,json,os,re,shlex,subprocess,sys
root=Path(__file__).resolve().parents[3];build=Path(sys.argv[1]).resolve();build.mkdir(parents=True,exist_ok=True)
source=(root/'tests/sana2/gchildstat.c').read_text()
source=re.sub(r'^#include .*\n','',source,flags=re.M).replace('int main(void)','int helper_main(void)')
prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t ULONG;typedef int BOOL;typedef const char *CONST_STRPTR;
struct MsgPort {void *mp_SigTask;};
static unsigned version,depth,bad,unsafe,copies;static ULONG child[6],*clock_data;static size_t clock_size;
static struct MsgPort child_port,clock_port;
static void Forbid(void){depth++;}
static void Permit(void){depth--;}
static struct MsgPort *FindPort(CONST_STRPTR name){
 if(depth!=1)unsafe++;
 if(!strcmp(name,"AmiNetXDuo gchild"))return version?&child_port:NULL;
 if((version==1&&!strcmp(name,"AmiNetXDuo eclock"))||
    (version==2&&!strcmp(name,"AmiNetXDuo eclock2"))||
    (version==3&&!strcmp(name,"AmiNetXDuo eclock3")))return &clock_port;
 return NULL;
}
static void CopyMem(const void *src,void *dst,ULONG n){
 size_t expected=src==child?sizeof child:clock_size;
 if(depth!=1)unsafe++;copies++;
 if(n!=expected)bad++;
 /* Record a wrong length as a semantic failure before a deliberate negative
    could read beyond the fake port. ASan still checks the positive copies. */
 memcpy(dst,src,n>expected?expected:n);
}
'''
main=r'''
int main(int argc,char **argv){
 if(argc!=2)return 2;version=(unsigned)atoi(argv[1]);
 ULONG values[11]={715909,4296,13603,0,0,6,123,4,128,50,2};
 clock_size=(version==1?5:version==2?6:11)*sizeof(ULONG);clock_data=malloc(clock_size);
 if(!clock_data)return 2;memcpy(clock_data,values,clock_size);clock_port.mp_SigTask=clock_data;child_port.mp_SigTask=child;
 int rc=helper_main();unsigned failures=0;
 failures+=(rc!=(version?0:5));failures+=(depth!=0||unsafe!=0||bad!=0);failures+=(copies!=(version==0?0:version==4?1:2));
 free(clock_data);printf("snapshot checks=3 failures=%u\n",failures);return failures?1:0;
}
'''
results={}
for variant in ['fixed','oversized-copy-negative']:
 body=source
 if variant!='fixed':
  body,n=re.subn(r'clock_v3 \? sizeof\(clock\) :\s*\(clock_v2 \? 6UL : 5UL\) \* sizeof\(ULONG\)','sizeof(clock)',body);assert n==1
 text=prelude+body+main;c=build/(variant+'.c');exe=build/variant;c.write_text(text)
 cmd=[os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-O1','-g']+shlex.split(os.environ.get('CFLAGS',''))
 if '--sanitize' in sys.argv:cmd+=['-fsanitize=address,undefined','-fno-sanitize-recover=all']
 elif '--ubsan' in sys.argv:cmd+=['-fsanitize=undefined','-fno-sanitize-recover=all']
 subprocess.run(cmd+[str(c),'-o',str(exe)],check=True)
 for version in (range(5) if variant=='fixed' else [1,2]):
  r=subprocess.run([str(exe),str(version)],capture_output=True,text=True);want=0 if variant=='fixed' else 1
  assert r.returncode==want and f'snapshot checks=3 failures={want}' in r.stdout,(variant,version,r.stdout,r.stderr)
  if variant=='fixed':
   assert ('gclockwait startup_waits=6' in r.stdout)==(version in [2,3])
   assert ('gclockprobe attempts=123 stride=4 frame_cap=128 skipped=50 forced=2' in r.stdout)==(version==3)
   assert ('gclockstat hz=715909' in r.stdout)==(version in [1,2,3])
  key=f'{variant}-v{version}';(build/(key+'.log')).write_text(r.stdout+r.stderr)
  results[key]={'exit':r.returncode,'checks':3,'failures':want,'source_sha256':hashlib.sha256(text.encode()).hexdigest(),'output':r.stdout};print(key,r.stdout)
(build/'results.json').write_text(json.dumps(results,indent=2)+'\n')
