module;
#include <rstd/macro.hpp>
module luato.extra;
import :http;
import :url;
using alloc::rc::make_rc;
using alloc::rc::Rc;
using rstd::async::coro;
using rstd::time::Duration;
using namespace rstd::prelude;
using namespace rstd::literals;

namespace luato::extra::http {
auto userdata_arg(luato::CallFrame &frame, usize index)
    -> luato::Result<luato::UserDataHandle> {
  return luato::argument<luato::UserDataHandle>(frame, index);
}
auto return_value(luato::CallFrame &frame, luato::LuaValue value)
    -> luato::BindingResult {
  return luato::return_value(frame, rstd::move(value));
}
auto return_self(luato::CallFrame &frame) -> luato::BindingResult {
  return return_value(frame, rstd_try(extra::take(frame, usize())));
}
auto request(const luato::UserDataHandle &handle)
    -> luato::Result<TakenRequest> {
  return handle.with<Builder>(
      [](const Builder &builder) -> luato::Result<TakenRequest> {
        rstd_try(builder.transport->check());
        return Ok(
            TakenRequest{builder.transport.clone(), builder.request.clone()});
      });
}
auto take_request(const luato::UserDataHandle &handle)
    -> luato::Result<TakenRequest> {
  return take_request(handle, [](const RequestData &) -> luato::Result<empty> {
    return Ok(empty{});
  });
}
auto response(const luato::UserDataHandle &handle)
    -> luato::Result<Rc<Response>> {
  return handle.with<Rc<Response>>(
      [](const Rc<Response> &value) -> luato::Result<Rc<Response>> {
        return Ok(value.clone());
      });
}

Context::Context(Rc<ClientSource> source, Rc<JsonContext> json)
    : source_(rstd::move(source)), json_(rstd::move(json)) {}
auto Context::json() const -> const JsonContext & { return *json_; }
auto Context::current() const -> luato::Result<TransportHandle> {
  auto source = source_.clone();
  return source.get()->current();
}
auto Context::create_client(luato::CallFrame &frame) const
    -> luato::BindingResult {
  auto transport = rstd_try(current());
  rstd_try(transport->check());
  auto client = rstd_try(frame.create_userdata(Client{rstd::move(transport)}));
  return return_value(frame, luato::LuaValue::UserData(rstd::move(client)));
}
auto Context::decoder(bool strict) const
    -> luato::Result<luato::FunctionHandle> {
  auto &value = strict ? strict_decoder_ : legacy_decoder_;
  if (value.is_none())
    return Err(error("JSON decoder is unavailable"_str));
  return value->clone();
}

namespace binding {
auto client_transport(luato::CallFrame &frame)
    -> luato::Result<TransportHandle> {
  auto handle = rstd_try(userdata_arg(frame));
  return handle.with<Client>(
      [](const Client &value) -> luato::Result<TransportHandle> {
        rstd_try(value.transport->check());
        return Ok(value.transport.clone());
      });
}

auto builder(luato::CallFrame &frame, Option<ref<str>> fixed)
    -> luato::BindingResult {
  auto transport = rstd_try(client_transport(frame));
  auto method = fixed.is_some() ? String::make(*fixed)
                                : rstd_try(string_arg(frame, usize(1)));
  auto url = rstd_try(string_arg(frame, fixed.is_some() ? usize(1) : usize(2)));
  RequestData data;
  data.method = rstd::move(method);
  data.url = rstd::move(url);
  auto handle = rstd_try(
      frame.create_userdata(Builder{rstd::move(transport), rstd::move(data)}));
  return return_value(frame, luato::LuaValue::UserData(rstd::move(handle)));
}

auto parse_header(const luato::LuaValue &name, const luato::LuaValue &value)
    -> luato::Result<lihttpto::Header> {
  if (!name.is_Bytes() || !value.is_Bytes())
    return Err(error("invalid request header"_str));
  auto text = rstd_try(name.as_utf8());
  return Ok(
      rstd_try(lihttpto::Header::make(text, value.as_Bytes().value.as_slice()),
               error("invalid request header"_str)));
}

auto header(luato::CallFrame &frame) -> luato::BindingResult {
  auto name = rstd_try(extra::take(frame, usize(1)));
  auto value = rstd_try(extra::take(frame, usize(2)));
  auto field = rstd_try(parse_header(name, value));
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    request.headers.push(rstd::move(field));
    return Ok(empty{});
  });
}

