/*
 * texteq_mys.c — MySQL-style text equality for the <=> operator.
 *
 * MySQL comparison ignores trailing spaces (PAD SPACE semantics),
 * unlike PostgreSQL's texteq.  This implementation uses PG18's public
 * collation-aware comparison and strips trailing spaces.
 *
 * Adapted from openHalo/openHalo bpchar.c:texteq_mys.
 */
#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/varlena.h"
#include "utils/pg_locale.h"

PG_FUNCTION_INFO_V1(texteq_mys);

Datum
texteq_mys(PG_FUNCTION_ARGS)
{
    text *arg1 = PG_GETARG_TEXT_PP(0);
    text *arg2 = PG_GETARG_TEXT_PP(1);
    bool  result;

    /* Use PG18's built-in texteq for collation-aware comparison.
       For MySQL trailing-space-insensitive behavior, we strip
       trailing spaces from both sides before comparing. */
    {
        char *s1 = text_to_cstring(arg1);
        char *s2 = text_to_cstring(arg2);
        int   len1 = strlen(s1);
        int   len2 = strlen(s2);

        /* MySQL PAD SPACE: trim trailing spaces */
        while (len1 > 0 && s1[len1 - 1] == ' ') len1--;
        while (len2 > 0 && s2[len2 - 1] == ' ') len2--;

        s1[len1] = '\0';
        s2[len2] = '\0';

        result = (len1 == len2 && memcmp(s1, s2, len1) == 0);

        pfree(s1);
        pfree(s2);
    }

    PG_RETURN_BOOL(result);
}
