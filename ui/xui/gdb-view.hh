//
// gnw-h7b0 User Interface -- GDB stub settings section.
//
// Lives in its own file (rather than inline in main-menu.cc) per the
// repo convention that main-menu.cc stays a thin registration point.
// Drawn as a section inside the System settings tab.
//
#pragma once

void DrawGdbSettings();

// Applies the persisted g_config.sys.gdb.* settings once at startup.
void ApplyGdbSettingsAtStartup();

// Called each HUD frame; performs the deferred startup start once the
// machine has a CPU.
void GdbSettingsTick();
