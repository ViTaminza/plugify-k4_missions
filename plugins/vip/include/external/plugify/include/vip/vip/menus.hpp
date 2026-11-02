#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: menus)

namespace vip {

  using _VIP_OpenMenu = void (*)(int32_t);
}
extern "C" PLUGIN_API vip::_VIP_OpenMenu __vip_VIP_OpenMenu;
namespace vip {
  /**
   * @brief Opens the VIP menu of a player (the ShowVIPMenu of the original, bReopen = true). Does nothing for a player without access.
   * @param slot (int32): Player slot.
   */
  inline void VIP_OpenMenu(int32_t slot) {
    return __vip_VIP_OpenMenu(slot);
  }

} // namespace vip
