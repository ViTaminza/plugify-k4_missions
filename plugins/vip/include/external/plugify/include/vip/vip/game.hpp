#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: game)

namespace vip {

  using _VIP_PistolRound = bool (*)();
}
extern "C" PLUGIN_API vip::_VIP_PistolRound __vip_VIP_PistolRound;
namespace vip {
  /**
   * @brief g_bPistolRound of the original: true on the first round, when the teams switch at the round reset and on a game restart (updated on round_prestart).
   * @return bool: Whether this is a pistol round.
   */
  inline bool VIP_PistolRound() {
    return __vip_VIP_PistolRound();
  }

  using _VIP_WarmupPeriod = bool (*)();
}
extern "C" PLUGIN_API vip::_VIP_WarmupPeriod __vip_VIP_WarmupPeriod;
namespace vip {
  /**
   * @brief m_bWarmupPeriod of the game rules.
   * @return bool: Whether the server is in warmup.
   */
  inline bool VIP_WarmupPeriod() {
    return __vip_VIP_WarmupPeriod();
  }

  using _VIP_GetTotalRounds = int32_t (*)();
}
extern "C" PLUGIN_API vip::_VIP_GetTotalRounds __vip_VIP_GetTotalRounds;
namespace vip {
  /**
   * @brief m_totalRoundsPlayed of the game rules (0 before the first game rules object exists).
   * @return int32: Rounds played.
   */
  inline int32_t VIP_GetTotalRounds() {
    return __vip_VIP_GetTotalRounds();
  }

} // namespace vip
