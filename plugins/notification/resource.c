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
#include <errno.h>

#include <res-conn.h>

#include "plugin.h"
#include "resource.h"
#include "ruleif.h"
#include "subscription.h"


typedef struct {
    resource_set_id_t  id;     /* ID of the resource set */
    char              *klass;  /* resource class      */
    uint32_t           mand;   /* mandatory resources */
    uint32_t           opt;    /* optional resources  */
    uint32_t           lliv;   /* longlive resources */
} rset_def_t;

typedef struct {
    resource_cb_t     function;
    void             *data;
} callback_t;

typedef struct fake_grant_s {
    struct fake_grant_s   *next;
    struct resource_set_s *rs;
    uint32_t               srcid;
    uint32_t               flags;
    callback_t             callback;
} fake_grant_t;

typedef struct resource_set_s {
    resource_set_type_t  type;
    resset_t            *resset;
    int                  acquire;
    uint32_t             reqno;
    uint32_t             flags;
    callback_t           grant;
    fake_grant_t        *fakes;
    struct {
        int    count;
        char **list;
    }                    event;
} resource_set_t;


OHM_IMPORTABLE(void *, timer_add  , (uint32_t delay,
                                     resconn_timercb_t callback,
                                     void *data));
OHM_IMPORTABLE(void  , timer_del  , (void *timer));

static resconn_t      *conn;
static resource_set_t  regular_set[rset_id_max];
static resource_set_t  longlive_set[rset_id_max];
static resource_set_t  empty_set[rset_id_max];
static uint32_t        reqno;
static int             verbose;

static void          connect_to_manager(resconn_t *);
static void          conn_status(resset_t *, resmsg_t *);
static void          grant_handler(resmsg_t *, resset_t *, void *);
static gboolean      fake_grant_handler(gpointer);
static fake_grant_t *fake_grant_create(resource_set_t *, uint32_t,
                                       resource_cb_t, void *);
static void          fake_grant_delete(fake_grant_t *);
static fake_grant_t *fake_grant_find(resource_set_t *, resource_cb_t, void *);
static void          update_event_list(void);
static void          free_event_list(char **);
static char         *strlist(char **, char *, int);


/*! \addtogroup pubif
 *  Functions
 *  @{
 */

void resource_init(OhmPlugin *plugin)
{
    char *timer_add_signature = (char *)timer_add_SIGNATURE;
    char *timer_del_signature = (char *)timer_del_SIGNATURE;
    int   failed = FALSE;

    (void)plugin;
 
    ohm_module_find_method("resource.restimer_add",
			   &timer_add_signature,
			   (void *)&timer_add);
    ohm_module_find_method("resource.restimer_del",
			   &timer_del_signature,
			   (void *)&timer_del);

    if (timer_add == NULL) {
        OHM_ERROR("notification: can't find mandatory method "
                  "'resource.timer_add'");
        failed = TRUE;
    }

    if (timer_del == NULL) {
        OHM_ERROR("notification: can't find mandatory method "
                  "'resource.timer_add'");
        failed = TRUE;
    }

    if (timer_add != NULL && timer_del != NULL) {
        conn = resproto_init(RESPROTO_ROLE_CLIENT, RESPROTO_TRANSPORT_INTERNAL,
                             connect_to_manager, "notification",
                             timer_add, timer_del);

        if (conn == NULL) {
            OHM_ERROR("notification: can't initialize "
                      "resource loopback protocol");
            failed = TRUE;
        }
        else {
            resproto_set_handler(conn, RESMSG_GRANT, grant_handler);
            connect_to_manager(conn);
        }
    }

    if (failed)
        exit(1);    

    verbose = TRUE;
}

int resource_set_acquire(resource_set_id_t    id,
                         resource_set_type_t  type,
                         uint32_t             mand,
                         uint32_t             opt,
                         resource_cb_t        function,
                         void                *data)
{
    resource_set_t *rspool;
    resource_set_t *rs;
    resset_t       *resset;
    uint32_t        all;
    resmsg_t        msg;
    char            mbuf[256];
    char            obuf[256];
    int             success;
    const char     *typstr;

    if (id < 0 || id >= rset_id_max || type < 0 || type >= rset_type_max)
        success = FALSE;
    else {
        switch (type) {
        case rset_regular:  rspool = regular_set;   typstr = "regular";  break;
        case rset_longlive: rspool = longlive_set;  typstr = "longlive"; break;
        default:            rspool = empty_set;     typstr = "???";      break;
        }

        all = mand | opt;
        rs  = rspool + id;
        resset = rs->resset;

        if (!resset)
            success = FALSE;
        else if (!all || rs->acquire) {
            if (type == rset_regular)
                fake_grant_create(rs, RESOURCE_SET_BUSY, function,data);
            success = TRUE;
        }
        else {
            rs->acquire = TRUE;
            rs->grant.function = function;
            rs->grant.data     = data;

            if ((all ^ resset->flags.all) || (opt ^ resset->flags.opt)) {

                OHM_DEBUG(DBG_RESRC, "updating %s resource set%u (reqno %u) "
                          "mandatory='%s' optional='%s'",typstr, id, rs->reqno,
                          resmsg_res_str(mand, mbuf, sizeof(mbuf)),
                          resmsg_res_str(opt , obuf, sizeof(obuf)));

                memset(&msg, 0, sizeof(msg));
                msg.record.type  = RESMSG_UPDATE;
                msg.record.id    = id;
                msg.record.reqno = ++reqno;
                msg.record.rset.all = all;
                msg.record.rset.opt = opt;
                msg.record.klass = resset->klass;
                msg.record.mode  = resset->mode;
                
                resproto_send_message(rs->resset, &msg, NULL);
            }

            rs->reqno = ++reqno;

            OHM_DEBUG(DBG_RESRC, "acquiring %s resource set%u (reqno %u)",
                      typstr, id, rs->reqno);

            memset(&msg, 0, sizeof(msg));
            msg.possess.type  = RESMSG_ACQUIRE;
            msg.possess.id    = id;
            msg.possess.reqno = rs->reqno;
            
            success = resproto_send_message(rs->resset, &msg, NULL);

            if (success)
                update_event_list();
        }
    }

    return success;
}

int resource_set_release(resource_set_id_t    id,
                         resource_set_type_t  type,
                         resource_cb_t        function,
                         void                *data)
{
    resource_set_t *rspool;
    resource_set_t *rs;
    resmsg_t        msg;
    int             success;
    fake_grant_t   *fake;
    const char     *typstr;

    if (id < 0 || id >= rset_id_max || type < 0 || type >= rset_type_max)
        success = FALSE;
    else {
        switch (type) {
        case rset_regular:  rspool = regular_set;   typstr = "regular";  break;
        case rset_longlive: rspool = longlive_set;  typstr = "longlive"; break;
        default:            rspool = empty_set;     typstr = "???";      break;
        }

        rs = rspool + id;

        if (function == rs->grant.function && data == rs->grant.data) {
            rs->reqno = ++reqno;
            rs->grant.function = NULL;
            rs->grant.data     = NULL;
            
            OHM_DEBUG(DBG_RESRC, "releasing %s resource set%u (reqno %u)",
                      typstr, id, rs->reqno);
            
            memset(&msg, 0, sizeof(msg));
            msg.possess.type  = RESMSG_RELEASE;
            msg.possess.id    = id;
            msg.possess.reqno = rs->reqno;
            
            success = resproto_send_message(rs->resset, &msg, NULL);

            if (success && type == rset_longlive)
                rs->acquire = FALSE;
        }
        else {
            success = TRUE;

            if ((fake = fake_grant_find(rs, function,data)) != NULL) {
                fake_grant_delete(fake);
            }
        }
    }

    return success;
}


