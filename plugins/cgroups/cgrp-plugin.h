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


#ifndef __OHM_PLUGIN_CGRP_H__
#define __OHM_PLUGIN_CGRP_H__

#include <stdio.h>

#include <ohm/ohm-plugin.h>
#include <ohm/ohm-plugin-log.h>
#include <ohm/ohm-plugin-debug.h>
#include <ohm/ohm-fact.h>

#include <glib.h>

#include "cgrp-basic-types.h"
#include "mm.h"
#include "list.h"

#define PLUGIN_PREFIX   cgroups
#define PLUGIN_NAME    "cgroups"
#define PLUGIN_VERSION "0.0.2"

#define DEFAULT_CONFIG "/etc/ohm/plugins.d/syspart.conf"
#define DEFAULT_NOTIFY 3001

#define CGRP_FACT_GROUP      "com.nokia.cgroups.group"
#define CGRP_FACT_PART       "com.nokia.cgroups.partition"
#define CGRP_FACT_APPCHANGES "com.nokia.policy.application_changes"

#define APP_ACTIVE   "active"
#define APP_INACTIVE "standby"

#define DEFAULT_ROOT "/syspart"

#define CGRP_SET_FLAG(flags, bit) ((flags) |=  (1 << (bit)))
#define CGRP_TST_FLAG(flags, bit) ((flags) &   (1 << (bit)))
#define CGRP_CLR_FLAG(flags, bit) ((flags) &= ~(1 << (bit)))

#define CGRP_SET_MASK(mask, bit) ((mask) |=  (1ULL << (bit)))
#define CGRP_TST_MASK(mask, bit) ((mask) &   (1ULL << (bit)))
#define CGRP_CLR_MASK(mask, bit) ((mask) &= ~(1ULL << (bit)))

typedef u64_t cgrp_mask_t;


/*
 * a custom partition controls
 */

typedef struct cgrp_ctrl_setting_s cgrp_ctrl_setting_t;
typedef struct cgrp_ctrl_s cgrp_ctrl_t;

struct cgrp_ctrl_setting_s {
    cgrp_ctrl_setting_t *next;
    char                *name;
    char                *value;
};


struct cgrp_ctrl_s {
    cgrp_ctrl_t         *next;
    char                *name;
    char                *path;
    cgrp_ctrl_setting_t *settings;
};




/*
 * a system partition
 */

typedef enum {
    CGRP_PARTITION_NONE     = 0x0,
    CGRP_PARTITION_NOFREEZE = 0x1,          /* partition not freezable */
    CGRP_PARTITION_FACT     = 0x2,          /* export partition to factstore */
} cgrp_part_flag_t;


#define CGRP_NO_CONTROL (-1)
#define CGRP_NO_LIMIT     0

typedef struct {
    char             *name;                 /* name of this partition */
    char             *path;                 /* path to this partition */
    int               flags;                  /* partition flags */
    struct {                                /* control file descriptors */
        int           tasks;                  /* partition tasks */
        int           freeze;                 /* partition freezer */
        int           cpu;                    /* CPU share/weight */
        int           mem;                    /* memory limit */
    } control;
    struct {                                /* resource limits */
        unsigned int  cpu;                    /* CPU shares */
        u64_t         mem;                    /* max memory in bytes */
        int           rt_period;              /* total CPU period */
        int           rt_runtime;             /* allowed realtime period */
    } limit;

#if 0    
    list_hook_t       hash_bucket;          /* hook to hash bucket chain */
    list_hook_t       hash_next;            /* hook to next hash entry */
#endif

    cgrp_ctrl_setting_t *settings;          /* extra cgroup controls */
} cgrp_partition_t;


/*
 * a process classification group
 */

typedef enum {
    CGRP_GROUPFLAG_STATIC,                  /* statically partitioned group */
    CGRP_GROUPFLAG_FACT,                    /* export to factstore */
    CGRP_GROUPFLAG_REASSIGN,                /* partitioning has failed */
} cgrp_group_flag_t;

#define CGRP_DEFAULT_PRIORITY 0xffff

typedef struct {
    char             *name;                 /* group name */
    char             *description;          /* group description */
    int               flags;                /* group flags */
    list_hook_t       processes;            /* processes in this group */
    cgrp_partition_t *partition;            /* current partititon */
    OhmFact          *fact;                 /* fact for this group */
    int               priority;             /* priority if given */
} cgrp_group_t;


/*
 * classification actions
 */

union cgrp_action_u;
typedef union cgrp_action_u cgrp_action_t;

typedef enum {
    CGRP_ACTION_UNKNOWN = 0,
    CGRP_ACTION_GROUP,                      /* assign process to a group */
    CGRP_ACTION_SCHEDULE,                   /* set scheduling policy */
    CGRP_ACTION_RENICE,                     /* renice process */
    CGRP_ACTION_RECLASSIFY,                 /* reclassify after a delay */
    CGRP_ACTION_CLASSIFY_ARGV0,             /* classify by argv[0] */
    CGRP_ACTION_IGNORE,                     /* apply the default rules */
    CGRP_ACTION_MAX,
} cgrp_action_type_t;


#define CGRP_ACTION_COMMON                  /* common action fields */  \
    cgrp_action_type_t  type;               /*   action type */         \
    cgrp_action_t      *next                /*   next action if any */

typedef struct {
    CGRP_ACTION_COMMON;                     /* common action fields */
} cgrp_action_any_t;

typedef struct {
    CGRP_ACTION_COMMON;                     /* common action fields */
    cgrp_group_t *group;                    /* group to assign to */
} cgrp_action_group_t;

typedef struct {
    CGRP_ACTION_COMMON;                     /* common action fields */
    int policy;                             /* scheduling policy */
    int priority;                           /* priority if applicable */
} cgrp_action_schedule_t;

typedef struct {
    CGRP_ACTION_COMMON;                     /* common action fields */
    int priority;                           /* scheduling priority */
} cgrp_action_renice_t;

typedef struct {
    CGRP_ACTION_COMMON;                     /* common action fields */
    int delay;                              /* retry after this many msecs */
} cgrp_action_classify_t;


union cgrp_action_u {
    cgrp_action_type_t     type;
    cgrp_action_any_t      any;
    cgrp_action_group_t    group;
    cgrp_action_schedule_t schedule;
    cgrp_action_renice_t   renice;
    cgrp_action_classify_t classify;
};



/*
 * classification statement expressions
 */

typedef enum {
    CGRP_VALUE_TYPE_UNKNOWN = 0,
    CGRP_VALUE_TYPE_STRING,
    CGRP_VALUE_TYPE_UINT32,
    CGRP_VALUE_TYPE_SINT32,
} cgrp_value_type_t;

typedef struct {
    cgrp_value_type_t type;
    union {
        char  *str;
        u32_t  u32;
        s32_t  s32;
    };
} cgrp_value_t;

typedef enum {
    CGRP_EXPR_UNKNOWN = 0,
    CGRP_EXPR_BOOL,
    CGRP_EXPR_PROP,
} cgrp_expr_type_t;

typedef union cgrp_expr_u cgrp_expr_t;

#define CGRP_EXPR_COMMON \
    cgrp_expr_type_t type


typedef enum {
    CGRP_BOOL_UNKNOWN = 0,
    CGRP_BOOL_AND,
    CGRP_BOOL_OR,
    CGRP_BOOL_NOT
} cgrp_bool_op_t;

typedef struct {
    CGRP_EXPR_COMMON;
    cgrp_bool_op_t  op;
    cgrp_expr_t    *arg1;
    cgrp_expr_t    *arg2;
} cgrp_bool_expr_t;

typedef enum {
    CGRP_OP_UNKNOWN = 0,
    CGRP_OP_EQUAL,
    CGRP_OP_NOTEQ,
    CGRP_OP_LESS,
} cgrp_prop_op_t;

#define CGRP_MAX_ARGS     32                /* max command line argument */
#define CGRP_AVG_ARG      64                /* average argument length */
#define CGRP_MAX_CMDLINE (CGRP_MAX_ARGS * CGRP_AVG_ARG)

