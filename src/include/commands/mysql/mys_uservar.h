/*-------------------------------------------------------------------------
 *
 * mys_uservar.h
 *	  MySQL user variable routines
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/include/commands/mysql/mys_uservar.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_USERVAR_H
#define MYS_USERVAR_H

#include "nodes/nodes.h"
#include "postgres.h"

void clearUserVars(void);
bool varValueIsDigit(Datum varValue, Oid varValueType);
void mysSetUserVarInternal(char *userVarName, char *userVarValue, Oid varValueType, bool isDigit, bool isNull);
void mysSetUserVarForPl(char *userVarName, Datum userVarValue, Oid varValueType, bool isNull);
bytea *mysGetUserVarValueInternal(char *userVarName);
Oid mysGetUserVarTypeInternal(char *userVarName);

/* Recover the variable name from a degraded pg_catalog.mys_get_user_var call. */
extern char *mys_extract_user_var_name(Node *expr);

#endif                          /* MYS_USERVAR_H */