void resource_flags_to_booleans(uint32_t  flags,
                                uint32_t *audio,
                                uint32_t *vibra,
                                uint32_t *leds,
                                uint32_t *blight)
{
    if (audio)   *audio  = (flags & RESMSG_AUDIO_PLAYBACK);
    if (vibra)   *vibra  = (flags & RESMSG_VIBRA         );
    if (leds)    *leds   = (flags & RESMSG_LEDS          );
    if (blight)  *blight = (flags & RESMSG_BACKLIGHT     );
}

uint32_t resource_name_to_flag(const char *name)
{
    typedef struct {
        const char *name;
        uint32_t    flag;
    } flag_def_t;

    static flag_def_t flag_defs[] = {
        { "audio"    , RESMSG_AUDIO_PLAYBACK },
        { "vibra"    , RESMSG_VIBRA          },
        { "leds"     , RESMSG_LEDS           },
        { "backlight", RESMSG_BACKLIGHT      },
        { NULL       , 0                     }
    };

    flag_def_t *fd;

    for (fd = flag_defs;  fd->name != NULL;  fd++) {
        if (!strcmp(name, fd->name))
            return fd->flag;
    }
    
    return 0;
}


/*!
 * @}
 */

static void connect_to_manager(resconn_t *rc)
{
#define MAND_DEFAULT   RESMSG_AUDIO_PLAYBACK | RESMSG_VIBRA
#define MAND_PROCLM    RESMSG_AUDIO_PLAYBACK
#define MAND_MISCALL   RESMSG_LEDS
#define MAND_NOTIF     RESMSG_LEDS
#define OPT_DEFAULT    RESMSG_BACKLIGHT
#define OPT_PROCLM     0
#define OPT_MISCALL    0
#define OPT_NOTIF      0
#define LLIV_PROCLM    0
#define LLIV_DEFAULT   RESMSG_LEDS
#define LLIV_RING      0
#define LLIV_MISCALL   0
#define LLIV_NOTIF     0
   
    static rset_def_t   defs[] = {
        {rset_proclaimer, "proclaimer", MAND_PROCLM ,OPT_PROCLM ,LLIV_PROCLM },
        {rset_ringtone  , "ringtone"  , MAND_DEFAULT,OPT_DEFAULT,LLIV_RING   },
        {rset_missedcall, "event"     , MAND_MISCALL,OPT_MISCALL,LLIV_MISCALL},
        {rset_alarm     , "alarm"     , MAND_DEFAULT,OPT_DEFAULT,LLIV_DEFAULT},
        {rset_event     , "event"     , MAND_DEFAULT,OPT_DEFAULT,LLIV_DEFAULT},
        {rset_notifier  , "ringtone"  , MAND_NOTIF  ,OPT_NOTIF  ,LLIV_NOTIF  },
    };

    rset_def_t      *def;
    unsigned int     i;
    resmsg_t         msg;
    resmsg_record_t *rec;
    int              success;

    memset(&msg, 0, sizeof(msg));
    rec = &msg.record;

    rec->type = RESMSG_REGISTER;

    for (i = 0, success = TRUE;   i < DIM(defs);   i++) {
        def = defs + i;

        rec->id       = def->id;
        rec->reqno    = ++reqno;
        rec->rset.all = def->mand | def->opt;
        rec->rset.opt = def->opt;
        rec->klass    = def->klass;
        rec->mode     = RESMSG_MODE_ALWAYS_REPLY | RESMSG_MODE_AUTO_RELEASE;

        if (resconn_connect(rc, &msg, conn_status) == NULL) {
            if (verbose) {
                OHM_ERROR("notification: can't register '%s' regular "
                          "resource class", def->klass);
            }
            success = FALSE;
        }

        if (def->lliv) {
            rec->id       = def->id + rset_id_max;
            rec->reqno    = ++reqno;
            rec->rset.all = def->lliv;
            rec->rset.opt = 0;
            rec->klass    = def->klass;
            rec->mode     = 0;
            
            if (resconn_connect(rc, &msg, conn_status) == NULL) {
                if (verbose) {
                    OHM_ERROR("notification: can't register '%s' longlive "
                              "resource class", def->klass);
                }
                success = FALSE;
            }
        }
    }

    if (success) {
        OHM_DEBUG(DBG_RESRC, "successfully registered all resource classes");
        update_event_list();
    }

#undef LLIV_NOTIF
#undef LLIV_MISCALL
#undef LLIV_RING
#undef LLIV_DEFAULT
#undef OPT_NOTIF
#undef OPT_MISCALL
#undef OPT PROCLM
#undef OPT_DEFAULT
#undef MAND_NOTIF
#undef MAND_MISCALL
#undef MAND_PROCLM
#undef MAND_DEFAULT
}

