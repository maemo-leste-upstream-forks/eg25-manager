/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gpio.h"

#include <unistd.h>

#define GPIO_CHIP1_LABEL "1c20800.pinctrl"
#define GPIO_CHIP2_LABEL "1f02c00.pinctrl"

#define MAX_GPIOCHIP_LINES 352

#define GPIO_IDX_INVAL 0xffff

enum {
    GPIO_OUT_DTR = 0,
    GPIO_OUT_PWRKEY,
    GPIO_OUT_RESET,
    GPIO_OUT_APREADY,
    GPIO_OUT_DISABLE,
    GPIO_OUT_COUNT
};


enum {
    GPIO_IN_STATUS = 0,
    GPIO_IN_COUNT
};

int gpio_sequence_poweron(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_PWRKEY], 1);
    sleep(1);
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_PWRKEY], 0);

    g_message("Executed power-on/off sequence");

    return 0;
}

int gpio_sequence_shutdown(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_DISABLE], 1);
    gpio_sequence_poweron(manager);

    g_message("Executed power-off sequence");

    return 0;
}

int gpio_sequence_suspend(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_APREADY], 1);

    g_message("Executed suspend sequence");

    return 0;
}

int gpio_sequence_resume(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_APREADY], 0);

    g_message("Executed resume sequence");

    return 0;
}

int gpio_sequence_wake(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_DTR], 0);

    /* Give the modem 5ms to wake from soft sleep */
    usleep(5000);

    g_message("Executed soft wake sequence");

    return 0;
}

int gpio_sequence_sleep(struct EG25Manager *manager)
{
    gpiod_line_set_value(manager->gpio_out[GPIO_OUT_DTR], 1);
    g_message("Executed soft sleep sequence");

    return 0;
}

static guint get_config_gpio(toml_table_t *config, const char *id)
{
    toml_datum_t value = toml_int_in(config, id);
    guint gpio;

    if (!value.ok)
        return GPIO_IDX_INVAL;

    gpio = (guint)value.u.i;

    return gpio;
}

int gpio_init(struct EG25Manager *manager, toml_table_t *config)
{
    int i, ret;
    guint gpio_out_idx[GPIO_OUT_COUNT];
    guint gpio_in_idx[GPIO_IN_COUNT];

    manager->gpiochip[0] = gpiod_chip_open_by_label(GPIO_CHIP1_LABEL);
    if (!manager->gpiochip[0]) {
        g_critical("Unable to open GPIO chip " GPIO_CHIP1_LABEL);
        return 1;
    }

    manager->gpiochip[1] = gpiod_chip_open_by_label(GPIO_CHIP2_LABEL);
    if (!manager->gpiochip[1]) {
        g_critical("Unable to open GPIO chip " GPIO_CHIP2_LABEL);
        return 1;
    }

    gpio_out_idx[GPIO_OUT_DTR] = get_config_gpio(config, "dtr");
    gpio_out_idx[GPIO_OUT_PWRKEY] = get_config_gpio(config, "pwrkey");
    gpio_out_idx[GPIO_OUT_RESET] = get_config_gpio(config, "reset");
    gpio_out_idx[GPIO_OUT_APREADY] = get_config_gpio(config, "apready");
    gpio_out_idx[GPIO_OUT_DISABLE] = get_config_gpio(config, "disable");
    gpio_in_idx[GPIO_IN_STATUS] = get_config_gpio(config, "status");

    for (i = 0; i < GPIO_OUT_COUNT; i++) {
        guint offset, chipidx;

        if (gpio_out_idx[i] < MAX_GPIOCHIP_LINES) {
            offset = gpio_out_idx[i];
            chipidx = 0;
        } else {
            offset = gpio_out_idx[i] - MAX_GPIOCHIP_LINES;
            chipidx = 1;
        }

        manager->gpio_out[i] = gpiod_chip_get_line(manager->gpiochip[chipidx], offset);
        if (!manager->gpio_out[i]) {
            g_error("Unable to get output GPIO %d", i);
            return 1;
        }

        ret = gpiod_line_request_output(manager->gpio_out[i], "eg25manager", 0);
        if (ret < 0) {
            g_error("Unable to request output GPIO %d", i);
            return 1;
        }
    }

    for (i = 0; i < GPIO_IN_COUNT; i++) {
        guint offset, chipidx;

        if (gpio_in_idx[i] == GPIO_IDX_INVAL)
            continue;

        if (gpio_in_idx[i] < MAX_GPIOCHIP_LINES) {
            offset = gpio_in_idx[i];
            chipidx = 0;
        } else {
            offset = gpio_in_idx[i] - MAX_GPIOCHIP_LINES;
            chipidx = 1;
        }

        manager->gpio_in[i] = gpiod_chip_get_line(manager->gpiochip[chipidx], offset);
        if (!manager->gpio_in[i]) {
            g_warning("Unable to get input GPIO %d", i);
            continue;
        }

        ret = gpiod_line_request_input(manager->gpio_in[i], "eg25manager");
        if (ret < 0) {
            g_warning("Unable to request input GPIO %d", i);
            manager->gpio_in[i] = NULL;
        }
    }

    return 0;
}

gboolean gpio_check_poweroff(struct EG25Manager *manager, gboolean keep_down)
{
    if (manager->gpio_in[GPIO_IN_STATUS] &&
        gpiod_line_get_value(manager->gpio_in[GPIO_IN_STATUS]) == 1) {

        if (keep_down && manager->gpio_out[GPIO_OUT_RESET]) {
            // Asserting RESET line to prevent modem from rebooting
            gpiod_line_set_value(manager->gpio_out[GPIO_OUT_RESET], 1);
        }

        return TRUE;
    }

    return FALSE;
}

void gpio_destroy(struct EG25Manager *manager)
{
    int i;

    for (i = 0; i < GPIO_OUT_COUNT; i++) {
        if (manager->gpio_out[i])
            gpiod_line_release(manager->gpio_out[i]);
    }

    for (i = 0; i < GPIO_IN_COUNT; i++) {
        if (manager->gpio_in[i])
            gpiod_line_release(manager->gpio_in[i]);
    }

    if (manager->gpiochip[0])
        gpiod_chip_close(manager->gpiochip[0]);
    if (manager->gpiochip[1])
        gpiod_chip_close(manager->gpiochip[1]);
}
