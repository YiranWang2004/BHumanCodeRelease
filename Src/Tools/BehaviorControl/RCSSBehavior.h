#pragma once

#ifdef BHUMAN_RCSS_SERVER
extern bool bhumanRCSSTruthMode();
#endif

namespace RCSSBehavior
{
  // Keep physical robots, SimRobot and measured localization on native behavior.
  inline bool truthMode()
  {
#ifdef BHUMAN_RCSS_SERVER
    return bhumanRCSSTruthMode();
#else
    return false;
#endif
  }
}
