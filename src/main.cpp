#include <argparse/argparse.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

import usb;
import modem;
import sms;
import esim;
import asterisk;

using usb::AtChannel;
using usb::UsbAt;
using usb::UsbError;

namespace {

std::atomic<bool> running{true};

void stop_running(int) { running.store(false); }

std::string join(const std::vector<std::string> &parts,
                 const std::string &sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i)
      out += sep;
    out += parts[i];
  }
  return out;
}

std::vector<std::string> tokenize(const std::string &line) {
  std::vector<std::string> out;
  std::istringstream ss(line);
  std::string tok;
  while (ss >> tok)
    out.push_back(tok);
  return out;
}

// 打开 USB AT 通道（带重试，适用于重启后 USB 重新枚举）
std::unique_ptr<AtChannel> open_at_channel(int retries = 1, int delay_ms = 0) {
  for (int i = 0; i < retries; ++i) {
    auto dev = UsbAt::open();
    if (dev.has_value()) {
      spdlog::info("USB AT bridge opened on {}", dev->description());
      return std::make_unique<UsbAt>(std::move(*dev));
    }
    if (i + 1 < retries && delay_ms > 0) {
      spdlog::info("waiting for USB re-enumeration...");
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
  }
  return nullptr;
}

void print_status(const modem::ModemStatus &s) {
  std::cout << "== DJI 4G 模块状态 ==\n";
  std::cout << "固件:       " << (s.firmware.empty() ? "-" : s.firmware)
            << "\n";
  std::cout << "ICCID:      " << (s.iccid.empty() ? "-" : s.iccid) << "\n";
  std::cout << "IMSI:       " << (s.imsi.empty() ? "-" : s.imsi) << "\n";
  std::cout << "运营商:     "
            << (s.operator_name.empty() ? "-" : s.operator_name) << "\n";
  std::cout << "SIM 已插入: " << (s.sim_inserted ? "是" : "否") << "\n";
  std::cout << "信号:       " << s.signal_dbm << " dBm"
            << (s.signal_rsrp != 0
                    ? " (RSRP " + std::to_string(s.signal_rsrp) + " RSRQ " +
                          std::to_string(s.signal_rsrq) + ")"
                    : "")
            << "\n";
  std::cout << "网络注册:   " << s.reg_status_text << " (" << s.reg_status
            << ")\n";
  std::cout << "网络制式:   "
            << (s.network_mode.empty() ? "-" : s.network_mode);
  if (!s.network_duplex.empty())
    std::cout << " " << s.network_duplex;
  if (!s.radio_band.empty())
    std::cout << " · " << s.radio_band;
  if (s.radio_channel != 0)
    std::cout << " ch" << s.radio_channel;
  std::cout << "\n";
  if (s.work_mode.has_value()) {
    std::cout << "工作模式:   " << modem::work_mode_display(*s.work_mode)
              << "\n";
  } else {
    std::cout << "工作模式:   未知 (usbnet " << s.usbnet_mode << ")\n";
  }
}

// 切换工作模式并重启模组（重启后 USB 重新枚举，AT 通道会短暂消失）
void switch_work_mode(AtChannel &at, modem::WorkMode target) {
  modem::Modem modem(at);
  auto current = modem.get_work_mode();
  if (current.has_value() && *current == target) {
    std::cout << "模块已处于" << modem::work_mode_display(target)
              << "，无需切换。\n";
    return;
  }
  std::cout << "切换到" << modem::work_mode_display(target) << " ...\n";
  std::string resp = modem.set_work_mode(target);
  std::cout << "设置响应: " << resp << "\n";
  std::cout << "重启模组使设置生效...\n";
  modem.reboot();
  std::cout << "模组正在重启并重新枚举 USB 接口，请稍候（约 8-13 秒）。\n";
}

int cmd_status() {
  auto at = open_at_channel();
  if (!at) {
    std::cerr << "未检测到 DJI USB 设备（2ca3:4006）。请连接模块后再试。\n";
    return 1;
  }
  modem::Modem modem(*at);
  print_status(modem.collect_status());
  return 0;
}

// 数据上网模式：切换 usbnet=data + 重启，输出使用指引。
int cmd_data() {
  auto at = open_at_channel();
  if (!at) {
    std::cerr << "无法打开 USB AT 接口。若模块已处于上网模式（暴露 USB "
                 "网卡）则无需操作；"
                 "否则请先让模块暴露 AT 接口（短信模式）后连接。\n";
    return 1;
  }
  switch_work_mode(*at, modem::WorkMode::Data);
  std::cout << "\n== 数据上网模式 ==\n";
  std::cout
      << "模块重启后将向 macOS 暴露 USB 网卡（通常命名为 \"Baiwang\"），\n";
  std::cout << "通过 DHCP 分配类似 192.168.225.x 的地址并转发蜂窝流量。\n";
  std::cout << "请在 系统设置 -> 网络 "
               "中查看该网络服务；如代理失效请为该服务重新配置系统代理。\n";
  std::cout << "进程将持续运行；按 Ctrl+C 退出。\n";
  std::signal(SIGINT, stop_running);
  std::signal(SIGTERM, stop_running);
  while (running.load())
    std::this_thread::sleep_for(std::chrono::seconds(1));
  std::cout << "已退出数据上网模式。\n";
  return 0;
}

// 通话&短信模式：切换 usbnet=sms + 重启，进入交互模式（短信轮询 + Asterisk）。
int cmd_sms() {
  auto at = open_at_channel();
  if (!at) {
    std::cerr
        << "无法打开 USB AT 接口（2ca3:4006）。请将模块置于短信模式并连接。\n";
    return 1;
  }
  auto modem = std::make_unique<modem::Modem>(*at);
  auto mode = modem->get_work_mode();
  if (!mode.has_value() || *mode != modem::WorkMode::Sms) {
    std::cout << "模块当前工作模式未知/非短信模式，切换到短信模式...\n";
    try {
      modem->set_work_mode(modem::WorkMode::Sms);
      modem->reboot();
    } catch (const UsbError &e) {
      spdlog::warn("switch to sms mode failed: {}", e.what());
    }
    // 重启后重新打开 AT 通道
    at = open_at_channel(15, 1000);
    if (!at) {
      std::cerr << "等待模块重启后无法重新打开 USB AT 接口。\n";
      return 1;
    }
    modem = std::make_unique<modem::Modem>(*at);
  }
  modem->init();

  sms::SmsService sms(*at);
  auto client = asterisk::create_client();
  asterisk::Callbacks callbacks;
  callbacks.on_incoming_call = [](const std::string &number) {
    std::cout << "\n[Asterisk 来电] " << (number.empty() ? "未知号码" : number)
              << "\n";
  };
  callbacks.on_call_answered = []() {
    std::cout << "\n[Asterisk] 通话已接通\n";
  };
  callbacks.on_call_ended = []() { std::cout << "\n[Asterisk] 通话已结束\n"; };
  callbacks.on_message = [](const std::string &number,
                            const std::string &message) {
    std::cout << "\n[Asterisk 消息] " << number << ": " << message << "\n";
  };
  if (!client->start(asterisk::Account{}, std::move(callbacks))) {
    spdlog::warn("Asterisk client is unavailable: {}", client->error());
  }

  std::atomic<bool> stop{false};

  // 短信轮询线程（每 8 秒）
  std::thread poller([&]() {
    while (!stop.load()) {
      try {
        auto new_msgs = sms.poll();
        if (!new_msgs.empty()) {
          int before = static_cast<int>(sms.cached().size());
          sms.merge(new_msgs);
          int added = static_cast<int>(sms.cached().size()) - before;
          if (added > 0) {
            std::cout << "\n[新短信 x" << added << "] 输入 help 查看命令。\n";
          }
        }
      } catch (const UsbError &e) {
        spdlog::warn("SMS poll failed: {}", e.what());
      }
      std::this_thread::sleep_for(std::chrono::seconds(8));
    }
  });

  // 调制解调器通话监控线程（保留用于显示蜂窝侧通话状态）。
  std::thread callmonitor([&]() {
    std::map<int, std::string> active;
    while (!stop.load()) {
      try {
        std::string resp = at->command("AT+CLCC", 3000);
        std::set<int> current;
        auto lines = usb::split_at_lines(resp);
        for (const auto &line : lines) {
          if (usb::find_ci(line, "+CLCC:") != 0)
            continue;
          auto ints = usb::parse_ints(line);
          if (ints.size() < 3)
            continue;
          int id = ints[0];
          int dir = ints[1];
          static_cast<void>(ints[2]);
          current.insert(id);
          auto quoted = usb::parse_quoted(line);
          std::string number = quoted.empty() ? "" : quoted[0];
          if (dir == 1 && active.find(id) == active.end()) {
            active[id] = number;
            spdlog::info("cellular incoming call from {}", number);
          }
        }
        for (auto it = active.begin(); it != active.end();) {
          if (current.find(it->first) == current.end()) {
            it = active.erase(it);
          } else {
            ++it;
          }
        }
      } catch (const UsbError &) {
      }
      std::this_thread::sleep_for(std::chrono::seconds(5));
    }
  });

  std::cout << "\n== 通话&短信模式 ==\n";
  std::cout << "已连接: " << at->description() << "\n";
  print_status(modem->collect_status());
  std::cout << "\n可用命令:\n";
  std::cout << "  send <号码> <内容>   发送短信\n";
  std::cout << "  list                 列出已收短信\n";
  std::cout << "  status               显示模块状态\n";
  std::cout << "  refresh              立即轮询短信\n";
  std::cout << "  clear-me             清空模块 ME 存储旧短信\n";
  std::cout << "  at <指令>            发送原始 AT 指令\n";
  std::cout << "  call <号码>          通过 Asterisk 拨号\n";
  std::cout << "  answer / hangup      接听 / 挂断 Asterisk 通话\n";
  std::cout << "  message-send <号码> <内容> 通过 Asterisk 发送消息\n";
  std::cout << "  quit                 退出（Ctrl+C 亦可）\n";

  std::string line;
  while (true) {
    std::cout << "\ncellular-sms> " << std::flush;
    if (!std::getline(std::cin, line))
      break;
    auto parts = tokenize(line);
    if (parts.empty())
      continue;
    std::string cmd = parts[0];
    if (cmd == "quit" || cmd == "exit" || cmd == "q")
      break;
    try {
      if (cmd == "send") {
        if (parts.size() < 3) {
          std::cout << "用法: send <号码> <内容>\n";
          continue;
        }
        std::string msg =
            join(std::vector<std::string>(parts.begin() + 2, parts.end()), " ");
        int seg = sms.send(parts[1], msg);
        std::cout << "短信已发送（" << seg << " 段）。\n";
      } else if (cmd == "list") {
        auto cached = sms.cached();
        if (cached.empty()) {
          std::cout << "（暂无已收短信）\n";
          continue;
        }
        for (size_t i = 0; i < cached.size(); ++i) {
          std::cout << "[" << (i + 1) << "] " << cached[i].timestamp << " 来自 "
                    << cached[i].sender
                    << (cached[i].code.empty()
                            ? ""
                            : " [验证码: " + cached[i].code + "]")
                    << "\n    " << cached[i].content << "\n";
        }
      } else if (cmd == "status") {
        print_status(modem->collect_status());
      } else if (cmd == "refresh") {
        auto new_msgs = sms.poll();
        sms.merge(new_msgs);
        std::cout << "轮询完成，共 " << sms.cached().size() << " 条，本次新增 "
                  << new_msgs.size() << " 条。\n";
      } else if (cmd == "clear-me") {
        auto [before, after] = sms.clear_memory("ME");
        std::cout << "已清空 ME 存储短信: " << before << " -> " << after
                  << "\n";
      } else if (cmd == "at") {
        if (parts.size() < 2) {
          std::cout << "用法: at <指令>\n";
          continue;
        }
        std::string at_cmd =
            join(std::vector<std::string>(parts.begin() + 1, parts.end()), " ");
        std::string resp = at->command(at_cmd, 20000);
        std::cout << resp << "\n";
      } else if (cmd == "call") {
        if (parts.size() < 2) {
          std::cout << "用法: call <号码>\n";
          continue;
        }
        client->dial(parts[1]);
      } else if (cmd == "answer") {
        client->answer();
      } else if (cmd == "hangup") {
        client->hangup();
      } else if (cmd == "message-send") {
        if (parts.size() < 3) {
          std::cout << "用法: message-send <号码> <内容>\n";
          continue;
        }
        std::string message =
            join(std::vector<std::string>(parts.begin() + 2, parts.end()), " ");
        if (!client->send_message(parts[1], message))
          std::cout << "Asterisk 消息发送失败: " << client->error() << "\n";
      } else if (cmd == "help") {
        std::cout << "send/list/status/refresh/clear-me/at/call/answer/hangup/"
                     "message-send/quit\n";
      } else {
        std::cout << "未知命令: " << cmd << "（输入 help 查看）\n";
      }
    } catch (const std::exception &e) {
      std::cout << "错误: " << e.what() << "\n";
    }
  }

  stop.store(true);
  if (poller.joinable())
    poller.join();
  if (callmonitor.joinable())
    callmonitor.join();
  client->stop();
  std::cout << "已退出通话&短信模式。\n";
  return 0;
}

int cmd_sms_send(const std::string &phone, const std::string &message) {
  auto ch = open_at_channel();
  if (!ch) {
    std::cerr << "无法打开 USB AT 接口（2ca3:4006）。\n";
    return 1;
  }
  sms::SmsService sms(*ch);
  int seg = sms.send(phone, message);
  std::cout << "短信已发送（" << seg << " 段）。\n";
  return 0;
}

int cmd_sms_list() {
  auto ch = open_at_channel();
  if (!ch) {
    std::cerr << "无法打开 USB AT 接口（2ca3:4006）。\n";
    return 1;
  }
  sms::SmsService sms(*ch);
  auto msgs = sms.poll();
  if (msgs.empty()) {
    std::cout << "（暂无短信）\n";
    return 0;
  }
  for (size_t i = 0; i < msgs.size(); ++i) {
    std::cout << "[" << (i + 1) << "] " << msgs[i].timestamp << " 来自 "
              << msgs[i].sender
              << (msgs[i].code.empty() ? "" : " [验证码: " + msgs[i].code + "]")
              << "\n    " << msgs[i].content << "\n";
  }
  return 0;
}

int cmd_sms_clear() {
  auto ch = open_at_channel();
  if (!ch) {
    std::cerr << "无法打开 USB AT 接口（2ca3:4006）。\n";
    return 1;
  }
  sms::SmsService sms(*ch);
  auto [before, after] = sms.clear_memory("ME");
  std::cout << "已清空 ME 存储短信: " << before << " -> " << after << "\n";
  return 0;
}

std::optional<std::string> discover_aid(esim::EsimClient &esim) {
  auto aids = esim.scan_aids();
  if (aids.empty())
    return std::nullopt;
  return aids.front();
}

int cmd_esim(const std::vector<std::string> &args) {
  if (args.empty()) {
    std::cerr
        << "用法: cellular esim <eid|list|switch|enable|disable|delete|apdu>\n";
    return 1;
  }
  auto ch = open_at_channel();
  if (!ch) {
    std::cerr << "无法打开 USB AT 接口（2ca3:4006）。\n";
    return 1;
  }
  esim::EsimClient esim(*ch);
  const std::string &action = args[0];

  if (action == "eid") {
    auto eid = esim.get_eid();
    if (!eid) {
      std::cerr << "未检测到兼容的 eUICC 卡片。\n";
      return 1;
    }
    std::cout << "EID: " << *eid << "\n";
    return 0;
  }

  if (action == "list") {
    auto aid = discover_aid(esim);
    if (!aid) {
      std::cerr << "未检测到兼容的 eUICC 卡片。\n";
      return 1;
    }
    auto profiles = esim.list_profiles(*aid);
    if (profiles.empty()) {
      std::cout << "（卡片上未安装 Profile）\n";
      return 0;
    }
    for (const auto &p : profiles) {
      std::cout << "ICCID: " << (p.iccid.empty() ? "-" : p.iccid) << "\n";
      std::cout << "  状态:    " << p.state << "\n";
      std::cout << "  名称:    " << (p.name.empty() ? "-" : p.name) << "\n";
      std::cout << "  服务商:  " << (p.provider.empty() ? "-" : p.provider)
                << "\n";
      std::cout << "  昵称:    " << (p.nickname.empty() ? "-" : p.nickname)
                << "\n";
      std::cout << "  ISD-P:   " << (p.isdp_aid.empty() ? "-" : p.isdp_aid)
                << "\n";
    }
    return 0;
  }

  if (action == "switch" || action == "enable" || action == "disable" ||
      action == "delete") {
    if (args.size() < 2) {
      std::cerr << "用法: cellular esim " << action << " <iccid>\n";
      return 1;
    }
    auto aid = discover_aid(esim);
    if (!aid) {
      std::cerr << "未检测到兼容的 eUICC 卡片。\n";
      return 1;
    }
    const std::string &iccid = args[1];
    if (action == "switch") {
      auto profiles = esim.list_profiles(*aid);
      for (const auto &p : profiles) {
        if (p.iccid == iccid) {
          esim.enable_profile(*aid, p.iccid, true);
          std::cout << "已启用 Profile " << iccid << "\n";
        } else {
          esim.disable_profile(*aid, p.iccid, true);
        }
      }
      std::cout << "重启模组使新 Profile 生效...\n";
      modem::Modem modem(*ch);
      modem.reboot();
      std::cout << "完成。\n";
    } else if (action == "enable") {
      esim.enable_profile(*aid, iccid, true);
      std::cout << "已启用 Profile " << iccid << "\n";
    } else if (action == "disable") {
      esim.disable_profile(*aid, iccid, true);
      std::cout << "已禁用 Profile " << iccid << "\n";
    } else {
      esim.delete_profile(*aid, iccid);
      std::cout << "已删除 Profile " << iccid << "\n";
    }
    return 0;
  }

  if (action == "apdu") {
    if (args.size() < 3) {
      std::cerr << "用法: cellular esim apdu <aidhex> <apduhex>\n";
      return 1;
    }
    int channel = esim.open_channel(args[1]);
    auto apdu = esim::hex_to_bytes(args[2]);
    auto resp = esim.transmit_apdu(channel, apdu);
    std::cout << "SW: "
              << esim::bytes_to_hex({static_cast<uint8_t>(resp.sw >> 8),
                                     static_cast<uint8_t>(resp.sw & 0xFF)})
              << "  Data: " << esim::bytes_to_hex(resp.data) << "\n";
    esim.close_channel(channel);
    return 0;
  }

  std::cerr << "未知 esim 操作: " << action << "\n";
  return 1;
}

} // namespace

