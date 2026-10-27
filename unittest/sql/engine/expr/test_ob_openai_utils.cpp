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
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace common
{

class TestObOpenAIUtils : public ::testing::Test
{
protected:
  ObArenaAllocator allocator_;

  int parse_json(const char *json_str, ObJsonObject *&json_object)
  {
    int ret = OB_SUCCESS;
    ObIJsonBase *j_base = nullptr;
    if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator_, json_str,
                                                 ObJsonInType::JSON_TREE,
                                                 ObJsonInType::JSON_TREE, j_base))) {
    } else if (j_base->json_type() != ObJsonNodeType::J_OBJECT) {
      ret = OB_INVALID_DATA;
    } else {
      json_object = static_cast<ObJsonObject *>(j_base);
    }
    return ret;
  }

  static std::string to_std_string(const ObString &str)
  {
    return std::string(str.ptr(), str.length());
  }

  // Read a json number without guessing its node type.
  static double get_number_value(ObJsonNode *node)
  {
    double value = 0.0;
    if (OB_NOTNULL(node)) {
      if (node->json_type() == ObJsonNodeType::J_DOUBLE) {
        value = static_cast<ObJsonDouble *>(node)->value();
      } else if (node->json_type() == ObJsonNodeType::J_INT) {
        value = static_cast<double>(static_cast<ObJsonInt *>(node)->value());
      }
    }
    return value;
  }
};

// The provider may answer the embeddings out of order, the answer has to be
// restored to the order of the request.
TEST_F(TestObOpenAIUtils, embed_parse_output_restores_the_order)
{
  const char *response =
    "{\"data\": ["
      "{""object"": ""embedding"", ""index"": 1, ""embedding"": [3, 4]},"
      "{""object"": ""embedding"", ""index"": 0, ""embedding"": [1, 2]}"
    "]}";
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(response, json_object));

  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator_, json_object, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());

  ObJsonArray *result_array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, result_array->element_count());
  // The first element belongs to the first input of the request.
  ObJsonNode *first = result_array->get_value(0);
  ASSERT_NE(nullptr, first);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, first->json_type());
  ObJsonArray *first_array = static_cast<ObJsonArray *>(first);
  ASSERT_EQ(2, first_array->element_count());
  EXPECT_DOUBLE_EQ(1.0, get_number_value(first_array->get_value(0)));
  EXPECT_DOUBLE_EQ(2.0, get_number_value(first_array->get_value(1)));
}

// A provider that does not report an index keeps its own order.
TEST_F(TestObOpenAIUtils, embed_parse_output_without_index)
{
  const char *response =
    "{\"data\": ["
      "{""embedding"": [1, 2]},"
      "{""embedding"": [3, 4]}"
    "]}";
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(response, json_object));

  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator_, json_object, result));
  ObJsonArray *result_array = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, result_array->element_count());
  ObJsonArray *first_array = static_cast<ObJsonArray *>(result_array->get_value(0));
  EXPECT_DOUBLE_EQ(1.0, get_number_value(first_array->get_value(0)));
}

// A response that does not carry any embedding is invalid data.
TEST_F(TestObOpenAIUtils, embed_parse_output_without_data)
{
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json("{\"object\": \"list\"}", json_object));

  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_INVALID_DATA, embed.parse_output(allocator_, json_object, result));
}

// An element without an embedding is invalid data as well.
TEST_F(TestObOpenAIUtils, embed_parse_output_with_broken_element)
{
  const char *response = "{\"data\": [{\"index\": 0}]}";
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(response, json_object));

  ObOpenAIUtils::ObOpenAIEmbed embed;
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_INVALID_DATA, embed.parse_output(allocator_, json_object, result));
}

// The openai completion asks the model in the chat format.
TEST_F(TestObOpenAIUtils, complete_get_body)
{
  ObOpenAIUtils::ObOpenAIComplete complete;
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ObString model("text-embedding-3-small");
  ObString prompt("");
  ObString content("hello");
  ObString url("http://127.0.0.1:8000/v1/chat/completions");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, prompt, content, config, url, body));
  ASSERT_NE(nullptr, body);
  ASSERT_NE(nullptr, body->get_value("model"));
  ASSERT_NE(nullptr, body->get_value("messages"));
  ObJsonArray *messages = static_cast<ObJsonArray *>(body->get_value("messages"));
  ASSERT_EQ(1, messages->element_count());
}

// The openai completion reads the answer out of the chat format.
TEST_F(TestObOpenAIUtils, complete_parse_output)
{
  const char *response =
    "{\"choices\": [{\"message\": {\"role\": \"assistant\", \"content\": \"42\"}}]}";
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(response, json_object));

  ObOpenAIUtils::ObOpenAIComplete complete;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator_, json_object, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_STRING, result->json_type());
  EXPECT_EQ("42", to_std_string(static_cast<ObJsonString *>(result)->get_str()));
}