static void conn_status(resset_t *resset, resmsg_t *msg)
{
    resource_set_t *rs;
    char           *kl;
    char          **evs;
    int             len;
    char            buf[256];

    if (msg->type == RESMSG_STATUS) {
        if (msg->status.errcod == 0) {
            if (resset->id < rset_id_max) {
                /* regular set */
                if (!ruleif_notification_events(resset->id, &kl, &evs, &len)) {
                    OHM_ERROR("notification: creation of '%s' resource set "
                              "(id %u) failed: querying event list failed",
                              resset->klass, resset->id);
                }
                else {
                    OHM_DEBUG(DBG_RESRC, "'%s' regular resource set (id %u) "
                              "successfully created (event list = %d %s)",
                              resset->klass, resset->id, len,
                              strlist(evs, buf, sizeof(buf)));

                    rs = regular_set + resset->id;

                    memset(rs, 0, sizeof(resource_set_t));
                    rs->type = rset_regular;
                    rs->resset = resset;
                    rs->event.count = len;
                    rs->event.list  = evs;
                    
                    resset->userdata = rs;
                }
            }
            else {
                /* longlive set */
                OHM_DEBUG(DBG_RESRC, "'%s' longlive resource set (id %u) "
                          "successfully created", resset->klass, resset->id);

                rs = longlive_set + (resset->id - rset_id_max);

                memset(rs, 0, sizeof(resource_set_t));
                rs->type = rset_longlive;
                rs->resset = resset;
                    
                resset->userdata = rs;
            }
        }
        else {
            OHM_ERROR("notification: creation of '%s' resource set "
                      "(id %u) failed: %d %s", resset->klass, resset->id,
                      msg->status.errcod,
                      msg->status.errmsg ? msg->status.errmsg : "");
        }
    }
}

static void grant_handler(resmsg_t *msg, resset_t *resset, void *protodata)
{
    resource_set_t *rs;
    callback_t      grant;
    int             update;
    char            buf[256];
    const char     *typstr;

    (void)protodata;

    if ((rs = resset->userdata) != NULL) {
        update = FALSE;
        grant  = rs->grant;
        typstr = (rs->type == rset_regular) ? "regular" : "longlive";

        OHM_DEBUG(DBG_RESRC, "granted %s resource set%u %s (reqno %u)",
                  typstr, resset->id,
                  resmsg_res_str(msg->notify.resrc, buf, sizeof(buf)),
                  msg->notify.reqno);


        switch (rs->type) {

        case rset_regular:
            if (rs->reqno != msg->notify.reqno)
                grant.function = NULL;
            else {
                rs->reqno = 0;
                rs->flags = msg->notify.resrc;
                
                if (rs->flags == 0) {
                    
                    update  = rs->acquire;
                    
                    rs->acquire        = FALSE; /* auto released */
                    rs->grant.function = NULL;
                    rs->grant.data     = NULL;
                }
                
                if (grant.function != NULL)
                    grant.function(rs->flags, grant.data);
            }
            break;

        case rset_longlive:
            rs->flags = msg->notify.resrc;
            break;

        default:
            grant.function = NULL;
            break;
        }

        if (grant.function != NULL)
            grant.function(rs->flags, grant.data);

        OHM_DEBUG(DBG_RESRC, "resource set%u acquire=%s reqno=%u %s",
                  resset->id, rs->acquire ? "True":"False", rs->reqno,
                  rs->grant.function ? "grantcb present":"no grantcb");
        
        if (update)
            update_event_list();
    }
}

