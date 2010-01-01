/*! \defgroup pubif Public Interfaces */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>

#include "plugin.h"
#include "resource-set.h"
#include "fsif.h"

#define HASH_BITS      8
#define HASH_DIM       (1 << HASH_BITS)
#define HASH_MASK      (HASH_DIM - 1)
#define HASH_INDEX(i)  ((i) & HASH_MASK)

#define INTEGER_FIELD(n,v) { fldtype_integer, n, .value.integer = v }
#define STRING_FIELD(n,v)  { fldtype_string , n, .value.string  = v ? v : "" }
#define INVALID_FIELD      { fldtype_invalid, NULL, .value.string = NULL }

#define SELIST_DIM  2

static resource_set_t  *hash_table[HASH_DIM];


static int add_factstore_entry(resource_set_t *);
static int delete_factstore_entry(resource_set_t *);
static int update_factstore_flags(resource_set_t *);
static int update_factstore_request(resource_set_t *);

static void add_to_hash_table(resource_set_t *);
static void delete_from_hash_table(resource_set_t *);
static resource_set_t *find_in_hash_table(uint32_t);


/*! \addtogroup pubif
 *  Functions
 *  @{
 */

void resource_set_init(OhmPlugin *plugin)
{
    (void)plugin;
}

resource_set_t *resource_set_create(resset_t *resset)
{
    static uint32_t  manager_id;

    resource_set_t *rs;

    if ((rs = malloc(sizeof(resource_set_t))) != NULL) {
        memset(rs, 0, sizeof(resource_set_t));
        rs->manager_id = manager_id++;
        rs->resset     = resset;
        rs->request    = strdup("release");

        resset->userdata = rs;
        add_to_hash_table(rs);
        add_factstore_entry(rs);
    }

    if (rs != NULL) {
        OHM_DEBUG(DBG_SET, "created resource set %s'/%u (manager id %u)",
                  resset->peer, resset->id, rs->manager_id);
    }
    else {
        OHM_ERROR("resource: can't create resource set %s/%u: out of memory",
                  resset->peer, resset->id);
    }
    
    return rs;
}

void resource_set_destroy(resset_t *resset)
{
    resource_set_t *rs;
    uint32_t        mgrid;

    if (resset == NULL || (rs = resset->userdata) == NULL)
        OHM_ERROR("resource: refuse to destroy sesource set: argument error");
    else {
        if (resset != rs->resset) {
            OHM_ERROR("resource: refuse to destroy resource set %s/%u "
                      "(manager id %u): confused with data structures",
                      resset->peer, resset->id, rs->manager_id);
        }
        else {
            mgrid = rs->manager_id;

            delete_factstore_entry(rs);
            delete_from_hash_table(rs);
            resset->userdata = NULL;

            free(rs->request);
            free(rs);

            OHM_DEBUG(DBG_SET, "destroyed resource set %s/%u (manager id %u)",
                      resset->peer, resset->id, mgrid);
        }
    }
}

int resource_set_update(resset_t *resset, resource_set_update_t what)
{
    resource_set_t *rs;
    int success = FALSE;

    if (resset == NULL || (rs = resset->userdata) == NULL)
        OHM_ERROR("resource: refuse to update resource set: argument error");
    else {
        if (resset != rs->resset) {
            OHM_ERROR("resource: refuse to update resource set %s/%u "
                      "(manager id %u): confused with data structures",
                      resset->peer, resset->id, rs->manager_id);
        }
        else {
            switch (what) {
            case update_flags:   success = update_factstore_flags(rs);   break;
            case update_request: success = update_factstore_request(rs); break;
            default:             success = FALSE;                        break;
            }
        }
    }

    return success;
}

void resource_set_send(resource_set_t          *rs,
                       uint32_t                 reqno,
                       resource_set_field_id_t  field)
{
    resset_t *resset;
    resmsg_t  msg;

    if (rs == NULL || (resset = rs->resset) == NULL) 
        OHM_ERROR("resource: refuse to send field: argument error");
    else {
        memset(&msg, 0, sizeof(msg));
        msg.any.id    = resset->id;
        msg.any.reqno = reqno;

        switch (field) {

        case resource_set_granted:
            if (rs->granted.client != rs->granted.factstore || reqno) {
                msg.notify.type  = RESMSG_GRANT;
                msg.notify.resrc = rs->granted.factstore;

                resource_set_dump_message(&msg, resset, "to");

                if (resproto_send_message(resset, &msg, NULL))
                    rs->granted.client = rs->granted.factstore;
                else {
                    OHM_ERROR("resource: failed to send grant message to "
                              "%s/%u (manager id %u)",
                              resset->peer, resset->id, rs->manager_id);
                }
            }
            break;

        case resource_set_advice:
            if (rs->advice.client != rs->advice.factstore || reqno) {
                msg.notify.type  = RESMSG_ADVICE;
                msg.notify.resrc = rs->advice.factstore;

                resource_set_dump_message(&msg, resset, "to");

                if (resproto_send_message(resset, &msg, NULL))
                    rs->advice.client = rs->advice.factstore;
                else {
                    OHM_ERROR("resource: failed to send grant message to "
                              "%s/%u (manager id %u)",
                              resset->peer, resset->id, rs->manager_id);
                }
            }
            break;

        default:
            OHM_ERROR("resource: refuse to send field: argument error");
            break;
        }
    }
}

resource_set_t *resource_set_find(fsif_entry_t *entry)
{
    uint32_t manager_id = INVALID_MANAGER_ID;
    resource_set_t *rs = NULL;
    
    fsif_get_field_by_entry(entry, fldtype_integer, "manager_id", &manager_id);

    if (manager_id == INVALID_MANAGER_ID)
        OHM_DEBUG(DBG_FS, "failed to get manager_id"); 
    else  {
        if ((rs = find_in_hash_table(manager_id)) == NULL) {
            OHM_DEBUG(DBG_SET, "can't find resource set with manager_id %u",
                      manager_id);
        }
    }

    return rs;
}

void resource_set_dump_message(resmsg_t *msg,resset_t *resset,const char *dir)
{
    resconn_t *rconn = resset->resconn;
    int        dump;
    char      *tstr;
    char      *mstr;
    char       buf[2048];

    switch (rconn->any.transp) {
    case RESPROTO_TRANSPORT_DBUS:     dump=DBG_DBUS;     tstr="dbus";    break;
    case RESPROTO_TRANSPORT_INTERNAL: dump=DBG_INTERNAL; tstr="internal";break;
    default:                          dump=FALSE;        tstr="???";     break;
    }

    if (dump) {
        mstr = resmsg_dump_message(msg, 3, buf,sizeof(buf));
        OHM_DEBUG(dump, "%s message %s '%s':\n%s",
                  tstr, dir, resset->peer, mstr);
    }
}


/*!
 * @}
 */

static int add_factstore_entry(resource_set_t *rs)
{
    resset_t *resset    = rs->resset;
    uint32_t  mandatory = resset->flags.all & ~resset->flags.opt;
    int       success;

    fsif_field_t  fldlist[] = {
        INTEGER_FIELD ("manager_id" , rs->manager_id       ),
        STRING_FIELD  ("client_name", resset->peer         ),
        INTEGER_FIELD ("client_id"  , resset->id           ),
        STRING_FIELD  ("class"      , resset->class        ),
        INTEGER_FIELD ("mandatory"  , mandatory            ),
        INTEGER_FIELD ("optional"   , resset->flags.opt    ),
        INTEGER_FIELD ("shared"     , resset->flags.share  ),
        INTEGER_FIELD ("granted"    , rs->granted.factstore),
        INTEGER_FIELD ("advice"     , rs->advice.factstore ),
        STRING_FIELD  ("request"    , rs->request          ),
        INTEGER_FIELD ("reqno"      , 0                    ),
        INVALID_FIELD
    };

    success = fsif_add_factstore_entry(FACTSTORE_RESOURCE_SET, fldlist);

    return success;
}

static int delete_factstore_entry(resource_set_t *rs)
{
    int success;

    fsif_field_t  selist[] = {
        INTEGER_FIELD("manager_id", rs->manager_id),
        INVALID_FIELD
    };


    success = fsif_delete_factstore_entry(FACTSTORE_RESOURCE_SET, selist);

    return success;
}

static int update_factstore_flags(resource_set_t *rs)
{
    resset_t *resset    = rs->resset;
    uint32_t  mandatory = resset->flags.all & ~resset->flags.opt;
    int       success;

    fsif_field_t  selist[]  = {
        INTEGER_FIELD("manager_id", rs->manager_id),
        INVALID_FIELD
    };
    fsif_field_t  fldlist[] = {
        INTEGER_FIELD ("mandatory"  , mandatory            ),
        INTEGER_FIELD ("optional"   , resset->flags.opt    ),
        INTEGER_FIELD ("shared"     , resset->flags.share  ),
        INVALID_FIELD
    };

    success = fsif_update_factstore_entry(FACTSTORE_RESOURCE_SET,
                                          selist, fldlist);

    return success;
}



static int update_factstore_request(resource_set_t *rs)
{
    static int reqno = 0;

    int        success;

    fsif_field_t  selist[]  = {
        INTEGER_FIELD("manager_id", rs->manager_id),
        INVALID_FIELD
    };
    fsif_field_t  fldlist[] = {
        STRING_FIELD  ("request", rs->request),
        INTEGER_FIELD ("reqno"  , reqno++    ),
        INVALID_FIELD
    };

    success = fsif_update_factstore_entry(FACTSTORE_RESOURCE_SET,
                                          selist, fldlist);

    return success;
}


static void add_to_hash_table(resource_set_t *rs)
{
    int index = HASH_INDEX(rs->manager_id);

    rs->next = hash_table[index];
    hash_table[index] = rs;
}

static void delete_from_hash_table(resource_set_t *rs)
{
    int              index  = HASH_INDEX(rs->manager_id);
    resset_t        *resset = rs->resset;
    resource_set_t  *prev;

    for (prev = (void *)&hash_table[index];   prev->next;  prev = prev->next) {
        if (prev->next == rs) {
            prev->next = rs->next;
            return;
        }
    }

    OHM_ERROR("resource: failed to remove resource %s/%u (manager id %u) "
              "from hash table: not found",
              resset->peer, resset->id, rs->manager_id); 
}

static resource_set_t *find_in_hash_table(uint32_t manager_id)
{
    int index = HASH_INDEX(manager_id);
    resource_set_t *rs;

    for (rs = hash_table[index];   rs != NULL;   rs = rs->next) {
        if (manager_id == rs->manager_id)
            break;
    }

    return rs;
}

/* 
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */
