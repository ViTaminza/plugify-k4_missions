#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: output)

namespace vip {

  using _VIP_PrintToCenter = void (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_PrintToCenter __vip_VIP_PrintToCenter;
namespace vip {
  /**
   * @brief PrintToCenter of the original (the utils PrintToCenter). The variadic printf of the original cannot cross the boundary, so the caller formats first - the consumer wrapper keeps the variadic signature and does exactly that.
   * @param slot (int32): Player slot.
   * @param msg (string): Message, already formatted.
   */
  inline void VIP_PrintToCenter(int32_t slot, const plg::string& msg) {
    return __vip_VIP_PrintToCenter(slot, msg);
  }

} // namespace vip
