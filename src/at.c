/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "at.h"
#include "suspend.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <glib-unix.h>

struct AtCommand {
    char *cmd;
    char *subcmd;
    char *value;
    char *expected;
    int retries;
};

static GArray *configure_commands = NULL;
static GArray *suspend_commands = NULL;
static GArray *resume_commands = NULL;
static GArray *reset_commands = NULL;

static int configure_serial(const char *tty)
{
    struct termios ttycfg;
    int fd;

    fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd > 0) {
        tcgetattr(fd, &ttycfg);
        ttycfg.c_iflag &= ~(IGNBRK | BRKINT | ICRNL | INLCR | PARMRK | INPCK | ISTRIP | IXON);
        ttycfg.c_oflag = 0;
        ttycfg.c_lflag &= ~(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
        ttycfg.c_cflag &= ~(CSIZE | PARENB);
        ttycfg.c_cflag |= CS8;
        ttycfg.c_cc[VMIN] = 1;
        ttycfg.c_cc[VTIME] = 0;

        cfsetspeed(&ttycfg, B115200);
        tcsetattr(fd, TCSANOW, &ttycfg);
    }

    return fd;
}

static gboolean send_at_command(struct EG25Manager *manager)
{
    char command[256];
    struct AtCommand *at_cmd = manager->at_cmds ? g_list_nth_data(manager->at_cmds, 0) : NULL;
    int ret, len = 0;

    if (at_cmd) {
        if (at_cmd->subcmd == NULL && at_cmd->value == NULL && at_cmd->expected == NULL)
            len = sprintf(command, "AT+%s\r\n", at_cmd->cmd);
        else if (at_cmd->subcmd == NULL && at_cmd->value == NULL)
            len = sprintf(command, "AT+%s?\r\n", at_cmd->cmd);
        else if (at_cmd->subcmd == NULL && at_cmd->value)
            len = sprintf(command, "AT+%s=%s\r\n", at_cmd->cmd, at_cmd->value);
        else if (at_cmd->subcmd && at_cmd->value == NULL)
            len = sprintf(command, "AT+%s=\"%s\"\r\n", at_cmd->cmd, at_cmd->subcmd);
        else if (at_cmd->subcmd && at_cmd->value)
            len = sprintf(command, "AT+%s=\"%s\",%s\r\n", at_cmd->cmd, at_cmd->subcmd, at_cmd->value);

        ret = write(manager->at_fd, command, len);
        if (ret < len)
            g_warning("Couldn't write full AT command: wrote %d/%d bytes", ret, len);

        g_message("Sending command: %s", g_strstrip(command));
    } else if (manager->modem_state < EG25_STATE_CONFIGURED) {
        if (manager->modem_iface == MODEM_IFACE_MODEMMANAGER) {
#ifdef HAVE_MMGLIB
            MMModemState modem_state = mm_modem_get_state(manager->mm_modem);

            if (manager->mm_modem && modem_state >= MM_MODEM_STATE_REGISTERED)
                modem_update_state(manager, modem_state);
            else
                manager->modem_state = EG25_STATE_CONFIGURED;
#endif
        } else {
            manager->modem_state = EG25_STATE_CONFIGURED;
        }
    } else if (manager->modem_state == EG25_STATE_SUSPENDING) {
        modem_suspend_post(manager);
    } else if (manager->modem_state == EG25_STATE_RESETTING) {
        manager->modem_state = EG25_STATE_POWERED;
    }

    return FALSE;
}

static void next_at_command(struct EG25Manager *manager)
{
    struct AtCommand *at_cmd = manager->at_cmds ? g_list_nth_data(manager->at_cmds, 0) : NULL;

    if (!at_cmd)
        return;

    if (at_cmd->cmd)
        g_free(at_cmd->cmd);
    if (at_cmd->subcmd)
        g_free(at_cmd->subcmd);
    if (at_cmd->value)
        g_free(at_cmd->value);
    if (at_cmd->expected)
        g_free(at_cmd->expected);
    g_free(at_cmd);
    manager->at_cmds = g_list_remove(manager->at_cmds, at_cmd);

    send_at_command(manager);
}

