/*-------------------------------------------------------------------------
 *
 * parserapi.h
 *    ParserRoutine interface for multi-dialect SQL parsing.
 *
 * A ParserRoutine provides dialect-specific callbacks for raw parsing,
 * semantic analysis, expression transformation, function resolution,
 * and utility-command handling.  It is selected per-session through the
 * protocol layer (ProtocolRoutine.parser_routine) and stored in
 * ParseState so that sub-queries inherit the same dialect.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/parserapi.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSERAPI_H
#define PARSERAPI_H

#include "nodes/pg_list.h"
#include "parser/parser.h"          /* RawParseMode, raw_parser declaration */

/* forward declarations */
struct OnConflictClause;
struct ParseState;

/* ----------------------------------------------------------------
 *    ParserRoutine
 *
 * Each SQL dialect provides one const instance.  Callbacks that are
 * NULL are treated as "use the standard PostgreSQL implementation".
 * ----------------------------------------------------------------
 */
typedef struct ParserRoutine
{
    /* ------------------------------------------------------------
     * Raw parsing
     * ------------------------------------------------------------
     */

    /*
     * raw_parse  –  convert a SQL string into a List of RawStmt nodes.
     * Equivalent to raw_parser() but may use a dialect-specific scanner
     * and grammar.
     */
    List       *(*raw_parse)(const char *str, RawParseMode mode);

    /* ------------------------------------------------------------
	 * Statement-level compatibility hooks.
     * ------------------------------------------------------------
     */
    Query      *(*transformOptionalSelectInto)(struct ParseState *pstate,
                                               Node *parseTree);
    void        (*transformOnConflictArbiter)(struct ParseState *pstate,
                                              struct OnConflictClause *onConflictClause,
                                              List **arbiterExpr,
                                              Node **arbiterWhere,
                                              Oid *constraint);

    /*
     * transform_expr_node  –  optional hook for transforming dialect-
     * specific raw expression nodes (e.g. UserVarRef, UserVarAssign)
     * before the standard expression transformer processes them.
     *
     * Return true if the node was fully transformed (result is set).
     * Return false to let the standard switch handle it.
     */
    bool        (*transform_expr_node)(struct ParseState *pstate,
                                       Node *expr,
                                       Node **result);

	/*
	 * figure_colname -- optionally derive an implicit result-column name
	 * from a dialect-specific raw expression.  Returning NULL requests the
	 * standard PostgreSQL FigureColname() fallback.
	 */
	char	   *(*figure_colname)(Node *expr);
} ParserRoutine;

#endif   /* PARSERAPI_H */
