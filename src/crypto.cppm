module;
#include <rstd/macro.hpp>
export module luato.extra:crypto;
export import luato;
export import rstd;
export import :value;
import licrypto;
using alloc::rc::Rc;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra {
struct CryptoContext {
  Option<luato::FunctionHandle> crypto_rsa_encrypt;
};
inline auto set_fn(luato::TableHandle &table, ref<str> name,
                   const luato::FunctionHandle &function)
    -> rstd::Result<empty, luato::Error> {
  auto copy = function.clone();
  if (copy.is_err())
    return Err(rstd::move(copy).unwrap_err());
  return table.set_in_call(
      str_key(name), luato::LuaValue::Function(rstd::move(copy).unwrap()));
}
inline auto hex_low(const Vec<u8> &bytes) -> String {
  auto out = String::make();
  for (const auto &byte : bytes.as_slice())
    out.push_str(
        rstd::format("{:02x}", unsigned(byte.to_primitive())).as_str());
  return out;
}
inline auto crypto_digest(luato::CallFrame &frame) -> luato::BindingResult {
  auto algorithm = string_arg(frame, usize());
  if (algorithm.is_err())
    return Err(rstd::move(algorithm).unwrap_err());
  auto data = take(frame, usize(1));
  if (data.is_err())
    return Err(rstd::move(data).unwrap_err());
  if (!data->is_Bytes())
    return Err(fail("crypto.digest expects bytes"_str));
  auto bytes = Vec<u8>::make();
  for (const auto &byte : data->as_Bytes().value.as_slice())
    bytes.push(u8(byte.to_primitive()));
  auto out = Vec<u8>::make();
  if (algorithm->as_str() == "md5"_str) {
    auto digest = licrypto::md5(bytes.as_slice());
    if (digest.is_err())
      return Err(utility_error(rstd::move(digest).unwrap_err(),
                               "digest failed"_str, "luato.extra.crypto"_str));
    for (const auto &byte : digest->as_slice())
      out.push(u8(byte.to_primitive()));
  } else if (algorithm->as_str() == "sha1"_str) {
    auto digest = licrypto::sha1(bytes.as_slice());
    if (digest.is_err())
      return Err(utility_error(rstd::move(digest).unwrap_err(),
                               "digest failed"_str, "luato.extra.crypto"_str));
    for (const auto &byte : digest->as_slice())
      out.push(u8(byte.to_primitive()));
  } else {
    return Err(fail("unsupported digest algorithm"_str));
  }
  frame.return_value(luato::OpaqueHandle{
      new luato::LuaValue(luato::LuaValue::Bytes(rstd::move(out)))});
  return Ok(usize());
}
inline auto crypto_encode(luato::CallFrame &frame) -> luato::BindingResult {
  auto data = take(frame, usize());
  if (data.is_err())
    return Err(rstd::move(data).unwrap_err());
  if (!data->is_Bytes())
    return Err(fail("crypto.encode expects bytes"_str));
  auto encoded = licrypto::base64_encode(data->as_Bytes().value.as_slice());
  auto out = String::make();
  auto bytes = encoded.as_str().as_bytes();
  for (usize index{}; index < encoded.as_str().len(); ++index) {
    out.push_ascii(static_cast<char>(bytes[index].to_primitive()));
    if ((index + usize(1)).to_primitive() % 64 == 0)
      out.push_ascii('\n');
  }
  if (!encoded.as_str().is_empty() &&
      encoded.as_str().len().to_primitive() % 64 != 0)
    out.push_ascii('\n');
  frame.push(rstd::move(out));
  return Ok(usize(1));
}
inline auto crypto_encode_block(luato::CallFrame &frame)
    -> luato::BindingResult {
  auto data = take(frame, usize());
  if (data.is_err())
    return Err(rstd::move(data).unwrap_err());
  if (!data->is_Bytes())
    return Err(fail("crypto.encode_block expects bytes"_str));
  frame.push(licrypto::base64_encode(data->as_Bytes().value.as_slice()));
  return Ok(usize(1));
}
inline auto aes_mode(ref<str> name) -> Option<licrypto::AesMode> {
  if (name == "aes-128-cbc"_str)
    return Some(licrypto::AesMode::Cbc);
  if (name == "aes-128-ecb"_str)
    return Some(licrypto::AesMode::Ecb);
  return None();
}
inline auto crypto_encrypt(luato::CallFrame &frame) -> luato::BindingResult {
  auto name = string_arg(frame, usize());
  auto key = take(frame, usize(1));
  auto iv = take(frame, usize(2));
  auto data = take(frame, usize(3));
  if (name.is_err())
    return Err(rstd::move(name).unwrap_err());
  if (key.is_err())
    return Err(rstd::move(key).unwrap_err());
  if (iv.is_err())
    return Err(rstd::move(iv).unwrap_err());
  if (data.is_err())
    return Err(rstd::move(data).unwrap_err());
  if (!key->is_Bytes() || !iv->is_Bytes() || !data->is_Bytes())
    return Err(fail("crypto.encrypt expects bytes"_str));
  auto mode = aes_mode(name->as_str());
  if (mode.is_none())
    return Err(fail("unsupported cipher"_str));
  auto result = licrypto::aes_encrypt(*mode, key->as_Bytes().value.as_slice(),
                                      iv->as_Bytes().value.as_slice(),
                                      data->as_Bytes().value.as_slice());
  if (result.is_err())
    return Err(utility_error(rstd::move(result).unwrap_err(),
                             "cipher encryption failed"_str,
                             "luato.extra.crypto"_str));
  frame.return_value(luato::OpaqueHandle{new luato::LuaValue(
      luato::LuaValue::Bytes(rstd::move(result).unwrap()))});
  return Ok(usize());
}
inline auto crypto_decrypt(luato::CallFrame &frame) -> luato::BindingResult {
  auto name = string_arg(frame, usize());
  auto key = take(frame, usize(1));
  auto iv = take(frame, usize(2));
  auto data = take(frame, usize(3));
  if (name.is_err())
    return Err(rstd::move(name).unwrap_err());
  if (key.is_err())
    return Err(rstd::move(key).unwrap_err());
  if (iv.is_err())
    return Err(rstd::move(iv).unwrap_err());
  if (data.is_err())
    return Err(rstd::move(data).unwrap_err());
  if (!key->is_Bytes() || !iv->is_Bytes() || !data->is_Bytes())
    return Err(fail("crypto.decrypt expects bytes"_str));
  auto mode = aes_mode(name->as_str());
  if (mode.is_none())
    return Err(fail("unsupported cipher"_str));
  auto result = licrypto::aes_decrypt(*mode, key->as_Bytes().value.as_slice(),
                                      iv->as_Bytes().value.as_slice(),
                                      data->as_Bytes().value.as_slice());
  if (result.is_err())
    return Err(utility_error(rstd::move(result).unwrap_err(),
                             "cipher decryption failed"_str,
                             "luato.extra.crypto"_str));
  frame.return_value(luato::OpaqueHandle{new luato::LuaValue(
      luato::LuaValue::Bytes(rstd::move(result).unwrap()))});
  return Ok(usize());
}
inline auto crypto_create_rsa(const CryptoContext &context,
                              luato::CallFrame &frame) -> luato::BindingResult {
  auto key = take(frame, usize());
  if (key.is_err())
    return Err(rstd::move(key).unwrap_err());
  if (!key->is_Bytes())
    return Err(fail("create_rsa expects a PEM key"_str));
  auto table = new_table(frame);
  if (table.is_err())
    return Err(rstd::move(table).unwrap_err());
  auto handle = rstd::move(table).unwrap();
  (void)handle.set_in_call(str_key("_pem"_str), rstd::move(*key));
  if (context.crypto_rsa_encrypt.is_some())
    (void)rstd_try(set_fn(handle, "encrypt"_str, *context.crypto_rsa_encrypt));
  return_table(frame, rstd::move(handle));
  return Ok(usize());
}
inline auto crypto_rsa_encrypt(luato::CallFrame &frame)
    -> luato::BindingResult {
  auto self = table_arg(frame, usize());
  auto data = take(frame, usize(1));
  if (self.is_err())
    return Err(rstd::move(self).unwrap_err());
  if (data.is_err())
    return Err(rstd::move(data).unwrap_err());
  if (!data->is_Bytes())
    return Err(fail("rsa.encrypt expects bytes"_str));
  auto pem = self->get_in_call(str_key("_pem"_str));
  if (pem.is_err() || !pem->is_Bytes())
    return Err(fail("invalid RSA key"_str));
  auto key = licrypto::RsaPublicKey::from_pem(pem->as_Bytes().value.as_slice());
  if (key.is_err())
    return Err(utility_error(rstd::move(key).unwrap_err(),
                             "invalid RSA public key"_str,
                             "luato.extra.crypto"_str));
  auto result = key->encrypt(data->as_Bytes().value.as_slice());
  if (result.is_err())
    return Err(utility_error(rstd::move(result).unwrap_err(),
                             "RSA encryption failed"_str,
                             "luato.extra.crypto"_str));
  frame.return_value(luato::OpaqueHandle{new luato::LuaValue(
      luato::LuaValue::Bytes(rstd::move(result).unwrap()))});
  return Ok(usize());
}
inline auto crypto_decode(luato::CallFrame &frame) -> luato::BindingResult {
  auto data = take(frame, usize());
  if (data.is_err())
    return Err(rstd::move(data).unwrap_err());
  if (!data->is_Bytes())
    return Err(fail("crypto.decode expects bytes"_str));
  auto text = String::from_utf8_unchecked(
      Vec<u8>::from(data->as_Bytes().value.as_slice()));
  auto decoded = licrypto::base64_decode(text.as_str());
  if (decoded.is_err())
    return Err(utility_error(rstd::move(decoded).unwrap_err(),
                             "invalid base64"_str, "luato.extra.crypto"_str));
  frame.return_value(luato::OpaqueHandle{new luato::LuaValue(
      luato::LuaValue::Bytes(rstd::move(decoded).unwrap()))});
  return Ok(usize());
}
inline auto crypto_hex_up(luato::CallFrame &frame) -> luato::BindingResult {
  auto value = take(frame, usize());
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  if (!value->is_Bytes())
    return Err(fail("hex.encode_up expects bytes"_str));
  auto out = String::make();
  for (const auto &byte : value->as_Bytes().value.as_slice())
    out.push_str(
        rstd::format("{:02X}", unsigned(byte.to_primitive())).as_str());
  frame.push(rstd::move(out));
  return Ok(usize(1));
}
inline auto crypto_modules(Rc<CryptoContext> context, ref<str> prefix)
    -> Vec<luato::ModuleSpec> {
  auto crypto = luato::ModuleSpec(utility_name(prefix, "crypto"_str));
  crypto.raw_function(String::make("digest"_str), usize(2),
                      [](luato::CallFrame &frame) -> luato::BindingResult {
                        return crypto_digest(frame);
                      });
  crypto.raw_function(String::make("encode"_str), usize(1),
                      [](luato::CallFrame &frame) -> luato::BindingResult {
                        return crypto_encode(frame);
                      });
  crypto.raw_function(String::make("decode"_str), usize(1),
                      [](luato::CallFrame &frame) -> luato::BindingResult {
                        return crypto_decode(frame);
                      });
  crypto.raw_function(String::make("encode_block"_str), usize(1),
                      [](luato::CallFrame &frame) -> luato::BindingResult {
                        return crypto_encode_block(frame);
                      });
  crypto.raw_function(String::make("encrypt"_str), usize(4),
                      [](luato::CallFrame &frame) -> luato::BindingResult {
                        return crypto_encrypt(frame);
                      });
  crypto.raw_function(String::make("decrypt"_str), usize(4),
                      [](luato::CallFrame &frame) -> luato::BindingResult {
                        return crypto_decrypt(frame);
                      });
  crypto.raw_function(String::make("create_rsa"_str), usize(1),
                      [context = context.clone()](
                          luato::CallFrame &frame) -> luato::BindingResult {
                        return crypto_create_rsa(*context, frame);
                      });
  crypto.raw_function(String::make("rsa_encrypt"_str), usize(2),
                      [](luato::CallFrame &frame) -> luato::BindingResult {
                        return crypto_rsa_encrypt(frame);
                      });
  auto crypto_hex = luato::ModuleSpec(utility_name(prefix, "crypto.hex"_str));
  crypto_hex.raw_function(
      String::make("encode_low"_str), usize(1),
      [](luato::CallFrame &frame) -> luato::BindingResult {
        auto value = take(frame, usize());
        if (value.is_err())
          return Err(rstd::move(value).unwrap_err());
        if (!value->is_Bytes())
          return Err(fail("hex.encode_low expects bytes"_str));
        auto bytes = Vec<u8>::make();
        for (const auto &byte : value->as_Bytes().value.as_slice())
          bytes.push(u8(byte.to_primitive()));
        frame.push(hex_low(bytes));
        return Ok(usize(1));
      });
  crypto_hex.raw_function(String::make("encode_up"_str), usize(1),
                          [](luato::CallFrame &frame) -> luato::BindingResult {
                            return crypto_hex_up(frame);
                          });
  Vec<luato::ModuleSpec> modules;
  modules.push(rstd::move(crypto));
  modules.push(rstd::move(crypto_hex));
  return modules;
}
} // namespace luato::extra
