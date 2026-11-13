module;
#include <rstd/macro.hpp>
export module luato.extra:value;
export import luato;
export import rstd;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra {
inline auto take(luato::CallFrame &frame, usize index)
    -> rstd::Result<luato::LuaValue, luato::Error> {
  auto handle = frame.argument(index);
  if (handle.is_err())
    return Err(rstd::move(handle).unwrap_err());
  return Ok(take_lua_value(*handle));
}
inline auto string_arg(luato::CallFrame &frame, usize index)
    -> rstd::Result<String, luato::Error> {
  auto value = take(frame, index);
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  if (!value->is_Bytes())
    return Err(luato::Error::make(luato::ErrorKind::Type, String::make(),
                                  String::make("expected a string"_str)));
  auto text = rstd::str_::from_utf8(value->as_Bytes().value.as_slice());
  if (text.is_err())
    return Err(luato::Error::make(luato::ErrorKind::Type, String::make(),
                                  String::make("expected a string"_str))
                   .with_cause(rstd::move(text).unwrap_err()));
  return Ok(String::make(*text));
}
inline auto new_table(luato::CallFrame &frame)
    -> rstd::Result<luato::TableHandle, luato::Error> {
  auto value = frame.new_table();
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  auto handle = take_lua_value(*value);
  if (!handle.is_Table())
    return Err(luato::Error::make(luato::ErrorKind::Binding, String::make(),
                                  String::make("expected a new table"_str)));
  return Ok(rstd::move(handle).as_Table().value);
}
inline auto new_table(luato::State &state)
    -> rstd::Result<luato::TableHandle, luato::Error> {
  return state.create_table();
}
inline auto table_arg(luato::CallFrame &frame, usize index)
    -> rstd::Result<luato::TableHandle, luato::Error> {
  auto value = take(frame, index);
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  if (!value->is_Table())
    return Err(luato::Error::make(luato::ErrorKind::Type, String::make(),
                                  String::make("expected a table"_str)));
  return Ok(rstd::move(*value).as_Table().value);
}
inline auto str_key(ref<str> text) -> luato::LuaValue {
  return luato::LuaValue::Bytes(Vec<u8>::from(text.as_bytes()));
}
inline auto str_value(ref<str> text) -> luato::LuaValue {
  return luato::LuaValue::Bytes(Vec<u8>::from(text.as_bytes()));
}
inline auto return_table(luato::CallFrame &frame, luato::TableHandle table)
    -> void {
  frame.return_value(luato::OpaqueHandle{
      new luato::LuaValue(luato::LuaValue::Table(rstd::move(table)))});
}
inline auto fail(ref<str> message) -> luato::Error {
  return luato::Error::make(luato::ErrorKind::Runtime,
                            String::make("luato.extra"_str),
                            String::make(message));
}
template <class E>
inline auto utility_error(E error, ref<str> context, ref<str> source)
    -> luato::Error {
  auto message = rstd::format("{}: {}", context, error);
  return luato::Error::make(luato::ErrorKind::Runtime, String::make(source),
                            rstd::move(message))
      .with_cause(rstd::move(error));
}
inline auto utility_name(ref<str> prefix, ref<str> suffix) -> String {
  auto name = String::make(prefix);
  name.push_ascii('.');
  name.push_str(suffix);
  return name;
}
} // namespace luato::extra