// A response without an answer is invalid data.
TEST_F(TestObOpenAIUtils, complete_parse_output_without_choice)
{
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json("{\"choices\": []}", json_object));

  ObOpenAIUtils::ObOpenAIComplete complete;
  ObIJsonBase *result = nullptr;
  EXPECT_EQ(OB_INVALID_DATA, complete.parse_output(allocator_, json_object, result));
}

// A provider that needs an access key does not get one when it is empty.
TEST_F(TestObOpenAIUtils, openai_header_needs_an_access_key)
{
  ObArray<ObString> headers;
  ObString api_key("");
  EXPECT_EQ(OB_INVALID_ARGUMENT,
            ObOpenAIUtils::get_header(allocator_, api_key, headers));
  EXPECT_EQ(0, headers.count());
}

// The ollama provider needs no access key but still sends a json post.
TEST_F(TestObOpenAIUtils, ollama_header_without_an_access_key)
{
  ObArray<ObString> headers;
  ASSERT_EQ(OB_SUCCESS, ObOllamaUtils::get_header(allocator_, headers));
  ASSERT_EQ(1, headers.count());
  EXPECT_EQ("Content-Type: application/json", to_std_string(headers[0]));
}

// The native ollama completion protocol asks for one answer, the streaming is
// turned off.
TEST_F(TestObOpenAIUtils, ollama_complete_get_body)
{
  ObOllamaUtils::ObOllamaComplete complete;
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ObString model("llama3.1");
  ObString prompt("");
  ObString content("What is the capital of France?");
  ObString url("http://127.0.0.1:11434/api/generate");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, prompt, content, config, url, body));
  ASSERT_NE(nullptr, body);
  ASSERT_NE(nullptr, body->get_value("model"));
  ASSERT_NE(nullptr, body->get_value("prompt"));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(static_cast<ObJsonBoolean *>(body->get_value("stream"))->value());
  // The native generate protocol does not use the chat format.
  EXPECT_EQ(nullptr, body->get_value("messages"));
}

// The ollama chat protocol asks in the chat format.
TEST_F(TestObOpenAIUtils, ollama_complete_get_body_for_chat)
{
  ObOllamaUtils::ObOllamaComplete complete;
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ObString model("llama3.1");
  ObString prompt("");
  ObString content("hi");
  ObString url("http://127.0.0.1:11434/api/chat");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.get_body(allocator_, model, prompt, content, config, url, body));
  ASSERT_NE(nullptr, body);
  ASSERT_NE(nullptr, body->get_value("messages"));
  EXPECT_EQ(nullptr, body->get_value("prompt"));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(static_cast<ObJsonBoolean *>(body->get_value("stream"))->value());
}

// The native ollama completion protocol answers in the response field.
TEST_F(TestObOpenAIUtils, ollama_complete_parse_output)
{
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(
      "{\"model\": \"llama3.1\", \"response\": \"Paris\", \"done\": true}", json_object));

  ObOllamaUtils::ObOllamaComplete complete;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator_, json_object, result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ("Paris", to_std_string(static_cast<ObJsonString *>(result)->get_str()));
}

// The ollama chat protocol answers in the message field.
TEST_F(TestObOpenAIUtils, ollama_complete_parse_output_for_chat)
{
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(
      "{\"model\": \"llama3.1\", \"message\": {\"role\": \"assistant\", \"content\": \"Paris\"}}",
      json_object));

  ObOllamaUtils::ObOllamaComplete complete;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, complete.parse_output(allocator_, json_object, result));
  ASSERT_NE(nullptr, result);
  EXPECT_EQ("Paris", to_std_string(static_cast<ObJsonString *>(result)->get_str()));
}

// The native ollama embedding protocol carries every input of the request in
// one request.
TEST_F(TestObOpenAIUtils, ollama_embed_get_body)
{
  ObOllamaUtils::ObOllamaEmbed embed;
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ObString model("nomic-embed-text");
  ObArray<ObString> contents;
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("first")));
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("second")));
  ObString url("http://127.0.0.1:11434/api/embed");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.get_body(allocator_, model, contents, config, url, body));
  ASSERT_NE(nullptr, body);
  ASSERT_NE(nullptr, body->get_value("input"));
  ObJsonArray *input = static_cast<ObJsonArray *>(body->get_value("input"));
  ASSERT_EQ(2, input->element_count());
}

