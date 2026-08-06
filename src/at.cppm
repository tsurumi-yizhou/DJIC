module;

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <libusb.h>
#include <memory>
#include <optional>
#include <span>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

export module usb;

namespace usb {

// 大疆第一代 4G 模块 USB 标识
export constexpr uint16_t kDjiVendorId = 0x2ca3;
export constexpr uint16_t kDjiProductId = 0x4006;

export class UsbError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// 统一的 AT 通道抽象：UsbAt 走 libusb 实机通道，未来可为测试实现模拟通道。
export class AtChannel {
public:
  virtual ~AtChannel() = default;
  virtual std::string command(const std::string &cmd, int timeout_ms) = 0;
  virtual std::string command_with_prompt(const std::string &cmd,
                                          const std::vector<uint8_t> &follow_up,
                                          int timeout_ms) = 0;
  virtual bool valid() const = 0;
  virtual std::string description() const = 0;
};

export inline std::string to_lower(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

// --- AT 响应解析辅助（与上游 cellular 行为一致） ---

export inline std::string normalize_at_response(const std::string &resp) {
  std::string r = resp;
  auto replace_all = [](std::string &s, const std::string &from,
                        const std::string &to) {
    if (from.empty())
      return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  };
  replace_all(r, "\r\r\n", "\r\n");

  // 去掉首尾空白
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  auto begin = r.begin();
  while (begin != r.end() && is_space(static_cast<unsigned char>(*begin)))
    ++begin;
  auto end = r.end();
  while (end != begin && is_space(static_cast<unsigned char>(*(end - 1))))
    --end;
  r.assign(begin, end);

  // 分行过滤空行（仅裁剪每行首尾空白，保留内部空格）
  std::vector<std::string> lines;
  auto trim_ws = [](std::string &s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    auto b = s.begin();
    while (b != s.end() && is_space(static_cast<unsigned char>(*b)))
      ++b;
    auto e = s.end();
    while (e != b && is_space(static_cast<unsigned char>(*(e - 1))))
      --e;
    s.assign(b, e);
  };
  size_t pos = 0;
  while (pos <= r.size()) {
    size_t nl = r.find('\n', pos);
    std::string line =
        (nl == std::string::npos) ? r.substr(pos) : r.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    trim_ws(line);
    if (!line.empty())
      lines.push_back(line);
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }

  std::string out;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (i)
      out += "\r\n";
    out += lines[i];
  }
  return out;
}

export inline std::vector<std::string> split_at_lines(const std::string &resp) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos <= resp.size()) {
    size_t nl = resp.find('\n', pos);
    std::string line = (nl == std::string::npos) ? resp.substr(pos)
                                                 : resp.substr(pos, nl - pos);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    auto begin = line.begin();
    while (begin != line.end() &&
           std::isspace(static_cast<unsigned char>(*begin)))
      ++begin;
    auto end = line.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))))
      --end;
    std::string trimmed(begin, end);
    if (!trimmed.empty())
      lines.push_back(trimmed);
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
  }
  return lines;
}

export inline std::string to_upper(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return out;
}

// --- 通用解析辅助（避免依赖 std::regex 在 libc++ 下的脆弱行为） ---

// 从 pos 起查找子串（忽略大小写），返回原始下标；未找到返回 npos。
export inline size_t find_ci(const std::string &hay, const std::string &needle,
                             size_t pos = 0) {
  std::string h = to_lower(hay.substr(pos));
  std::string n = to_lower(needle);
  auto idx = h.find(n);
  return idx == std::string::npos ? std::string::npos : pos + idx;
}

// 依次解析字符串中出现的所有非负整数。
export inline std::vector<int> parse_ints(const std::string &s) {
  std::vector<int> out;
  for (size_t i = 0; i < s.size();) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) {
      ++i;
      continue;
    }
    long v = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      v = v * 10 + (s[i] - '0');
      ++i;
    }
    out.push_back(static_cast<int>(v));
  }
  return out;
}

// 依次解析字符串中出现的双引号括起来的字段。
export inline std::vector<std::string> parse_quoted(const std::string &s) {
  std::vector<std::string> out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '"')
      continue;
    size_t j = s.find('"', i + 1);
    if (j == std::string::npos)
      break;
    out.push_back(s.substr(i + 1, j - i - 1));
    i = j;
  }
  return out;
}

