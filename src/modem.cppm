module;

#include <cctype>
#include <cstdint>
#include <optional>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

export module modem;

import usb;

namespace modem {

// 工作模式（usbnet）：仅保留短信模式与上网模式，不再暴露 2/3 实验模式。
export enum class WorkMode {
  Sms = 0,  // 短信模式
  Data = 1, // 上网模式
};

export struct ModemStatus {
  std::string imei;
  std::string firmware;
  std::string iccid;
  std::string imsi;
  std::string operator_name;
  bool sim_inserted = false;
  int signal_dbm = 0;
  int signal_rsrp = 0;
  int signal_rsrq = 0;
  int reg_status = 0;
  std::string reg_status_text;
  std::string lac;
  std::string cell_id;
  std::string apn;
  std::string network_mode;
  std::string network_duplex;
  std::string radio_band;
  uint32_t radio_channel = 0;
  int usbnet_mode = -1;
  std::optional<WorkMode> work_mode;
};

export inline std::string registration_text(int status) {
  switch (status) {
  case 1:
    return "已注册";
  case 5:
    return "漫游注册";
  case 2:
    return "搜索中";
  case 3:
    return "注册被拒绝";
  default:
    return "未注册";
  }
}

export inline std::string work_mode_name(WorkMode m) {
  switch (m) {
  case WorkMode::Sms:
    return "sms";
  case WorkMode::Data:
    return "data";
  }
  return "unknown";
}

export inline std::string work_mode_display(WorkMode m) {
  switch (m) {
  case WorkMode::Sms:
    return "短信模式";
  case WorkMode::Data:
    return "上网模式";
  }
  return "未知";
}

// 按模式名解析；接受中英文别名。
export inline std::optional<WorkMode>
work_mode_from_name(const std::string &name) {
  std::string low = usb::to_lower(name);
  if (low == "sms" || low == "短信" || low == "call" || low == "通话")
    return WorkMode::Sms;
  if (low == "data" || low == "上网" || low == "net" || low == "internet")
    return WorkMode::Data;
  return std::nullopt;
}

// --- 解析辅助（与上游行为一致） ---

export inline std::string trim_ws(const std::string &s) {
  auto b = s.begin();
  while (b != s.end() && std::isspace(static_cast<unsigned char>(*b)))
    ++b;
  auto e = s.end();
  while (e != b && std::isspace(static_cast<unsigned char>(*(e - 1))))
    --e;
  return std::string(b, e);
}

export inline std::vector<std::string> split_lines(const std::string &resp) {
  return usb::split_at_lines(resp);
}

export inline std::string line_after_prefix(const std::string &resp,
                                            const std::string &prefix) {
  for (const auto &line : split_lines(resp)) {
    if (line.rfind(prefix, 0) == 0) {
      return trim_ws(line.substr(prefix.size()));
    }
  }
  return "";
}

export inline int parse_csq_dbm(const std::string &resp) {
  size_t pos = usb::find_ci(resp, "+CSQ:");
  if (pos == std::string::npos)
    return 0;
  auto ints = usb::parse_ints(resp.substr(pos));
  if (ints.empty())
    return 0;
  int rssi = ints[0];
  if (rssi == 99)
    return 0;
  return -113 + 2 * rssi;
}

export inline int first_nonzero_registration(const std::string &cereg,
                                             const std::string &creg) {
  for (const auto &resp : {cereg, creg}) {
    // "+CREG:" / "+CEREG:" 都包含子串 "REG:"
    size_t pos = usb::find_ci(resp, "REG:");
    if (pos == std::string::npos)
      continue;
    auto ints = usb::parse_ints(resp.substr(pos));
    if (ints.size() >= 2 && ints[1] != 0)
      return ints[1];
  }
  return 0;
}

export inline std::string parse_operator(const std::string &resp) {
  size_t pos = usb::find_ci(resp, "+COPS:");
  if (pos == std::string::npos)
    return "";
  auto quoted = usb::parse_quoted(resp.substr(pos));
  if (quoted.empty())
    return "";
  return quoted[0];
}

export inline void parse_qnwinfo(const std::string &resp, std::string &mode,
                                 std::string &duplex, std::string &band,
                                 uint32_t &channel) {
  mode.clear();
  duplex.clear();
  band.clear();
  channel = 0;
  size_t pos = usb::find_ci(resp, "+QNWINFO:");
  if (pos == std::string::npos)
    return;
  auto rest = resp.substr(pos);
  auto quoted = usb::parse_quoted(rest);
  auto ints = usb::parse_ints(rest);
  if (quoted.size() < 3)
    return;
  mode = quoted[0];
  std::string radio = quoted[2];
  if (usb::find_ci(mode, "FDD") != std::string::npos) {
    duplex = "FDD";
    if (mode.size() >= 3)
      mode.erase(0, 3);
    mode = trim_ws(mode);
  }
  if (usb::find_ci(mode, "TDD") != std::string::npos) {
    duplex = "TDD";
    if (mode.size() >= 3)
      mode.erase(0, 3);
    mode = trim_ws(mode);
  }
  band = radio;
  if (usb::find_ci(band, "LTE ") == 0)
    band.erase(0, 4);
  if (!ints.empty())
    channel = static_cast<uint32_t>(ints.back());
}

export inline int parse_usbnet_mode(const std::string &resp) {
  size_t pos = usb::find_ci(resp, "\"usbnet\"");
  if (pos == std::string::npos)
    return -1;
  auto ints = usb::parse_ints(resp.substr(pos));
  if (ints.empty())
    return -1;
  return ints[0];
}

export inline std::string parse_imsi(const std::string &resp) {
  for (const auto &line : split_lines(resp)) {
    std::string up = usb::to_upper(line);
    if (up == "OK" || up.rfind("AT", 0) == 0)
      continue;
    if (line.size() >= 5) {
      bool all_digits = true;
      for (char c : line) {
        if (c < '0' || c > '9') {
          all_digits = false;
          break;
        }
      }
      if (all_digits)
        return line;
    }
  }
  return "";
}

export inline std::string parse_firmware(const std::string &resp) {
  std::vector<std::string> useful;
  for (const auto &line : split_lines(resp)) {
    std::string up = usb::to_upper(line);
    if (up.rfind("ATI", 0) == 0 || up == "OK")
      continue;
    useful.push_back(line);
  }
  std::string out;
  for (size_t i = 0; i < useful.size(); ++i) {
    if (i)
      out += " · ";
    out += useful[i];
  }
  return out;
}

export inline bool parse_sim_ready(const std::string &cpin_resp) {
  return usb::to_upper(cpin_resp).find("READY") != std::string::npos;
}

// --- 调制解调器控制 ---

export class Modem {
public:
  explicit Modem(usb::AtChannel &at) : at_(at) {}

