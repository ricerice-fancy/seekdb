/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_file_scan_utils.h"
#include "lib/alloc/alloc_func.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

namespace oceanbase
{
namespace sql
{

common::ObObjType file_type_to_obj_type(ObFileSqlType type)
{
  switch (type) {
  case FILE_TYPE_NULL: return common::ObNullType;
  case FILE_TYPE_BOOLEAN: return common::ObTinyIntType;
  case FILE_TYPE_BIGINT: return common::ObIntType;
  case FILE_TYPE_DOUBLE: return common::ObDoubleType;
  case FILE_TYPE_DATE: return common::ObDateType;
  case FILE_TYPE_DATETIME: return common::ObDateTimeType;
  case FILE_TYPE_VARCHAR: return common::ObVarcharType;
  default: return common::ObVarcharType;
  }
}

const char *file_type_to_str(ObFileSqlType type)
{
  switch (type) {
  case FILE_TYPE_NULL: return "NULL";
  case FILE_TYPE_BOOLEAN: return "BOOLEAN";
  case FILE_TYPE_BIGINT: return "BIGINT";
  case FILE_TYPE_DOUBLE: return "DOUBLE";
  case FILE_TYPE_DATE: return "DATE";
  case FILE_TYPE_DATETIME: return "DATETIME";
  case FILE_TYPE_VARCHAR: return "VARCHAR";
  default: return "INVALID";
  }
}

namespace {
bool is_null_token(const common::ObString &str, const ObFileScanOptions &opt)
{
  return str.length() == 0
         || (!opt.null_if_.empty() && str.case_compare(opt.null_if_) == 0)
         || (str.length() == 4 && 0 == str.case_compare("NULL"));
}

bool is_integer_text(const common::ObString &str)
{
  bool valid = false;
  if (!str.empty()) {
    const char *p = str.ptr();
    int64_t i = 0;
    if (p[0] == '-' || p[0] == '+') { i++; }
    const int64_t start_digits = i;
    for (; i < str.length(); i++) {
      if (p[i] < '0' || p[i] > '9') { break; }
    }
    if (i == str.length() && i > start_digits) {
      // range check via strtoll
      char *end = NULL;
      errno = 0;
      (void)strtoll(p, &end, 10);
      valid = (errno != ERANGE) && (end == p + str.length());
    }
  }
  return valid;
}

bool is_double_text(const common::ObString &str)
{
  bool valid = false;
  if (!str.empty()) {
    char *end = NULL;
    errno = 0;
    (void)strtod(str.ptr(), &end);
    valid = (errno != ERANGE) && (end == str.ptr() + str.length());
  }
  return valid;
}

bool is_boolean_text(const common::ObString &str)
{
  static const char *TRUE_VALS[] = {"true", "1", "yes", "t", "y"};
  static const char *FALSE_VALS[] = {"false", "0", "no", "f", "n"};
  for (int i = 0; i < 5; i++) {
    if (0 == str.case_compare(TRUE_VALS[i]) || 0 == str.case_compare(FALSE_VALS[i])) {
      return true;
    }
  }
  return false;
}

bool is_date_like(const common::ObString &str)
{
  // yyyy-mm-dd or yyyy/mm/dd
  if (str.length() != 10) { return false; }
  const char *p = str.ptr();
  return ((p[4] == '-' && p[7] == '-') || (p[4] == '/' && p[7] == '/'));
}

bool is_datetime_like(const common::ObString &str)
{
  // yyyy-mm-dd HH:MM:SS[.fraction]
  return str.length() >= 19 && is_date_like(common::ObString(10, str.ptr()));
}
} // namespace

ObFileSqlType infer_value_type(const common::ObString &str)
{
  ObFileSqlType type = FILE_TYPE_NULL;
  if (!str.empty()) {
    // NOTE: null-token handling is done at the caller (row materialization);
    // here a non-empty string is inferred by its syntactic shape.
    if (is_boolean_text(str)) {
      // "1"/"0" could be BIGINT; narrow boolean only for textual bool tokens.
      if (str.length() == 1 && (str[0] == '1' || str[0] == '0')) {
        type = FILE_TYPE_BIGINT; // prefer numeric for 0/1, widening handles mixes
      } else {
        type = FILE_TYPE_BOOLEAN;
      }
    } else if (is_integer_text(str)) {
      type = FILE_TYPE_BIGINT;
    } else if (is_double_text(str)) {
      type = FILE_TYPE_DOUBLE;
    } else if (is_datetime_like(str)) {
      type = FILE_TYPE_DATETIME;
    } else if (is_date_like(str)) {
      type = FILE_TYPE_DATE;
    } else {
      type = FILE_TYPE_VARCHAR;
    }
  }
  return type;
}

ObFileSqlType widen_file_type(ObFileSqlType lhs, ObFileSqlType rhs)
{
  // Ordering: NULL < BOOLEAN < BIGINT < DOUBLE < DATE < DATETIME < VARCHAR.
  // DATE/DATETIME vs numeric widen to VARCHAR (no common numeric type).
  if (lhs == rhs) { return lhs; }
  if (lhs == FILE_TYPE_NULL) { return rhs; }
  if (rhs == FILE_TYPE_NULL) { return lhs; }
  if ((lhs == FILE_TYPE_DATE || lhs == FILE_TYPE_DATETIME)
      && (rhs == FILE_TYPE_DATE || rhs == FILE_TYPE_DATETIME)) {
    return FILE_TYPE_DATETIME;
  }
  if (lhs == FILE_TYPE_DATE || lhs == FILE_TYPE_DATETIME
      || rhs == FILE_TYPE_DATE || rhs == FILE_TYPE_DATETIME) {
    return FILE_TYPE_VARCHAR;
  }
  if ((lhs == FILE_TYPE_BOOLEAN && rhs == FILE_TYPE_BIGINT)
      || (lhs == FILE_TYPE_BIGINT && rhs == FILE_TYPE_BOOLEAN)) {
    return FILE_TYPE_BIGINT;
  }
  if (lhs == FILE_TYPE_BOOLEAN && rhs == FILE_TYPE_DOUBLE) { return FILE_TYPE_DOUBLE; }
  if (lhs == FILE_TYPE_DOUBLE && rhs == FILE_TYPE_BOOLEAN) { return FILE_TYPE_DOUBLE; }
  if (lhs == FILE_TYPE_BIGINT && rhs == FILE_TYPE_DOUBLE) { return FILE_TYPE_DOUBLE; }
  if (lhs == FILE_TYPE_DOUBLE && rhs == FILE_TYPE_BIGINT) { return FILE_TYPE_DOUBLE; }
  return FILE_TYPE_VARCHAR;
}

int ObCsvSchemaInfer::infer(const char *data, int64_t len,
                            const ObFileScanOptions &options,
                            common::ObIArray<ObFileSqlType> &types,
                            int64_t &column_count)
{
  int ret = common::OB_SUCCESS;
  column_count = 0;
  if (OB_ISNULL(data) || len < 0) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    bool first_data_row = true;
    const char *p = data;
    const char *end = data + len;
    while (OB_SUCC(ret) && p < end) {
      const char *line_end = p;
      while (line_end < end && *line_end != options.line_delim_) { line_end++; }
      const bool blank = (line_end == p);
      const bool header_row = (p == data) && options.has_header_;
      if (!blank && !header_row) {
        // Split into fields and infer/widen each column type.
        int64_t col = 0;
        const char *field_start = p;
        const char *f = p;
        bool in_quote = false;
        while (OB_SUCC(ret) && f <= line_end) {
          const bool at_delim = (f == line_end && !in_quote)
                                || (f < line_end && *f == options.field_delim_ && !in_quote);
          if (at_delim) {
            common::ObString field(static_cast<int32_t>(f - field_start), field_start);
            // strip surrounding quotes
            if (field.length() >= 2 && field.ptr()[0] == options.quote_
                && field.ptr()[field.length() - 1] == options.quote_) {
              field = common::ObString(field.length() - 2, field.ptr() + 1);
            }
            const ObFileSqlType t = is_null_token(field, options)
                                    ? FILE_TYPE_NULL : infer_value_type(field);
            if (first_data_row) {
              if (OB_FAIL(types.push_back(t))) { LOG_WARN("push type failed", K(ret)); }
            } else if (col < types.count()) {
              types.at(col) = widen_file_type(types.at(col), t);
            }
            col++;
            field_start = f + 1;
          } else if (f < line_end && *f == options.quote_) {
            in_quote = !in_quote;
          } else if (f < line_end && *f == options.escape_ && f + 1 < line_end) {
            f++; // skip escaped char
          }
          f++;
        }
        if (first_data_row) {
          column_count = col;
          first_data_row = false;
        }
      }
      p = (line_end < end) ? line_end + 1 : end;
    }
    if (column_count == 0) {
      ret = common::OB_INVALID_DATA;
    }
  }
  return ret;
}

int ObJsonlSchema::merge_object_keys(const common::ObIArray<common::ObString> &keys,
                                     common::ObIArray<common::ObString> &all_keys)
{
  int ret = common::OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < keys.count(); i++) {
    bool found = false;
    for (int64_t j = 0; j < all_keys.count(); j++) {
      if (keys.at(i) == all_keys.at(j)) { found = true; break; }
    }
    if (!found) {
      if (OB_FAIL(all_keys.push_back(keys.at(i)))) { LOG_WARN("push key failed", K(ret)); }
    }
  }
  return ret;
}

bool ObJsonlSchema::is_flat_value(int type)
{
  // J_STRING/J_INT/J_UINT/J_DOUBLE/J_DECIMAL/J_BOOLEAN/J_NULL are flat;
  // J_OBJECT/J_ARRAY are nested and rejected by the JSONL reader.
  return type != 6 /*J_OBJECT*/ && type != 7 /*J_ARRAY*/;
}

int get_file_identity(const common::ObString &path, ObFileIdentity &identity)
{
  int ret = common::OB_SUCCESS;
  if (path.empty()) {
    ret = common::OB_INVALID_ARGUMENT;
  } else {
    common::ObString zpath;
    const int64_t buf_len = path.length() + 1;
    char *buf = static_cast<char *>(ob_malloc(buf_len, common::ObMemAttr("FileIdent")));
    if (OB_ISNULL(buf)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, path.ptr(), path.length());
      buf[path.length()] = '\0';
      struct stat st;
      if (0 != stat(buf, &st)) {
        ret = common::OB_FILE_NOT_EXIST;
      } else {
        identity.dev_ = static_cast<uint64_t>(st.st_dev);
        identity.ino_ = static_cast<uint64_t>(st.st_ino);
        identity.mtime_ = static_cast<int64_t>(st.st_mtime);
        identity.size_ = static_cast<int64_t>(st.st_size);
      }
      ob_free(buf);
    }
  }
  return ret;
}

bool is_file_identity_changed(const ObFileIdentity &lhs, const ObFileIdentity &rhs)
{
  return lhs.dev_ != rhs.dev_ || lhs.ino_ != rhs.ino_
         || lhs.mtime_ != rhs.mtime_ || lhs.size_ != rhs.size_;
}

} // namespace sql
} // namespace oceanbase
