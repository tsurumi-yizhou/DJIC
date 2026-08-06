module;

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

export module smscodec;

namespace smscodec {

// --- GSM-7 默认字母表（索引 0-127） ---
constexpr std::string_view kGsm7Alphabet =
    "@\xa3$\xa5\xe8\xe9\xf9\xec\xf2\xc7\n\xd8\xf8\r\xc5\xe5"
    "\xce_\xc6\xc8\xe3\xcc\xc9\xfb\xd1\xdc\xd4\xdf\xc4\xc6\xc9\xc1"
    " !\"#$%&'()*+,-./0123456789:;<=>?"
    "\xa1"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "\xc4\xd6\xd1\xdc\xa7\xbf"
    "abcdefghijklmnopqrstuvwxyz"
    "\xe4\xf6\xf1\xfc\xe0";

export inline std::string gsm7_char_at(int index) {
  std::string s(1, kGsm7Alphabet[index]);
  return s;
}

export inline const std::string &gsm7_alphabet() {
  static const std::string s(kGsm7Alphabet);
  return s;
}

// 尝试将文本编码为 GSM-7 septet 序列；若含不可编码字符返回 nullopt（应回退
// UCS2）。
export inline std::optional<std::vector<uint8_t>>
encode_gsm7_septets(const std::string &text) {
  static const auto table = [] {
    std::unordered_map<char, uint8_t> m;
    const std::string &a = gsm7_alphabet();
    for (size_t i = 0; i < a.size(); ++i) {
      m[a[i]] = static_cast<uint8_t>(i);
    }
    return m;
  }();
  std::vector<uint8_t> septets;
  septets.reserve(text.size());
  for (unsigned char c : text) {
    auto it = table.find(static_cast<char>(c));
    if (it == table.end()) {
      return std::nullopt;
    }
    septets.push_back(it->second);
  }
  return septets;
}

export inline std::string
decode_gsm7_septets(const std::vector<uint8_t> &septets) {
  std::string out;
  out.reserve(septets.size());
  for (uint8_t s : septets) {
    if (s < 128) {
      out.push_back(kGsm7Alphabet[s]);
    }
  }
  return out;
}

// --- 7-bit packing ---
export inline std::vector<uint8_t>
pack_septets(const std::vector<uint8_t> &septets) {
  std::vector<uint8_t> out;
  uint32_t bitbuf = 0;
  int bitcount = 0;
  for (uint8_t s : septets) {
    bitbuf |= static_cast<uint32_t>(s & 0x7F) << bitcount;
    bitcount += 7;
    while (bitcount >= 8) {
      out.push_back(bitbuf & 0xFF);
      bitbuf >>= 8;
      bitcount -= 8;
    }
  }
  if (bitcount > 0) {
    out.push_back(bitbuf & 0xFF);
  }
  return out;
}

export inline std::vector<uint8_t>
unpack_septets(std::span<const uint8_t> octets, size_t nseptets) {
  std::vector<uint8_t> out;
  out.reserve(nseptets);
  uint32_t bitbuf = 0;
  int bitcount = 0;
  size_t octidx = 0;
  for (size_t i = 0; i < nseptets; ++i) {
    while (bitcount < 7) {
      if (octidx >= octets.size()) {
        return out;
      }
      bitbuf |= static_cast<uint32_t>(octets[octidx++]) << bitcount;
      bitcount += 8;
    }
    out.push_back(bitbuf & 0x7F);
    bitbuf >>= 7;
    bitcount -= 7;
  }
  return out;
}

// 混合位流打包：先 8-bit 头字节再 7-bit septet，用于 GSM-7 + UDH。
export inline std::vector<uint8_t>
pack_header_then_septets(const std::vector<uint8_t> &header_octets,
                         const std::vector<uint8_t> &septets) {
  std::vector<uint8_t> out;
  uint32_t bitbuf = 0;
  int bitcount = 0;
  auto push = [&](uint32_t val, int nbits) {
    bitbuf |= val << bitcount;
    bitcount += nbits;
    while (bitcount >= 8) {
      out.push_back(bitbuf & 0xFF);
      bitbuf >>= 8;
      bitcount -= 8;
    }
  };
  for (uint8_t o : header_octets)
    push(o, 8);
  for (uint8_t s : septets)
    push(s & 0x7F, 7);
  if (bitcount > 0)
    out.push_back(bitbuf & 0xFF);
  return out;
}

export inline std::vector<uint8_t>
unpack_header_then_septets(std::span<const uint8_t> octets, size_t nseptets,
                           std::vector<uint8_t> &out_header,
                           std::vector<uint8_t> &out_septets) {
  // 从 septet 流重建位数组
  std::vector<bool> bits;
  bits.reserve(nseptets * 7);
  const size_t available = std::min(nseptets, octets.size());
  for (size_t i = 0; i < available; ++i) {
    uint8_t s = octets[i];
    for (int b = 0; b < 7; ++b) {
      bits.push_back((s >> b) & 1);
    }
  }
  out_header.clear();
  out_septets.clear();
  size_t pos = 0;
  while (pos + 8 <= bits.size()) {
    uint8_t o = 0;
    for (int b = 0; b < 8; ++b) {
      if (bits[pos + b])
        o |= static_cast<uint8_t>(1u << b);
    }
    out_header.push_back(o);
    pos += 8;
  }
  while (pos + 7 <= bits.size()) {
    uint8_t s = 0;
    for (int b = 0; b < 7; ++b) {
      if (bits[pos + b])
        s |= static_cast<uint8_t>(1u << b);
    }
    out_septets.push_back(s);
    pos += 7;
  }
  return {};
}

// --- UCS2 ---
export inline std::vector<uint8_t> encode_ucs2(const std::string &utf8) {
  // UTF-8 -> code points -> UTF-16BE (含代理对)
  std::vector<uint8_t> out;
  auto append_utf16 = [&](uint32_t cp) {
    if (cp < 0x10000) {
      out.push_back(static_cast<uint8_t>(cp >> 8));
      out.push_back(static_cast<uint8_t>(cp & 0xFF));
    } else {
      cp -= 0x10000;
      uint32_t hi = 0xD800 + (cp >> 10);
      uint32_t lo = 0xDC00 + (cp & 0x3FF);
      out.push_back(static_cast<uint8_t>(hi >> 8));
      out.push_back(static_cast<uint8_t>(hi & 0xFF));
      out.push_back(static_cast<uint8_t>(lo >> 8));
      out.push_back(static_cast<uint8_t>(lo & 0xFF));
    }
  };
  size_t i = 0;
  while (i < utf8.size()) {
    unsigned char c = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = 0;
    size_t n = 0;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 0x6) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c >> 4) == 0xE) {
      cp = c & 0x0F;
      n = 3;
    } else if ((c >> 3) == 0x1E) {
      cp = c & 0x07;
      n = 4;
    }
    if (i + n > utf8.size())
      break;
    for (size_t k = 1; k < n; ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    }
    append_utf16(cp);
    i += n;
  }
  return out;
}

