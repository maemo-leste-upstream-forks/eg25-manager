/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "at.h"
#include "gpio.h"
#include "manager.h"

#ifdef HAVE_MMGLIB
#include "mm-iface.h"
#endif

#include "ofono-iface.h"
#include "suspend.h"
#include "udev.h"

#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>
#include <libusb.h>

#ifndef EG25_CONFDIR
#define EG25_CONFDIR "/etc/eg25-manager"
#endif

#ifndef EG25_DATADIR
#define EG25_DATADIR "/usr/share/eg25-manager"
#endif

static gboolean quit_app(struct EG25Manager *manager)
{
    int i;

    g_message("Request to quit...");

    at_destroy(manager);
#ifdef HAVE_MMGLIB
    mm_iface_destroy(manager);
#endif
    ofono_iface_destroy(manager);
    suspend_destroy(manager);
    udev_destroy(manager);

    if (manager->modem_state >= EG25_STATE_STARTED) {
        g_message("Powering down the modem...");
        gpio_sequence_shutdown(manager);
        manager->modem_state = EG25_STATE_FINISHING;
        for (i = 0; i < 30; i++) {
            if (gpio_check_poweroff(manager, TRUE))
                break;
            sleep(1);
        }
    }
    g_message("Modem down, quitting...");

    g_main_loop_quit(manager->loop);

    return FALSE;
}

static gboolean modem_start(struct EG25Manager *manager)
{
    ssize_t i, count;
    gboolean should_boot = TRUE;
    libusb_context *ctx = NULL;
    libusb_device **devices = NULL;
    struct libusb_device_descriptor desc;

    if (manager->use_libusb) {
        // BH don't have the STATUS line connected, so check if USB device is present
        libusb_init(&ctx);

        count = libusb_get_device_list(ctx, &devices);
        for (i = 0; i < count; i++) {
            libusb_get_device_descriptor(devices[i], &desc);
            if (desc.idVendor == manager->usb_vid && desc.idProduct == manager->usb_pid) {
                g_message("Found corresponding USB device, modem already powered");
                should_boot = FALSE;
                break;
            }
        }

        libusb_free_device_list(devices, 1);
        libusb_exit(ctx);
    } else if (!gpio_check_poweroff(manager, FALSE)) {
        g_message("STATUS is low, modem already powered");
        should_boot = FALSE;
    }

    if (should_boot) {
        g_message("Starting modem...");
        // Modem might crash on boot (especially with worn battery) if we don't delay here
        if (manager->poweron_delay > 0)
            g_usleep(manager->poweron_delay);
        gpio_sequence_poweron(manager);
        manager->modem_state = EG25_STATE_POWERED;
    } else {
        manager->modem_state = EG25_STATE_STARTED;
    }

    return FALSE;
}

#ifdef HAVE_MMGLIB
void modem_update_state(struct EG25Manager *manager, MMModemState state)
{
    switch (state) {
    case MM_MODEM_STATE_REGISTERED:
    case MM_MODEM_STATE_DISCONNECTING:
    case MM_MODEM_STATE_CONNECTING:
        manager->modem_state = EG25_STATE_REGISTERED;
        break;
    case MM_MODEM_STATE_CONNECTED:
        manager->modem_state = EG25_STATE_CONNECTED;
        break;
    default:
        manager->modem_state = EG25_STATE_CONFIGURED;
        break;
    }
}
#endif

void modem_configure(struct EG25Manager *manager)
{
    at_sequence_configure(manager);
}

static gboolean modem_reset_done(struct EG25Manager* manager)
{
    manager->modem_state = EG25_STATE_RESUMING;
    manager->reset_timer = 0;
    return FALSE;
}

void modem_reset(struct EG25Manager *manager)
{
    int fd, ret, len;

    if (manager->reset_timer)
        return;

    /*
     * If we are managing the modem through lets say ofono, we should not
     * reset the modem based on the availability of USB ID
     * TODO: Improve ofono plugin and add support for fetching USB ID
     */
    if (manager->modem_iface != MODEM_IFACE_MODEMMANAGER)
        return;        

    if (manager->modem_recovery_timer) {
        g_source_remove(manager->modem_recovery_timer);
        manager->modem_recovery_timer = 0;
    }

    if (!manager->modem_usb_id) {
        g_warning("Unknown modem USB ID");
        goto error;
    }

    len = strlen(manager->modem_usb_id);

    manager->modem_state = EG25_STATE_RESETTING;

    fd = open("/sys/bus/usb/drivers/usb/unbind", O_WRONLY);
    if (fd < 0)
        goto error;
    ret = write(fd, manager->modem_usb_id, len);
    if (ret < len)
        g_warning("Couldn't unbind modem: wrote %d/%d bytes", ret, len);
    close(fd);

    fd = open("/sys/bus/usb/drivers/usb/bind", O_WRONLY);
    if (fd < 0)
        goto error;
    ret = write(fd, manager->modem_usb_id, len);
    if (ret < len)
        g_warning("Couldn't bind modem: wrote %d/%d bytes", ret, len);
    close(fd);

    /*
     * 3s is long enough to make sure the modem has been bound back and
     * short enough to ensure it hasn't been acquired by ModemManager
     */
    manager->reset_timer = g_timeout_add_seconds(3, G_SOURCE_FUNC(modem_reset_done), manager);

    return;

error:
    // Release blocking sleep inhibitor
    if (manager->suspend_block_fd >= 0)
        suspend_inhibit(manager, FALSE, TRUE);
    if (manager->modem_boot_timer) {
        g_source_remove(manager->modem_boot_timer);
        manager->modem_boot_timer = 0;
    }
    // Everything else failed, reboot the modem
    at_sequence_reset(manager);
}

