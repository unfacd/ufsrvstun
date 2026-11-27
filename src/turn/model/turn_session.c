#include <pthread.h>
#include <syslog.h>
#include <uflib/recycler/recycler.h>

#include "turn/model/turn_allocation.h"

static RecyclerPoolHandle *session_pool;
static pthread_mutex_t     session_pool_lock = PTHREAD_MUTEX_INITIALIZER;

bool
TurnSessionPoolInit(size_t groups, size_t sessions_per_group)
{
  if (session_pool) return true;
  if (!groups || !sessions_per_group) {
    syslog(LOG_ERR, "%s: unusable pool geometry {groups:'%zu', sessions_per_group:'%zu'}", __func__, groups,
           sessions_per_group);
    return false;
  }

  RecyclerPoolOps ops = {0};
  session_pool = RecyclerInitTypePool("TurnModelSession", sizeof(TurnModelSession), groups, sessions_per_group, &ops);
  if (!session_pool)
    syslog(LOG_ERR, "%s: could not allocate the TURN session pool {groups:'%zu', sessions_per_group:'%zu'}", __func__,
           groups, sessions_per_group);
  return session_pool != NULL;
}

TurnModelSession *
TurnSessionForFlow(UdpServer *server, UdpFlowHandle flow)
{
  InstanceHolder *holder = UdpServerFlowSession(server, flow);
  return holder ? (TurnModelSession*)RecyclerGetClientContextData(holder) : NULL;
}

TurnModelSession *
TurnSessionAttach(UdpServer *server, UdpFlowHandle flow, const TurnPrincipalId *principal)
{
  if (!session_pool || !server) {
    syslog(LOG_ERR, "%s: %s", __func__, session_pool ? "no UDP server" : "session pool not initialised");
    return NULL;
  }

  pthread_mutex_lock(&session_pool_lock);
  InstanceHolder *holder = RecyclerGet(session_pool->type, NULL, 0);
  pthread_mutex_unlock(&session_pool_lock);
  if (!holder) {
    syslog(LOG_WARNING, "%s: session pool exhausted", __func__);
    return NULL;
  }

  TurnModelSession *session = (TurnModelSession*)RecyclerGetClientContextData(holder);
  *session                  = (TurnModelSession)
  {
    .principal = *principal
  };
  if (UdpServerAttachSession(server, flow, holder)) return session;

  TurnSessionRelease(flow, holder);
  return NULL;
}

void
TurnSessionRelease(UdpFlowHandle flow, InstanceHolder *holder)
{
  (void)flow;
  TurnModelSession *session = (TurnModelSession*)RecyclerGetClientContextData(holder);
  TurnAllocationClose(session);
  *session = (TurnModelSession)
  {
    0
  };

  pthread_mutex_lock(&session_pool_lock);
  int result = RecyclerPut(session_pool->type, holder, NULL, 0);
  pthread_mutex_unlock(&session_pool_lock);
  if (result != 0) syslog(LOG_ERR, "Could not return TURN session to recycler (%d)", result);
}