auto dense(luato::TableHandle &table) -> luato::Result<usize> {
  auto length = rstd_try(table.length());
  auto keys = rstd_try(table.keys());
  if (keys.len() != length)
    return Err(error("pairs must be a dense array"_str));
  for (const auto &key : keys) {
    if (!key.is_Integer() || key.as_Integer().value < i64(1) ||
        u64(key.as_Integer().value.to_primitive()) > u64(length.to_primitive()))
      return Err(error("pairs must be a dense array"_str));
  }
  return Ok(length);
}

auto pair(luato::TableHandle &table, usize index)
    -> luato::Result<rstd::tuple<luato::LuaValue, luato::LuaValue>> {
  auto item = rstd_try(table.get_in_call(
      luato::LuaValue::Integer(i64(index.to_primitive() + 1))));
  if (!item.is_Table())
    return Err(error("expected a name/value pair"_str));
  auto entry = rstd::move(item).as_Table().value;
  if (rstd_try(dense(entry)) != usize(2))
    return Err(error("expected a name/value pair"_str));
  auto name = rstd_try(entry.get_in_call(luato::LuaValue::Integer(i64(1))));
  auto value = rstd_try(entry.get_in_call(luato::LuaValue::Integer(i64(2))));
  return Ok(rstd::make_tuple(rstd::move(name), rstd::move(value)));
}

auto headers(luato::CallFrame &frame, bool pairs) -> luato::BindingResult {
  auto input = rstd_try(table_arg(frame, usize(1)));
  lihttpto::Headers parsed;
  if (pairs) {
    auto length = rstd_try(dense(input));
    for (usize index{}; index < length; ++index) {
      auto values = rstd_try(pair(input, index));
      parsed.push(
          rstd_try(parse_header(rstd::get<0>(values), rstd::get<1>(values))));
    }
  } else {
    auto keys = rstd_try(input.keys());
    for (const auto &key : keys) {
      auto value = rstd_try(input.get_in_call(key));
      auto field = rstd_try(parse_header(key, value));
      parsed.remove(field.name.as_str());
      parsed.push(rstd::move(field));
    }
  }
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    if (!pairs)
      for (const auto &field : parsed.as_slice())
        request.headers.remove(field.name.as_str());
    for (auto &field : parsed.as_slice())
      request.headers.push(field.clone());
    return Ok(empty{});
  });
}

auto default_header(RequestData &request, ref<str> name, ref<str> value,
                    bool append) -> luato::Result<empty> {
  if (append || !request.headers.contains(name))
    rstd_try(request.headers.add(name, value),
             error("invalid request header"_str));
  return Ok(empty{});
}

auto body(luato::CallFrame &frame) -> luato::BindingResult {
  auto value = rstd_try(extra::take(frame, usize(1)));
  if (!value.is_Bytes())
    return Err(error("builder body expects a string"_str));
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    request.body = Some(rstd::move(value).as_Bytes().value);
    return Ok(empty{});
  });
}

auto json(const Context &context, luato::CallFrame &frame)
    -> luato::BindingResult {
  auto value = rstd_try(extra::take(frame, usize(1)));
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    auto text = rstd_try(context.json().encode(value, request.strict_json));
    request.body = Some(rstd::move(text).into_bytes());
    return default_header(request, "content-type"_str, "application/json"_str,
                          true);
  });
}

auto codec(luato::CallFrame &frame) -> luato::BindingResult {
  auto name = rstd_try(string_arg(frame, usize(1)));
  if (name != "strict"_str && name != "legacy"_str)
    return Err(error("unknown JSON codec"_str));
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    request.strict_json = name == "strict"_str;
    return Ok(empty{});
  });
}

auto timeout(const luato::LuaValue &value) -> luato::Result<Duration> {
  double seconds;
  if (value.is_Integer())
    seconds = static_cast<double>(value.as_Integer().value.to_primitive());
  else if (value.is_Number())
    seconds = value.as_Number().value.to_primitive();
  else
    return Err(error("builder timeout expects a number"_str));
  if (!f64(seconds).is_finite() || seconds <= 0 ||
      seconds >= 18446744073709551616.0)
    return Err(error("builder timeout must be positive and finite"_str));
  auto duration = Duration::from_secs_f64(seconds);
  if (duration.is_zero())
    return Err(error("builder timeout must be positive"_str));
  return Ok(duration);
}

auto set_timeout(luato::CallFrame &frame) -> luato::BindingResult {
  auto duration = rstd_try(timeout(rstd_try(extra::take(frame, usize(1)))));
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    request.timeout = Some(duration);
    return Ok(empty{});
  });
}

