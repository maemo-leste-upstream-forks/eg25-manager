/*
 * Copyright (C) 2019 Purism SPC
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * Copied and adapted from Wys' `main.c`: https://source.puri.sm/Librem5/wys/
 * Author: Bob Ham <bob.ham@puri.sm>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mm-iface.h"

#include <string.h>

static void state_changed_cb(MMModem                  *modem,
                             MMModemState              old,
                             MMModemState              new,
                             MMModemStateChangeReason  reason,
                             struct EG25Manager       *manager)
{
    if (manager->modem_state >= EG25_STATE_CONFIGURED)
        modem_update_state(manager, new);
}

static void add_modem(struct EG25Manager *manager, GDBusObject *object)
{
    const gchar *path;
    MmGdbusModem *gdbus_modem;

    path = g_dbus_object_get_object_path(object);
    g_message("Adding new modem `%s'", path);

    g_assert(MM_IS_OBJECT (object));
    manager->mm_modem = mm_object_get_modem(MM_OBJECT(object));
    g_assert(manager->mm_modem != NULL);

    if (manager->modem_state == EG25_STATE_RESUMING) {
        if (manager->modem_recovery_timer) {
            g_source_remove(manager->modem_recovery_timer);
            manager->modem_recovery_timer = 0;
        }
        modem_resume_post(manager);
        manager->modem_state = EG25_STATE_CONFIGURED;
    }

    if (manager->modem_state < EG25_STATE_ACQUIRED)
        manager->modem_state = EG25_STATE_ACQUIRED;

    if (manager->modem_state < EG25_STATE_CONFIGURED)
        modem_configure(manager);

    path = mm_modem_get_device(manager->mm_modem);

    if (manager->modem_usb_id)
        g_free(manager->modem_usb_id);
    manager->modem_usb_id = g_strdup(strrchr(path, '/') + 1);

    gdbus_modem = MM_GDBUS_MODEM(manager->mm_modem);

    g_signal_connect(gdbus_modem, "state-changed", G_CALLBACK(state_changed_cb), manager);
}

static void interface_added_cb (struct EG25Manager *manager,
                                GDBusObject        *object,
                                GDBusInterface     *interface)
{
    GDBusInterfaceInfo *info;

    info = g_dbus_interface_get_info(interface);
    g_message("ModemManager interface `%s' found on object `%s'",
              info->name, g_dbus_object_get_object_path(object));

    if (g_strcmp0(info->name, MM_DBUS_INTERFACE_MODEM) == 0)
        add_modem(manager, object);
}


static void interface_removed_cb(struct EG25Manager *manager,
                                 GDBusObject        *object,
                                 GDBusInterface     *interface)
{
    const gchar *path;
    GDBusInterfaceInfo *info;

    path = g_dbus_object_get_object_path(object);
    info = g_dbus_interface_get_info(interface);

    g_message("ModemManager interface `%s' removed on object `%s'", info->name, path);

    if (g_strcmp0(info->name, MM_DBUS_INTERFACE_MODEM) == 0)
        manager->mm_modem = NULL;
}


static void add_mm_object(struct EG25Manager *manager, GDBusObject *object)
{
    GList *ifaces, *node;

    ifaces = g_dbus_object_get_interfaces(object);
    for (node = ifaces; node; node = node->next)
        interface_added_cb(manager, object, G_DBUS_INTERFACE(node->data));

    g_list_free_full(ifaces, g_object_unref);
}


static void add_mm_objects(struct EG25Manager *manager)
{
    GList *objects, *node;

    objects = g_dbus_object_manager_get_objects(G_DBUS_OBJECT_MANAGER(manager->mm_manager));
    for (node = objects; node; node = node->next)
        add_mm_object(manager, G_DBUS_OBJECT(node->data));

    g_list_free_full(objects, g_object_unref);
}


static void object_added_cb(struct EG25Manager *manager, GDBusObject *object)
{
    g_message("ModemManager object `%s' added", g_dbus_object_get_object_path(object));
    add_mm_object(manager, object);
}


static void object_removed_cb(struct EG25Manager *manager, GDBusObject *object)
{
    const gchar *path;

    path = g_dbus_object_get_object_path(object);
    g_message("ModemManager object `%s' removed", path);

    manager->mm_modem = NULL;
}


static void mm_manager_new_cb(GDBusConnection    *connection,
                              GAsyncResult       *res,
                              struct EG25Manager *manager)
{
    g_autoptr (GError) error = NULL;

    manager->mm_manager = mm_manager_new_finish(res, &error);
    if (!manager->mm_manager)
        g_critical("Error creating ModemManager Manager: %s", error->message);

    g_signal_connect_swapped(G_DBUS_OBJECT_MANAGER(manager->mm_manager),
                             "interface-added", G_CALLBACK(interface_added_cb), manager);
    g_signal_connect_swapped(G_DBUS_OBJECT_MANAGER(manager->mm_manager),
                             "interface-removed", G_CALLBACK(interface_removed_cb), manager);
    g_signal_connect_swapped(G_DBUS_OBJECT_MANAGER(manager->mm_manager),
                             "object-added", G_CALLBACK(object_added_cb), manager);
    g_signal_connect_swapped(G_DBUS_OBJECT_MANAGER(manager->mm_manager),
                             "object-removed", G_CALLBACK(object_removed_cb), manager);

    add_mm_objects(manager);
}

static void mm_appeared_cb(GDBusConnection    *connection,
                           const gchar        *name,
                           const gchar        *name_owner,
                           struct EG25Manager *manager)
{
    g_message("ModemManager appeared on D-Bus");

    if (manager->modem_iface != MODEM_IFACE_NONE) {
        g_critical("Modem interface already found! Make sure to only run either of ModemManager or oFono.");
        return;
    }
    manager->modem_iface = MODEM_IFACE_MODEMMANAGER;

    mm_manager_new(connection, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                   NULL, (GAsyncReadyCallback)mm_manager_new_cb, manager);
}

static void mm_iface_clean(struct EG25Manager *manager)
{
    if (manager->mm_manager) {
        g_clear_object(&manager->mm_manager);
        manager->mm_manager = NULL;
    }
    if (manager->modem_usb_id) {
        g_free(manager->modem_usb_id);
        manager->modem_usb_id = NULL;
    }
    if (manager->modem_iface == MODEM_IFACE_MODEMMANAGER) {
        manager->modem_iface = MODEM_IFACE_NONE;
    }
}

static void mm_vanished_cb(GDBusConnection    *connection,
                           const gchar        *name,
                           struct EG25Manager *manager)
{
    g_message("ModemManager vanished from D-Bus");
    mm_iface_clean(manager);
}

void mm_iface_init(struct EG25Manager *manager, toml_table_t *config)
{
    manager->mm_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM, MM_DBUS_SERVICE,
                                         G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
                                         (GBusNameAppearedCallback)mm_appeared_cb,
                                         (GBusNameVanishedCallback)mm_vanished_cb,
                                         manager, NULL);
}

void mm_iface_destroy(struct EG25Manager *manager)
{
    mm_iface_clean(manager);
    if (manager->mm_watch != 0) {
        g_bus_unwatch_name(manager->mm_watch);
        manager->mm_watch = 0;
    }
}
