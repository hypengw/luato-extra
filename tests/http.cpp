#include <rstd/macro.hpp>
#include <rstd/test/gtest.hpp>
import luato.extra;
using alloc::rc::make_rc;
using alloc::rc::Rc;
using rstd::async::coro;
using rstd::async::RuntimeHandle;
using rstd::bytes::Bytes;
using rstd::sync::Arc;
using rstd::sync::Mutex;
using rstd::time::Duration;
using namespace rstd::prelude;
using namespace rstd::literals;
namespace http = luato::extra::http;

namespace http_tests {
struct Counts {
  unsigned active{}, peak{}, started{}, finished{};
  unsigned reads{}, cancels{}, closes{};
  usize limit{};
  bool fail{}, pending{}, valid{true};
};
struct Control {
  Mutex<Counts> counts{Counts{}};
};
struct FakeTransport {
  Arc<Control> control;
  RuntimeHandle executor;
  auto check() const -> luato::Result<empty> {
    auto counts = control->counts.lock().unwrap();
    if (!counts->valid)
      return Err(http::error("transport invalidated"_str));
    return Ok(empty{});
  }
  auto send(http::RequestData request, usize limit) const
      -> coro<luato::Result<http::ResponseData>> {
    rstd_co_try(check());
    {
      auto counts = control->counts.lock().unwrap();
      ++counts->started;
      ++counts->active;
      if (counts->active > counts->peak)
        counts->peak = counts->active;
    }
    struct Guard {
      Arc<Control> control;
      ~Guard() {
        auto counts = control->counts.lock().unwrap();
        --counts->active;
        ++counts->finished;
      }
    } guard{control.clone()};
    auto delay = request.url.as_str().contains("slow"_str) ? u64(30) : u64(1);
    co_await rstd::async::sleep(Duration::from_millis(delay));
    rstd_co_try(check());
    http::ResponseData result;
    result.status = u16(200);
    result.url = rstd::move(request.url);
    result.headers = rstd::move(request.headers);
    result.headers.add("X-Proof"_str, "one"_str).unwrap();
    result.headers.add("x-proof"_str, "two"_str).unwrap();
    if (request.timeout.is_some())
      result.headers.add("x-timeout"_str, "set"_str).unwrap();
    result.body = request.body.is_some()
                      ? rstd::move(*request.body)
                      : Vec<u8>::from("{\"ok\":true}"_str.as_bytes());
    if (result.body.len() > limit)
      co_return Err(http::error("transport limit"_str));
    co_return Ok(rstd::move(result));
  }
  auto runtime() const -> luato::Result<RuntimeHandle> {
    return Ok(executor.clone());
  }
  auto cookie(ref<str>, ref<str>) const -> Option<String> {
    return Some("cookie"_Str);
  }
  auto set_cookie(ref<str>, ref<str>) const -> bool { return true; }
  auto clear_cookies() const -> void {}
};
struct FakeBody {
  Arc<Control> control;
  auto check() const -> luato::Result<empty> {
    auto counts = control->counts.lock().unwrap();
    if (!counts->valid)
      return Err(http::error("body invalidated"_str));
    return Ok(empty{});
  }
  auto next() const -> coro<luato::Result<Option<Bytes>>> {
    co_return Ok<Option<Bytes>>(None());
  }
  auto read(usize limit, Duration) const -> coro<luato::Result<Vec<u8>>> {
    bool pending;
    {
      auto counts = control->counts.lock().unwrap();
      ++counts->reads;
      counts->limit = limit;
      if (counts->fail)
        co_return Err(http::error("body read failed"_str));
      pending = counts->pending;
    }
    if (pending)
      co_await rstd::async::sleep(Duration::from_secs(u64(60)));
    co_return Ok(Vec<u8>::from("body"_str.as_bytes()));
  }
  auto cancel() const -> void {
    auto counts = control->counts.lock().unwrap();
    ++counts->cancels;
  }
  auto close() const -> coro<empty> {
    auto counts = control->counts.lock().unwrap();
    ++counts->closes;
    co_return empty{};
  }
};
} // namespace http_tests

namespace rstd {
template <>
struct Impl<http::Transport, http_tests::FakeTransport>
    : ImplBase<http_tests::FakeTransport> {
  auto check() const { return self().check(); }
  auto send(http::RequestData request, usize limit) const {
    return self().send(rstd::move(request), limit);
  }
  auto runtime() const { return self().runtime(); }
  auto cookie(ref<str> url, ref<str> name) const {
    return self().cookie(url, name);
  }
  auto set_cookie(ref<str> url, ref<str> value) const {
    return self().set_cookie(url, value);
  }
  auto clear_cookies() const -> void { self().clear_cookies(); }
};
template <>
struct Impl<http::ResponseBody, http_tests::FakeBody>
    : ImplBase<http_tests::FakeBody> {
  auto check() const { return self().check(); }
  auto next() const { return self().next(); }
  auto read(usize limit, Duration timeout) const {
    return self().read(limit, timeout);
  }
  auto cancel() const -> void { self().cancel(); }
  auto close() const { return self().close(); }
};
} // namespace rstd

