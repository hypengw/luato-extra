module;
#include <rstd/macro.hpp>
export module luato.extra:utilities;
export import :json;
export import :url;
export import :time;
export import :random;
export import :crypto;
using alloc::rc::make_rc;
using alloc::rc::Rc;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra {
struct UtilityInstallation {
  Rc<JsonContext> json;
  luato::TableHandle root;
  luato::FunctionHandle json_encode;
  luato::FunctionHandle json_decode;
  luato::FunctionHandle strict_json_decode;
};

inline auto install_utilities(luato::State &state, ref<str> prefix)
    -> luato::Result<UtilityInstallation> {
  if (prefix.is_empty())
    return Err(luato::Error::binding(
        String::make("utility prefix cannot be empty"_str)));
  auto globals = rstd_try(state.globals());
  auto existing = rstd_try(globals.raw_get_value(prefix));
  if (!existing.is_Table() && !existing.is_Nil())
    return Err(luato::Error::binding(
        String::make("utility root must be a table"_str)));
  auto json = make_rc<JsonContext>(rstd_try(JsonContext::create(state)));
  auto crypto = make_rc<CryptoContext>();
  auto modules = json_modules(json.clone(), prefix);
  modules.push(url_module(prefix));
  modules.push(time_module(prefix));
  modules.push(random_module(prefix));
  for (auto &module : crypto_modules(crypto.clone(), prefix))
    modules.push(rstd::move(module));
  for (auto &module : modules)
    (void)rstd_try(state.register_module(rstd::move(module)));

  auto root = [&]() -> luato::Result<luato::TableHandle> {
    if (existing.is_Table())
      return Ok(rstd::move(existing).as_Table().value);
    auto table = rstd_try(state.create_table());
    (void)rstd_try(globals.set(
        str_key(prefix), luato::LuaValue::Table(rstd_try(table.clone()))));
    return Ok(rstd::move(table));
  }();
  if (root.is_err())
    return Err(rstd::move(root).unwrap_err());
  auto table = [&](ref<str> suffix) -> luato::Result<luato::TableHandle> {
    return globals.get_table(utility_name(prefix, suffix).as_str());
  };
  auto json_table = rstd_try(table("json"_str));
  auto strict_table = rstd_try(table("json.strict"_str));
  auto url_table = rstd_try(table("url"_str));
  auto crypto_table = rstd_try(table("crypto"_str));
  auto crypto_hex_table = rstd_try(table("crypto.hex"_str));
  auto encode = rstd_try(json_table.get_function("encode"_str));
  auto decode = rstd_try(json_table.get_function("decode"_str));
  auto strict_decode = rstd_try(strict_table.get_function("decode"_str));
  crypto.get()->crypto_rsa_encrypt =
      Some(rstd_try(crypto_table.get_function("rsa_encrypt"_str)));

  auto set_table = [](luato::TableHandle &target, ref<str> name,
                      luato::TableHandle value) -> luato::Result<empty> {
    return target.set(str_key(name), luato::LuaValue::Table(rstd::move(value)));
  };
  (void)rstd_try(
      strict_table.set(str_key("null"_str), rstd_try(json->null_value())));
  (void)rstd_try(set_table(json_table, "strict"_str, rstd::move(strict_table)));
  (void)rstd_try(
      set_table(crypto_table, "hex"_str, rstd::move(crypto_hex_table)));
  (void)rstd_try(
      set_table(crypto_table, "url"_str, rstd_try(url_table.clone())));
  (void)rstd_try(set_table(*root, "json"_str, rstd::move(json_table)));
  (void)rstd_try(set_table(*root, "url"_str, rstd::move(url_table)));
  (void)rstd_try(set_table(*root, "time"_str, rstd_try(table("time"_str))));
  (void)rstd_try(set_table(*root, "random"_str, rstd_try(table("random"_str))));
  (void)rstd_try(set_table(*root, "crypto"_str, rstd::move(crypto_table)));
  (void)rstd_try(root->set(str_key("null"_str), rstd_try(json->null_value())));
  (void)rstd_try(
      root->set(str_key("encode_json"_str),
                luato::LuaValue::Function(rstd_try(encode.clone()))));
  return Ok(UtilityInstallation{rstd::move(json), rstd::move(*root),
                                rstd::move(encode), rstd::move(decode),
                                rstd::move(strict_decode)});
}
} // namespace luato::extra
