#include "Protocol.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message)
{
  if(!condition) throw std::runtime_error(message);
}

int main()
{
  const auto frame = hsl::decode(
    "(time(now 1.24))(GS(pm KickOff_Right)(tl Home)(tr Away)(ko 1)(st 0))"
    "(Pen(p 1)(s 12))(HJ(n q_hj1)(ax 90)(vx -180)(tau 3.25))"
    "(See(P(pol 2 3 4))(P(team Away)(id 2)(head(pol 1 2 3))))");
  require(frame.time == 1.24 && frame.mode == "KickOff_Right" && frame.kickoffTeam == 1, "Game state decoding");
  require(frame.penalized && frame.penaltySeconds == 12, "Per-player penalty decoding");
  require(frame.joints.size() == 1 && frame.joints[0].hasTorque && frame.joints[0].tau == 3.25, "Joint torque decoding");
  require(std::abs(frame.joints[0].q - 1.5707963267948966) < 1e-9 && std::abs(frame.joints[0].dq + 3.141592653589793) < 1e-9, "Joint SI unit conversion");
  require(frame.objects.size() == 1 && frame.objects[0].type == "P", "Penalty marker must not be classified as a player");
  require(frame.players.size() == 1 && frame.players[0].team == "Away" && frame.players[0].number == 2, "Player identity decoding");
  require(!hsl::decode("(HJ(n q_hj1)(ax 0)(vx 0))").joints[0].hasTorque, "Legacy packet must not invent torque readings");
  for(const std::string& input : std::vector<std::string>{"(unclosed", ")", std::string(40, '(') + std::string(40, ')')})
  {
    bool rejected = false;
    try { hsl::parse(input); } catch(const std::runtime_error&) { rejected = true; }
    require(rejected, "Malformed or excessive nesting must be rejected");
  }
  std::cout << "HSL protocol checks passed\n";
}
