#pragma once

#include "enums.hpp"
#include "aliases.hpp"
#include "delegates.hpp"
#include <plugin_export.h>

// Generated from vip.pplugin (group: callbacks)

namespace vip {

  using _VIP_OnVIPLoaded_Register = void (*)(ReadyDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnVIPLoaded_Register __vip_VIP_OnVIPLoaded_Register;
namespace vip {
  /**
   * @brief Registers the (single) VIP_OnVIPLoaded listener of the calling plugin; registering again replaces it. Fires once when the database is up and the vip_users table exists.
   * @param callback (function): Function to invoke.
   */
  inline void VIP_OnVIPLoaded_Register(ReadyDelegate callback) {
    return __vip_VIP_OnVIPLoaded_Register(callback);
  }

  using _VIP_OnVIPLoaded_Unregister = void (*)(ReadyDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnVIPLoaded_Unregister __vip_VIP_OnVIPLoaded_Unregister;
namespace vip {
  /**
   * @brief Removes the VIP_OnVIPLoaded listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void VIP_OnVIPLoaded_Unregister(ReadyDelegate callback) {
    return __vip_VIP_OnVIPLoaded_Unregister(callback);
  }

  using _VIP_OnClientLoaded_Register = void (*)(ClientLoadedOrDisconnectDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnClientLoaded_Register __vip_VIP_OnClientLoaded_Register;
namespace vip {
  /**
   * @brief Registers the (single) VIP_OnClientLoaded listener of the calling plugin. Fires after the vip_users row of a player was read - for connect loads and for manual reloads - and again when the permissions user (with the cookies) has been loaded.
   * @param callback (function): Function to invoke.
   */
  inline void VIP_OnClientLoaded_Register(ClientLoadedOrDisconnectDelegate callback) {
    return __vip_VIP_OnClientLoaded_Register(callback);
  }

  using _VIP_OnClientLoaded_Unregister = void (*)(ClientLoadedOrDisconnectDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnClientLoaded_Unregister __vip_VIP_OnClientLoaded_Unregister;
namespace vip {
  /**
   * @brief Removes the VIP_OnClientLoaded listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void VIP_OnClientLoaded_Unregister(ClientLoadedOrDisconnectDelegate callback) {
    return __vip_VIP_OnClientLoaded_Unregister(callback);
  }

  using _VIP_OnClientDisconnect_Register = void (*)(ClientLoadedOrDisconnectDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnClientDisconnect_Register __vip_VIP_OnClientDisconnect_Register;
namespace vip {
  /**
   * @brief Registers the (single) VIP_OnClientDisconnect listener of the calling plugin. Fires when a player that was seen to be a VIP disconnects, and from VIP_RemoveClientVIP with isVIP = false.
   * @param callback (function): Function to invoke.
   */
  inline void VIP_OnClientDisconnect_Register(ClientLoadedOrDisconnectDelegate callback) {
    return __vip_VIP_OnClientDisconnect_Register(callback);
  }

  using _VIP_OnClientDisconnect_Unregister = void (*)(ClientLoadedOrDisconnectDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnClientDisconnect_Unregister __vip_VIP_OnClientDisconnect_Unregister;
namespace vip {
  /**
   * @brief Removes the VIP_OnClientDisconnect listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void VIP_OnClientDisconnect_Unregister(ClientLoadedOrDisconnectDelegate callback) {
    return __vip_VIP_OnClientDisconnect_Unregister(callback);
  }

  using _VIP_OnPlayerSpawn_Register = void (*)(SpawnDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnPlayerSpawn_Register __vip_VIP_OnPlayerSpawn_Register;
namespace vip {
  /**
   * @brief Registers the (single) VIP_OnPlayerSpawn listener of the calling plugin. Fires one frame after player_spawn for a living, non-bot player.
   * @param callback (function): Function to invoke.
   */
  inline void VIP_OnPlayerSpawn_Register(SpawnDelegate callback) {
    return __vip_VIP_OnPlayerSpawn_Register(callback);
  }

  using _VIP_OnPlayerSpawn_Unregister = void (*)(SpawnDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnPlayerSpawn_Unregister __vip_VIP_OnPlayerSpawn_Unregister;
namespace vip {
  /**
   * @brief Removes the VIP_OnPlayerSpawn listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void VIP_OnPlayerSpawn_Unregister(SpawnDelegate callback) {
    return __vip_VIP_OnPlayerSpawn_Unregister(callback);
  }

  using _VIP_OnVIPClientRemoved_Register = void (*)(VipRemoveDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnVIPClientRemoved_Register __vip_VIP_OnVIPClientRemoved_Register;
namespace vip {
  /**
   * @brief Registers the (single) VIP_OnVIPClientRemoved listener of the calling plugin. Fires when the VIP status of a player ends (reason 1 = expired by the per-second check, 2 = VIP_RemoveClientVIP).
   * @param callback (function): Function to invoke.
   */
  inline void VIP_OnVIPClientRemoved_Register(VipRemoveDelegate callback) {
    return __vip_VIP_OnVIPClientRemoved_Register(callback);
  }

  using _VIP_OnVIPClientRemoved_Unregister = void (*)(VipRemoveDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnVIPClientRemoved_Unregister __vip_VIP_OnVIPClientRemoved_Unregister;
namespace vip {
  /**
   * @brief Removes the VIP_OnVIPClientRemoved listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void VIP_OnVIPClientRemoved_Unregister(VipRemoveDelegate callback) {
    return __vip_VIP_OnVIPClientRemoved_Unregister(callback);
  }

  using _VIP_OnVIPClientAdded_Register = void (*)(VipAddDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnVIPClientAdded_Register __vip_VIP_OnVIPClientAdded_Register;
namespace vip {
  /**
   * @brief Registers the (single) VIP_OnVIPClientAdded listener of the calling plugin. Fires after VIP_GiveClientVIP succeeded.
   * @param callback (function): Function to invoke.
   */
  inline void VIP_OnVIPClientAdded_Register(VipAddDelegate callback) {
    return __vip_VIP_OnVIPClientAdded_Register(callback);
  }

  using _VIP_OnVIPClientAdded_Unregister = void (*)(VipAddDelegate);
}
extern "C" PLUGIN_API vip::_VIP_OnVIPClientAdded_Unregister __vip_VIP_OnVIPClientAdded_Unregister;
namespace vip {
  /**
   * @brief Removes the VIP_OnVIPClientAdded listener registered by this plugin. Idempotent.
   * @param callback (function): Function to remove.
   */
  inline void VIP_OnVIPClientAdded_Unregister(VipAddDelegate callback) {
    return __vip_VIP_OnVIPClientAdded_Unregister(callback);
  }

} // namespace vip
