/*************************************************************************
Copyright (C) 2010 Nokia Corporation.

These OHM Modules are free software; you can redistribute
it and/or modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation
version 2.1 of the License.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
USA.
*************************************************************************/


/*! \defgroup pubif Public Interfaces */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>

#include <glib/gmain.h>
#include <res-conn.h>

#include "plugin.h"
#include "proxy.h"
#include "dbusif.h"
#include "ruleif.h"
#include "resource.h"
#include "longlive.h"

/*
 * these should match their counterpart
 * in duirealngfclient_p.h
 */
#define NGF_COMPLETED        0
#define NGF_FAILED           1
#define NGF_BUSY             (1 << 1)
#define NGF_LONG             (1 << 2)
#define NGF_SHORT            (1 << 3)

#define MSEC                 1
#define SECOND               1000
#define MINUTE               (60 * SECOND)
#define HOUR                 (60 * MINUTE_IN_SECS)
#define DEFAULT_PLAY_LIMIT   (5 * MINUTE)

/* ID based hashing */
#define ID_HASH_BITS      6
#define ID_HASH_DIM       (1 << ID_HASH_BITS)
#define ID_HASH_MASK      (ID_HASH_DIM - 1)
#define ID_HASH_INDEX(i)  (i & ID_HASH_MASK) 

/* client based hashing */
#define CL_HASH_BITS      8
#define CL_HASH_DIM       (1 << CL_HASH_BITS)
#define CL_HASH_MASK      (CL_HASH_DIM - 1)
#define CL_HASH_INDEX(i)  (i & CL_HASH_MASK) 


typedef enum {
    state_created = 0,          /* just created after a play request */
    state_acquiring,            /* waiting for grant after acquiring */
    state_forwarded,            /* after play request forwarded to backend */
    state_completed,            /* after reciving status from backend */
    state_stopped,              /* client stop request or lost resources */
    state_killed,               /* client died accrding to D-Bus */
    state_longlive,             /* if only longlive resources are in use */
} proxy_state_t;

typedef enum {
    resource_grant = 0,         /* resource grant from policy */
    backend_status,             /* backend status message */
    backend_update,             /* backend update message */
    backend_timeout,            /* backend timeout ie. no status message */
    client_stop,                /* client stop request */
    client_died,                /* client D-Bus connetion is down */
} proxy_event_t;


typedef struct proxy_s {
    struct proxy_s  *idnext;    /* chain for id hashes */
    struct proxy_s  *clnext;    /* chain for client hashes */
    int              type;      /* notification type */
    uint32_t         id;        /* system-wide unique play request ID */
    const char      *client;    /* client's D-Bus address */
    uint32_t         longlive;  /* wheter we have longlive resources or not */
    proxy_state_t    state;     /* stages of play request processing */
    uint32_t         resources; /* bitmap of regular resources we own */
    uint32_t         timeout;   /* timer ID or zero if no timer is set */
    void            *data;      /* play data */
    uint32_t         status;    /* status to reply */
} proxy_t;


static uint32_t      seqno = 1;              /* serial # for unique notif.ID */
static proxy_t      *idhashtbl[ID_HASH_DIM]; /* for ID based access */
static proxy_t      *clhashtbl[CL_HASH_DIM]; /* for client based access */
static uint32_t      play_limit;             /* notif. play limit in msec's */
static uint32_t      play_timeout;           /* max.time for a play request */
static uint32_t      stop_timeout;           /* max.time for a stop request */

static proxy_t *proxy_create(uint32_t, const char *, void *);
static void     proxy_destroy(proxy_t *);

static void     id_hash_add(proxy_t *);
static void     id_hash_delete(proxy_t *);
static proxy_t *id_hash_lookup(uint32_t);

static uint32_t cl_hash_index(const char *);
static void     cl_hash_add(proxy_t *);
static void     cl_hash_delete(proxy_t *);
static proxy_t *cl_hash_lookup_proxy(const char *, void **);
static int      cl_hash_lookup_client(const char *);

static void     timeout_create(proxy_t *, uint32_t);
static void     timeout_destroy(proxy_t *);
static uint32_t timeout_handler(proxy_t *);

static void     grant_handler(uint32_t, void *);

