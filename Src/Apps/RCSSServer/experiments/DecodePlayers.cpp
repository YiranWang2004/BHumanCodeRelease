// Offline experiment: exercise the production decoder on actual server packets.
#include "Protocol.h"
#include <iomanip>
#include <iostream>

int main()
{
  std::cout << std::setprecision(17);
  std::string line;
  while(std::getline(std::cin, line))
  {
    const auto frame = rcss::decode(line);
    bool found = false;
    for(const auto& player : frame.players)
      if(player.team == "RangeTarget" && !player.parts.empty())
      {
        const auto& p = player.parts.front();
        std::cout << p.type << ' ' << p.distance << ' ' << p.azimuth << ' ' << p.elevation << '\n';
        found = true;
        break;
      }
    if(!found) std::cout << "none\n";
  }
}
