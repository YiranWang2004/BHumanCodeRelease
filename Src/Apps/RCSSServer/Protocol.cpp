#include "Protocol.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace rcss
{
const Expression* Expression::find(std::string_view name) const
{
  for(const auto& child : children)
    if(!child.children.empty() && child.tag() == name) return &child;
  return nullptr;
}
std::string Expression::text(size_t i) const { return i < children.size() ? children[i].atom : ""; }
double Expression::number(size_t i, double fallback) const
{
  const auto value = text(i);
  char* end = nullptr;
  const double result = std::strtod(value.c_str(), &end);
  return !value.empty() && end == value.c_str() + value.size() && std::isfinite(result) ? result : fallback;
}
std::vector<Expression> parse(std::string_view text)
{
  size_t pos = 0;
  const auto skip = [&] { while(pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos; };
  const auto read = [&](auto&& self, unsigned depth) -> Expression
  {
    if(depth > 32) throw std::runtime_error("S-expression nesting exceeds limit");
    skip();
    Expression node;
    if(pos == text.size()) throw std::runtime_error("Truncated S-expression");
    if(text[pos] == '(')
    {
      ++pos;
      for(;;)
      {
        skip();
        if(pos == text.size()) throw std::runtime_error("Unclosed S-expression");
        if(text[pos] == ')') { ++pos; break; }
        node.children.push_back(self(self, depth + 1));
      }
    }
    else
    {
      const auto start = pos;
      while(pos < text.size() && text[pos] != '(' && text[pos] != ')' && !std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
      if(pos == start) throw std::runtime_error("Invalid S-expression token");
      node.atom = std::string(text.substr(start, pos - start));
    }
    return node;
  };
  std::vector<Expression> result;
  skip();
  while(pos < text.size()) { result.push_back(read(read, 0)); skip(); }
  return result;
}

Frame decode(std::string_view message)
{
  constexpr double radians = 0.017453292519943295;
  Frame frame;
  const auto object = [](const Expression& node)
  {
    Object o;
    o.type = node.tag();
    if(const auto* p = node.find("pol"))
    { o.distance = p->number(1); o.azimuth = p->number(2); o.elevation = p->number(3); }
    return o;
  };
  for(const auto& node : parse(message))
  {
    const auto name = node.tag();
    const auto number = [&](std::string_view key, double fallback = 0.)
    { const auto* child = node.find(key); return child ? child->number(1, fallback) : fallback; };
    const auto text = [&](std::string_view key)
    { const auto* child = node.find(key); return child ? child->text() : ""; };
    if(name == "time") frame.time = number("now", -1.);
    else if(name == "GS")
    {
      frame.phase = static_cast<int>(number("ph"));
      frame.playTime = number("t"); frame.mode = text("pm");
      frame.leftTeam = text("tl"); frame.rightTeam = text("tr");
      frame.leftScore = static_cast<int>(number("sl")); frame.rightScore = static_cast<int>(number("sr"));
      frame.kickoffTeam = static_cast<int>(number("ko", -1.)); frame.stopped = number("st") != 0.;
    }
    else if(name == "Pen") { frame.penalized = number("p") != 0.; frame.penaltySeconds = number("s"); }
    else if(name == "pos" && node.find("p"))
    {
      frame.hasPosition = true;
      for(size_t i = 0; i < 3; ++i) frame.position[i] = node.find("p")->number(i + 1);
    }
    else if(name == "quat" && node.find("q"))
    {
      frame.hasOrientation = true;
      for(size_t i = 0; i < 4; ++i) frame.quaternion[i] = node.find("q")->number(i + 1);
    }
    else if(name == "GYR" && node.find("rt"))
      for(size_t i = 0; i < 3; ++i) frame.gyro[i] = node.find("rt")->number(i + 1) * radians;
    else if(name == "ACC" && node.find("a"))
      for(size_t i = 0; i < 3; ++i) frame.acceleration[i] = node.find("a")->number(i + 1);
    else if(name == "HJ")
      frame.joints.push_back({text("n"), number("ax") * radians, number("vx") * radians, number("tau"), node.find("tau") != nullptr});
    else if(name == "See")
    {
      frame.hasVision = true;
      for(const auto& child : node.children)
        if(child.find("team"))
        {
          Player player;
          player.team = child.find("team")->text();
          if(child.find("id")) player.number = static_cast<int>(child.find("id")->number());
          for(const auto& part : child.children) if(part.find("pol")) player.parts.push_back(object(part));
          frame.players.push_back(std::move(player));
        }
        else if(child.find("pol")) frame.objects.push_back(object(child));
    }
    else if(name == "error" || name == "Error") throw std::runtime_error("Server rejected agent: " + std::string(message));
  }
  return frame;
}

Connection::~Connection() { close(); }
void Connection::connect(const std::string& host, int port)
{
  addrinfo hints{}, *addresses = nullptr;
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
  if(getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses)) throw std::runtime_error("Cannot resolve server host " + host);
  for(auto* address = addresses; address; address = address->ai_next)
  {
    socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if(socket < 0) continue;
    if(::connect(socket, address->ai_addr, address->ai_addrlen) == 0) break;
    ::close(socket); socket = -1;
  }
  freeaddrinfo(addresses);
  if(socket < 0) throw std::runtime_error("Cannot connect to " + host + ":" + std::to_string(port));
  const int one = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  const timeval timeout{10, 0};
  setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}
bool Connection::transfer(void* buffer, size_t length, bool sending)
{
  auto* data = static_cast<char*>(buffer);
  while(length)
  {
    const auto count = sending ? ::send(socket, data, length, MSG_NOSIGNAL) : ::recv(socket, data, length, 0);
    if(count < 0 && errno == EINTR) continue;
    if(count <= 0) return false;
    data += count; length -= static_cast<size_t>(count);
  }
  return true;
}
void Connection::send(const std::string& message)
{
  auto size = htonl(static_cast<uint32_t>(message.size()));
  if(!transfer(&size, sizeof(size), true) || !transfer(const_cast<char*>(message.data()), message.size(), true)) throw std::runtime_error("Server send failed");
}
std::string Connection::receive()
{
  uint32_t size;
  if(!transfer(&size, sizeof(size), false)) throw std::runtime_error("Server disconnected or timed out");
  size = ntohl(size);
  if(size > 4 * 1024 * 1024) throw std::runtime_error("Server message exceeds 4 MiB");
  std::string result(size, '\0');
  if(!transfer(result.data(), size, false)) throw std::runtime_error("Truncated server message");
  return result;
}
void Connection::close() { if(socket >= 0) { ::shutdown(socket, SHUT_RDWR); ::close(socket); socket = -1; } }
}