typedef enum {
    CGRP_PROP_BINARY,
    CGRP_PROP_ARG0,
    /* CGRP_PROP_ARG(1) ... CGRP_PROP_ARG(CGRP_MAX_ARGS - 1) */
    CGRP_PROP_ARG_MAX = CGRP_PROP_ARG0 + CGRP_MAX_ARGS - 1,
    CGRP_PROP_CMDLINE,
    CGRP_PROP_NAME,
    CGRP_PROP_TYPE,
    CGRP_PROP_PARENT,
    CGRP_PROP_EUID,
    CGRP_PROP_EGID,
    CGRP_PROP_RECLASSIFY
} cgrp_prop_type_t;

#define CGRP_PROP_ARG(n) ((cgrp_prop_type_t)(CGRP_PROP_ARG0 + (n)))

typedef struct {
    CGRP_EXPR_COMMON;
    cgrp_prop_type_t prop;
    cgrp_prop_op_t   op;
    cgrp_value_t     value;
} cgrp_prop_expr_t;


union cgrp_expr_u {
    cgrp_expr_type_t type;                  /* expression type */
    cgrp_bool_expr_t bool;                  /* boolean expression */
    cgrp_prop_expr_t prop;                  /* property expression */
};
    

/*
 * classification statements (conditional commands)
 */

typedef struct cgrp_stmt_s cgrp_stmt_t;
struct cgrp_stmt_s {
    cgrp_expr_t   *expr;                    /* test expression */
    cgrp_action_t *actions;                 /* actions if expr is true */
    cgrp_stmt_t   *next;                    /* more statements */
};


/*
 * a process definition
 */

typedef struct {
    char          *binary;                  /* path to binary */
    int            renice;                  /* XXX temporary renicing kludge */
    cgrp_stmt_t   *statements;              /* classification statements */
} cgrp_procdef_t;


/*
 * a classified process
 */

typedef struct {
    pid_t         pid;                      /* process id */
    char         *binary;                   /* path to binary */
    char         *argv0;                    /* argv[0] if needed */
    cgrp_group_t *group;                    /* current group */
    list_hook_t   proc_hook;                /* hook to process table */
    list_hook_t   group_hook;               /* hook to group */
} cgrp_process_t;

typedef enum {
    CGRP_PROC_BINARY = 0,                   /* process binary path */
    CGRP_PROC_ARG0   = CGRP_PROP_ARG0,      /* process arguments */
    /* CGRP_PROC_ARG(1) ... CGRP_PROC_ARG(CGRP_MAX_ARGS - 1) */
    CPGR_PROC_ARG_MAX = CGRP_PROC_ARG0 + CGRP_MAX_ARGS - 1,
    CGRP_PROC_CMDLINE,                      /* process command line */
    CGRP_PROC_NAME,                         /* process name */
    CGRP_PROC_TYPE,                         /* process type */
    CGRP_PROC_PPID,                         /* process parent binary */
    CGRP_PROC_EUID,                         /* effective user ID */
    CGRP_PROC_EGID,                         /* effective group ID */
    CGRP_PROC_RECLASSIFY,                   /* being reclassified ? */
} cgrp_proc_attr_type_t;

#define CGRP_PROC_ARG(n) ((cgrp_proc_attr_type_t)(CGRP_PROC_ARG0 + (n)))

typedef enum {
    CGRP_PROC_UNKNOWN = 0,
    CGRP_PROC_USER,
    CGRP_PROC_KERNEL,
} cgrp_proc_type_t;

#define CGRP_COMM_LEN 16                    /* TASK_COMM_LEN */

typedef struct {
    cgrp_mask_t        mask;                /* attribute mask */
    pid_t              pid;                 /* process id */
    pid_t              ppid;                /* parent process id */
    char              *binary;              /* path to binary */
    char               name[CGRP_COMM_LEN]; /* task_struct.comm */
    cgrp_proc_type_t   type;                /* user or kernel process */
    char              *cmdline;             /* command line */
    char             **argv;                /* command line arguments */
    int                argc;                /* number of arguments */
    uid_t              euid;                /* effective user id */
    gid_t              egid;                /* effective group id */
    int                reclassify;          /* reclassification count */
    int                byargvx;             /* classifying by argv[x] */
} cgrp_proc_attr_t;



#if 0

/*
 * a hash table
 */

typedef struct {
    list_hook_t *buckets;                   /* hash buckets */
    int          nbucket;                   /* number of buckets */
    list_hook_t  entries;                   /* hash table entries */
} cgrp_hash_table_t;

