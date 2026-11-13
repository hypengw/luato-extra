module;
#include <rstd/macro.hpp>
export module luato.extra:time;
export import luato;
export import rstd;
export import :value;
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
  auto text = reinterpret_cast<const unsigned char *>(
      input->as_str().as_bytes().as_raw_ptr());
  auto length = input->len().to_primitive();
  auto digits = [&](rstd::size_t offset, rstd::size_t count) -> int {
    int value = 0;
    for (rstd::size_t index = 0; index < count; ++index)
      value = value * 10 + (text[offset + index] - '0');
    return value;
  };
  auto valid_digits = [&](rstd::size_t offset, rstd::size_t count) {
    for (rstd::size_t index = 0; index < count; ++index)
      if (text[offset + index] < '0' || text[offset + index] > '9')
        return false;
    return true;
  };
  if (length < 20)
    return Err(fail("invalid RFC3339 timestamp"_str));
  if (text[4] != '-' || text[7] != '-' || text[13] != ':' || text[16] != ':')
    return Err(fail("invalid RFC3339 timestamp"_str));
  if (text[10] != 'T' && text[10] != 't' && text[10] != ' ')
    return Err(fail("invalid RFC3339 timestamp"_str));
  if (!valid_digits(0, 4) || !valid_digits(5, 2) || !valid_digits(8, 2) ||
      !valid_digits(11, 2) || !valid_digits(14, 2) || !valid_digits(17, 2))
    return Err(fail("invalid RFC3339 timestamp"_str));
  auto year = digits(0, 4);
  auto month = digits(5, 2);
  auto day = digits(8, 2);
  auto hour = digits(11, 2);
  auto minute = digits(14, 2);
  auto second = digits(17, 2);
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 60)
    return Err(fail("invalid RFC3339 timestamp"_str));
  rstd::size_t position = 19;
  int millis = 0;
  if (position < length && text[position] == '.') {
    ++position;
    auto start = position;
    while (position < length && text[position] >= '0' && text[position] <= '9')
      ++position;
    auto count = rstd::min<rstd::size_t>(3, position - start);
    for (rstd::size_t index = 0; index < count; ++index)
      millis = millis * 10 + (text[start + index] - '0');
    for (rstd::size_t index = count; index < 3; ++index)
      millis *= 10;
  }
  int offset = 0;
  if (position < length && (text[position] == 'Z' || text[position] == 'z')) {
    ++position;
  } else if (position + 6 <= length &&
             (text[position] == '+' || text[position] == '-')) {
    int sign = text[position] == '-' ? -1 : 1;
    if (!valid_digits(position + 1, 2) || !valid_digits(position + 4, 2) ||
        text[position + 3] != ':')
      return Err(fail("invalid RFC3339 timestamp"_str));
    offset =
        sign * (digits(position + 1, 2) * 3600 + digits(position + 4, 2) * 60);
    position += 6;
  } else {
    return Err(fail("invalid RFC3339 timestamp"_str));
  }
  if (position != length)
    return Err(fail("invalid RFC3339 timestamp"_str));
  int adjusted_year = month <= 2 ? year - 1 : year;
  int era =
      adjusted_year >= 0 ? adjusted_year / 400 : (adjusted_year - 399) / 400;
  int yoe = adjusted_year - era * 400;
  int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  auto days = static_cast<long long>(era) * 146097 + doe - 719468;
  auto epoch = days * 86400 + hour * 3600 + minute * 60 + second;
  frame.push(i64(epoch * 1000 - offset * 1000LL + millis));
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
