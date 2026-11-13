#include <rstd/test/gtest.hpp>

import luato.extra;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace json_serde_tests {
struct Request {
  Option<i64> optional;
  empty unit;
  rstd::json::Value data;
};
constexpr auto rstd_serde_definition(Request *) {
  using namespace rstd::serde;
  return RecordDefinition<
      Request, "Request", Direction::Serialize, UnknownFieldPolicy::Reject,
      Member<&Request::optional, "optional">, Member<&Request::unit, "unit">,
      Member<&Request::data, "data">>{};
}
auto source(ref<str> text) -> luato::LuaModuleSource {
  return {"json-serde"_Str, "test:json-serde"_Str, "json-serde.lua"_Str,
          Vec<u8>::from(text.as_bytes())};
}
auto state() -> luato::State {
  return luato::State::create(luato::StateOptions::build_script()).unwrap();
}
auto matches(const luato::LuaValue &value, const luato::TableHandle &marker)
    -> bool {
  if (!value.is_Table())
    return false;
  auto table = value.as_Table().value.clone().unwrap();
  auto meta = table.metatable().unwrap();
  return meta.is_some() && meta->same_object(marker).unwrap();
}
} // namespace json_serde_tests

TEST(LuatoExtraJsonSerde, SharesStableMarkersWithDecodeAndStrictMark) {
  auto state = json_serde_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto read_options = installed.json->deserialize_options();
  ASSERT_TRUE(read_options.null_value != nullptr);
  ASSERT_TRUE(read_options.object_metatable != nullptr);
  ASSERT_TRUE(read_options.array_metatable != nullptr);
  EXPECT_TRUE(read_options.array_metatable ==
              installed.json->deserialize_options().array_metatable);
  auto options = luato::SerializeOptions::lua(read_options);
  for (auto text : {"null"_str, "{}"_str, "[]"_str}) {
    auto parsed = rstd::json::from_str(text).unwrap();
    auto serialized = luato::to_value(state, parsed, options);
    auto decoded = installed.json->decode(state, parsed);
    ASSERT_TRUE(serialized.is_ok());
    ASSERT_TRUE(decoded.is_ok());
    for (const auto *value : {&*serialized, &*decoded}) {
      auto roundtrip =
          luato::from_value<rstd::json::Value>(*value, read_options);
      ASSERT_TRUE(roundtrip.is_ok());
      EXPECT_EQ(rstd::json::to_string(*roundtrip), text);
      EXPECT_EQ(installed.json->encode(*value, true, true).unwrap(), text);
      if (parsed.is_array())
        EXPECT_TRUE(
            json_serde_tests::matches(*value, *read_options.array_metatable));
      else if (parsed.is_object())
        EXPECT_TRUE(
            json_serde_tests::matches(*value, *read_options.object_metatable));
      else
        EXPECT_TRUE(installed.json->is_null(value->as_Table().value).unwrap());
    }
  }
  auto marked = state.evaluate_entry(json_serde_tests::source(
      "return tools.json.strict.array{}, tools.json.strict.object{}"_str));
  ASSERT_TRUE(marked.is_ok());
  EXPECT_TRUE(json_serde_tests::matches(marked->values()[usize()],
                                        *read_options.array_metatable));
  EXPECT_TRUE(json_serde_tests::matches(marked->values()[usize(1)],
                                        *read_options.object_metatable));
  ASSERT_TRUE(
      state
          .execute_entry(json_serde_tests::source(
              "collectgarbage('collect'); collectgarbage('collect')"_str))
          .is_ok());
  auto after_gc = luato::to_value(state, Vec<i64>{}, options).unwrap();
  EXPECT_TRUE(
      json_serde_tests::matches(after_gc, *read_options.array_metatable));
  auto other = luato::extra::install_utilities(state, "other"_str).unwrap();
  EXPECT_FALSE(
      read_options.array_metatable
          ->same_object(*other.json->deserialize_options().array_metatable)
          .unwrap());
  EXPECT_FALSE(
      read_options.object_metatable
          ->same_object(*other.json->deserialize_options().object_metatable)
          .unwrap());
}

