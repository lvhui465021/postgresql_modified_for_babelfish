#include "postgres.h"
/*-------------------------------------------------------------------------
 *
 * bpchar.c
 *	  Extend bpchar routines
 *
 * 

 * 
 *
 * IDENTIFICATION
 *	  src/backend/utils/ddsm/mysm/bpchar.c
 *
 *-------------------------------------------------------------------------
 */

#include "mysm_compat.h"

#include <ctype.h>

#include "access/detoast.h"
#include "catalog/pg_collation.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/pg_locale.h"
#include "miscadmin.h"
/* adapter.h → mysm_compat.h */

#include "../../adt/like.c"
#include "../../adt/varchar.c"

PG_MODULE_MAGIC;


// static int	text_cmp(text *arg1, text *arg2, Oid collid);


PG_FUNCTION_INFO_V1(char_eq_char_for_date_format);
Datum
char_eq_char_for_date_format(PG_FUNCTION_ARGS)
{
    bool result;
    BpChar *arg1;
    BpChar *arg2;
    int len1;
    int len2;
    char* str1;
    char* str2;

    if (PG_ARGISNULL(0))
    {
        PG_RETURN_BOOL(false);
    }
    if (PG_ARGISNULL(1))
    {
        PG_RETURN_BOOL(false);
    }

    arg1 = PG_GETARG_BPCHAR_PP(0);
    arg2 = PG_GETARG_BPCHAR_PP(1);
    len1 = VARSIZE_ANY_EXHDR(arg1);
    len2 = VARSIZE_ANY_EXHDR(arg2);
    str1 = VARDATA_ANY(arg1);
    str2 = VARDATA_ANY(arg2);
    if (len1 == len2)
    {
        if (0 < len1)
        {
            if (str1[0] == str2[0])
            {
                result = true;
            }
            else 
            {
                result = false;
            }
        }
        else
        {
            result = false;
        }
    }
    else
    {
        result = false;
    }

    PG_FREE_IF_COPY(arg1, 0);
    PG_FREE_IF_COPY(arg2, 1);

    PG_RETURN_BOOL(result);
}


