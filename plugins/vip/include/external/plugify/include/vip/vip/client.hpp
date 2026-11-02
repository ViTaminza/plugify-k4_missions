#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: client)

namespace vip {

  using _VIP_IsClientVIP = bool (*)(int32_t);
}
extern "C" PLUGIN_API vip::_VIP_IsClientVIP __vip_VIP_IsClientVIP;
namespace vip {
  /**
   * @brief VIP_IsClientVIP() of the original: the player must be a non-bot, fully authenticated client whose vip_users row exists, has not expired and names a group that exists in groups.ini.
   * @param slot (int32): Player slot.
   * @return bool: Whether the player currently is a VIP.
   */
  inline bool VIP_IsClientVIP(int32_t slot) {
    return __vip_VIP_IsClientVIP(slot);
  }

  using _VIP_GetClientAccessTime = int32_t (*)(int32_t);
}
extern "C" PLUGIN_API vip::_VIP_GetClientAccessTime __vip_VIP_GetClientAccessTime;
namespace vip {
  /**
   * @brief Expiry of the VIP status of a slot: absolute unix timestamp, 0 = permanent, -1 = the player is not a VIP (the original returned the stored int, which is -1 for 'no row').
   * @param slot (int32): Player slot.
   * @return int32: Expiry timestamp, 0 or -1.
   */
  inline int32_t VIP_GetClientAccessTime(int32_t slot) {
    return __vip_VIP_GetClientAccessTime(slot);
  }

  using _VIP_SetClientAccessTime = bool (*)(int32_t, int32_t, bool);
}
extern "C" PLUGIN_API vip::_VIP_SetClientAccessTime __vip_VIP_SetClientAccessTime;
namespace vip {
  /**
   * @brief VIP_SetClientAccessTime() of the original: overwrites the expiry of an already VIP player (in memory and, when inDB is true, in vip_users). Fails for a player without a VIP row.
   * @param slot (int32): Player slot.
   * @param time (int32): New absolute expiry timestamp, 0 = permanent.
   * @param inDB (bool): false = change the in-memory record only.
   * @return bool: Whether the change was applied.
   */
  inline bool VIP_SetClientAccessTime(int32_t slot, int32_t time, bool inDB) {
    return __vip_VIP_SetClientAccessTime(slot, time, inDB);
  }

  using _VIP_GetClientVIPGroup = plg::string (*)(int32_t);
}
extern "C" PLUGIN_API vip::_VIP_GetClientVIPGroup __vip_VIP_GetClientVIPGroup;
namespace vip {
  /**
   * @brief Name of the group the VIP row of a slot names (empty for a non-VIP).
   * @param slot (int32): Player slot.
   * @return string: Group name or an empty string.
   */
  inline plg::string VIP_GetClientVIPGroup(int32_t slot) {
    return __vip_VIP_GetClientVIPGroup(slot);
  }

  using _VIP_SetClientVIPGroup = bool (*)(int32_t, const plg::string&, bool);
}
extern "C" PLUGIN_API vip::_VIP_SetClientVIPGroup __vip_VIP_SetClientVIPGroup;
namespace vip {
  /**
   * @brief VIP_SetClientVIPGroup() of the original: switches an already VIP player to another group of groups.ini (in memory and, when inDB is true, in vip_users). Fails for a non-VIP, an expired row or an unknown group.
   * @param slot (int32): Player slot.
   * @param group (string): New group name.
   * @param inDB (bool): false = change the in-memory record only.
   * @return bool: Whether the change was applied.
   */
  inline bool VIP_SetClientVIPGroup(int32_t slot, const plg::string& group, bool inDB) {
    return __vip_VIP_SetClientVIPGroup(slot, group, inDB);
  }

  using _VIP_GiveClientVIP = bool (*)(int32_t, int32_t, const plg::string&, bool);
}
extern "C" PLUGIN_API vip::_VIP_GiveClientVIP __vip_VIP_GiveClientVIP;
namespace vip {
  /**
   * @brief VIP_GiveClientVIP() of the original: gives the player a VIP status of |time| seconds (0 = permanent) in group |group|. Fails when the player already is a VIP. When addToDB is true the vip_users row is INSERTed and the player gets the Welcome/WelcomePerm message.
   * @param slot (int32): Player slot.
   * @param time (int32): Lifetime in seconds from now, 0 = permanent.
   * @param group (string): Group name from groups.ini.
   * @param addToDB (bool): false = change the in-memory record only.
   * @return bool: Whether the VIP status was granted.
   */
  inline bool VIP_GiveClientVIP(int32_t slot, int32_t time, const plg::string& group, bool addToDB) {
    return __vip_VIP_GiveClientVIP(slot, time, group, addToDB);
  }

  using _VIP_RemoveClientVIP = bool (*)(int32_t, bool, bool);
}
extern "C" PLUGIN_API vip::_VIP_RemoveClientVIP __vip_VIP_RemoveClientVIP;
namespace vip {
  /**
   * @brief VIP_RemoveClientVIP() of the original: drops the VIP status of a player, optionally deletes the vip_users row and sends the VIPExpired1..3 messages. Fires VIP_OnVIPClientRemoved with reason 2 and VIP_OnClientDisconnect(slot, false).
   * @param slot (int32): Player slot.
   * @param notify (bool): false = send no chat messages.
   * @param inDB (bool): false = keep the vip_users row.
   * @return bool: Whether the VIP status was removed.
   */
  inline bool VIP_RemoveClientVIP(int32_t slot, bool notify, bool inDB) {
    return __vip_VIP_RemoveClientVIP(slot, notify, inDB);
  }

} // namespace vip