export inline std::string decode_ucs2(std::span<const uint8_t> data) {
  std::string out;
  auto append_utf8 = [&](uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };
  for (size_t i = 0; i + 1 < data.size(); i += 2) {
    uint16_t u = static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < data.size()) {
      uint16_t lo = static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        uint32_t cp = 0x10000 + ((static_cast<uint32_t>(u - 0xD800) << 10) |
                                 (lo - 0xDC00));
        append_utf8(cp);
        i += 2;
        continue;
      }
    }
    append_utf8(u);
  }
  return out;
}

// --- 号码打包 ---
export inline bool is_short_code(const std::string &phone) {
  if (!phone.empty() && phone[0] == '+')
    return false;
  if (phone.size() > 6)
    return false;
  for (char c : phone) {
    if (c < '0' || c > '9')
      return false;
  }
  return true;
}

// 打包地址：地址长度(半字节数) + TON/NPI + 数字（每字节两位，F 填充）
export inline std::vector<uint8_t> pack_phone(const std::string &number,
                                              uint8_t &ton_npi) {
  std::string digits;
  if (!number.empty() && number[0] == '+') {
    digits = number.substr(1);
  } else {
    digits = number;
  }
  ton_npi = is_short_code(number) ? 0x81 : 0x91;
  std::vector<uint8_t> out;
  out.push_back(static_cast<uint8_t>(digits.size()));
  out.push_back(ton_npi);
  for (size_t i = 0; i < digits.size(); i += 2) {
    uint8_t hi = static_cast<uint8_t>(digits[i] - '0');
    uint8_t lo = (i + 1 < digits.size())
                     ? static_cast<uint8_t>(digits[i + 1] - '0')
                     : 0x0F;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return out;
}

// 解包地址（addr 指向长度字节）
export inline std::string unpack_phone(std::span<const uint8_t> addr) {
  if (addr.size() < 2) {
    return {};
  }
  int len = addr[0];
  const size_t required = 2 + (static_cast<size_t>(len) + 1) / 2;
  if (addr.size() < required) {
    return {};
  }
  std::string num;
  for (int i = 0; i < len; ++i) {
    uint8_t nib =
        (i % 2 == 0) ? (addr[2 + i / 2] >> 4) : (addr[2 + i / 2] & 0x0F);
    if (nib <= 9) {
      num.push_back(static_cast<char>('0' + nib));
    } else {
      break;
    }
  }
  return num;
}

// --- 时间 ---
// SCTS：7 字节，每字节高低半字节交换的 BCD（YY MM DD HH MM SS TZ）
export inline std::string decode_scts(std::span<const uint8_t> scts) {
  if (scts.size() < 7) {
    return {};
  }
  // 字节为交换后 BCD：tens 在低半字节，units 在高半字节
  auto bcd = [](uint8_t b) {
    return static_cast<int>((b & 0x0F) * 10 + ((b >> 4) & 0x0F));
  };
  int yy = bcd(scts[0]);
  int mm = bcd(scts[1]);
  int dd = bcd(scts[2]);
  int hh = bcd(scts[3]);
  int mi = bcd(scts[4]);
  int ss = bcd(scts[5]);
  std::array<char, 32> buf{};
  std::snprintf(buf.data(), buf.size(), "%04d-%02d-%02d %02d:%02d:%02d",
                2000 + yy, mm, dd, hh, mi, ss);
  return std::string(buf.data());
}

// --- 结构体 ---
export struct ConcatInfo {
  bool is_concat = false;
  int ref = 0;
  int ref_bits = 0;
  int total = 0;
  int seq = 0;
};

export struct Deliver {
  std::string sender;
  std::string content;
  std::string timestamp;
  ConcatInfo concat;
  bool ok = false;
  std::string error;
};

export struct SubmitSegment {
  std::vector<uint8_t> tpdu; // 不含 SMSC 长度
  int length = 0;            // TPDU 字节数
};

// 解析 UDH 拼接信息（跳过 UDHL 长度字节）
export inline ConcatInfo parse_udh_concat(const std::vector<uint8_t> &udh) {
  ConcatInfo info;
  if (udh.empty())
    return info;
  size_t udhl = udh[0];
  size_t i = 1;
  size_t limit = std::min(udh.size(), static_cast<size_t>(1 + udhl));
  while (i + 1 < limit) {
    uint8_t iei = udh[i];
    uint8_t len = udh[i + 1];
    if (i + 2 + len > limit)
      break;
    if (iei == 0x00 && len == 3) {
      info.is_concat = true;
      info.ref = udh[i + 2];
      info.ref_bits = 8;
      info.total = udh[i + 3];
      info.seq = udh[i + 4];
      break;
    } else if (iei == 0x08 && len == 4) {
      info.is_concat = true;
      info.ref = (udh[i + 2] << 8) | udh[i + 3];
      info.ref_bits = 16;
      info.total = udh[i + 4];
      info.seq = udh[i + 5];
      break;
    }
    i += 2 + len;
  }
  return info;
}

// 解码 SMS-DELIVER TPDU（不含 SMSC）
export inline Deliver decode_deliver_tpdu(const std::vector<uint8_t> &tpdu) {
  Deliver d;
  if (tpdu.size() < 2) {
    d.error = "TPDU too short";
    return d;
  }
  uint8_t first = tpdu[0];
  bool udhi = (first & 0x40) != 0;
  size_t pos = 1;

  // TP-OA
  if (pos >= tpdu.size()) {
    d.error = "truncated OA";
    return d;
  }
  uint8_t addr_len = tpdu[pos];
  size_t addr_bytes = 2 + static_cast<size_t>((addr_len + 1) / 2);
  if (pos + addr_bytes > tpdu.size()) {
    d.error = "truncated address";
    return d;
  }
  d.sender = unpack_phone(std::span(tpdu).subspan(pos));
  pos += addr_bytes;

  // TP-PID / TP-DCS
  if (pos + 2 > tpdu.size()) {
    d.error = "truncated pid/dcs";
    return d;
  }
  pos += 2;
  uint8_t dcs = tpdu[pos - 1];

  // TP-SCTS（7 字节）
  if (pos + 7 > tpdu.size()) {
    d.error = "truncated scts";
    return d;
  }
  d.timestamp = decode_scts(std::span(tpdu).subspan(pos));
  pos += 7;

  // TP-UDL / TP-UD
  if (pos >= tpdu.size()) {
    d.error = "truncated udl";
    return d;
  }
  size_t udl = tpdu[pos];
  ++pos;

  bool is_ucs2 = (dcs & 0x0C) == 0x08;
  bool is_gsm7 = (dcs & 0x0C) == 0x00;
  bool is_8bit = (dcs & 0x0C) == 0x04;

  if (is_ucs2) {
    size_t bytes = udl;
    if (pos + bytes > tpdu.size()) {
      d.error = "truncated ucs2 data";
      return d;
    }
    const auto ud = std::span(tpdu).subspan(pos, bytes);
    if (udhi) {
      if (bytes < 2) {
        d.error = "truncated udh";
        return d;
      }
      size_t udhl = ud[0];    // UDHL：UDHL 之后 UDH 的字节数
      size_t hlen = udhl + 1; // 含 UDHL 自身
      std::vector<uint8_t> udh(ud.begin(), ud.begin() + std::min(hlen, bytes));
      d.concat = parse_udh_concat(udh);
      if (hlen <= bytes) {
        d.content = decode_ucs2(ud.subspan(hlen));
      }
    } else {
      d.content = decode_ucs2(ud);
    }
  } else if (is_gsm7) {
    if (udhi) {
      std::vector<uint8_t> header;
      std::vector<uint8_t> septets;
      unpack_header_then_septets(std::span(tpdu).subspan(pos), udl, header,
                                 septets);
      d.concat = parse_udh_concat(header);
      d.content = decode_gsm7_septets(septets);
    } else {
      auto septets = unpack_septets(std::span(tpdu).subspan(pos), udl);
      d.content = decode_gsm7_septets(septets);
    }
  } else if (is_8bit) {
    size_t bytes = udl;
    if (pos + bytes > tpdu.size()) {
      d.error = "truncated 8bit data";
      return d;
    }
    // 二进制短信（如运营商 OTA），不强行按文本解码
    std::string hex;
    for (size_t i = 0; i < bytes; ++i) {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%02X", tpdu[pos + i]);
      hex += buf;
    }
    d.content = "[二进制短信 " + std::to_string(bytes) + "B " + hex + "]";
  } else {
    d.error = "unsupported DCS";
    return d;
  }

  d.ok = true;
  return d;
}

// 构建上行 SMS-SUBMIT TPDU 列表（自动分段 + 拼接 UDH）
// force_ucs2: 强制 UCS2；否则字符 > 127 自动 UCS2
export inline std::vector<SubmitSegment>
build_submit_tpdus(const std::string &phone, const std::string &text,
                   bool force_ucs2) {
  std::vector<SubmitSegment> out;

  bool use_ucs2 = force_ucs2;
  std::vector<uint8_t> gsm7_septets;
  if (!use_ucs2) {
    for (unsigned char c : text) {
      if (c > 127) {
        use_ucs2 = true;
        break;
      }
    }
  }

  if (!use_ucs2) {
    // 检查 GSM-7 可编码性，不可编码回退 UCS2
    const auto encoded = encode_gsm7_septets(text);
    if (!encoded) {
      use_ucs2 = true;
    } else {
      gsm7_septets = *encoded;
    }
  }

  auto append_segment = [&](const std::vector<uint8_t> &tpdu) {
    out.push_back(SubmitSegment{tpdu, static_cast<int>(tpdu.size())});
  };

  if (use_ucs2) {
    auto bytes = encode_ucs2(text);
    size_t nchars = bytes.size() / 2;
    size_t per_segment = 70;
    size_t segments =
        (nchars == 0) ? 1 : (nchars + per_segment - 1) / per_segment;
    if (segments == 1) {
      std::vector<uint8_t> tpdu;
      tpdu.push_back(0x11);
      tpdu.push_back(0x00);
      uint8_t ton = 0;
      auto da = pack_phone(phone, ton);
      tpdu.insert(tpdu.end(), da.begin(), da.end());
      tpdu.push_back(0x00);                               // PID
      tpdu.push_back(0x08);                               // DCS UCS2
      tpdu.push_back(static_cast<uint8_t>(bytes.size())); // UDL（字节）
      tpdu.insert(tpdu.end(), bytes.begin(), bytes.end());
      append_segment(tpdu);
    } else {
      // 拼接段：UDL 上限 134 字节（含 6 字节 UDH），正文 <= 128 字节 = 64 字符
      size_t per_concat = 64;
      size_t seg_count = (nchars + per_concat - 1) / per_concat;
      for (size_t seg = 0; seg < seg_count; ++seg) {
        size_t seg_len = per_concat;
        if (seg == seg_count - 1) {
          seg_len = nchars - seg * per_concat;
        }
        std::vector<uint8_t> udh = {0x05,
                                    0x00,
                                    0x03,
                                    static_cast<uint8_t>(0x01),
                                    static_cast<uint8_t>(seg_count),
                                    static_cast<uint8_t>(seg + 1)};
        std::vector<uint8_t> body(bytes.begin() + seg * per_concat * 2,
                                  bytes.begin() +
                                      (seg * per_concat + seg_len) * 2);
        std::vector<uint8_t> tpdu;
        tpdu.push_back(0x51); // UDHI
        tpdu.push_back(0x00);
        uint8_t ton = 0;
        auto da = pack_phone(phone, ton);
        tpdu.insert(tpdu.end(), da.begin(), da.end());
        tpdu.push_back(0x00);
        tpdu.push_back(0x08);
        size_t udl = udh.size() + body.size();
        tpdu.push_back(static_cast<uint8_t>(udl));
        tpdu.insert(tpdu.end(), udh.begin(), udh.end());
        tpdu.insert(tpdu.end(), body.begin(), body.end());
        append_segment(tpdu);
      }
    }
  } else {
    size_t nseptets = gsm7_septets.size();
    size_t per_segment = 160;
    size_t segments =
        (nseptets == 0) ? 1 : (nseptets + per_segment - 1) / per_segment;
    if (segments == 1) {
      auto packed = pack_septets(gsm7_septets);
      std::vector<uint8_t> tpdu;
      tpdu.push_back(0x11);
      tpdu.push_back(0x00);
      uint8_t ton = 0;
      auto da = pack_phone(phone, ton);
      tpdu.insert(tpdu.end(), da.begin(), da.end());
      tpdu.push_back(0x00);
      tpdu.push_back(0x00); // DCS GSM-7
      tpdu.push_back(static_cast<uint8_t>(nseptets));
      tpdu.insert(tpdu.end(), packed.begin(), packed.end());
      append_segment(tpdu);
    } else {
      size_t msg_per_segment = 153 - 7; // UDL 上限 153，头占 7 septet
      for (size_t seg = 0; seg < segments; ++seg) {
        size_t seg_len = msg_per_segment;
        if (seg == segments - 1) {
          seg_len = nseptets - seg * msg_per_segment;
        }
        std::vector<uint8_t> udh = {0x05,
                                    0x00,
                                    0x03,
                                    static_cast<uint8_t>(0x01),
                                    static_cast<uint8_t>(segments),
                                    static_cast<uint8_t>(seg + 1)};
        std::vector<uint8_t> body(gsm7_septets.begin() + seg * msg_per_segment,
                                  gsm7_septets.begin() + seg * msg_per_segment +
                                      seg_len);
        auto packed = pack_header_then_septets(udh, body);
        std::vector<uint8_t> tpdu;
        tpdu.push_back(0x51);
        tpdu.push_back(0x00);
        uint8_t ton = 0;
        auto da = pack_phone(phone, ton);
        tpdu.insert(tpdu.end(), da.begin(), da.end());
        tpdu.push_back(0x00);
        tpdu.push_back(0x00);
        size_t udl = 7 + seg_len;
        tpdu.push_back(static_cast<uint8_t>(udl));
        tpdu.insert(tpdu.end(), packed.begin(), packed.end());
        append_segment(tpdu);
      }
    }
  }
  return out;
}

export inline std::string to_hex(const std::vector<uint8_t> &data) {
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t b : data) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02X", b);
    out += buf;
  }
  return out;
}

export inline std::vector<uint8_t> from_hex(const std::string &hex) {
  std::vector<uint8_t> out;
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    int hi = val(hex[i]);
    int lo = val(hex[i + 1]);
    if (hi < 0 || lo < 0)
      break;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return out;
}

} // namespace     smscodec
