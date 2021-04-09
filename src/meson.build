#
# Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#


subdir('libgdbofono')

src = [
        'at.c', 'at.h',
        'gpio.c', 'gpio.h',
        'manager.c', 'manager.h',
        'ofono-iface.c', 'ofono-iface.h',
        'suspend.c', 'suspend.h',
        'toml.c', 'toml.h',
        'udev.c', 'udev.h',
]

if mmglib_dep.found()
  src += ['mm-iface.c', 'mm-iface.h']
endif

executable (
    'eg25manager',
    src,
    dependencies : mgr_deps,
    link_with: gdbofono_lib,
    install : true
)
