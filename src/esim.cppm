module;

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

export module esim;

import usb;
import smscodec;

namespace esim {

export struct ApduResponse {
  std::vector<uint8_t> data;
  uint16_t sw = 0;
  bool ok = false;
};

export struct Profile {
  std::string iccid;
  std::string isdp_aid;
  std::string state;
  std::string nickname;
  std::string provider;
  std::string name;
};

export inline std::string bytes_to_hex(const std::vector<uint8_t> &data) {
  return smscodec::to_hex(data);
}

export inline std::vector<uint8_t> hex_to_bytes(const std::string &hex) {
  return smscodec::from_hex(hex);
}

// 支持的 ISD-R AID 列表（与上游一致）
export inline std::vector<std::string> known_isd_r_aids() {
  return {
      "A06573746B6D65FFFF4953442D522030",   // eSTK.me SE0
      "A06573746B6D65FFFF4953442D522031",   // eSTK.me SE1
      "A0000005591010FFFFFFFF8900000100",   // 标准 GSMA ISD-R
      "A000000559101000000000008900000300", // eSIM.me
      "A0000005591010000000890000000300",   // eSIM.me V2
      "A0000005591010FFFFFFFF8900050500",   // 5ber.eSIM
      "A0000005591010FFFFFFFF8900000177",   // XeSIM
      "A000000559104C696E6B736669656C64",   // LinksField
      "A0000006281010FFFFFFFF8900000100",   // GlocalMe
      "A06573746B6D65FFFFFFFFFF4953442D52", // eSTK.me AUX
  };
}

// --- 最小 BER-TLV 编解码（SGP.22 用） ---

struct Tlv {
  std::vector<uint8_t> tag;
  std::vector<uint8_t> value;
  std::vector<Tlv> children;
};

inline std::vector<uint8_t> encode_tlv(const std::vector<uint8_t> &tag,
                                       const std::vector<uint8_t> &value) {
  std::vector<uint8_t> out = tag;
  if (value.size() < 128) {
    out.push_back(static_cast<uint8_t>(value.size()));
  } else if (value.size() < 256) {
    out.push_back(0x81);
    out.push_back(static_cast<uint8_t>(value.size()));
  } else {
    out.push_back(0x82);
    out.push_back(static_cast<uint8_t>(value.size() >> 8));
    out.push_back(static_cast<uint8_t>(value.size() & 0xFF));
  }
  out.insert(out.end(), value.begin(), value.end());
  return out;
}

inline std::vector<Tlv> parse_tlv(std::span<const uint8_t> data,
                                  size_t &consumed) {
  std::vector<Tlv> out;
  size_t pos = 0;
  while (pos < data.size()) {
    Tlv t;
    uint8_t first = data[pos++];
    uint8_t class_form = first & 0xC0;
    uint8_t form = first & 0x20;
    uint8_t tag_num = first & 0x1F;
    t.tag.push_back(first);
    if (tag_num == 0x1F) {
      if (pos >= data.size())
        break;
      t.tag.push_back(data[pos++]);
    }
    (void)class_form;
    (void)form;
    size_t length = 0;
    if (pos >= data.size())
      break;
    uint8_t len_byte = data[pos++];
    if (len_byte < 0x80) {
      length = len_byte;
    } else {
      int num = len_byte & 0x7F;
      for (int i = 0; i < num && pos < data.size(); ++i) {
        length = (length << 8) | data[pos++];
      }
    }
    if (pos + length > data.size()) {
      length = data.size() - pos;
    }
    t.value.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
                   data.begin() + static_cast<std::ptrdiff_t>(pos + length));
    pos += length;
    out.push_back(std::move(t));
  }
  consumed = pos;
  return out;
}

inline std::optional<std::reference_wrapper<const Tlv>>
find_child(const Tlv &parent, const std::vector<uint8_t> &tag) {
  for (const auto &c : parent.children) {
    if (c.tag == tag)
      return std::cref(c);
  }
  return std::nullopt;
}

inline std::vector<uint8_t> context_constructed(int value) {
  if (value < 31) {
    return {static_cast<uint8_t>(0xA0 | value)};
  }
  return {0xBF, static_cast<uint8_t>(value)};
}

// 全局常量 tag
inline const std::vector<uint8_t> &tag_app_iccid() {
  static const std::vector<uint8_t> t{0x5A};
  return t;
}
inline const std::vector<uint8_t> &tag_app_isdpaid() {
  static const std::vector<uint8_t> t{0x4F};
  return t;
}
inline const std::vector<uint8_t> &tag_app_requestdata() {
  static const std::vector<uint8_t> t{0x5C};
  return t;
}
inline const std::vector<uint8_t> &tag_profile_state() {
  static const std::vector<uint8_t> t{0x81, 0x70};
  return t;
}
inline const std::vector<uint8_t> &tag_nickname() {
  static const std::vector<uint8_t> t{0x90};
  return t;
}
inline const std::vector<uint8_t> &tag_spn() {
  static const std::vector<uint8_t> t{0x91};
  return t;
}
inline const std::vector<uint8_t> &tag_profile_name() {
  static const std::vector<uint8_t> t{0x92};
  return t;
}
inline const std::vector<uint8_t> &tag_result() {
  static const std::vector<uint8_t> t{0x80};
  return t;
}
inline const std::vector<uint8_t> &tag_profile_root_e3() {
  static const std::vector<uint8_t> t{0xE3};
  return t;
}
inline const std::vector<uint8_t> &tag_profile_root_bf25() {
  static const std::vector<uint8_t> t{0xBF, 0x25};
  return t;
}

// ICCID <-> BCD（10 字节）
inline std::string iccid_from_bcd(const std::vector<uint8_t> &bcd) {
  std::string out;
  for (uint8_t b : bcd) {
    out.push_back(static_cast<char>('0' + (b >> 4)));
    out.push_back(static_cast<char>('0' + (b & 0x0F)));
  }
  return out;
}

inline std::vector<uint8_t> iccid_to_bcd(const std::string &iccid) {
  std::string digits;
  for (char c : iccid) {
    if (c >= '0' && c <= '9')
      digits.push_back(c);
  }
  while (digits.size() < 20)
    digits.push_back('0');
  digits.resize(20);
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < digits.size(); i += 2) {
    out.push_back(
        static_cast<uint8_t>(((digits[i] - '0') << 4) | (digits[i + 1] - '0')));
  }
  return out;
}

export class EsimClient {
public:
  explicit EsimClient(usb::AtChannel &at) : at_(at) {}

  // 打开 eUICC 逻辑通道（AT+CCHO）
  int open_channel(const std::string &aid_hex) {
    auto resp = at_.command("AT+CCHO=\"" + aid_hex + "\"", 8000);
    size_t pos = usb::find_ci(resp, "+CCHO:");
    if (pos == std::string::npos) {
      throw usb::UsbError("parse CCHO response failed: " + resp);
    }
    auto ints = usb::parse_ints(resp.substr(pos));
    if (ints.empty()) {
      throw usb::UsbError("parse CCHO response failed: " + resp);
    }
    return ints[0];
  }

  // 透传 APDU（AT+CGLA），返回完整响应（含 SW）
  ApduResponse transmit_apdu(int channel, const std::vector<uint8_t> &apdu) {
    if (channel == 0) {
      throw usb::UsbError("eUICC logical channel not open");
    }
    std::string hex = bytes_to_hex(apdu);
    std::string resp =
        at_.command("AT+CGLA=" + std::to_string(channel) + "," +
                        std::to_string(hex.size()) + ",\"" + hex + "\"",
                    15000);
    size_t pos = usb::find_ci(resp, "+CGLA:");
    if (pos == std::string::npos) {
      throw usb::UsbError("parse CGLA response failed: " + resp);
    }
    auto rest = resp.substr(pos);
    auto quoted = usb::parse_quoted(rest);
    std::string hex_out;
    if (!quoted.empty()) {
      hex_out = quoted[0];
    } else {
      // 未加引号的十六进制：取 "+CGLA:" 之后第一个连续的 hex 串
      size_t start = rest.find_first_of("0123456789abcdefABCDEF");
      if (start == std::string::npos) {
        throw usb::UsbError("parse CGLA response failed: " + resp);
      }
      size_t end = start;
      while (end < rest.size() &&
             std::isxdigit(static_cast<unsigned char>(rest[end]))) {
        ++end;
      }
      hex_out = rest.substr(start, end - start);
    }
    auto out = hex_to_bytes(hex_out);
    ApduResponse r;
    if (out.size() >= 2) {
      r.sw = static_cast<uint16_t>((out[out.size() - 2] << 8) |
                                   out[out.size() - 1]);
      r.data.assign(out.begin(), out.end() - 2);
      r.ok = (r.sw == 0x9000);
    } else {
      r.ok = false;
    }
    return r;
  }