auto response_limit(luato::CallFrame &frame) -> luato::BindingResult {
  auto value = rstd_try(extra::take(frame, usize(1)));
  if (!value.is_Integer())
    return Err(error("builder response limit expects an integer"_str));
  return mutate(
      frame, [](RequestData &) -> luato::Result<empty> { return Ok(empty{}); });
}

auto url_pairs(luato::CallFrame &frame, bool form) -> luato::BindingResult {
  auto input = rstd_try(table_arg(frame, usize(1)));
  auto length = rstd_try(dense(input));
  String encoded;
  for (usize index{}; index < length; ++index) {
    auto values = rstd_try(pair(input, index));
    auto name = rstd_try(rstd::get<0>(values).as_utf8());
    auto value = rstd_try(rstd::get<1>(values).as_utf8());
    if (!encoded.is_empty())
      encoded.push_ascii('&');
    encoded.push_str(percent_encode_bytes(name.as_bytes(), true).as_str());
    encoded.push_ascii('=');
    encoded.push_str(percent_encode_bytes(value.as_bytes(), true).as_str());
  }
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    if (form) {
      request.body = Some(rstd::move(encoded).into_bytes());
      return default_header(request, "content-type"_str,
                            "application/x-www-form-urlencoded"_str);
    }
    if (!encoded.is_empty()) {
      auto fragment = request.url.as_str().find("#"_str);
      auto suffix = fragment.is_none()
                        ? String::make()
                        : String::make(rstd::get<1>(
                              request.url.as_str().split_at(*fragment)));
      if (fragment.is_some())
        request.url.truncate(*fragment);
      request.url.push_ascii(request.url.as_str().contains("?"_str) ? '&'
                                                                    : '?');
      request.url.push_str(encoded.as_str());
      request.url.push_str(suffix.as_str());
    }
    return Ok(empty{});
  });
}

auto form(luato::CallFrame &frame) -> luato::BindingResult {
  auto input = rstd_try(extra::take(frame, usize(1)));
  Vec<u8> bytes;
  if (input.is_Bytes())
    bytes = Vec<u8>::from(input.as_Bytes().value.as_slice());
  else if (input.is_Table()) {
    auto table = rstd::move(input).as_Table().value;
    auto keys = rstd_try(table.keys());
    for (const auto &key : keys) {
      if (!key.is_Bytes())
        continue;
      auto item = rstd_try(table.get_in_call(key));
      Vec<u8> text;
      if (item.is_Bytes())
        text = Vec<u8>::from(item.as_Bytes().value.as_slice());
      else if (item.is_Integer())
        text = rstd::format("{}", item.as_Integer().value.to_primitive())
                   .into_bytes();
      else if (item.is_Number())
        text = rstd::format("{:.6}", item.as_Number().value.to_primitive())
                   .into_bytes();
      else if (item.is_Boolean())
        text = Vec<u8>::from(
            (item.as_Boolean().value ? "true"_str : "false"_str).as_bytes());
      else
        continue;
      if (!bytes.is_empty())
        bytes.push(u8('&'));
      bytes.extend_from_slice(
          percent_encode_bytes(key.as_Bytes().value.as_slice())
              .as_str()
              .as_bytes());
      bytes.push(u8('='));
      bytes.extend_from_slice(
          percent_encode_bytes(text.as_slice()).as_str().as_bytes());
    }
  } else
    return Err(error("builder form expects a string or table"_str));
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    request.body = Some(rstd::move(bytes));
    return default_header(request, "content-type"_str,
                          "application/x-www-form-urlencoded"_str);
  });
}

auto query(luato::CallFrame &frame) -> luato::BindingResult {
  auto input = rstd_try(table_arg(frame, usize(1)));
  auto keys = rstd_try(input.keys());
  String query;
  for (const auto &key : keys) {
    if (!key.is_Bytes())
      continue;
    auto value = rstd_try(input.get_in_call(key));
    if (!query.is_empty())
      query.push_ascii('&');
    query.push_str(
        percent_encode_bytes(key.as_Bytes().value.as_slice()).as_str());
    query.push_ascii('=');
    if (value.is_Bytes())
      query.push_str(
          percent_encode_bytes(value.as_Bytes().value.as_slice()).as_str());
  }
  return mutate(frame, [&](RequestData &request) -> luato::Result<empty> {
    if (!query.is_empty()) {
      request.url.push_ascii(request.url.as_str().contains("?"_str) ? '&'
                                                                    : '?');
      request.url.push_str(query.as_str());
    }
    return Ok(empty{});
  });
}

