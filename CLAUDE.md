# postgresql_modified_for_babelfish — openHalo × Babelfish fusion tree

Babelfish's PG18.3 kernel fork (SQL Server/T-SQL compat) used as the base, with
openHalo's MySQL-compat additions ported in on top. Active branch:
`openhalo-fusion`. Both MySQL and T-SQL dialects run end-to-end on this single
kernel. Full history/rationale: `~/.claude/plans/reflective-mapping-blum.md`.

## Build (meson)

```bash
meson setup build --prefix=$(pwd)/inst -Duuid=ossp
cd build && ninja -j16 && ninja install
```

**`-Duuid=ossp` is required**, not optional: `babelfishpg_tds` CASCADE-requires
the `uuid-ossp` extension, which isn't built without it. This flag lives in
the `build/` directory's meson config, **not in source** — if you
`rm -rf build`, you must pass it again on the next `meson setup`.

After any kernel code change: `ninja` alone only updates `build/`. You must
also run `ninja install` — the binaries actually used by a running server
live in `inst/`, not `build/`. Forgetting this is a recurring trap: the
server will silently keep running stale code.

## Test suites

```bash
meson test --suite setup --suite postmaster --suite aux_mysql --suite regress
```

`--suite setup` is not optional — it builds `tmp_install`/`initdb-template`,
scaffolding that every other suite depends on but that plain `ninja` doesn't
build. Omitting it fails every test with `cp: cannot copy .../initdb-template`.

- `postmaster/004_mysql_protocol` — MySQL wire protocol TAP suite
- `aux_mysql/005_mysql_compat` — MySQL SQL-compatibility comparison suite
  (`contrib/aux_mysql/t/mysql_compat/`, driven by `run_mysql_compat.sh`)
- `regress/regress` — Babelfish's own core PG regression suite (standard PG
  wire protocol only, doesn't exercise MySQL or T-SQL code paths)

## Building the Babelfish extensions (`../babelfish_extensions`)

The kernel is meson-built; these extensions use classic autotools-era PGXS
Makefiles. Meson's `Makefile.global` is a much narrower reimplementation of
the autoconf one — several values `./configure` used to fold into global
flags (CXX, ICU_LIBS, libxml2 include path) are tracked per-target inside
meson and never surface through `pg_config`. Build order and required env:

```bash
export PG_CONFIG=$(pwd)/inst/bin/pg_config
export PG_SRC=$(pwd)                      # the fusion tree root
export PATH=$(pwd)/inst/bin:$PATH
export cmake=$(which cmake)               # only needed for babelfishpg_tsql

cd ../babelfish_extensions/contrib/babelfishpg_money  && make && make install
cd ../babelfishpg_common                              && make && make install
cd ../babelfishpg_tds                                 && make && make install
cd ../babelfishpg_tsql                                && make && make install
```

One-time host dependencies (not part of this repo, install once):
- `cmake` — needed to generate ANTLR C++ sources for `babelfishpg_tsql`
- **ANTLR4 C++ runtime, exactly 4.13.2**, built from source and installed to
  `/usr/local`. `apt`'s `libantlr4-runtime4.10` will NOT work — ANTLR
  generated code is version-sensitive against its runtime, and the generator
  jar vendored at `babelfishpg_tsql/antlr/thirdparty/antlr/antlr-4.13.2-complete.jar`
  is 4.13.2. Build: clone `antlr/antlr4` at tag `4.13.2`,
  `cd runtime/Cpp && mkdir build && cd build && cmake .. -DANTLR4_INSTALL=ON -DCMAKE_BUILD_TYPE=Release && make -j$(nproc) && sudo make install && sudo ldconfig`
- `freetds-bin` (`apt`) — provides the `tsql` CLI, the only easy way to test
  a real TDS connection without a Windows/ODBC client

## Starting a test server (T-SQL / TDS)

```
shared_preload_libraries = 'babelfishpg_tds'   # NOT babelfishpg_tsql — tds pulls it in
babelfishpg_tds.port = 51433                   # separate from the PG port
```
Bootstrap sequence (mirrors `babelfish_extensions/test/JDBC/init.sh`):
```sql
CREATE USER jdbc_user WITH SUPERUSER CREATEDB CREATEROLE PASSWORD '...' INHERIT;
CREATE DATABASE babelfish_db OWNER jdbc_user;
\c babelfish_db
CREATE EXTENSION IF NOT EXISTS "babelfishpg_tds" CASCADE;
GRANT ALL ON SCHEMA sys to jdbc_user;
ALTER SYSTEM SET babelfishpg_tsql.database_name = 'babelfish_db';
ALTER SYSTEM SET babelfishpg_tsql.migration_mode = 'multi-db';
SELECT pg_reload_conf();
\c babelfish_db
CALL sys.initialize_babelfish('jdbc_user');
```
Then connect: `tsql -H 127.0.0.1 -p 51433 -U jdbc_user -P <password>`.

## Starting a test server (MySQL)

```bash
initdb -D <datadir> -m mysql
```
sets up `mysql_listener_on`/`mysql_port` and creates the `aux_mysql`
extension automatically. Connect with a real `mysql` CLI (not just `psql`) —
MySQL-mode bugs frequently only surface over the actual MySQL wire protocol,
not PG's.

## Key gotcha: dispatch-missing bugs are silent, not crashes

The recurring bug class found while porting openHalo's MySQL support into
this tree: code paths that push data to the client (GUC ParameterStatus,
error messages, DDL dispatch, column naming, expression rewriting) via
Babelfish's `protocol_config`-based senders unconditionally. For MySQL
connections this resolves to raw PostgreSQL wire framing and corrupts the
byte stream — with no crash and often no log line. If a MySQL-mode feature
silently doesn't work (or hangs) after kernel changes, suspect a missing
`GetCurrentProtocolRoutine()` check before falling back to `protocol_config`,
not a logic bug in the feature code itself.
