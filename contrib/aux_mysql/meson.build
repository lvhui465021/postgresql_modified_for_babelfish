# Copyright (c) 2025, openHalo

# MySQL wire protocol (moved out of the kernel) as a loadable module.
# Preloaded via shared_preload_libraries so the ProtocolRoutine and the
# listener are registered before any backend starts.
aux_mysql_sources = files(
  'src/aux_mysql_init.c',
  'src/mysql_auth.c',
  'src/mysql_command.c',
  'src/mysql_packet.c',
  'src/mysql_protocol.c',
  'src/mysql_stmt.c',
  'src/mys_utility.c',
)

aux_mysql = shared_module('aux_mysql',
  aux_mysql_sources,
  include_directories: postgres_inc,
  c_args: ['-fvisibility=default'],
  install: true,
  install_dir: dir_lib_pkg,
  name_prefix: '',
)

install_data(
  'aux_mysql.control',
  kwargs: contrib_data_args,
)

install_data(
  'aux_mysql--1.1.sql',
  'aux_mysql--1.1--1.2.sql',
  'aux_mysql--1.2--1.3.sql',
  'aux_mysql--1.3--1.4.sql',
  'aux_mysql--1.4--1.5.sql',
  'aux_mysql--1.5--1.6.sql',
  install_dir: dir_data_extension,
)

tests += {
  'name': 'aux_mysql',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'versioning',
    ],
    'regress_args': ['--temp-config', files('aux_mysql_regress.conf')],
  },
}