void modem_suspend_pre(struct EG25Manager *manager)
{
    at_sequence_suspend(manager);
}

void modem_suspend_post(struct EG25Manager *manager)
{
    gpio_sequence_suspend(manager);
    g_message("suspend sequence is over, drop inhibitor");
    suspend_inhibit(manager, FALSE, FALSE);
}

void modem_resume_pre(struct EG25Manager *manager)
{
    gpio_sequence_resume(manager);
}

void modem_resume_post(struct EG25Manager *manager)
{
    at_sequence_resume(manager);
}

static toml_table_t *parse_config_file(char *config_file)
{
    toml_table_t *toml_config;
    gchar *compatible;
    gchar error[256];
    gsize len;
    FILE *f = NULL;

    if (config_file) {
        f = fopen(config_file, "r");
    } else if (g_file_get_contents("/proc/device-tree/compatible", &compatible, &len, NULL)) {
        g_autoptr (GPtrArray) compat = g_ptr_array_new();
        gsize pos = 0;

        /*
         * `compatible` file is a list of NULL-terminated strings, convert it
         * to an array
         */
        do {
            g_ptr_array_add(compat, &compatible[pos]);
            pos += strlen(&compatible[pos]) + 1;
        } while (pos < len);

        for (pos = 0; pos < compat->len; pos++) {
            g_autofree gchar *filename = g_strdup_printf(EG25_CONFDIR "/%s.toml", (gchar *)g_ptr_array_index(compat, pos));
            if (access(filename, F_OK) == 0) {
                g_message("Opening config file: %s", filename);
                f = fopen(filename, "r");
                break;
            }
        }

        if (!f) {
            for (pos = 0; pos < compat->len; pos++) {
                g_autofree gchar *filename = g_strdup_printf(EG25_DATADIR "/%s.toml", (gchar *)g_ptr_array_index(compat, pos));
                if (access(filename, F_OK) == 0) {
                    g_message("Opening config file: %s", filename);
                    f = fopen(filename, "r");
                    break;
                }
            }
        }
    }

    if (!f)
        g_error("unable to find a suitable config file!");

    toml_config = toml_parse_file(f, error, sizeof(error));
    if (!toml_config)
        g_error("unable to parse config file: %s", error);

    return toml_config;
}

int main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) opt_context = NULL;
    g_autoptr(GError) err = NULL;
    struct EG25Manager manager;
    gchar *config_file = NULL;
    toml_table_t *toml_config;
    toml_table_t *toml_manager;
    toml_datum_t toml_value;
    const GOptionEntry options[] = {
        { "config", 'c', 0, G_OPTION_ARG_STRING, &config_file, "Config file to use.", NULL },
        { NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL }
    };

    memset(&manager, 0, sizeof(manager));
    manager.at_fd = -1;
    manager.suspend_delay_fd = -1;
    manager.suspend_block_fd = -1;

    opt_context = g_option_context_new ("- Power management for the Quectel EG25 modem");
    g_option_context_add_main_entries (opt_context, options, NULL);
    if (!g_option_context_parse (opt_context, &argc, &argv, &err)) {
        g_warning ("%s", err->message);
        return 1;
    }

    manager.loop = g_main_loop_new(NULL, FALSE);

    toml_config = parse_config_file(config_file);

    toml_manager = toml_table_in(toml_config, "manager");
    if (toml_manager) {
        toml_value = toml_bool_in(toml_manager, "need_libusb");
        if (toml_value.ok)
            manager.use_libusb = toml_value.u.b;

        toml_value = toml_int_in(toml_manager, "usb_vid");
        if (toml_value.ok)
            manager.usb_vid = toml_value.u.i;

        toml_value = toml_int_in(toml_manager, "usb_pid");
        if (toml_value.ok)
            manager.usb_pid = toml_value.u.i;

        toml_value = toml_int_in(toml_manager, "poweron_delay");
        if (toml_value.ok) {
            if (toml_value.u.i >= 0 && toml_value.u.i <= G_MAXULONG) {
                // Safe to cast into gulong
                manager.poweron_delay = (gulong) toml_value.u.i;
            } else {
                // Changed from initialized default value but not in range
                g_message("Configured poweron_delay out of range, using default");
            }
        }
    }

    at_init(&manager, toml_table_in(toml_config, "at"));
    gpio_init(&manager, toml_table_in(toml_config, "gpio"));
#ifdef HAVE_MMGLIB
    mm_iface_init(&manager, toml_table_in(toml_config, "mm-iface"));
#endif
    ofono_iface_init(&manager);
    suspend_init(&manager, toml_table_in(toml_config, "suspend"));
    udev_init(&manager, toml_table_in(toml_config, "udev"));

    g_idle_add(G_SOURCE_FUNC(modem_start), &manager);

    g_unix_signal_add(SIGINT, G_SOURCE_FUNC(quit_app), &manager);
    g_unix_signal_add(SIGTERM, G_SOURCE_FUNC(quit_app), &manager);

    g_main_loop_run(manager.loop);

    gpio_destroy(&manager);

    return 0;
}
