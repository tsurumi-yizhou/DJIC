module;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

export module sms;

import usb;
import smscodec;

namespace sms {

export struct Sms {
  std::string sender;
  std::string content;
  std::string timestamp;
  std::string code;
};

// 从短信内容中提取验证码（须紧邻 OTP 关键词，避免把普通号码当验证码）
export inline std::string extract_sms_code(const std::string &content) {
  static const std::vector<std::string> keywords = {
      "验证码",
      "校验码",
      "动态码",
      "验证代码",
      "verification code",
      "security code",
      "one-time password",
      "one time password",
      "otp",
      "passcode",
      "login code",
      "code",
  };
  // 折叠空白并小写，便于匹配带空格/换行的英文关键词
  auto normalize = [](const std::string &s) {
    std::string out;
    bool prev_space = false;
    for (char c : s) {
      if (std::isspace(static_cast<unsigned char>(c))) {
        if (!prev_space && !out.empty())
          out.push_back(' ');
        prev_space = true;
        continue;
      }
      prev_space = false;
      out.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
  };
  std::string norm = normalize(content);
  auto is_digit = [&](size_t i) {
    return i < norm.size() && norm[i] >= '0' && norm[i] <= '9';
  };

  // 模式 1：关键词 -> 至多 12 个非数字 -> 4-8 位数字
  for (const auto &kw : keywords) {
    size_t p = 0;
    while ((p = norm.find(kw, p)) != std::string::npos) {
      size_t q = p + kw.size();
      size_t skip = 0;
      while (q < norm.size() && !is_digit(q) && skip < 12) {
        ++q;
        ++skip;
      }
      if (q < norm.size() && is_digit(q)) {
        size_t start = q;
        while (q < norm.size() && is_digit(q))
          ++q;
        size_t len = q - start;
        if (len >= 4 && len <= 8)
          return norm.substr(start, len);
      }
      p += kw.size();
    }
  }

  // 模式 2：4-8 位数字 -> 至多 12 个非数字 -> 关键词
  for (size_t i = 0; i < norm.size();) {
    if (!is_digit(i)) {
      ++i;
      continue;
    }
    size_t start = i;
    while (i < norm.size() && is_digit(i))
      ++i;
    size_t len = i - start;
    if (len >= 4 && len <= 8) {
      size_t q = i;
      size_t skip = 0;
      while (q < norm.size() && !is_digit(q) && skip <= 12) {
        for (const auto &kw : keywords) {
          if (norm.compare(q, kw.size(), kw) == 0)
            return norm.substr(start, len);
        }
        ++q;
        ++skip;
      }
    }
  }
  return "";
}

export struct SendResult {
  int segments = 0;
  std::string error;
  bool ok = false;
};

// 长短信重组器（按 sender + ref 缓存分片，齐全后拼接）
export class Reassembler {
public:
  bool add(const std::string &sender, const smscodec::ConcatInfo &concat,
           const std::string &content, std::string &full) {
    if (!concat.is_concat) {
      full = content;
      return true;
    }
    std::lock_guard<std::mutex> lk(mu_);
    std::string key = sender + "_" + std::to_string(concat.ref);
    for (const auto &f : cache_[key]) {
      if (f.seq == concat.seq) {
        return false;
      }
    }
    cache_[key].push_back(Fragment{concat.seq, concat.total, content,
                                   std::chrono::steady_clock::now()});
    auto &frags = cache_[key];
    if (static_cast<int>(frags.size()) != concat.total) {
      return false;
    }
    std::sort(
        frags.begin(), frags.end(),
        [](const Fragment &a, const Fragment &b) { return a.seq < b.seq; });
    std::string joined;
    for (const auto &f : frags) {
      joined += f.content;
    }
    cache_.erase(key);
    full = joined;
    return true;
  }

  void cleanup(std::chrono::seconds ttl) {
    std::lock_guard<std::mutex> lk(mu_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache_.begin(); it != cache_.end();) {
      auto latest = std::chrono::steady_clock::time_point{};
      for (const auto &f : it->second) {
        latest = std::max(latest, f.time);
      }
      if (now - latest > ttl) {
        it = cache_.erase(it);
      } else {
        ++it;
      }
    }
  }

private:
  struct Fragment {
    int seq;
    int total;
    std::string content;
    std::chrono::steady_clock::time_point time;
  };
  std::mutex mu_;
  std::map<std::string, std::vector<Fragment>> cache_;
};

export class SmsService {
public:
  explicit SmsService(usb::AtChannel &at) : at_(at) {}

  // 发送短信（PDU 模式）。返回分段数，失败抛出 UsbError。
  int send(const std::string &phone, const std::string &message) {
    auto resp = at_.command("AT+CMGF=0", 5000);
    if (!usb::at_probe_succeeded(resp)) {
      throw usb::UsbError("set SMS PDU mode failed: " + resp);
    }
    bool force_ucs2 = false;
    for (unsigned char c : message) {
      if (c > 127) {
        force_ucs2 = true;
        break;
      }
    }
    auto segments = smscodec::build_submit_tpdus(phone, message, force_ucs2);
    if (segments.empty()) {
      throw usb::UsbError("build SMS PDU failed");
    }
    for (size_t i = 0; i < segments.size(); ++i) {
      std::vector<uint8_t> pdu;
      pdu.push_back(0x00); // SMSC 长度 0 = 使用默认短信中心
      pdu.insert(pdu.end(), segments[i].tpdu.begin(), segments[i].tpdu.end());

      std::vector<uint8_t> payload;
      std::string hex = smscodec::to_hex(pdu);
      payload.insert(payload.end(), hex.begin(), hex.end());
      payload.push_back(0x1A); // Ctrl-Z 提交 PDU

      std::string send_resp;
      try {
        send_resp = at_.command_with_prompt(
            "AT+CMGS=" + std::to_string(segments[i].length), payload, 45000);
      } catch (const usb::UsbError &e) {
        throw usb::UsbError("send SMS segment " + std::to_string(i + 1) + "/" +
                            std::to_string(segments.size()) + ": " + e.what());
      }
      if (usb::at_response_is_error(send_resp) ||
          send_resp.find("+CMGS:") == std::string::npos ||
          !usb::at_probe_succeeded(send_resp)) {
        throw usb::UsbError("send SMS segment " + std::to_string(i + 1) + "/" +
                            std::to_string(segments.size()) +
                            " failed: " + send_resp);
      }
      if (i + 1 < segments.size()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }
    }
    return static_cast<int>(segments.size());
  }

  // 轮询读取 SM + ME 存储中的短信，去重并重组长短信。
  std::vector<Sms> poll() {
    at_.command("AT+CMGF=0", 3000);
    std::vector<Sms> messages;
    std::map<std::string, bool> seen;
    std::vector<std::string> errors;
    for (const auto &memory : {std::string("SM"), std::string("ME")}) {
      try {
        auto items = poll_memory(memory);
        for (auto &item : items) {
          std::string key =
              item.sender + "\x00" + item.content + "\x00" + item.timestamp;
          if (seen[key])
            continue;
          seen[key] = true;
          messages.push_back(item);
        }
      } catch (const usb::UsbError &e) {
        errors.push_back(memory + ": " + e.what());
      }
    }
    std::sort(messages.begin(), messages.end(), [](const Sms &a, const Sms &b) {
      return a.timestamp > b.timestamp;
    });
    if (messages.empty() && errors.size() == 2) {
      throw usb::UsbError("list SMS failed: " + errors[0] + "; " + errors[1]);
    }
    return messages;
  }

  // 清空指定存储（"ME"）中的短信，返回 [before, after]。
  std::pair<int, int> clear_memory(const std::string &memory) {
    auto resp = at_.command("AT+CPMS=\"" + memory + "\",\"" + memory + "\",\"" +
                                memory + "\"",
                            5000);
    int before = parse_cpms_used(resp);
    at_.command("AT+CMGD=1,4", 20000);
    resp = at_.command("AT+CPMS=\"" + memory + "\",\"" + memory + "\",\"" +
                           memory + "\"",
                       5000);
    int after = parse_cpms_used(resp);
    return {before, after};
  }

  std::vector<Sms> cached() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cache_;
  }

  void merge(const std::vector<Sms> &messages) {
    std::lock_guard<std::mutex> lk(mu_);
    std::map<std::string, bool> seen;
    for (const auto &item : cache_) {
      seen[item.sender + "\x00" + item.content + "\x00" + item.timestamp] =
          true;
    }
    for (auto item : messages) {
      if (item.code.empty())
        item.code = extract_sms_code(item.content);
      std::string key =
          item.sender + "\x00" + item.content + "\x00" + item.timestamp;
      if (seen[key])
        continue;
      seen[key] = true;
      cache_.push_back(item);
    }
    std::sort(cache_.begin(), cache_.end(), [](const Sms &a, const Sms &b) {
      return a.timestamp > b.timestamp;
    });
    if (cache_.size() > 500)
      cache_.resize(500);
  }

  Reassembler &reassembler() { return reassembler_; }

private:
  static std::string trim_ws(const std::string &s) {
    auto b = s.begin();
    while (b != s.end() && std::isspace(static_cast<unsigned char>(*b)))
      ++b;
    auto e = s.end();
    while (e != b && std::isspace(static_cast<unsigned char>(*(e - 1))))
      --e;
    return std::string(b, e);
  }

  std::vector<Sms> poll_memory(const std::string &memory) {
    at_.command("AT+CPMS=\"" + memory + "\",\"" + memory + "\",\"" + memory +
                    "\"",
                5000);
    auto resp = at_.command("AT+CMGL=4", 15000);
    std::vector<Sms> out;
    auto lines = usb::split_at_lines(resp);
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
      if (lines[i].rfind("+CMGL:", 0) != 0)
        continue;
      std::string pdu_hex = trim_ws(lines[i + 1]);
      if (!is_hex(pdu_hex))
        continue;
      auto decoded = decode_pdu(pdu_hex);
      if (!decoded.ok) {
        out.push_back(Sms{"PDU",
                          "[短信解析失败] " + decoded.error + "\n" + pdu_hex,
                          now_str(), ""});
        ++i;
        continue;
      }
      if (decoded.concat.is_concat) {
        std::string full;
        bool complete = reassembler_.add(decoded.sender, decoded.concat,
                                         decoded.content, full);
        if (!complete) {
          ++i;
          continue;
        }
        decoded.content = full;
      }
      out.push_back(Sms{decoded.sender, decoded.content, decoded.timestamp,
                        extract_sms_code(decoded.content)});
      ++i;
    }
    reassembler_.cleanup(std::chrono::minutes(10));
    return out;
  }

  smscodec::Deliver decode_pdu(const std::string &pdu_hex) {
    auto full = smscodec::from_hex(pdu_hex);
    if (full.size() < 2) {
      smscodec::Deliver d;
      d.error = "PDU too short";
      return d;
    }
    int smsc_len = full[0];
    size_t offset = 1 + static_cast<size_t>(smsc_len);
    if (offset >= full.size()) {
      smscodec::Deliver d;
      d.error = "invalid SMSC length";
      return d;
    }
    auto d = smscodec::decode_deliver_tpdu(std::vector<uint8_t>(
        full.begin() + static_cast<long>(offset), full.end()));
    if (d.timestamp.empty())
      d.timestamp = now_str();
    return d;
  }

  static int parse_cpms_used(const std::string &resp) {
    size_t pos = usb::find_ci(resp, "+CPMS:");
    if (pos == std::string::npos)
      return 0;
    auto ints = usb::parse_ints(resp.substr(pos));
    if (ints.empty())
      return 0;
    return ints[0];
  }

  static bool is_hex(const std::string &s) {
    if (s.size() < 2 || s.size() % 2 != 0)
      return false;
    for (char c : s) {
      if (!std::isxdigit(static_cast<unsigned char>(c)))
        return false;
    }
    return true;
  }

  static std::string now_str() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
  }

  usb::AtChannel &at_;
  Reassembler reassembler_;
  mutable std::mutex mu_;
  std::vector<Sms> cache_;
};

} // namespace     sms
