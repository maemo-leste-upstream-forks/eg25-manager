#
# Copyright (C) 2019 Purism SPC
# Copyright (C) 2020 Arnaud Ferraris <arnaud.ferraris@gmail.com>
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

project (
  'eg25-manager',
  'c',
  version : '0.2.0',
  license : 'GPLv3+',
  meson_version : '>= 0.50.0',
  default_options :
    [
      'warning_level=1',
      'buildtype=debugoptimized',
      'c_std=gnu11'
    ],
)

cc = meson.get_compiler('c')

app_name = meson.project_name()

prefix = get_option('prefix')
datadir = get_option('datadir')
sysconfdir = get_option('sysconfdir')
bindir = join_paths(prefix, get_option('bindir'))
udevrulesdir = join_paths(prefix, 'lib/udev/rules.d')

if datadir.startswith('/')
  full_datadir = datadir
else
  full_datadir = join_paths(prefix, datadir)
endif

if sysconfdir.startswith('/')
  full_sysconfdir = sysconfdir
else
  full_sysconfdir = join_paths(prefix, sysconfdir)
endif

eg25_confdir = join_paths(full_sysconfdir, meson.project_name())
eg25_datadir = join_paths(full_datadir, meson.project_name())

add_global_arguments('-D@0@="@1@"'.format('EG25_CONFDIR', eg25_confdir), language : 'c')
add_global_arguments('-D@0@="@1@"'.format('EG25_DATADIR', eg25_datadir), language : 'c')

mmglib_dep = dependency('mm-glib', required : false)
if mmglib_dep.found()
  add_global_arguments('-DHAVE_MMGLIB=1', language : 'c')
endif

mgr_deps = [
    dependency('glib-2.0'),
    dependency('gio-unix-2.0'),
    dependency('gudev-1.0'),
    dependency('libgpiod'),
    dependency('libusb-1.0'),
    mmglib_dep,
]

subdir('data')
subdir('src')
subdir('udev')
