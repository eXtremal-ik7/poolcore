%code requires {
  #include "ast.h"
  typedef void *yyscan_t;
}

%code {
  #include "lexer.h"
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
  #include <utility>

  void yyerror(YYLTYPE *yylloc, CIdlFile *file, yyscan_t scanner, const char *msg);

  static CVariantAlt makeVariantAlt(CFieldType *type) {
    CVariantAlt alt;
    alt.IsScalar = type->IsScalar;
    alt.Scalar = type->Scalar;
    alt.RefName = std::move(type->RefName);
    alt.IsMapped = type->IsMapped;
    alt.MappedCppType = std::move(type->MappedCppType);
    alt.MappedWireType = std::move(type->MappedWireType);
    alt.Dims = std::move(type->InnerDims);
    return alt;
  }

  static CFieldType *wrapOptionalType(CFieldType *type,
                                      const COptionalPolicySpec &policy,
                                      YYLTYPE *yylloc,
                                      CIdlFile *file,
                                      yyscan_t scanner) {
    switch (type->Shape) {
      case ETypeShape::Plain:
        type->Shape = ETypeShape::OptionalObject;
        break;
      case ETypeShape::Array:
        type->Shape = ETypeShape::OptionalArray;
        break;
      case ETypeShape::FixedArray:
        type->Shape = ETypeShape::OptionalFixedArray;
        break;
      case ETypeShape::Variant:
        type->Shape = ETypeShape::OptionalVariant;
        break;
      default:
        yyerror(yylloc, file, scanner, "optional<T> expects a plain, array, fixed array, or variant type");
        return nullptr;
    }
    type->NullIn = policy.NullIn;
    type->EmptyOut = policy.EmptyOut;
    return type;
  }

  static bool setOptionalPolicy(COptionalPolicySpec &policy,
                                const std::string &name,
                                const std::string &value,
                                YYLTYPE *yylloc,
                                CIdlFile *file,
                                yyscan_t scanner) {
    if (name == "null_in") {
      if (policy.HasNullIn) {
        yyerror(yylloc, file, scanner, "duplicate optional<T> policy 'null_in'");
        return false;
      }
      policy.HasNullIn = true;
      if (value == "deny") policy.NullIn = ENullInPolicy::Deny;
      else if (value == "allow") policy.NullIn = ENullInPolicy::Allow;
      else if (value == "required") policy.NullIn = ENullInPolicy::Required;
      else {
        yyerror(yylloc, file, scanner, "invalid value for null_in; expected deny, allow, or required");
        return false;
      }
      return true;
    }

    if (name == "empty_out") {
      if (policy.HasEmptyOut) {
        yyerror(yylloc, file, scanner, "duplicate optional<T> policy 'empty_out'");
        return false;
      }
      policy.HasEmptyOut = true;
      if (value == "omit") policy.EmptyOut = EEmptyOutPolicy::Omit;
      else if (value == "null") policy.EmptyOut = EEmptyOutPolicy::Null;
      else {
        yyerror(yylloc, file, scanner, "invalid value for empty_out; expected omit or null");
        return false;
      }
      return true;
    }

    yyerror(yylloc, file, scanner, ("unknown optional<T> policy '" + name + "'").c_str());
    return false;
  }
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
  CMappedTypeDef *mappedDef;
  CFieldType *fieldType;
  COptionalPolicySpec *optionalPolicy;
  CDefaultValue *defaultVal;
  int tagVal;
  std::vector<std::variant<CMixinRef, CFieldDef, CCtxGroupDecl, CCppBlock, CGenerateDecl>> *memberList;
  std::vector<CFieldDef> *fieldList;
  std::vector<std::string> *stringList;
  std::vector<CVariantAlt> *variantAlts;
}

%token TOK_STRUCT TOK_MIXIN TOK_ENUM TOK_TRUE TOK_FALSE TOK_NOW TOK_INCLUDE
%token TOK_MAPPED TOK_TYPE TOK_CONTEXT TOK_GENERATE TOK_VARIANT TOK_CTXGROUP
%token TOK_OPTIONAL
%token <strVal> TOK_CPP_BLOCK
%token <strVal> TOK_IDENTIFIER TOK_STRING_LITERAL
%token <strVal> TOK_CHRONO_SECONDS TOK_CHRONO_MINUTES TOK_CHRONO_HOURS
%token <intVal> TOK_INT_LITERAL
%token <floatVal> TOK_FLOAT_LITERAL

%type <structDef> struct_def mixin_def
%type <enumDef> enum_def
%type <includeDef> include_directive
%type <mappedDef> mapped_def
%type <fieldDef> field
%type <fieldType> type_spec base_type_spec array_elem
%type <optionalPolicy> optional_policy_list optional_policy_clause
%type <defaultVal> default_value
%type <tagVal> field_tag
%type <strVal> type_name field_name
%type <memberList> member_list
%type <fieldList> mixin_field_list
%type <stringList> enum_values generate_flag_list field_name_list
%type <strVal> generate_flag
%type <variantAlts> variant_alts variant_tail

%destructor { free($$); } <strVal>
%destructor { delete $$; } <structDef> <enumDef> <includeDef> <mappedDef> <fieldDef> <fieldType> <optionalPolicy> <defaultVal> <memberList> <fieldList> <stringList> <variantAlts>

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
  | mapped_def {
      file->MappedTypes.push_back(std::move(*$1));
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

mapped_def:
    TOK_MAPPED TOK_TYPE TOK_IDENTIFIER '(' TOK_STRING_LITERAL ')' ':' TOK_IDENTIFIER TOK_INCLUDE TOK_STRING_LITERAL ';' {
      $$ = new CMappedTypeDef();
      $$->Name = $3;
      $$->CppType = $5;
      $$->JsonWireType = $8;
      $$->IncludePath = $10;
      $$->Line = @1.first_line;
      free($3);
      free($5);
      free($8);
      free($10);
    }
  | TOK_MAPPED TOK_TYPE TOK_IDENTIFIER '(' TOK_STRING_LITERAL ')' ':' TOK_IDENTIFIER TOK_CONTEXT '(' type_name ')' TOK_INCLUDE TOK_STRING_LITERAL ';' {
      $$ = new CMappedTypeDef();
      $$->Name = $3;
      $$->CppType = $5;
      $$->JsonWireType = $8;
      $$->IncludePath = $14;
      $$->Line = @1.first_line;
      auto ct = parseScalarType($11);
      if (ct) {
        $$->ContextType = *ct;
      } else {
        yyerror(&@11, file, scanner, "invalid context type");
        YYERROR;
      }
      free($3);
      free($5);
      free($8);
      free($11);
      free($14);
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
      $$ = new std::vector<std::variant<CMixinRef, CFieldDef, CCtxGroupDecl, CCppBlock, CGenerateDecl>>();
    }
  | member_list TOK_MIXIN TOK_IDENTIFIER ';' {
      $$ = $1;
      CMixinRef ref;
      ref.Name = $3;
      ref.Line = @2.first_line;
      $$->push_back(std::move(ref));
      free($3);
    }
  | member_list '.' TOK_CTXGROUP '(' field_name_list ')' ';' {
      $$ = $1;
      CCtxGroupDecl cg;
      cg.FieldNames = std::move(*$5);
      cg.Line = @2.first_line;
      $$->push_back(std::move(cg));
      delete $5;
    }
  | member_list '.' TOK_GENERATE '(' generate_flag_list ')' ';' {
      $$ = $1;
      CGenerateDecl gd;
      gd.Line = @2.first_line;
      for (auto &flag : *$5) {
        if (flag == "parse") gd.Parse = true;
        else if (flag == "parse.verbose") gd.ParseVerbose = true;
        else if (flag == "serialize") gd.Serialize = true;
        else if (flag == "serialize.flat") gd.SerializeFlat = true;
        else { yyerror(&@5, file, scanner,
               ("unknown generate flag: " + flag).c_str()); YYERROR; }
      }
      $$->push_back(std::move(gd));
      delete $5;
    }
  | member_list field {
      $$ = $1;
      $$->push_back(std::move(*$2));
      delete $2;
    }
  | member_list TOK_CPP_BLOCK {
      $$ = $1;
      CCppBlock blk;
      blk.Code = $2;
      blk.Line = @2.first_line;
      $$->push_back(std::move(blk));
      free($2);
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

field_tag:
    /* empty */ { $$ = 0; }
  | field_tag '@' TOK_INT_LITERAL { $$ = (int)$3; }
  ;

field_name:
    TOK_IDENTIFIER { $$ = $1; }
  | TOK_TYPE       { $$ = strdup("type"); }
  | TOK_CONTEXT    { $$ = strdup("context"); }
  | TOK_CTXGROUP   { $$ = strdup("ctxgroup"); }
  | TOK_MAPPED     { $$ = strdup("mapped"); }
  | TOK_GENERATE   { $$ = strdup("generate"); }
  | TOK_VARIANT    { $$ = strdup("variant"); }
  ;

field_name_list:
    field_name {
      $$ = new std::vector<std::string>();
      $$->push_back($1);
      free($1);
    }
  | field_name_list ',' field_name {
      $$ = $1;
      $$->push_back($3);
      free($3);
    }
  ;

field:
    field_name ':' type_spec field_tag ';' {
      $$ = new CFieldDef();
      $$->Name = $1;
      $$->Type = *$3;
      $$->NullIn = $3->NullIn;
      $$->EmptyOut = $3->EmptyOut;
      $$->Tag = $4;
      $$->Line = @1.first_line;
      switch ($3->Shape) {
        case ETypeShape::Plain:              $$->Kind = EFieldKind::Required; break;
        case ETypeShape::OptionalObject:     $$->Kind = EFieldKind::OptionalObject; break;
        case ETypeShape::Array:              $$->Kind = EFieldKind::Array; break;
        case ETypeShape::OptionalArray:      $$->Kind = EFieldKind::OptionalArray; break;
        case ETypeShape::FixedArray:         $$->Kind = EFieldKind::FixedArray; break;
        case ETypeShape::OptionalFixedArray: $$->Kind = EFieldKind::OptionalFixedArray; break;
        case ETypeShape::Variant:            $$->Kind = EFieldKind::Variant; break;
        case ETypeShape::OptionalVariant:    $$->Kind = EFieldKind::OptionalVariant; break;
      }
      free($1);
      delete $3;
    }
  | field_name ':' type_spec '=' default_value field_tag ';' {
      if ($3->Shape != ETypeShape::Plain) {
        yyerror(&@3, file, scanner, "defaults are only supported for plain fields");
        YYERROR;
      }
      $$ = new CFieldDef();
      $$->Name = $1;
      $$->Type = *$3;
      $$->Default = *$5;
      $$->Tag = $6;
      $$->Kind = EFieldKind::Optional;
      $$->Line = @1.first_line;
      free($1);
      delete $3;
      delete $5;
    }
  ;

type_name:
    TOK_IDENTIFIER      { $$ = $1; }
  | TOK_CHRONO_SECONDS  { $$ = $1; }
  | TOK_CHRONO_MINUTES  { $$ = $1; }
  | TOK_CHRONO_HOURS    { $$ = $1; }
  ;

array_elem:
    type_name {
      $$ = new CFieldType();
      auto scalar = parseScalarType($1);
      if (scalar) { $$->IsScalar = true; $$->Scalar = *scalar; }
      else { $$->RefName = $1; }
      free($1);
    }
  | '[' array_elem ']' {
      $$ = $2;
      $$->InnerDims.insert($$->InnerDims.begin(), CArrayDim{0});
    }
  | '[' array_elem ';' TOK_INT_LITERAL ']' {
      $$ = $2;
      $$->InnerDims.insert($$->InnerDims.begin(), CArrayDim{(int)$4});
    }
  ;

variant_alts:
    array_elem ',' variant_tail {
      $$ = new std::vector<CVariantAlt>();
      CVariantAlt first = makeVariantAlt($1);
      $$->push_back(std::move(first));
      for (auto &a : *$3) $$->push_back(std::move(a));
      delete $1;
      delete $3;
    }
  ;

variant_tail:
    array_elem {
      $$ = new std::vector<CVariantAlt>();
      CVariantAlt alt = makeVariantAlt($1);
      $$->push_back(std::move(alt));
      delete $1;
    }
  | variant_tail ',' array_elem {
      $$ = $1;
      CVariantAlt alt = makeVariantAlt($3);
      $$->push_back(std::move(alt));
      delete $3;
    }
  ;

base_type_spec:
    /* Plain scalar/struct/enum */
    type_name {
      $$ = new CFieldType();
      $$->Shape = ETypeShape::Plain;
      auto scalar = parseScalarType($1);
      if (scalar) { $$->IsScalar = true; $$->Scalar = *scalar; }
      else { $$->RefName = $1; }
      free($1);
    }
    /* Variant: variant(T1, T2, ...) */
  | TOK_VARIANT '(' variant_alts ')' {
      $$ = new CFieldType();
      $$->Shape = ETypeShape::Variant;
      for (auto &a : *$3) $$->Alternatives.push_back(std::move(a));
      delete $3;
    }
    /* Dynamic array */
  | '[' array_elem ']' {
      $$ = $2;
      $$->Shape = ETypeShape::Array;
    }
    /* Fixed-size array */
  | '[' array_elem ';' TOK_INT_LITERAL ']' {
      $$ = $2;
      $$->Shape = ETypeShape::FixedArray;
      $$->FixedSize = (int)$4;
    }
  ;

optional_policy_clause:
    /* empty */ {
      $$ = new COptionalPolicySpec();
    }
  | '(' optional_policy_list ')' {
      $$ = $2;
    }
  ;

optional_policy_list:
    TOK_IDENTIFIER '=' TOK_IDENTIFIER {
      $$ = new COptionalPolicySpec();
      if (!setOptionalPolicy(*$$, $1, $3, &@1, file, scanner))
        YYERROR;
      free($1);
      free($3);
    }
  | optional_policy_list ',' TOK_IDENTIFIER '=' TOK_IDENTIFIER {
      $$ = $1;
      if (!setOptionalPolicy(*$$, $3, $5, &@3, file, scanner))
        YYERROR;
      free($3);
      free($5);
    }
  ;

type_spec:
    base_type_spec {
      $$ = $1;
    }
  | TOK_OPTIONAL '<' base_type_spec '>' optional_policy_clause {
      $$ = wrapOptionalType($3, *$5, &@1, file, scanner);
      if (!$$)
        YYERROR;
      delete $5;
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

generate_flag:
    TOK_IDENTIFIER                        { $$ = $1; }
  | TOK_IDENTIFIER '.' TOK_IDENTIFIER    {
      size_t len = strlen($1) + 1 + strlen($3) + 1;
      $$ = (char*)malloc(len);
      snprintf($$, len, "%s.%s", $1, $3);
      free($1); free($3);
    }
  ;

generate_flag_list:
    generate_flag {
      $$ = new std::vector<std::string>();
      $$->push_back($1); free($1);
    }
  | generate_flag_list ',' generate_flag {
      $$ = $1;
      $$->push_back($3); free($3);
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