export inline bool at_response_is_error(const std::string &resp) {
  std::string norm;
  norm.reserve(resp.size());
  for (size_t i = 0; i < resp.size(); ++i) {
    if (resp[i] == '\r' && i + 1 < resp.size() && resp[i + 1] == '\n') {
      norm.push_back('\n');
      ++i;
    } else {
      norm.push_back(
          static_cast<char>(std::toupper(static_cast<unsigned char>(resp[i]))));
    }
  }
  if (norm.find("\nERROR\n") != std::string::npos)
    return true;
  if (norm.size() >= 5 && norm.compare(norm.size() - 5, 5, "\nERROR") == 0)
    return true;
  if (norm.find("+CME ERROR:") != std::string::npos)
    return true;
  if (norm.find("+CMS ERROR:") != std::string::npos)
    return true;
  return false;
}

export inline bool at_response_complete(const std::string &resp) {
  std::string norm;
  for (size_t i = 0; i < resp.size(); ++i) {
    if (resp[i] == '\r' && i + 1 < resp.size() && resp[i + 1] == '\n') {
      norm.push_back('\n');
      ++i;
    } else {
      norm.push_back(resp[i]);
    }
  }
  if (norm.find("\nOK\n") != std::string::npos)
    return true;
  if (norm.size() >= 3 && norm.compare(norm.size() - 3, 3, "\nOK") == 0)
    return true;
  return at_response_is_error(norm);
}

export inline bool at_response_has_prompt(const std::string &resp) {
  size_t end = resp.size();
  while (end > 0) {
    char c = resp[end - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      --end;
    } else {
      break;
    }
  }
  return end > 0 && resp[end - 1] == '>';
}

// 探测必须收到字面 OK：仅收到 ERROR 只证明 bulk 接口能收字节（QMI
// 接口也能），不是 AT 通道。
export inline bool at_probe_succeeded(const std::string &resp) {
  std::string norm;
  for (size_t i = 0; i < resp.size(); ++i) {
    if (resp[i] == '\r' && i + 1 < resp.size() && resp[i + 1] == '\n') {
      norm.push_back('\n');
      ++i;
    } else {
      norm.push_back(resp[i]);
    }
  }
  auto trim = [](std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    auto b = s.begin();
    while (b != s.end() && is_space(static_cast<unsigned char>(*b)))
      ++b;
    auto e = s.end();
    while (e != b && is_space(static_cast<unsigned char>(*(e - 1))))
      --e;
    return std::string(b, e);
  };
  norm = trim(norm);
  if (norm == "OK")
    return true;
  if (norm.size() >= 3 && norm.compare(norm.size() - 3, 3, "\nOK") == 0)
    return true;
  return false;
}

// --- libusb USB AT 桥 ---

export class UsbAt final : public AtChannel {
public:
  UsbAt() = default;
  ~UsbAt() override { close(); }
  UsbAt(UsbAt &&other) noexcept { *this = std::move(other); }
  UsbAt &operator=(UsbAt &&other) noexcept {
    if (this != &other) {
      close();
      ctx_ = other.ctx_;
      handle_ = other.handle_;
      iface_ = other.iface_;
      ep_in_ = other.ep_in_;
      ep_out_ = other.ep_out_;
      other.ctx_ = nullptr;
      other.handle_ = nullptr;
    }
    return *this;
  }
  UsbAt(const UsbAt &) = delete;
  UsbAt &operator=(const UsbAt &) = delete;

  // 打开大疆模块 USB AT 通道：扫描所有带 bulk IN+OUT 的接口并逐一对 "AT" 探测。
  static std::optional<UsbAt> open();

  bool valid() const override { return handle_ != nullptr; }
  std::string description() const override;

  std::string command(const std::string &cmd, int timeout_ms) override;
  std::string command_with_prompt(const std::string &cmd,
                                  const std::vector<uint8_t> &follow_up,
                                  int timeout_ms) override;
  void close();

private:
  struct Candidate {
    int interface;
    uint8_t ep_in;
    uint8_t ep_out;
  };
  static std::vector<Candidate> collect_candidates(libusb_device *dev);

  void drain_locked();
  void bulk_write(uint8_t ep, std::span<const uint8_t> data, int timeout_ms);
  std::vector<uint8_t> bulk_read(uint8_t ep, int timeout_ms, bool &timed_out);

  libusb_context *ctx_ = nullptr;
  libusb_device_handle *handle_ = nullptr;
  int iface_ = -1;
  uint8_t ep_in_ = 0;
  uint8_t ep_out_ = 0;
};

