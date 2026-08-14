/*-------------------------------------------------------------------------
 *
 * mys_keywords.c
 *    MySQL keyword-list catalog.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * 
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "common/keywords.h"


/* ScanKeywordList lookup data for SQL keywords */

#include "mys_kwlist_d.h"

/* Keyword categories for SQL keywords */

#define PG_KEYWORD(kwname, value, category, collabel) category,

const uint8 MysScanKeywordCategories[MYSSCANKEYWORDS_NUM_KEYWORDS] = {
#include "parser/mysql/mys_kwlist.h"
};

#undef PG_KEYWORD
