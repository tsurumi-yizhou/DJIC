module;

#include <algorithm>
#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#define SIMDJSON_SERIALIZATION_INL_H
#define SIMDJSON_DOM_FRACTURED_JSON_INL_H
#include <simdjson.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

export module asterisk;

namespace asterisk {

export struct Account {
  std::string username;
  std::string domain;
  std::string password;
};

export struct Callbacks {
  std::function<void(const std::string &number)> on_incoming_call;
  std::function<void()> on_call_answered;
  std::function<void()> on_call_ended;
  std::function<void(const std::string &number, const std::string &message)>
      on_message;
};

export enum class State {
  Idle,
  Starting,
  Registered,
  Registering,
  InCall,
  Stopped,
};

export constexpr std::string_view state_name(State state) {
  switch (state) {
  case State::Idle:
    return "idle";
  case State::Starting:
    return "starting";
  case State::Registered:
    return "registered";
  case State::Registering:
    return "registering";
  case State::InCall:
    return "in_call";
  case State::Stopped:
    return "stopped";
  }
  return "unknown";
}

export class Client {
public:
  virtual ~Client() = default;

  virtual bool start(const Account &account, Callbacks callbacks) = 0;
  virtual void stop() = 0;
  virtual bool dial(const std::string &number) = 0;
  virtual bool answer() = 0;
  virtual bool hangup() = 0;
  virtual bool send_message(const std::string &number,
                            const std::string &message) = 0;
  virtual bool in_call() const = 0;
  virtual State state() const = 0;
  virtual std::string error() const = 0;
};

export std::unique_ptr<Client> create_client();

namespace detail {

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::use_awaitable;
using boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;

constexpr std::string_view DEFAULT_HOST = "127.0.0.1";
constexpr std::string_view DEFAULT_ARI_PORT = "8088";
constexpr std::string_view DEFAULT_AMI_PORT = "5038";
constexpr std::string_view DEFAULT_APPLICATION = "djic";
constexpr std::string_view DEFAULT_ARI_USERNAME_VARIABLE =
    "ASTERISK_ARI_USERNAME";
constexpr std::string_view DEFAULT_ARI_PASSWORD_VARIABLE =
    "ASTERISK_ARI_PASSWORD";
constexpr std::string_view DEFAULT_AMI_USERNAME_VARIABLE =
    "ASTERISK_AMI_USERNAME";
constexpr std::string_view DEFAULT_AMI_PASSWORD_VARIABLE =
    "ASTERISK_AMI_PASSWORD";
constexpr std::string_view DEFAULT_HOST_VARIABLE = "ASTERISK_HOST";
constexpr std::string_view DEFAULT_ARI_PORT_VARIABLE = "ASTERISK_ARI_PORT";
constexpr std::string_view DEFAULT_AMI_PORT_VARIABLE = "ASTERISK_AMI_PORT";
constexpr std::string_view DEFAULT_APPLICATION_VARIABLE =
    "ASTERISK_ARI_APPLICATION";
constexpr std::string_view DEFAULT_MESSAGE_FROM_VARIABLE =
    "ASTERISK_MESSAGE_FROM";
constexpr std::string_view MESSAGE_ACTION = "MessageSend";
constexpr std::string_view AMI_DELIMITER = "\r\n\r\n";
constexpr size_t MAX_NUMBER_LENGTH = 128;
constexpr size_t MAX_CREDENTIAL_LENGTH = 256;
constexpr size_t MAX_MESSAGE_LENGTH = 4096;
constexpr size_t MAX_AMI_FRAME_LENGTH = 64 * 1024;
constexpr size_t MAX_ARI_EVENT_LENGTH = 1024 * 1024;
constexpr size_t MAX_ARI_RESPONSE_LENGTH = 1024 * 1024;
constexpr auto INITIAL_RETRY_DELAY = std::chrono::seconds(1);
constexpr auto MAX_RETRY_DELAY = std::chrono::seconds(30);

std::string environment(std::string_view name, std::string fallback = {}) {
  const std::string key(name);
  // std::getenv 是不可避免的 C ABI 边界；裸指针只在此处读取，绝不向业务层泄漏。
  const auto *raw_value = std::getenv(key.c_str());
  return raw_value == nullptr ? fallback : std::string(raw_value);
}

constexpr std::string base64(std::string_view value) {
  constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve((value.size() + 2) / 3 * 4);
  for (size_t index = 0; index < value.size(); index += 3) {
    const unsigned first = static_cast<unsigned char>(value[index]);
    const unsigned second = index + 1 < value.size()
                                ? static_cast<unsigned char>(value[index + 1])
                                : 0U;
    const unsigned third = index + 2 < value.size()
                               ? static_cast<unsigned char>(value[index + 2])
                               : 0U;
    result.push_back(alphabet[first >> 2U]);
    result.push_back(alphabet[((first & 0x03U) << 4U) | (second >> 4U)]);
    result.push_back(index + 1 < value.size()
                         ? alphabet[((second & 0x0FU) << 2U) | (third >> 6U)]
                         : '=');
    result.push_back(index + 2 < value.size() ? alphabet[third & 0x3FU] : '=');
  }
  return result;
}

constexpr bool is_unreserved(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '-' || value == '_' ||
         value == '.' || value == '~';
}

constexpr std::string url_encode(std::string_view value) {
  constexpr char hex[] = "0123456789ABCDEF";
  std::string result;
  for (const char character : value) {
    if (is_unreserved(character)) {
      result.push_back(character);
    } else {
      const auto byte = static_cast<unsigned char>(character);
      result.push_back('%');
      result.push_back(hex[byte >> 4U]);
      result.push_back(hex[byte & 0x0FU]);
    }
  }
  return result;
}

constexpr bool contains_line_break(std::string_view value) {
  return std::ranges::any_of(value, [](char character) {
    return character == '\r' || character == '\n' || character == '\0';
  });
}

constexpr bool valid_protocol_field(std::string_view value,
                                    size_t maximum_length) {
  return !value.empty() && value.size() <= maximum_length &&
         !contains_line_break(value);
}

constexpr bool valid_port(std::string_view value) {
  return value.size() <= 5 && !value.empty() &&
         std::ranges::all_of(value, [](char character) {
           return character >= '0' && character <= '9';
         });
}

constexpr bool is_local_host(std::string_view value) {
  return value == "127.0.0.1" || value == "::1";
}

struct AmiFields {
  std::string_view response;
  std::string_view event;
  std::string_view user_event;
  std::string_view from;
  std::string_view caller_id;
  std::string_view body;
  std::string_view content;
};

AmiFields parse_ami_message(std::string_view message) {
  AmiFields fields;
  size_t begin = 0;
  while (begin < message.size()) {
    const size_t end = message.find("\r\n", begin);
    const std::string_view line = message.substr(
        begin,
        end == std::string_view::npos ? message.size() - begin : end - begin);
    const size_t separator = line.find(':');
    if (separator != std::string_view::npos) {
      size_t value_begin = separator + 1;
      while (value_begin < line.size() && line[value_begin] == ' ')
        ++value_begin;
      const std::string_view key = line.substr(0, separator);
      const std::string_view value = line.substr(value_begin);
      if (key == "Response")
        fields.response = value;
      else if (key == "Event")
        fields.event = value;
      else if (key == "UserEvent")
        fields.user_event = value;
      else if (key == "From")
        fields.from = value;
      else if (key == "CallerIDNum")
        fields.caller_id = value;
      else if (key == "Body")
        fields.body = value;
      else if (key == "Content")
        fields.content = value;
    }
    if (end == std::string_view::npos)
      break;
    begin = end + 2;
  }
  return fields;
}

struct Settings {
  std::string host =
      environment(DEFAULT_HOST_VARIABLE, std::string(DEFAULT_HOST));
  std::string ari_port =
      environment(DEFAULT_ARI_PORT_VARIABLE, std::string(DEFAULT_ARI_PORT));
  std::string ami_port =
      environment(DEFAULT_AMI_PORT_VARIABLE, std::string(DEFAULT_AMI_PORT));
  std::string application = environment(DEFAULT_APPLICATION_VARIABLE,
                                        std::string(DEFAULT_APPLICATION));
  std::string message_from = environment(DEFAULT_MESSAGE_FROM_VARIABLE);
  std::string ari_username = environment(DEFAULT_ARI_USERNAME_VARIABLE);
  std::string ari_password = environment(DEFAULT_ARI_PASSWORD_VARIABLE);
  std::string ami_username = environment(DEFAULT_AMI_USERNAME_VARIABLE);
  std::string ami_password = environment(DEFAULT_AMI_PASSWORD_VARIABLE);
  std::string ari_authorization;
  std::string application_target;

