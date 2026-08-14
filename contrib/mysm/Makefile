# contrib/mysm/Makefile

MODULE_big = mysm
OBJS = \
	src/aggregates.o \
	src/bpchar_pg18.o \
	src/common_funcs.o \
	src/date_func.o \
	src/mys_adtext.o \
	src/mys_date.o \
	src/mys_json.o \
	src/mys_timestamp.o \
	src/mysm_globals.o \
	src/mysm_stubs.o \
	src/operators.o \
	src/partListColumns.o \
	src/strfuncs.o \
	src/systemVar.o \
	src/texteq_mys.o \
	src/time_func.o \
	src/timestamp_func.o \
	src/user.o \
	src/user_var.o \
	src/uuid_short.o \
	src/uuid_stubs.o

# The kernel meson build used -fvisibility=default for this module; mirror it.
PG_CFLAGS += -fvisibility=default

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/mysm
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif