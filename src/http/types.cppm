module;
#include <rstd/macro.hpp>
export module luato.extra:http.types;
export import luato;
export import lihttpto;
export import rstd;
using alloc::rc::Rc;
using rstd::async::coro;
using rstd::async::RuntimeHandle;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
using rstd::time::Duration;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra::http {
inline auto error(ref<str> message) -> luato::Error {
  return luato::Error::make(ErrorKind::Runtime, "http"_Str,
                            String::make(message));
}
struct RequestData {
  String method;
  String url;
  lihttpto::Headers headers;
  Option<Vec<u8>> body;
  Option<Duration> timeout;
  bool strict_json{};

  auto clone() const -> RequestData {
    Option<Vec<u8>> copied;
    if (body.is_some())
      copied = Some(Vec<u8>::from(body->as_slice()));
    return {method.clone(),     url.clone(), headers.clone(),
            rstd::move(copied), timeout,     strict_json};
  }
};
struct ResponseData {
  u16 status{};
  Vec<u8> body;
  lihttpto::Headers headers;
  String url;
};
struct ReadOptions {
  usize limit{8 * 1024 * 1024};
  Duration timeout = Duration::from_secs(u64(30));
  bool strict{};
};
struct Transport {
  template <class Self, class = void> struct Api {
    using Trait = Transport;
    auto check() const -> luato::Result<empty> {
      return rstd::trait_call<0>(this);
    }
    auto send(RequestData request, usize limit) const
        -> coro<luato::Result<ResponseData>> {
      return rstd::trait_call<1>(this, rstd::move(request), limit);
    }
    auto runtime() const -> luato::Result<RuntimeHandle> {
      return rstd::trait_call<2>(this);
    }
    auto cookie(ref<str> url, ref<str> name) const -> Option<String> {
      return rstd::trait_call<3>(this, url, name);
    }
    auto set_cookie(ref<str> url, ref<str> value) const -> bool {
      return rstd::trait_call<4>(this, url, value);
    }
    auto clear_cookies() const -> void { rstd::trait_call<5>(this); }
  };
  template <class T>
  using Funcs = rstd::TraitFuncs<&T::check, &T::send, &T::runtime, &T::cookie,
                                 &T::set_cookie, &T::clear_cookies>;
};
using TransportHandle = Arc<dyn<Transport>>;
class ClientSource {
public:
  virtual ~ClientSource() = default;
  virtual auto current() -> luato::Result<TransportHandle> = 0;
};
struct ResponseBody {
  template <class Self, class = void> struct Api {
    using Trait = ResponseBody;
    auto check() const -> luato::Result<empty> {
      return rstd::trait_call<0>(this);
    }
    auto next() const -> coro<luato::Result<Option<Bytes>>> {
      return rstd::trait_call<1>(this);
    }
    auto read(usize limit, Duration timeout) const
        -> coro<luato::Result<Vec<u8>>> {
      return rstd::trait_call<2>(this, limit, timeout);
    }
    auto cancel() const -> void { rstd::trait_call<3>(this); }
    auto close() const -> coro<empty> { return rstd::trait_call<4>(this); }
  };
  template <class T>
  using Funcs =
      rstd::TraitFuncs<&T::check, &T::next, &T::read, &T::cancel, &T::close>;
};
using BodyHandle = Arc<dyn<ResponseBody>>;
struct ResponseParts {
  ResponseData data;
  Option<BodyHandle> body;
};
class Response {
  ResponseData data_;
  Option<BodyHandle> body_;
  bool reading_{};
  bool closed_{};
  bool expired_{};
  bool strict_{};

public:
  explicit Response(ResponseData data, Option<BodyHandle> body = None(),
                    bool strict = false)
      : data_(rstd::move(data)), body_(rstd::move(body)), strict_(strict) {}
  ~Response() {
    if (body_.is_some())
      (*body_)->cancel();
  }
  auto data() const -> const ResponseData & { return data_; }
  auto strict_json() const -> bool { return strict_; }
  auto receive(ResponseData data) -> void { data_ = rstd::move(data); }
  auto check() const -> luato::Result<empty> {
    if (expired_)
      return Err(error("response expired"_str));
    if (closed_)
      return Err(error("response is closed"_str));
    if (reading_)
      return Err(error("response body is being read"_str));
    return Ok(empty{});
  }
  auto replace_body(Vec<u8> bytes) -> luato::Result<empty> {
    rstd_try(check());
    if (body_.is_some())
      (*body_)->cancel();
    body_ = None();
    data_.body = rstd::move(bytes);
    return Ok(empty{});
  }
  auto close() -> luato::Result<empty> {
    if (reading_)
      return Err(error("response body is being read"_str));
    if (body_.is_some())
      (*body_)->cancel();
    body_ = None();
    closed_ = true;
    return Ok(empty{});
  }
  auto release() -> luato::Result<ResponseParts> {
    rstd_try(check());
    expired_ = true;
    return Ok(ResponseParts{rstd::move(data_), body_.take()});
  }
  auto expire() -> void {
    expired_ = true;
    if (body_.is_some())
      (*body_)->cancel();
    body_ = None();
  }
  static auto read(Rc<Response> self, ReadOptions options)
      -> coro<luato::Result<Vec<u8>>> {
    rstd_co_try(self.get()->check());
    if (self.get()->body_.is_some()) {
      auto body = rstd::move(*self.get()->body_);
      self.get()->body_ = None();
      rstd_co_try(body->check());
      self.get()->reading_ = true;
      struct Guard {
        Rc<Response> owner;
        BodyHandle body;
        bool complete{};
        ~Guard() {
          body->cancel();
          owner.get()->reading_ = false;
          if (!complete)
            owner.get()->closed_ = true;
        }
      } guard{self.clone(), body.clone()};
      auto bytes = co_await body->read(options.limit, options.timeout);
      body->cancel();
      co_await body->close();
      auto result = rstd_co_try(rstd::move(bytes));
      rstd_co_try(body->check());
      self.get()->data_.body = rstd::move(result);
      guard.complete = true;
    }
    if (self.get()->data_.body.len() > options.limit)
      co_return Err(error("response exceeds limit"_str));
    co_return Ok(Vec<u8>::from(self.get()->data_.body.as_slice()));
  }
};
struct Client {
  TransportHandle transport;
};
struct Builder {
  TransportHandle transport;
  RequestData request;
};
using Request = Builder;
struct TakenRequest {
  TransportHandle transport;
  RequestData request;
};
} // namespace luato::extra::http