  Settings() {
    if (message_from.empty())
      message_from = "pjsip:" + application;
  }
};

struct MessageRequest {
  std::string number;
  std::string message;
};

class AsteriskClient {
public:
  AsteriskClient() : work_(boost::asio::make_work_guard(io_)) {}

  ~AsteriskClient() { stop(); }

  AsteriskClient(const AsteriskClient &) = delete;
  AsteriskClient &operator=(const AsteriskClient &) = delete;

  bool start(const Account &account, Callbacks callbacks) {
    std::lock_guard lock(mutex_);
    if (started_ || state_ == State::Stopped)
      return false;
    callbacks_ = std::move(callbacks);
    if (!account.username.empty()) {
      settings_.ari_username = account.username;
      settings_.ami_username = account.username;
    }
    if (!account.password.empty()) {
      settings_.ari_password = account.password;
      settings_.ami_password = account.password;
    }
    if (!account.domain.empty())
      settings_.host = account.domain;
    const bool valid_settings =
        is_local_host(settings_.host) &&
        valid_protocol_field(settings_.host, MAX_CREDENTIAL_LENGTH) &&
        valid_port(settings_.ari_port) && valid_port(settings_.ami_port) &&
        valid_protocol_field(settings_.application, MAX_CREDENTIAL_LENGTH) &&
        valid_protocol_field(settings_.message_from, MAX_CREDENTIAL_LENGTH) &&
        valid_protocol_field(settings_.ari_username, MAX_CREDENTIAL_LENGTH) &&
        valid_protocol_field(settings_.ari_password, MAX_CREDENTIAL_LENGTH) &&
        valid_protocol_field(settings_.ami_username, MAX_CREDENTIAL_LENGTH) &&
        valid_protocol_field(settings_.ami_password, MAX_CREDENTIAL_LENGTH);
    if (!valid_settings) {
      error_ = "Asterisk configuration is missing or contains invalid protocol "
               "fields";
      return false;
    }
    settings_.ari_authorization =
        "Basic " +
        base64(settings_.ari_username + ":" + settings_.ari_password);
    settings_.application_target = url_encode(settings_.application);
    started_ = true;
    state_ = State::Starting;
    worker_ = std::thread([this]() { io_.run(); });
    co_spawn(io_, ari_event_loop(), detached);
    co_spawn(io_, ami_event_loop(), detached);
    return true;
  }

