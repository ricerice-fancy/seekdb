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

#ifndef OB_FILE_SCAN_UTILS_H_
#define OB_FILE_SCAN_UTILS_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_array.h"
#include "share/schema/ob_schema_struct.h"

namespace oceanbase
{
namespace sql
{

// Deterministic type set produced by file schema inference.
// Widening order: NULL < BOOLEAN < BIGINT < DOUBLE < DATE < DATETIME < VARCHAR.
enum ObFileSqlType
{
  FILE_TYPE_INVALID = 0,
  FILE_TYPE_NULL,
  FILE_TYPE_BOOLEAN,
  FILE_TYPE_BIGINT,
  FILE_TYPE_DOUBLE,
  FILE_TYPE_DATE,
  FILE_TYPE_DATETIME,
  FILE_TYPE_VARCHAR,
  FILE_TYPE_MAX,
};

// Map an ObFileSqlType to the corresponding SQL expression type.
common::ObObjType file_type_to_obj_type(ObFileSqlType type);
const char *file_type_to_str(ObFileSqlType type);

// Detect the narrowest type that can represent `str`.
// Empty strings and the configured NULL tokens produce FILE_TYPE_NULL.
ObFileSqlType infer_value_type(const common::ObString &str);

// Widen two inferred types to the narrowest common type.
ObFileSqlType widen_file_type(ObFileSqlType lhs, ObFileSqlType rhs);

struct ObFileScanOptions
{
  ObFileScanOptions()
    : field_delim_(','), line_delim_('\n'), quote_('"'), escape_('\\'),
      has_header_(true), skip_blank_lines_(true), null_if_(), max_line_size_(1 << 20)
  {}
  char field_delim_;
  char line_delim_;
  char quote_;
  char escape_;
  bool has_header_;
  bool skip_blank_lines_;
  common::ObString null_if_;
  int64_t max_line_size_;
  TO_STRING_KV(K_(field_delim), K_(line_delim), K_(quote), K_(escape),
               K_(has_header), K_(skip_blank_lines), K_(max_line_size));
};

// Infer column types for a whole CSV file by scanning every row.
// `types` is filled with one entry per column; all rows are read so the
// inference is deterministic and independent of any sample size.
class ObCsvSchemaInfer
{
public:
  static int infer(const char *data, int64_t len,
                   const ObFileScanOptions &options,
                   common::ObIArray<ObFileSqlType> &types,
                   int64_t &column_count);
};

// Ordered union of the keys of a set of flat JSON objects. Missing keys are
// filled with NULL when rows are materialized; nested objects/arrays are
// rejected according to the JSONL spec.
class ObJsonlSchema
{
public:
  static int merge_object_keys(const common::ObIArray<common::ObString> &keys,
                               common::ObIArray<common::ObString> &all_keys);
  static bool is_flat_value(int type);
};

// Detect whether a file changed since it was scanned (path/identity/mtime/size).
struct ObFileIdentity
{
  ObFileIdentity() : dev_(0), ino_(0), mtime_(0), size_(0) {}
  uint64_t dev_;
  uint64_t ino_;
  int64_t mtime_;
  int64_t size_;
  TO_STRING_KV(K_(dev), K_(ino), K_(mtime), K_(size));
};

int get_file_identity(const common::ObString &path, ObFileIdentity &identity);
bool is_file_identity_changed(const ObFileIdentity &lhs, const ObFileIdentity &rhs);

} // namespace sql
} // namespace oceanbase

#endif // OB_FILE_SCAN_UTILS_H_
