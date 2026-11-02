#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: groups)

namespace vip {

  using _VIP_IsValidVIPGroup = bool (*)(const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_IsValidVIPGroup __vip_VIP_IsValidVIPGroup;
namespace vip {
  /**
   * @brief True when groups.ini contains the group (VIP_IsValidVIPGroup of the original).
   * @param group (string): Group name.
   * @return bool: Whether the group exists.
   */
  inline bool VIP_IsValidVIPGroup(const plg::string& group) {
    return __vip_VIP_IsValidVIPGroup(group);
  }

} // namespace vip
