module;
#include <rstd/macro.hpp>
export module luato.extra:http;
export import :http.types;
export import :http.batch;
export import :json;
using alloc::rc::make_rc;
using alloc::rc::Rc;
using rstd::async::coro;
using rstd::time::Duration;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra::http {
auto userdata_arg(luato::CallFrame &frame, usize index = usize())
    -> luato::Result<luato::UserDataHandle>;
auto return_value(luato::CallFrame &frame, luato::LuaValue value)
    -> luato::BindingResult;
auto return_self(luato::CallFrame &frame) -> luato::BindingResult;
auto request(const luato::UserDataHandle &handle)
    -> luato::Result<TakenRequest>;
template <class Validate>
inline auto take_request(const luato::UserDataHandle &handle, Validate validate)
    -> luato::Result<TakenRequest> {
  rstd_try(
      handle.with<Builder>([&](const Builder &value) -> luato::Result<empty> {
        rstd_try(value.transport->check());
        return validate(value.request);
      }));
  auto builder = rstd_try(handle.take<Builder>());
  return Ok(
      TakenRequest{rstd::move(builder.transport), rstd::move(builder.request)});
}
auto take_request(const luato::UserDataHandle &handle)
    -> luato::Result<TakenRequest>;
auto response(const luato::UserDataHandle &handle)
    -> luato::Result<Rc<Response>>;
template <class Source>
inline auto create_response(Source &source, ResponseData data,
                            Option<BodyHandle> body = None(),
                            bool strict = false)
    -> luato::Result<luato::UserDataHandle> {
  return source.create_userdata(
      make_rc<Response>(rstd::move(data), rstd::move(body), strict));
}
class Context {
  Rc<ClientSource> source_;
  Rc<JsonContext> json_;
  Option<luato::FunctionHandle> legacy_decoder_;
  Option<luato::FunctionHandle> strict_decoder_;

public:
  Context(Rc<ClientSource> source, Rc<JsonContext> json);
  auto json() const -> const JsonContext &;
  auto current() const -> luato::Result<TransportHandle>;
  auto create_client(luato::CallFrame &frame) const -> luato::BindingResult;
  auto decoder(bool strict) const -> luato::Result<luato::FunctionHandle>;

private:
  auto install(luato::State &state, ref<str> prefix, Rc<Context> owner)
      -> luato::Result<empty>;
  friend auto install(luato::State &state, ref<str> prefix,
                      Rc<ClientSource> source, Rc<JsonContext> json)
      -> luato::Result<Rc<Context>>;
};

namespace binding {
auto client_transport(luato::CallFrame &frame)
    -> luato::Result<TransportHandle>;
auto builder(luato::CallFrame &frame, Option<ref<str>> fixed = None())
    -> luato::BindingResult;
template <class F>
inline auto mutate(luato::CallFrame &frame, F function)
    -> luato::BindingResult {
  auto handle = rstd_try(userdata_arg(frame));
  rstd_try(
      handle.with_mut<Builder>([&](Builder &value) -> luato::Result<empty> {
        rstd_try(value.transport->check());
        return function(value.request);
      }));
  return return_self(frame);
}
auto parse_header(const luato::LuaValue &name, const luato::LuaValue &value)
    -> luato::Result<lihttpto::Header>;
auto header(luato::CallFrame &frame) -> luato::BindingResult;
auto dense(luato::TableHandle &table) -> luato::Result<usize>;
auto pair(luato::TableHandle &table, usize index)
    -> luato::Result<rstd::tuple<luato::LuaValue, luato::LuaValue>>;
auto headers(luato::CallFrame &frame, bool pairs) -> luato::BindingResult;
auto default_header(RequestData &request, ref<str> name, ref<str> value,
                    bool append = false) -> luato::Result<empty>;
auto body(luato::CallFrame &frame) -> luato::BindingResult;
auto json(const Context &context, luato::CallFrame &frame)
    -> luato::BindingResult;
auto codec(luato::CallFrame &frame) -> luato::BindingResult;
auto timeout(const luato::LuaValue &value) -> luato::Result<Duration>;
auto set_timeout(luato::CallFrame &frame) -> luato::BindingResult;
auto response_limit(luato::CallFrame &frame) -> luato::BindingResult;
auto url_pairs(luato::CallFrame &frame, bool form) -> luato::BindingResult;
auto form(luato::CallFrame &frame) -> luato::BindingResult;
auto query(luato::CallFrame &frame) -> luato::BindingResult;
auto send_run(TakenRequest request, luato::UserDataHandle output)
    -> coro<luato::Result<luato::MultiValue>>;
auto send(luato::CallFrame &frame, usize index)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>>;
auto read_options(luato::CallFrame &frame, bool strict, bool json)
    -> luato::Result<ReadOptions>;
auto read_run(Rc<Response> value, luato::UserDataHandle handle,
              ReadOptions options, Option<luato::FunctionHandle> decoder,
              bool with_self) -> coro<luato::Result<luato::MultiValue>>;
auto read(const Context &context, luato::CallFrame &frame, bool json,
          bool with_self)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>>;
auto response_field(luato::CallFrame &frame, unsigned field)
    -> luato::BindingResult;
auto response_headers(luato::CallFrame &frame, unsigned mode)
    -> luato::BindingResult;
auto batch(luato::CallFrame &frame) -> luato::Result<Rc<Batch>>;
auto new_batch(luato::CallFrame &frame) -> luato::BindingResult;
auto batch_add(luato::CallFrame &frame) -> luato::BindingResult;
auto batch_add_response(luato::CallFrame &frame) -> luato::BindingResult;
auto batch_wait_run(Rc<Batch> value, luato::TableHandle output,
                    luato::UserDataHandle response_handle)
    -> coro<luato::Result<luato::MultiValue>>;
auto batch_wait(luato::CallFrame &frame)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>>;
auto batch_close_run(Vec<rstd::async::JoinHandle<luato::Result<empty>>> tasks)
    -> coro<luato::Result<luato::MultiValue>>;
auto batch_close(luato::CallFrame &frame)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>>;
auto direct_run(TakenRequest request, usize limit, luato::TableHandle output,
                luato::TableHandle headers)
    -> coro<luato::Result<luato::MultiValue>>;
auto direct(const Context &context, luato::CallFrame &frame)
    -> luato::Result<coro<luato::Result<luato::MultiValue>>>;
} // namespace binding

auto install(luato::State &state, ref<str> prefix, Rc<ClientSource> source,
             Rc<JsonContext> json) -> luato::Result<Rc<Context>>;
} // namespace luato::extra::http