static gboolean fake_grant_handler(gpointer data)
{
    fake_grant_t   *fake = data;
    resource_set_t *rs;
    resset_t       *resset;
    

    if (fake && (rs = fake->rs) && (resset = rs->resset)) {

        OHM_DEBUG(DBG_RESRC, "resourse set%u fake grant %u",
                  resset->id, fake->flags);

        fake->callback.function(fake->flags, fake->callback.data);
        fake->srcid = 0;
        fake_grant_delete(fake);
    }

    return FALSE; /*  destroy this source */
}


static fake_grant_t *fake_grant_create(resource_set_t *rs,
                                       uint32_t        flags,
                                       resource_cb_t   function,
                                       void           *data)
{
    fake_grant_t *fake;
    fake_grant_t *last;

    for (last = (fake_grant_t *)&rs->fakes;   last->next;   last = last->next)
        ;

    if ((fake = malloc(sizeof(fake_grant_t))) != NULL) {
        memset(fake, 0, sizeof(fake_grant_t));
        fake->rs = rs;
        fake->srcid = g_idle_add(fake_grant_handler, fake);
        fake->flags = flags;
        fake->callback.function = function;
        fake->callback.data = data;

        last->next = fake;
    }

    return fake;
}

static void fake_grant_delete(fake_grant_t *fake)
{
    resource_set_t *rs;
    fake_grant_t   *prev;

    if (!fake || !(rs = fake->rs))
        return;

    for (prev = (fake_grant_t *)&rs->fakes;  prev->next;  prev = prev->next) {
        if (fake == prev->next) {

            if (fake->srcid)
                g_source_remove(fake->srcid);

            prev->next = fake->next;
            free(fake);

            return;
        }
    }
}

static fake_grant_t *fake_grant_find(resource_set_t  *rs, 
                                       resource_cb_t  function,
                                       void          *data)
{
    fake_grant_t *fake = NULL;
    fake_grant_t *prev;

    if (rs != NULL) {
        for (prev = (fake_grant_t *)&rs->fakes;
             (fake = prev->next) != NULL;
             prev = prev->next)
        {
            if (fake->callback.function == function ||
                fake->callback.data     == data       )
            {
                break;
            }
        }
    }

    return fake;
}

static void update_event_list(void)
{
    resource_set_t *rs;
    char           *evls[256];
    int             evcnt;
    uint32_t        sign;
    int             i, k;
    char            buf[256*10];

    sign = 0;

    for (i = evcnt = 0;   i < rset_id_max && evcnt < DIM(evls)-1;    i++) {
        rs = regular_set + i;

        if (!rs->acquire) {
            sign |= (((uint32_t)1) << i);

            for (k = 0;  k < rs->event.count;  k++)
                evls[evcnt++] = rs->event.list[k];
        }
    }

    evls[evcnt] = NULL;

    OHM_DEBUG(DBG_RESRC, "signature %u event list %d '%s'",
              sign, evcnt, strlist(evls, buf, sizeof(buf)));

    subscription_update_event_list(sign, evls, evcnt);
}


static void free_event_list(char **list)
{
    int i;

    if (list != NULL) {
        for (i = 0;  list[i]; i++)
            free(list[i]);

        free(list);
    }
}

static char *strlist(char **arr, char *buf, int len)
{
    int   i;
    char *p;
    char *sep;
    int   l;

    if (arr[0] == NULL)
        snprintf(buf, len, "<empty-list>");
    else {
        for (i=0, sep="", p=buf;    arr[i] && len > 0;    i++, sep=",") {
            l = snprintf(p, len, "%s%s", sep, arr[i]);

            p   += l;
            len -= l;
        }
    }

    return buf;
}

/* 
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */
