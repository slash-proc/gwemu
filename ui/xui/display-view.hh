//
// gnw-h7b0 User Interface -- Display settings tab
//
// Ported from xemu's MainMenuDisplayView concept (structural reference
// only -- ui/xui/main-menu.hh's original in the upstream xemu tree).
// Xbox-specific options dropped: AV pack / resolution mode selection
// (480p/720p/1080i etc. -- meaningless for this board's fixed small
// panel), widescreen-hack toggle, Vulkan-specific settings. Kept: the
// two settings genuinely already wired to real behavior in ui/gwemu.c
// (fullscreen_on_startup, window.vsync) plus the HUD's own UI scale
// (viewport-manager.cc's g_config.display.ui.scale/auto_scale).
//
#pragma once
// Deliberately does NOT #include "main-menu.hh" -- that header includes
// this one (after defining MainMenuTabView, see its own comment), so
// self-including here would create a cycle. MainMenuTabView must already
// be visible wherever this header is included from.

class MainMenuDisplayView : public virtual MainMenuTabView
{
public:
    void Draw() override;
};