  usb::AtChannel &channel() { return at_; }

  // 初始化序列：关闭回显、PDU 模式、新短信上报、来电显示、UAC 语音模式
  void init() {
    for (const auto &cmd : {
             std::string("ATE0"),
             std::string("AT+CMGF=0"),
             std::string("AT+CNMI=2,1,0,0,0"),
             std::string("AT+CLIP=1"),
             std::string("AT+QPCMV=1,2"),
         }) {
      try {
        at_.command(cmd, 2000);
      } catch (const usb::UsbError &e) {
        spdlog::warn("init command {} failed: {}", cmd, e.what());
      }
    }
  }

  std::string raw_at(const std::string &cmd, int timeout_ms = 8000) {
    return at_.command(cmd, timeout_ms);
  }

  // 切换工作模式（需要随后 reboot 生效）
  std::string set_work_mode(WorkMode mode) {
    return at_.command(
        "AT+QCFG=\"usbnet\"," + std::to_string(static_cast<int>(mode)), 8000);
  }

  std::optional<WorkMode> get_work_mode() {
    auto resp = at_.command("AT+QCFG=\"usbnet\"", 3000);
    int raw = parse_usbnet_mode(resp);
    if (raw == static_cast<int>(WorkMode::Sms))
      return WorkMode::Sms;
    if (raw == static_cast<int>(WorkMode::Data))
      return WorkMode::Data;
    return std::nullopt;
  }

  // 重启模组使 usbnet 模式生效（USB 会重新枚举）
  std::string reboot() { return at_.command("AT+CFUN=1,1", 3000); }

  ModemStatus collect_status() {
    ModemStatus s;

    std::string firmware_resp, cpin_resp, csq_resp, cereg_resp, creg_resp;
    std::string cops_resp, qccid_resp, cimi_resp, qnwinfo_resp, usbnet_resp;
    auto safe = [&](const std::string &cmd, int timeout, std::string &out) {
      try {
        out = at_.command(cmd, timeout);
      } catch (const usb::UsbError &e) {
        spdlog::warn("status query {} failed: {}", cmd, e.what());
      }
    };
    safe("ATI", 3000, firmware_resp);
    safe("AT+CPIN?", 3000, cpin_resp);
    safe("AT+CSQ", 3000, csq_resp);
    safe("AT+CEREG?", 3000, cereg_resp);
    safe("AT+CREG?", 3000, creg_resp);
    safe("AT+COPS=3,2", 2000, cops_resp);
    safe("AT+COPS?", 3000, cops_resp);
    safe("AT+QCCID", 3000, qccid_resp);
    safe("AT+CIMI", 3000, cimi_resp);
    safe("AT+QNWINFO", 3000, qnwinfo_resp);
    safe("AT+QCFG=\"usbnet\"", 3000, usbnet_resp);

    s.firmware = parse_firmware(firmware_resp);
    s.iccid = line_after_prefix(qccid_resp, "+QCCID:");
    s.imsi = parse_imsi(cimi_resp);
    s.operator_name = parse_operator(cops_resp);
    if (s.operator_name.empty() &&
        cops_resp.find("CHN-UNICOM") != std::string::npos) {
      s.operator_name = "CHN-UNICOM";
    }
    s.sim_inserted = parse_sim_ready(cpin_resp);
    s.signal_dbm = parse_csq_dbm(csq_resp);
    s.reg_status = first_nonzero_registration(cereg_resp, creg_resp);
    s.reg_status_text = registration_text(s.reg_status);
    parse_qnwinfo(qnwinfo_resp, s.network_mode, s.network_duplex, s.radio_band,
                  s.radio_channel);
    s.usbnet_mode = parse_usbnet_mode(usbnet_resp);
    if (s.usbnet_mode == static_cast<int>(WorkMode::Sms))
      s.work_mode = WorkMode::Sms;
    if (s.usbnet_mode == static_cast<int>(WorkMode::Data))
      s.work_mode = WorkMode::Data;
    return s;
  }

private:
  usb::AtChannel &at_;
};

} // namespace     modem