static int evaluate_notification_request_rules(const char *, uint32_t *,
                                               uint32_t *, uint32_t *, char *);

static int state_machine(proxy_t *, proxy_event_t, void *);

static int forward_play_request_to_backend(proxy_t *, uint32_t, uint32_t);
static int forward_stop_request_to_backend(proxy_t *, void *);
static int create_and_send_stop_request_to_backend(proxy_t *);
static int forward_status_to_client(proxy_t *, void *);
static int partial_release_of_resources(proxy_t *, void *);
static int create_and_send_status_to_client(proxy_t *, uint32_t);
static int stop_if_loose_resources(proxy_t *, uint32_t);
static int premature_stop(proxy_t *);
static int playback_partial_end(proxy_t *);
static int fake_backend_timeout(proxy_t *);

static uint32_t play_status(proxy_t *, uint32_t);

static const char *state_str(proxy_state_t);
static const char *event_str(proxy_event_t);


/*! \addtogroup pubif
 *  Functions
 *  @{
 */

void proxy_init(OhmPlugin *plugin)
{
    uint32_t    limit;
    const char *limit_str;
    char       *e;

    if ((limit_str = ohm_plugin_get_param(plugin, "play-limit")) == NULL)
        play_limit = DEFAULT_PLAY_LIMIT;
    else {
        limit = strtol(limit_str, &e, 10);

        if (limit > 0 && *e == '\0')
            play_limit = limit * SECOND;
        else {
            OHM_ERROR("notification: Invalid value '%s' for 'play-limit'",
                      limit_str);
            play_limit = DEFAULT_PLAY_LIMIT;
        }
    }

    play_timeout = play_limit + 30 * SECOND;
    stop_timeout = 10 * SECOND;
}

int proxy_playback_request(const char *event,   /* eg. ringtone, alarm, etc */
                           const char *client,  /* client's D-Bus address */
                           void       *data,    /* incoming message */
                           char       *err)     /* ptr to a error msg buffer */
{
    uint32_t       mand   = 0;
    uint32_t       opt    = 0;
    uint32_t       lliv   = 0;
    proxy_t       *proxy  = NULL;
    uint32_t       status = NGF_COMPLETED;
    uint32_t       proxid;
    proxy_state_t  state;
    int            type;
    int            success;

    do { /* not a loop */

        type = evaluate_notification_request_rules(event,&mand,&opt,&lliv,err);

        if (type >= 0)
            state  = state_acquiring;
        else {
            state = state_created;
            mand  = opt = lliv = 0;

            if (!strcmp(err, "Busy")) {
                /* reply with status 'busy' */
                status = NGF_BUSY;
            }
            else {
                /* reply with error */
                break;
            }
        }

        proxid = NOTIFICATION_ID(regular_id, seqno);

        if ((proxy = proxy_create(proxid, client, data)) == NULL) {
            strncpy(err, "internal proxy error", DBUS_DESCBUF_LEN);
            err[DBUS_DESCBUF_LEN-1] = '\0';
            break;
        }

        proxy->type     = type;
        proxy->longlive = lliv;
        proxy->state    = state;
        proxy->status   = status;

        if (!(mand | opt)) {
            if (!lliv)
                timeout_create(proxy, 0);
        }
        else {
            success = resource_set_acquire(type, rset_regular, mand, opt,
                                           grant_handler, proxy);
            if (!success) {
                strncpy(err, "recource acquisition failed", DBUS_DESCBUF_LEN);
                err[DBUS_DESCBUF_LEN-1] = '\0';
                break;
            }
        }

        if (lliv)
            longlive_playback_request(type, lliv);

        seqno++;
        
        return proxid;

    } while(0);

    /* something failed */

    proxy_destroy(proxy);

    return 0;
}


int proxy_stop_request(uint32_t id, const char *client, void *data, char *err)
{
    proxy_t *proxy;
    int      success = FALSE;

    if ((proxy = id_hash_lookup(id)) == NULL) {
        OHM_DEBUG(DBG_PROXY, "can't find proxy (id %u) for stop request", id);
        strncpy(err, "can't find corresponding play request",DBUS_DESCBUF_LEN);
    }
    else if (strcmp(client, proxy->client)) {
        OHM_DEBUG(DBG_PROXY, "refusing to accept request from non-owner: owner"
                  " is '%s' while requestor was '%s'", proxy->client, client);
        strncpy(err, "requests is from diferrent client", DBUS_DESCBUF_LEN);
    }
    else {
        if (proxy->longlive) {
            proxy->longlive = 0;
            longlive_stop_request(proxy->type);
        }

        success = state_machine(proxy, client_stop, data);

        if (!success)
            strncpy(err, "can't accept in the current stage",DBUS_DESCBUF_LEN);
    }

    err[DBUS_DESCBUF_LEN-1] = '\0';

    return success;
}


int proxy_status_request(uint32_t id, void *data)
{
    proxy_t *proxy;
    int      success = FALSE;

    if ((proxy = id_hash_lookup(id)) == NULL)
        OHM_DEBUG(DBG_PROXY, "can't find proxy (id %u) for status request",id);
    else {
        success = TRUE;

        if (!state_machine(proxy, backend_status, data)) {
            OHM_DEBUG(DBG_PROXY, "ignoring out of sequence status "
                      "message (id %u)", id);
        }
    }

    return success;
}

int proxy_update_request(uint32_t id, void *data)
{
    proxy_t *proxy;
    int      success = FALSE;

    if ((proxy = id_hash_lookup(id)) == NULL)
        OHM_DEBUG(DBG_PROXY, "can't find proxy (id %u) for update request",id);
    else {
        success = TRUE;

        if (!state_machine(proxy, backend_update, data)) {
            OHM_DEBUG(DBG_PROXY, "ignoring out of sequence update "
                      "message (id %u)", id);
        }
    }

    return success;
}

void proxy_client_is_down(const char *client)
{
    proxy_t *proxy;
    void    *cur = NULL;

    while ((proxy = cl_hash_lookup_proxy(client, &cur)) != NULL) {
        state_machine(proxy, client_died, NULL);
    }
}

void proxy_backend_is_down(void)
{
    proxy_t *proxy;
    int      i;

    for (i = 0;  i < ID_HASH_DIM;  i++) {
        while ((proxy = idhashtbl[i]) != NULL) {
            timeout_destroy(proxy);
            timeout_handler(proxy);
        }
    }
}

/*!
 * @}
 */

static proxy_t *proxy_create(uint32_t id, const char *client, void *data)
{
    proxy_t *proxy;

    if ((proxy = malloc(sizeof(proxy_t))) != NULL) {
        memset(proxy, 0, sizeof(proxy_t));
        proxy->type   = -1;     /* invalid type */
        proxy->id     = id;
        proxy->client = strdup(client);
        proxy->state  = state_created; 
        proxy->data   = dbusif_engage_data(data);

        if (!cl_hash_lookup_client(client))
            dbusif_monitor_client(client, TRUE);

        id_hash_add(proxy);
        cl_hash_add(proxy);

        OHM_DEBUG(DBG_PROXY, "proxy object created (id %u)", id);
    }

    return proxy;
}


static void proxy_destroy(proxy_t *proxy)
{
    if (proxy != NULL) {
        OHM_DEBUG(DBG_PROXY, "proxy object will be destroyed (id %u)",
                  proxy->id);

        if (proxy->longlive)
            longlive_stop_request(proxy->type);

        timeout_destroy(proxy);
        resource_set_release(proxy->type, rset_regular, grant_handler, proxy);

        id_hash_delete(proxy);
        cl_hash_delete(proxy);

        if (!cl_hash_lookup_client(proxy->client))
            dbusif_monitor_client(proxy->client, FALSE);

        free((void *)proxy->client);
        dbusif_free_data(proxy->data);

        free(proxy);
    }
}


static void id_hash_add(proxy_t *proxy)
{
    uint32_t idx = ID_HASH_INDEX(proxy->id);

    proxy->idnext  = idhashtbl[idx];
    idhashtbl[idx] = proxy;
}

static void id_hash_delete(proxy_t *proxy)
{
    uint32_t  idx = ID_HASH_INDEX(proxy->id);
    proxy_t  *prev;

    for (prev = (proxy_t *)&idhashtbl[idx];
         prev->idnext != NULL;
         prev = prev->idnext)
    {
        if (prev->idnext == proxy) {
            prev->idnext  = proxy->idnext;
            proxy->idnext = NULL;
            return;
        }
    }
}

static proxy_t *id_hash_lookup(uint32_t id)
{
    uint32_t  idx = ID_HASH_INDEX(id);
    proxy_t  *proxy;

    for (proxy = idhashtbl[idx];  proxy;  proxy = proxy->idnext) {
        if (id == proxy->id)
            return proxy;
    }

    return NULL;
}

static uint32_t cl_hash_index(const char *client)
{
    uint32_t    idx = 0;
    const char *numstr;
    uint32_t    num;
    
    if (client != NULL) {
        if ((numstr = strchr(client, '.')) != NULL)
            numstr++;
        else
            numstr = client;

        num = strtoul(numstr, NULL, 10);
        idx = CL_HASH_INDEX(num);
    }

    return idx;
}

static void cl_hash_add(proxy_t *proxy)
{
    uint32_t  idx = cl_hash_index(proxy->client);

    proxy->clnext  = clhashtbl[idx];
    clhashtbl[idx] = proxy;
}


static void cl_hash_delete(proxy_t *proxy)
{
    uint32_t  idx = cl_hash_index(proxy->client);
    proxy_t  *prev;

    if (proxy != NULL) {
        if ((prev = clhashtbl[idx]) == proxy) {
            clhashtbl[idx] = proxy->clnext;
            proxy->clnext = NULL;
        }
        else {
            while (prev->clnext != NULL) {
                if (prev->clnext == proxy) {
                    prev->clnext  = proxy->clnext;
                    proxy->clnext = NULL;
                    return;
                }

                prev = prev->clnext;
            }
        }
    }
}

static proxy_t *cl_hash_lookup_proxy(const char *client, void **cursor)
{
    proxy_t  *proxy;

    if (cursor != NULL && client != NULL) {
        if (*cursor == NULL)
            proxy = clhashtbl[cl_hash_index(client)];
        else {
            proxy = *(proxy_t **)cursor;
            proxy = proxy->clnext;
        }

        while (proxy != NULL) { 
            if (!strcmp(client, proxy->client)) {
                *cursor = proxy;
                return proxy;
            }
            
            proxy = proxy->clnext;
        }

        *cursor = NULL;
    }
 
    return NULL;
}

static int cl_hash_lookup_client(const char *client)
{
    uint32_t   idx;
    proxy_t   *proxy;

    if (client != NULL) {
        idx = cl_hash_index(client);

        for (proxy = clhashtbl[idx];  proxy;  proxy = proxy->clnext) {
            if (proxy->client && !strcmp(client, proxy->client))
                return TRUE;
        }
    }

    return FALSE;
}


static void timeout_create(proxy_t *proxy, uint32_t delay)
{
    if (proxy) {
        timeout_destroy(proxy);

        if (delay == 0)
            proxy->timeout = g_idle_add((GSourceFunc)timeout_handler, proxy);
        else {
            proxy->timeout = g_timeout_add(delay,
                                           (GSourceFunc)timeout_handler,
                                           proxy);
        }

        OHM_DEBUG(DBG_PROXY,"%u msec timeout added to proxy (id %u, timer %u)",
                  delay, proxy->id, proxy->timeout);
    }
}

static void timeout_destroy(proxy_t *proxy)
{
    if (proxy && proxy->timeout) {
        OHM_DEBUG(DBG_PROXY, "timeout removed from proxy (id %u, timer %u)",
                  proxy->id, proxy->timeout);

        g_source_remove(proxy->timeout);

        proxy->timeout = 0;
    }
}

static uint32_t timeout_handler(proxy_t *proxy)
{
    if (proxy != NULL) {
        if (proxy->timeout)
            OHM_DEBUG(DBG_PROXY, "timeout fired for proxy (id %u)", proxy->id);

        proxy->timeout = 0;       /* prevent subsequent timer destructions */

        state_machine(proxy, backend_timeout, NULL);
    }

    return FALSE; /* destroy this timer */
}


static void grant_handler(uint32_t granted, void *void_proxy)
{
    proxy_t  *proxy = void_proxy;
    char      buf[256];

    if (proxy == NULL)
        return;

    OHM_DEBUG(DBG_PROXY, "granted regular resources %s",
              resmsg_res_str(granted, buf, sizeof(buf)));

    state_machine(proxy, resource_grant, (void *)granted);
}

