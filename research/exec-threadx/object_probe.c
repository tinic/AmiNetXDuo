/* Real created rings and unchanged pinned delete bodies, no scheduler model.
 * SPDX-License-Identifier: MIT */
#include "tx_bridge.h"
#include "tx_thread.h"
#include "tx_mutex.h"
#include "tx_event_flags.h"
#include "object_probe.h"
#include <string.h>
#define CHECK(x) do {if (!(x)) return __LINE__;} while (0)
#ifndef TX_DISABLE_NOTIFY_CALLBACKS
static VOID unsupported_notify(TX_EVENT_FLAGS_GROUP *g) {(void)g;}
#endif
int anx_object_probe(void)
{
    TX_MUTEX m[3],saved,fake;
    TX_EVENT_FLAGS_GROUP g[3],eg,fakeg;
    ULONG actual=0;
    CHECK(!_tx_mutex_created_count && !_tx_mutex_created_ptr &&
          !_tx_event_flags_created_count && !_tx_event_flags_created_ptr);
    memset(m,0xa5,sizeof(m));memset(g,0xa5,sizeof(g));
    for (unsigned i=0;i<3;i++) {
        CHECK(tx_mutex_create(&m[i],(CHAR *)"ring",TX_NO_INHERIT)==TX_SUCCESS &&
              tx_event_flags_create(&g[i],(CHAR *)"ring")==TX_SUCCESS);
    }
    CHECK(_tx_mutex_created_count==3 && _tx_event_flags_created_count==3 &&
          _tx_mutex_created_ptr==&m[0] && _tx_event_flags_created_ptr==&g[0]);
    saved=m[1];eg=g[1];
    CHECK(tx_mutex_create(&m[1],(CHAR *)"duplicate",TX_NO_INHERIT)==TX_MUTEX_ERROR &&
          tx_event_flags_create(&g[1],(CHAR *)"duplicate")==TX_GROUP_ERROR &&
          !memcmp(&saved,&m[1],sizeof(saved)) && !memcmp(&eg,&g[1],sizeof(eg)));
    memset(&fake,0,sizeof(fake));memset(&fakeg,0,sizeof(fakeg));
    fake.tx_mutex_id=TX_MUTEX_ID;fakeg.tx_event_flags_group_id=TX_EVENT_FLAGS_ID;
    CHECK(tx_mutex_delete(&fake)==TX_MUTEX_ERROR && tx_event_flags_delete(&fakeg)==TX_GROUP_ERROR &&
          tx_mutex_delete(TX_NULL)==TX_MUTEX_ERROR && tx_event_flags_delete(TX_NULL)==TX_GROUP_ERROR &&
          tx_mutex_create(TX_NULL,(CHAR *)"null",TX_NO_INHERIT)==TX_MUTEX_ERROR &&
          tx_event_flags_create(TX_NULL,(CHAR *)"null")==TX_GROUP_ERROR);
    CHECK(tx_mutex_get(&m[1],TX_NO_WAIT)==TX_SUCCESS && tx_mutex_get(&m[1],TX_NO_WAIT)==TX_SUCCESS);
    saved=m[1];
    CHECK(tx_mutex_delete(&m[1])==TX_FEATURE_NOT_ENABLED && !memcmp(&saved,&m[1],sizeof(saved)) &&
          tx_mutex_put(&m[1])==TX_SUCCESS && tx_mutex_delete(&m[1])==TX_FEATURE_NOT_ENABLED &&
          tx_mutex_put(&m[1])==TX_SUCCESS);
    CHECK(tx_event_flags_set(&g[1],7,TX_OR)==TX_SUCCESS &&
          tx_event_flags_get(&g[1],7,TX_AND_CLEAR,&actual,TX_NO_WAIT)==TX_SUCCESS && actual==7);
#ifndef TX_DISABLE_NOTIFY_CALLBACKS
    /* Retirement must refuse an installed callback; it is never invoked. */
    g[1].tx_event_flags_group_set_notify=unsupported_notify;
    eg=g[1];
    CHECK(tx_event_flags_delete(&g[1])==TX_FEATURE_NOT_ENABLED && !memcmp(&eg,&g[1],sizeof(eg)));
    g[1].tx_event_flags_group_set_notify=TX_NULL;
#endif
    _tx_thread_preempt_disable++;
    CHECK(tx_mutex_delete(&m[1])==TX_SUCCESS && tx_event_flags_delete(&g[1])==TX_SUCCESS &&
          _tx_thread_preempt_disable==1 && _tx_mutex_created_count==2 && _tx_event_flags_created_count==2);
    CHECK(m[0].tx_mutex_created_next==&m[2] && m[2].tx_mutex_created_previous==&m[0] &&
          g[0].tx_event_flags_group_created_next==&g[2] && g[2].tx_event_flags_group_created_previous==&g[0]);
    CHECK(tx_mutex_delete(&m[0])==TX_SUCCESS && tx_event_flags_delete(&g[0])==TX_SUCCESS &&
          _tx_mutex_created_ptr==&m[2] && _tx_event_flags_created_ptr==&g[2] &&
          m[2].tx_mutex_created_next==&m[2] && m[2].tx_mutex_created_previous==&m[2] &&
          g[2].tx_event_flags_group_created_next==&g[2] && g[2].tx_event_flags_group_created_previous==&g[2]);
    CHECK(tx_mutex_delete(&m[2])==TX_SUCCESS && tx_event_flags_delete(&g[2])==TX_SUCCESS &&
          _tx_thread_preempt_disable==1 && !_tx_mutex_created_count && !_tx_mutex_created_ptr &&
          !_tx_event_flags_created_count && !_tx_event_flags_created_ptr);
    _tx_thread_preempt_disable--;
    CHECK(tx_mutex_delete(&m[2])==TX_MUTEX_ERROR && tx_event_flags_delete(&g[2])==TX_GROUP_ERROR &&
          tx_mutex_get(&m[2],TX_NO_WAIT)==TX_MUTEX_ERROR &&
          tx_event_flags_set(&g[2],1,TX_OR)==TX_GROUP_ERROR &&
          !m[0].tx_mutex_id && !m[1].tx_mutex_id && !m[2].tx_mutex_id &&
          !g[0].tx_event_flags_group_id && !g[1].tx_event_flags_group_id && !g[2].tx_event_flags_group_id);
    /* Only after actual deletion is storage poisoned and recreated. */
    memset(&m[2],0x5a,sizeof(m[2]));memset(&g[2],0x5a,sizeof(g[2]));
    CHECK(tx_mutex_create(&m[2],(CHAR *)"reuse",TX_NO_INHERIT)==TX_SUCCESS &&
          tx_event_flags_create(&g[2],(CHAR *)"reuse")==TX_SUCCESS &&
          tx_mutex_get(&m[2],TX_NO_WAIT)==TX_SUCCESS && tx_mutex_put(&m[2])==TX_SUCCESS &&
          tx_event_flags_set(&g[2],1,TX_OR)==TX_SUCCESS &&
          tx_event_flags_get(&g[2],1,TX_OR_CLEAR,&actual,TX_NO_WAIT)==TX_SUCCESS && actual==1 &&
          tx_mutex_delete(&m[2])==TX_SUCCESS && tx_event_flags_delete(&g[2])==TX_SUCCESS);
    return 0;
}