  void stop() {
    {
      std::lock_guard lock(mutex_);
      if (!started_)
        return;
      started_ = false;
      state_ = State::Stopped;
    }
    work_.reset();
    io_.stop();
    if (worker_.joinable())
      worker_.join();
  }

  bool dial(const std::string &number) {
    if (!valid_number(number))
      return fail("dial number is empty or contains invalid characters");
    std::string target = "PJSIP/" + number;
    {
      std::lock_guard lock(mutex_);
      if (!started_)
        return fail_locked("Asterisk client is not started");
      state_ = State::Registering;
      dialing_ = true;
      error_.clear();
    }
    co_spawn(io_, originate(std::move(target)), detached);
    return true;
  }

  bool answer() {
    const auto channel = current_channel();
    if (!channel)
      return fail("there is no active Asterisk channel");
    co_spawn(io_, answer_channel(*channel), detached);
    return true;
  }

  bool hangup() {
    const auto channel = current_channel();
    if (!channel)
      return fail("there is no active Asterisk channel");
    co_spawn(io_, hangup_channel(*channel), detached);
    return true;
  }

  bool send_message(const std::string &number, const std::string &message) {
    if (!valid_number(number))
      return fail("message number is empty or contains invalid characters");
    if (message.empty() || message.size() > MAX_MESSAGE_LENGTH ||
        contains_line_break(message))
      return fail("message is empty, too long, or contains invalid characters");
    {
      std::lock_guard lock(mutex_);
      if (!started_)
        return fail_locked("Asterisk client is not started");
    }
    boost::asio::post(
        io_, [this, request = MessageRequest{number, message}]() mutable {
          message_queue_.push_back(std::move(request));
          if (!message_sender_running_) {
            message_sender_running_ = true;
            co_spawn(io_, send_ami_message(), detached);
          }
        });
    return true;
  }

  bool in_call() const {
    std::lock_guard lock(mutex_);
    return state_ == State::InCall;
  }

