module;
#include <rstd/macro.hpp>
export module luato.extra:json;
export import luato;
export import rstd;
export import rstd.json;
export import :value;
using alloc::rc::Rc;
using rstd::tuple;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra {
class JsonContext {
public:
  static auto create(luato::State &state) -> luato::Result<JsonContext> {
    auto marker = rstd_try(state.create_table());
    return Ok(JsonContext(rstd::move(marker)));
  }
  auto is_null(const luato::TableHandle &table) const -> luato::Result<bool> {
    return table.same_object_in_call(null_table_);
  }
  auto null_value() const -> luato::Result<luato::LuaValue> {
    auto marker = rstd_try(null_table_.clone());
    return Ok(luato::LuaValue::Table(rstd::move(marker)));
  }
  auto deserialize_options() const &[[clang::lifetimebound]]
  -> luato::DeserializeOptions {
    return {rstd::addressof(null_table_)};
  }
  auto deserialize_options() const && -> luato::DeserializeOptions = delete;
  auto encode(const luato::LuaValue &value, bool strict = false,
              bool canonical = false) const -> luato::Result<String>;
  template <class Source>
  auto decode(Source &source, const rstd::json::Value &value,
              bool null_as_nil = false) const -> luato::Result<luato::LuaValue>;
  template <class Source>
  auto decode(Source &source, ref<str> text, bool null_as_nil = false) const
      -> luato::Result<luato::LuaValue>;

private:
  explicit JsonContext(luato::TableHandle marker)
      : null_table_(rstd::move(marker)) {}
  luato::TableHandle null_table_;
};
template <class E>
inline auto json_error(E error, ref<str> context) -> luato::Error {
  return utility_error(rstd::move(error), context, "luato.extra.json"_str);
}
inline auto escape(ref<str> text, String &out) -> void {
  Vec<u8> result;
  result.push(u8(static_cast<unsigned char>('"')));
  for (const auto &byte : text.as_bytes()) {
    auto c = byte.to_primitive();
    switch (c) {
    case '"':
      result.push(u8(static_cast<unsigned char>('\\')));
      result.push(u8(static_cast<unsigned char>('"')));
      break;
    case '\\':
      result.push(u8(static_cast<unsigned char>('\\')));
      result.push(u8(static_cast<unsigned char>('\\')));
      break;
    case '\n':
      result.push(u8(static_cast<unsigned char>('\\')));
      result.push(u8(static_cast<unsigned char>('n')));
      break;
    case '\r':
      result.push(u8(static_cast<unsigned char>('\\')));
      result.push(u8(static_cast<unsigned char>('r')));
      break;
    case '\t':
      result.push(u8(static_cast<unsigned char>('\\')));
      result.push(u8(static_cast<unsigned char>('t')));
      break;
    default:
      if (c < 0x20) {
        auto escaped = rstd::format("\\u{:04x}", unsigned(c));
        for (const auto &item : escaped.as_str().as_bytes())
          result.push(u8(item.to_primitive()));
      } else
        result.push(u8(c));
    }
  }
  result.push(u8(static_cast<unsigned char>('"')));
  out.push_str(String::from_utf8_unchecked(rstd::move(result)).as_str());
}
template <class Source>
inline auto mark_table(Source &frame, luato::TableHandle &table, bool array)
    -> rstd::Result<empty, luato::Error> {
  auto meta = new_table(frame);
  if (meta.is_err())
    return Err(rstd::move(meta).unwrap_err());
  auto set = meta->set_in_call(str_key("__mlua_extra_json_kind"_str),
                               str_value(array ? "array"_str : "object"_str));
  if (set.is_err())
    return Err(rstd::move(set).unwrap_err());
  return table.set_metatable_in_call(&*meta);
}
inline auto table_kind(luato::TableHandle &table)
    -> rstd::Result<Option<String>, luato::Error> {
  auto meta = table.metatable_in_call();
  if (meta.is_err())
    return Err(rstd::move(meta).unwrap_err());
  if (meta->is_none())
    return Ok<Option<String>>(None());
  auto value = (**meta).get_in_call(str_key("__mlua_extra_json_kind"_str));
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  if (value->is_Nil())
    return Ok<Option<String>>(None());
  auto text = value->as_utf8();
  if (text.is_err())
    return Err(fail("invalid JSON table kind"_str));
  return Ok(Some(String::make(*text)));
}

inline auto encode_json_value(const JsonContext &context,
                              const luato::LuaValue &value, String &out,
                              usize depth, bool strict, bool canonical)
    -> rstd::Result<empty, luato::Error> {
  if (depth > usize(64))
    return Err(fail("Lua value nesting is too deep"_str));
  if (value.is_Nil()) {
    if (strict)
      return Err(fail("nil is not a strict JSON value"_str));
    out.push_str("null"_str);
    return Ok(empty{});
  }
  if (value.is_Boolean()) {
    out.push_str(value.as_Boolean().value ? "true"_str : "false"_str);
    return Ok(empty{});
  }
  if (value.is_Integer()) {
    out.push_str(
        rstd::format("{}", value.as_Integer().value.to_primitive()).as_str());
    return Ok(empty{});
  }
  if (value.is_Number()) {
    auto number = value.as_Number().value;
    auto primitive = number.to_primitive();
    if (canonical && number.fract() == f64() &&
        number.abs() <= f64(9007199254740991.0))
      out.push_str(
          rstd::format("{}", static_cast<long long>(primitive)).as_str());
    else
      out.push_str(rstd::format("{}", number).as_str());
    return Ok(empty{});
  }
  if (value.is_Bytes()) {
    auto text = rstd::str_::from_utf8(value.as_Bytes().value.as_slice());
    if (text.is_err())
      return Err(json_error(rstd::move(text).unwrap_err(),
                            "JSON string is not valid UTF-8"_str));
    escape(*text, out);
    return Ok(empty{});
  }
  if (!value.is_Table())
    return Err(fail("unsupported Lua value in JSON"_str));
  auto table = value.as_Table().value.clone();
  if (table.is_err())
    return Err(rstd::move(table).unwrap_err());
  auto handle = rstd::move(table).unwrap();
  auto same = context.is_null(handle);
  if (same.is_err())
    return Err(rstd::move(same).unwrap_err());
  if (*same) {
    out.push_str("null"_str);
    return Ok(empty{});
  }
  auto length = handle.length();
  if (length.is_err())
    return Err(rstd::move(length).unwrap_err());
  auto keys = handle.keys();
  if (keys.is_err())
    return Err(rstd::move(keys).unwrap_err());
  auto marked = table_kind(handle);
  if (marked.is_err())
    return Err(rstd::move(marked).unwrap_err());
  if (marked->is_some() && **marked != "array"_str && **marked != "object"_str)
    return Err(fail("unknown JSON table kind"_str));
  bool array = marked->is_some()
                   ? **marked == "array"_str
                   : (strict ? !keys->is_empty() : *length > usize());
  if (marked->is_none()) {
    for (const auto &key : keys->as_slice()) {
      if (!key.is_Integer()) {
        array = false;
        break;
      }
      if (!strict && (key.as_Integer().value.to_primitive() < 1 ||
                      static_cast<unsigned long long>(
                          key.as_Integer().value.to_primitive()) >
                          length->to_primitive())) {
        array = false;
        break;
      }
    }
    if (strict && keys->is_empty())
      return Err(fail("empty table requires json.array or json.object"_str));
  }
  if (array) {
    if (strict) {
      if (keys->len() != *length)
        return Err(fail("JSON array must be a dense sequence"_str));
      for (const auto &key : keys->as_slice()) {
        if (!key.is_Integer() || key.as_Integer().value.to_primitive() < 1 ||
            static_cast<unsigned long long>(
                key.as_Integer().value.to_primitive()) > length->to_primitive())
          return Err(fail("JSON array must be a dense sequence"_str));
      }
    }
    out.push_ascii('[');
    for (usize index{}; index < *length; ++index) {
      if (index != usize())
        out.push_ascii(',');
      auto item = handle.get_in_call(luato::LuaValue::Integer(
          i64(static_cast<long long>(index.to_primitive() + 1))));
      if (item.is_err())
        return Err(rstd::move(item).unwrap_err());
      auto step = encode_json_value(context, *item, out, depth + usize(1),
                                    strict, canonical);
      if (step.is_err())
        return Err(rstd::move(step).unwrap_err());
    }
    out.push_ascii(']');
    return Ok(empty{});
  }
  Vec<tuple<String, luato::LuaValue>> entries;
  for (const auto &key : keys->as_slice()) {
    if (!key.is_Bytes())
      return Err(fail("JSON object keys must be strings"_str));
    auto text = rstd::str_::from_utf8(key.as_Bytes().value.as_slice());
    if (text.is_err())
      return Err(json_error(rstd::move(text).unwrap_err(),
                            "JSON key is not valid UTF-8"_str));
    auto item = handle.get_in_call(key);
    if (item.is_err())
      return Err(rstd::move(item).unwrap_err());
    entries.push({String::make(*text), rstd::move(*item)});
  }
  if (canonical)
    rstd::slice_::sort_by(
        entries.as_mut_slice().as_mut_ref(),
        [](const auto &left, const auto &right) {
          return rstd::iter::from_slice(rstd::get<0>(left).as_str().as_bytes())
              .copied()
              .cmp(rstd::iter::from_slice(
                       rstd::get<0>(right).as_str().as_bytes())
                       .copied());
        });
  out.push_ascii('{');
  bool first = true;
  for (auto &entry : entries) {
    if (!first)
      out.push_ascii(',');
    first = false;
    escape(rstd::get<0>(entry).as_str(), out);
    out.push_ascii(':');
    auto step = encode_json_value(context, rstd::get<1>(entry), out,
                                  depth + usize(1), strict, canonical);
    if (step.is_err())
      return Err(rstd::move(step).unwrap_err());
  }
  out.push_ascii('}');
  return Ok(empty{});
}
template <class Source>
inline auto decode_json_value(const JsonContext &context, Source &frame,
                              const rstd::json::Value &value, usize depth,
                              bool null_as_nil = false)
    -> rstd::Result<luato::LuaValue, luato::Error> {
  if (depth > usize(64))
    return Err(fail("JSON nesting is too deep"_str));
  if (value.is_Null()) {
    if (null_as_nil)
      return Ok(luato::LuaValue::Nil());
    return context.null_value();
  }
  if (value.is_Bool())
    return Ok(luato::LuaValue::Boolean(*value.as_bool()));
  if (value.as_i64().is_some())
    return Ok(luato::LuaValue::Integer(i64(*value.as_i64())));
  if (value.is_Number())
    return Ok(luato::LuaValue::Number(*value.as_f64()));
  if (value.is_String()) {
    auto text = value.as_str().unwrap();
    return Ok(str_value(text));
  }
  auto table = new_table(frame);
  if (table.is_err())
    return Err(rstd::move(table).unwrap_err());
  auto handle = rstd::move(table).unwrap();
  auto marked = mark_table(frame, handle, value.is_Array());
  if (marked.is_err())
    return Err(rstd::move(marked).unwrap_err());
  if (value.is_Array()) {
    auto rows = value.as_array().unwrap();
    for (usize index{}; index < rows->len(); ++index) {
      auto child = decode_json_value(context, frame, rows->as_slice()[index],
                                     depth + usize(1), null_as_nil);
      if (child.is_err())
        return Err(rstd::move(child).unwrap_err());
      (void)rstd_try(handle.set_in_call(
          luato::LuaValue::Integer(
              i64(static_cast<long long>(index.to_primitive() + 1))),
          *child));
    }
  } else {
    auto object = value.as_object().unwrap();
    for (auto entry : object->iter()) {
      auto name = rstd::get<0>(entry);
      auto decoded = decode_json_value(context, frame, *rstd::get<1>(entry),
                                       depth + usize(1), null_as_nil);
      if (decoded.is_err())
        return Err(rstd::move(decoded).unwrap_err());
      (void)rstd_try(handle.set_in_call(str_value(name->as_str()), *decoded));
    }
  }
  return Ok(luato::LuaValue::Table(rstd::move(handle)));
}

inline auto json_encode(const JsonContext &context, luato::CallFrame &frame)
    -> rstd::Result<String, luato::Error> {
  auto value = take(frame, usize());
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  return context.encode(*value);
}
inline auto JsonContext::encode(const luato::LuaValue &value, bool strict,
                                bool canonical) const -> luato::Result<String> {
  auto out = String::make();
  (void)rstd_try(
      encode_json_value(*this, value, out, usize(), strict, canonical));
  return Ok(rstd::move(out));
}
template <class Source>
inline auto JsonContext::decode(Source &source, const rstd::json::Value &value,
                                bool null_as_nil) const
    -> luato::Result<luato::LuaValue> {
  return decode_json_value(*this, source, value, usize(), null_as_nil);
}
template <class Source>
inline auto JsonContext::decode(Source &source, ref<str> text,
                                bool null_as_nil) const
    -> luato::Result<luato::LuaValue> {
  auto parsed = rstd_try(rstd::json::from_str(text), [](auto error) {
    return json_error(rstd::move(error), "invalid JSON"_str);
  });
  return decode(source, parsed, null_as_nil);
}
inline auto strict_encode(Rc<JsonContext> context, luato::CallFrame &frame,
                          bool canonical) -> luato::BindingResult {
  auto value = take(frame, usize());
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  auto encoded = context->encode(*value, true, canonical);
  if (encoded.is_err())
    return Err(rstd::move(encoded).unwrap_err());
  frame.push(rstd::move(*encoded));
  return Ok(usize(1));
}
inline auto strict_kind(Rc<JsonContext> context, luato::CallFrame &frame)
    -> luato::BindingResult {
  auto value = take(frame, usize());
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  if (!value->is_Table()) {
    frame.push_nil();
    return Ok(usize(1));
  }
  auto table = rstd::move(*value).as_Table().value;
  auto same = rstd_try(context->is_null(table));
  if (same) {
    frame.push("null"_str);
    return Ok(usize(1));
  }
  auto kind = table_kind(table);
  if (kind.is_ok() && kind->is_some())
    frame.push((**kind).as_str());
  else
    frame.push_nil();
  return Ok(usize(1));
}
inline auto strict_mark(Rc<JsonContext>, luato::CallFrame &frame, bool array)
    -> luato::BindingResult {
  auto value = take(frame, usize());
  if (value.is_err())
    return Err(rstd::move(value).unwrap_err());
  if (!value->is_Table())
    return Err(fail("json kind expects a table"_str));
  auto table = rstd::move(*value).as_Table().value;
  auto marked = mark_table(frame, table, array);
  if (marked.is_err())
    return Err(rstd::move(marked).unwrap_err());
  frame.return_value(luato::OpaqueHandle{
      new luato::LuaValue(luato::LuaValue::Table(rstd::move(table)))});
  return Ok(usize());
}

inline auto json_modules(Rc<JsonContext> context, ref<str> prefix)
    -> Vec<luato::ModuleSpec> {
  auto json = luato::ModuleSpec(utility_name(prefix, "json"_str));
  json.raw_function(String::make("encode"_str), usize(1),
                    [context = context.clone()](
                        luato::CallFrame &frame) -> luato::BindingResult {
                      auto text = json_encode(*context, frame);
                      if (text.is_err())
                        return Err(rstd::move(text).unwrap_err());
                      frame.push(rstd::move(*text));
                      return Ok(usize(1));
                    });
  json.raw_function(String::make("object"_str), usize(1),
                    [](luato::CallFrame &frame) -> luato::BindingResult {
                      auto value = take(frame, usize());
                      if (value.is_err())
                        return Err(rstd::move(value).unwrap_err());
                      frame.return_value(luato::OpaqueHandle{
                          new luato::LuaValue(rstd::move(*value))});
                      return Ok(usize());
                    });
  json.raw_function(String::make("array"_str), usize(1),
                    [](luato::CallFrame &frame) -> luato::BindingResult {
                      auto value = take(frame, usize());
                      if (value.is_err())
                        return Err(rstd::move(value).unwrap_err());
                      frame.return_value(luato::OpaqueHandle{
                          new luato::LuaValue(rstd::move(*value))});
                      return Ok(usize());
                    });
  json.raw_function(String::make("decode"_str), usize(1),
                    [context = context.clone()](
                        luato::CallFrame &frame) -> luato::BindingResult {
                      auto text = string_arg(frame, usize());
                      if (text.is_err())
                        return Err(rstd::move(text).unwrap_err());
                      auto value = context->decode(frame, text->as_str(), true);
                      if (value.is_err())
                        return Err(rstd::move(value).unwrap_err());
                      frame.return_value(luato::OpaqueHandle{
                          new luato::LuaValue(rstd::move(*value))});
                      return Ok(usize());
                    });
  auto strict_json = luato::ModuleSpec(utility_name(prefix, "json.strict"_str));
  strict_json.raw_function(String::make("kind"_str), usize(1),
                           [context = context.clone()](luato::CallFrame &frame)
                               -> luato::BindingResult {
                             return strict_kind(context.clone(), frame);
                           });
  strict_json.raw_function(String::make("object"_str), usize(1),
                           [context = context.clone()](luato::CallFrame &frame)
                               -> luato::BindingResult {
                             return strict_mark(context.clone(), frame, false);
                           });
  strict_json.raw_function(String::make("array"_str), usize(1),
                           [context = context.clone()](luato::CallFrame &frame)
                               -> luato::BindingResult {
                             return strict_mark(context.clone(), frame, true);
                           });
  strict_json.raw_function(
      String::make("encode"_str), usize(1),
      [context =
           context.clone()](luato::CallFrame &frame) -> luato::BindingResult {
        return strict_encode(context.clone(), frame, false);
      });
  strict_json.raw_function(String::make("encode_canonical"_str), usize(1),
                           [context = context.clone()](luato::CallFrame &frame)
                               -> luato::BindingResult {
                             return strict_encode(context.clone(), frame, true);
                           });
  strict_json.raw_function(
      String::make("decode"_str), usize(1),
      [context =
           context.clone()](luato::CallFrame &frame) -> luato::BindingResult {
        auto text = string_arg(frame, usize());
        if (text.is_err())
          return Err(rstd::move(text).unwrap_err());
        auto value = context->decode(frame, text->as_str());
        if (value.is_err())
          return Err(rstd::move(value).unwrap_err());
        frame.return_value(
            luato::OpaqueHandle{new luato::LuaValue(rstd::move(*value))});
        return Ok(usize());
      });
  Vec<luato::ModuleSpec> modules;
  modules.push(rstd::move(json));
  modules.push(rstd::move(strict_json));
  return modules;
}
} // namespace luato::extra
