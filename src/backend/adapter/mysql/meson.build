# Copyright (c) 2025-2026, HaloLab / UDB-TX Contributors

# systemVar.c stays in the kernel: the shared session/global system-variable
# state (mys_sqlMode, time zone, autocommit, shmem allocation) is referenced
# by kernel code (ipci.c, commands/mysql) as well as by the loadable
# modules (aux_mysql, mysm, mysql_parser).
backend_sources += files(
  'systemVar.c'
)
