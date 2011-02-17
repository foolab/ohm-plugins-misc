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


#include <errno.h>
#include <sched.h>

#include "cgrp-plugin.h"

#define PROC_BUCKETS 1024

/********************
 * classify_init
 ********************/
int
classify_init(cgrp_context_t *ctx)
{
    if (!rule_hash_init(ctx) || !proc_hash_init(ctx) || !addon_hash_init(ctx)) {
        classify_exit(ctx);
        return FALSE;
    }
    else
        return TRUE;
}
    

/********************
 * classify_exit
 ********************/
void
classify_exit(cgrp_context_t *ctx)
{
    rule_hash_exit(ctx);
    proc_hash_exit(ctx);
}


/********************
 * classify_config
 ********************/
int
classify_config(cgrp_context_t *ctx)
{
    cgrp_procdef_t *pd;
    int             i;

    for (i = 0, pd = ctx->procdefs; i < ctx->nprocdef; i++, pd++)
        if (!rule_hash_insert(ctx, pd))
            return FALSE;

    for (i = 0, pd = ctx->addons; i < ctx->naddon; i++, pd++)
        addon_hash_insert(ctx, pd);
    
    return TRUE;
}


/********************
 * classify_reconfig
 ********************/
int
classify_reconfig(cgrp_context_t *ctx)
{
    cgrp_procdef_t *pd;
    int             i;

    for (i = 0, pd = ctx->addons; i < ctx->naddon; i++, pd++)
        addon_hash_insert(ctx, pd);
    
    return TRUE;
}


/********************
 * classify_by_parent
 ********************/
int
classify_by_parent(cgrp_context_t *ctx, pid_t pid, pid_t tgid, pid_t ppid)
{
    cgrp_process_t   *parent, *process;
    cgrp_proc_attr_t  attr;


    if ((parent = proc_hash_lookup(ctx, ppid)) != NULL) {
        attr.pid    = pid;
        attr.tgid   = tgid;
        attr.binary = parent->binary;

        CGRP_SET_MASK(attr.mask, CGRP_PROC_TGID);
        CGRP_SET_MASK(attr.mask, CGRP_PROC_BINARY);
        
        process = process_create(ctx, &attr);
        
        if (process == NULL) {
            OHM_ERROR("cgrp: failed to allocate new process");
            return FALSE;
        }

        OHM_DEBUG(DBG_CLASSIFY, "<%u, %s>: group %s",
                  process->pid, process->binary, parent->group->name);
        group_add_process(ctx, parent->group, process);

        return TRUE;
    }
    else
        return FALSE;
}


/********************
 * classify_event
 ********************/
int
classify_event(cgrp_context_t *ctx, cgrp_event_t *event)
{
    cgrp_proc_attr_t  attr;
    char             *argv[CGRP_MAX_ARGS];
    char              args[CGRP_MAX_CMDLINE];
    char              cmdl[CGRP_MAX_CMDLINE];
    char              bin[PATH_MAX];
    
    OHM_DEBUG(DBG_CLASSIFY, "classification event 0x%x for <%u/%u>",
              event->any.type, event->any.tgid, event->any.pid);
    
    switch (event->any.type) {
    case CGRP_EVENT_FORK:
        if (classify_by_parent(ctx, event->fork.pid, event->fork.tgid,
                               event->fork.ppid))
            return TRUE;
        /* intentional fallthrough */
    
    case CGRP_EVENT_FORCE:
    case CGRP_EVENT_EXEC:
    case CGRP_EVENT_UID:
    case CGRP_EVENT_GID:
    case CGRP_EVENT_SID:
    case CGRP_EVENT_NAME:
        if ((ctx->event_mask & (1 << event->any.type)) == 0)
            return TRUE;
        
        memset(&attr, 0, sizeof(attr));
        bin[0]         = '\0';
    
        attr.binary  = bin;
        attr.pid     = event->any.pid;
        attr.tgid    = event->any.tgid;
        attr.argv    = argv;
        argv[0]      = args;
        attr.cmdline = cmdl;
        attr.process = proc_hash_lookup(ctx, attr.pid);

        if (!process_get_binary(&attr))
            return FALSE;                   /* we assume it's gone already */
        else {
            if (event->any.type == CGRP_EVENT_EXEC && attr.process != NULL) {
                FREE(attr.process->binary);
                attr.process->binary = STRDUP(attr.binary);
            }

            return classify_by_rules(ctx, event, &attr);
        }
        
    case CGRP_EVENT_EXIT:
        process_remove_by_pid(ctx, event->any.pid);
        return TRUE;

    default:
        return FALSE;
    }
}


/********************
 * classify_by_binary
 ********************/
int
classify_by_binary(cgrp_context_t *ctx, pid_t pid, int reclassify)
{
    cgrp_proc_attr_t  attr;
    cgrp_event_t      event;
    char             *argv[CGRP_MAX_ARGS];
    char              args[CGRP_MAX_CMDLINE];
    char              cmdl[CGRP_MAX_CMDLINE];
    char              bin[PATH_MAX];
    
    OHM_DEBUG(DBG_CLASSIFY, "%sclassifying process <%u> by binary",
              reclassify ? "re" : "", pid);
    
    memset(&attr, 0, sizeof(attr));
    bin[0]  = '\0';
    argv[0] = args;
    
    attr.pid     = pid;
    attr.binary  = bin;
    attr.argv    = argv;
    attr.cmdline = cmdl;
    attr.retry   = reclassify;
    attr.process = proc_hash_lookup(ctx, pid);
    
    if (attr.process != NULL) {
        attr.binary = attr.process->binary;
        attr.tgid   = attr.process->tgid;
        CGRP_SET_MASK(attr.mask, CGRP_PROC_TGID);
        CGRP_SET_MASK(attr.mask, CGRP_PROC_BINARY);
    }
    else {
        if (!process_get_binary(&attr))
            return -ENOENT;                  /* we assume it's gone already */

        process_get_tgid(&attr);
        attr.process = process_create(ctx, &attr);
    }

    event.exec.type = CGRP_EVENT_EXEC;
    event.exec.pid  = attr.pid;
    event.exec.tgid = attr.tgid;

    return classify_by_rules(ctx, &event, &attr);
}


/********************
 * classify_by_argvx
 ********************/
int
classify_by_argvx(cgrp_context_t *ctx, cgrp_proc_attr_t *attr, int argn)
{
    cgrp_event_t event;

    if (attr->byargvx) {
        OHM_ERROR("cgrp: classify-by-argvx loop for process <%u>", attr->pid);
        return -EINVAL;
    }
    
    OHM_DEBUG(DBG_CLASSIFY, "%sclassifying process <%u> by argv%d",
              attr->retry ? "re" : "", attr->pid, argn);

    if (process_get_argv(attr, CGRP_MAX_ARGS) == NULL)
        return -ENOENT;                       /* we assume it's gone already */

    if (argn >= attr->argc) {
        OHM_WARNING("cgrp: classify-by-argv%d found only %d arguments",
                    argn, attr->argc);
        attr->binary = "<none>";          /* force fallback-rule */
    }
    else
        attr->binary = attr->argv[argn];
    
    attr->byargvx = TRUE;    
    
    event.exec.type = CGRP_EVENT_EXEC;
    event.exec.pid  = attr->pid;
    event.exec.tgid = attr->tgid;

    if (classify_by_rules(ctx, &event, attr)) {
        if (attr->process == NULL)
            attr->process = proc_hash_lookup(ctx, attr->pid);
        
        if (attr->process != NULL && attr->process->argvx == NULL)
            attr->process->argvx = STRDUP(attr->binary);
        
        return TRUE;
    }
    else
        return FALSE;
}


/********************
 * classify_by_rules
 ********************/
int
classify_by_rules(cgrp_context_t *ctx,
                  cgrp_event_t *event, cgrp_proc_attr_t *attr)
{
    cgrp_procdef_t *def;
    cgrp_rule_t    *rules;
    cgrp_action_t  *actions;

    OHM_DEBUG(DBG_CLASSIFY, "classifying process <%u> by rules for event 0x%x",
              event->any.pid, event->any.type);

    /*
     * The basic algorithm here is roughly the following:
     *
     *   1) Find classification primary classification rules by binary path.
     *
     *   2) If no primary rules were found
     *      a) give up if the triggering event is *-ID or name change and
     *         we are not configured to always use fallback rules,
     *      b) otherwise take the fallback rules.
     *
     *   3) If any rules were found
     *      3.1) Evaluate them to find actions to execute.
     *
     *      3.2) If no actions were found and we still have fallback rules
     *           evaluate fallback rules to find actions to execute.
     *
     *      3.3) If any actions were found execute them.
     */
    
    if ((def = rule_hash_lookup(ctx, attr->binary))  != NULL ||
        (def = addon_hash_lookup(ctx, attr->binary)) != NULL)
        rules = rule_find(def->rules, event);
    else
        rules = NULL;
    
    if (rules == NULL) {
        if (!CGRP_TST_FLAG(ctx->options.flags, CGRP_FLAG_ALWAYS_FALLBACK) &&
            (event->any.type == CGRP_EVENT_GID ||
             event->any.type == CGRP_EVENT_UID ||
             event->any.type == CGRP_EVENT_SID ||
             event->any.type == CGRP_EVENT_NAME)) {
            OHM_DEBUG(DBG_CLASSIFY, "no matching rule, omitting fallback.");
            return TRUE;
        }
        else
            rules = ctx->fallback;
    }

    if (rules != NULL) {
        actions = rule_eval(ctx, rules, attr);

        if (actions == NULL && rules != ctx->fallback && ctx->fallback != NULL)
            actions = rule_eval(ctx, ctx->fallback, attr);

        if (actions != NULL) {
            procattr_dump(attr);
            return action_exec(ctx, attr, actions);
        }
    }
    
    return FALSE;
}




/********************
 * reclassify_process
 ********************/
static gboolean
reclassify_process(gpointer data)
{
    cgrp_reclassify_t *reclassify = (cgrp_reclassify_t *)data;

    OHM_DEBUG(DBG_CLASSIFY, "reclassifying process <%u>", reclassify->pid);
    classify_by_binary(reclassify->ctx, reclassify->pid, reclassify->count);
    return FALSE;
}


static void
free_reclassify(gpointer data)
{
    FREE(data);
}


/********************
 * classify_schedule
 ********************/
void
classify_schedule(cgrp_context_t *ctx, pid_t pid, unsigned int delay,
                  int count)
{
    cgrp_reclassify_t *reclassify;

    if (ALLOC_OBJ(reclassify) != NULL) {
        reclassify->ctx   = ctx;
        reclassify->pid   = pid;
        reclassify->count = count;

        g_timeout_add_full(G_PRIORITY_DEFAULT, delay,
                           reclassify_process, reclassify, free_reclassify);
    }
    else
        OHM_ERROR("cgrp: failed to allocate reclassification data");
}




/* 
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */
