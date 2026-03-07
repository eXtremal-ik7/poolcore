%{
#include "ast.h"
#include "parser.h"
#include <cstring>
#include <cstdlib>
#include <string>
%}

%option reentrant bison-bridge bison-locations
%option noyywrap nounput
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
"include"               { return TOK_INCLUDE; }
"mapped"                { return TOK_MAPPED; }
"type"                  { return TOK_TYPE; }
"context"               { return TOK_CONTEXT; }
"generate"              { return TOK_GENERATE; }
"variant"               { return TOK_VARIANT; }

"cpp" {
    int c;
    while ((c = yyinput(yyscanner)) > 0 && c != '{') {}
    if (c != '{') { fprintf(stderr, "lexer error at line %d: expected '{' after 'cpp'\n", yylineno); return 0; }
    int depth = 1;
    std::string buf;
    while (depth > 0) {
        c = yyinput(yyscanner);
        if (c <= 0) { fprintf(stderr, "lexer error at line %d: unterminated cpp block\n", yylineno); break; }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) break; }
        buf += (char)c;
    }
    // Dedent: strip common leading whitespace, trim leading/trailing blank lines
    size_t minIndent = SIZE_MAX;
    size_t pos = 0;
    while (pos < buf.size()) {
        size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos) eol = buf.size();
        size_t lineStart = pos;
        size_t indent = 0;
        while (lineStart + indent < eol && (buf[lineStart + indent] == ' ' || buf[lineStart + indent] == '\t'))
            indent++;
        if (lineStart + indent < eol) // non-empty line
            minIndent = std::min(minIndent, indent);
        pos = eol + 1;
    }
    if (minIndent == SIZE_MAX) minIndent = 0;
    std::string result;
    pos = 0;
    while (pos < buf.size()) {
        size_t eol = buf.find('\n', pos);
        if (eol == std::string::npos) eol = buf.size();
        size_t skip = std::min(minIndent, eol - pos);
        result.append(buf, pos + skip, eol - pos - skip);
        result += '\n';
        pos = eol + 1;
    }
    // Trim leading/trailing blank lines
    size_t start = result.find_first_not_of(" \t\r\n");
    size_t end = result.find_last_not_of(" \t\r\n");
    if (start != std::string::npos)
        result = result.substr(start, end - start + 1);
    else
        result.clear();
    yylval->strVal = strdup(result.c_str());
    return TOK_CPP_BLOCK;
}

"chrono::seconds"       { yylval->strVal = strdup("chrono::seconds"); return TOK_CHRONO_SECONDS; }
"chrono::minutes"       { yylval->strVal = strdup("chrono::minutes"); return TOK_CHRONO_MINUTES; }
"chrono::hours"         { yylval->strVal = strdup("chrono::hours"); return TOK_CHRONO_HOURS; }

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
"."                     { return '.'; }
"|"                     { return '|'; }

.                       { fprintf(stderr, "lexer error at line %d: unexpected '%s'\n", yylineno, yytext); }

%%
