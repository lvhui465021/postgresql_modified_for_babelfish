/*-------------------------------------------------------------------------
 *
 * aux_mysql_init.c
 *    Module load hook for the aux_mysql MySQL-compatibility module.
 *
 * The MySQL wire protocol (listener, ProtocolRoutine, protocol GUCs)
 * moved out of the kernel into this loadable module.  _PG_init()
 * registers what the postmaster used to do inline:
 *   - protocol GUCs (was src/backend/utils/misc/guc_tables.c)
 *   - the MySQL ProtocolRoutine and its parser binding
 *   - the CTAS post-hook
 *   - the MySQL listener, opened via ListenProtocolServerPort() from
 *     postmaster startup (listen_init_hook)
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/contrib/aux_mysql/src/aux_mysql_init.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "adapter/mysql/mysql_protocol.h"
#include "adapter/mysql/mys_session_state_exports.h"
/* relcache.h must precede commands/mysql headers (Relation type). */
#include "utils/relcache.h"
#include "commands/mysql/mys_tablecmds.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "parser/parsereng.h"
#include "postmaster/protocol_routine.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/varlena.h"

PG_MODULE_MAGIC;

/*
 * Protocol GUCs (mysql_listener_on, mysql_port, mysql_max_allowed_packet,
 * mysql_backend_database) are registered in the kernel (guc_tables.c):
 * dotless custom GUCs cannot be set in postgresql.conf before the module
 * is preloaded, which would make a MySQL-mode cluster fail to start.
 * The variables are kernel globals (utils/guc.h); only their consumers
 * live in this module.
 */

/*
 * listen_init_hook is a single global function pointer, not a registry: if
 * another loadable module (e.g. a TDS or other compatibility listener)
 * already claimed it before us, overwriting it without calling through
 * would silently disable that module's listener. Save the previous value
 * and chain to it so listener registration composes regardless of module
 * load order (mirrors babelfishpg_tds's identical fix on the Babelfish
 * side, tds_srv.c's pe_listen_init()).
 */
static listen_init_hook_type prev_listen_init;

/*
 * mysql_listen_init
 *
 * Open the MySQL TCP listener from postmaster startup.  Called via
 * listen_init_hook; ListenProtocolServerPort() (kernel) registers the
 * socket with the postmaster's wait set.
 */
static void
mysql_listen_init(void)
{
	if (mysql_listener_on)
	{
		char	   *addresses;
		List	   *elemlist;
		ListCell   *l;
		bool		success = false;

		addresses = pstrdup(GetConfigOption("listen_addresses", false, false));
		if (!SplitIdentifierString(pstrdup(addresses), ',', &elemlist))
			ereport(FATAL,
					(errmsg("invalid list syntax for \"listen_addresses\"")));

		foreach(l, elemlist)
		{
			char	   *curhost = (char *) lfirst(l);

			if (ListenProtocolServerPort(COMPAT_PROTOCOL_MYSQL, AF_UNSPEC,
										 curhost, (unsigned short) mysql_port,
										 NULL) == STATUS_OK)
				success = true;
		}

		list_free_deep(elemlist);
		pfree(addresses);

		if (!success)
			ereport(LOG,
					(errmsg("could not create MySQL listener on any address")));
	}

	/* Chain to whatever listen_init_hook was already registered (see the
	 * comment on prev_listen_init above) regardless of whether MySQL's own
	 * listener is enabled -- a disabled MySQL listener must not disable
	 * another module's listener that registered before us. */
	if (prev_listen_init != NULL)
		prev_listen_init();
}

/*
 * Module load hook.  Runs in the postmaster (shared_preload_libraries)
 * and in each forked process; registration is idempotent.
 */
void
_PG_init(void)
{
	/*
	 * Protocol GUCs are registered by the kernel (guc_tables.c); see the
	 * header comment above for why dotless custom GUCs cannot move here.
	 */

	/*
	 * Bind the loadable MySQL parser and register the protocol routine.
	 * Raw-parse dispatch in PostgresMain() reads
	 * protocol_routine->parser_routine; the kernel cannot reference
	 * mysql_parser.so symbols at link time.
	 */
	MySQLProtocolRoutine.parser_routine =
		GetRegisteredParserRoutine(COMPAT_PROTOCOL_MYSQL);
	if (MySQLProtocolRoutine.parser_routine == NULL)
		ereport(ERROR,
				(errmsg("aux_mysql requires mysql_parser to be preloaded before aux_mysql"),
				errhint("set shared_preload_libraries to 'mysql_parser, mysm, aux_mysql'")));
	RegisterProtocolRoutine(&MySQLProtocolRoutine);

	/* Publish packet-state getters to the kernel SQL helpers. */
	mys_session_state_exports = &mys_session_state_exports_data;

	/* Register MySQL's CTAS post-hook so backends inherit it. */
	InitMysCtasHook();

	/* Open the MySQL listener from postmaster startup. */
	prev_listen_init = listen_init_hook;
	listen_init_hook = mysql_listen_init;
}
