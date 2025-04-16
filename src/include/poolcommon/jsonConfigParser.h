#pragma once
#include "rapidjson/document.h"
#include <optional>
#include <vector>

enum EErrorType {
  EOk = 0,
  ENotExists,
  ETypeMismatch
};

static inline void setErrorDescription(EErrorType error, EErrorType *errorAcc, const std::string &place, const char *name, const char *requiredType, std::string &errorDescription)
{
  *errorAcc = error;
  if (error == ENotExists) {
    errorDescription = (std::string)"Required parameter '" + name + "' does not exists at " + place;
  } else if (error == ETypeMismatch) {
    errorDescription = (std::string)"Type mismatch: parameter '" + place + " -> " + name + "' must be a " + requiredType;
  }
}

static inline void jsonParseString(const rapidjson::Value &value,
                                   const char *name,
                                   std::string &out,
                                   std::pair<bool, const std::string> &defaultValue,
                                   EErrorType *validAcc,
                                   const std::string &place,
                                   std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsString())
      out = value[name].GetString();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "string", errorDescription);
  } else if (defaultValue.first) {
    out = defaultValue.second;
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "string", errorDescription);
  }
}

static inline void jsonParseStringOptional(const rapidjson::Value &value,
    const char *name,
    std::optional<std::string> &out,
    EErrorType *validAcc,
    const std::string &place,
    std::string &errorDescription)
{
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsString())
      out = value[name].GetString();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "string", errorDescription);
  } else {
    out.reset();
  }
}

static inline void jsonParseBoolean(const rapidjson::Value &value,
                                    const char *name,
                                    bool *out,
                                    std::pair<bool, bool> defaultValue,
                                    EErrorType *validAcc,
                                    const std::string &place,
                                    std::string &errorDescription)
{
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsBool())
      *out = value[name].GetBool();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "boolean", errorDescription);
  } else if (defaultValue.first) {
    *out = defaultValue.second;
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "boolean", errorDescription);
  }
}

static inline void jsonParseBooleanOptional(const rapidjson::Value &value,
                                            const char *name,
                                            std::optional<bool> &out,
                                            EErrorType *validAcc,
                                            const std::string &place,
                                            std::string &errorDescription)
{
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsBool())
      *out = value[name].GetBool();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "boolean", errorDescription);
  } else {
    out.reset();
  }
}

static inline void jsonParseUnsignedInt(const rapidjson::Value &value,
                                        const char *name,
                                        unsigned *out,
                                        EErrorType *validAcc,
                                        const std::string &place,
                                        std::string &errorDescription)
{
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsUint())
      *out = value[name].GetUint();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "unsigned integer", errorDescription);
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "unsigned integer", errorDescription);
  }
}

static inline void jsonParseDouble(const rapidjson::Value &value,
                                   const char *name,
                                   double *out,
                                   EErrorType *validAcc,
                                   const std::string &place,
                                   std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsFloat())
      *out = value[name].GetDouble();
    else if (value[name].IsInt64())
      *out = value[name].GetInt64();
    else
      setErrorDescription(ETypeMismatch, validAcc, place, name, "floating point number (like 1.0)", errorDescription);
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "floating point number (like 1.0)", errorDescription);
  }
}

static inline void jsonParseStringArray(const rapidjson::Value &value, const char *name, std::vector<std::string> &out, EErrorType *validAcc, const std::string &place, std::string &errorDescription) {
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsArray()) {
      rapidjson::Value::ConstArray array = value[name].GetArray();
      for (rapidjson::SizeType i = 0; i < array.Size(); i++) {
        if (!array[i].IsString()) {
          setErrorDescription(ETypeMismatch, validAcc, place, name, "array of string", errorDescription);
          break;
        }

        out.emplace_back(array[i].GetString());
      }
    } else {
      setErrorDescription(ETypeMismatch, validAcc, place, name, "array of string", errorDescription);
    }
  } else {
    setErrorDescription(ENotExists, validAcc, place, name, "array of string", errorDescription);
  }
}

static inline void jsonParseStringArrayOptional(const rapidjson::Value &value,
                                                const char *name,
                                                std::optional<std::vector<std::string>> &out,
                                                EErrorType *validAcc,
                                                const std::string &place,
                                                std::string &errorDescription)
{
  if (*validAcc != EOk)
    return;

  if (value.HasMember(name)) {
    if (value[name].IsArray()) {
      out.emplace();
      rapidjson::Value::ConstArray array = value[name].GetArray();
      for (rapidjson::SizeType i = 0; i < array.Size(); i++) {
        if (!array[i].IsString()) {
          setErrorDescription(ETypeMismatch, validAcc, place, name, "array of string", errorDescription);
          break;
        }

        out.value().emplace_back(array[i].GetString());
      }
    } else {
      setErrorDescription(ETypeMismatch, validAcc, place, name, "array of string", errorDescription);
    }
  } else {
    out.reset();
  }
}