static void retry_at_command(struct EG25Manager *manager)
{
    struct AtCommand *at_cmd = manager->at_cmds ? g_list_nth_data(manager->at_cmds, 0) : NULL;

    if (!at_cmd)
        return;

    at_cmd->retries++;
    if (at_cmd->retries > 3) {
        g_critical("Command %s retried %d times, aborting...", at_cmd->cmd, at_cmd->retries);
        next_at_command(manager);
    } else {
        g_timeout_add(500, G_SOURCE_FUNC(send_at_command), manager);
    }
}

static void process_at_result(struct EG25Manager *manager, char *response)
{
    struct AtCommand *at_cmd = manager->at_cmds ? g_list_nth_data(manager->at_cmds, 0) : NULL;

    if (!at_cmd)
        return;

    if (at_cmd->expected && !strstr(response, at_cmd->expected)) {
        if (at_cmd->value)
            g_free(at_cmd->value);
        at_cmd->value = at_cmd->expected;
        at_cmd->expected = NULL;
        g_message("Got a different result than expected, changing value...");
        g_message("\t%s\n\t%s", at_cmd->expected, response);
        send_at_command(manager);
    } else {
        next_at_command(manager);
    }
}

static int append_at_command(struct EG25Manager *manager,
                             const char         *cmd,
                             const char         *subcmd,
                             const char         *value,
                             const char         *expected)
{
    struct AtCommand *at_cmd = calloc(1, sizeof(struct AtCommand));

    if (!at_cmd)
        return -1;

    at_cmd->cmd = g_strdup(cmd);
    if (subcmd)
        at_cmd->subcmd = g_strdup(subcmd);
    if (value)
        at_cmd->value = g_strdup(value);
    if (expected)
        at_cmd->expected = g_strdup(expected);

    manager->at_cmds = g_list_append(manager->at_cmds, at_cmd);

    return 0;
}

#define READ_BUFFER_SIZE 256

static gboolean modem_response(gint fd,
                               GIOCondition event,
                               gpointer data)
{
    struct EG25Manager *manager = data;
    char response[READ_BUFFER_SIZE*4+1];
    char tmp[READ_BUFFER_SIZE];
    ssize_t ret, pos = 0;

    /*
     * Several reads can be necessary to get the full response, so we loop
     * until we read 0 chars with a reasonable delay between attempts
     * (remember the transfer rate is 115200 here)
     */
    do {
        ret = read(fd, tmp, sizeof(tmp));
        if (ret > 0) {
            memcpy(&response[pos], tmp, ret);
            pos += ret;
            usleep(10000);
        }
    } while (ret > 0 && pos < (sizeof(response) - 1));

    if (pos > 0) {
        response[pos] = 0;
        g_strstrip(response);
        if (strlen(response) == 0)
            return TRUE;

        g_message("Response: [%s]", response);

        if (strcmp(response, "RDY") == 0) {
            suspend_inhibit(manager, TRUE, TRUE);
            manager->modem_state = EG25_STATE_STARTED;
        }
        else if (strstr(response, "ERROR"))
            retry_at_command(manager);
        else if (strstr(response, "OK"))
            process_at_result(manager, response);
        else
            // Not a recognized response, try running next command, just in case
            next_at_command(manager);
    }

    return TRUE;
}

