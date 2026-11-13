module;
#include <rstd/macro.hpp>
export module luato.extra:url;
export import luato;
export import rstd;
export import :value;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra {
inline auto percent_encode_bytes(slice<u8> text, bool form = false) -> String {
  static constexpr char hex[] = "0123456789ABCDEF";
  auto out = String::make();
  for (auto byte : text) {
    auto c = byte.to_primitive();
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        (form ? c == '*' : c == '~')) {
      out.push_ascii(static_cast<char>(c));
    } else if (form && c == ' ') {
      out.push_ascii('+');
    } else {
      out.push_ascii('%');
      out.push_ascii(hex[c >> 4]);
      out.push_ascii(hex[c & 0x0f]);
    }
  }
  return out;
}
inline auto percent_encode(ref<str> text) -> String {
  return percent_encode_bytes(text.as_bytes());
}
inline auto decode_percent_bytes(slice<u8> input, bool plus_as_space,
                                 bool strict)
    -> rstd::Result<Vec<u8>, luato::Error> {
  auto digit = [](u8 byte) -> int {
    auto c = byte.to_primitive();
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  auto out = Vec<u8>::make();
  for (usize index{}; index < input.len(); ++index) {
    auto c = input[index];
    if (plus_as_space && c == u8('+')) {
      out.push(u8(' '));
    } else if (c == u8('%')) {
      if (index + usize(2) < input.len()) {
        auto high = digit(input[index + usize(1)]);
        auto low = digit(input[index + usize(2)]);
        if (high >= 0 && low >= 0) {
          out.push(u8(static_cast<unsigned char>(high * 16 + low)));
          index += usize(2);
          continue;
        }
      }
      if (strict)
        return Err(fail("invalid percent escape"_str));
      out.push(u8(c));
    } else {
      out.push(u8(c));
    }
  }
  return Ok(rstd::move(out));
}
inline auto lower_ascii(ref<str> text) -> String {
  auto bytes = Vec<u8>::from(text.as_bytes());
  for (usize index{}; index < bytes.len(); ++index) {
    auto byte = bytes[index];
    auto c = byte.get().to_primitive();
    if (c >= 'A' && c <= 'Z')
      byte = u8(static_cast<unsigned char>(c + 32));
  }
  return String::from_utf8_unchecked(rstd::move(bytes));
}
struct UrlParts {
  String scheme, userinfo, host, path, query, fragment;
  bool has_userinfo{}, has_fragment{};
  Option<i64> port;
};
inline auto parse_url(ref<str> uri) -> Option<UrlParts> {
  usize scheme_end{};
  while (scheme_end < uri.len()) {
    auto c = uri[scheme_end].to_primitive();
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    bool rest =
        alpha || (scheme_end > usize() &&
                  ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
    if (!rest)
      break;
    ++scheme_end;
  }
  if (scheme_end == usize() ||
      !rstd::get<1>(uri.split_at(scheme_end)).starts_with("://"_str))
    return None();
  UrlParts parts;
  parts.scheme = lower_ascii(rstd::get<0>(uri.split_at(scheme_end)));
  auto rest = rstd::get<1>(uri.split_at(scheme_end + usize(3)));
  usize authority_end{};
  while (authority_end < rest.len()) {
    auto c = rest[authority_end].to_primitive();
    if (c == '/' || c == '?' || c == '#')
      break;
    ++authority_end;
  }
  auto authority = rstd::get<0>(rest.split_at(authority_end));
  auto remainder = rstd::get<1>(rest.split_at(authority_end));
  auto hash = remainder.find("#"_str);
  if (hash.is_some()) {
    parts.has_fragment = true;
    parts.fragment =
        String::make(rstd::get<1>(remainder.split_at(*hash + usize(1))));
    remainder = rstd::get<0>(remainder.split_at(*hash));
  }
  auto question = remainder.find("?"_str);
  if (question.is_some()) {
    parts.query =
        String::make(rstd::get<1>(remainder.split_at(*question + usize(1))));
    remainder = rstd::get<0>(remainder.split_at(*question));
  }
  parts.path = String::make(remainder);
  auto at = authority.rfind("@"_str);
  if (at.is_some()) {
    parts.has_userinfo = true;
    parts.userinfo = String::make(rstd::get<0>(authority.split_at(*at)));
    authority = rstd::get<1>(authority.split_at(*at + usize(1)));
  }
  ref<str> port;
  if (authority.starts_with("["_str)) {
    auto close = authority.find("]"_str);
    if (close.is_none())
      return None();
    auto tail = rstd::get<1>(authority.split_at(*close + usize(1)));
    parts.host =
        lower_ascii(rstd::get<0>(authority.split_at(*close + usize(1))));
    if (tail.len() > usize(1) && tail.starts_with(":"_str))
      port = rstd::get<1>(tail.split_at(usize(1)));
  } else {
    auto colon = authority.rfind(":"_str);
    if (colon.is_some()) {
      port = rstd::get<1>(authority.split_at(*colon + usize(1)));
      authority = rstd::get<0>(authority.split_at(*colon));
    }
    parts.host = lower_ascii(authority);
  }
  if (!port.is_empty()) {
    i64 value{};
    bool valid = true;
    for (auto byte : port.as_bytes()) {
      auto c = byte.to_primitive();
      if (c < '0' || c > '9') {
        valid = false;
        break;
      }
      value = i64(value.to_primitive() * 10 + (c - '0'));
    }
    if (valid)
      parts.port = Some(value);
  }
  if (parts.port.is_none()) {
    if (parts.scheme == "http"_str || parts.scheme == "ws"_str)
      parts.port = Some(i64(80));
    else if (parts.scheme == "https"_str || parts.scheme == "wss"_str)
      parts.port = Some(i64(443));
  }
  return Some(rstd::move(parts));
}
inline auto url_parse(ref<str> uri, luato::CallFrame &frame)
    -> rstd::Result<Option<luato::TableHandle>, luato::Error> {
  auto parsed = parse_url(uri);
  if (parsed.is_none())
    return Ok(None());
  auto handle = rstd_try(new_table(frame));
  (void)handle.set_in_call(str_key("scheme"_str),
                           str_value(parsed->scheme.as_str()));
  (void)handle.set_in_call(str_key("host"_str),
                           str_value(parsed->host.as_str()));
  if (parsed->port.is_some())
    (void)handle.set_in_call(str_key("port"_str),
                             luato::LuaValue::Integer(*parsed->port));
  if (parsed->has_userinfo)
    (void)handle.set_in_call(str_key("userinfo"_str),
                             str_value(parsed->userinfo.as_str()));
  (void)handle.set_in_call(str_key("path"_str),
                           str_value(parsed->path.as_str()));
  (void)handle.set_in_call(str_key("query"_str),
                           str_value(parsed->query.as_str()));
  if (parsed->has_fragment)
    (void)handle.set_in_call(str_key("fragment"_str),
                             str_value(parsed->fragment.as_str()));
  return Ok(Some(rstd::move(handle)));
}
inline auto url_join(ref<str> base, ref<str> reference) -> Option<String> {
  if (parse_url(reference).is_some())
    return Some(String::make(reference));
  auto root = parse_url(base);
  if (root.is_none())
    return None();
  auto path = String::make();
  if (reference.starts_with("/"_str)) {
    path = String::make(reference);
  } else {
    auto slash = root->path.as_str().rfind("/"_str);
    path = slash.is_none()
               ? String::make("/"_str)
               : String::make(rstd::get<0>(
                     root->path.as_str().split_at(*slash + usize(1))));
    path.push_str(reference);
  }
  auto segments = Vec<String>::make();
  auto remaining = path.as_str();
  while (true) {
    auto slash = remaining.find("/"_str);
    auto current =
        slash.is_some() ? rstd::get<0>(remaining.split_at(*slash)) : remaining;
    if (current == ".."_str) {
      (void)segments.pop();
    } else if (!current.is_empty() && current != "."_str) {
      segments.push(String::make(current));
    }
    if (slash.is_none())
      break;
    remaining = rstd::get<1>(remaining.split_at(*slash + usize(1)));
  }
  auto out = root->scheme.clone();
  out.push_str("://"_str);
  if (root->has_userinfo) {
    out.push_str(root->userinfo.as_str());
    out.push_ascii('@');
  }
  out.push_str(root->host.as_str());
  bool default_port = (root->scheme == "http"_str && root->port.is_some() &&
                       *root->port == i64(80)) ||
                      (root->scheme == "https"_str && root->port.is_some() &&
                       *root->port == i64(443));
  if (root->port.is_some() && !default_port) {
    out.push_ascii(':');
    out.push_str(rstd::format("{}", root->port->to_primitive()).as_str());
  }
  out.push_ascii('/');
  for (usize index{}; index < segments.len(); ++index) {
    if (index != usize())
      out.push_ascii('/');
    out.push_str(segments[index].as_str());
  }
  if (!root->query.is_empty()) {
    out.push_ascii('?');
    out.push_str(root->query.as_str());
  }
  return Some(rstd::move(out));
}
inline auto url_decode(luato::CallFrame &frame) -> luato::BindingResult {
  auto query = rstd_try(string_arg(frame, usize()));
  auto handle = rstd_try(new_table(frame));
  auto text = query.as_str();
  usize start{};
  while (start <= text.len()) {
    auto end = start;
    while (end < text.len() && text[end] != u8('&') && text[end] != u8(';'))
      ++end;
    auto part = *text.get(start, end);
    if (!part.is_empty()) {
      auto mapping = part.find("="_str);
      auto key =
          mapping.is_none() ? part : rstd::get<0>(part.split_at(*mapping));
      auto value = mapping.is_none()
                       ? ""_str
                       : rstd::get<1>(part.split_at(*mapping + usize(1)));
      auto decoded_key =
          rstd_try(decode_percent_bytes(key.as_bytes(), true, false));
      auto decoded_value =
          rstd_try(decode_percent_bytes(value.as_bytes(), true, false));
      (void)handle.set_in_call(
          luato::LuaValue::Bytes(rstd::move(decoded_key)),
          luato::LuaValue::Bytes(rstd::move(decoded_value)));
    }
    if (end == text.len())
      break;
    start = end + usize(1);
  }
  return_table(frame, rstd::move(handle));
  return Ok(usize());
}
inline auto value_bytes(const luato::LuaValue &value) -> Option<Vec<u8>> {
  if (value.is_Bytes())
    return Some(Vec<u8>::from(value.as_Bytes().value.as_slice()));
  if (value.is_Integer())
    return Some(rstd::format("{}", value.as_Integer().value.to_primitive())
                    .into_bytes());
  if (value.is_Number())
    return Some(rstd::format("{:.6}", value.as_Number().value.to_primitive())
                    .into_bytes());
  if (value.is_Boolean())
    return Some(Vec<u8>::from(
        (value.as_Boolean().value ? "true"_str : "false"_str).as_bytes()));
  return None();
}
inline auto url_percent_decode(luato::CallFrame &frame)
    -> luato::BindingResult {
  auto text = rstd_try(string_arg(frame, usize()));
  auto bytes =
      rstd_try(decode_percent_bytes(text.as_str().as_bytes(), false, true));
  frame.return_value(luato::OpaqueHandle{
      new luato::LuaValue(luato::LuaValue::Bytes(rstd::move(bytes)))});
  return Ok(usize());
}
inline auto url_encode_query_pairs(luato::CallFrame &frame)
    -> luato::BindingResult {
  auto pairs = rstd_try(table_arg(frame, usize()));
  auto length = rstd_try(pairs.length());
  auto out = String::make();
  for (usize index{}; index < length; ++index) {
    auto pair = pairs.get_in_call(luato::LuaValue::Integer(
        i64(static_cast<long long>(index.to_primitive() + 1))));
    if (pair.is_err() || !pair->is_Table())
      return Err(fail("url pairs must be a dense array"_str));
    auto entry = rstd::move(*pair).as_Table().value;
    auto name = entry.get_in_call(luato::LuaValue::Integer(i64(1)));
    auto value = entry.get_in_call(luato::LuaValue::Integer(i64(2)));
    if (name.is_err() || value.is_err())
      return Err(fail("invalid url pair"_str));
    auto name_bytes = value_bytes(*name);
    auto bytes = value_bytes(*value);
    if (name_bytes.is_none() || bytes.is_none())
      return Err(fail("url pair values must be strings"_str));
    if (!out.is_empty())
      out.push_ascii('&');
    out.push_str(percent_encode_bytes(name_bytes->as_slice(), true).as_str());
    out.push_ascii('=');
    out.push_str(percent_encode_bytes(bytes->as_slice(), true).as_str());
  }
  frame.push(rstd::move(out));
  return Ok(usize(1));
}
inline auto url_encode(luato::CallFrame &frame) -> luato::BindingResult {
  auto value = rstd_try(take(frame, usize()));
  if (!value.is_Table())
    return Err(fail("url.encode expects a table"_str));
  auto table = rstd::move(value).as_Table().value;
  auto keys = rstd_try(table.keys());
  auto out = String::make();
  for (const auto &key : keys.as_slice()) {
    auto item = table.get_in_call(key);
    if (item.is_err())
      continue;
    auto name = value_bytes(key);
    auto bytes = value_bytes(*item);
    if (name.is_none() || bytes.is_none())
      continue;
    if (!out.is_empty())
      out.push_ascii('&');
    out.push_str(percent_encode_bytes(name->as_slice(), true).as_str());
    out.push_ascii('=');
    out.push_str(percent_encode_bytes(bytes->as_slice(), true).as_str());
  }
  frame.push(rstd::move(out));
  return Ok(usize(1));
}
inline auto url_host(luato::CallFrame &frame) -> luato::BindingResult {
  auto text = rstd_try(string_arg(frame, usize()));
  auto parsed = parse_url(text.as_str());
  if (parsed.is_none() || parsed->host.is_empty()) {
    frame.push_nil();
    return Ok(usize(1));
  }
  frame.push(rstd::move(parsed->host));
  return Ok(usize(1));
}
inline auto url_append_api_path(luato::CallFrame &frame)
    -> luato::BindingResult {
  auto base = rstd_try(string_arg(frame, usize()));
  auto path = rstd_try(string_arg(frame, usize(1)));
  auto parsed = parse_url(base.as_str());
  if (parsed.is_none() ||
      (parsed->scheme != "http"_str && parsed->scheme != "https"_str) ||
      !parsed->query.is_empty() || parsed->has_fragment)
    return Err(fail("invalid API base url"_str));
  auto relative = path.as_str();
  if (relative.starts_with("//"_str) || relative.contains("?"_str) ||
      relative.contains("#"_str) || relative.contains("\\"_str))
    return Err(fail("invalid API path"_str));
  auto lower = lower_ascii(relative);
  auto at = lower.as_str().find("%2e"_str);
  while (at.is_some()) {
    lower.replace_range(*at, *at + usize(3), "."_str);
    auto begin = *at + usize(1);
    at = rstd::get<1>(lower.as_str().split_at(begin)).find("%2e"_str);
    if (at.is_some())
      *at += begin;
  }
  auto normalized = lower.as_str();
  if (normalized == "."_str || normalized == ".."_str ||
      normalized.contains("/./"_str) || normalized.contains("/../"_str) ||
      normalized.starts_with("./"_str) || normalized.starts_with("../"_str) ||
      normalized.ends_with("/."_str) || normalized.ends_with("/.."_str))
    return Err(fail("invalid API path"_str));
  auto joined = rstd::move(base);
  while (joined.len() > parsed->scheme.len() + usize(3) &&
         joined.as_str().ends_with("/"_str))
    joined.truncate(joined.len() - usize(1));
  if (!joined.is_empty() && !joined.as_str().ends_with("/"_str))
    joined.push_ascii('/');
  while (relative.starts_with("/"_str))
    relative = rstd::get<1>(relative.split_at(usize(1)));
  joined.push_str(relative);
  frame.push(rstd::move(joined));
  return Ok(usize(1));
}
inline auto url_encode_component(luato::CallFrame &frame)
    -> luato::BindingResult {
  auto text = rstd_try(string_arg(frame, usize()));
  frame.push(percent_encode_bytes(text.as_str().as_bytes(), true));
  return Ok(usize(1));
}
inline auto url_decode_component(luato::CallFrame &frame)
    -> luato::BindingResult {
  auto text = rstd_try(string_arg(frame, usize()));
  auto bytes =
      rstd_try(decode_percent_bytes(text.as_str().as_bytes(), true, true));
  frame.return_value(luato::OpaqueHandle{
      new luato::LuaValue(luato::LuaValue::Bytes(rstd::move(bytes)))});
  return Ok(usize());
}

inline auto url_module(ref<str> prefix) -> luato::ModuleSpec {
  auto url = luato::ModuleSpec(utility_name(prefix, "url"_str));
  url.raw_function(String::make("encode_path_segment"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     auto text = string_arg(frame, usize());
                     if (text.is_err())
                       return Err(rstd::move(text).unwrap_err());
                     frame.push(percent_encode(text->as_str()));
                     return Ok(usize(1));
                   });
  url.raw_function(String::make("decode"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     return url_decode(frame);
                   });
  url.raw_function(String::make("parse"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     auto text = string_arg(frame, usize());
                     if (text.is_err())
                       return Err(rstd::move(text).unwrap_err());
                     auto parsed = url_parse(text->as_str(), frame);
                     if (parsed.is_err())
                       return Err(rstd::move(parsed).unwrap_err());
                     if (parsed->is_none())
                       return Ok(usize());
                     return_table(frame, rstd::move(**parsed));
                     return Ok(usize());
                   });
  url.raw_variadic_function(
      String::make("join"_str),
      [](luato::CallFrame &frame) -> luato::BindingResult {
        auto base = string_arg(frame, usize());
        auto reference = string_arg(frame, usize(1));
        if (base.is_err())
          return Err(rstd::move(base).unwrap_err());
        if (reference.is_err())
          return Err(rstd::move(reference).unwrap_err());
        auto joined = url_join(base->as_str(), reference->as_str());
        if (joined.is_none())
          return Ok(usize());
        frame.push(rstd::move(*joined));
        return Ok(usize(1));
      });
  url.raw_function(String::make("percent_decode"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     return url_percent_decode(frame);
                   });
  url.raw_function(String::make("encode_query_pairs"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     return url_encode_query_pairs(frame);
                   });
  url.raw_function(String::make("append_api_path"_str), usize(2),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     return url_append_api_path(frame);
                   });
  url.raw_function(String::make("encode"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     return url_encode(frame);
                   });
  url.raw_function(String::make("encode_component"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     return url_encode_component(frame);
                   });
  url.raw_function(String::make("decode_component"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     return url_decode_component(frame);
                   });
  url.raw_function(String::make("host"_str), usize(1),
                   [](luato::CallFrame &frame) -> luato::BindingResult {
                     return url_host(frame);
                   });
  return url;
}
} // namespace luato::extra
