/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_MYS_YY_SRC_MYS_GRAM_H_INCLUDED
# define YY_MYS_YY_SRC_MYS_GRAM_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int mys_yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    IDENT = 258,                   /* IDENT  */
    UIDENT = 259,                  /* UIDENT  */
    FCONST = 260,                  /* FCONST  */
    SCONST = 261,                  /* SCONST  */
    USCONST = 262,                 /* USCONST  */
    BCONST = 263,                  /* BCONST  */
    XCONST = 264,                  /* XCONST  */
    Op = 265,                      /* Op  */
    MysqlUserVariableName = 266,   /* MysqlUserVariableName  */
    MysSysVarName = 267,           /* MysSysVarName  */
    ICONST = 268,                  /* ICONST  */
    PARAM = 269,                   /* PARAM  */
    TYPECAST = 270,                /* TYPECAST  */
    DOT_DOT = 271,                 /* DOT_DOT  */
    COLON_EQUALS = 272,            /* COLON_EQUALS  */
    EQUALS_GREATER = 273,          /* EQUALS_GREATER  */
    LESS_EQUALS = 274,             /* LESS_EQUALS  */
    GREATER_EQUALS = 275,          /* GREATER_EQUALS  */
    NOT_EQUALS = 276,              /* NOT_EQUALS  */
    UNDERSCORE_BINARY = 277,       /* UNDERSCORE_BINARY  */
    MysParam = 278,                /* MysParam  */
    Op_And = 279,                  /* Op_And  */
    Op_Or = 280,                   /* Op_Or  */
    ABORT_P = 281,                 /* ABORT_P  */
    ABSOLUTE_P = 282,              /* ABSOLUTE_P  */
    ACCESS = 283,                  /* ACCESS  */
    ACTION = 284,                  /* ACTION  */
    ADD_P = 285,                   /* ADD_P  */
    ADMIN = 286,                   /* ADMIN  */
    AFTER = 287,                   /* AFTER  */
    AGGREGATE = 288,               /* AGGREGATE  */
    ALGORITHM = 289,               /* ALGORITHM  */
    ALL = 290,                     /* ALL  */
    ALSO = 291,                    /* ALSO  */
    ALTER = 292,                   /* ALTER  */
    ALWAYS = 293,                  /* ALWAYS  */
    ANALYSE = 294,                 /* ANALYSE  */
    ANALYZE = 295,                 /* ANALYZE  */
    AND = 296,                     /* AND  */
    ANY = 297,                     /* ANY  */
    ANY_VALUE = 298,               /* ANY_VALUE  */
    ARRAY = 299,                   /* ARRAY  */
    AS = 300,                      /* AS  */
    ASC = 301,                     /* ASC  */
    ASCII = 302,                   /* ASCII  */
    ASENSITIVE = 303,              /* ASENSITIVE  */
    ASSERTION = 304,               /* ASSERTION  */
    ASSIGNMENT = 305,              /* ASSIGNMENT  */
    ASYMMETRIC = 306,              /* ASYMMETRIC  */
    ATOMIC = 307,                  /* ATOMIC  */
    AT = 308,                      /* AT  */
    ATTACH = 309,                  /* ATTACH  */
    ATTRIBUTE = 310,               /* ATTRIBUTE  */
    AUTO_INCREMENT = 311,          /* AUTO_INCREMENT  */
    AVG_ROW_LENGTH = 312,          /* AVG_ROW_LENGTH  */
    BACKWARD = 313,                /* BACKWARD  */
    BEFORE = 314,                  /* BEFORE  */
    BEGIN_P = 315,                 /* BEGIN_P  */
    BETWEEN = 316,                 /* BETWEEN  */
    BIGINT = 317,                  /* BIGINT  */
    BINARY = 318,                  /* BINARY  */
    BIT = 319,                     /* BIT  */
    BIT_AS_BYTEA = 320,            /* BIT_AS_BYTEA  */
    BLOB = 321,                    /* BLOB  */
    BOOL_P = 322,                  /* BOOL_P  */
    BOOLEAN_P = 323,               /* BOOLEAN_P  */
    BOTH = 324,                    /* BOTH  */
    BREADTH = 325,                 /* BREADTH  */
    BY = 326,                      /* BY  */
    BYTE = 327,                    /* BYTE  */
    CACHE = 328,                   /* CACHE  */
    CALL = 329,                    /* CALL  */
    CALLED = 330,                  /* CALLED  */
    CASCADE = 331,                 /* CASCADE  */
    CASCADED = 332,                /* CASCADED  */
    CASE = 333,                    /* CASE  */
    CAST = 334,                    /* CAST  */
    CATALOG_P = 335,               /* CATALOG_P  */
    CHAIN = 336,                   /* CHAIN  */
    CHANGE = 337,                  /* CHANGE  */
    CHAR_P = 338,                  /* CHAR_P  */
    CHARACTER = 339,               /* CHARACTER  */
    CHARACTERISTICS = 340,         /* CHARACTERISTICS  */
    CHARSET = 341,                 /* CHARSET  */
    CHECK = 342,                   /* CHECK  */
    CHECKPOINT = 343,              /* CHECKPOINT  */
    CHECKSUM = 344,                /* CHECKSUM  */
    CLASS = 345,                   /* CLASS  */
    CLOSE = 346,                   /* CLOSE  */
    CLUSTER = 347,                 /* CLUSTER  */
    COALESCE = 348,                /* COALESCE  */
    COLLATE = 349,                 /* COLLATE  */
    COLLATION = 350,               /* COLLATION  */
    COLUMN = 351,                  /* COLUMN  */
    COLUMNS = 352,                 /* COLUMNS  */
    COLUMN_FORMAT = 353,           /* COLUMN_FORMAT  */
    COMMENT = 354,                 /* COMMENT  */
    COMMENTS = 355,                /* COMMENTS  */
    COMMIT = 356,                  /* COMMIT  */
    COMMITTED = 357,               /* COMMITTED  */
    COMPRESSION = 358,             /* COMPRESSION  */
    CONCURRENTLY = 359,            /* CONCURRENTLY  */
    CONFIGURATION = 360,           /* CONFIGURATION  */
    CONFLICT = 361,                /* CONFLICT  */
    CONNECTION = 362,              /* CONNECTION  */
    CONSTRAINT = 363,              /* CONSTRAINT  */
    CONSTRAINTS = 364,             /* CONSTRAINTS  */
    CONTAINS = 365,                /* CONTAINS  */
    CONTENT_P = 366,               /* CONTENT_P  */
    CONTINUE_P = 367,              /* CONTINUE_P  */
    CONVERSION_P = 368,            /* CONVERSION_P  */
    CONVERT = 369,                 /* CONVERT  */
    COPY = 370,                    /* COPY  */
    COST = 371,                    /* COST  */
    CREATE = 372,                  /* CREATE  */
    CROSS = 373,                   /* CROSS  */
    CSV = 374,                     /* CSV  */
    CUBE = 375,                    /* CUBE  */
    CURRENT_P = 376,               /* CURRENT_P  */
    CURRENT_CATALOG = 377,         /* CURRENT_CATALOG  */
    CURRENT_DATE = 378,            /* CURRENT_DATE  */
    CURRENT_ROLE = 379,            /* CURRENT_ROLE  */
    CURRENT_SCHEMA = 380,          /* CURRENT_SCHEMA  */
    CURRENT_TIME = 381,            /* CURRENT_TIME  */
    CURRENT_TIMESTAMP = 382,       /* CURRENT_TIMESTAMP  */
    CURRENT_USER = 383,            /* CURRENT_USER  */
    CURSOR = 384,                  /* CURSOR  */
    CYCLE = 385,                   /* CYCLE  */
    DATA_P = 386,                  /* DATA_P  */
    DATABASE = 387,                /* DATABASE  */
    DATABASES = 388,               /* DATABASES  */
    DATE = 389,                    /* DATE  */
    DATETIME = 390,                /* DATETIME  */
    DAY_P = 391,                   /* DAY_P  */
    DEALLOCATE = 392,              /* DEALLOCATE  */
    DEC = 393,                     /* DEC  */
    DECIMAL_P = 394,               /* DECIMAL_P  */
    DECLARE = 395,                 /* DECLARE  */
    DEFAULT = 396,                 /* DEFAULT  */
    DEFAULTS = 397,                /* DEFAULTS  */
    DEFERRABLE = 398,              /* DEFERRABLE  */
    DEFERRED = 399,                /* DEFERRED  */
    DEFINER = 400,                 /* DEFINER  */
    DELAY_KEY_WRITE = 401,         /* DELAY_KEY_WRITE  */
    DELAYED = 402,                 /* DELAYED  */
    DELETE_P = 403,                /* DELETE_P  */
    DELIMITER = 404,               /* DELIMITER  */
    DELIMITERS = 405,              /* DELIMITERS  */
    DEPENDS = 406,                 /* DEPENDS  */
    DEPTH = 407,                   /* DEPTH  */
    DESC = 408,                    /* DESC  */
    DESCRIBE = 409,                /* DESCRIBE  */
    DETACH = 410,                  /* DETACH  */
    DETERMINISTIC = 411,           /* DETERMINISTIC  */
    DICTIONARY = 412,              /* DICTIONARY  */
    DIRECTORY = 413,               /* DIRECTORY  */
    DISABLE_P = 414,               /* DISABLE_P  */
    DISCARD = 415,                 /* DISCARD  */
    DISK = 416,                    /* DISK  */
    DISTINCT = 417,                /* DISTINCT  */
    DISTINCTROW = 418,             /* DISTINCTROW  */
    DIV = 419,                     /* DIV  */
    DO = 420,                      /* DO  */
    DOCUMENT_P = 421,              /* DOCUMENT_P  */
    DOMAIN_P = 422,                /* DOMAIN_P  */
    DOUBLE_P = 423,                /* DOUBLE_P  */
    DROP = 424,                    /* DROP  */
    DUPLICATE = 425,               /* DUPLICATE  */
    DYNAMIC = 426,                 /* DYNAMIC  */
    EACH = 427,                    /* EACH  */
    ELSE = 428,                    /* ELSE  */
    ELSEIF = 429,                  /* ELSEIF  */
    ENABLE_P = 430,                /* ENABLE_P  */
    ENCODING = 431,                /* ENCODING  */
    ENCRYPTED = 432,               /* ENCRYPTED  */
    ENCRYPTION = 433,              /* ENCRYPTION  */
    END_P = 434,                   /* END_P  */
    MYS_ENGINE = 435,              /* MYS_ENGINE  */
    ENGINES = 436,                 /* ENGINES  */
    ENUM_P = 437,                  /* ENUM_P  */
    ESCAPE = 438,                  /* ESCAPE  */
    EVENT = 439,                   /* EVENT  */
    EXCEPT = 440,                  /* EXCEPT  */
    EXCLUDE = 441,                 /* EXCLUDE  */
    EXCLUDING = 442,               /* EXCLUDING  */
    EXCLUSIVE = 443,               /* EXCLUSIVE  */
    EXECUTE = 444,                 /* EXECUTE  */
    EXISTS = 445,                  /* EXISTS  */
    EXIT = 446,                    /* EXIT  */
    EXPLAIN = 447,                 /* EXPLAIN  */
    EXPRESSION = 448,              /* EXPRESSION  */
    EXTENSION = 449,               /* EXTENSION  */
    EXTERNAL = 450,                /* EXTERNAL  */
    EXTRACT = 451,                 /* EXTRACT  */
    FALSE_P = 452,                 /* FALSE_P  */
    FAMILY = 453,                  /* FAMILY  */
    FETCH = 454,                   /* FETCH  */
    FIELDS = 455,                  /* FIELDS  */
    FILTER = 456,                  /* FILTER  */
    FINALIZE = 457,                /* FINALIZE  */
    FIRST_P = 458,                 /* FIRST_P  */
    FIXED = 459,                   /* FIXED  */
    FLOAT_P = 460,                 /* FLOAT_P  */
    FOLLOWING = 461,               /* FOLLOWING  */
    FOR = 462,                     /* FOR  */
    FORCE = 463,                   /* FORCE  */
    FOREIGN = 464,                 /* FOREIGN  */
    FORWARD = 465,                 /* FORWARD  */
    FOUND = 466,                   /* FOUND  */
    FREEZE = 467,                  /* FREEZE  */
    FROM = 468,                    /* FROM  */
    FULL = 469,                    /* FULL  */
    FULLTEXT = 470,                /* FULLTEXT  */
    FUNCTION = 471,                /* FUNCTION  */
    FUNCTIONS = 472,               /* FUNCTIONS  */
    GENERATED = 473,               /* GENERATED  */
    GET = 474,                     /* GET  */
    GLOBAL = 475,                  /* GLOBAL  */
    GRANT = 476,                   /* GRANT  */
    GRANTED = 477,                 /* GRANTED  */
    GREATEST = 478,                /* GREATEST  */
    GROUP_P = 479,                 /* GROUP_P  */
    GROUPING = 480,                /* GROUPING  */
    GROUPS = 481,                  /* GROUPS  */
    GROUP_CONCAT = 482,            /* GROUP_CONCAT  */
    HANDLER = 483,                 /* HANDLER  */
    HASH = 484,                    /* HASH  */
    HAVING = 485,                  /* HAVING  */
    HEADER_P = 486,                /* HEADER_P  */
    HIGH_PRIORITY = 487,           /* HIGH_PRIORITY  */
    HOLD = 488,                    /* HOLD  */
    HOUR_P = 489,                  /* HOUR_P  */
    IDENTIFIED = 490,              /* IDENTIFIED  */
    IDENTITY_P = 491,              /* IDENTITY_P  */
    IF_P = 492,                    /* IF_P  */
    IGNORE = 493,                  /* IGNORE  */
    ILIKE = 494,                   /* ILIKE  */
    IMMEDIATE = 495,               /* IMMEDIATE  */
    IMMUTABLE = 496,               /* IMMUTABLE  */
    IMPLICIT_P = 497,              /* IMPLICIT_P  */
    IMPORT_P = 498,                /* IMPORT_P  */
    IN_P = 499,                    /* IN_P  */
    INCLUDE = 500,                 /* INCLUDE  */
    INCLUDING = 501,               /* INCLUDING  */
    INCREMENT = 502,               /* INCREMENT  */
    INDEX = 503,                   /* INDEX  */
    INDEXES = 504,                 /* INDEXES  */
    INHERIT = 505,                 /* INHERIT  */
    INHERITS = 506,                /* INHERITS  */
    INITIALLY = 507,               /* INITIALLY  */
    INLINE_P = 508,                /* INLINE_P  */
    INNER_P = 509,                 /* INNER_P  */
    INOUT = 510,                   /* INOUT  */
    INPLACE = 511,                 /* INPLACE  */
    INPUT_P = 512,                 /* INPUT_P  */
    INSENSITIVE = 513,             /* INSENSITIVE  */
    INSERT = 514,                  /* INSERT  */
    INSERT_METHOD = 515,           /* INSERT_METHOD  */
    INSTEAD = 516,                 /* INSTEAD  */
    INT_P = 517,                   /* INT_P  */
    INTEGER = 518,                 /* INTEGER  */
    INTERSECT = 519,               /* INTERSECT  */
    INTERVAL = 520,                /* INTERVAL  */
    INTO = 521,                    /* INTO  */
    INVISIBLE = 522,               /* INVISIBLE  */
    INVOKER = 523,                 /* INVOKER  */
    IS = 524,                      /* IS  */
    ISOLATION = 525,               /* ISOLATION  */
    ITERATE = 526,                 /* ITERATE  */
    JOIN = 527,                    /* JOIN  */
    KEY = 528,                     /* KEY  */
    KEYS_P = 529,                  /* KEYS_P  */
    KEY_BLOCK_SIZE = 530,          /* KEY_BLOCK_SIZE  */
    LABEL = 531,                   /* LABEL  */
    LANGUAGE = 532,                /* LANGUAGE  */
    LARGE_P = 533,                 /* LARGE_P  */
    LAST_P = 534,                  /* LAST_P  */
    LATERAL_P = 535,               /* LATERAL_P  */
    LEADING = 536,                 /* LEADING  */
    LEAKPROOF = 537,               /* LEAKPROOF  */
    LEAST = 538,                   /* LEAST  */
    LEAVE = 539,                   /* LEAVE  */
    LEFT = 540,                    /* LEFT  */
    LESS = 541,                    /* LESS  */
    LEVEL = 542,                   /* LEVEL  */
    LIKE = 543,                    /* LIKE  */
    LIMIT = 544,                   /* LIMIT  */
    LINEAR = 545,                  /* LINEAR  */
    LIST = 546,                    /* LIST  */
    LISTEN = 547,                  /* LISTEN  */
    LOAD = 548,                    /* LOAD  */
    LOCAL = 549,                   /* LOCAL  */
    LOCALTIME = 550,               /* LOCALTIME  */
    LOCALTIMESTAMP = 551,          /* LOCALTIMESTAMP  */
    LOCATION = 552,                /* LOCATION  */
    LOCK_P = 553,                  /* LOCK_P  */
    LOCKED = 554,                  /* LOCKED  */
    LOGGED = 555,                  /* LOGGED  */
    LONG = 556,                    /* LONG  */
    LONGBLOB = 557,                /* LONGBLOB  */
    LONGTEXT = 558,                /* LONGTEXT  */
    LOOP = 559,                    /* LOOP  */
    LOW_PRIORITY = 560,            /* LOW_PRIORITY  */
    MAPPING = 561,                 /* MAPPING  */
    MATCH = 562,                   /* MATCH  */
    MATERIALIZED = 563,            /* MATERIALIZED  */
    MAXVALUE = 564,                /* MAXVALUE  */
    MAX_ROWS = 565,                /* MAX_ROWS  */
    MEDIUMBLOB = 566,              /* MEDIUMBLOB  */
    MEDIUMINT = 567,               /* MEDIUMINT  */
    MEDIUMTEXT = 568,              /* MEDIUMTEXT  */
    MEMORY = 569,                  /* MEMORY  */
    METHOD = 570,                  /* METHOD  */
    MERGE = 571,                   /* MERGE  */
    MID = 572,                     /* MID  */
    MINUTE_P = 573,                /* MINUTE_P  */
    MINVALUE = 574,                /* MINVALUE  */
    MIN_ROWS = 575,                /* MIN_ROWS  */
    MOD = 576,                     /* MOD  */
    MODE = 577,                    /* MODE  */
    MODIFIES = 578,                /* MODIFIES  */
    MODIFY = 579,                  /* MODIFY  */
    MONTH_P = 580,                 /* MONTH_P  */
    MOVE = 581,                    /* MOVE  */
    NAME_P = 582,                  /* NAME_P  */
    NAMES = 583,                   /* NAMES  */
    NATIONAL = 584,                /* NATIONAL  */
    NATURAL = 585,                 /* NATURAL  */
    NCHAR = 586,                   /* NCHAR  */
    NEW = 587,                     /* NEW  */
    NEXT = 588,                    /* NEXT  */
    NFC = 589,                     /* NFC  */
    NFD = 590,                     /* NFD  */
    NFKC = 591,                    /* NFKC  */
    NFKD = 592,                    /* NFKD  */
    NO = 593,                      /* NO  */
    NONE = 594,                    /* NONE  */
    NORMALIZE = 595,               /* NORMALIZE  */
    NORMALIZED = 596,              /* NORMALIZED  */
    NOT = 597,                     /* NOT  */
    NOTHING = 598,                 /* NOTHING  */
    NOTIFY = 599,                  /* NOTIFY  */
    NOTNULL = 600,                 /* NOTNULL  */
    NOW = 601,                     /* NOW  */
    NOWAIT = 602,                  /* NOWAIT  */
    NULL_P = 603,                  /* NULL_P  */
    NULLIF = 604,                  /* NULLIF  */
    NULLS_P = 605,                 /* NULLS_P  */
    NUMERIC = 606,                 /* NUMERIC  */
    NVARCHAR = 607,                /* NVARCHAR  */
    OBJECT_P = 608,                /* OBJECT_P  */
    OF = 609,                      /* OF  */
    OFF = 610,                     /* OFF  */
    OFFSET = 611,                  /* OFFSET  */
    OIDS = 612,                    /* OIDS  */
    OLD = 613,                     /* OLD  */
    ON = 614,                      /* ON  */
    ONLY = 615,                    /* ONLY  */
    OPEN = 616,                    /* OPEN  */
    OPERATOR = 617,                /* OPERATOR  */
    OPTION = 618,                  /* OPTION  */
    OPTIONS = 619,                 /* OPTIONS  */
    OR = 620,                      /* OR  */
    OPTIMIZE = 621,                /* OPTIMIZE  */
    ORDER = 622,                   /* ORDER  */
    ORDINALITY = 623,              /* ORDINALITY  */
    OTHERS = 624,                  /* OTHERS  */
    OUT_P = 625,                   /* OUT_P  */
    OUTER_P = 626,                 /* OUTER_P  */
    OVER = 627,                    /* OVER  */
    OVERLAPS = 628,                /* OVERLAPS  */
    OVERLAY = 629,                 /* OVERLAY  */
    OVERRIDING = 630,              /* OVERRIDING  */
    OWNED = 631,                   /* OWNED  */
    OWNER = 632,                   /* OWNER  */
    PACK_KEYS = 633,               /* PACK_KEYS  */
    PARALLEL = 634,                /* PARALLEL  */
    PARSER = 635,                  /* PARSER  */
    PARTIAL = 636,                 /* PARTIAL  */
    PARTITION = 637,               /* PARTITION  */
    PARTITIONS = 638,              /* PARTITIONS  */
    PASSING = 639,                 /* PASSING  */
    PASSWORD = 640,                /* PASSWORD  */
    PLACING = 641,                 /* PLACING  */
    PLANS = 642,                   /* PLANS  */
    PLUGINS = 643,                 /* PLUGINS  */
    POLICY = 644,                  /* POLICY  */
    POSITION = 645,                /* POSITION  */
    PRECEDING = 646,               /* PRECEDING  */
    PRECISION = 647,               /* PRECISION  */
    PRESERVE = 648,                /* PRESERVE  */
    PREPARE = 649,                 /* PREPARE  */
    PREPARED = 650,                /* PREPARED  */
    PRIMARY = 651,                 /* PRIMARY  */
    PRIOR = 652,                   /* PRIOR  */
    PRIVILEGES = 653,              /* PRIVILEGES  */
    PROCEDURAL = 654,              /* PROCEDURAL  */
    PROCEDURE = 655,               /* PROCEDURE  */
    PROCEDURES = 656,              /* PROCEDURES  */
    PROCESSLIST = 657,             /* PROCESSLIST  */
    PROGRAM = 658,                 /* PROGRAM  */
    PUBLICATION = 659,             /* PUBLICATION  */
    QUICK = 660,                   /* QUICK  */
    QUOTE = 661,                   /* QUOTE  */
    RANGE = 662,                   /* RANGE  */
    READ = 663,                    /* READ  */
    READS = 664,                   /* READS  */
    REAL = 665,                    /* REAL  */
    REASSIGN = 666,                /* REASSIGN  */
    RECHECK = 667,                 /* RECHECK  */
    RECURSIVE = 668,               /* RECURSIVE  */
    REF = 669,                     /* REF  */
    REFERENCES = 670,              /* REFERENCES  */
    REFERENCING = 671,             /* REFERENCING  */
    REFRESH = 672,                 /* REFRESH  */
    REGEXP = 673,                  /* REGEXP  */
    REINDEX = 674,                 /* REINDEX  */
    RELATIVE_P = 675,              /* RELATIVE_P  */
    RELEASE = 676,                 /* RELEASE  */
    RENAME = 677,                  /* RENAME  */
    REPEAT = 678,                  /* REPEAT  */
    REPEATABLE = 679,              /* REPEATABLE  */
    REPLACE = 680,                 /* REPLACE  */
    REPLICA = 681,                 /* REPLICA  */
    REPAIR = 682,                  /* REPAIR  */
    RESET = 683,                   /* RESET  */
    RESTART = 684,                 /* RESTART  */
    RESTRICT = 685,                /* RESTRICT  */
    RETURN = 686,                  /* RETURN  */
    RETURNING = 687,               /* RETURNING  */
    RETURNS = 688,                 /* RETURNS  */
    REVOKE = 689,                  /* REVOKE  */
    RIGHT = 690,                   /* RIGHT  */
    RLIKE = 691,                   /* RLIKE  */
    ROLE = 692,                    /* ROLE  */
    ROLLBACK = 693,                /* ROLLBACK  */
    ROLLUP = 694,                  /* ROLLUP  */
    ROUTINE = 695,                 /* ROUTINE  */
    ROUTINES = 696,                /* ROUTINES  */
    ROW = 697,                     /* ROW  */
    ROW_FORMAT = 698,              /* ROW_FORMAT  */
    ROWS = 699,                    /* ROWS  */
    RULE = 700,                    /* RULE  */
    SAVEPOINT = 701,               /* SAVEPOINT  */
    SCHEMA = 702,                  /* SCHEMA  */
    SCHEMAS = 703,                 /* SCHEMAS  */
    SCROLL = 704,                  /* SCROLL  */
    SEARCH = 705,                  /* SEARCH  */
    SECOND_P = 706,                /* SECOND_P  */
    SECURITY = 707,                /* SECURITY  */
    SELECT = 708,                  /* SELECT  */
    SELETC = 709,                  /* SELETC  */
    SEPARATOR = 710,               /* SEPARATOR  */
    SEQUENCE = 711,                /* SEQUENCE  */
    SEQUENCES = 712,               /* SEQUENCES  */
    SERIALIZABLE = 713,            /* SERIALIZABLE  */
    SERVER = 714,                  /* SERVER  */
    SESSION = 715,                 /* SESSION  */
    SESSION_USER = 716,            /* SESSION_USER  */
    SET = 717,                     /* SET  */
    SETS = 718,                    /* SETS  */
    SETOF = 719,                   /* SETOF  */
    SHARE = 720,                   /* SHARE  */
    SHARED = 721,                  /* SHARED  */
    SHOW = 722,                    /* SHOW  */
    SIGNED = 723,                  /* SIGNED  */
    SIMILAR = 724,                 /* SIMILAR  */
    SIMPLE = 725,                  /* SIMPLE  */
    SKIP = 726,                    /* SKIP  */
    SMALLINT = 727,                /* SMALLINT  */
    SNAPSHOT = 728,                /* SNAPSHOT  */
    SOME = 729,                    /* SOME  */
    SPATIAL = 730,                 /* SPATIAL  */
    SQL_BIG_RESULT = 731,          /* SQL_BIG_RESULT  */
    SQL_BUFFER_RESULT = 732,       /* SQL_BUFFER_RESULT  */
    SQL_CACHE = 733,               /* SQL_CACHE  */
    SQL_CALC_FOUND_ROWS = 734,     /* SQL_CALC_FOUND_ROWS  */
    SQL_NO_CACHE = 735,            /* SQL_NO_CACHE  */
    SQL_SMALL_RESULT = 736,        /* SQL_SMALL_RESULT  */
    SQL_P = 737,                   /* SQL_P  */
    SQLEXCEPTION = 738,            /* SQLEXCEPTION  */
    SQLSTATE = 739,                /* SQLSTATE  */
    STABLE = 740,                  /* STABLE  */
    STANDALONE_P = 741,            /* STANDALONE_P  */
    START = 742,                   /* START  */
    STATEMENT = 743,               /* STATEMENT  */
    STATISTICS = 744,              /* STATISTICS  */
    STATS_AUTO_RECALC = 745,       /* STATS_AUTO_RECALC  */
    STATS_PERSISTENT = 746,        /* STATS_PERSISTENT  */
    STATS_SAMPLE_PAGES = 747,      /* STATS_SAMPLE_PAGES  */
    STATUS = 748,                  /* STATUS  */
    STD = 749,                     /* STD  */
    STDDEV = 750,                  /* STDDEV  */
    STDIN = 751,                   /* STDIN  */
    STDOUT = 752,                  /* STDOUT  */
    STORAGE = 753,                 /* STORAGE  */
    STORED = 754,                  /* STORED  */
    STRICT_P = 755,                /* STRICT_P  */
    STRIP_P = 756,                 /* STRIP_P  */
    SUBPARTITION = 757,            /* SUBPARTITION  */
    SUBPARTITIONS = 758,           /* SUBPARTITIONS  */
    SUBSCRIPTION = 759,            /* SUBSCRIPTION  */
    SUBSTR = 760,                  /* SUBSTR  */
    SUBSTRING = 761,               /* SUBSTRING  */
    SUPPORT = 762,                 /* SUPPORT  */
    SYMMETRIC = 763,               /* SYMMETRIC  */
    SYSID = 764,                   /* SYSID  */
    SYSTEM_P = 765,                /* SYSTEM_P  */
    SYSTEM_USER = 766,             /* SYSTEM_USER  */
    TABLE = 767,                   /* TABLE  */
    TABLES = 768,                  /* TABLES  */
    TABLESAMPLE = 769,             /* TABLESAMPLE  */
    TABLESPACE = 770,              /* TABLESPACE  */
    TEMP = 771,                    /* TEMP  */
    TEMPLATE = 772,                /* TEMPLATE  */
    TEMPORARY = 773,               /* TEMPORARY  */
    TEMPTABLE = 774,               /* TEMPTABLE  */
    TEXT_P = 775,                  /* TEXT_P  */
    THAN = 776,                    /* THAN  */
    THEN = 777,                    /* THEN  */
    TIES = 778,                    /* TIES  */
    TIME = 779,                    /* TIME  */
    TIMESTAMP = 780,               /* TIMESTAMP  */
    TIMESTAMPADD = 781,            /* TIMESTAMPADD  */
    TIMESTAMPDIFF = 782,           /* TIMESTAMPDIFF  */
    TINYBLOB = 783,                /* TINYBLOB  */
    TINYINT = 784,                 /* TINYINT  */
    TINYTEXT = 785,                /* TINYTEXT  */
    TO = 786,                      /* TO  */
    TRAILING = 787,                /* TRAILING  */
    TRANSACTION = 788,             /* TRANSACTION  */
    TRANSFORM = 789,               /* TRANSFORM  */
    TREAT = 790,                   /* TREAT  */
    TRIGGER = 791,                 /* TRIGGER  */
    TRIGGERS = 792,                /* TRIGGERS  */
    TRIM = 793,                    /* TRIM  */
    TRUE_P = 794,                  /* TRUE_P  */
    TRUNCATE = 795,                /* TRUNCATE  */
    TRUSTED = 796,                 /* TRUSTED  */
    TYPE_P = 797,                  /* TYPE_P  */
    TYPES_P = 798,                 /* TYPES_P  */
    UESCAPE = 799,                 /* UESCAPE  */
    UNBOUNDED = 800,               /* UNBOUNDED  */
    UNCOMMITTED = 801,             /* UNCOMMITTED  */
    UNDEFINED = 802,               /* UNDEFINED  */
    UNENCRYPTED = 803,             /* UNENCRYPTED  */
    UNICODE = 804,                 /* UNICODE  */
    UNION = 805,                   /* UNION  */
    UNIQUE = 806,                  /* UNIQUE  */
    UNKNOWN = 807,                 /* UNKNOWN  */
    UNLISTEN = 808,                /* UNLISTEN  */
    UNLOCK = 809,                  /* UNLOCK  */
    UNLOGGED = 810,                /* UNLOGGED  */
    UNSIGNED = 811,                /* UNSIGNED  */
    UNTIL = 812,                   /* UNTIL  */
    UPDATE = 813,                  /* UPDATE  */
    USE = 814,                     /* USE  */
    USER = 815,                    /* USER  */
    USING = 816,                   /* USING  */
    UTC_DATE = 817,                /* UTC_DATE  */
    UTC_TIME = 818,                /* UTC_TIME  */
    UTC_TIMESTAMP = 819,           /* UTC_TIMESTAMP  */
    VACUUM = 820,                  /* VACUUM  */
    VALID = 821,                   /* VALID  */
    VALIDATE = 822,                /* VALIDATE  */
    VALIDATOR = 823,               /* VALIDATOR  */
    VALUE_P = 824,                 /* VALUE_P  */
    VALUES = 825,                  /* VALUES  */
    VARBINARY = 826,               /* VARBINARY  */
    VARCHAR = 827,                 /* VARCHAR  */
    VARIABLES = 828,               /* VARIABLES  */
    VARIADIC = 829,                /* VARIADIC  */
    VARYING = 830,                 /* VARYING  */
    VERBOSE = 831,                 /* VERBOSE  */
    VERSION_P = 832,               /* VERSION_P  */
    VIEW = 833,                    /* VIEW  */
    VIEWS = 834,                   /* VIEWS  */
    VIRTUAL = 835,                 /* VIRTUAL  */
    VISIBLE = 836,                 /* VISIBLE  */
    VOLATILE = 837,                /* VOLATILE  */
    WHEN = 838,                    /* WHEN  */
    WHERE = 839,                   /* WHERE  */
    WHILE = 840,                   /* WHILE  */
    WHITESPACE_P = 841,            /* WHITESPACE_P  */
    WARNINGS = 842,                /* WARNINGS  */
    WINDOW = 843,                  /* WINDOW  */
    WITH = 844,                    /* WITH  */
    WITHIN = 845,                  /* WITHIN  */
    WITHOUT = 846,                 /* WITHOUT  */
    WORK = 847,                    /* WORK  */
    WRAPPER = 848,                 /* WRAPPER  */
    WRITE = 849,                   /* WRITE  */
    XML_P = 850,                   /* XML_P  */
    XMLATTRIBUTES = 851,           /* XMLATTRIBUTES  */
    XMLCONCAT = 852,               /* XMLCONCAT  */
    XMLELEMENT = 853,              /* XMLELEMENT  */
    XMLEXISTS = 854,               /* XMLEXISTS  */
    XMLFOREST = 855,               /* XMLFOREST  */
    XMLNAMESPACES = 856,           /* XMLNAMESPACES  */
    XMLPARSE = 857,                /* XMLPARSE  */
    XMLPI = 858,                   /* XMLPI  */
    XMLROOT = 859,                 /* XMLROOT  */
    XMLSERIALIZE = 860,            /* XMLSERIALIZE  */
    XMLTABLE = 861,                /* XMLTABLE  */
    XOR = 862,                     /* XOR  */
    YEAR_P = 863,                  /* YEAR_P  */
    YES_P = 864,                   /* YES_P  */
    ZEROFILL = 865,                /* ZEROFILL  */
    ZONE = 866,                    /* ZONE  */
    NOT_LA = 867,                  /* NOT_LA  */
    NULLS_LA = 868,                /* NULLS_LA  */
    WITH_LA = 869,                 /* WITH_LA  */
    BINARY_LA = 870,               /* BINARY_LA  */
    MODE_TYPE_NAME = 871,          /* MODE_TYPE_NAME  */
    MODE_PLPGSQL_EXPR = 872,       /* MODE_PLPGSQL_EXPR  */
    MODE_PLPGSQL_ASSIGN1 = 873,    /* MODE_PLPGSQL_ASSIGN1  */
    MODE_PLPGSQL_ASSIGN2 = 874,    /* MODE_PLPGSQL_ASSIGN2  */
    MODE_PLPGSQL_ASSIGN3 = 875,    /* MODE_PLPGSQL_ASSIGN3  */
    KEYWORD_USED_AS_IDENT = 876,   /* KEYWORD_USED_AS_IDENT  */
    BINARY_P = 877,                /* BINARY_P  */
    UMINUS = 878                   /* UMINUS  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 394 "src/mys_gram.y"

	core_YYSTYPE		core_yystype;
	/* these fields must match core_YYSTYPE: */
	int					ival;
	char				*str;
	const char			*keyword;

	char				chr;
	bool				boolean;
	JoinType			jtype;
	DropBehavior		dbehavior;
	OnCommitAction		oncommit;
	List				*list;
	Node				*node;
	ObjectType			objtype;
	TypeName			*typnam;
	FunctionParameter   *fun_param;
	FunctionParameterMode fun_param_mode;
	ObjectWithArgs		*objwithargs;
	DefElem				*defelt;
	SortBy				*sortby;
	WindowDef			*windef;
	JoinExpr			*jexpr;
	IndexElem			*ielem;
	StatsElem			*selem;
	Alias				*alias;
	RangeVar			*range;
	IntoClause			*into;
	WithClause			*with;
	InferClause			*infer;
	OnConflictClause	*onconflict;
	A_Indices			*aind;
	ResTarget			*target;
	struct PrivTarget	*privtarget;
	AccessPriv			*accesspriv;
	struct ImportQual	*importqual;
	InsertStmt			*istmt;
	VariableSetStmt		*vsetstmt;
	PartitionElem		*partelem;
	PartitionSpec		*partspec;
	PartitionBoundSpec	*partboundspec;
	RoleSpec			*rolespec;
	struct SelectLimit	*selectlimit;
	SetQuantifier	 setquantifier;
	struct GroupClause  *groupclause;
	ReturningClause		*retclause;

#line 735 "src/mys_gram.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE YYLTYPE;
struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif




int mys_yyparse (core_yyscan_t yyscanner);


#endif /* !YY_MYS_YY_SRC_MYS_GRAM_H_INCLUDED  */