PG_FUNCTION_INFO_V1(texteq_mys);
Datum
texteq_mys(PG_FUNCTION_ARGS)
{
    bool		result;
    Oid			collid = PG_GET_COLLATION();

    check_collation_set(collid);
    if (lc_collate_is_c(collid) ||
        collid == DEFAULT_COLLATION_OID ||
        pg_newlocale_from_collation(collid)->deterministic)
    {
        Datum datum1 = PG_GETARG_DATUM(0);
        char* str1 = TextDatumGetCString(datum1);
        Size orig_len1 = strlen(str1);
        Datum datum2 = PG_GETARG_DATUM(1);
        char* str2 = TextDatumGetCString(datum2);
        Size orig_len2 = strlen(str2);
        Size len1 = orig_len1;
        Size len2 = orig_len2;
        Size i;
        Size j;

        for (i = (orig_len1 - 1); 0 <= i; i--)
        {
            if (str1[i] != ' ')
            {
                if (i < (orig_len1 - 1))
                {
                    len1 = i + 1;
                    str1[len1] = '\0';
                }
                break;
            }
        }
        for (j = (orig_len2 - 1); 0 <= j; j--)
        {
            if (str2[j] != ' ')
            {
                if (j < (orig_len2 - 1))
                {
                    len2 = j + 1;
                    str2[len2] = '\0';
                }
                break;
            }
        }

        if (len1 != len2)
        {
            result = false;
        }
        else
        {
            if (collid == caseInsensitiveId)
            {
                result = (strncasecmp(str1, str2, len1) == 0);
            }
            else 
            {
                result = (memcmp(str1, str2, len1) == 0);
            }
        }
    }
    else
    {
        Datum datum1 = PG_GETARG_DATUM(0);
        char* str1 = TextDatumGetCString(datum1);
        Size orig_len1 = strlen(str1);
        Datum datum2 = PG_GETARG_DATUM(1);
        char* str2 = TextDatumGetCString(datum2);
        Size orig_len2 = strlen(str2);
        Size len1;
        Size len2;
        int i;
        int j;

        len1 = 0;
        len2 = 0;
        for (i = (orig_len1 - 1); 0 <= i; i--)
        {
            if (str1[i] != ' ')
            {
                if (i < (orig_len1 - 1))
                {
                    len1 = i + 1;
                    str1[len1] = '\0';
                }
                break;
            }
        }
        for (j = (orig_len2 - 1); 0 <= j; j--)
        {
            if (str2[j] != ' ')
            {
                if (j < (orig_len2 - 1))
                {
                    len2 = j + 1;
                    str1[len2] = '\0';
                }
                break;
            }
        }

        result = (varstr_cmp(str1, len1, str2, len2, collid) == 0);
    }

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(textne_mys);
Datum
textne_mys(PG_FUNCTION_ARGS)
{
    bool		result;
    Oid			collid = PG_GET_COLLATION();

    check_collation_set(collid);
    if (lc_collate_is_c(collid) ||
        collid == DEFAULT_COLLATION_OID ||
        pg_newlocale_from_collation(collid)->deterministic)
    {
        Datum datum1 = PG_GETARG_DATUM(0);
        char* str1 = TextDatumGetCString(datum1);
        Size orig_len1 = strlen(str1);
        Datum datum2 = PG_GETARG_DATUM(1);
        char* str2 = TextDatumGetCString(datum2);
        Size orig_len2 = strlen(str2);
        Size len1 = orig_len1;
        Size len2 = orig_len2;
        Size i;
        Size j;

        for (i = (orig_len1 - 1); 0 <= i; i--)
        {
            if (str1[i] != ' ')
            {
                if (i < (orig_len1 - 1))
                {
                    len1 = i + 1;
                    str1[len1] = '\0';
                }
                break;
            }
        }
        for (j = (orig_len2 - 1); 0 <= j; j--)
        {
            if (str2[j] != ' ')
            {
                if (j < (orig_len2 - 1))
                {
                    len2 = j + 1;
                    str2[len2] = '\0';
                }
                break;
            }
        }

        if (len1 != len2)
        {
            result = true;
        }
        else
        {
            if (collid == caseInsensitiveId)
            {
                result = (strncasecmp(str1, str2, len1) != 0);
            }
            else 
            {
                result = (memcmp(str1, str2, len1) != 0);
            }
        }
    }
    else
    {
        Datum datum1 = PG_GETARG_DATUM(0);
        char* str1 = TextDatumGetCString(datum1);
        Size orig_len1 = strlen(str1);
        Datum datum2 = PG_GETARG_DATUM(1);
        char* str2 = TextDatumGetCString(datum2);
        Size orig_len2 = strlen(str2);
        Size len1 = 0;
        Size len2 = 0;
        int i;
        int j;

        for (i = (orig_len1 - 1); 0 <= i; i--)
        {
            if (str1[i] != ' ')
            {
                if (i < (orig_len1 - 1))
                {
                    len1 = i + 1;
                    str1[len1] = '\0';
                }
                break;
            }
        }
        for (j = (orig_len2 - 1); 0 <= j; j--)
        {
            if (str2[j] != ' ')
            {
                if (j < (orig_len2 - 1))
                {
                    len2 = j + 1;
                    str1[len2] = '\0';
                }
                break;
            }
        }

        result = (varstr_cmp(str1, len1, str2, len2, collid) != 0);
    }

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(bpcharlike);
Datum
bpcharlike(PG_FUNCTION_ARGS)
{
    BpChar	   *str = PG_GETARG_BPCHAR_PP(0);
    text	   *pat = PG_GETARG_TEXT_PP(1);
    bool		result;
    char* s = VARDATA_ANY(str);
    int slen = VARSIZE_ANY_EXHDR(str);
    char* p = VARDATA_ANY(pat);
    int plen = VARSIZE_ANY_EXHDR(pat);
    int str_orig_len = slen;
    int i;
    Oid collid;

    for (i = (str_orig_len - 1); 0 <= i; i--)
    {
        if (s[i] != ' ')
        {
            if (i < (str_orig_len - 1))
            {
                slen = i + 1;
                s[slen] = '\0';
            }
            break;
        }
    }

    collid = PG_GET_COLLATION();
    if (collid == caseInsensitiveId)
    {
        result = (Generic_Text_IC_like(str, pat) == LIKE_TRUE);
    }
    else 
    {
        result = (MatchText(s, slen, p, plen, 0)) == LIKE_TRUE);
    }

    if (i < (str_orig_len - 1))
    {
        s[slen] = ' ';
    }

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(bpcharnlike);
Datum
bpcharnlike(PG_FUNCTION_ARGS)
{
    BpChar	   *str = PG_GETARG_BPCHAR_PP(0);
    text	   *pat = PG_GETARG_TEXT_PP(1);
    bool		result;
    char* s = VARDATA_ANY(str);
    int slen = VARSIZE_ANY_EXHDR(str);
    char* p = VARDATA_ANY(pat);
    int plen = VARSIZE_ANY_EXHDR(pat);
    int str_orig_len = slen;
    int i;
    Oid collid;

    for (i = (str_orig_len - 1); 0 <= i; i--)
    {
        if (s[i] != ' ')
        {
            if (i < (str_orig_len - 1))
            {
                slen = i + 1;
                s[slen] = '\0';
            }
            break;
        }
    }

    collid = PG_GET_COLLATION();
    if (collid == caseInsensitiveId)
    {
        result = (Generic_Text_IC_like(str, pat) != LIKE_TRUE);
    }
    else 
    {
        result = (MatchText(s, slen, p, plen, 0)) != LIKE_TRUE);
    }

    if (i < (str_orig_len - 1))
    {
        s[slen] = ' ';
    }

    PG_RETURN_BOOL(result);
}

/* text_cmp()
 * Internal comparison function for text strings.
 * Returns -1, 0 or 1
 */
// static int
// text_cmp(text *arg1, text *arg2, Oid collid)
// {
//     char	   *a1p,
//                *a2p;
//     int			len1,
//                 len2;

//     a1p = VARDATA_ANY(arg1);
//     a2p = VARDATA_ANY(arg2);

//     len1 = VARSIZE_ANY_EXHDR(arg1);
//     len2 = VARSIZE_ANY_EXHDR(arg2);

//     return varstr_cmp(a1p, len1, a2p, len2, collid);
// }

