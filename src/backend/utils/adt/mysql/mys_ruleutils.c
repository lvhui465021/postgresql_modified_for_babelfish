/*-------------------------------------------------------------------------
 *
 * mys_ruleutils.c
 *    MySQL ADT compatibility: backtick-quoted identifier utilities.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/backend/utils/adt/mysql/mys_ruleutils.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/keywords.h"
#include "parser/mysql/mys_parser_exports.h"
#include "utils/builtins.h"
#include "utils/mysql/mys_ruleutils.h"


const char *
mys_quote_identifier(const char *ident)
{
    /*
	 * Can avoid backtick if ident starts with a lowercase letter or underscore
	 * and contains only lowercase letters, digits, and underscores, *and* is
	 * not any SQL keyword.  Otherwise, supply backtick.
	 */
	int			nbackticks = 0;
	bool		safe;
	const char *ptr;
	char	   *result;
	char	   *optr;

	/*
	 * would like to use <ctype.h> macros here, but they might yield unwanted
	 * locale-specific results...
	 */
	safe = ((ident[0] >= 'a' && ident[0] <= 'z') || ident[0] == '_');

	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if ((ch >= 'a' && ch <= 'z') ||
			(ch >= '0' && ch <= '9') ||
			(ch == '_'))
		{
			/* okay */
		}
		else
		{
			safe = false;
			if (ch == '`')
				nbackticks++;
		}
	}

	if (quote_all_identifiers)
		safe = false;

	if (safe)
	{
		/*
		 * Check for keyword.  We quote keywords except for unreserved ones.
		 * (In some cases we could avoid quoting a col_name or type_func_name
		 * keyword, but it seems much harder than it's worth to tell that.)
		 *
		 * Note: ScanKeywordLookup() does case-insensitive comparison, but
		 * that's fine, since we already know we have all-lower-case.
		 */
		int			kwnum = ScanKeywordLookup(ident,
											  mys_parser_exports->MysScanKeywords);

		if (kwnum >= 0 &&
			mys_parser_exports->MysScanKeywordCategories[kwnum] != UNRESERVED_KEYWORD)
			safe = false;
	}

	if (safe)
		return ident;			/* no change needed */

	result = (char *) palloc(strlen(ident) + nbackticks + 2 + 1);

	optr = result;
	*optr++ = '`';
	for (ptr = ident; *ptr; ptr++)
	{
		char		ch = *ptr;

		if (ch == '`')
			*optr++ = '`';
		*optr++ = ch;
	}
	*optr++ = '`';
	*optr = '\0';

	return result;
}