  State state() const {
    std::lock_guard lock(mutex_);
    return state_;
  }

  std::string error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

private:
  static bool valid_number(std::string_view number) {
    if (number.empty() || number.size() > MAX_NUMBER_LENGTH)
      return false;
    return std::ranges::all_of(number, [](char value) {
      return (value >= '0' && value <= '9') || value == '+' || value == '*' ||
             value == '#' || value == '-' || value == '.';
    });
  }

  bool fail(std::string message) {
    std::lock_guard lock(mutex_);
    return fail_locked(std::move(message));
  }

  bool fail_locked(std::string message) {
    error_ = std::move(message);
    return false;
  }

  std::optional<std::string> current_channel() const {
    std::lock_guard lock(mutex_);
    if (channel_id_.empty())
      return std::nullopt;
    return channel_id_;
  }

  void set_error(std::string message) {
    std::lock_guard lock(mutex_);
    error_ = std::move(message);
  }

  void handle_ari_event(const simdjson::padded_string &payload) {
    auto document_result = json_parser_.parse(payload);
    if (document_result.error() != simdjson::SUCCESS) {
      set_error(std::string("ARI event JSON parse failed: ") +
                simdjson::error_message(document_result.error()));
      return;
    }
    const simdjson::dom::element document = document_result.value();
    std::string_view type;
    if (document["type"].get(type) != simdjson::SUCCESS)
      return;
    simdjson::dom::object channel;
    if (document["channel"].get(channel) != simdjson::SUCCESS)
      return;
    std::string_view id_view;
    if (channel["id"].get(id_view) != simdjson::SUCCESS)
      return;
    const std::string id(id_view);
    if (type == "StasisStart") {
      std::string_view number_view;
      std::string number;
      simdjson::dom::object caller;
      if (channel["caller"].get(caller) == simdjson::SUCCESS &&
          caller["number"].get(number_view) == simdjson::SUCCESS)
        number.assign(number_view);
      std::function<void(const std::string &)> callback;
      {
        std::lock_guard lock(mutex_);
        if (!channel_id_.empty() && channel_id_ != id)
          return;
        channel_id_ = id;
        state_ = State::Registering;
        error_.clear();
        const bool originated = dialing_;
        dialing_ = false;
        if (!originated)
          callback = callbacks_.on_incoming_call;
      }
      if (callback)
        callback(number);
    } else if (type == "ChannelStateChange") {
      std::string_view channel_state;
      if (channel["state"].get(channel_state) == simdjson::SUCCESS &&
          channel_state == "Up") {
        std::function<void()> callback;
        {
          std::lock_guard lock(mutex_);
          if (id != channel_id_)
            return;
          state_ = State::InCall;
          callback = callbacks_.on_call_answered;
        }
        if (callback)
          callback();
      }
    } else if (type == "StasisEnd" || type == "ChannelDestroyed") {
      std::function<void()> callback;
      {
        std::lock_guard lock(mutex_);
        if (id == channel_id_) {
          channel_id_.clear();
          dialing_ = false;
          state_ = State::Registered;
          callback = callbacks_.on_call_ended;
        }
      }
      if (callback)
        callback();
    }
  }

  awaitable<void> ari_request(http::verb method, std::string target,
                              std::string body = {}) {
    try {
      auto executor = co_await boost::asio::this_coro::executor;
      tcp::resolver resolver(executor);
      const auto endpoints = co_await resolver.async_resolve(
          settings_.host, settings_.ari_port, use_awaitable);
      beast::tcp_stream stream(executor);
      co_await stream.async_connect(endpoints, use_awaitable);
      http::request<http::string_body> request(method, target, HTTP_VERSION);
      request.set(http::field::host, settings_.host);
      request.set(http::field::authorization, settings_.ari_authorization);
      request.set(http::field::user_agent, "DJIC-AsteriskClient/1");
      if (!body.empty()) {
        request.set(http::field::content_type, "application/json");
        request.body() = std::move(body);
        request.prepare_payload();
      }
      co_await http::async_write(stream, request, use_awaitable);
      beast::flat_buffer buffer;
      http::response_parser<http::string_body> parser;
      parser.body_limit(MAX_ARI_RESPONSE_LENGTH);
      co_await http::async_read(stream, buffer, parser, use_awaitable);
      const auto response = parser.release();
      if (response.result_int() < 200 || response.result_int() >= 300) {
        set_error("ARI request rejected with HTTP status " +
                  std::to_string(response.result_int()));
      }
      beast::error_code ignored;
      stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
    } catch (const std::exception &) {
      set_error("ARI request failed");
    }
  }

