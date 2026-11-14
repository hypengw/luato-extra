module;
#include <rstd/macro.hpp>
export module luato.extra:time;
export import luato;
export import rstd;
export import :value;
using rstd::time::parse_rfc3339;
using rstd::time::SystemTime;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra {
inline auto time_now(luato::CallFrame &frame) -> luato::BindingResult {
  auto now = SystemTime::now().as_unix_time();
  frame.push(now.seconds * i64(1000) +
             i64((now.nanoseconds / u32(1000000)).to_primitive()));
  return Ok(usize(1));
}
inline auto time_parse(luato::CallFrame &frame) -> luato::BindingResult {
  auto input = string_arg(frame, usize());
  if (input.is_err())
    return Err(rstd::move(input).unwrap_err());
  auto parsed = parse_rfc3339(input->as_str());
  if (parsed.is_err())
    return Err(fail("invalid RFC3339 timestamp"_str));
  auto millis = parsed->unix_milliseconds();
  if (millis.is_err())
    return Err(fail("invalid RFC3339 timestamp"_str));
  frame.push(*millis);
  return Ok(usize(1));
}
inline auto time_module(ref<str> prefix) -> luato::ModuleSpec {
  auto time = luato::ModuleSpec(utility_name(prefix, "time"_str));
  time.raw_function(String::make("parse_rfc3339"_str), usize(1),
                    [](luato::CallFrame &frame) -> luato::BindingResult {
                      return time_parse(frame);
                    });
  time.raw_function(String::make("now"_str), usize(0),
                    [](luato::CallFrame &frame) -> luato::BindingResult {
                      return time_now(frame);
                    });
  return time;
}
} // namespace luato::extra