// The legacy ollama embedding protocol takes one input only.
TEST_F(TestObOpenAIUtils, ollama_embed_get_body_for_the_legacy_protocol)
{
  ObOllamaUtils::ObOllamaEmbed embed;
  ObJsonObject *config = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object(allocator_, config));
  ObString model("nomic-embed-text");
  ObArray<ObString> contents;
  ASSERT_EQ(OB_SUCCESS, contents.push_back(ObString("first")));
  ObString url("http://127.0.0.1:11434/api/embeddings");
  ObJsonObject *body = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.get_body(allocator_, model, contents, config, url, body));
  ASSERT_NE(nullptr, body);
  ASSERT_NE(nullptr, body->get_value("prompt"));
  EXPECT_EQ(nullptr, body->get_value("input"));
}

// The native ollama embedding protocol answers with one vector per input.
TEST_F(TestObOpenAIUtils, ollama_embed_parse_output)
{
  const char *response =
    "{\"model\": \"nomic-embed-text\", \"embeddings\": [[0.1, 0.2], [0.3, 0.4]]}";
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(response, json_object));

  ObOllamaUtils::ObOllamaEmbed embed;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator_, json_object, result));
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, result->json_type());
  ObJsonArray *embeddings = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(2, embeddings->element_count());
}

// The legacy ollama embedding protocol answers with one vector, it is wrapped
// so that the caller always sees one embedding per input.
TEST_F(TestObOpenAIUtils, ollama_embed_parse_output_for_the_legacy_protocol)
{
  const char *response = "{""embedding"": [1, 2]}";
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json(response, json_object));

  ObOllamaUtils::ObOllamaEmbed embed;
  ObIJsonBase *result = nullptr;
  ASSERT_EQ(OB_SUCCESS, embed.parse_output(allocator_, json_object, result));
  ASSERT_NE(nullptr, result);
  ObJsonArray *embeddings = static_cast<ObJsonArray *>(result);
  ASSERT_EQ(1, embeddings->element_count());
  ASSERT_EQ(ObJsonNodeType::J_ARRAY, embeddings->get_value(0)->json_type());
}

// Every element of an embedding vector has to be a number.
TEST_F(TestObOpenAIUtils, embedding_elements_have_to_be_numbers)
{
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json("{""embedding"": [1, 2]}", json_object));
  ObJsonArray *vector = static_cast<ObJsonArray *>(json_object->get_value("embedding"));
  ASSERT_NE(nullptr, vector);
  EXPECT_EQ(OB_SUCCESS, ObAIFuncUtils::check_embedding_elements(vector));

  ObJsonObject *broken = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json("{""embedding"": [1, ""x""]}", broken));
  ObJsonArray *broken_vector = static_cast<ObJsonArray *>(broken->get_value("embedding"));
  EXPECT_EQ(OB_INVALID_DATA, ObAIFuncUtils::check_embedding_elements(broken_vector));
}

// A base64 encoded vector is turned back into a json array of numbers.
TEST_F(TestObOpenAIUtils, base64_embedding_is_decoded)
{
  ObJsonObject *json_object = nullptr;
  // The base64 of the two little endian floats 1.0 and 2.0.
  ASSERT_EQ(OB_SUCCESS, parse_json("{\"embedding\": \"AACAPwAAAEA=\"}", json_object));
  ObJsonNode *embedding = json_object->get_value("embedding");
  ASSERT_NE(nullptr, embedding);

  ObJsonArray *vector = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::decode_base64_embedding_to_json(
                            allocator_, *embedding, 2, vector));
  ASSERT_NE(nullptr, vector);
  ASSERT_EQ(2, vector->element_count());
  EXPECT_DOUBLE_EQ(1.0, get_number_value(vector->get_value(0)));
  EXPECT_DOUBLE_EQ(2.0, get_number_value(vector->get_value(1)));
}

// A base64 encoded vector of the wrong length does not match the dimension.
TEST_F(TestObOpenAIUtils, base64_embedding_with_a_wrong_dimension)
{
  ObJsonObject *json_object = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse_json("{\"embedding\": \"AACAPwAAAEA=\"}", json_object));
  ObJsonNode *embedding = json_object->get_value("embedding");
  ASSERT_NE(nullptr, embedding);

  ObJsonArray *vector = nullptr;
  EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncUtils::decode_base64_embedding_to_json(
                                   allocator_, *embedding, 3, vector));
}

// The ollama provider is known to the function layer.
TEST_F(TestObOpenAIUtils, ollama_provider_is_wired)
{
  ObAIFuncIComplete *complete_provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_complete_provider(
                            allocator_, ObString("OLLAMA"), complete_provider));
  EXPECT_NE(nullptr, complete_provider);

  ObAIFuncIEmbed *embed_provider = nullptr;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncUtils::get_embed_provider(
                            allocator_, ObString("ollama"), embed_provider));
  EXPECT_NE(nullptr, embed_provider);
}

} // namespace common
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