std::vector<UsbAt::Candidate> UsbAt::collect_candidates(libusb_device *dev) {
  std::vector<Candidate> out;
  libusb_config_descriptor *config = nullptr;
  int rc = libusb_get_active_config_descriptor(dev, &config);
  if (rc != 0) {
    throw UsbError("get active USB config descriptor: " +
                   std::string(libusb_error_name(rc)));
  }
  auto free_config = [](libusb_config_descriptor *c) {
    libusb_free_config_descriptor(c);
  };
  std::unique_ptr<libusb_config_descriptor, decltype(free_config)> guard(
      config, free_config);

  for (int i = 0; i < static_cast<int>(config->bNumInterfaces); ++i) {
    const libusb_interface *intf = &config->interface[i];
    for (int j = 0; j < static_cast<int>(intf->num_altsetting); ++j) {
      const libusb_interface_descriptor *alt = &intf->altsetting[j];
      uint8_t ep_in = 0;
      uint8_t ep_out = 0;
      for (int k = 0; k < static_cast<int>(alt->bNumEndpoints); ++k) {
        const libusb_endpoint_descriptor *ep = &alt->endpoint[k];
        uint8_t attrs = ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
        if (attrs != LIBUSB_TRANSFER_TYPE_BULK)
          continue;
        if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) {
          ep_in = ep->bEndpointAddress;
        } else {
          ep_out = ep->bEndpointAddress;
        }
      }
      if (ep_in != 0 && ep_out != 0) {
        out.push_back(
            Candidate{static_cast<int>(alt->bInterfaceNumber), ep_in, ep_out});
      }
    }
  }
  return out;
}

std::optional<UsbAt> UsbAt::open() {
  libusb_context *ctx = nullptr;
  int rc = libusb_init(&ctx);
  if (rc != 0) {
    throw UsbError("libusb init: " + std::string(libusb_error_name(rc)));
  }
  auto release_ctx = [&]() {
    if (ctx) {
      libusb_exit(ctx);
      ctx = nullptr;
    }
  };

  libusb_device_handle *handle =
      libusb_open_device_with_vid_pid(ctx, kDjiVendorId, kDjiProductId);
  if (handle == nullptr) {
    release_ctx();
    return std::nullopt;
  }
  auto close_handle = [&]() {
    if (handle) {
      libusb_close(handle);
      handle = nullptr;
    }
  };

  std::vector<Candidate> candidates;
  try {
    candidates = collect_candidates(libusb_get_device(handle));
  } catch (const UsbError &) {
    close_handle();
    release_ctx();
    throw;
  }

  std::string last_error;
  for (const auto &cand : candidates) {
    if (libusb_claim_interface(handle, cand.interface) != 0) {
      last_error =
          "claim interface " + std::to_string(cand.interface) + " failed";
      continue;
    }
    UsbAt dev;
    dev.ctx_ = ctx;
    dev.handle_ = handle;
    dev.iface_ = cand.interface;
    dev.ep_in_ = cand.ep_in;
    dev.ep_out_ = cand.ep_out;

    try {
      std::string resp = dev.command("AT", 900);
      if (at_probe_succeeded(resp)) {
        return dev;
      }
      last_error = "unexpected AT probe response '" + resp + "'";
    } catch (const UsbError &e) {
      last_error = e.what();
    }
    libusb_release_interface(handle, cand.interface);
  }

  close_handle();
  release_ctx();
  if (!last_error.empty()) {
    throw UsbError("no usable DJI AT channel (2ca3:4006): " + last_error);
  }
  return std::nullopt;
}

std::string UsbAt::description() const {
  if (handle_ == nullptr) {
    return "USB AT";
  }
  std::array<char, 128> buf{};
  std::snprintf(buf.data(), buf.size(),
                "USB AT · 2ca3:4006 interface %d out 0x%02x in 0x%02x", iface_,
                ep_out_, ep_in_);
  return std::string(buf.data());
}

void UsbAt::bulk_write(uint8_t ep, std::span<const uint8_t> data,
                       int timeout_ms) {
  int transferred = 0;
  // libusb C ABI 未正确标注 const；仅在适配边界移除 const，库不会修改写缓冲区。
  int rc = libusb_bulk_transfer(handle_, ep, const_cast<uint8_t *>(data.data()),
                                static_cast<int>(data.size()), &transferred,
                                timeout_ms);
  if (rc != 0) {
    throw UsbError("USB bulk write: " + std::string(libusb_error_name(rc)));
  }
  if (static_cast<size_t>(transferred) != data.size()) {
    throw UsbError(
        "USB bulk write short transfer: " + std::to_string(transferred) + "/" +
        std::to_string(data.size()));
  }
}

std::vector<uint8_t> UsbAt::bulk_read(uint8_t ep, int timeout_ms,
                                      bool &timed_out) {
  timed_out = false;
  std::vector<uint8_t> buf(512);
  int transferred = 0;
  int rc = libusb_bulk_transfer(handle_, ep, buf.data(),
                                static_cast<int>(buf.size()), &transferred,
                                timeout_ms);
  if (rc == LIBUSB_ERROR_TIMEOUT) {
    timed_out = true;
    return {};
  }
  if (rc != 0) {
    throw UsbError("USB bulk read: " + std::string(libusb_error_name(rc)));
  }
  buf.resize(static_cast<size_t>(transferred));
  return buf;
}

void UsbAt::drain_locked() {
  for (;;) {
    bool timed_out = false;
    try {
      bulk_read(ep_in_, 80, timed_out);
    } catch (const UsbError &) {
      return;
    }
    if (timed_out)
      return;
  }
}

std::string UsbAt::command(const std::string &cmd, int timeout_ms) {
  if (handle_ == nullptr) {
    throw UsbError("USB AT device is not open");
  }
  if (timeout_ms <= 0)
    timeout_ms = 3000;

  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::milliseconds(timeout_ms);

  drain_locked();
  std::vector<uint8_t> payload(cmd.begin(), cmd.end());
  payload.push_back('\r');
  bulk_write(ep_out_, payload, timeout_ms);

  std::string chunks;
  for (;;) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      break;
    int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    if (remaining > 900)
      remaining = 900;

    bool timed_out = false;
    auto data = bulk_read(ep_in_, remaining, timed_out);
    if (timed_out)
      continue;
    if (data.empty())
      continue;
    chunks.append(data.begin(), data.end());
    if (at_response_complete(chunks)) {
      return normalize_at_response(chunks);
    }
  }
  if (chunks.empty()) {
    throw UsbError("USB AT command timed out without response");
  }
  return normalize_at_response(chunks);
}

std::string UsbAt::command_with_prompt(const std::string &cmd,
                                       const std::vector<uint8_t> &follow_up,
                                       int timeout_ms) {
  if (handle_ == nullptr) {
    throw UsbError("USB AT device is not open");
  }
  if (timeout_ms <= 0)
    timeout_ms = 30000;

  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::milliseconds(timeout_ms);

  drain_locked();
  std::vector<uint8_t> payload(cmd.begin(), cmd.end());
  payload.push_back('\r');
  bulk_write(ep_out_, payload, timeout_ms);

  std::string response;
  bool prompt_received = false;
  for (;;) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      break;
    int remaining = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count());
    if (remaining > 900)
      remaining = 900;

    bool timed_out = false;
    auto data = bulk_read(ep_in_, remaining, timed_out);
    if (timed_out)
      continue;
    if (data.empty())
      continue;
    response.append(data.begin(), data.end());

    if (!prompt_received) {
      if (at_response_is_error(response)) {
        return normalize_at_response(response);
      }
      if (!at_response_has_prompt(response)) {
        continue;
      }
      bulk_write(ep_out_, follow_up,
                 static_cast<int>(
                     std::chrono::duration_cast<std::chrono::milliseconds>(
                         deadline - std::chrono::steady_clock::now())
                         .count()));
      prompt_received = true;
      continue;
    }

    if (at_response_complete(response)) {
      return normalize_at_response(response);
    }
  }

  if (prompt_received) {
    // ESC 取消仍在等待输入的编辑器（某些固件仍接受输入）
    try {
      const std::array<uint8_t, 1> esc{0x1b};
      bulk_write(ep_out_, esc, 300);
    } catch (const UsbError &) {
    }
  }
  if (response.empty()) {
    throw UsbError("USB interactive AT command timed out without response");
  }
  throw UsbError("USB interactive AT command timed out before completion");
}

void UsbAt::close() {
  if (handle_ != nullptr) {
    if (iface_ >= 0) {
      libusb_release_interface(handle_, iface_);
    }
    libusb_close(handle_);
    handle_ = nullptr;
  }
  if (ctx_ != nullptr) {
    libusb_exit(ctx_);
    ctx_ = nullptr;
  }
  iface_ = -1;
  ep_in_ = 0;
  ep_out_ = 0;
}

} // namespace     usb
