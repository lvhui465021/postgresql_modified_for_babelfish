# contrib/mysql_parser/Makefile

MODULE_big = mysql_parser
OBJS = \
	src/mys_expr_transform.o \
	src/mys_gram.o \
	src/mys_gram_globals.o \
	src/mys_keywords.o \
	src/mys_namespace.o \
	src/mys_parse_utilcmd.o \
	src/mys_parser.o \
	src/mys_scan.o

# The kernel meson build used -fvisibility=default for this module; mirror it.
PG_CFLAGS += -fvisibility=default

# Generated headers live next to the sources: the installed
# parser/mysql/mys_gramparse.h includes the bare "mys_gram.h" and
# mys_keywords.c includes the bare "mys_kwlist_d.h".
PG_CPPFLAGS += -I$(srcdir)/src

# gen_keywordlist.pl is not shipped by pgxs; it lives in the kernel source
# tree.  build-all.sh passes PG_SRC explicitly.
PG_SRC ?= $(shell $(PG_CONFIG) --includedir-server 2>/dev/null | sed 's|/include/server||')

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/mysql_parser
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# bison: mys_gram.y -> mys_gram.c + mys_gram.h
src/mys_gram.c src/mys_gram.h &: src/mys_gram.y
	$(BISON) -o src/mys_gram.c -d src/mys_gram.y

# flex: mys_scan.l -> mys_scan.c (needs the grammar header at compile time)
src/mys_scan.c: src/mys_scan.l src/mys_gram.h
	$(FLEX) -CF -p -p -o src/mys_scan.c src/mys_scan.l

# keyword hash: mys_kwlist.h (kernel-side parser contract) -> mys_kwlist_d.h
src/mys_kwlist_d.h: $(PG_SRC)/src/include/parser/mysql/mys_kwlist.h
	$(PERL) $(PG_SRC)/src/tools/gen_keywordlist.pl --extern --output src --varname=MysScanKeywords $<

src/mys_keywords.o: src/mys_kwlist_d.h

clean: clean-mysgen
clean-mysgen:
	rm -f src/mys_gram.c src/mys_gram.h src/mys_scan.c src/mys_kwlist_d.h