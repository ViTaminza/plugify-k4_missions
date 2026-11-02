#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: core)

namespace vip {

  using _VIP_IsVIPLoaded = bool (*)();
}
extern "C" PLUGIN_API vip::_VIP_IsVIPLoaded __vip_VIP_IsVIPLoaded;
namespace vip {
  /**
   * @brief True once the database connection is up and the vip_users table exists (the bReady flag the original set after its CREATE TABLE ran). Modules that were loaded late poll this instead of waiting for VIP_OnVIPLoaded.
   * @return bool: Whether the VIP core finished loading.
   */
  inline bool VIP_IsVIPLoaded() {
    return __vip_VIP_IsVIPLoaded();
  }

  using _VIP_GetTranslate = plg::string (*)(const plg::string&);
}
extern "C" PLUGIN_API vip::_VIP_GetTranslate __vip_VIP_GetTranslate;
namespace vip {
  /**
   * @brief Phrase from vip.phrases.txt (the g_vecPhrases map of the original). When the key is unknown an empty string is returned, exactly like the original's map lookup. Like the original, the phrase table is built once at plugin start (vip_reload only refreshes groups.ini), so a plugin may cache the value for the session.
   * @param phrase (string): Phrase key.
   * @return string: Translated phrase (empty when the key is missing).
   */
  inline plg::string VIP_GetTranslate(const plg::string& phrase) {
    return __vip_VIP_GetTranslate(phrase);
  }

} // namespace vip
