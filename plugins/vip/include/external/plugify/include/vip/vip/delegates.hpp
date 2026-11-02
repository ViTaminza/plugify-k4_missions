#pragma once

#include "enums.hpp"
#include "aliases.hpp"
// Generated from vip.pplugin

namespace vip {

  // Called when the player presses a SELECTABLE feature in the VIP menu. Return true to close the menu after the call.
  using ItemSelectableDelegate = bool (*)(int32_t, const plg::string&);


  // Called when the player toggles a TOGGLABLE feature. The callback may veto the change by returning true (the menu then keeps the old status) and may rewrite newStatus. The core stores the resulting state in the '<group>.<feature>' cookie itself.
  using ItemTogglableDelegate = bool (*)(int32_t, const plg::string&, ToggleState, ToggleState&);


  // Called while the VIP menu is built; a non-empty return value replaces the generated label.
  using ItemDisplayDelegate = plg::string (*)(int32_t, const plg::string&);


  // VIP_OnVIPLoaded listener body (no arguments).
  using ReadyDelegate = void (*)();


  // VIP_OnClientLoaded / VIP_OnClientDisconnect listener body: the VIP state of a slot after its vip_users row was read (OnClientLoaded) or when the player left (OnClientDisconnect).
  using ClientLoadedOrDisconnectDelegate = void (*)(int32_t, bool);


  // VIP_OnPlayerSpawn listener body; fires one frame after the player_spawn event for a living, non-bot player.
  using SpawnDelegate = void (*)(int32_t, int32_t, bool);


  // VIP_OnVIPClientRemoved listener body: reason 1 = the VIP status expired, 2 = VIP_RemoveClientVIP.
  using VipRemoveDelegate = void (*)(int32_t, int32_t);


  // VIP_OnVIPClientAdded listener body; fires after VIP_GiveClientVIP succeeded.
  using VipAddDelegate = void (*)(int32_t);


} // namespace vip
