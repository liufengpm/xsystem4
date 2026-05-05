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

#ifndef YY_YINI_F_MYPROJECT_VINTAGE_POMELO_XSYSTEM4_BUILD_HARMONY_ARM64_SUBPROJECTS_LIBSYS4_INI_PARSER_TAB_H_INCLUDED
# define YY_YINI_F_MYPROJECT_VINTAGE_POMELO_XSYSTEM4_BUILD_HARMONY_ARM64_SUBPROJECTS_LIBSYS4_INI_PARSER_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YINI_DEBUG
# if defined YYDEBUG
#if YYDEBUG
#   define YINI_DEBUG 1
#  else
#   define YINI_DEBUG 0
#  endif
# else /* ! defined YYDEBUG */
#  define YINI_DEBUG 0
# endif /* ! defined YYDEBUG */
#endif  /* ! defined YINI_DEBUG */
#if YINI_DEBUG
extern int yini_debug;
#endif
/* "%code requires" blocks.  */
#line 14 "F:/MyProject/vintage-pomelo/xsystem4/subprojects/libsys4/src/ini_parser.y"

    #include "kvec.h"
    #include "system4/ini.h"

    kv_decl(entry_list, struct ini_entry*);
    kv_decl(value_list, struct ini_value);
    extern entry_list *yini_entries;

#line 66 "F:/MyProject/vintage-pomelo/xsystem4/build_harmony_arm64/subprojects/libsys4/ini_parser.tab.h"

/* Token kinds.  */
#ifndef YINI_TOKENTYPE
# define YINI_TOKENTYPE
  enum yini_tokentype
  {
    YINI_EMPTY = -2,
    YINI_EOF = 0,                  /* "end of file"  */
    YINI_error = 256,              /* error  */
    YINI_UNDEF = 257,              /* "invalid token"  */
    TRUE = 258,                    /* TRUE  */
    FALSE = 259,                   /* FALSE  */
    FORMATION = 260,               /* FORMATION  */
    INTEGER = 261,                 /* INTEGER  */
    FLOAT = 262,                   /* FLOAT  */
    STRING = 263,                  /* STRING  */
    IDENTIFIER = 264               /* IDENTIFIER  */
  };
  typedef enum yini_tokentype yini_token_kind_t;
#endif

/* Value type.  */
#if ! defined YINI_STYPE && ! defined YINI_STYPE_IS_DECLARED
union YINI_STYPE
{
#line 3 "F:/MyProject/vintage-pomelo/xsystem4/subprojects/libsys4/src/ini_parser.y"

    int token;
    int i;
    float f;
    struct string *s;
    struct ini_entry *entry;
    struct ini_value value;
    entry_list *entries;
    value_list *list;

#line 103 "F:/MyProject/vintage-pomelo/xsystem4/build_harmony_arm64/subprojects/libsys4/ini_parser.tab.h"

};
typedef union YINI_STYPE YINI_STYPE;
# define YINI_STYPE_IS_TRIVIAL 1
# define YINI_STYPE_IS_DECLARED 1
#endif


extern YINI_STYPE yini_lval;


int yini_parse (void);


#endif /* !YY_YINI_F_MYPROJECT_VINTAGE_POMELO_XSYSTEM4_BUILD_HARMONY_ARM64_SUBPROJECTS_LIBSYS4_INI_PARSER_TAB_H_INCLUDED  */
