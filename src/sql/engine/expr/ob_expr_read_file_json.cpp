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
#include "ob_expr_read_file_json.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include <cstdio>
#include <cstring>
#include <glob.h>

namespace oceanbase
{
namespace sql
{

ObExprReadFileJson::ObExprReadFileJson(common::ObIAllocator &alloc)
  : ObFuncExprOperator(alloc, T_FUN_SYS_READ_FILE_JSON, N_READ_FILE_JSON,
                       TWO_OR_THREE, NOT_VALID_FOR_GENERATED_COL, NOT_ROW_DIMENSION)
{
}

ObExprReadFileJson::~ObExprReadFileJson() {}

int ObExprReadFileJson::calc_result_typeN(ObExprResType &type,
                                          ObExprResType *types_stack,
                                          int64_t param_num,
                                          common::ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = common::OB_SUCCESS;
  if (param_num < 2 || param_num > 3) {
    ret = common::OB_ERR_PARAM_SIZE;
  } else {
    for (int64_t i = 0; i < param_num; i++) {
      types_stack[i].set_calc_type(common::ObVarcharType);
      types_stack[i].set_calc_collation_type(common::CS_TYPE_UTF8MB4_BIN);
    }
    type.set_type(common::ObLongTextType);
    type.set_collation_type(common::CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(common::CS_LEVEL_IMPLICIT);
  }
  return ret;
}

namespace {
// Escape a JSON string value into `buf` (appending). Returns false on overflow.
bool append_json_string(char *buf, int64_t cap, int64_t &pos, const char *s, int64_t len)
{
  if (pos >= cap) { return false; }
  buf[pos++] = '"';
  for (int64_t i = 0; i < len; i++) {
    const char c = s[i];
    if (c == '"' || c == '\\') {
      if (pos + 2 > cap) { return false; }
      buf[pos++] = '\\';
      buf[pos++] = c;
    } else if (c == '\n') {
      if (pos + 2 > cap) { return false; }
      buf[pos++] = '\\'; buf[pos++] = 'n';
    } else if (c == '\r') {
      if (pos + 2 > cap) { return false; }
      buf[pos++] = '\\'; buf[pos++] = 'r';
    } else if (c == '\t') {
      if (pos + 2 > cap) { return false; }
      buf[pos++] = '\\'; buf[pos++] = 't';
    } else {
      if (pos + 1 > cap) { return false; }
      buf[pos++] = c;
    }
  }
  if (pos + 1 > cap) { return false; }
  buf[pos++] = '"';
  return true;
}

int read_whole_file(common::ObIAllocator &allocator, const common::ObString &path,
                    char *&data, int64_t &len)
{
  int ret = common::OB_SUCCESS;
  const int64_t cap = path.length() + 1;
  char *path_buf = static_cast<char *>(allocator.alloc(cap));
  if (OB_ISNULL(path_buf)) {
    ret = common::OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    FILE *fp = fopen(path_buf, "rb");
    if (OB_ISNULL(fp)) {
      ret = common::OB_FILE_NOT_EXIST;
    } else {
      if (0 != fseek(fp, 0, SEEK_END)) {
        ret = common::OB_ERR_UNEXPECTED;
      } else {
        const long sz = ftell(fp);
        if (sz < 0) {
          ret = common::OB_ERR_UNEXPECTED;
        } else {
          len = static_cast<int64_t>(sz);
          data = static_cast<char *>(allocator.alloc(len + 1));
          if (OB_ISNULL(data)) {
            ret = common::OB_ALLOCATE_MEMORY_FAILED;
          } else {
            rewind(fp);
            const size_t got = fread(data, 1, static_cast<size_t>(len), fp);
            if (static_cast<int64_t>(got) != len) {
              ret = common::OB_INVALID_DATA;
            } else {
              data[len] = '\0';
            }
          }
        }
      }
      fclose(fp);
    }
  }
  return ret;
}
} // namespace

int ObExprReadFileJson::read_csv_to_json(common::ObIAllocator &allocator,
                                          const common::ObString &path,
                                          const common::ObString &options,
                                          common::ObString &result)
{
  int ret = common::OB_SUCCESS;
  char *data = NULL;
  int64_t len = 0;
  if (OB_FAIL(read_whole_file(allocator, path, data, len))) {
    LOG_WARN("read csv file failed", K(ret), K(path));
  } else {
    ObFileScanOptions opts;
    // parse simple options: "delimiter=;|header=false|quote='|..." (best effort)
    if (!options.empty()) {
      common::ObString opts_copy = options;
      // defaults are fine for the basic case; advanced options parsed here later
      UNUSED(opts_copy);
    }
    // output buffer (each input byte may expand ~6x for escapes)
    const int64_t cap = len * 6 + 1024;
    char *out = static_cast<char *>(allocator.alloc(cap));
    if (OB_ISNULL(out)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t pos = 0;
      out[pos++] = '[';
      bool first_row = true;
      int64_t col_count = 0;
      // column names from the header row (fall back to col_N)
      char col_names[256][256];
      int64_t nnames = 0;
      const char *p = data;
      const char *end = data + len;
      while (OB_SUCC(ret) && p < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != opts.line_delim_ && *line_end != '\r') { line_end++; }
        const bool header_row = (p == data) && opts.has_header_;
        if (header_row && line_end > p) {
          // capture header field names
          const char *hf = p;
          const char *fp = p;
          while (hf <= line_end && nnames < 256) {
            if (hf == line_end || *hf == opts.field_delim_) {
              int64_t l = hf - fp;
              while (l > 0 && (fp[0] == ' ' || fp[0] == '"' || fp[0] == '\'')) { fp++; l--; }
              while (l > 0 && (fp[l-1] == ' ' || fp[l-1] == '"' || fp[l-1] == '\'')) { l--; }
              if (l > 0 && l < 256) {
                MEMCPY(col_names[nnames], fp, static_cast<size_t>(l));
                col_names[nnames][l] = '\0';
              } else {
                snprintf(col_names[nnames], sizeof(col_names[nnames]), "col_%ld", nnames);
              }
              nnames++;
              fp = hf + 1;
            }
            hf++;
          }
        }
        if (!header_row && line_end > p) {
          // split fields
          common::ObString fields[64];
          int64_t nfields = 0;
          const char *f = p;
          const char *field_start = p;
          bool in_quote = false;
          while (f <= line_end && nfields < 64) {
            const bool at_delim = (f == line_end)
                                  || (f < line_end && *f == opts.field_delim_ && !in_quote);
            if (at_delim) {
              fields[nfields].assign_ptr(const_cast<char *>(field_start),
                                         static_cast<int32_t>(f - field_start));
              // strip quotes
              if (fields[nfields].length() >= 2
                  && fields[nfields].ptr()[0] == opts.quote_
                  && fields[nfields].ptr()[fields[nfields].length() - 1] == opts.quote_) {
                fields[nfields] = common::ObString(fields[nfields].length() - 2,
                                                    fields[nfields].ptr() + 1);
              }
              nfields++;
              field_start = f + 1;
            } else if (f < line_end && *f == opts.quote_) {
              in_quote = !in_quote;
            }
            f++;
          }
          if (first_row) { col_count = nfields; first_row = false; }
          // header row supplies column names; data rows use col_0.. naming
          if (!header_row) {
            if (!first_row && out[pos - 1] != '[') { out[pos++] = ','; }
            out[pos++] = '{';
            for (int64_t c = 0; c < col_count && c < nfields; c++) {
              if (c > 0) { out[pos++] = ','; }
              const char *key = (c < nnames) ? col_names[c] : "col";
              const int key_len = static_cast<int>(strlen(key));
              if (!append_json_string(out, cap, pos, key, key_len)
                  || pos + 1 > cap) { ret = common::OB_BUF_NOT_ENOUGH; break; }
              out[pos++] = ':';
              if (!append_json_string(out, cap, pos, fields[c].ptr(), fields[c].length())) {
                ret = common::OB_BUF_NOT_ENOUGH; break;
              }
            }
            out[pos++] = '}';
          }
        }
        // advance past line delimiters
        while (line_end < end && (*line_end == '\n' || *line_end == '\r')) { line_end++; }
        p = line_end > p ? line_end : p + 1;
      }
      if (OB_SUCC(ret)) {
        out[pos++] = ']';
        result.assign_ptr(out, static_cast<int32_t>(pos));
      }
    }
  }
  return ret;
}

int ObExprReadFileJson::read_json_to_json(common::ObIAllocator &allocator,
                                           const common::ObString &path,
                                           common::ObString &result)
{
  int ret = common::OB_SUCCESS;
  char *data = NULL;
  int64_t len = 0;
  if (OB_FAIL(read_whole_file(allocator, path, data, len))) {
    LOG_WARN("read json file failed", K(ret), K(path));
  } else {
    // JSONL: wrap newline-separated objects into an array.
    const int64_t cap = len + 16;
    char *out = static_cast<char *>(allocator.alloc(cap));
    if (OB_ISNULL(out)) {
      ret = common::OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t pos = 0;
      out[pos++] = '[';
      const char *p = data;
      const char *end = data + len;
      while (p < end) {
        const char *line_end = p;
        while (line_end < end && *line_end != '\n' && *line_end != '\r') { line_end++; }
        if (line_end > p) {
          if (out[pos - 1] != '[') { out[pos++] = ','; }
          MEMCPY(out + pos, p, line_end - p);
          pos += line_end - p;
        }
        while (line_end < end && (*line_end == '\n' || *line_end == '\r')) { line_end++; }
        p = line_end;
      }
      out[pos++] = ']';
      result.assign_ptr(out, static_cast<int32_t>(pos));
    }
  }
  return ret;
}

int ObExprReadFileJson::eval_read_file_json(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *path_datum = NULL;
  ObDatum *format_datum = NULL;
  ObDatum *options_datum = NULL;
  if (expr.arg_cnt_ < 2) {
    ret = common::OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(expr.eval_param_value(ctx, path_datum, format_datum, options_datum))) {
  } else if (path_datum->is_null() || format_datum->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    common::ObArenaAllocator &allocator = tmp_alloc_g.get_allocator();
    const common::ObString path = path_datum->get_string();
    const common::ObString fmt = format_datum->get_string();
    const common::ObString options = (expr.arg_cnt_ >= 3 && !options_datum->is_null())
                                     ? options_datum->get_string() : common::ObString();
    // secure_file_priv check
    common::ObString secure_priv;
    if (OB_SUCC(ret)) {
      ObSQLSessionInfo *session = ctx.exec_ctx_.get_my_session();
      if (OB_NOT_NULL(session) && OB_SUCC(session->get_secure_file_priv(secure_priv))
          && !secure_priv.empty()) {
        if (OB_FAIL(ObResolverUtils::check_secure_path(secure_priv, path))) {
          LOG_USER_ERROR(OB_ERR_NO_PRIV_DIRECT_PATH_ACCESS, path.length(), path.ptr());
        }
      }
    }
    common::ObString json_result;
    if (OB_FAIL(ret)) {
    } else if (fmt.case_compare("csv") == 0) {
      ret = read_csv_to_json(allocator, path, options, json_result);
    } else if (fmt.case_compare("json") == 0 || fmt.case_compare("ndjson") == 0) {
      ret = read_json_to_json(allocator, path, json_result);
    } else if (fmt.case_compare("glob") == 0) {
      // Expand a glob pattern into [{file:path}, ...].
      const int64_t cap = path.length() + 1;
      char *pattern = static_cast<char *>(allocator.alloc(cap));
      if (OB_ISNULL(pattern)) {
        ret = common::OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(pattern, path.ptr(), path.length());
        pattern[path.length()] = '\0';
        glob_t g;
        MEMSET(&g, 0, sizeof(g));
        const int gr = glob(pattern, GLOB_NOSORT, NULL, &g);
        if (gr != 0 && gr != GLOB_NOMATCH) {
          ret = common::OB_INVALID_ARGUMENT;
        } else {
          int64_t need = 2;
          for (size_t i = 0; i < g.gl_pathc; i++) {
            need += strlen(g.gl_pathv[i]) + 16;
          }
          char *out = static_cast<char *>(allocator.alloc(need));
          if (OB_ISNULL(out)) {
            ret = common::OB_ALLOCATE_MEMORY_FAILED;
          } else {
            int64_t pos = 0;
            out[pos++] = '[';
            for (size_t i = 0; i < g.gl_pathc; i++) {
              if (i > 0) { out[pos++] = ','; }
              const char *s = "{\"file\":\"";
              MEMCPY(out + pos, s, strlen(s)); pos += strlen(s);
              const size_t l = strlen(g.gl_pathv[i]);
              MEMCPY(out + pos, g.gl_pathv[i], l); pos += l;
              out[pos++] = '"'; out[pos++] = '}';
            }
            out[pos++] = ']';
            json_result.assign_ptr(out, static_cast<int32_t>(pos));
          }
          globfree(&g);
        }
      }
    } else {
      ret = common::OB_NOT_SUPPORTED;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, json_result))) {
      // use generic string result helper
    }
  }
  return ret;
}

int ObExprReadFileJson::cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                                ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  rt_expr.eval_func_ = eval_read_file_json;
  return common::OB_SUCCESS;
}

} // namespace sql
} // namespace oceanbase
