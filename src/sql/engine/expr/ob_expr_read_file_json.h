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

#ifndef OB_EXPR_READ_FILE_JSON_H_
#define OB_EXPR_READ_FILE_JSON_H_

#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase
{
namespace sql
{

// Internal helper expression for file_sql table functions.
// read_file_json(path, format, options_text) -> JSON array text.
// Each file row/record becomes one JSON object in the array, so the result can
// be consumed by JSON_TABLE in the resolver rewrite.
class ObExprReadFileJson : public ObFuncExprOperator
{
public:
  explicit ObExprReadFileJson(common::ObIAllocator &alloc);
  virtual ~ObExprReadFileJson();
  virtual int calc_result_typeN(ObExprResType &type,
                                ObExprResType *types_stack,
                                int64_t param_num,
                                common::ObExprTypeCtx &type_ctx) const override;
  static int eval_read_file_json(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);
  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx, const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const override;

private:
  static int read_csv_to_json(common::ObIAllocator &allocator,
                              const common::ObString &path,
                              const common::ObString &options,
                              common::ObString &result);
  static int read_json_to_json(common::ObIAllocator &allocator,
                               const common::ObString &path,
                               common::ObString &result);
  DISALLOW_COPY_AND_ASSIGN(ObExprReadFileJson);
};

} // namespace sql
} // namespace oceanbase

#endif // OB_EXPR_READ_FILE_JSON_H_
