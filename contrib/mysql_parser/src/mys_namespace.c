/*-------------------------------------------------------------------------
 *
 * mys_namespace.c
 *    Namespace helpers used by the MySQL parser.
 *
 * The MySQL USE statement is lowered to a PostgreSQL search_path change.
 * Keeping namespace resolution here makes SHOW/metadata grammar actions use
 * the same current schema as normal PostgreSQL name resolution.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/parser/mysql/mys_namespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "parser/mysql/mys_parse_utilcmd.h"

/*
 * Return the first explicit namespace in the active creation search path.
 *
 * fetch_search_path(false) intentionally omits implicit namespaces such as
 * pg_catalog.  That is the namespace a MySQL unqualified table operation is
 * expected to address after USE has set search_path.  A dropped or otherwise
 * unusable explicit path can leave the list empty; retain a useful default in
 * that case and report a normal PostgreSQL error if even public is gone.
 */
Oid
getCurrentNamespaceOid(void)
{
	List *search_path = fetch_search_path(false);
	ListCell *lc;

	foreach(lc, search_path)
	{
		Oid namespaceOid = lfirst_oid(lc);

		if (OidIsValid(namespaceOid))
			return namespaceOid;
	}

	{
		Oid publicOid = get_namespace_oid("public", true);

		if (OidIsValid(publicOid))
			return publicOid;
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_SCHEMA),
			 errmsg("no usable schema in the current search path")));

	return InvalidOid; /* keep the compiler quiet */
}