auto send_run(TakenRequest request, luato::UserDataHandle output)
    -> coro<luato::Result<luato::MultiValue>> {
  auto data = rstd_co_try(co_await request.transport->send(
      rstd::move(request.request), usize(8 * 1024 * 1024)));
  auto target = rstd_co_try(response(output));
  target.get()->receive(rstd::move(data));
  auto values = luato::MultiValue::make();
  values.push(luato::LuaValue::UserData(rstd::move(output)));
  co_return Ok(rstd::move(values));
}

auto send(luato::CallFrame &frame, usize index)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>> {
  if (index != usize())
    rstd_try(client_transport(frame));
  auto input = rstd_try(userdata_arg(frame, index));
  auto owned = rstd_try(request(input));
  auto output = rstd_try(create_response(frame, ResponseData{}, None(),
                                         owned.request.strict_json));
  return Ok(send_run(rstd::move(owned), rstd::move(output)));
}

auto read_options(luato::CallFrame &frame, bool strict, bool json)
    -> luato::Result<ReadOptions> {
  ReadOptions result;
  result.strict = strict;
  if (frame.argument_count() > usize(2))
    return Err(error("invalid response options"_str));
  if (frame.argument_count() < usize(2))
    return Ok(result);
  auto value = rstd_try(extra::take(frame, usize(1)));
  if (value.is_Nil())
    return Ok(result);
  if (!value.is_Table())
    return Err(error("invalid response options"_str));
  auto table = rstd::move(value).as_Table().value;
  auto max = rstd_try(table.get_in_call(str_key("max_bytes"_str)));
  if (!max.is_Nil()) {
    if (!max.is_Integer() || max.as_Integer().value < i64())
      return Err(error("invalid response limit"_str));
    result.limit =
        rstd::min(result.limit, usize(max.as_Integer().value.to_primitive()));
  }
  auto ms = rstd_try(table.get_in_call(str_key("timeout_ms"_str)));
  if (!ms.is_Nil()) {
    if (!ms.is_Integer() || ms.as_Integer().value <= i64())
      return Err(error("invalid response timeout"_str));
    result.timeout =
        Duration::from_millis(u64(ms.as_Integer().value.to_primitive()));
  }
  if (json) {
    auto codec = rstd_try(table.get_in_call(str_key("codec"_str)));
    if (!codec.is_Nil()) {
      auto name = rstd_try(codec.as_utf8());
      if (name != "strict"_str && name != "legacy"_str)
        return Err(error("unknown JSON codec"_str));
      result.strict = name == "strict"_str;
    }
  }
  return Ok(result);
}

auto read_run(Rc<Response> value, luato::UserDataHandle handle,
              ReadOptions options, Option<luato::FunctionHandle> decoder,
              bool with_self) -> coro<luato::Result<luato::MultiValue>> {
  auto bytes = rstd_co_try(co_await Response::read(rstd::move(value), options));
  auto output = luato::MultiValue::make();
  if (with_self)
    output.push(luato::LuaValue::UserData(rstd::move(handle)));
  if (decoder.is_some()) {
    rstd_co_try(rstd::str_::from_utf8(bytes.as_slice()), [](auto cause) {
      return extra::json_error(rstd::move(cause),
                               "response is not valid UTF-8"_str);
    });
    auto args = luato::MultiValue::make();
    args.push(luato::LuaValue::Bytes(rstd::move(bytes)));
    auto decoded = rstd_co_try(decoder->call_values(rstd::move(args)));
    if (decoded.len() != usize(1))
      co_return Err(error("invalid JSON response"_str));
    co_return Ok(rstd::move(decoded));
  }
  output.push(luato::LuaValue::Bytes(rstd::move(bytes)));
  co_return Ok(rstd::move(output));
}

auto read(const Context &context, luato::CallFrame &frame, bool json,
          bool with_self)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>> {
  auto handle = rstd_try(userdata_arg(frame));
  auto value = rstd_try(response(handle));
  auto options = rstd_try(read_options(frame, value->strict_json(), json));
  Option<luato::FunctionHandle> decoder;
  if (json)
    decoder = Some(rstd_try(context.decoder(options.strict)));
  return Ok(read_run(rstd::move(value), rstd::move(handle), options,
                     rstd::move(decoder), with_self));
}

auto response_field(luato::CallFrame &frame, unsigned field)
    -> luato::BindingResult {
  auto handle = rstd_try(userdata_arg(frame));
  auto value = rstd_try(response(handle));
  if (field == 0) {
    frame.push(i64(value->data().status.to_primitive()));
    return Ok(usize(1));
  }
  if (field == 1) {
    frame.push(value->data().url.as_str());
    return Ok(usize(1));
  }
  if (field == 2) {
    rstd_try(value.get()->close());
    frame.push_nil();
    return Ok(usize(1));
  }
  auto body = rstd_try(extra::take(frame, usize(1)));
  if (!body.is_Bytes())
    return Err(error("response body must be a string"_str));
  rstd_try(value.get()->replace_body(rstd::move(body).as_Bytes().value));
  return return_self(frame);
}

auto response_headers(luato::CallFrame &frame, unsigned mode)
    -> luato::BindingResult {
  auto handle = rstd_try(userdata_arg(frame));
  auto value = rstd_try(response(handle));
  rstd_try(value->check());
  Option<String> name;
  if (mode < 2)
    name = Some(rstd_try(string_arg(frame, usize(1))));
  auto output = rstd_try(new_table(frame));
  i64 count{};
  for (const auto &field : value->data().headers.as_slice()) {
    if (name.is_some() && !field.name.matches(name->as_str()))
      continue;
    auto bytes = luato::LuaValue::Bytes(Vec<u8>::from(field.value.as_slice()));
    if (mode == 0)
      return return_value(frame, rstd::move(bytes));
    if (mode == 3) {
      auto text = rstd::str_::from_utf8(field.value.as_slice());
      if (text.is_ok())
        rstd_try(output.set_in_call(
            str_key(lower_ascii(field.name.as_str()).as_str()),
            str_value(*text)));
    } else {
      count += i64(1);
      if (mode == 2) {
        auto pair = rstd_try(new_table(frame));
        rstd_try(pair.set_in_call(
            luato::LuaValue::Integer(i64(1)),
            str_value(lower_ascii(field.name.as_str()).as_str())));
        rstd_try(pair.set_in_call(luato::LuaValue::Integer(i64(2)),
                                  rstd::move(bytes)));
        bytes = luato::LuaValue::Table(rstd::move(pair));
      }
      rstd_try(output.set_in_call(luato::LuaValue::Integer(count),
                                  rstd::move(bytes)));
    }
  }
  if (mode == 0)
    return return_value(frame, luato::LuaValue::Nil());
  return return_value(frame, luato::LuaValue::Table(rstd::move(output)));
}

auto batch(luato::CallFrame &frame) -> luato::Result<Rc<Batch>> {
  auto handle = rstd_try(userdata_arg(frame));
  return handle.with<Rc<Batch>>(
      [](const Rc<Batch> &value) -> luato::Result<Rc<Batch>> {
        return Ok(value.clone());
      });
}

auto new_batch(luato::CallFrame &frame) -> luato::BindingResult {
  auto transport = rstd_try(client_transport(frame));
  bool response_mode = false;
  usize limit = usize::MAX;
  if (frame.argument_count() > usize(1)) {
    auto value = rstd_try(extra::take(frame, usize(1)));
    if (!value.is_Nil()) {
      if (!value.is_Table())
        return Err(error("invalid batch options"_str));
      auto options = rstd::move(value).as_Table().value;
      auto results = rstd_try(options.get_in_call(str_key("results"_str)));
      if (!results.is_Bytes() || rstd_try(results.as_utf8()) != "response"_str)
        return Err(error("batch results must be 'response'"_str));
      response_mode = true;
      auto max = rstd_try(options.get_in_call(str_key("limit"_str)));
      auto number = max.is_Integer() ? max.as_Integer().value : i64(12);
      if (number <= i64() || number > i64(1024))
        return Err(error("batch limit must be between 1 and 1024"_str));
      limit = usize(number.to_primitive());
    }
  }
  auto value = make_rc<Batch>(rstd::move(transport), response_mode, limit);
  auto handle = rstd_try(frame.create_userdata(rstd::move(value)));
  return return_value(frame, luato::LuaValue::UserData(rstd::move(handle)));
}