static int evaluate_notification_request_rules(const char *event,
                                               uint32_t   *mand_ret,
                                               uint32_t   *opt_ret,
                                               uint32_t   *lliv_ret,
                                               char       *errbuf)
{
    int   success;
    int   type;
    int   mand;
    int   opt;
    int   lliv;
    char *error = NULL;

    success = ruleif_notification_request(event,
                     RULEIF_INTEGER_ARG ("type"     , type ),
                     RULEIF_STRING_ARG  ("error"    , error),
                     RULEIF_INTEGER_ARG ("mandatory", mand ),
                     RULEIF_INTEGER_ARG ("optional" , opt  ),
                     RULEIF_INTEGER_ARG ("longlive" , lliv ),
                     RULEIF_ARGLIST_END                    );

    if (!success || type < 0) {
        type = -1;

        if (error) {
            OHM_DEBUG(DBG_PROXY, "notification request is rejected: %s",error);

            strncpy(errbuf, error, DBUS_DESCBUF_LEN);
        }
        else {
            OHM_DEBUG(DBG_PROXY, "notification request is rejected");

            strncpy(errbuf, "policy rejects play request", DBUS_DESCBUF_LEN);
        }
        
        errbuf[DBUS_DESCBUF_LEN-1] = '\0';
    }
    else {
        errbuf[0] = '\0';

        if (mand_ret != NULL)
            *mand_ret = (uint32_t)mand;

        if (opt_ret != NULL)
            *opt_ret = (uint32_t)opt;

        if (lliv_ret != NULL)
            *lliv_ret = (uint32_t)lliv;

        OHM_DEBUG(DBG_PROXY, "notification request rules returned: "
                  "type=%d, mandatory=%d optional=%d longliv=%d err='%s'",
                  type, mand, opt, lliv, error);
    }

    free(error);

    return type;
}

static int state_machine(proxy_t *proxy, proxy_event_t ev, void *evdata)
{
    uint32_t       granted = (uint32_t)evdata;
    void          *data    = evdata;
    int            success = TRUE;
    int            type    = proxy->type;
    proxy_state_t  state   = proxy->state;
    int            killed  = FALSE;
    uint32_t       status;

    OHM_DEBUG(DBG_PROXY, "proxy %s/%d received '%s' event in '%s' state",
              proxy->client,type, event_str(ev), state_str(state));


    switch (state) {

    case state_created:
        switch (ev) {
        case backend_timeout:
            create_and_send_status_to_client(proxy, proxy->status);
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        case client_died:
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        default:
            success = FALSE;
            break;
        }
        break;

    case state_acquiring:
        switch (ev) {
        case resource_grant:
            status = play_status(proxy, granted);
            create_and_send_status_to_client(proxy, status);
            if (!forward_play_request_to_backend(proxy, granted, status)) {
                create_and_send_status_to_client(proxy, NGF_FAILED);
                proxy_destroy(proxy);
                killed = TRUE;
            }
            break;
        case backend_timeout:
            create_and_send_status_to_client(proxy, NGF_COMPLETED);
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        case client_stop:
            premature_stop(proxy);
            break;
        case client_died:
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        default:
            success = FALSE;
            break;
        }
        break;


    case state_forwarded:
        switch (ev) {
        case resource_grant:
            stop_if_loose_resources(proxy, granted);
            break;
        case backend_status: 
            if (proxy->longlive)
                playback_partial_end(proxy);
            else {
                forward_status_to_client(proxy, data);
                proxy_destroy(proxy);
                killed = TRUE;
            }
            break;
        case backend_timeout:
            create_and_send_status_to_client(proxy, NGF_FAILED);
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        case backend_update:
            timeout_destroy(proxy);
            partial_release_of_resources(proxy, data);
            break;
        case client_stop:
            forward_stop_request_to_backend(proxy, data);
            break;
        case client_died:
            create_and_send_stop_request_to_backend(proxy);
            break;
        default:
            success = FALSE;
            break;
        }
        break;


    case state_completed:
        switch (ev) {
        case client_died:
        case resource_grant:
            proxy_destroy(proxy);   /* will release the resources */
            killed = TRUE;
            break; 
        case backend_timeout:
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        default:
            success = FALSE;
            break;
        }
        break;


    case state_stopped:
        switch (ev) {
        case backend_status:
            forward_status_to_client(proxy, data);
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        case client_died:
        case backend_timeout:
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        default:
            success = FALSE;
            break;
        }
        break;

    case state_killed:
        switch (ev) {
        case backend_status:
        case backend_timeout:
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        default:
            success = FALSE;
            break;
        }
        break;

    case state_longlive:
        switch (ev) {
        case client_stop:
            timeout_create(proxy, 0);
            break;
        case backend_timeout:
            create_and_send_status_to_client(proxy, NGF_COMPLETED);
            proxy_destroy(proxy);
            killed = TRUE;
            break;
        case client_died:
            proxy_destroy(proxy); /* that will send the longlieve stop */
            killed = TRUE;
            break;
        default:
            success = FALSE;
            break;
        }
        break;

    default:
        OHM_ERROR("notification: invalid proxy state %d for %s/%u",
                  proxy->state, proxy->client, proxy->id);
        break;
    }

    if (!killed && state != proxy->state) {
        OHM_DEBUG(DBG_PROXY, "proxy %s/%d enters to '%s' state",
                  proxy->client,type, state_str(proxy->state));
    }

    return success;
}


