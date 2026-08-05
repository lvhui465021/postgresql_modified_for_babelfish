/*-------------------------------------------------------------------------
 *
 * systemVar.h
 *    MySQL adapter systemVar routines
 *
 * Portions Copyright (c) 2026, HaloLab / openHalo Contributors
 *
 * src/include/adapter/mysql/systemVar.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef ADAPTER_MYSQL_SYSTEMVAR_H
#define ADAPTER_MYSQL_SYSTEMVAR_H


#include "utils/hsearch.h"


/* Bits for different SQL modes modes (including ANSI mode) */
#define MYS_MODE_REAL_AS_FLOAT              1
#define MYS_MODE_PIPES_AS_CONCAT            2
#define MYS_MODE_ANSI_QUOTES                4
#define MYS_MODE_IGNORE_SPACE               8
#define MYS_MODE_NOT_USED                   16
#define MYS_MODE_ONLY_FULL_GROUP_BY         32
#define MYS_MODE_NO_UNSIGNED_SUBTRACTION    64
#define MYS_MODE_NO_DIR_IN_CREATE           128
#define MYS_MODE_POSTGRESQL                 256
#define MYS_MODE_ORACLE                     512
#define MYS_MODE_MSSQL                      1024
#define MYS_MODE_DB2                        2048
#define MYS_MODE_MAXDB                      4096
#define MYS_MODE_NO_KEY_OPTIONS             8192
#define MYS_MODE_NO_TABLE_OPTIONS           16384
#define MYS_MODE_NO_FIELD_OPTIONS           32768
#define MYS_MODE_MYSQL323                   65536L
#define MYS_MODE_MYSQL40                    (1 << 17)
#define MYS_MODE_ANSI                       (1 << 18)
#define MYS_MODE_NO_AUTO_VALUE_ON_ZERO      (1 << 19)
#define MYS_MODE_NO_BACKSLASH_ESCAPES       (1 << 20)
#define MYS_MODE_STRICT_TRANS_TABLES        (1 << 21)
#define MYS_MODE_STRICT_ALL_TABLES          (1 << 22)
/*
 * NO_ZERO_DATE, NO_ZERO_IN_DATE and ERROR_FOR_DIVISION_BY_ZERO modes are
 * removed in 5.7 and their functionality is merged with STRICT MODE.
 * However, For backward compatibility during upgrade, these modes are kept
 * but they are not used. Setting these modes in 5.7 will give warning and
 * have no effect.
 */
#define MYS_MODE_NO_ZERO_IN_DATE            (1 << 23)
#define MYS_MODE_NO_ZERO_DATE               (1 << 24)
#define MYS_MODE_INVALID_DATES              (1 << 25)
#define MYS_MODE_ERROR_FOR_DIVISION_BY_ZERO (1 << 26)
#define MYS_MODE_TRADITIONAL                (1 << 27)
#define MYS_MODE_NO_AUTO_CREATE_USER        (1 << 28)
#define MYS_MODE_HIGH_NOT_PRECEDENCE        (1 << 29)
#define MYS_MODE_NO_ENGINE_SUBSTITUTION     (1 << 30)
#define MYS_MODE_PAD_CHAR_TO_FULL_LENGTH    (1ULL << 31)


extern int autoCommit;
extern uint64 mys_sqlMode;
extern int default_week_format;

extern HTAB *globalSystemVars;
extern HTAB *globalSystemVarsLock;

extern bool needCommitTrx;
extern bool needStartNewTrx;
extern bool isStrictTransTablesOn;

extern bool MysAutocommitEnabled(void);
extern void MysSetAutocommit(bool enabled);
extern void MysInitSessionTimeZone(void);
extern bool MysIsSessionVariableSupported(const char *name);
extern bool MysIsGlobalVariableSupported(const char *name);
extern void MysSetSessionTimeZone(const char *value);
extern const char *MysGetSessionTimeZone(void);
extern void MysSetGlobalTimeZone(const char *value);
extern char *MysGetGlobalTimeZone(void);
extern Size MysTimeZoneShmemSize(void);
extern void MysTimeZoneShmemInit(void);

Size GlobalSystemVariablesShmemSize(void);
void GlobalSystemVariablesShmemInit(void);
void getSystemVariableValueForSelect(char *varName, 
                                     bool isSessionSystemVar, 
                                     char **varValue);
bool getSystemVariableValueForShow(char *varName, 
                                   bool isSessionSystemVar, 
                                   char **varValue);
void setSystemVariableValue(char *varName, char *varValue, 
                            bool isSessionSystemVar);
void setSystemVariableDatum(char *varName, Datum varConfValue, 
                            Oid varValueType, bool isNull, 
                            bool isSessionSystemVar);
bool isSystemVariable(char *varName);


#endif                          /* ADAPTER_MYSQL_SYSTEMVAR_H */