auto batch_add(luato::CallFrame &frame) -> luato::BindingResult {
  auto value = rstd_try(batch(frame));
  auto expected = value->response_mode() ? usize(3) : usize(2);
  if (frame.argument_count() != expected)
    return Err(error("invalid batch add arguments"_str));
  Option<String> key;
  if (value->response_mode())
    key = Some(rstd_try(string_arg(frame, usize(1))));
  auto handle = rstd_try(userdata_arg(frame, expected - usize(1)));
  auto owned = rstd_try(request(handle));
  auto count = rstd_try(value.get()->add(rstd::move(owned), rstd::move(key)));
  frame.push(i64(count.to_primitive()));
  return Ok(usize(1));
}

auto batch_add_response(luato::CallFrame &frame) -> luato::BindingResult {
  auto value = rstd_try(batch(frame));
  auto handle = rstd_try(userdata_arg(frame, usize(1)));
  auto input = rstd_try(response(handle));
  auto count = rstd_try(
      value.get()->add_response(Vec<u8>::from(input->data().body.as_slice())));
  frame.push(i64(count.to_primitive()));
  return Ok(usize(1));
}

auto batch_wait_run(Rc<Batch> value, luato::TableHandle output,
                    luato::UserDataHandle response_handle)
    -> coro<luato::Result<luato::MultiValue>> {
  auto result = rstd_co_try(co_await Batch::wait(value.clone()));
  auto values = luato::MultiValue::make();
  if (result.is_none()) {
    values.push(luato::LuaValue::Nil());
    co_return Ok(rstd::move(values));
  }
  if (!value->response_mode()) {
    auto data = rstd_co_try(rstd::move(result->response));
    values.push(luato::LuaValue::Bytes(rstd::move(data.body)));
    co_return Ok(rstd::move(values));
  }
  rstd_co_try(
      output.set_in_call(str_key("key"_str), str_value(result->key.as_str())));
  if (result->response.is_err()) {
    rstd_co_try(output.set_error_in_call(
        str_key("error"_str), result->response.as_ref().unwrap_err()));
  } else {
    auto native = rstd_co_try(response(response_handle));
    native.get()->receive(rstd::move(result->response).unwrap());
    rstd_co_try(output.set_in_call(
        str_key("response"_str),
        luato::LuaValue::UserData(rstd::move(response_handle))));
  }
  values.push(luato::LuaValue::Table(rstd::move(output)));
  co_return Ok(rstd::move(values));
}

auto batch_wait(luato::CallFrame &frame)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>> {
  auto value = rstd_try(batch(frame));
  rstd_try(value->empty_result());
  auto output = rstd_try(new_table(frame));
  auto response_handle = rstd_try(create_response(frame, ResponseData{}));
  return Ok(batch_wait_run(rstd::move(value), rstd::move(output),
                           rstd::move(response_handle)));
}

auto batch_close_run(Vec<rstd::async::JoinHandle<luato::Result<empty>>> tasks)
    -> coro<luato::Result<luato::MultiValue>> {
  rstd_co_try(co_await Batch::close(rstd::move(tasks)));
  auto values = luato::MultiValue::make();
  values.push(luato::LuaValue::Nil());
  co_return Ok(rstd::move(values));
}

auto batch_close(luato::CallFrame &frame)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>> {
  auto value = rstd_try(batch(frame));
  return Ok(batch_close_run(value.get()->close_tasks()));
}

auto direct_run(TakenRequest request, usize limit, luato::TableHandle output,
                luato::TableHandle headers)
    -> coro<luato::Result<luato::MultiValue>> {
  auto response = rstd_co_try(
      co_await request.transport->send(rstd::move(request.request), limit));
  rstd_co_try(output.set_in_call(
      str_key("status"_str),
      luato::LuaValue::Integer(i64(response.status.to_primitive()))));
  rstd_co_try(output.set_in_call(
      str_key("body"_str), luato::LuaValue::Bytes(rstd::move(response.body))));
  for (const auto &field : response.headers.as_slice()) {
    auto text = rstd::str_::from_utf8(field.value.as_slice());
    if (text.is_ok())
      rstd_co_try(headers.set_in_call(
          str_key(lower_ascii(field.name.as_str()).as_str()),
          str_value(*text)));
  }
  rstd_co_try(output.set_in_call(str_key("header_map"_str),
                                 luato::LuaValue::Table(rstd::move(headers))));
  auto values = luato::MultiValue::make();
  values.push(luato::LuaValue::Table(rstd::move(output)));
  co_return Ok(rstd::move(values));
}

