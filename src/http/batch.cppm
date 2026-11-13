module;
#include <rstd/macro.hpp>
export module luato.extra:http.batch;
export import :http.types;
using alloc::rc::Rc;
using rstd::async::coro;
using rstd::async::JoinHandle;
using rstd::sync::Arc;
using rstd::sync::Mutex;
using rstd::time::Duration;
using namespace rstd::prelude;
using namespace rstd::literals;

export namespace luato::extra::http {
struct BatchResult {
  String key;
  luato::Result<ResponseData> response;
};
class Batch {
  struct Slot {
    Mutex<Option<luato::Result<ResponseData>>> result{None()};
    u64 completed{};
    Option<JoinHandle<luato::Result<empty>>> task;
  };
  struct Entry {
    String key;
    Option<Vec<u8>> ready;
    Option<Arc<Slot>> task;
  };
  TransportHandle transport_;
  bool response_mode_{};
  usize limit_;
  bool sealed_{};
  bool cancelled_{};
  u64 sequence_{};
  Vec<String> seen_;
  Vec<Entry> entries_;
  Arc<Mutex<u64>> completed_{Arc<Mutex<u64>>::make(u64())};

  static auto run(TransportHandle transport, RequestData request,
                  Arc<Slot> slot, Arc<Mutex<u64>> completed)
      -> coro<luato::Result<empty>> {
    auto response =
        co_await transport->send(rstd::move(request), usize(8 * 1024 * 1024));
    auto order =
        rstd_co_try(completed->lock(), error("batch state is unavailable"_str));
    auto guard = rstd_co_try(slot->result.lock(),
                             error("batch state is unavailable"_str));
    slot->completed = *order;
    *order += u64(1);
    guard->insert(rstd::move(response));
    co_return Ok(empty{});
  }
  auto key(Option<String> input) -> luato::Result<String> {
    if (response_mode_) {
      if (input.is_none())
        return Err(error("batch add expects a key and request"_str));
      for (const auto &seen : seen_)
        if (seen == *input)
          return Err(error("batch key already used"_str));
      if (seen_.len() >= limit_)
        return Err(error("batch is full"_str));
      seen_.push(input->clone());
      return Ok(rstd::move(*input));
    }
    if (input.is_some())
      return Err(error("batch add expects a request"_str));
    auto result = rstd::format("{}", sequence_.to_primitive());
    sequence_ += u64(1);
    return Ok(rstd::move(result));
  }

public:
  Batch(TransportHandle transport, bool response_mode, usize limit)
      : transport_(rstd::move(transport)), response_mode_(response_mode),
        limit_(limit) {}
  ~Batch() { cancel(); }
  auto response_mode() const -> bool { return response_mode_; }
  auto check_open() const -> luato::Result<empty> {
    rstd_try(transport_->check());
    if (cancelled_ || sealed_)
      return Err(error("batch is closed"_str));
    return Ok(empty{});
  }
  auto add(TakenRequest request, Option<String> input) -> luato::Result<usize> {
    rstd_try(check_open());
    rstd_try(request.transport->check());
    auto runtime = rstd_try(request.transport->runtime());
    auto name = rstd_try(key(rstd::move(input)));
    auto slot = Arc<Slot>::make();
    slot->task = Some(runtime.spawn(run(rstd::move(request.transport),
                                        rstd::move(request.request),
                                        slot.clone(), completed_.clone())));
    entries_.push(Entry{rstd::move(name), None(), Some(rstd::move(slot))});
    return Ok(entries_.len());
  }
  auto add_response(Vec<u8> body) -> luato::Result<usize> {
    rstd_try(check_open());
    if (response_mode_)
      return Err(error("add_rsp requires legacy batch mode"_str));
    auto name = rstd_try(key(None()));
    entries_.push(Entry{rstd::move(name), Some(rstd::move(body)), None()});
    return Ok(entries_.len());
  }
  auto seal() -> luato::Result<empty> {
    rstd_try(transport_->check());
    sealed_ = true;
    return Ok(empty{});
  }
  auto cancel() -> void {
    cancelled_ = true;
    for (auto &entry : entries_)
      if (entry.task.is_some() && (**entry.task).task.is_some())
        (**entry.task).task->abort();
  }
  auto close_tasks() -> Vec<JoinHandle<luato::Result<empty>>> {
    cancel();
    Vec<JoinHandle<luato::Result<empty>>> tasks;
    for (auto &entry : entries_)
      if (entry.task.is_some() && (**entry.task).task.is_some())
        tasks.push(rstd::move(*(**entry.task).task));
    entries_.clear();
    return tasks;
  }
  auto empty_result() const -> luato::Result<bool> {
    rstd_try(transport_->check());
    if (cancelled_)
      return Err(error("batch was cancelled"_str));
    if (entries_.is_empty() && response_mode_ && !sealed_)
      return Err(error("unsealed batch is empty"_str));
    return Ok(entries_.is_empty());
  }
  auto poll() -> luato::Result<Option<BatchResult>> {
    rstd_try(transport_->check());
    if (cancelled_)
      return Err(error("batch was cancelled"_str));
    Option<usize> selected;
    u64 selected_order = u64::MAX;
    for (usize at{}; at < entries_.len(); ++at) {
      auto &entry = entries_[at];
      u64 order{};
      if (entry.ready.is_none()) {
        if (entry.task.is_none())
          continue;
        auto guard = rstd_try((**entry.task).result.lock(),
                              error("batch task failed"_str));
        if (guard->is_none()) {
          if ((**entry.task).task.is_some() &&
              (**entry.task).task->is_finished())
            return Err(error("batch task failed"_str));
          continue;
        }
        order = (**entry.task).completed;
      }
      if (selected.is_none() || order < selected_order) {
        selected = Some(at);
        selected_order = order;
      }
    }
    if (selected.is_none())
      return Ok<Option<BatchResult>>(None());
    auto at = *selected;
    auto entry = rstd::move(entries_[at]);
    auto last = entries_.len() - usize(1);
    if (at != last)
      entries_[at] = rstd::move(entries_[last]);
    (void)entries_.pop();
    for (usize index{}; index < seen_.len(); ++index) {
      if (seen_[index] != entry.key)
        continue;
      auto end = seen_.len() - usize(1);
      if (index != end)
        seen_[index] = rstd::move(seen_[end]);
      (void)seen_.pop();
      break;
    }
    if (entry.ready.is_some()) {
      ResponseData response;
      response.body = rstd::move(*entry.ready);
      return Ok(
          Some(BatchResult{rstd::move(entry.key), Ok(rstd::move(response))}));
    }
    auto guard =
        rstd_try((**entry.task).result.lock(), error("batch task failed"_str));
    return Ok(Some(BatchResult{rstd::move(entry.key), rstd::move(**guard)}));
  }
  static auto wait(Rc<Batch> self) -> coro<luato::Result<Option<BatchResult>>> {
    if (rstd_co_try(self.get()->empty_result()))
      co_return Ok<Option<BatchResult>>(None());
    for (;;) {
      auto result = rstd_co_try(self.get()->poll());
      if (result.is_some())
        co_return Ok(rstd::move(result));
      co_await rstd::async::sleep(Duration::from_millis(u64(1)));
    }
  }
  static auto close(Vec<JoinHandle<luato::Result<empty>>> tasks)
      -> coro<luato::Result<empty>> {
    for (;;) {
      bool pending = false;
      for (const auto &task : tasks)
        if (!task.is_finished())
          pending = true;
      if (!pending)
        co_return Ok(empty{});
      co_await rstd::async::sleep(Duration::from_millis(u64(1)));
    }
  }
};
} // namespace luato::extra::http
