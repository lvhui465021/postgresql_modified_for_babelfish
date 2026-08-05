/*-------------------------------------------------------------------------
 *
 * mys_utility.h
 *    MySQL utility-command executor entry point.
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_UTILITY_H
#define MYS_UTILITY_H

#include "tcop/utility.h"

extern void mys_standard_ProcessUtility(PlannedStmt *pstmt,
                                        const char *queryString,
                                        bool readOnlyTree,
                                        ProcessUtilityContext context,
                                        ParamListInfo params,
                                        QueryEnvironment *queryEnv,
                                        DestReceiver *dest,
                                        QueryCompletion *qc);

#endif                              /* MYS_UTILITY_H */
