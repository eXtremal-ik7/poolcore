%code requires {
  #include "ast.h"
  typedef void *yyscan_t;

  struct CFieldAnnotation {
    int Tag;
    char *ContextField;
  };
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
  CIncludeDirective *includeDef;
  CExternTypeDef *externDef;
  CFieldType *fieldType;
  CDefaultValue *defaultVal;
  CFieldAnnotation *annotation;
  std::vector<std::variant<CMixinRef, CFieldDef>> *memberList;
  std::vector<CFieldDef> *fieldList;
  std::vector<std::string> *stringList;
}

%token TOK_STRUCT TOK_MIXIN TOK_ENUM TOK_TRUE TOK_FALSE TOK_NOW TOK_INCLUDE
%token TOK_EXTERN TOK_TYPE TOK_CONTEXT
%token <strVal> TOK_IDENTIFIER TOK_STRING_LITERAL
%token <strVal> TOK_CHRONO_SECONDS TOK_CHRONO_MINUTES TOK_CHRONO_HOURS
%token <intVal> TOK_INT_LITERAL
%token <floatVal> TOK_FLOAT_LITERAL

%type <structDef> struct_def mixin_def
%type <enumDef> enum_def
%type <includeDef> include_directive
%type <externDef> extern_def
%type <fieldDef> field
%type <fieldType> type_spec
%type <defaultVal> default_value
%type <annotation> field_annotations
%type <strVal> type_name field_name
%type <memberList> member_list
%type <fieldList> mixin_field_list
%type <stringList> enum_values

%destructor { free($$); } <strVal>
%destructor { delete $$; } <structDef> <enumDef> <includeDef> <externDef> <fieldDef> <fieldType> <defaultVal> <memberList> <fieldList> <stringList>
%destructor { if ($$->ContextField) free($$->ContextField); delete $$; } <annotation>

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
  | include_directive {
      file->Includes.push_back(std::move(*$1));
      delete $1;
    }
  | extern_def {
      file->ExternTypes.push_back(std::move(*$1));
      delete $1;
    }
  ;

include_directive:
    TOK_INCLUDE TOK_STRING_LITERAL ';' {
      $$ = new CIncludeDirective();
      $$->Path = $2;
      $$->Line = @1.first_line;
      free($2);
    }
  ;

extern_def:
    TOK_EXTERN TOK_TYPE TOK_IDENTIFIER ':' TOK_IDENTIFIER TOK_INCLUDE TOK_STRING_LITERAL ';' {
      $$ = new CExternTypeDef();
      $$->Name = $3;
      $$->JsonWireType = $5;
      $$->IncludePath = $7;
      $$->Line = @1.first_line;
      free($3);
      free($5);
      free($7);
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

field_annotations:
    /* empty */ {
      $$ = new CFieldAnnotation();
      $$->Tag = 0;
      $$->ContextField = nullptr;
    }
  | field_annotations '@' TOK_INT_LITERAL {
      $$ = $1;
      $$->Tag = (int)$3;
    }
  | field_annotations TOK_CONTEXT '(' type_name ')' {
      $$ = $1;
      if ($$->ContextField) free($$->ContextField);
      $$->ContextField = $4;
    }
  ;

field_name:
    TOK_IDENTIFIER { $$ = $1; }
  | TOK_TYPE       { $$ = strdup("type"); }
  | TOK_CONTEXT    { $$ = strdup("context"); }
  | TOK_EXTERN     { $$ = strdup("extern"); }
  ;

field:
    field_name ':' type_spec field_annotations ';' {
      $$ = new CFieldDef();
      $$->Name = $1;
      $$->Type = *$3;
      $$->Tag = $4->Tag;
      if ($4->ContextField) { $$->ContextField = $4->ContextField; $4->ContextField = nullptr; }
      $$->Line = @1.first_line;
      switch ($3->Shape) {
        case ETypeShape::Plain:          $$->Kind = EFieldKind::Required; break;
        case ETypeShape::OptionalObject: $$->Kind = EFieldKind::OptionalObject; break;
        case ETypeShape::Array:          $$->Kind = EFieldKind::Array; break;
        case ETypeShape::OptionalArray:  $$->Kind = EFieldKind::OptionalArray; break;
      }
      free($1);
      delete $3;
      delete $4;
    }
  | field_name ':' type_spec '=' default_value field_annotations ';' {
      $$ = new CFieldDef();
      $$->Name = $1;
      $$->Type = *$3;
      $$->Default = *$5;
      $$->Tag = $6->Tag;
      if ($6->ContextField) { $$->ContextField = $6->ContextField; $6->ContextField = nullptr; }
      $$->Kind = EFieldKind::Optional;
      $$->Line = @1.first_line;
      free($1);
      delete $3;
      delete $5;
      delete $6;
    }
  ;

type_name:
    TOK_IDENTIFIER      { $$ = $1; }
  | TOK_CHRONO_SECONDS  { $$ = $1; }
  | TOK_CHRONO_MINUTES  { $$ = $1; }
  | TOK_CHRONO_HOURS    { $$ = $1; }
  ;

type_spec:
    type_name {
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
  | type_name '?' {
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
  | '[' type_name ']' {
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
  | '[' type_name ']' '?' {
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
