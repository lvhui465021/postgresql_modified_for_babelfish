# Copyright (c) 2022-2025, PostgreSQL Global Development Group

tests += {
  'name': 'postmaster',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_connection_limits.pl',
      't/003_start_stop.pl',
    ],
    # 004_mysql_protocol and 005_mysql_listener_stability need the MySQL
    # compatibility modules (mysql_parser/mysm/aux_mysql), which moved out of
    # the meson build; run them via prove against the installed extensions
    # (see run-baseline.sh).
  },
}