static void parse_commands_list(toml_array_t *array, GArray **cmds)
{
    int len;

    len = toml_array_nelem(array);
    *cmds = g_array_new(FALSE, TRUE, sizeof(struct AtCommand));
    g_array_set_size(*cmds, (guint)len);
    for (int i = 0; i < len; i++) {
        struct AtCommand *cmd = &g_array_index(*cmds, struct AtCommand, i);
        toml_table_t *table = toml_table_at(array, i);
        toml_datum_t value;

        if (!table)
            continue;

        value = toml_string_in(table, "cmd");
        if (value.ok) {
            cmd->cmd = g_strdup(value.u.s);
            free(value.u.s);
        }

        value = toml_string_in(table, "subcmd");
        if (value.ok) {
            cmd->subcmd = g_strdup(value.u.s);
            free(value.u.s);
        }

        value = toml_string_in(table, "value");
        if (value.ok) {
            cmd->value = g_strdup(value.u.s);
            free(value.u.s);
        }

        value = toml_string_in(table, "expect");
        if (value.ok) {
            cmd->expected = g_strdup(value.u.s);
            free(value.u.s);
        }
    }
}

int at_init(struct EG25Manager *manager, toml_table_t *config)
{
    toml_array_t *commands;
    toml_datum_t uart_port;

    uart_port = toml_string_in(config, "uart");
    if (!uart_port.ok)
        g_error("Configuration file lacks UART port definition");

    manager->at_fd = configure_serial(uart_port.u.s);
    if (manager->at_fd < 0) {
        g_critical("Unable to configure %s", uart_port.u.s);
        free(uart_port.u.s);
        return 1;
    }
    free(uart_port.u.s);

    manager->at_source = g_unix_fd_add(manager->at_fd, G_IO_IN, modem_response, manager);

    commands = toml_array_in(config, "configure");
    if (!commands)
        g_error("Configuration file lacks initial AT commands list");
    parse_commands_list(commands, &configure_commands);

    commands = toml_array_in(config, "suspend");
    if (!commands)
        g_error("Configuration file lacks suspend AT commands list");
    parse_commands_list(commands, &suspend_commands);

    commands = toml_array_in(config, "resume");
    if (!commands)
        g_error("Configuration file lacks resume AT commands list");
    parse_commands_list(commands, &resume_commands);

    commands = toml_array_in(config, "reset");
    if (!commands)
        g_error("Configuration file lacks reset AT commands list");
    parse_commands_list(commands, &reset_commands);

    return 0;
}

void at_destroy(struct EG25Manager *manager)
{
    g_source_remove(manager->at_source);
    if (manager->at_fd > 0)
        close(manager->at_fd);

    g_array_free(configure_commands, TRUE);
    g_array_free(suspend_commands, TRUE);
    g_array_free(resume_commands, TRUE);
    g_array_free(reset_commands, TRUE);
}

void at_sequence_configure(struct EG25Manager *manager)
{
    for (guint i = 0; i < configure_commands->len; i++) {
        struct AtCommand *cmd = &g_array_index(configure_commands, struct AtCommand, i);
        append_at_command(manager, cmd->cmd, cmd->subcmd, cmd->value, cmd->expected);
    }
    send_at_command(manager);
}

void at_sequence_suspend(struct EG25Manager *manager)
{
    for (guint i = 0; i < suspend_commands->len; i++) {
        struct AtCommand *cmd = &g_array_index(suspend_commands, struct AtCommand, i);
        append_at_command(manager, cmd->cmd, cmd->subcmd, cmd->value, cmd->expected);
    }
    send_at_command(manager);
}

void at_sequence_resume(struct EG25Manager *manager)
{
    for (guint i = 0; i < resume_commands->len; i++) {
        struct AtCommand *cmd = &g_array_index(resume_commands, struct AtCommand, i);
        append_at_command(manager, cmd->cmd, cmd->subcmd, cmd->value, cmd->expected);
    }
    send_at_command(manager);
}

void at_sequence_reset(struct EG25Manager *manager)
{
    for (guint i = 0; i < reset_commands->len; i++) {
        struct AtCommand *cmd = &g_array_index(reset_commands, struct AtCommand, i);
        append_at_command(manager, cmd->cmd, cmd->subcmd, cmd->value, cmd->expected);
    }
    send_at_command(manager);
}
