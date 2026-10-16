#include "Model.h"
#include <SFML/Graphics.hpp>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace
{
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
std::string number(double n, int precision = 1)
{
  std::ostringstream out; out << std::fixed << std::setprecision(precision) << n; return out.str();
}
std::string clockText(double seconds)
{
  const auto s = static_cast<unsigned>(std::max(0., std::min(seconds, 1e8)));
  std::ostringstream out; out << s / 60 << ':' << std::setw(2) << std::setfill('0') << s % 60; return out.str();
}
struct Options
{
  std::string listen = "0.0.0.0", team, font, summary, screenshot;
  int port = 10190, frames = 0;
  double duration = 0;
  bool headless = false;
};
Options options(int argc, char** argv)
{
  Options o;
  for(int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if(arg == "--help")
    {
      std::cout << "B-Human live desktop monitor (UDP, no replay or automatic recording)\n"
        "  --listen IPv4    bind address (default 0.0.0.0)\n"
        "  --port PORT      UDP port (default 10190)\n"
        "  --team NAME      show only this team name\n"
        "  --font PATH      TrueType/OpenType font supporting Chinese\n"
        "Diagnostics: --headless --duration SECONDS --summary FILE\n"
        "             --frames N --screenshot FILE\n";
      std::exit(0);
    }
    if(arg == "--headless") { o.headless = true; continue; }
    if(i + 1 == argc) throw std::runtime_error("Missing value for " + arg);
    const std::string value = argv[++i];
    const auto positive = [&]()
    {
      size_t used = 0; const double v = std::stod(value, &used);
      if(used != value.size() || !std::isfinite(v) || v <= 0) throw std::runtime_error("Invalid positive number: " + value);
      return v;
    };
    if(arg == "--listen") o.listen = value;
    else if(arg == "--team") o.team = value;
    else if(arg == "--font") o.font = value;
    else if(arg == "--summary") o.summary = value;
    else if(arg == "--screenshot") o.screenshot = value;
    else if(arg == "--duration") o.duration = positive();
    else if(arg == "--port" || arg == "--frames")
    {
      const double v = positive();
      if(v != std::floor(v) || v > (arg == "--port" ? 65535 : 10000000)) throw std::runtime_error("Invalid value for " + arg);
      (arg == "--port" ? o.port : o.frames) = static_cast<int>(v);
    }
    else throw std::runtime_error("Unknown option: " + arg);
  }
  if(o.headless && !o.screenshot.empty()) throw std::runtime_error("--screenshot requires a desktop window");
  return o;
}
class Receiver
{
  int fd = -1;
public:
  explicit Receiver(const Options& o)
  {
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(o.port);
    if(inet_pton(AF_INET, o.listen.c_str(), &address.sin_addr) != 1) throw std::runtime_error("--listen must be an IPv4 address");
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(fd < 0) throw std::runtime_error(std::strerror(errno));
    if(bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
    {
      const auto error = std::string(std::strerror(errno)); close(fd); fd = -1;
      throw std::runtime_error("Cannot bind UDP " + o.listen + ":" + std::to_string(o.port) + ": " + error);
    }
    const int buffer = 1024 * 1024; setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
  }
  ~Receiver() { if(fd >= 0) close(fd); }
  void drain(monitor::Store& store)
  {
    std::array<char, 65536> bytes;
    for(int i = 0; i < 256; ++i)
    {
      const auto count = recv(fd, bytes.data(), bytes.size(), MSG_DONTWAIT);
      if(count < 0)
      {
        if(errno == EAGAIN || errno == EWOULDBLOCK) break;
        if(errno == EINTR) continue;
        throw std::runtime_error(std::string("UDP receive: ") + std::strerror(errno));
      }
      store.ingest(std::string(bytes.data(), static_cast<size_t>(count)));
    }
  }
};

const sf::Color background(13, 20, 31), panel(23, 34, 48), selected(32, 61, 70), ink(231, 239, 246), muted(144, 165, 184), teal(64, 220, 182), amber(255, 193, 99), red(246, 116, 129);
sf::String utf8(const std::string& s) { return sf::String::fromUtf8(s.begin(), s.end()); }
struct Canvas
{
  sf::RenderWindow& window;
  sf::Font font;
  sf::Vector2f mouse;
  std::string tooltip;
  explicit Canvas(sf::RenderWindow& window, const std::string& requested) : window(window)
  {
    const std::vector<std::string> candidates = requested.empty() ? std::vector<std::string>{
      "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
      "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"} : std::vector<std::string>{requested};
    bool loaded = false;
    for(const auto& path : candidates) if(font.loadFromFile(path)) { loaded = true; break; }
    if(!loaded) throw std::runtime_error("Cannot load font; specify --font PATH");
  }
  void box(sf::FloatRect r, sf::Color color, sf::Color border = sf::Color::Transparent)
  {
    sf::RectangleShape shape({r.width, r.height}); shape.setPosition(r.left, r.top); shape.setFillColor(color);
    shape.setOutlineColor(border); shape.setOutlineThickness(border.a ? 1.f : 0.f); window.draw(shape);
  }
  void text(const std::string& value, float x, float y, unsigned size = 15, sf::Color color = ink, float width = 10000)
  {
    sf::Text t(utf8(value), font, size); t.setPosition(x, y - 4); t.setFillColor(color);
    if(t.getLocalBounds().width > width)
    {
      auto clipped = t.getString();
      while(clipped.getSize() && t.getLocalBounds().width > width)
      {
        clipped.erase(clipped.getSize() - 1); t.setString(clipped + utf8("…"));
      }
      if(sf::FloatRect(x, y, width, size + 8.f).contains(mouse)) tooltip = value;
    }
    window.draw(t);
  }
  void line(sf::Vector2f a, sf::Vector2f b, sf::Color color = muted)
  {
    const sf::Vertex vertices[] = {sf::Vertex(a, color), sf::Vertex(b, color)}; window.draw(vertices, 2, sf::Lines);
  }
  void circle(sf::Vector2f p, float radius, sf::Color color, sf::Color border = sf::Color::Transparent)
  {
    sf::CircleShape shape(radius, 48); shape.setOrigin(radius, radius); shape.setPosition(p); shape.setFillColor(color);
    shape.setOutlineColor(border); shape.setOutlineThickness(border.a ? 1.3f : 0.f); window.draw(shape);
  }
  void showTooltip()
  {
    if(tooltip.empty()) return;
    const auto full = utf8(tooltip);
    std::vector<sf::String> rows(1);
    for(size_t i = 0; i < full.getSize(); ++i)
    {
      rows.back() += full[i];
      if(sf::Text(rows.back(), font, 15).getLocalBounds().width > 740) rows.emplace_back();
    }
    const float h = 20.f * rows.size() + 18, x = std::min(mouse.x + 14, 810.f), y = std::max(10.f, std::min(mouse.y + 22, 984.f - h));
    box({x, y, 770, h}, sf::Color(38, 53, 68), muted);
    for(size_t i = 0; i < rows.size(); ++i)
    {
      const auto bytes = rows[i].toUtf8();
      text(std::string(bytes.begin(), bytes.end()), x + 10, y + 8 + 20 * i);
    }
  }
};

struct UI
{
  std::string selectedTeam;
  int player = 1, treeScroll = 0, eventScroll = 0;
  const sf::FloatRect previousTeam{24, 76, 34, 32}, nextTeam{980, 76, 34, 32};
  const sf::FloatRect fieldPanel{338, 130, 724, 548}, eventsPanel{338, 692, 724, 264}, treePanel{1092, 514, 470, 418};
  std::array<sf::FloatRect, 7> cards;
  std::vector<std::pair<int, sf::Vector2f>> fieldRobots;
  UI() { for(int i = 0; i < 7; ++i) cards[i] = {22, 166.f + 113.f * i, 300, 102}; }
  std::vector<std::string> keys(const monitor::Store& store, const Options& o) const
  {
    std::vector<std::string> result;
    for(const auto& t : store.teams) if(o.team.empty() || t.second.begin()->second.data["team"].asString() == o.team) result.push_back(t.first);
    return result;
  }
  void changeTeam(const std::vector<std::string>& teams, int direction)
  {
    if(teams.empty()) return;
    auto it = std::find(teams.begin(), teams.end(), selectedTeam);
    const auto index = it == teams.end() ? 0 : it - teams.begin();
    selectedTeam = teams[(index + direction + teams.size()) % teams.size()]; treeScroll = eventScroll = 0;
  }
  void select(int n) { if(player != n) { player = n; treeScroll = eventScroll = 0; } }
  void event(const sf::Event& e, sf::Vector2f p, const monitor::Store& store, const Options& o)
  {
    if(e.type == sf::Event::KeyPressed)
    {
      if(e.key.code >= sf::Keyboard::Num1 && e.key.code <= sf::Keyboard::Num7) select(1 + e.key.code - sf::Keyboard::Num1);
      if(e.key.code == sf::Keyboard::Tab) changeTeam(keys(store, o), e.key.shift ? -1 : 1);
    }
    if(e.type == sf::Event::MouseButtonPressed && e.mouseButton.button == sf::Mouse::Left)
    {
      for(int i = 0; i < 7; ++i) if(cards[i].contains(p)) select(i + 1);
      for(const auto& robot : fieldRobots) if(std::hypot(p.x - robot.second.x, p.y - robot.second.y) < 19) select(robot.first);
      if(previousTeam.contains(p)) changeTeam(keys(store, o), -1);
      if(nextTeam.contains(p)) changeTeam(keys(store, o), 1);
    }
    if(e.type == sf::Event::MouseWheelScrolled)
    {
      const int delta = static_cast<int>(-e.mouseWheelScroll.delta * 2);
      if(treePanel.contains(p)) treeScroll = std::max(0, treeScroll + delta);
      if(eventsPanel.contains(p)) eventScroll = std::max(0, eventScroll + delta);
    }
  }
  void draw(Canvas& c, const monitor::Store& store, const Options& o)
  {
    c.tooltip.clear(); c.window.clear(background);
    const auto teams = keys(store, o);
    if(!teams.empty() && std::find(teams.begin(), teams.end(), selectedTeam) == teams.end()) selectedTeam = teams.front();
    const std::map<int, monitor::Robot> empty;
    const auto found = store.teams.find(selectedTeam);
    const auto& robots = found == store.teams.end() ? empty : found->second;
    const auto chosen = robots.find(player);
    const monitor::Robot* robot = chosen == robots.end() ? nullptr : &chosen->second;
    unsigned fresh = 0;
    for(const auto& r : robots) if(r.second.age() <= .5) ++fresh;
    c.text("B-HUMAN", 24, 20, 27);
    c.text("实时决策监视", 195, 27, 19, muted);
    c.circle({1250, 36}, 4, fresh ? teal : amber);
    c.text(std::to_string(fresh) + "/7 状态新鲜  ·  UDP " + std::to_string(o.port), 1264, 24, 16, ink, 305);
    c.box({22, 68, 1040, 48}, panel);
    c.box(previousTeam, selected); c.text("‹", 33, 77, 21, teal);
    c.box(nextTeam, selected); c.text("›", 990, 77, 21, teal);
    c.text(robots.empty() ? "等待球队数据：run_player --monitor" : robots.begin()->second.data["team"].asString(), 74, 78, 18, ink, 450);
    c.text(robots.empty() ? "" : selectedTeam, 540, 82, 13, muted, 420);
    c.text("点击或 1–7 选机器人 · Tab 切换球队", 1092, 77, 15, muted, 480);
    c.text("球队总览", 24, 132, 17);
    c.text("接收 " + std::to_string(store.accepted) + "  无效 " + std::to_string(store.rejected), 142, 135, 12, muted, 178);
    for(int i = 0; i < 7; ++i)
    {
      const auto r = cards[i]; const auto it = robots.find(i + 1);
      const bool have = it != robots.end(); const auto* d = have ? &it->second.data : nullptr;
      const auto age = have ? it->second.age() : 999.;
      const sf::Color status = age <= .5 ? teal : age <= 2.5 ? amber : muted;
      c.box(r, player == i + 1 ? selected : panel, player == i + 1 ? teal : sf::Color::Transparent);
      c.text(std::to_string(i + 1), r.left + 12, r.top + 8, 24, status);
      c.text(have ? (*d)["robot"].asString() + " · " + (*d)["role"].asString() : "等待连接", r.left + 48, r.top + 10, 16, have ? ink : muted, 235);
      c.text(have ? monitor::primaryBehavior(*d) : "加 --monitor 后显示状态", r.left + 12, r.top + 39, 14, have ? ink : muted, 276);
      c.text(have ? "NN  " + ((*d)["policy_current"].asBool() ? (*d)["policy_kind"].asString() + " · " : "") + monitor::currentPolicy(*d) : "NN  --", r.left + 12, r.top + 62, 13, have && age <= 2.5 && (*d)["policy_current"].asBool() ? teal : muted, 276);
      c.circle({r.left + 17, r.top + 89}, 3, status);
      c.text(have ? "接收 " + number(age) + "s 前  ·  帧内球龄 " + number((*d)["ball_age"].asDouble()) + "s" : "尚无遥测", r.left + 28, r.top + 79, 12, status, 255);
    }
    drawField(c, robots, robot);
    drawEvents(c, robot);
    drawDetails(c, robot);
    c.text("仅实时 · 不自动保存    |    坐标统一为本队进攻 +X    |    >2.5s 未更新会变灰（也可能比赛暂停）", 24, 972, 13, muted, 1320);
    c.text(o.listen + ":" + std::to_string(o.port), 1360, 972, 12, muted, 218);
    c.showTooltip();
  }
  void drawField(Canvas& c, const std::map<int, monitor::Robot>& robots, const monitor::Robot* selectedRobot)
  {
    c.box(fieldPanel, panel);
    c.text("球场", 356, 147, 18);
    c.text("本队进攻 →", 920, 150, 14, teal);
    const Json::Value* geometry = selectedRobot ? &selectedRobot->data["field"] : robots.empty() ? nullptr : &robots.begin()->second.data["field"];
    const double length = geometry && geometry->isObject() ? (*geometry)["half_length"].asDouble() : 27.5;
    const double width = geometry && geometry->isObject() ? (*geometry)["half_width"].asDouble() : 18.;
    const float scale = std::min(650. / (2 * length + 6), 431. / (2 * width + 6));
    const sf::Vector2f center{700, 413};
    const auto point = [&](double x, double y)
    {
      return sf::Vector2f(center.x + static_cast<float>(std::clamp(x * scale, -346., 346.)), center.y - static_cast<float>(std::clamp(y * scale, -222., 222.)));
    };
    c.box({center.x - static_cast<float>((length + 2) * scale), center.y - static_cast<float>((width + 2) * scale),
      static_cast<float>(2 * (length + 2) * scale), static_cast<float>(2 * (width + 2) * scale)}, sf::Color(22, 62, 55));
    const sf::Color white(160, 193, 182);
    if(geometry && (*geometry)["lines"].isArray())
      for(const auto& l : (*geometry)["lines"]) c.line(point(l[0].asDouble(), l[1].asDouble()), point(l[2].asDouble(), l[3].asDouble()), white);
    else
    {
      c.line(point(-length, -width), point(length, -width), white); c.line(point(-length, width), point(length, width), white);
      c.line(point(-length, -width), point(-length, width), white); c.line(point(length, -width), point(length, width), white);
      c.line(point(0, -width), point(0, width), white);
      c.text("等待客户端发送实际场地尺寸", 569, 425, 14, muted);
    }
    fieldRobots.clear();
    if(selectedRobot)
    {
      const auto& d = selectedRobot->data;
      const bool fresh = selectedRobot->age() <= 2.5;
      for(const auto& p : d["opponents"]) c.circle(point(p["x"].asDouble(), p["y"].asDouble()), 6, fresh ? red : muted);
      if(d["ball_age"].asDouble() < 5)
      {
        const double yaw = d["yaw"].asDouble(), bx = d["ball_x"].asDouble(), by = d["ball_y"].asDouble();
        const auto b = point(d["x"].asDouble() + std::cos(yaw) * bx - std::sin(yaw) * by, d["y"].asDouble() + std::sin(yaw) * bx + std::cos(yaw) * by);
        c.line(point(d["x"].asDouble(), d["y"].asDouble()), b, sf::Color(145, 124, 70));
        c.circle(b, 5, fresh ? amber : muted, ink);
      }
    }
    for(const auto& r : robots)
    {
      const auto& d = r.second.data; const auto p = point(d["x"].asDouble(), d["y"].asDouble());
      const auto color = r.second.age() > 2.5 ? muted : d["penalized"].asBool() ? amber : teal;
      if(r.first == player) c.circle(p, 18, sf::Color::Transparent, color);
      c.circle(p, 12, color);
      c.text(std::to_string(r.first), p.x - 4, p.y - 10, 15, background);
      const double yaw = d["yaw"].asDouble();
      c.line({p.x + 12 * static_cast<float>(std::cos(yaw)), p.y - 12 * static_cast<float>(std::sin(yaw))},
        {p.x + 25 * static_cast<float>(std::cos(yaw)), p.y - 25 * static_cast<float>(std::sin(yaw))}, color);
      fieldRobots.emplace_back(r.first, p);
    }
    c.text(number(length * 2, 0) + " × " + number(width * 2, 0) + " m · 本队各自定位", 356, 638, 13, muted, 300);
    c.text("橙：选中机球估计   红：选中机对手列表", 666, 638, 13, muted, 380);
  }
  void drawEvents(Canvas& c, const monitor::Robot* robot)
  {
    c.box(eventsPanel, panel);
    c.text("#" + std::to_string(player) + " 切换时间线", 356, 708, 18);
    c.text("角色 / 行为 / 本帧 NN · 仅保留最近 200 次", 584, 713, 13, muted, 450);
    if(!robot) { c.text("等待实时状态", 356, 756, 15, muted); return; }
    eventScroll = std::clamp(eventScroll, 0, std::max(0, static_cast<int>(robot->events.size()) - 4));
    for(int row = 0; row < 4 && row + eventScroll < static_cast<int>(robot->events.size()); ++row)
    {
      const auto& e = robot->events[row + eventScroll]; const float y = 746.f + 48 * row;
      c.circle({362, y + 10}, 3, row ? muted : teal);
      c.text("play " + clockText(e.playTime) + "  ·  sim " + number(e.time, 2) + "s", 376, y, 12, muted, 660);
      c.text(e.text, 376, y + 19, 14, row ? ink : teal, 663);
    }
    if(robot->events.size() > 4) c.text("在此滚轮查看较早变化", 877, 933, 11, muted, 170);
  }
  void drawDetails(Canvas& c, const monitor::Robot* robot)
  {
    c.box({1078, 130, 500, 826}, panel);
    c.text("#" + std::to_string(player) + "  单机详情", 1094, 147, 18);
    if(!robot) { c.text("等待客户端连接", 1094, 197, 16, muted); return; }
    const auto& d = robot->data;
    const bool stale = robot->age() > 2.5;
    c.text(stale ? "未更新 " + number(robot->age()) + "s" : "接收 " + number(robot->age(), 2) + "s 前", 1380, 151, 13, robot->age() > .5 ? amber : teal, 180);
    const auto row = [&](const std::string& label, const std::string& value, float y, sf::Color color = ink)
    {
      c.text(label, 1094, y, 13, muted, 107); c.text(value, 1206, y, 14, color, 353);
    };
    row("比赛 / 计时", d["mode"].asString() + " · " + clockText(d["play_time"].asDouble()), 187);
    row("角色 / 站位", d["role"].asString() + " / " + d["position"].asString(), 214);
    row("战术 / 定式", d["tactic"].asString() + " / " + d["set_play"].asString(), 241);
    row("Skill 请求", d["skill"].asString(), 268);
    row("Motion 请求", d["motion"].asString(), 295);
    row(stale ? "末帧 Phase" : "执行 Phase", d["phase"].asString() + (d["penalized"].asBool() ? " · 受罚" : ""), 322, stale ? muted : teal);
    row(stale ? "末帧推理网络" : "本帧推理网络", d["policy_current"].asBool() ? d["policy_kind"].asString() + " · " + d["policy"].asString() : "无新推理采样", 349, !stale && d["policy_current"].asBool() ? teal : muted);
    row("最近网络 / 间隔", d["policy"].asString() + " / " + number(d["policy_age"].asDouble(), 2) + "s", 376);
    row("推理 W / K / Up", d["walk_inferences"].asString() + " / " + d["kick_inferences"].asString() + " / " + d["get_up_inferences"].asString(), 403);
    row("定位 / 见球间隔", d.get("localization", "--").asString() + " / " + number(d["ball_age"].asDouble()) + "s", 430);
    row("位置 x,y / 队信", number(d["x"].asDouble()) + ", " + number(d["y"].asDouble()) + "m · TX " + d["team_sent"].asString() + " RX " + d["team_received"].asString(), 457);
    c.line({1094, 489}, {1560, 489}, sf::Color(53, 72, 89));
    c.text("完整活动行为树", 1094, 498, 17);
    c.text("滚轮查看 · 悬停展开长文本", 1358, 502, 12, muted, 203);
    const auto& graph = d["graph"];
    const int visible = 5;
    treeScroll = std::clamp(treeScroll, 0, std::max(0, static_cast<int>(graph.size()) - visible));
    if(graph.empty()) c.text("此帧没有活动节点", 1094, 543, 15, muted);
    for(int i = 0; i < visible && i + treeScroll < static_cast<int>(graph.size()); ++i)
    {
      const auto& n = graph[i + treeScroll]; const float y = 535.f + 78.f * i;
      const float indent = std::min(100, n["depth"].asInt() * 12);
      c.text(std::to_string(n["depth"].asInt()), 1094, y, 11, muted);
      c.line({1116 + indent, y + 9}, {1122 + indent, y + 9}, teal);
      c.text(n["option"].asString(), 1128 + indent, y, 15, ink, 430 - indent);
      c.text(n["state"].asString() + "  [state " + number(n["state_ms"].asDouble() / 1000., 2) + "s / option " + number(n["option_ms"].asDouble() / 1000., 2) + "s]", 1128 + indent, y + 24, 13, teal, 430 - indent);
      std::string args;
      for(const auto& a : n["arguments"]) { if(!args.empty()) args += ", "; args += a.asString(); }
      c.text(args.empty() ? "参数：--" : args, 1128 + indent, y + 46, 12, muted, 430 - indent);
    }
    c.text("节点 " + std::to_string(graph.empty() ? 0 : treeScroll + 1) + "–" + std::to_string(std::min(static_cast<int>(graph.size()), treeScroll + visible)) + " / " + std::to_string(graph.size()) + " · 所有活动节点均可滚动查看", 1094, 934, 12, muted, 463);
  }
};
void writeSummary(const monitor::Store& store, const Options& o)
{
  if(o.summary.empty()) return;
  std::ofstream out(o.summary); if(!out) throw std::runtime_error("Cannot write summary " + o.summary);
  out << store.summary() << '\n';
}
}
int main(int argc, char** argv)
{
  try
  {
    const auto o = options(argc, argv);
    Receiver receiver(o); monitor::Store store;
    std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
    const auto start = monitor::Clock::now();
    const auto expired = [&] { return o.duration > 0 && std::chrono::duration<double>(monitor::Clock::now() - start).count() >= o.duration; };
    std::cout << "B-Human monitor listening on UDP " << o.listen << ':' << o.port << std::endl;
    if(o.headless)
    {
      while(!stopped && !expired()) { receiver.drain(store); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    else
    {
      sf::RenderWindow window(sf::VideoMode(1600, 1000), "B-Human | Live Decision Monitor");
      window.setFramerateLimit(30);
      Canvas canvas(window, o.font); UI ui; int frames = 0;
      while(window.isOpen() && !stopped && !expired())
      {
        receiver.drain(store);
        sf::View view(sf::FloatRect(0, 0, 1600, 1000));
        const auto size = window.getSize();
        const float ratio = static_cast<float>(std::max(1u, size.x)) / std::max(1u, size.y);
        if(ratio > 1.6f) view.setViewport({(1 - 1.6f / ratio) / 2, 0, 1.6f / ratio, 1});
        else view.setViewport({0, (1 - ratio / 1.6f) / 2, 1, ratio / 1.6f});
        window.setView(view);
        sf::Event e;
        while(window.pollEvent(e))
        {
          if(e.type == sf::Event::Closed || (e.type == sf::Event::KeyPressed && e.key.code == sf::Keyboard::Escape)) { window.close(); break; }
          sf::Vector2i p = sf::Mouse::getPosition(window);
          if(e.type == sf::Event::MouseButtonPressed) p = {e.mouseButton.x, e.mouseButton.y};
          if(e.type == sf::Event::MouseWheelScrolled) p = {e.mouseWheelScroll.x, e.mouseWheelScroll.y};
          ui.event(e, window.mapPixelToCoords(p), store, o);
        }
        if(!window.isOpen()) break;
        canvas.mouse = window.mapPixelToCoords(sf::Mouse::getPosition(window));
        ui.draw(canvas, store, o);
        if(o.frames && frames + 1 >= o.frames && !o.screenshot.empty())
        {
          sf::Texture texture; texture.create(size.x, size.y); texture.update(window);
          if(!texture.copyToImage().saveToFile(o.screenshot)) throw std::runtime_error("Cannot save screenshot");
        }
        window.display();
        if(o.frames && ++frames >= o.frames) window.close();
      }
    }
    writeSummary(store, o);
    return 0;
  }
  catch(const std::exception& e) { std::cerr << "B-Human monitor: " << e.what() << '\n'; return 1; }
}