static int forward_play_request_to_backend(proxy_t *proxy,
                                           uint32_t granted,
                                           uint32_t status)
{
    void     *data;
    char     *mode;
    uint32_t  audio;
    uint32_t  vibra;
    uint32_t  leds;
    uint32_t  blight;
    uint32_t  limit;
    int       success = FALSE;

    switch (status) {

    case NGF_LONG:
        mode  = "long";
        resource_flags_to_booleans(granted, &audio, &vibra, &leds, &blight);
        limit = (audio || vibra || blight) ? play_limit : 0;
        break;

    case NGF_SHORT:
        mode  = "short";
        audio = TRUE;
        vibra = leds = blight = FALSE;
        limit = play_limit;
        break;

    default:
        proxy->state = state_completed;
        timeout_create(proxy, 0);
        return TRUE;
    }
    
    data = dbusif_append_to_play_data(proxy->data,
                      DBUSIF_UNSIGNED_ARG ( NGF_TAG_POLICY_ID   , proxy->id ),
                      DBUSIF_STRING_ARG   ( NGF_TAG_PLAY_MODE   , mode      ),
                      DBUSIF_UNSIGNED_ARG ( NGF_TAG_PLAY_LIMIT  , limit     ),
                      DBUSIF_BOOLEAN_ARG  ( NGF_TAG_MEDIA_AUDIO , audio     ),
                      DBUSIF_BOOLEAN_ARG  ( NGF_TAG_MEDIA_VIBRA , vibra     ),
                      DBUSIF_BOOLEAN_ARG  ( NGF_TAG_MEDIA_LEDS  , leds      ),
                      DBUSIF_BOOLEAN_ARG  ( NGF_TAG_MEDIA_BLIGHT, blight    ),
                      DBUSIF_ARGLIST_END                                    );

    if (data == NULL) {
        OHM_DEBUG(DBG_PROXY, "extended play request creation "
                   "failed (id %u) ", proxy->id);
        success = FALSE;
    }
    else {
        dbusif_forward_data(data);

        OHM_DEBUG(DBG_PROXY, "extended play request (id %u) "
                  "forwarded to backend", proxy->id);

        dbusif_free_data(proxy->data);
        
        proxy->resources = granted;
        proxy->state     = state_forwarded; 
        proxy->data      = NULL;
        
        if (limit)
            timeout_create(proxy, play_timeout);

        success = TRUE;
    }

    return success;
}

static int forward_stop_request_to_backend(proxy_t *proxy, void *data)
{
    void *fwdata = dbusif_copy_stop_data(data);

    dbusif_forward_data(fwdata);
    
    OHM_DEBUG(DBG_PROXY,"stop request (id %u) forwarded to backend",proxy->id);

    proxy->state = state_stopped;

    return TRUE;
}