#endif


/*
 * system partitioning context
 */

enum {
    CGRP_FLAG_GROUP_FACTS = 0,
    CGRP_FLAG_PART_FACTS,
    CGRP_FLAG_MOUNT_FREEZER,
    CGRP_FLAG_MOUNT_CPU,
    CGRP_FLAG_MOUNT_MEMORY,
    CGRP_FLAG_MOUNT_CPUSET,
    CGRP_FLAG_ADDON_RULES,
    CGRP_FLAG_ADDON_MONITOR,
};


typedef struct {
    int   flags;
    char *addon_rules;                      /* add-on rule pattern */
} cgrp_options_t;


typedef enum {
    ESTIM_TYPE_UNKNOWN = 0,
    ESTIM_TYPE_WINDOW,                      /* sliding window estimator */
    ESTIM_TYPE_EWMA,                        /* EWMA estimator */
} estim_type_t;

typedef struct {
    estim_type_t  type;                     /* ESTIM_TYPE_WINDOW */
    unsigned long total;                    /* total */
    int           size;                     /* window size */
    int           ready;                    /* has enough items ? */
    int           idx;                      /* current index */
    unsigned long items[0];                 /* actual items */
} sldwin_t;

typedef struct {
    estim_type_t type;                      /* ESTIM_TYPE_EWMA */
    double       alpha;                     /* smoothing factor, 2 / (N + 1) */
    double       S;                         /* current moving average */
} ewma_t;

typedef union {
    estim_type_t type;                      /* estimator type */
    sldwin_t     win;                       /* a sliding window integrator */
    ewma_t       ewma;                      /* a weighted moving average */
} estim_t;

typedef struct timespec timestamp_t;

typedef struct {
    unsigned int     thres_low;             /* low threshold */
    unsigned int     thres_high;            /* high threshold */
    unsigned int     poll_low;              /* poll interval when low */
    unsigned int     poll_high;             /* poll interval when high */
    estim_type_t     type;                  /* estimator type */
    unsigned int     nsample;               /*   and number of samples */
    estim_t         *estim;                 /* estimator */
    char            *hook;                  /* resolver notification hook */
    unsigned int     startup_delay;         /* initial delay before sampling */
    
    unsigned long    sample;                /* last sample */
    timestamp_t      stamp;                 /*   and its timestamp */
    guint            timer;                 /* next sampling timer */
    int              alert;                 /* whether above high threshold */
} cgrp_iowait_t;


typedef struct {
    char            *path;                  /* sysdev path */
    unsigned int     thres_low;             /* low threshold */
    unsigned int     thres_high;            /* high threshold */
    unsigned int     period;                /* calculation period (msec) */
    char            *hook;                  /* resolver notification hook */

    int              qafd;                  /* average queue length fd */
    GIOChannel      *gioc;                  /*   associated GIO channel */
    guint            gsrc;                  /*   and event source */
} cgrp_ioqlen_t;


typedef struct {
    unsigned int     low;                   /* low threshold */
    unsigned int     high;                  /* hight threshold */
    unsigned int     interval;              /* minimum notification interval */
    char            *hook;                  /* notification hook */
} cgrp_swap_t;


typedef struct {
    char             *desired_mount;        /* desired mount point */
    char             *actual_mount;         /* actual mount point */
    unsigned int      cgroup_options;       /* cgroup mount options */
    cgrp_ctrl_t      *controls;             /* cgroup extra controls */

    cgrp_partition_t *root;                 /* root partition */
    cgrp_group_t     *groups;               /* classification groups */
    int               ngroup;               /* number of groups */
    cgrp_procdef_t   *procdefs;             /* process definitions */
    int               nprocdef;             /* number of process definitions */
    cgrp_procdef_t   *fallback;             /* fallback process definition */
    cgrp_procdef_t   *addons;               /* add-on rules */
    int               naddon;               /* number of add-on rules */
    int               addonwd;              /* addon watch descriptor */
    GIOChannel       *addonchnl;            /* g I/O channel and */
    guint             addonsrc;             /*   event source */
    guint             addontmr;             /* addon reload timer */

    cgrp_options_t    options;              /* global options */

    GHashTable       *ruletbl;              /* lookup table of procdefs */
    GHashTable       *addontbl;             /* lookup table of extra procdefs */
    GHashTable       *grouptbl;             /* lookup table of groups */
    GHashTable       *parttbl;              /* lookup table of partitions */
    list_hook_t      *proctbl;              /* lookup table of processes */

    cgrp_process_t   *active_process;       /* currently active process */
    cgrp_group_t     *active_group;         /* currently active group */
    list_hook_t       procsubscr;           /* event subscribers */

    OhmFactStore     *store;                /* ohm factstore */
    GObject          *sigconn;              /* policy signaling interface */
    gulong            sigdcn;               /* policy decision id */
    gulong            sigkey;               /* policy keychange id */
    
    int               apptrack_sock;        /* notification fd */
    GIOChannel       *apptrack_chnl;        /* associated I/O channel */
    guint             apptrack_src;         /*     and event source */
    list_hook_t       apptrack_subscribers; /* list of subscribers */
    OhmFact          *apptrack_changes;     /* $application_changes */
    guint             apptrack_update;      /* scheduled change update */
    
    int             (*resolve)(char *, char **);

    /* I/O wait monitoring */
    int               proc_stat;            /* /proc/stat fd */
    cgrp_iowait_t     iow;                  /* I/O-wait state monitoring */
    cgrp_ioqlen_t     ioq;                  /* I/O queue length monitoring */
    cgrp_swap_t       swp;                  /* swap pressure monitoring */
} cgrp_context_t;


#define CGRP_RECLASSIFY_MAX 16

typedef struct {
    cgrp_context_t *ctx;
    pid_t           pid;
    unsigned int    count;
} cgrp_reclassify_t;



/* cgrp-plugin.c */
extern int DBG_EVENT, DBG_PROCESS, DBG_CLASSIFY, DBG_NOTIFY, DBG_ACTION;
extern int DBG_SYSMON, DBG_CONFIG;

/* cgrp-process.c */
int  proc_init(cgrp_context_t *);
void proc_exit(cgrp_context_t *);

char   *process_get_binary (cgrp_proc_attr_t *);
char   *process_get_cmdline(cgrp_proc_attr_t *);
char   *process_get_name   (cgrp_proc_attr_t *);
char  **process_get_argv   (cgrp_proc_attr_t *, int);
uid_t   process_get_euid   (cgrp_proc_attr_t *);
gid_t   process_get_egid   (cgrp_proc_attr_t *);
pid_t   process_get_ppid   (cgrp_proc_attr_t *);

cgrp_proc_type_t process_get_type(cgrp_proc_attr_t *);

int process_ignore(cgrp_context_t *, cgrp_process_t *);
int process_remove_by_pid(cgrp_context_t *, pid_t);
int process_scan_proc(cgrp_context_t *);
int process_update_state(cgrp_context_t *, cgrp_process_t *, char *);
int process_set_priority(cgrp_process_t *, int);
void procattr_dump(cgrp_proc_attr_t *);

void proc_notify(cgrp_context_t *,
                 void (*)(cgrp_context_t *, int, pid_t, void *), void *);

/* cgrp-partition.c */
int  partition_init(cgrp_context_t *);
void partition_exit(cgrp_context_t *);
int  partition_config(cgrp_context_t *);

cgrp_partition_t *partition_add(cgrp_context_t *, cgrp_partition_t *);
void              partition_del(cgrp_context_t *, cgrp_partition_t *);
cgrp_partition_t *partition_lookup(cgrp_context_t *, const char *);
int               partition_add_root(cgrp_context_t *);

void partition_dump(cgrp_context_t *, FILE *);
void partition_print(cgrp_partition_t *, FILE *);
int partition_add_process(cgrp_partition_t *, pid_t);
int partition_add_group(cgrp_partition_t *, cgrp_group_t *, int);
int partition_freeze(cgrp_context_t *, cgrp_partition_t *, int);
int partition_limit_cpu(cgrp_partition_t *, unsigned int);
int partition_limit_mem(cgrp_partition_t *, unsigned int);
int partition_limit_rt(cgrp_partition_t *, int, int);
int partition_apply_settings(cgrp_context_t *, cgrp_partition_t *);

void ctrl_dump(cgrp_context_t *, FILE *);
void ctrl_del(cgrp_ctrl_t *);
void ctrl_setting_del(cgrp_ctrl_setting_t *);
int ctrl_apply(cgrp_context_t *, cgrp_partition_t *, cgrp_ctrl_setting_t *);

/* cgrp-group.c */
int  group_init(cgrp_context_t *);
void group_exit(cgrp_context_t *);
int  group_config(cgrp_context_t *);

cgrp_group_t *group_add(cgrp_context_t *, cgrp_group_t *);
void          group_purge(cgrp_context_t *, cgrp_group_t *);
cgrp_group_t *group_find(cgrp_context_t *, const char *);
cgrp_group_t *group_lookup(cgrp_context_t *, const char *);

void group_dump(cgrp_context_t *, FILE *);
void group_print(cgrp_context_t *, cgrp_group_t *, FILE *);

int  group_add_process(cgrp_context_t *, cgrp_group_t *, cgrp_process_t *);
int  group_del_process(cgrp_process_t *);
int  group_set_priority(cgrp_group_t *, int);


/* cgrp_procdef.c */
int  procdef_init(cgrp_context_t *);
void procdef_exit(cgrp_context_t *);

int  procdef_add(cgrp_context_t *, cgrp_procdef_t *);
void procdef_purge(cgrp_procdef_t *);

int  addon_add(cgrp_context_t *, cgrp_procdef_t *);
void addon_reset(cgrp_context_t *);
int  addon_reload(cgrp_context_t *);

void procdef_dump(cgrp_context_t *, FILE *);
void procdef_print(cgrp_context_t *, cgrp_procdef_t *, FILE *);
cgrp_action_t *rule_eval(cgrp_context_t *,
                         cgrp_procdef_t *, cgrp_proc_attr_t *);


/* cgrp-classify.c */
int  classify_init  (cgrp_context_t *);
void classify_exit  (cgrp_context_t *);
int  classify_config(cgrp_context_t *);
int  classify_reconfig(cgrp_context_t *);
int  classify_by_binary(cgrp_context_t *, pid_t, int);
int  classify_by_argvx(cgrp_context_t *, cgrp_proc_attr_t *, int);


void classify_schedule(cgrp_context_t *, pid_t, unsigned int, int);


/* cgrp-action.c */
cgrp_action_t *action_add(cgrp_action_t *, cgrp_action_t *);
void           action_del(cgrp_action_t *);

int            action_print(cgrp_context_t *, FILE *, cgrp_action_t *);
int            action_exec (cgrp_context_t *, cgrp_proc_attr_t *,
                            cgrp_action_t *);
void           action_free (cgrp_action_t *);

cgrp_action_t *action_group_new   (cgrp_group_t *);
cgrp_action_t *action_schedule_new(char *, int);
cgrp_action_t *action_renice_new  (int);
cgrp_action_t *action_classify_new(int);
cgrp_action_t *action_ignore_new  (void);




/* cgrp-ep.c */
int  ep_init(cgrp_context_t *, GObject *(*)(gchar *, gchar **));
void ep_exit(cgrp_context_t *, gboolean (*)(GObject *));



/* cgrp-hash.c */
int  rule_hash_init  (cgrp_context_t *);
void rule_hash_exit  (cgrp_context_t *);
int  rule_hash_insert(cgrp_context_t *, cgrp_procdef_t *);
int  rule_hash_remove(cgrp_context_t *, const char *);
cgrp_procdef_t *rule_hash_lookup(cgrp_context_t *, const char *);

int  addon_hash_init  (cgrp_context_t *);
void addon_hash_exit  (cgrp_context_t *);
void addon_hash_reset (cgrp_context_t *);
int  addon_hash_insert(cgrp_context_t *, cgrp_procdef_t *);
int  addon_hash_remove(cgrp_context_t *, const char *);
cgrp_procdef_t *addon_hash_lookup(cgrp_context_t *, const char *);
void addon_hash_dump(cgrp_context_t *, FILE *);


