/*
 * Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "manager.h"

int gpio_init(struct EG25Manager *state);
void gpio_destroy(struct EG25Manager *state);

int gpio_sequence_poweron(struct EG25Manager *state);
int gpio_sequence_shutdown(struct EG25Manager *state);
int gpio_sequence_suspend(struct EG25Manager *state);
int gpio_sequence_resume(struct EG25Manager *state);
