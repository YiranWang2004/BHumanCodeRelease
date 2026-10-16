#pragma once
#include <chrono>
#include <string>

namespace rcss
{
struct Runtime;
std::string monitorSnapshot(const Runtime& runtime);

// Optional, non-blocking diagnostics. Uses an independent UDP destination.
class MonitorSender
{
  int socket = -1;
  std::chrono::steady_clock::time_point lastSend;
public:
  MonitorSender(const std::string& host, int port, bool enabled);
  ~MonitorSender();
  MonitorSender(const MonitorSender&) = delete;
  MonitorSender& operator=(const MonitorSender&) = delete;
  void update(const Runtime& runtime);
};
}