namespace http_tests {
class Source final : public http::ClientSource {
  http::TransportHandle transport_;

public:
  explicit Source(http::TransportHandle transport)
      : transport_(rstd::move(transport)) {}
  auto current() -> luato::Result<http::TransportHandle> override {
    return Ok(transport_.clone());
  }
};
auto source(ref<str> text) -> luato::LuaModuleSource {
  return {"http-tests"_Str, "http:test"_Str, "http.lua"_Str,
          Vec<u8>::from(text.as_bytes())};
}
struct Harness {
  rstd::async::Runtime runtime{rstd::async::RuntimeBuilder::current_thread()
                                   .enable_all()
                                   .build()
                                   .unwrap()};
  Arc<Control> control{Arc<Control>::make()};
  http::TransportHandle transport{http::TransportHandle::make(
      FakeTransport{control.clone(), runtime.handle()})};
  luato::State state{
      luato::State::create(luato::StateOptions::build_script()).unwrap()};

  Harness() {
    auto root = luato::ModuleSpec("extra"_Str);
    root.raw_function("client"_Str, usize(),
                      [transport = transport.clone()](
                          luato::CallFrame &frame) -> luato::BindingResult {
                        auto client = rstd_try(frame.create_userdata(
                            http::Client{transport.clone()}));
                        return luato::return_value(frame, rstd::move(client));
                      });
    state.register_module(rstd::move(root)).unwrap();
    auto utilities =
        luato::extra::install_utilities(state, "extra"_str).unwrap();
    auto owner =
        http::install(state, "extra"_str,
                      Rc<http::ClientSource>(new Source(transport.clone())),
                      utilities.json.clone())
            .unwrap();
    owner.reset();
  }
  auto run(ref<str> script) -> luato::Result<String> {
    auto values = rstd_try(state.evaluate_entry(source(script))).into_values();
    auto function = rstd::move(values[usize()].as_Function().value);
    return runtime.block_on(function.call_async_typed<String>());
  }
};
auto response(Arc<Control> control) -> Rc<http::Response> {
  http::ResponseData data;
  data.status = u16(200);
  return make_rc<http::Response>(
      rstd::move(data),
      Some(http::BodyHandle::make(FakeBody{rstd::move(control)})));
}
auto cancel_read(Rc<http::Response> response, Arc<Control> control,
                 RuntimeHandle executor) -> coro<bool> {
  auto task = executor.spawn(
      http::Response::read(response.clone(), http::ReadOptions{}));
  for (;;) {
    bool started;
    {
      auto counts = control->counts.lock().unwrap();
      started = counts->reads != 0u;
    }
    if (started)
      break;
    co_await rstd::async::sleep(Duration::from_millis(u64(1)));
  }
  bool blocked = response.get()->close().is_err() &&
                 response.get()->replace_body(Vec<u8>::make()).is_err();
  task.abort();
  while (!task.is_finished())
    co_await rstd::async::sleep(Duration::from_millis(u64(1)));
  co_return blocked;
}
} // namespace http_tests