  void close_channel(int channel) {
    at_.command("AT+CCHC=" + std::to_string(channel), 8000);
  }

  // 扫描已知 ISD-R AID，返回成功打开的 AID 列表
  std::vector<std::string> scan_aids() {
    std::vector<std::string> found;
    for (const auto &aid : known_isd_r_aids()) {
      try {
        int ch = open_channel(aid);
        auto eid = invoke_get_eid(ch);
        close_channel(ch);
        if (eid.has_value()) {
          found.push_back(aid);
        }
      } catch (const usb::UsbError &) {
      }
    }
    return found;
  }

  std::optional<std::string> get_eid() {
    for (const auto &aid : known_isd_r_aids()) {
      try {
        int ch = open_channel(aid);
        auto eid = invoke_get_eid(ch);
        close_channel(ch);
        if (eid.has_value()) {
          spdlog::info("eUICC detected on AID {}", aid);
          return eid;
        }
      } catch (const usb::UsbError &e) {
        spdlog::debug("AID {} not usable: {}", aid, e.what());
      }
    }
    return std::nullopt;
  }

  std::vector<Profile> list_profiles(const std::string &aid_hex) {
    int ch = open_channel(aid_hex);
    try {
      // GetProfilesInfo 请求：BF 2D <len> 5C <taglist>
      std::vector<uint8_t> tags_value;
      for (const auto &t :
           {tag_app_iccid(), tag_app_isdpaid(), tag_profile_state(),
            tag_nickname(), tag_spn(), tag_profile_name()}) {
        auto empty = encode_tlv(t, {});
        tags_value.insert(tags_value.end(), empty.begin(), empty.end());
      }
      auto reqdata = encode_tlv(tag_app_requestdata(), tags_value);
      auto request = encode_tlv(context_constructed(45), reqdata);
      auto resp = invoke_sgp22(ch, request);
      auto profiles = parse_profiles(resp);
      close_channel(ch);
      return profiles;
    } catch (...) {
      try {
        close_channel(ch);
      } catch (...) {
      }
      throw;
    }
  }

  void enable_profile(const std::string &aid_hex, const std::string &iccid,
                      bool refresh = true) {
    profile_operation(aid_hex, iccid, refresh, 49);
  }

  void disable_profile(const std::string &aid_hex, const std::string &iccid,
                       bool refresh = true) {
    profile_operation(aid_hex, iccid, refresh, 50);
  }

  void delete_profile(const std::string &aid_hex, const std::string &iccid) {
    profile_operation(aid_hex, iccid, false, 51);
  }

private:
  std::optional<std::string> invoke_get_eid(int channel) {
    // GetEID 请求：BF 3E 04 5C 02 5A 00
    auto child =
        encode_tlv(tag_app_requestdata(), encode_tlv(tag_app_iccid(), {}));
    auto request = encode_tlv(context_constructed(62), child);
    auto resp = invoke_sgp22(channel, request);
    size_t consumed = 0;
    auto tlvs = parse_tlv(resp, consumed);
    if (tlvs.empty())
      return std::nullopt;
    const auto eid = find_child(tlvs[0], tag_app_iccid());
    if (!eid.has_value())
      return std::nullopt;
    const Tlv &eid_value = eid->get();
    std::string hex = bytes_to_hex(eid_value.value);
    if (hex.size() != 32)
      return std::nullopt;
    return hex;
  }