auto direct(const Context &context, luato::CallFrame &frame)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>> {
  RequestData data;
  data.method = rstd_try(string_arg(frame, usize()));
  data.url = rstd_try(string_arg(frame, usize(1)));
  auto fields = rstd_try(table_arg(frame, usize(2)));
  auto keys = rstd_try(fields.keys());
  for (const auto &key : keys) {
    auto value = rstd_try(fields.get_in_call(key));
    if (!value.is_Bytes())
      continue;
    auto field = rstd_try(parse_header(key, value));
    data.headers.push(rstd::move(field));
  }
  auto body = rstd_try(extra::take(frame, usize(3)));
  if (!body.is_Bytes())
    return Err(error("invalid request body"_str));
  if (!body.as_Bytes().value.is_empty())
    data.body = Some(rstd::move(body).as_Bytes().value);
  auto limit = rstd_try(luato::argument<i64>(frame, usize(4)));
  auto transport = rstd_try(context.current());
  rstd_try(transport->check());
  auto output = rstd_try(new_table(frame));
  auto headers = rstd_try(new_table(frame));
  return Ok(
      direct_run(TakenRequest{rstd::move(transport), rstd::move(data)},
                 usize(static_cast<unsigned long long>(limit.to_primitive())),
                 rstd::move(output), rstd::move(headers)));
}
} // namespace binding