  awaitable<void> originate(std::string target) {
    const std::string path = "/ari/channels?endpoint=" + url_encode(target) +
                             "&app=" + settings_.application_target;
    co_await ari_request(http::verb::post, path);
    std::lock_guard lock(mutex_);
    if (!error_.empty())
      dialing_ = false;
  }

  awaitable<void> answer_channel(std::string channel) {
    co_await ari_request(http::verb::post,
                         "/ari/channels/" + url_encode(channel) + "/answer");
  }

  awaitable<void> hangup_channel(std::string channel) {
    co_await ari_request(http::verb::delete_,
                         "/ari/channels/" + url_encode(channel));
  }

  awaitable<void> ari_event_loop() {
    auto retry_delay = INITIAL_RETRY_DELAY;
    for (;;) {
      if (!started())
        co_return;
      bool retry = false;
      try {
        auto executor = co_await boost::asio::this_coro::executor;
        tcp::resolver resolver(executor);
        const auto endpoints = co_await resolver.async_resolve(
            settings_.host, settings_.ari_port, use_awaitable);
        websocket::stream<beast::tcp_stream> stream(executor);
        stream.read_message_max(MAX_ARI_EVENT_LENGTH);
        co_await stream.next_layer().async_connect(endpoints, use_awaitable);
        stream.set_option(websocket::stream_base::decorator([this](
                                                                auto &request) {
          request.set(http::field::authorization, settings_.ari_authorization);
        }));
        const std::string target =
            "/ari/events?app=" + settings_.application_target;
        co_await stream.async_handshake(settings_.host, target, use_awaitable);
        {
          std::lock_guard lock(mutex_);
          state_ = State::Registered;
          error_.clear();
        }
        retry_delay = INITIAL_RETRY_DELAY;
        while (started()) {
          beast::flat_buffer buffer;
          co_await stream.async_read(buffer, use_awaitable);
          simdjson::padded_string payload(buffer.size());
          boost::asio::buffer_copy(
              boost::asio::buffer(payload.data(), buffer.size()),
              buffer.data());
          handle_ari_event(payload);
        }
      } catch (const std::exception &) {
        set_error("ARI event stream unavailable");
        retry = true;
      }
      if (retry && started()) {
        co_await wait_before_retry(retry_delay);
        retry_delay = std::min(retry_delay * 2, MAX_RETRY_DELAY);
      }
    }
  }

  awaitable<void> wait_before_retry(std::chrono::seconds delay) {
    boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor,
                                    delay);
    boost::system::error_code error;
    co_await timer.async_wait(
        boost::asio::redirect_error(use_awaitable, error));
  }

  awaitable<void> ami_login(tcp::socket &socket, bool receive_events) {
    const std::string login =
        "Action: Login\r\nUsername: " + settings_.ami_username +
        "\r\nSecret: " + settings_.ami_password +
        "\r\nEvents: " + (receive_events ? "on" : "off") + "\r\n\r\n";
    co_await boost::asio::async_write(socket, boost::asio::buffer(login),
                                      use_awaitable);
    std::string response;
    co_await boost::asio::async_read_until(
        socket, boost::asio::dynamic_buffer(response, MAX_AMI_FRAME_LENGTH),
        AMI_DELIMITER, use_awaitable);
    const auto fields = parse_ami_message(response);
    if (fields.response != "Success")
      throw std::runtime_error("AMI login rejected");
  }