TEST(LuatoExtraJsonSerde, RoundTripsMixedDtoThroughStateAndCallFrame) {
  auto state = json_serde_tests::state();
  auto installed =
      Some(luato::extra::install_utilities(state, "tools"_str).unwrap());
  auto read_options = installed->json->deserialize_options();
  auto options = luato::SerializeOptions::lua(read_options);
  auto data =
      rstd::json::from_str(
          "{\"array\":[],\"nested\":[null,{},[]],\"null\":null,\"object\":{}}"_str)
          .unwrap();
  auto expected =
      "{\"data\":{\"array\":[],\"nested\":[null,{},[]],\"null\":null,\"object\":{}}}"_str;
  auto request = json_serde_tests::Request{None(), {}, data.clone()};
  auto value = luato::to_value(state, request, options);
  ASSERT_TRUE(value.is_ok());
  EXPECT_EQ(installed->json->encode(*value, true, true).unwrap(), expected);
  auto roundtrip = luato::from_value<rstd::json::Value>(*value, read_options);
  ASSERT_TRUE(roundtrip.is_ok());
  EXPECT_EQ(rstd::json::to_string(*roundtrip), expected);

  auto module = luato::ModuleSpec("serde_host"_Str);
  module.raw_function(
      "get"_Str, usize(),
      [context = installed->json.clone(), data = rstd::move(data)](
          luato::CallFrame &frame) -> luato::BindingResult {
        auto options =
            luato::SerializeOptions::lua(context->deserialize_options());
        auto request = json_serde_tests::Request{None(), {}, data.clone()};
        return luato::return_value(frame,
                                   luato::to_value(frame, request, options));
      });
  ASSERT_TRUE(state.register_module(rstd::move(module)).is_ok());
  installed = None();
  auto returned = state.evaluate_entry(json_serde_tests::source(R"(
local value = serde_host.get()
local S = tools.json.strict
assert(value.optional == nil and value.unit == nil)
assert(value.data.null == S.null and value.data.nested[1] == S.null)
assert(S.kind(value.data) == 'object' and S.kind(value.data.array) == 'array')
assert(S.kind(value.data.object) == 'object')
assert(S.kind(value.data.nested[2]) == 'object' and S.kind(value.data.nested[3]) == 'array')
assert(S.encode_canonical(value) == '{"data":{"array":[],"nested":[null,{},[]],"null":null,"object":{}}}')
return value
)"_str));
  ASSERT_TRUE(returned.is_ok());
  auto frame_roundtrip = luato::from_value<rstd::json::Value>(
      returned->values()[usize()], read_options);
  ASSERT_TRUE(frame_roundtrip.is_ok());
  EXPECT_EQ(rstd::json::to_string(*frame_roundtrip), expected);

  auto callable = state
                      .evaluate_entry(json_serde_tests::source(
                          "return function() return serde_host.get() end"_str))
                      .unwrap()
                      .into_values();
  auto runtime = rstd::async::RuntimeBuilder::current_thread()
                     .enable_all()
                     .build()
                     .unwrap();
  auto async_value = runtime.block_on(
      rstd::async::into_future(callable[usize()].as_Function().value.call_async(
          luato::MultiValue::make())));
  ASSERT_TRUE(async_value.is_ok());
  auto async_roundtrip = luato::from_value<rstd::json::Value>(
      async_value->values()[usize()], read_options);
  ASSERT_TRUE(async_roundtrip.is_ok());
  EXPECT_EQ(rstd::json::to_string(*async_roundtrip), expected);
}

TEST(LuatoExtraJsonSerde, SeparatesNoneUnitAndJsonNullWithSerializerFallback) {
  auto state = json_serde_tests::state();
  auto context = luato::extra::JsonContext::create(state).unwrap();
  auto options = luato::SerializeOptions::lua(context.deserialize_options());
  for (unsigned flags = 0; flags < 8; ++flags) {
    options.serialize_none_to_null = (flags & 1) != 0;
    options.serialize_unit_to_null = (flags & 2) != 0;
    options.serialize_json_null_to_null = (flags & 4) != 0;
    auto request =
        json_serde_tests::Request{None(), {}, rstd::json::Value::Null()};
    auto value = luato::to_value(state, request, options);
    ASSERT_TRUE(value.is_ok());
    auto &table = value->as_Table().value;
    auto none = table.raw_get_value("optional"_str).unwrap();
    auto unit = table.raw_get_value("unit"_str).unwrap();
    auto json_null = table.raw_get_value("data"_str).unwrap();
    EXPECT_EQ(none.is_Nil(), !options.serialize_none_to_null);
    EXPECT_EQ(unit.is_Nil(), !options.serialize_unit_to_null);
    EXPECT_EQ(json_null.is_Nil(), !options.serialize_json_null_to_null);
    if (!json_null.is_Nil())
      EXPECT_TRUE(context.is_null(json_null.as_Table().value).unwrap());
  }
  auto fallback = rstd::json::to_value(rstd::json::Value::Null());
  ASSERT_TRUE(fallback.is_ok());
  EXPECT_TRUE(fallback->is_null());
  auto encoded = rstd::json::encode(rstd::json::Value::Null());
  ASSERT_TRUE(encoded.is_ok());
  EXPECT_EQ(*encoded, "null"_str);
}

TEST(LuatoExtraJsonSerde, RejectsExpiredAndForeignMarkerOwners) {
  Option<luato::extra::JsonContext> expired;
  {
    auto state = json_serde_tests::state();
    expired = Some(luato::extra::JsonContext::create(state).unwrap());
  }
  auto state = json_serde_tests::state();
  auto expired_options =
      luato::SerializeOptions::lua(expired->deserialize_options());
  EXPECT_TRUE(luato::to_value(state, Vec<i64>{}, expired_options).is_err());
  EXPECT_TRUE(
      luato::to_value(state, rstd::json::Map{}, expired_options).is_err());
  EXPECT_TRUE(luato::to_value(state, rstd::json::Value::Null(), expired_options)
                  .is_err());
  auto foreign = json_serde_tests::state();
  auto context = luato::extra::JsonContext::create(foreign).unwrap();
  auto options = luato::SerializeOptions::lua(context.deserialize_options());
  EXPECT_TRUE(luato::to_value(state, Vec<i64>{}, options).is_err());
  EXPECT_TRUE(luato::to_value(state, rstd::json::Map{}, options).is_err());
  EXPECT_TRUE(
      luato::to_value(state, rstd::json::Value::Null(), options).is_err());
}