auto Context::install(luato::State &state, ref<str> prefix, Rc<Context> owner)
    -> luato::Result<empty> {
  auto client = luato::UserDataSpec<Client>("http.client"_Str);
  client.raw_method("request"_Str, [](luato::CallFrame &frame) {
    return binding::builder(frame);
  });
  client.raw_method("get"_Str, [](luato::CallFrame &frame) {
    return binding::builder(frame, Some("GET"_str));
  });
  client.raw_method("post"_Str, [](luato::CallFrame &frame) {
    return binding::builder(frame, Some("POST"_str));
  });
  client.raw_method("put"_Str, [](luato::CallFrame &frame) {
    return binding::builder(frame, Some("PUT"_str));
  });
  client.raw_method("delete"_Str, [](luato::CallFrame &frame) {
    return binding::builder(frame, Some("DELETE"_str));
  });
  client.raw_async_method("send"_Str, [](luato::CallFrame &frame) {
    return binding::send(frame, usize(1));
  });
  client.raw_method("new_batch"_Str, binding::new_batch);
  client.method("cookie"_Str,
                [](const Client &client, String url,
                   String name) -> luato::Result<Option<String>> {
                  rstd_try(client.transport->check());
                  return Ok(
                      client.transport->cookie(url.as_str(), name.as_str()));
                });
  client.method(
      "set_cookie"_Str,
      [](const Client &client, String url,
         String value) -> luato::Result<bool> {
        rstd_try(client.transport->check());
        return Ok(client.transport->set_cookie(url.as_str(), value.as_str()));
      });
  client.method("clear_cookies"_Str,
                [](const Client &client) -> luato::Result<empty> {
                  rstd_try(client.transport->check());
                  client.transport->clear_cookies();
                  return Ok(empty{});
                });
  rstd_try(state.register_userdata(rstd::move(client)));
  auto builder = luato::UserDataSpec<Builder>("http.request"_Str);
  builder.raw_method("header"_Str, binding::header);
  builder.raw_method("headers"_Str, [](luato::CallFrame &frame) {
    return binding::headers(frame, false);
  });
  builder.raw_method("header_pairs"_Str, [](luato::CallFrame &frame) {
    return binding::headers(frame, true);
  });
  builder.raw_method("body"_Str, binding::body);
  builder.raw_method("json"_Str,
                     [context = owner.clone()](luato::CallFrame &frame) {
                       return binding::json(*context.get(), frame);
                     });
  builder.raw_method("json_codec"_Str, binding::codec);
  builder.raw_method("timeout"_Str, binding::set_timeout);
  builder.raw_method("response_limit"_Str, binding::response_limit);
  builder.raw_method("query"_Str, binding::query);
  builder.raw_method("form"_Str, binding::form);
  builder.raw_method("query_pairs"_Str, [](luato::CallFrame &frame) {
    return binding::url_pairs(frame, false);
  });
  builder.raw_method("form_pairs"_Str, [](luato::CallFrame &frame) {
    return binding::url_pairs(frame, true);
  });
  builder.raw_method("build"_Str, [](luato::CallFrame &frame) {
    return binding::mutate(frame, [](RequestData &) -> luato::Result<empty> {
      return Ok(empty{});
    });
  });
  builder.raw_method("stream"_Str, [](luato::CallFrame &frame) {
    return binding::mutate(frame, [](RequestData &) -> luato::Result<empty> {
      return Ok(empty{});
    });
  });
  builder.raw_async_method("send"_Str, [](luato::CallFrame &frame) {
    return binding::send(frame, usize());
  });
  rstd_try(state.register_userdata(rstd::move(builder)));
  auto response = luato::UserDataSpec<Rc<Response>>("http.response"_Str);
  response.method("status"_Str,
                  [](const Rc<Response> &response) -> luato::Result<i64> {
                    rstd_try(response->check());
                    return Ok(i64(response->data().status.to_primitive()));
                  });
  response.method("url"_Str,
                  [](const Rc<Response> &response) -> luato::Result<String> {
                    rstd_try(response->check());
                    return Ok(response->data().url.clone());
                  });
  response.method("close"_Str,
                  [](Rc<Response> &response) -> luato::Result<luato::LuaValue> {
                    rstd_try(response.get()->close());
                    return Ok(luato::LuaValue::Nil());
                  });
  response.raw_method("with_body"_Str, [](luato::CallFrame &frame) {
    return binding::response_field(frame, 3);
  });
  response.raw_method("header"_Str, [](luato::CallFrame &frame) {
    return binding::response_headers(frame, 0);
  });
  response.raw_method("header_values"_Str, [](luato::CallFrame &frame) {
    return binding::response_headers(frame, 1);
  });
  response.raw_method("header_pairs"_Str, [](luato::CallFrame &frame) {
    return binding::response_headers(frame, 2);
  });
  response.raw_method("headers"_Str, [](luato::CallFrame &frame) {
    return binding::response_headers(frame, 3);
  });
  response.raw_async_method(
      "text"_Str, [context = owner.clone()](luato::CallFrame &frame) {
        return binding::read(*context.get(), frame, false, false);
      });
  response.raw_async_method(
      "bytes"_Str, [context = owner.clone()](luato::CallFrame &frame) {
        return binding::read(*context.get(), frame, false, false);
      });
  response.raw_async_method(
      "json"_Str, [context = owner.clone()](luato::CallFrame &frame) {
        return binding::read(*context.get(), frame, true, false);
      });
  response.raw_async_method(
      "read_body"_Str, [context = owner.clone()](luato::CallFrame &frame) {
        return binding::read(*context.get(), frame, false, true);
      });
  rstd_try(state.register_userdata(rstd::move(response)));
  auto batch = luato::UserDataSpec<Rc<Batch>>("http.batch"_Str);
  batch.raw_method("add"_Str, binding::batch_add);
  batch.raw_method("add_rsp"_Str, binding::batch_add_response);
  batch.method("seal"_Str,
               [](Rc<Batch> &value) -> luato::Result<luato::LuaValue> {
                 rstd_try(value.get()->seal());
                 return Ok(luato::LuaValue::Nil());
               });
  batch.method("cancel"_Str,
               [](Rc<Batch> &value) -> luato::Result<luato::LuaValue> {
                 value.get()->cancel();
                 return Ok(luato::LuaValue::Nil());
               });
  batch.raw_async_method("wait_one"_Str, binding::batch_wait);
  batch.raw_async_method("close"_Str, binding::batch_close);
  rstd_try(state.register_userdata(rstd::move(batch)));
  auto globals = rstd_try(state.globals());
  auto legacy =
      rstd_try(globals.get_table(utility_name(prefix, "json"_str).as_str()));
  auto strict = rstd_try(
      globals.get_table(utility_name(prefix, "json.strict"_str).as_str()));
  legacy_decoder_ = Some(rstd_try(legacy.get_function("decode"_str)));
  strict_decoder_ = Some(rstd_try(strict.get_function("decode"_str)));
  auto module = luato::ModuleSpec(utility_name(prefix, "http"_str));
  module.async_values_function(
      "request"_Str, usize(5),
      [context = owner.clone()](luato::CallFrame &frame) {
        return binding::direct(*context.get(), frame);
      });
  return state.register_module(rstd::move(module));
}
auto install(luato::State &state, ref<str> prefix, Rc<ClientSource> source,
             Rc<JsonContext> json) -> luato::Result<Rc<Context>> {
  auto context = make_rc<Context>(rstd::move(source), rstd::move(json));
  rstd_try(context.get()->install(state, prefix, context.clone()));
  return Ok(rstd::move(context));
}
} // namespace luato::extra::http
