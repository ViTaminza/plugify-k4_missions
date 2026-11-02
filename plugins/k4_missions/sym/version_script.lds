{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* hudmenu binding slots (defined by hudmenu_bindings; the optional dependency is used only
           for the player's own "!menu" type choice, so they stay null without the plugin) */
        __hudmenu_*;

        /* permissions binding slots (defined by permissions_bindings) */
        __permissions_*;

        /* vip binding slots (defined by the generated vip.cpp, filled from the vip plugin) */
        __vip_*;

        /* No exported methods: k4_missions exposes nothing to other plugins. */

    local: *;
};
