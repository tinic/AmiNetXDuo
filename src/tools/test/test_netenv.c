/* Real projection/publication transitions, without AmigaOS or a new harness.
 * SPDX-License-Identifier: MIT */
#include "netenv_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, failures, writes, deletes;
static NeSnapshot s;
static NeTable t;
static NeOwned owned;
static struct { char key[32]; char *value; } env[NE_KEY_MAX];
static const char *fail_key;
static int fail_delete;
static void check(int ok, const char *label)
{
    checks++;
    if (!ok) { failures++; printf("FAIL: %s\n", label); }
}
static const char *get(const char *key)
{
    unsigned i;
    for (i=0;i<NE_KEY_MAX;i++) if (env[i].value && !strcmp(env[i].key,key)) return env[i].value;
    return NULL;
}
static int read_env(void *ctx, const char *key, char *buf, size_t cap)
{
    const char *value=get(key);
    (void)ctx;
    if (!value || strlen(value)>=cap) return 0;
    strcpy(buf,value); return 1;
}
static int write_env(void *ctx, const char *key, const char *value)
{
    unsigned i, slot=NE_KEY_MAX;
    (void)ctx;
    if (fail_key && !strcmp(fail_key,key) && (!!value != fail_delete)) return 0;
    for(i=0;i<NE_KEY_MAX;i++) {
        if (!env[i].value) { if(slot==NE_KEY_MAX)slot=i; }
        else if(!strcmp(env[i].key,key)) {slot=i;break;}
    }
    if(!value) {
        if(i<NE_KEY_MAX) { free(env[i].value);env[i].value=NULL;deletes++; }
        return 1;
    }
    if(slot==NE_KEY_MAX)return 0;
    free(env[slot].value);
    env[slot].value=malloc(strlen(value)+1);
    if(!env[slot].value)abort();
    strcpy(env[slot].key,key);strcpy(env[slot].value,value);writes++;return 1;
}
static void value(const char *key,const char *want)
{
    const char *got=ne_value(&t,key);
    check(want ? got && !strcmp(got,want) : !got,key);
}
static void build(void) { check(ne_build(&s,&t),"build complete snapshot"); }
static void publish(void) { check(ne_publish(&owned,&t,read_env,write_env,NULL),"publish"); }
static void setup(void)
{
    memset(&s,0,sizeof(s));s.state="UP";s.interfaces=2;
    strcpy(s.iface[0].name,"wifi0");s.iface[0].index=3;s.iface[0].up=1;
    s.iface[0].address=0xc0a80202;s.iface[0].mask=0xffffff00;
    strcpy(s.iface[1].name,"eth0");s.iface[1].index=1;s.iface[1].up=1;
    s.iface[1].address=0xc0a80102;s.iface[1].mask=0xffffff00;
    s.routes=1;s.route[0].index=1;strcpy(s.route[0].gateway,"192.168.1.1");
    s.addresses6=3;
    strcpy(s.addr6[0].text,"fe80::abcd");s.addr6[0].index=1;s.addr6[0].prefix=64;s.addr6[0].scoped=1;s.addr6[0].ready=1;
    strcpy(s.addr6[1].text,"2001:db8::1234");s.addr6[1].index=1;s.addr6[1].prefix=64;s.addr6[1].ready=1;
    strcpy(s.addr6[2].text,"2001:db8::bad");s.addr6[2].index=1;s.addr6[2].prefix=64;
}
int main(void)
{
    unsigned before, i;
    NeAddress6 swap;
    setup();build();
    value("ANXD/INTERFACES","eth0 wifi0");
    value("ANXD/IF/eth0/ADDRESS","192.168.1.2");
    value("ANXD/IF/wifi0/ADDRESS","192.168.2.2");
    value("ANXD/IF/eth0/NETMASK","255.255.255.0");
    value("ANXD/IF/eth0/ADDR6","2001:db8::1234/64 fe80::abcd%eth0/64");
    value("ANXD/DEFAULT/INTERFACE","eth0");value("ANXD/DEFAULT/GATEWAY","192.168.1.1");
    value("ANXD/DEFAULT6/GATEWAY",NULL);
    publish();before=writes;publish();check(writes==before,"unchanged snapshot: zero writes");
    swap=s.addr6[0];s.addr6[0]=s.addr6[1];s.addr6[1]=swap;build();publish();
    check(writes==before,"address list reordering: zero writes");
    s.iface[1].mask=0xffff0000;build();publish();check(writes==before+2,"mask-only change updates interface and default");
    s.route[0].index=3;strcpy(s.route[0].gateway,"192.168.2.1");build();publish();
    value("ANXD/DEFAULT/INTERFACE","wifi0");value("ANXD/DEFAULT/ADDRESS","192.168.2.2");
    s.routes=2;s.route[1].index=1;s.route[1].ipv6=1;s.route[1].scoped=1;
    strcpy(s.route[1].gateway,"fe80::1");build();value("ANXD/DEFAULT6/GATEWAY","fe80::1%eth0");
    value("ANXD/DEFAULT6/INTERFACE","eth0");publish();
    s.routes=3;s.route[2]=s.route[1];strcpy(s.route[2].gateway,"fe80::2");build();publish();
    value("ANXD/DEFAULT6/GATEWAY",NULL);check(!get("ANXD/DEFAULT6/GATEWAY"),"ambiguous IPv6 default withdrawn");
    s.route[2]=s.route[1];build();value("ANXD/DEFAULT6/GATEWAY","fe80::1%eth0");
    s.routes=0;build();publish();value("ANXD/DEFAULT/ADDRESS",NULL);
    s.iface[1].address=0;build();publish();value("ANXD/IF/eth0/ADDRESS",NULL);
    value("ANXD/IF/eth0/STATE","UP"); /* IPv6-only is still up. */
    s.addresses6=0;build();value("ANXD/IF/eth0/STATE","CONFIGURING");
    s.iface[1].up=0;build();publish();value("ANXD/IF/eth0/STATE","DOWN");
    value("ANXD/IF/eth0/ADDR6",NULL);
    s.interfaces=1;build();publish();check(!get("ANXD/IF/eth0/STATE"),"removed interface cleaned up");
    /* Failed writes are retried; successfully written siblings are not rewritten. */
    s.iface[0].address=0xc0a80203;build();fail_key="ANXD/IF/wifi0/ADDRESS";
    check(!ne_publish(&owned,&t,read_env,write_env,NULL),"write failure surfaced");
    before=writes;fail_key=NULL;publish();check(writes==before+1,"only failed write retried");
    fail_key="ANXD/IF/wifi0/ADDRESS";fail_delete=1;s.state="DOWN";build();
    check(!ne_publish(&owned,&t,read_env,write_env,NULL),"delete failure surfaced");
    check(get(fail_key)!=NULL,"failed deletion retained for retry");
    fail_key=NULL;publish();check(!get("ANXD/IF/wifi0/ADDRESS"),"delete retried after stack stop");
    value("ANXD/STATE","DOWN");value("ANXD/INTERFACES","");
    s.state="ERROR";build();publish();value("ANXD/STATE","ERROR");
    t.count=t.used=0;publish();check(owned.count==0,"normal exit relinquishes all keys");
    for(i=0;i<NE_KEY_MAX;i++)check(!env[i].value,"no variables left on exit");
    setup();strcpy(s.iface[0].name,"123456789012345");build();
    for(i=0;i<t.count;i++)check(strlen(t.entry[i].name)<=31,"old DOS variable-name limit");
    strcpy(s.iface[0].name,"../escape");check(!ne_build(&s,&t),"unsafe interface name refused");
    setup();s.addresses6=NE_ADDR6_MAX+1;check(!ne_build(&s,&t),"oversized snapshot refused");
    setup();s.addr6[0].prefix=129;check(!ne_build(&s,&t),"bad IPv6 prefix refused");
    setup();s.iface[0]=s.iface[1];check(!ne_build(&s,&t),"duplicate interface name refused");
    setup();strcpy(s.iface[0].name,"ETH0");
    check(!ne_build(&s,&t),"case-insensitive ENV name collision refused");
    setup();s.iface[0].up=0;s.iface[0].address=0;build();
    value("ANXD/IF/wifi0/ADDRESS",NULL);value("ANXD/IF/wifi0/STATE","DOWN");
    setup();s.addresses6=NE_ADDR6_MAX;
    for(i=0;i<NE_ADDR6_MAX;i++) {
        s.addr6[i]=s.addr6[0];
        snprintf(s.addr6[i].text,sizeof(s.addr6[i].text),"2001:db8::%x",i+1);
    }
    build();check(strlen(ne_value(&t,"ANXD/IF/eth0/ADDR6"))>2000,
                  "complete large IPv6 list, no truncation");
    t.count=t.used=0;
    check(ne_own(&owned,"ANXD/IF/old0/ADDRESS"),"recover previous instance key");
    check(write_env(NULL,"ANXD/IF/old0/ADDRESS","10.0.0.2"),"old ENV value");
    publish();check(!get("ANXD/IF/old0/ADDRESS"),"recovered stale key removed");
    printf("NetEnv: %u checks, %u failures; %u writes, %u deletions\n",checks,failures,writes,deletes);
    return failures ? 1 : 0;
}
