#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: cookies)

namespace vip {

  using _VIP_SetClientCookie = bool (*)(int32_t, const plg::string&, const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_SetClientCookie __vip_VIP_SetClientCookie;
namespace vip {
  /**
   * @brief Stores a per-player setting under the key '<group>.<name>' in the official permissions plugin (the ICookiesApi of the original, same keys). Returns false when the player is not a VIP or the permissions plugin is unavailable.
   * @param slot (int32): Player slot.
   * @param name (string): Cookie name (a feature name).
   * @param data (string): Value to store.
   * @return bool: Whether the cookie was stored.
   */
  inline bool VIP_SetClientCookie(int32_t slot, const plg::string& name, const plg::string& data) {
    return __vip_VIP_SetClientCookie(slot, name, data);
  }

  using _VIP_GetClientCookie = plg::string (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_GetClientCookie __vip_VIP_GetClientCookie;
namespace vip {
  /**
   * @brief Reads the '<group>.<name>' permission cookie of the player. Empty when it is unset, the player is not a VIP or the permissions plugin is unavailable.
   * @param slot (int32): Player slot.
   * @param name (string): Cookie name (a feature name).
   * @return string: Stored value or an empty string.
   */
  inline plg::string VIP_GetClientCookie(int32_t slot, const plg::string& name) {
    return __vip_VIP_GetClientCookie(slot, name);
  }

} // namespace vip
