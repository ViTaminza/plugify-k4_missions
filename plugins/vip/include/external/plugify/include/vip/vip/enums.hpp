#pragma once

#include <cstdint>
// Generated from vip.pplugin

namespace vip {

  // VIP_ValueType of the Pisex IVIPApi: the type of a VIP feature value.
  enum class ValueType : int32_t {
    VIP_NULL = 0, // No data: the feature produces no menu entry.
    VIP_INT = 1, // Integer value.
    VIP_FLOAT = 2, // Floating point value.
    VIP_BOOL = 3, // Boolean value; the menu prints On/Off.
    VIP_STRING = 4, // String value.
  };

  // VIP_FeatureType of the Pisex IVIPApi: how a feature appears in the VIP menu.
  enum class FeatureType : int32_t {
    TOGGLABLE = 0, // The player can switch it on and off; the state is stored in the '<group>.<feature>' cookie.
    SELECTABLE = 1, // Pressing it runs an action (opens a submenu); it has no state.
    HIDE = 2, // Registered but hidden from the VIP menu.
  };

  // VIP_ToggleState of the Pisex IVIPApi: the state a togglable feature passes to its callback.
  enum class ToggleState : int32_t {
    DISABLED = 0, // The feature is switched off.
    ENABLED = 1, // The feature is switched on.
    NO_ACCESS = 2, // Unused by the core; kept for parity with Pisex.
  };


  /// Ownership type for RAII wrappers
  enum class Ownership : bool { Borrowed, Owned };

} // namespace vip
