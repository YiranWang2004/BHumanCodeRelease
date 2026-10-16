#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace rcss
{
struct Expression
{
  std::string atom;
  std::vector<Expression> children;
  const Expression* find(std::string_view tag) const;
  std::string text(size_t i = 1) const;
  double number(size_t i = 1, double fallback = 0.) const;
  std::string tag() const { return children.empty() ? atom : children.front().atom; }
};
std::vector<Expression> parse(std::string_view text);

struct Object
{
  std::string type;
  double distance = 0., azimuth = 0., elevation = 0.;
};
struct Player
{
  std::string team;
  int number = 0;
  std::vector<Object> parts;
};
struct Joint { std::string name; double q = 0., dq = 0., tau = 0.; bool hasTorque = false; };
struct Frame
{
  double time = -1., playTime = 0.;
  std::string mode, leftTeam, rightTeam;
  int phase = 0, leftScore = 0, rightScore = 0, kickoffTeam = -1;
  bool penalized = false, stopped = false, hasVision = false, hasPosition = false, hasOrientation = false;
  double penaltySeconds = 0.;
  std::array<double, 3> position{}, gyro{}, acceleration{};
  std::array<double, 4> quaternion{1., 0., 0., 0.};
  std::vector<Joint> joints;
  std::vector<Object> objects;
  std::vector<Player> players;
};
Frame decode(std::string_view message);

// Server framing: four-byte big-endian payload length and S-expressions.
// Owns one agent socket; close() also interrupts an in-flight receive.
class Connection
{
  int socket = -1;
  bool transfer(void* buffer, size_t length, bool sending);
public:
  ~Connection();
  Connection() = default;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  void connect(const std::string& host, int port);
  void send(const std::string& message);
  std::string receive();
  void close();
};
}
