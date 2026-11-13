#include <rstd/test/gtest.hpp>

import luato.extra;
import licrypto;

using namespace rstd::prelude;
using namespace rstd::literals;

namespace tools_tests {
auto source(ref<str> text) -> luato::LuaModuleSource {
  return {"tools"_Str, "tools:test"_Str, "tools.lua"_Str,
          Vec<u8>::from(text.as_bytes())};
}
auto state() -> luato::State {
  return luato::State::create(luato::StateOptions::build_script()).unwrap();
}
} // namespace tools_tests

TEST(LuatoExtraTools, InstallsUnderIndependentPrefixesAndPreservesRoot) {
  auto state = tools_tests::state();
  auto root = luato::ModuleSpec("tools"_Str);
  root.set("existing"_Str, "kept"_Str);
  ASSERT_TRUE(state.register_module(rstd::move(root)).is_ok());
  auto first = luato::extra::install_utilities(state, "tools"_str);
  auto second = luato::extra::install_utilities(state, "other"_str);
  ASSERT_TRUE(first.is_ok());
  ASSERT_TRUE(second.is_ok());
  auto result = state.execute_entry(tools_tests::source(R"(
assert(qcm == nil and tools.existing == 'kept')
assert(tools.json ~= other.json and tools.null ~= other.null)
assert(tools.null == tools.json.strict.null)
assert(tools.crypto.url == tools.url)
assert(tools.encode_json == tools.json.encode)
assert(_G['tools.json'] == tools.json)
assert(_G['tools.json.strict'] == tools.json.strict)
assert(_G['tools.crypto.hex'] == tools.crypto.hex)
)"_str));
  ASSERT_TRUE(result.is_ok());
  EXPECT_TRUE(luato::extra::install_utilities(state, "tools"_str).is_err());
  EXPECT_TRUE(luato::extra::install_utilities(state, ""_str).is_err());
}

TEST(LuatoExtraTools, PreservesJsonNullAndEmptyKinds) {
  auto state = tools_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto result = state.execute_entry(tools_tests::source(R"(
local J, S = tools.json, tools.json.strict
assert(J.decode('null') == nil and S.decode('null') == S.null)
assert(J.decode('{"a":null}').a == nil)
local value = S.decode('{"a":null,"items":[],"object":{}}')
assert(value.a == S.null and S.kind(value) == 'object')
assert(S.kind(value.items) == 'array' and S.kind(value.object) == 'object')
assert(S.encode_canonical(value) == '{"a":null,"items":[],"object":{}}')
assert(J.encode(nil) == 'null' and J.encode({}) == '{}')
assert(J.encode(S.array{}) == '[]' and J.encode(S.object{}) == '{}')
local empty = {}
assert(J.object(empty) == empty and J.array(empty) == empty)
assert(S.kind(empty) == nil)
assert(not pcall(S.encode, nil) and not pcall(S.encode, {}))
assert(not pcall(S.encode, {[2]='sparse'}))
assert(not pcall(S.encode, S.array{[1]='a', extra='b'}))
assert(not pcall(S.encode, {[true]='bad key'}))
assert(S.encode_canonical(S.object{z=1.0, a=2}) == '{"a":2,"z":1}')
assert(S.encode(S.object{value=S.null}) == '{"value":null}')
local nested = S.array{}
for i=1,66 do nested = S.array{nested} end
assert(not pcall(S.encode, nested))
assert(not pcall(J.encode, string.char(255)))
)"_str));
  ASSERT_TRUE(result.is_ok());
}

TEST(LuatoExtraTools, JsonMarkersFollowMetatablesAndDoNotRetainTables) {
  auto state = tools_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto result = state.execute_entry(tools_tests::source(R"(
local S = tools.json.strict
local value = S.array{}
assert(getmetatable(value).__mlua_extra_json_kind == 'array')
S.object(value)
assert(S.kind(value) == 'object' and S.encode(value) == '{}')
setmetatable(value, {})
assert(S.kind(value) == nil and not pcall(S.encode, value))
setmetatable(value, {__mlua_extra_json_kind='unexpected'})
assert(S.kind(value) == 'unexpected' and not pcall(S.encode, value))
local weak = setmetatable({}, {__mode='k'})
for i=1,100 do
  weak[S.array{}] = true
  weak[S.decode('{"items":[]}')] = true
end
collectgarbage('collect')
collectgarbage('collect')
assert(next(weak) == nil)
)"_str));
  ASSERT_TRUE(result.is_ok());
}

TEST(LuatoExtraTools, JsonCodecAcceptsParsedValuesAndSharesDecoderIdentity) {
  auto state = tools_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto parsed = rstd::json::from_str("{\"items\":[],\"a\":null}"_str).unwrap();
  auto value = installed.json->decode(state, parsed);
  ASSERT_TRUE(value.is_ok());
  auto encoded = installed.json->encode(*value, true, true);
  ASSERT_TRUE(encoded.is_ok());
  EXPECT_EQ(*encoded, "{\"a\":null,\"items\":[]}"_str);
  auto marker = installed.json->null_value().unwrap();
  ASSERT_TRUE(marker.is_Table());
  EXPECT_TRUE(installed.json->is_null(marker.as_Table().value).unwrap());
  EXPECT_TRUE(luato::from_value<Option<String>>(
                  marker, installed.json->deserialize_options())
                  .unwrap()
                  .is_none());
  auto args = luato::MultiValue::make();
  args.push(luato::LuaValue::Bytes(Vec<u8>::from("null"_str.as_bytes())));
  auto strict = installed.strict_json_decode.call_values(rstd::move(args));
  ASSERT_TRUE(strict.is_ok());
  EXPECT_TRUE(
      installed.json->is_null(strict->values()[usize()].as_Table().value)
          .unwrap());
}

TEST(LuatoExtraTools, PreservesUrlParsingAndByteEncoding) {
  auto state = tools_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto result = state.execute_entry(tools_tests::source(R"(
local U = tools.url
assert(U.encode_path_segment('a b/~') == 'a%20b%2F~')
assert(U.encode_component('a b/~*') == 'a+b%2F%7E*')
assert(U.decode_component('a+b%2F') == 'a b/')
assert(U.percent_decode('a+b%00%FF') == 'a+b'..string.char(0,255))
assert(not pcall(U.percent_decode, '%Q1'))
assert(not pcall(U.decode_component, '%'))
local query = U.decode('a=1;a=2&b=a+b&bad=%Q1')
assert(query.a == '2' and query.b == 'a b' and query.bad == '%Q1')
assert(U.encode_query_pairs{{'a','b c'},{'a','/'},{'n',true}} == 'a=b+c&a=%2F&n=true')
assert(U.encode({x='a b'}) == 'x=a+b')
local p = U.parse('HTTPS://user:pass@EXAMPLE.COM:8443/a?b=c#d')
assert(p.scheme == 'https' and p.host == 'example.com' and p.port == 8443)
assert(p.userinfo == 'user:pass' and p.path == '/a' and p.query == 'b=c' and p.fragment == 'd')
assert(U.parse('relative') == nil and U.host('relative') == nil)
assert(U.host('https://EXAMPLE.COM/x') == 'example.com')
assert(U.join('https://example.com/a/b', '../c') == 'https://example.com/c')
assert(U.append_api_path('https://example.com/api/', '/v1/items') == 'https://example.com/api/v1/items')
assert(not pcall(U.append_api_path, 'https://example.com?q=1', 'v1'))
assert(not pcall(U.append_api_path, 'https://example.com', '%2e%2e/private'))
assert(not pcall(U.append_api_path, 'https://example.com', '//other'))
)"_str));
  ASSERT_TRUE(result.is_ok());
}

TEST(LuatoExtraTools, PreservesTimeMillisecondsAndRandomBounds) {
  auto state = tools_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto result = state.execute_entry(tools_tests::source(R"(
local T, R = tools.time, tools.random
assert(T.parse_rfc3339('1970-01-01T00:00:00Z') == 0)
assert(T.parse_rfc3339('1970-01-01T01:00:00.123456+01:00') == 123)
assert(T.parse_rfc3339('1969-12-31 23:59:59.999z') == -1)
assert(T.parse_rfc3339('1970-01-01t00:00:00-01:00') == 3600000)
assert(not pcall(T.parse_rfc3339, '1970-13-01T00:00:00Z'))
assert(not pcall(T.parse_rfc3339, '1970-01-01T00:00:00'))
assert(type(T.now()) == 'number' and T.now() > 0)
assert(R.hex(0) == '')
assert(#R.hex(256) == 512 and R.hex(16):match('^[0-9a-f]+$'))
assert(not pcall(R.hex, -1) and not pcall(R.hex, 257))
assert(not pcall(R.hex, 1.5) and not pcall(R.hex, '16'))
)"_str));
  ASSERT_TRUE(result.is_ok());
}

TEST(LuatoExtraTools, PreservesCryptoAlgorithmsAndBinaryValues) {
  auto state = tools_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto result = state.execute_entry(tools_tests::source(R"(
local C = tools.crypto
assert(C.hex.encode_low(C.digest('md5', 'abc')) == '900150983cd24fb0d6963f7d28e17f72')
assert(C.hex.encode_up(C.digest('sha1', 'abc')) == 'A9993E364706816ABA3E25717850C26C9CD0D89D')
assert(not pcall(C.digest, 'sha256', 'abc'))
assert(C.encode('abc') == 'YWJj\n' and C.encode_block('abc') == 'YWJj')
assert(C.encode(string.rep('a',48)) == C.encode_block(string.rep('a',48))..'\n')
assert(C.encode('') == '' and C.decode('YWJj') == 'abc')
local binary = string.char(0,255,128,1)
assert(C.decode(C.encode_block(binary)) == binary)
assert(C.hex.encode_low(binary) == '00ff8001' and C.hex.encode_up(binary) == '00FF8001')
local key, iv = '0123456789abcdef', 'abcdef0123456789'
for _, mode in ipairs{'aes-128-cbc', 'aes-128-ecb'} do
  local ciphertext = C.encrypt(mode, key, iv, binary)
  assert(C.decrypt(mode, key, iv, ciphertext) == binary)
end
assert(not pcall(C.encrypt, 'aes-256-cbc', key, iv, binary))
assert(not pcall(C.decrypt, 'aes-128-cbc', key, iv, 'bad'))
)"_str));
  ASSERT_TRUE(result.is_ok());
}

TEST(LuatoExtraTools, PreservesRsaFactoryAndCachedMethod) {
  auto state = tools_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto result = state.execute_entry(tools_tests::source(R"(
local pem = [==[-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEArGcMNZNfb0L88T8lkbf8
2JwmfzSPSgS60+owWWnungDFjYBLeXZKlhvIPXndFGvMzabcRwpHyY+7/P024O3M
tSQuwNkXDFJRIGPwvw8nnzEN8GkRRqoCCGOJ4207T7/hlBK6mkvqAyCFxScPoWip
vFnYxZDnwY6U4+j+aMaNSqiM+6CsWRidUfUAZ5gsrddQvbwLtuJnEnDoppY3pfDF
ynhSPOnJZIFZKzA4Ilr8wYrkDKpv9gFzQMN9FYH7ujMHRBsJwxxcNB1X7exQrF1F
heM47GGSIQgIi5LUZtsPddTc5mNRNcyHRnQUd2NcoUAFC8uBMwAmmIeCA4sswxPX
BQIDAQAB
-----END PUBLIC KEY-----
]==]
local first = tools.crypto.create_rsa(pem)
local second = tools.crypto.create_rsa(pem)
assert(first.encrypt == second.encrypt)
assert(#first:encrypt(string.rep('\0',256)) == 256)
assert(not pcall(first.encrypt, first, 'too short'))
local invalid = tools.crypto.create_rsa('bad key')
assert(not pcall(invalid.encrypt, invalid, 'data'))
)"_str));
  ASSERT_TRUE(result.is_ok());
}

TEST(LuatoExtraTools, RetainsNativeJsonAndCryptoCausesAcrossLuaErrors) {
  auto state = tools_tests::state();
  auto installed = luato::extra::install_utilities(state, "tools"_str).unwrap();
  auto json =
      state.execute_entry(tools_tests::source("tools.json.decode('{')"_str));
  ASSERT_TRUE(json.is_err());
  auto json_error = rstd::move(json).unwrap_err();
  auto json_cause = json_error.native_cause();
  ASSERT_TRUE(json_cause.is_some());
  EXPECT_TRUE(
      rstd::error::downcast_ref<rstd::json::Error>(*json_cause).is_some());
  auto crypto =
      state.execute_entry(tools_tests::source("tools.crypto.decode('!')"_str));
  ASSERT_TRUE(crypto.is_err());
  auto crypto_error = rstd::move(crypto).unwrap_err();
  auto crypto_cause = crypto_error.native_cause();
  ASSERT_TRUE(crypto_cause.is_some());
  EXPECT_TRUE(
      rstd::error::downcast_ref<licrypto::Base64DecodeError>(*crypto_cause)
          .is_some());
  auto copy = crypto_error.clone();
  EXPECT_TRUE(copy.native_cause().is_some());
  auto utf8 = state.execute_entry(
      tools_tests::source("tools.json.encode(string.char(255))"_str));
  ASSERT_TRUE(utf8.is_err());
  auto utf8_error = rstd::move(utf8).unwrap_err();
  EXPECT_TRUE(utf8_error.native_cause().is_some());
  auto cipher = state.execute_entry(tools_tests::source(
      "tools.crypto.encrypt('aes-128-cbc', 'short', 'short', 'data')"_str));
  ASSERT_TRUE(cipher.is_err());
  auto cipher_error = rstd::move(cipher).unwrap_err();
  auto cipher_cause = cipher_error.native_cause();
  ASSERT_TRUE(cipher_cause.is_some());
  EXPECT_TRUE(
      rstd::error::downcast_ref<licrypto::Error>(*cipher_cause).is_some());
}

TEST(LuatoExtraTools, HandlesAndJsonMarkerExpireWithVm) {
  Option<luato::extra::UtilityInstallation> installed;
  {
    auto state = tools_tests::state();
    installed =
        Some(luato::extra::install_utilities(state, "tools"_str).unwrap());
  }
  auto marker = installed->json->null_value().unwrap();
  EXPECT_TRUE(installed->json->encode(marker).is_err());
  EXPECT_TRUE(
      installed->json_decode.call_values(luato::MultiValue::make()).is_err());
}

TEST(LuatoExtraTools, RejectsJsonTablesFromAnotherVm) {
  auto first = tools_tests::state();
  auto second = tools_tests::state();
  auto installed = luato::extra::install_utilities(first, "tools"_str).unwrap();
  auto foreign = luato::LuaValue::Table(second.create_table().unwrap());
  EXPECT_TRUE(installed.json->is_null(foreign.as_Table().value).is_err());
  EXPECT_TRUE(installed.json->encode(foreign).is_err());
}

TEST(LuatoExtraTools, RejectsConflictingRootBeforeRegisteringModules) {
  auto state = tools_tests::state();
  ASSERT_TRUE(
      state.execute_entry(tools_tests::source("tools = false"_str)).is_ok());
  EXPECT_TRUE(luato::extra::install_utilities(state, "tools"_str).is_err());
  auto globals = state.globals().unwrap();
  EXPECT_TRUE(globals.raw_get_value("tools.json"_str).unwrap().is_Nil());
}