TEST(LuatoExtraHttp, InstalledCallbacksOwnContextAndPreserveWireValues) {
  http_tests::Harness test;
  auto result = test.run(R"LUA(
return function()
  collectgarbage('collect')
  local c = extra.client()
  assert(type(c) == 'userdata' and c:cookie('https://fake/', 'x') == 'cookie')
  assert(c:set_cookie('https://fake/', 'x=y') and select('#', c:clear_cookies()) == 0)
  local bytes = string.char(0, 255, 128)..'data'
  local request = c:post('https://fake/path'):body(bytes)
    :header('x-repeat', 'old'):headers({['x-repeat']='replacement'})
    :header_pairs{{'x-repeat','last'},{'x-binary',string.char(255)}}:timeout(0.5)
  local response = c:send(request:build())
  assert(type(request) == 'userdata' and type(response) == 'userdata')
  assert(response:status() == 200 and response:url() == 'https://fake/path')
  assert(response:bytes() == bytes and response:text() == bytes)
  assert(response:header('X-REPEAT') == 'replacement')
  assert(#response:header_values('x-repeat') == 2 and response:header('x-timeout') == 'set')
  assert(response:header('x-binary') == string.char(255))
  assert(#response:header_values('x-proof') == 2 and #response:header_pairs() >= 6)
  assert(response:header('X-PROOF') == 'one' and response:headers()['x-proof'] == 'two')
  for _, pair in ipairs(response:header_pairs()) do
    assert(pair[1] == string.lower(pair[1]))
  end
  local same, body = response:read_body{max_bytes=100, timeout_ms=20}
  assert(same == response and body == bytes)
  assert(response:with_body('new') == response and response:bytes() == 'new')
  assert(select('#', response:close()) == 1 and response:close() == nil)
  assert(not pcall(function() response:bytes() end))
  local direct = _G['extra.http'].request('POST', 'https://fake/direct', {}, bytes, 100)
  assert(direct.status == 200 and direct.body == bytes)
  return 'ok'
end
)LUA"_str);
  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(*result, "ok"_str);
}

TEST(LuatoExtraHttp, RequestTakeIsTypedAndConsumesOnlyAfterValidation) {
  http_tests::Harness test;
  http::RequestData data;
  data.method = "GET"_Str;
  data.url = "https://fake/"_Str;
  auto request = test.state
                     .create_userdata(http::Builder{test.transport.clone(),
                                                    rstd::move(data)})
                     .unwrap();
  auto rejected = http::take_request(
      request, [](const http::RequestData &) -> luato::Result<empty> {
        return Err(http::error("rejected"_str));
      });
  EXPECT_TRUE(rejected.is_err());
  EXPECT_TRUE(http::request(request).is_ok());
  auto taken = http::take_request(request);
  ASSERT_TRUE(taken.is_ok());
  EXPECT_EQ(taken->request.url, "https://fake/"_str);
  EXPECT_TRUE(http::take_request(request).is_err());
  EXPECT_TRUE(http::request(request).is_err());
  auto wrong =
      test.state.create_userdata(http::Client{test.transport.clone()}).unwrap();
  EXPECT_TRUE(http::take_request(wrong).is_err());
  EXPECT_TRUE(wrong.is<http::Client>().unwrap());
}

TEST(LuatoExtraHttp, StrictJsonAndRequestAndResponseOptionsRemainDistinct) {
  http_tests::Harness test;
  auto result = test.run(R"LUA(
return function()
  local c, S = extra.client(), extra.json.strict
  local request = c:post('https://fake/json'):json_codec('strict')
  assert(not pcall(function() request:json({}) end))
  local response = request:json(S.object{items=S.array{}, value=S.null}):send()
  local value = response:json()
  assert(S.kind(value) == 'object' and S.kind(value.items) == 'array' and value.value == S.null)
  local legacy = response:json{codec='legacy'}
  assert(legacy.value == nil)
  assert(not pcall(function() c:get('x'):timeout(0) end))
  assert(not pcall(function() c:get('x'):timeout(0/0) end))
  assert(not pcall(function() c:get('x'):json_codec('unknown') end))
  assert(not pcall(function() c:get('x'):header('bad\nname', 'x') end))
  assert(not pcall(function() c:get('x'):header_pairs({[2]={'x','y'}}) end))
  assert(not pcall(function() c:get('x'):query_pairs{{'x', false}} end))
  assert(not pcall(function() response:bytes{max_bytes=-1} end))
  assert(not pcall(function() response:bytes{timeout_ms=0} end))
  assert(not pcall(function() response:json{codec='other'} end))
  assert(not pcall(function() response:bytes{max_bytes=1} end))
  local query = c:get('https://fake/path#fragment'):query_pairs{{'x','a b'},{'x','/'}}:send()
  assert(query:url() == 'https://fake/path?x=a+b&x=%2F#fragment')
  assert(c:post('https://fake/'):form_pairs{{'x','a b'}}:send():text() == 'x=a+b')
  return 'ok'
end
)LUA"_str);
  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(*result, "ok"_str);
}

TEST(LuatoExtraHttp,
     ResponseBodyFailureAndReplacementReleaseCancellationGuards) {
  http_tests::Harness test;
  auto control = Arc<http_tests::Control>::make();
  {
    auto counts = control->counts.lock().unwrap();
    counts->fail = true;
  }
  auto failed = http_tests::response(control.clone());
  auto read = test.runtime.block_on(
      http::Response::read(failed.clone(), http::ReadOptions{}));
  EXPECT_TRUE(read.is_err());
  EXPECT_TRUE(failed->check().is_err());
  EXPECT_TRUE(failed.get()->replace_body(Vec<u8>::make()).is_err());
  {
    auto counts = control->counts.lock().unwrap();
    EXPECT_EQ(counts->reads, 1u);
    EXPECT_EQ(counts->closes, 1u);
    EXPECT_TRUE(counts->cancels > 0u);
    counts->fail = false;
  }
  auto replacement = http_tests::response(control.clone());
  ASSERT_TRUE(replacement.get()
                  ->replace_body(Vec<u8>::from("replacement"_str.as_bytes()))
                  .is_ok());
  auto bytes = test.runtime.block_on(
      http::Response::read(replacement.clone(), http::ReadOptions{}));
  ASSERT_TRUE(bytes.is_ok());
  EXPECT_EQ(String::from_utf8_unchecked(rstd::move(*bytes)), "replacement"_str);
  auto expired = http_tests::response(control.clone());
  expired.get()->expire();
  EXPECT_TRUE(
      test.runtime
          .block_on(http::Response::read(expired.clone(), http::ReadOptions{}))
          .is_err());
}

TEST(LuatoExtraHttp,
     ReleaseTransfersLiveBodyAndCachedBytesWithoutCancellingDelivery) {
  http_tests::Harness test;
  auto control = Arc<http_tests::Control>::make();
  auto live = http_tests::response(control.clone());
  auto parts = live.get()->release().unwrap();
  ASSERT_TRUE(parts.body.is_some());
  live.get()->expire();
  live.reset();
  {
    auto counts = control->counts.lock().unwrap();
    EXPECT_EQ(counts->cancels, 0u);
  }
  EXPECT_TRUE((*parts.body)->check().is_ok());
  (*parts.body)->cancel();
  test.runtime.block_on((*parts.body)->close());
  auto cached = http_tests::response(control.clone());
  http::ReadOptions options;
  options.limit = usize(42);
  auto bytes =
      test.runtime.block_on(http::Response::read(cached.clone(), options));
  ASSERT_TRUE(bytes.is_ok());
  auto cached_parts = cached.get()->release().unwrap();
  EXPECT_TRUE(cached_parts.body.is_none());
  EXPECT_EQ(String::from_utf8_unchecked(rstd::move(cached_parts.data.body)),
            "body"_str);
  cached.get()->expire();
  cached.reset();
  auto counts = control->counts.lock().unwrap();
  EXPECT_EQ(counts->limit, usize(42));
}

TEST(LuatoExtraHttp, AbortingPendingBodyReadResetsBorrowAndCancelsBody) {
  http_tests::Harness test;
  auto control = Arc<http_tests::Control>::make();
  {
    auto counts = control->counts.lock().unwrap();
    counts->pending = true;
  }
  auto response = http_tests::response(control.clone());
  EXPECT_TRUE(test.runtime.block_on(http_tests::cancel_read(
      response.clone(), control.clone(), test.runtime.handle())));
  auto checked = response->check();
  ASSERT_TRUE(checked.is_err());
  EXPECT_EQ(checked.as_ref().unwrap_err().message.as_str(),
            "response is closed"_str);
  auto counts = control->counts.lock().unwrap();
  EXPECT_TRUE(counts->cancels > 0u);
}

TEST(LuatoExtraHttp, BatchRunsConcurrentlyAndCancelsAndClosesOwnedTasks) {
  http_tests::Harness test;
  auto result = test.run(R"LUA(
return function()
  local c = extra.client()
  local batch = c:new_batch{results='response', limit=2}
  assert(type(batch) == 'userdata')
  batch:add('slow', c:get('https://fake/slow'))
  assert(not pcall(function() batch:add('slow', c:get('https://fake/other')) end))
  batch:add('fast', c:get('https://fake/fast'))
  assert(not pcall(function() batch:add('full', c:get('https://fake/full')) end))
  assert(select('#', batch:seal()) == 1)
  local first, second = batch:wait_one(), batch:wait_one()
  assert(first.key == 'fast' and first.response:status() == 200)
  assert(second.key == 'slow' and batch:wait_one() == nil)
  assert(select('#', batch:close()) == 1)
  local cancelled = c:new_batch()
  cancelled:add(c:get('https://fake/slow'))
  assert(select('#', cancelled:cancel()) == 1)
  cancelled:close()
  assert(not pcall(function() cancelled:wait_one() end))
  local legacy = c:new_batch()
  legacy:add_rsp(c:get('https://fake/fast'):body('ready'):send())
  assert(legacy:wait_one() == 'ready' and legacy:wait_one() == nil)
  legacy:close()
  return 'ok'
end
)LUA"_str);
  ASSERT_TRUE(result.is_ok());
  EXPECT_EQ(*result, "ok"_str);
  auto counts = test.control->counts.lock().unwrap();
  EXPECT_EQ(counts->active, 0u);
  EXPECT_TRUE(counts->peak >= 2u);
}
