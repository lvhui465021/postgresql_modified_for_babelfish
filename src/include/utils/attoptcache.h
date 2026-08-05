/*-------------------------------------------------------------------------
 *
 * attoptcache.h
 *	  Attribute options cache.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/attoptcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ATTOPTCACHE_H
#define ATTOPTCACHE_H

/*
 * Attribute options.
 */
typedef struct AttributeOpts
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	float8		n_distinct;
	float8		n_distinct_inherited;
	int			mysql_default_kind;
} AttributeOpts;

#define MYSQL_DEFAULT_KIND_NONE		0
#define MYSQL_DEFAULT_KIND_LITERAL	1
#define MYSQL_DEFAULT_KIND_EXPRESSION 2

extern AttributeOpts *get_attribute_options(Oid attrelid, int attnum);

#endif							/* ATTOPTCACHE_H */