  // SGP.22 分块 APDU 调用（INS 0xE2 + GET RESPONSE 0xC0）
  std::vector<uint8_t> invoke_sgp22(int channel,
                                    const std::vector<uint8_t> &request) {
    constexpr size_t kMss = 220;
    std::vector<uint8_t> response;
    size_t chunks = (request.size() + kMss - 1) / kMss;
    if (chunks == 0)
      chunks = 1;
    for (size_t idx = 0; idx < chunks; ++idx) {
      size_t begin = idx * kMss;
      size_t end = std::min(begin + kMss, request.size());
      std::vector<uint8_t> chunk(request.begin() + static_cast<long>(begin),
                                 request.begin() + static_cast<long>(end));
      std::vector<uint8_t> apdu;
      uint8_t cla = static_cast<uint8_t>(0x80 | (channel & 0x0F));
      apdu.push_back(cla);
      apdu.push_back(0xE2);
      apdu.push_back((idx == chunks - 1) ? 0x91 : 0x11);
      apdu.push_back(static_cast<uint8_t>(idx));
      apdu.push_back(static_cast<uint8_t>(chunk.size()));
      apdu.insert(apdu.end(), chunk.begin(), chunk.end());

      auto r = transmit_apdu(channel, apdu);
      if (r.sw == 0x61) {
        // GET RESPONSE 拉取剩余数据
        uint8_t le = static_cast<uint8_t>(r.sw & 0xFF);
        for (;;) {
          std::vector<uint8_t> gr{static_cast<uint8_t>(0x80 | (channel & 0x0F)),
                                  0xC0, 0x00, 0x00, le};
          auto g = transmit_apdu(channel, gr);
          response.insert(response.end(), g.data.begin(), g.data.end());
          if (g.sw == 0x61) {
            le = static_cast<uint8_t>(g.sw & 0xFF);
            continue;
          }
          if (g.sw != 0x9000) {
            throw usb::UsbError(
                "SGP.22 GET RESPONSE failed, SW=" +
                bytes_to_hex({static_cast<uint8_t>(g.sw >> 8),
                              static_cast<uint8_t>(g.sw & 0xFF)}));
          }
          break;
        }
      } else {
        response.insert(response.end(), r.data.begin(), r.data.end());
      }
    }
    // 最后一块之后一般还有 E2 应答的 9000；无需再校验。
    return response;
  }

  void profile_operation(const std::string &aid_hex, const std::string &iccid,
                         bool refresh, int operation) {
    int ch = open_channel(aid_hex);
    try {
      std::vector<uint8_t> identifier =
          encode_tlv(tag_app_iccid(), iccid_to_bcd(iccid));
      std::vector<uint8_t> body;
      if (operation == 51) {
        // DeleteProfile：直接携带 identifier
        body = identifier;
      } else {
        auto ident_wrap = encode_tlv(context_constructed(0), identifier);
        body = ident_wrap;
        auto refresh_flag =
            encode_tlv({0x81}, {static_cast<uint8_t>(refresh ? 1 : 0)});
        body.insert(body.end(), refresh_flag.begin(), refresh_flag.end());
      }
      auto request = encode_tlv(context_constructed(operation), body);
      auto resp = invoke_sgp22(ch, request);
      size_t consumed = 0;
      auto tlvs = parse_tlv(resp, consumed);
      if (!tlvs.empty()) {
        if (const auto res = find_child(tlvs[0], tag_result());
            res.has_value()) {
          const Tlv &result_tlv = res->get();
          int result = 0;
          for (uint8_t b : result_tlv.value)
            result = (result << 8) | b;
          if (result != 0) {
            throw usb::UsbError("profile operation result=" +
                                std::to_string(result));
          }
        }
      }
    } catch (...) {
      close_channel(ch);
      throw;
    }
    close_channel(ch);
  }

  std::vector<Profile> parse_profiles(const std::vector<uint8_t> &data) {
    std::vector<Profile> out;
    size_t consumed = 0;
    auto roots = parse_tlv(data, consumed);
    if (roots.empty())
      return out;
    const auto list = find_child(roots[0], context_constructed(0));
    if (!list.has_value())
      return out;
    const Tlv &list_tlv = list->get();
    for (const auto &node : list_tlv.children) {
      if (node.tag != tag_profile_root_e3() &&
          node.tag != tag_profile_root_bf25()) {
        continue;
      }
      Profile p;
      if (const auto c = find_child(node, tag_app_iccid()); c.has_value()) {
        p.iccid = iccid_from_bcd(c->get().value);
      }
      if (const auto c = find_child(node, tag_app_isdpaid()); c.has_value()) {
        p.isdp_aid = bytes_to_hex(c->get().value);
      }
      if (const auto c = find_child(node, tag_profile_state()); c.has_value()) {
        int st = 0;
        for (uint8_t b : c->get().value)
          st = (st << 8) | b;
        p.state = (st == 1) ? "enabled" : (st == 0 ? "disabled" : "terminated");
      }
      if (const auto c = find_child(node, tag_nickname()); c.has_value()) {
        p.nickname.assign(c->get().value.begin(), c->get().value.end());
      }
      if (const auto c = find_child(node, tag_spn()); c.has_value()) {
        p.provider.assign(c->get().value.begin(), c->get().value.end());
      }
      if (const auto c = find_child(node, tag_profile_name()); c.has_value()) {
        p.name.assign(c->get().value.begin(), c->get().value.end());
      }
      out.push_back(p);
    }
    return out;
  }

  usb::AtChannel &at_;
};

} // namespace     esim