static int create_and_send_stop_request_to_backend(proxy_t *proxy)
{
    void *data;
    int   success;

    data = dbusif_create_stop_data(proxy->id);

    if (data == NULL) {
        OHM_DEBUG(DBG_PROXY, "failed to send stop request (id %u) "
                  "to backend",proxy->id);
        success = FALSE;
    }
    else {    
        OHM_DEBUG(DBG_PROXY,"stop request (id %u) will be "
                  "sent to backend", proxy->id);

        dbusif_forward_data(data);

        proxy->state = state_killed;

        success = TRUE;
    }

    return success;
}

static int forward_status_to_client(proxy_t *proxy, void *data)
{
    void *fwdata;
    int   success;

    if (proxy->client == NULL || proxy->client[0] == '\0')
        success = FALSE;
    else {
        success = TRUE;
        fwdata  = dbusif_copy_status_data(proxy->client, data);

        dbusif_forward_data(fwdata);
    
        timeout_create(proxy, stop_timeout);

        OHM_DEBUG(DBG_PROXY, "status request (id %u) forwarded "
                  "to client %s", proxy->id, proxy->client);
    }
    
    proxy->state = state_completed;

    return success;
}

static int partial_release_of_resources(proxy_t *proxy, void *data)
{
    uint32_t flags = *(uint32_t *)data;

    OHM_DEBUG(DBG_PROXY, "partial release of resources (id %u resources 0x%x)",
              proxy->id, flags);

    return TRUE;
}

static int create_and_send_status_to_client(proxy_t *proxy, uint32_t status)
{
    void *data;
    int   success;

    if (proxy->client == NULL || proxy->client[0] == '\0')
        success = FALSE;
    else {
        OHM_DEBUG(DBG_PROXY, "create & send status message to client %s "
                  "(id %u)", proxy->client, proxy->id);

        success = TRUE;
        data = dbusif_create_status_data(proxy->client, proxy->id, status);

        dbusif_forward_data(data);
    }

    return success;
}


static int stop_if_loose_resources(proxy_t *proxy, uint32_t granted)
{
    uint32_t  current = proxy->resources;
    int       success = FALSE;
    void     *data;

    if (current && (!granted || (current & granted) != current)) {
        /*
         * we lost some or all of our granted resources;
         * send a stop request to the backend
         */
        data = dbusif_create_stop_data(proxy->id);
        dbusif_forward_data(data);
                
        timeout_create(proxy, stop_timeout);

        proxy->state = state_stopped;

        success = TRUE;
    }

    return success;
}

static int premature_stop(proxy_t *proxy)
{
    /*
     * backend is unaware the play request, so no backend operation
     * moving to completed state
     */
    proxy->state = state_completed;
    timeout_create(proxy, 0);

    return TRUE;
}

static int playback_partial_end(proxy_t *proxy)
{
    timeout_destroy(proxy);
    resource_set_release(proxy->type, rset_regular, grant_handler,proxy);
    proxy->state = state_longlive;

    return TRUE;
}

static int fake_backend_timeout(proxy_t *proxy)
{
    timeout_create(proxy, 0);

    return TRUE;
}

static uint32_t play_status(proxy_t *proxy, uint32_t granted)
{
    static uint32_t  status[rset_id_max] = {
        [ rset_ringtone   ] = NGF_SHORT ,
        [ rset_missedcall ] = NGF_BUSY  ,
        [ rset_alarm      ] = NGF_SHORT ,
        [ rset_event      ] = NGF_SHORT
    };

    if (granted == RESOURCE_SET_BUSY)
        return NGF_BUSY;

    if (granted)
        return NGF_LONG;

    if (proxy->type < 0 || proxy->type >= rset_id_max)
        return NGF_BUSY;

    return status[proxy->type];
}

static const char *state_str(proxy_state_t state)
{
    switch (state) {
    case state_created:      return "created";
    case state_acquiring:    return "acquiring";
    case state_forwarded:    return "forwarded";
    case state_completed:    return "completed";
    case state_stopped:      return "stopped";
    case state_killed:       return "killed";
    case state_longlive:     return "longlive";
    default:                 return "<unknown>";
    }
}

static const char *event_str(proxy_event_t event)
{
    switch (event) {
    case resource_grant:    return "resource grant";
    case backend_status:    return "backend status";
    case backend_update:    return "backend update";
    case backend_timeout:   return "backend_timeout";
    case client_stop:       return "client stop";
    case client_died:       return "client died";
    default:                return "<unknown>";
    }
}


/* 
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */
