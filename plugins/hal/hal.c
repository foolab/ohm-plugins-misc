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


/**
 * @file hal.c
 * @brief OHM HAL plugin 
 * @author ismo.h.puustinen@nokia.com
 *
 * Copyright (C) 2008, Nokia. All rights reserved.
 */

#include "hal.h"

static int DBG_HAL, DBG_FACTS;

OHM_DEBUG_PLUGIN(hal,
    OHM_DEBUG_FLAG("hal"  , "HAL events"       , &DBG_HAL),
    OHM_DEBUG_FLAG("facts", "fact manipulation", &DBG_FACTS));

hal_plugin *hal_plugin_p = NULL;

static void
plugin_init(OhmPlugin * plugin)
{
    DBusConnection *c = ohm_plugin_dbus_get_connection();

    (void) plugin;

    if (!OHM_DEBUG_INIT(hal))
        g_warning("Failed to initialize HAL plugin debugging.");
    OHM_DEBUG(DBG_HAL, "> HAL plugin init");
    /* should we ref the connection? */
    hal_plugin_p = init_hal(c, DBG_HAL, DBG_FACTS);
    OHM_DEBUG(DBG_HAL, "< HAL plugin init");

    return;
}


OHM_EXPORTABLE(gboolean, set_observer, (gchar *capability, hal_cb cb, void *user_data))
{
    OHM_DEBUG(DBG_HAL, "> set_observer");
    return decorate(hal_plugin_p, capability, cb, user_data);
}

OHM_EXPORTABLE(gboolean, unset_observer, (void *user_data))
{
    OHM_DEBUG(DBG_HAL, "> unset_observer");
    return undecorate(hal_plugin_p, user_data);
}

static void
plugin_exit(OhmPlugin * plugin)
{
    (void) plugin;
    
    if (hal_plugin_p) {
        deinit_hal(hal_plugin_p);
        hal_plugin_p = NULL;
    }
    return;
}

DBusHandlerResult check_hald(DBusConnection *c, DBusMessage *msg,
        void *user_data)
{
    gchar *sender = NULL, *before = NULL, *after = NULL;
    gboolean ret;
    hal_plugin *plugin = hal_plugin_p;

    OHM_DEBUG(DBG_HAL, "> check_hald");

    if (plugin == NULL) {
        goto end;
    }

    ret = dbus_message_get_args(msg,
            NULL,
            DBUS_TYPE_STRING,
            &sender,
            DBUS_TYPE_STRING,
            &before,
            DBUS_TYPE_STRING,
            &after,
            DBUS_TYPE_INVALID);

    if (ret) {
        OHM_DEBUG(DBG_HAL, "  check_hald: sender '%s', before '%s', after '%s'",
                sender, before, after);

        if (!strcmp(before, "") && strcmp(after, "")) {
            /* a service just started, check if it is hald */
            if (!strcmp(sender, "org.freedesktop.Hal")) {
                reload_hal_context(c, plugin);
            }
        }
    }

end:
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

OHM_PLUGIN_DESCRIPTION("hal",
        "0.0.1",
        "ismo.h.puustinen@nokia.com",
        OHM_LICENSE_NON_FREE, plugin_init, plugin_exit,
        NULL);

OHM_PLUGIN_PROVIDES_METHODS(hal, 2,
        OHM_EXPORT(unset_observer, "unset_observer"),
        OHM_EXPORT(set_observer, "set_observer"));

OHM_PLUGIN_DBUS_SIGNALS(
     { NULL, "org.freedesktop.DBus", "NameOwnerChanged", NULL, check_hald, NULL }
);

/*
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */
