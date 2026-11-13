module;
#include <rstd/macro.hpp>
export module luato.extra:random;
export import luato;
export import rstd;
export import :value;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra {
inline auto random_hex(luato::CallFrame &frame) -> luato::BindingResult {
  auto count = take(frame, usize());
  if (count.is_err())
    return Err(rstd::move(count).unwrap_err());
  if (!count->is_Integer())
    return Err(fail("random.hex expects an integer"_str));
  auto length = count->as_Integer().value.to_primitive();
  if (length < 0 || length > 256)
    return Err(fail("random.hex size is out of range"_str));
  auto bytes = Vec<u8>::make();
  for (long long index = 0; index < length; ++index)
    bytes.push(u8());
  auto filled = rstd::random::fill_secure(bytes.deref_mut());
  if (filled.is_err())
    return Err(utility_error(rstd::move(filled).unwrap_err(),
                             "secure random generation failed"_str,
                             "luato.extra.random"_str));
  auto out = String::make();
  for (const auto &byte : bytes.as_slice())
    out.push_str(
        rstd::format("{:02x}", unsigned(byte.to_primitive())).as_str());
  frame.push(rstd::move(out));
  return Ok(usize(1));
}
inline auto random_module(ref<str> prefix) -> luato::ModuleSpec {
  auto random = luato::ModuleSpec(utility_name(prefix, "random"_str));
  random.raw_function(String::make("hex"_str), usize(1),
                      [](luato::CallFrame &frame) -> luato::BindingResult {
                        return random_hex(frame);
                      });
  return random;
}
} // namespace luato::extra
