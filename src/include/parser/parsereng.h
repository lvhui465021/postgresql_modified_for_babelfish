/*-------------------------------------------------------------------------
 *
 * parsereng.h
 *    Parser engine selection and initialization.
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/parser/parsereng.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSERENG_H
#define PARSERENG_H

#include "libpq/libpq-be.h"
#include "parser/parserapi.h"

extern const struct ParserRoutine *GetStandardParserRoutine(void);
extern const struct ParserRoutine *GetDialectParserRoutine(void);
extern const struct ParserRoutine *GetRegisteredParserRoutine(CompatibilityProtocolKind kind);
extern void InitCompatMode(void);
extern void InitParserEngine(void);

/*
 * Dialect parser registration.
 *
 * A dialect (MySQL, and future T-SQL) registers its ParserRoutine with
 * RegisterParserRoutine() so that the kernel can dispatch to it without
 * statically linking the dialect's parser implementation.  This is the
 * seam that lets the parser ship as a loadable library (like mysm) rather
 * than being compiled into the backend.
 */
extern void RegisterParserRoutine(CompatibilityProtocolKind kind,
								  const struct ParserRoutine *routine);

/* Backend-level dialect context (protocol override or cluster default). */
extern CompatibilityProtocolKind MyCompatMode;

/* GUC variable; values use CompatibilityProtocolKind directly. */
extern int database_compat_mode;

/* Parser Engine Instance */
extern const struct ParserRoutine *parserengine;

#endif   /* PARSERENG_H */
