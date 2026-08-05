/*-------------------------------------------------------------------------
 *
 * mys_parser.h
 *    Declarations for the MySQL-compatibility parser.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/mysql/mys_parser.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_PARSER_H
#define MYS_PARSER_H

#include "parser/parser.h"

extern List *mys_raw_parser(const char *str, RawParseMode mode);

#endif							/* MYS_PARSER_H */
