/*-------------------------------------------------------------------------
 *
 * mys_expr_transform.h
 *    Declarations for MySQL-specific expression node lowering.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/mysql/mys_expr_transform.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_EXPR_TRANSFORM_H
#define MYS_EXPR_TRANSFORM_H

struct ParseState;
struct Node;

extern bool mys_transform_expr_node(struct ParseState *pstate,
                                    struct Node *expr,
                                    struct Node **result);
extern char *mys_figure_colname(struct Node *expr);

#endif /* MYS_EXPR_TRANSFORM_H */
