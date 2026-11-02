#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: features)

namespace vip {

  using _VIP_GetClientFeatureInt = int32_t (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_GetClientFeatureInt __vip_VIP_GetClientFeatureInt;
namespace vip {
  /**
   * @brief Integer value of a feature of the player's group, honouring the '<group>.<feature>' toggle cookie: -1 when the player is not a VIP, the feature is missing, or the toggle is off (the original returned -1 in all those cases).
   * @param slot (int32): Player slot.
   * @param feature (string): Feature name, e.g. 'jumps'.
   * @return int32: Feature value or -1.
   */
  inline int32_t VIP_GetClientFeatureInt(int32_t slot, const plg::string& feature) {
    return __vip_VIP_GetClientFeatureInt(slot, feature);
  }

  using _VIP_GetClientFeatureBool = bool (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_GetClientFeatureBool __vip_VIP_GetClientFeatureBool;
namespace vip {
  /**
   * @brief Boolean value of a feature (the group value parsed with atoi, so '1' is true). false when the player is not a VIP, the feature is missing or the toggle is off.
   * @param slot (int32): Player slot.
   * @param feature (string): Feature name.
   * @return bool: Feature value.
   */
  inline bool VIP_GetClientFeatureBool(int32_t slot, const plg::string& feature) {
    return __vip_VIP_GetClientFeatureBool(slot, feature);
  }

  using _VIP_GetClientFeatureFloat = float (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_GetClientFeatureFloat __vip_VIP_GetClientFeatureFloat;
namespace vip {
  /**
   * @brief Floating point value of a feature (the group value parsed with atof). 1.0 when the player is not a VIP, the feature is missing or the toggle is off.
   * @param slot (int32): Player slot.
   * @param feature (string): Feature name.
   * @return float: Feature value or 1.0.
   */
  inline float VIP_GetClientFeatureFloat(int32_t slot, const plg::string& feature) {
    return __vip_VIP_GetClientFeatureFloat(slot, feature);
  }

  using _VIP_GetClientFeatureString = plg::string (*)(int32_t, const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_GetClientFeatureString __vip_VIP_GetClientFeatureString;
namespace vip {
  /**
   * @brief String value of a feature. Empty when the player is not a VIP, the feature is missing or the toggle is off. The original returned a pointer into a thread-local buffer because a local map copy had dangled; returning the value itself makes that class of bug impossible.
   * @param slot (int32): Player slot.
   * @param feature (string): Feature name.
   * @return string: Feature value or an empty string.
   */
  inline plg::string VIP_GetClientFeatureString(int32_t slot, const plg::string& feature) {
    return __vip_VIP_GetClientFeatureString(slot, feature);
  }

  using _VIP_RegisterFeature = void (*)(const plg::string&, ValueType, FeatureType, ItemSelectableDelegate, ItemTogglableDelegate, ItemDisplayDelegate);
}
extern "C" PLUGIN_API vip::_VIP_RegisterFeature __vip_VIP_RegisterFeature;
namespace vip {
  /**
   * @brief VIP_RegisterFeature() of the original: registers (or replaces) a feature of the VIP menu. The core keys it by feature name; the VIP menu only shows features with valueType != VIP_NULL and featureType != HIDE that appear in the player's group. Any callback may be null.
   * @param feature (string): Feature name, the key used in groups.ini and in the cookies.
   * @param valueType (int32): Type of the group value.
   * @param featureType (int32): Menu behaviour.
   * @param select (function): SELECTABLE callback, may be null.
   * @param toggle (function): TOGGLABLE callback, may be null.
   * @param display (function): Display callback, may be null.
   */
  inline void VIP_RegisterFeature(const plg::string& feature, ValueType valueType, FeatureType featureType, ItemSelectableDelegate select, ItemTogglableDelegate toggle, ItemDisplayDelegate display) {
    return __vip_VIP_RegisterFeature(feature, valueType, featureType, select, toggle, display);
  }

} // namespace vip
