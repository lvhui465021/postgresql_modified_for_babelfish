/*-------------------------------------------------------------------------
 *
 * mys_sequence.h
 *    MySQL AUTO_INCREMENT sequence bridge.
 *
 *-------------------------------------------------------------------------
 */
#ifndef MYS_SEQUENCE_H
#define MYS_SEQUENCE_H

#include "postgres.h"

extern int64 mys_setval3_oid(Oid seqOid, int64 next, bool isCalled);

#endif                          /* MYS_SEQUENCE_H */
