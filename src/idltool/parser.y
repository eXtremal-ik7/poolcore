%code requires {
  #include "ast.h"
  typedef void *yyscan_t;
}

%code {
  #include "lexer.h"
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  void yyerror(YYLTYPE *yylloc, CIdlFile *file, yyscan_t scanner, const char *msg);
}

%define api.pure full
%locations
%parse-param { CIdlFile *file }
%parse-param { yyscan_t scanner }
%lex-param { yyscan_t scanner }

%union {
  char *strVal;
  int64_t intVal;
  double floatVal;
  CStructDef *structDef;
  CFieldDef *fieldDef;
  CEnumDef *enumDef;
  CFieldType *fieldType;
  CDefaultValue *defaultVal;
  std::vector<std::variant<CMixinRef, CFieldDef>> *memberList;
  std::vector<CFieldDef> *fieldList;
  std::vector<std::string> *stringList;
}

%token TOK_STRUCT TOK_MIXIN TOK_ENUM TOK_TRUE TOK_FALSE TOK_NOW
%token <strVal> TOK_IDENTIFIER TOK_STRING_LITERAL
%token <intVal> TOK_INT_LITERAL
%token <floatVal> TOK_FLOAT_LITERAL

%type <structDef> struct_def mixin_def
%type <enumDef> enum_def
%type <fieldDef> field
%type <fieldType> type_spec
%type <defaultVal> default_value
%type <memberList> member_list
%type <fieldList> mixin_field_list
%type <stringList> enum_values

%destructor { free($$); } <strVal>
%destructor { delete $$; } <structDef> <enumDef> <fieldDef> <fieldType> <defaultVal> <memberList> <fieldList> <stringList>

%%

file:
    /* empty */
  | file definition
  ;

definition:
    struct_def {
      file->Structs.push_back(std::move(*$1));
      delete $1;
    }
  | mixin_def {
      file->Structs.push_back(std::move(*$1));
      delete $1;
    }
  | enum_def {
      file->Enums.push_back(std::move(*$1));
      delete $1;
    }
  ;

struct_def:
    TOK_STRUCT TOK_IDENTIFIER '{' member_list '}' {
      $$ = new CStructDef();
      $$->Name = $2;
      $$->IsMixin = false;
      $$->Members = std::move(*$4);
      $$->Line = @1.first_line;
      free($2);
      delete $4;
    }
  ;

mixin_def:
    TOK_MIXIN TOK_IDENTIFIER '{' mixin_field_list '}' {
      $$ = new CStructDef();
      $$->Name = $2;
      $$->IsMixin = true;
      for (auto &f : *$4)
        $$->Members.push_back(std::move(f));
      $$->Line = @1.first_line;
      free($2);
      delete $4;
    }
  ;

enum_def:
    TOK_ENUM TOK_IDENTIFIER ':' TOK_IDENTIFIER '{' enum_values '}' {
      $$ = new CEnumDef();
      $$->Name = $2;
      $$->BaseType = $4;
      $$->Values = std::move(*$6);
      $$->Line = @1.first_line;
      free($2);
      free($4);
      delete $6;
    }
  ;

member_list:
    /* empty */ {
      $$ = new std::vector<std::variant<CMixinRef, CFieldDef>>();
    }
  | member_list TOK_MIXIN TOK_IDENTIFIER ';' {
      $$ = $1;
      CMixinRef ref;
      ref.Name = $3;
      ref.Line = @2.first_line;
      $$->push_back(std::move(ref));
      free($3);
    }
  | member_list field {
      $$ = $1;
      $$->push_back(std::move(*$2));
      delete $2;
    }
  ;

mixin_field_list:
    /* empty */ {
      $$ = new std::vector<CFieldDef>();
    }
  | mixin_field_list field {
      $$ = $1;
      $$->push_back(std::move(*$2));
      delete $2;
    }
  ;

field:
    TOK_IDENTIFIER ':' type_spec ';' {
      $$ = new CFieldDef();
      $$->Name = $1;
      $$->Type = *$3;
      $$->Line = @1.first_line;
      switch ($3->Shape) {
        case ETypeShape::Plain:          $$->Kind = EFieldKind::Required; break;
        case ETypeShape::OptionalObject: $$->Kind = EFieldKind::OptionalObject; break;
        case ETypeShape::Array:          $$->Kind = EFieldKind::Array; break;
        case ETypeShape::OptionalArray:  $$->Kind = EFieldKind::OptionalArray; break;
      }
      free($1);
      delete $3;
    }
  | TOK_IDENTIFIER ':' type_spec '=' default_value ';' {
      $$ = new CFieldDef();
      $$->Name = $1;
      $$->Type = *$3;
      $$->Default = *$5;
      $$->Kind = EFieldKind::Optional;
      $$->Line = @1.first_line;
      free($1);
      delete $3;
      delete $5;
    }
  ;

type_spec:
    TOK_IDENTIFIER {
      $$ = new CFieldType();
      $$->Shape = ETypeShape::Plain;
      auto scalar = parseScalarType($1);
      if (scalar) {
        $$->IsScalar = true;
        $$->Scalar = *scalar;
      } else {
        $$->RefName = $1;
      }
      free($1);
    }
  | TOK_IDENTIFIER '?' {
      $$ = new CFieldType();
      $$->Shape = ETypeShape::OptionalObject;
      auto scalar = parseScalarType($1);
      if (scalar) {
        $$->IsScalar = true;
        $$->Scalar = *scalar;
      } else {
        $$->RefName = $1;
      }
      free($1);
    }
  | '[' TOK_IDENTIFIER ']' {
      $$ = new CFieldType();
      $$->Shape = ETypeShape::Array;
      auto scalar = parseScalarType($2);
      if (scalar) {
        $$->IsScalar = true;
        $$->Scalar = *scalar;
      } else {
        $$->RefName = $2;
      }
      free($2);
    }
  | '[' TOK_IDENTIFIER ']' '?' {
      $$ = new CFieldType();
      $$->Shape = ETypeShape::OptionalArray;
      auto scalar = parseScalarType($2);
      if (scalar) {
        $$->IsScalar = true;
        $$->Scalar = *scalar;
      } else {
        $$->RefName = $2;
      }
      free($2);
    }
  ;

default_value:
    TOK_STRING_LITERAL {
      $$ = new CDefaultValue();
      $$->Kind = EDefaultKind::String;
      $$->StringVal = $1;
      free($1);
    }
  | TOK_INT_LITERAL {
      $$ = new CDefaultValue();
      $$->Kind = EDefaultKind::Int;
      $$->IntVal = $1;
    }
  | '-' TOK_INT_LITERAL {
      $$ = new CDefaultValue();
      $$->Kind = EDefaultKind::Int;
      $$->IntVal = -$2;
    }
  | TOK_FLOAT_LITERAL {
      $$ = new CDefaultValue();
      $$->Kind = EDefaultKind::Float;
      $$->FloatVal = $1;
    }
  | TOK_TRUE {
      $$ = new CDefaultValue();
      $$->Kind = EDefaultKind::Bool;
      $$->BoolVal = true;
    }
  | TOK_FALSE {
      $$ = new CDefaultValue();
      $$->Kind = EDefaultKind::Bool;
      $$->BoolVal = false;
    }
  | '@' TOK_NOW {
      $$ = new CDefaultValue();
      $$->Kind = EDefaultKind::RuntimeNow;
    }
  | '@' '(' TOK_NOW '-' TOK_INT_LITERAL ')' {
      $$ = new CDefaultValue();
      $$->Kind = EDefaultKind::RuntimeNowOffset;
      $$->IntVal = $5;
    }
  ;

enum_values:
    TOK_IDENTIFIER {
      $$ = new std::vector<std::string>();
      $$->push_back($1);
      free($1);
    }
  | enum_values ',' TOK_IDENTIFIER {
      $$ = $1;
      $$->push_back($3);
      free($3);
    }
  ;

%%

void yyerror(YYLTYPE *yylloc, CIdlFile *file, yyscan_t scanner, const char *msg) {
  (void)file;
  (void)scanner;
  fprintf(stderr, "parse error at line %d: %s\n", yylloc->first_line, msg);
}