  awaitable<void> ami_event_loop() {
    auto retry_delay = INITIAL_RETRY_DELAY;
    for (;;) {
      if (!started())
        co_return;
      bool retry = false;
      try {
        auto executor = co_await boost::asio::this_coro::executor;
        tcp::resolver resolver(executor);
        const auto endpoints = co_await resolver.async_resolve(
            settings_.host, settings_.ami_port, use_awaitable);
        tcp::socket socket(executor);
        co_await boost::asio::async_connect(socket, endpoints, use_awaitable);
        co_await ami_login(socket, true);
        retry_delay = INITIAL_RETRY_DELAY;
        std::string data;
        while (started()) {
          co_await boost::asio::async_read_until(
              socket, boost::asio::dynamic_buffer(data, MAX_AMI_FRAME_LENGTH),
              AMI_DELIMITER, use_awaitable);
          const size_t end = data.find(AMI_DELIMITER);
          if (end == std::string::npos)
            continue;
          const auto fields = parse_ami_message(data.substr(0, end));
          data.erase(0, end + AMI_DELIMITER.size());
          if ((fields.event == "UserEvent" &&
               fields.user_event == "SipMessage") ||
              fields.event == "Message") {
            const std::string_view from =
                fields.from.empty() ? fields.caller_id : fields.from;
            const std::string_view body =
                fields.body.empty() ? fields.content : fields.body;
            std::function<void(const std::string &, const std::string &)>
                callback;
            {
              std::lock_guard lock(mutex_);
              callback = callbacks_.on_message;
            }
            if (callback)
              callback(std::string(from), std::string(body));
          }
        }
      } catch (const std::exception &) {
        set_error("AMI event stream unavailable");
        retry = true;
      }
      if (retry && started()) {
        co_await wait_before_retry(retry_delay);
        retry_delay = std::min(retry_delay * 2, MAX_RETRY_DELAY);
      }
    }
  }

  awaitable<void> send_ami_message() {
    try {
      auto executor = co_await boost::asio::this_coro::executor;
      tcp::resolver resolver(executor);
      const auto endpoints = co_await resolver.async_resolve(
          settings_.host, settings_.ami_port, use_awaitable);
      tcp::socket socket(executor);
      co_await boost::asio::async_connect(socket, endpoints, use_awaitable);
      co_await ami_login(socket, false);
      while (started() && !message_queue_.empty()) {
        MessageRequest request = std::move(message_queue_.front());
        message_queue_.pop_front();
        const std::string action = "Action: " + std::string(MESSAGE_ACTION) +
                                   "\r\nTo: pjsip:" + request.number +
                                   "\r\nFrom: " + settings_.message_from +
                                   "\r\nBody: " + request.message + "\r\n\r\n";
        co_await boost::asio::async_write(socket, boost::asio::buffer(action),
                                          use_awaitable);
        std::string response;
        co_await boost::asio::async_read_until(
            socket, boost::asio::dynamic_buffer(response, MAX_AMI_FRAME_LENGTH),
            AMI_DELIMITER, use_awaitable);
        const auto fields = parse_ami_message(response);
        if (fields.response != "Success")
          throw std::runtime_error("AMI MessageSend was rejected");
      }
    } catch (const std::exception &) {
      set_error("AMI message failed");
    }
    message_queue_.clear();
    message_sender_running_ = false;
  }

  bool started() const {
    std::lock_guard lock(mutex_);
    return started_;
  }

  static constexpr unsigned HTTP_VERSION = 11;

  mutable std::mutex mutex_;
  Settings settings_;
  Callbacks callbacks_;
  boost::asio::io_context io_;
  simdjson::dom::parser json_parser_;
  std::optional<
      boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>
      work_;
  std::thread worker_;
  bool started_ = false;
  State state_ = State::Idle;
  std::string channel_id_;
  bool dialing_ = false;
  std::deque<MessageRequest> message_queue_;
  bool message_sender_running_ = false;
  std::string error_;
};

} // namespace detail

class AsteriskStack final : public Client {
public:
  AsteriskStack() : client_(std::make_unique<detail::AsteriskClient>()) {}
  ~AsteriskStack() override = default;

  AsteriskStack(const AsteriskStack &) = delete;
  AsteriskStack &operator=(const AsteriskStack &) = delete;

  bool start(const Account &account, Callbacks callbacks) override {
    return client_->start(account, std::move(callbacks));
  }
  void stop() override { client_->stop(); }
  bool dial(const std::string &number) override {
    return client_->dial(number);
  }
  bool answer() override { return client_->answer(); }
  bool hangup() override { return client_->hangup(); }
  bool send_message(const std::string &number,
                    const std::string &message) override {
    return client_->send_message(number, message);
  }
  bool in_call() const override { return client_->in_call(); }
  State state() const override { return client_->state(); }
  std::string error() const override { return client_->error(); }

private:
  std::unique_ptr<detail::AsteriskClient> client_;
};

std::unique_ptr<Client> create_client() {
  return std::make_unique<AsteriskStack>();
}

} // namespace asterisk