int main(int argc, char *argv[]) {
  argparse::ArgumentParser program("cellular", "0.1.0");
  program.add_description(
      "DJI 第一代 4G 模块 CLI 工具：数据上网模式 / 通话&短信模式");
  program.add_argument("-h", "--help")
      .help("显示帮助")
      .default_value(false)
      .implicit_value(true);

  argparse::ArgumentParser data_cmd("data");
  data_cmd.add_description("切换到数据上网模式并重启模块");
  program.add_subparser(data_cmd);

  argparse::ArgumentParser status_cmd("status");
  status_cmd.add_description("显示模块状态");
  program.add_subparser(status_cmd);

  argparse::ArgumentParser sms_cmd("sms");
  sms_cmd.add_description("在短信模式下执行短信操作");

  argparse::ArgumentParser sms_clear_cmd("clear");
  sms_clear_cmd.add_description("清空模块 ME 存储中的旧短信");
  sms_cmd.add_subparser(sms_clear_cmd);

  argparse::ArgumentParser sms_list_cmd("list");
  sms_list_cmd.add_description("列出已收短信");
  sms_cmd.add_subparser(sms_list_cmd);

  argparse::ArgumentParser sms_send_cmd("send");
  sms_send_cmd.add_description("发送短信");
  sms_send_cmd.add_argument("phone").help("接收号码（国际格式加 +）");
  sms_send_cmd.add_argument("message").help("短信内容").remaining();
  sms_cmd.add_subparser(sms_send_cmd);
  program.add_subparser(sms_cmd);

  argparse::ArgumentParser esim_cmd("esim");
  esim_cmd.add_description("eSIM 基础能力：EID / Profile 列表 / 切换");

  for (const std::string &action : {"eid", "list"}) {
    argparse::ArgumentParser command(action);
    command.add_description(action == "eid" ? "显示 EID" : "列出 Profile");
    esim_cmd.add_subparser(command);
  }

  for (const std::string &action : {"switch", "enable", "disable", "delete"}) {
    argparse::ArgumentParser command(action);
    command.add_description("操作指定 ICCID 的 Profile");
    command.add_argument("iccid").help("Profile ICCID");
    esim_cmd.add_subparser(command);
  }

  argparse::ArgumentParser apdu_command("apdu");
  apdu_command.add_description("发送 APDU 指令");
  apdu_command.add_argument("aid").help("ISD-R AID（十六进制）");
  apdu_command.add_argument("command").help("APDU（十六进制）");
  esim_cmd.add_subparser(apdu_command);
  program.add_subparser(esim_cmd);

  try {
    program.parse_args(argc, argv);
  } catch (const std::exception &err) {
    if (program["--help"] == true) {
      std::cout << program;
      return 0;
    }
    std::cerr << err.what() << "\n\n";
    std::cerr << program;
    return 1;
  }

  if (program["--help"] == true) {
    std::cout << program;
    return 0;
  }

  if (program.is_subcommand_used("data"))
    return cmd_data();
  if (program.is_subcommand_used("status"))
    return cmd_status();
  if (program.is_subcommand_used("sms")) {
    auto &sms = program.at<argparse::ArgumentParser>("sms");
    if (sms.is_subcommand_used("send")) {
      auto &sub = sms.at<argparse::ArgumentParser>("send");
      std::string phone = sub.get<std::string>("phone");
      auto parts = sub.present<std::vector<std::string>>("message").value_or(
          std::vector<std::string>{});
      if (parts.empty()) {
        std::cerr << "用法: cellular sms send <号码> <内容>\n";
        return 1;
      }
      return cmd_sms_send(phone, join(parts, " "));
    }
    if (sms.is_subcommand_used("list"))
      return cmd_sms_list();
    if (sms.is_subcommand_used("clear"))
      return cmd_sms_clear();
    return cmd_sms();
  }
  /*
  if (program.is_subcommand_used("sms-send")) {
    auto &sub = program.at<argparse::ArgumentParser>("sms-send");
    std::string phone = sub.get<std::string>("phone");
    auto parts = sub.present<std::vector<std::string>>("message").value_or(
        std::vector<std::string>{});
    if (phone.empty() || parts.empty()) {
      std::cerr << "用法: cellular sms-send <号码> <内容>\n";
      return 1;
    }
    return cmd_sms_send(phone, join(parts, " "));
  }
  if (program.is_subcommand_used("sms-list"))
    return cmd_sms_list();
  if (program.is_subcommand_used("sms-clear-me"))
    return cmd_sms_clear();
  */
  if (program.is_subcommand_used("esim")) {
    auto &group = program.at<argparse::ArgumentParser>("esim");
    std::vector<std::string> args;
    for (const std::string &action : {"eid", "list"}) {
      if (group.is_subcommand_used(action)) {
        args.push_back(action);
        return cmd_esim(args);
      }
    }
    for (const std::string &action :
         {"switch", "enable", "disable", "delete"}) {
      if (group.is_subcommand_used(action)) {
        auto &sub = group.at<argparse::ArgumentParser>(action);
        args.push_back(action);
        args.push_back(sub.get<std::string>("iccid"));
        return cmd_esim(args);
      }
    }
    if (group.is_subcommand_used("apdu")) {
      auto &sub = group.at<argparse::ArgumentParser>("apdu");
      args.push_back("apdu");
      args.push_back(sub.get<std::string>("aid"));
      args.push_back(sub.get<std::string>("command"));
      return cmd_esim(args);
    }
    std::cerr
        << "用法: cellular esim <eid|list|switch|enable|disable|delete|apdu>\n";
    return 1;
  }

  std::cout << program;
  return 0;
}