int  proc_hash_init  (cgrp_context_t *);
void proc_hash_exit  (cgrp_context_t *);
int  proc_hash_insert(cgrp_context_t *, cgrp_process_t *);
cgrp_process_t *proc_hash_remove(cgrp_context_t *, pid_t);
void proc_hash_unhash(cgrp_context_t *, cgrp_process_t *);
cgrp_process_t *proc_hash_lookup(cgrp_context_t *, pid_t);
void proc_hash_foreach(cgrp_context_t *,
                       void (*)(cgrp_context_t *, cgrp_process_t *, void *),
                       void *);
int  group_hash_init  (cgrp_context_t *);
void group_hash_exit  (cgrp_context_t *);
int  group_hash_insert(cgrp_context_t *, cgrp_group_t *);
int  group_hash_delete(cgrp_context_t *, const char *);
cgrp_group_t *group_hash_lookup(cgrp_context_t *, const char *);

int  part_hash_init  (cgrp_context_t *);
void part_hash_exit  (cgrp_context_t *);
int  part_hash_insert(cgrp_context_t *, cgrp_partition_t *);
int  part_hash_delete(cgrp_context_t *, const char *);
cgrp_partition_t *part_hash_lookup(cgrp_context_t *, const char *);
void part_hash_foreach(cgrp_context_t *, GHFunc , void *);
int  cgroup_set_option(cgrp_context_t *, char *);



/* cgrp-eval.c */
cgrp_expr_t *bool_expr(cgrp_bool_op_t, cgrp_expr_t *, cgrp_expr_t *);
cgrp_expr_t *prop_expr(cgrp_prop_type_t, cgrp_prop_op_t, cgrp_value_t *);
void statement_free_all(cgrp_stmt_t *);

void statements_print(cgrp_context_t *, cgrp_stmt_t *, FILE *);
void statement_print(cgrp_context_t *, cgrp_stmt_t *, FILE *);
void expr_print(cgrp_context_t *, cgrp_expr_t *, FILE *);
void bool_print(cgrp_context_t *, cgrp_bool_expr_t *, FILE *);
void prop_print(cgrp_context_t *, cgrp_prop_expr_t *, FILE *);
void value_print(cgrp_context_t *, cgrp_value_t *, FILE *);
int  expr_eval(cgrp_context_t *, cgrp_expr_t *, cgrp_proc_attr_t *);


/* cgrp-config.y */
int  config_parse_config(cgrp_context_t *, char *);
int  config_parse_addons(cgrp_context_t *);
void config_print(cgrp_context_t *, FILE *);
void config_schedule_reload(cgrp_context_t *);
int  config_monitor_init(cgrp_context_t *);
void config_monitor_exit(cgrp_context_t *);



/* cgrp-lexer.l */
void        lexer_reset(int);
void        lexer_disable_include(void);
void        lexer_enable_include(void);
int         lexer_push_input(char *);
int         lexer_line (void);
const char *lexer_file (void);

/* cgrp-utils.c */
uid_t cgrp_getuid(const char *);
gid_t cgrp_getgid(const char *);

/* cgrp-facts.c */
int  fact_init(cgrp_context_t *);
void fact_exit(cgrp_context_t *);

OhmFact *fact_create(cgrp_context_t *, const char *, const char *);
void     fact_delete(cgrp_context_t *, OhmFact *);

void fact_add_process(OhmFact *, cgrp_process_t *);
void fact_del_process(OhmFact *, cgrp_process_t *);

/* cgrp-apptrack.c */
int  apptrack_init(cgrp_context_t *, OhmPlugin *);
void apptrack_exit(cgrp_context_t *);
int  apptrack_group_change(cgrp_context_t *, cgrp_group_t *, cgrp_group_t *);
void apptrack_subscribe(void (*)(pid_t, const char *, const char *,
                                 const char *, void *),
                        void *);
void apptrack_unsubscribe(void (*)(pid_t, const char *, const char *,
                                   const char *, void *),
                          void *);
void apptrack_query(pid_t *, const char **, const char **, const char **);


/* cgrp-console.c */
int  console_init(cgrp_context_t *);
void console_exit(void);

/* cgrp-sysmon.c */
int  sysmon_init(cgrp_context_t *);
void sysmon_exit(cgrp_context_t *);

estim_t *estim_alloc(char *, int);



#endif /* __OHM_PLUGIN_CGRP_H__ */




/* 
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */
