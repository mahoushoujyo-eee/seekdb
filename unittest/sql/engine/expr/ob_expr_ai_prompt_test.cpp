/**
 * Copyright (c) 2025 OceanBase.
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

#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{
using namespace sql;

class TestObExprAIPrompt : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  // Build a prompt object out of a template and a list of string arguments.
  int build_prompt(const char *template_str,
                   const std::vector<ObString> &args,
                   ObJsonObject *&prompt_object)
  {
    int ret = OB_SUCCESS;
    ObJsonArray *args_array = nullptr;
    ObString template_ob_str(template_str);
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator_, args_array))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < args.size(); i++) {
        ObJsonString *arg_node = nullptr;
        ObString arg = args[i];
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator_, arg, arg_node))) {
        } else if (OB_FAIL(args_array->append(arg_node))) {
        }
      }
      if (OB_SUCC(ret)) {
        ret = ObAIFuncPromptObjectUtils::construct_prompt_object(
            allocator_, template_ob_str, args_array, prompt_object);
      }
    }
    return ret;
  }

  // Build a prompt object whose single argument is a json value.
  int build_prompt_with_json_arg(const char *template_str,
                                 const char *json_arg,
                                 ObJsonObject *&prompt_object)
  {
    int ret = OB_SUCCESS;
    ObJsonArray *args_array = nullptr;
    ObJsonObject *prompt_obj = nullptr;
    ObString template_ob_str(template_str);
    ObIJsonBase *j_base = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator_, json_arg,
                                                 ObJsonInType::JSON_TREE,
                                                 ObJsonInType::JSON_TREE, j_base))) {
    } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator_, args_array))) {
    } else if (OB_FAIL(args_array->append(static_cast<ObJsonNode *>(j_base)))) {
    } else if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(
                   allocator_, template_ob_str, args_array, prompt_obj))) {
    } else {
      prompt_object = prompt_obj;
    }
    return ret;
  }

  static std::string to_std_string(const ObString &str)
  {
    return std::string(str.ptr(), str.length());
  }
};

// A template with one placeholder per argument is rendered argument by
// argument.
TEST_F(TestObExprAIPrompt, simple_replacement)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("world"));
  args.push_back(ObString("moon"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("hello {0} and {1}", args, prompt_object));
  ASSERT_NE(nullptr, prompt_object);
  EXPECT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced));
  EXPECT_EQ("hello world and moon", to_std_string(replaced));
}

// The same placeholder may be used more than once, every occurrence has to be
// replaced.
TEST_F(TestObExprAIPrompt, repeated_placeholder)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("x"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{0}-{0}-{0}", args, prompt_object));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced));
  EXPECT_EQ("x-x-x", to_std_string(replaced));
}

// A repeated placeholder whose argument is longer than the placeholder itself
// must not run out of room.
TEST_F(TestObExprAIPrompt, repeated_placeholder_grows_the_result)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("0123456789"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{0}{0}{0}{0}{0}", args, prompt_object));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced));
  EXPECT_EQ("01234567890123456789012345678901234567890123456789",
            to_std_string(replaced));
}

// An index that does not belong to the argument list is an error.
TEST_F(TestObExprAIPrompt, illegal_placeholder_index)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("a"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{3}", args, prompt_object));

  ObString replaced;
  EXPECT_EQ(OB_INVALID_ARGUMENT,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                allocator_, prompt_object, replaced));
}

// An empty argument list can not fill a placeholder.
TEST_F(TestObExprAIPrompt, placeholder_without_argument)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  ASSERT_EQ(OB_SUCCESS, build_prompt("{0}", args, prompt_object));

  ObString replaced;
  EXPECT_EQ(OB_INVALID_ARGUMENT,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                allocator_, prompt_object, replaced));
}

// A brace that does not open a placeholder stays as it is.
TEST_F(TestObExprAIPrompt, brace_that_is_not_a_placeholder)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("a"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{\"key\": \"{0}\"}", args, prompt_object));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced));
  EXPECT_EQ("{\"key\": \"a\"}", to_std_string(replaced));
}

// A json object argument is rendered as its compact json text.
TEST_F(TestObExprAIPrompt, json_object_argument)
{
  ObJsonObject *prompt_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_with_json_arg(
                            "payload: {0}", "{\"a\":\"1\"}", prompt_object));
  ASSERT_NE(nullptr, prompt_object);
  EXPECT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced));
  EXPECT_EQ("payload: {\"a\":\"1\"}", to_std_string(replaced));
}

// A json array argument is rendered as its compact json text.
TEST_F(TestObExprAIPrompt, json_array_argument)
{
  ObJsonObject *prompt_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_with_json_arg(
                            "payload: {0}", "[\"a\",\"b\"]", prompt_object));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced));
  EXPECT_EQ("payload: [\"a\",\"b\"]", to_std_string(replaced));
}

// A prompt object may be resolved into the template text it stands for, that
// is what makes a nested prompt work.
TEST_F(TestObExprAIPrompt, nested_prompt_is_resolved)
{
  ObJsonObject *inner = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("b"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("a {0} c", args, inner));

  ObString resolved;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::resolve_prompt_object(
                            allocator_, inner, resolved));
  EXPECT_EQ("a b c", to_std_string(resolved));
}

// The arguments of a nested prompt may be json values as well.
TEST_F(TestObExprAIPrompt, nested_prompt_with_json_argument)
{
  ObJsonObject *inner = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_with_json_arg(
                            "{0} tail", "{\"k\":1}", inner));

  ObString resolved;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::resolve_prompt_object(
                            allocator_, inner, resolved));
  EXPECT_EQ("{\"k\":1} tail", to_std_string(resolved));
}

// A prompt object has to carry a template and an argument list.
TEST_F(TestObExprAIPrompt, prompt_object_validation)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("a"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{0}", args, prompt_object));
  EXPECT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  EXPECT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(nullptr));

  // An object without a template and an argument list is not a prompt.
  ObJsonObject *empty_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, empty_object));
  EXPECT_FALSE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(empty_object));
}

// The placeholder scanner tells a placeholder from an ordinary brace.
TEST_F(TestObExprAIPrompt, parse_placeholder)
{
  const char *template_str = "x{12}y";
  int64_t end_pos = 0;
  int64_t index = 0;
  bool found = false;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::parse_placeholder(
                            template_str, 6, 1, end_pos, index, found));
  EXPECT_TRUE(found);
  EXPECT_EQ(4, end_pos);
  EXPECT_EQ(12, index);

  found = false;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::parse_placeholder(
                            template_str, 6, 0, end_pos, index, found));
  EXPECT_FALSE(found);
}

// Unicode text and escaped characters survive the rendering.
TEST_F(TestObExprAIPrompt, unicode_and_escaped_text)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("世界"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("你好 {0} \\n 结束", args, prompt_object));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced));
  EXPECT_EQ("你好 世界 \\n 结束", to_std_string(replaced));
  // The result is measured in bytes, a multibyte character keeps its length.
  EXPECT_EQ(strlen("你好 世界 \\n 结束"), replaced.length());
}

// An empty argument is allowed, it only renders as an empty text.
TEST_F(TestObExprAIPrompt, empty_argument)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString(""));
  ASSERT_EQ(OB_SUCCESS, build_prompt("1+1=?{0}", args, prompt_object));

  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, prompt_object, replaced));
  EXPECT_EQ("1+1=?", to_std_string(replaced));
}

// An index that does not belong to the argument list is an error.
TEST_F(TestObExprAIPrompt, illegal_placeholder_index)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("a"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{3}", args, prompt_object));

  ObString replaced;
  EXPECT_EQ(OB_INVALID_ARGUMENT,
            ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                allocator_, prompt_object, replaced));
  EXPECT_EQ(OB_INVALID_ARGUMENT,
            ObAIFuncPromptObjectUtils::check_placeholders(ObString("{3}"), 1));
  EXPECT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::check_placeholders(ObString("{0}"), 1));
  EXPECT_EQ(OB_SUCCESS,
            ObAIFuncPromptObjectUtils::check_placeholders(ObString("no placeholder"), 0));
}

// The prompt object is built out of a template and its arguments.
TEST_F(TestObExprAIPrompt, prompt_object_carries_the_arguments)
{
  ObJsonObject *prompt_object = nullptr;
  std::vector<ObString> args;
  args.push_back(ObString("a"));
  args.push_back(ObString("b"));
  ASSERT_EQ(OB_SUCCESS, build_prompt("{0} {1}", args, prompt_object));
  ASSERT_NE(nullptr, prompt_object);
  ASSERT_NE(nullptr, prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_template_key));
  ASSERT_NE(nullptr, prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key));
  ObJsonArray *prompt_args = static_cast<ObJsonArray *>(
      prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key));
  ASSERT_EQ(2, prompt_args->element_count());
}

// A json array or a json object is one argument.
TEST_F(TestObExprAIPrompt, json_arguments)
{
  ObJsonObject *object_prompt = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_with_json_arg(
                            "{0}", "{\"k\":\"v\"}", object_prompt));
  ObString replaced;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, object_prompt, replaced));
  EXPECT_EQ("{\"k\":\"v\"}", to_std_string(replaced));

  ObJsonObject *array_prompt = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_with_json_arg(
                            "{0}", "[\"a\",\"b\"]", array_prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                            allocator_, array_prompt, replaced));
  EXPECT_EQ("[\"a\",\"b\"]", to_std_string(replaced));
}

// The arguments of a prompt object may be json values as well.
TEST_F(TestObExprAIPrompt, json_arguments_in_the_prompt_object)
{
  ObJsonObject *prompt_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, build_prompt_with_json_arg(
                            "{0}", "{\"k\":\"v\"}", prompt_object));
  EXPECT_TRUE(ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt_object));
  ObJsonArray *prompt_args = static_cast<ObJsonArray *>(
      prompt_object->get_value(ObAIFuncPromptObjectUtils::prompt_args_key));
  ASSERT_NE(nullptr, prompt_args);
  ASSERT_EQ(1, prompt_args->element_count());
  EXPECT_EQ(ObJsonNodeType::J_OBJECT, prompt_args->get_value(0)->json_type());
}

} // namespace common
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
