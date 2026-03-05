%{
#include "ast.h"
#include "parser.h"
#include <cstring>
#include <cstdlib>
%}

%option reentrant bison-bridge bison-locations
%option noyywrap nounput noinput
%option yylineno

%%

[ \t\r]+                ;
\n                      ;
"//"[^\n]*              ;

"struct"                { return TOK_STRUCT; }
"mixin"                 { return TOK_MIXIN; }
"enum"                  { return TOK_ENUM; }
"true"                  { return TOK_TRUE; }
"false"                 { return TOK_FALSE; }
"now"                   { return TOK_NOW; }

[a-zA-Z_][a-zA-Z0-9_]* {
  yylval->strVal = strdup(yytext);
  return TOK_IDENTIFIER;
}

\"([^"\\]|\\.)*\" {
  // Strip quotes, copy inner content
  size_t len = yyleng - 2;
  yylval->strVal = (char *)malloc(len + 1);
  memcpy(yylval->strVal, yytext + 1, len);
  yylval->strVal[len] = '\0';
  return TOK_STRING_LITERAL;
}

[0-9]+\.[0-9]+ {
  yylval->floatVal = atof(yytext);
  return TOK_FLOAT_LITERAL;
}

[0-9]+ {
  yylval->intVal = (int64_t)atoll(yytext);
  return TOK_INT_LITERAL;
}

"{"                     { return '{'; }
"}"                     { return '}'; }
";"                     { return ';'; }
":"                     { return ':'; }
"="                     { return '='; }
","                     { return ','; }
"["                     { return '['; }
"]"                     { return ']'; }
"?"                     { return '?'; }
"("                     { return '('; }
")"                     { return ')'; }
"-"                     { return '-'; }
"@"                     { return '@'; }

.                       { fprintf(stderr, "lexer error at line %d: unexpected '%s'\n", yylineno, yytext); }

%%
