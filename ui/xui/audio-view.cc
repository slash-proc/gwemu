//
// gnw-h7b0 User Interface -- Audio settings tab
//
#include "common.hh"
#include "main-menu.hh"
#include "widgets.hh"
#include "../gwemu-settings.h"

void MainMenuAudioView::Draw()
{
    SectionTitle("Volume");
    if (ImGui::SliderFloat("Output volume", &g_config.audio.volume_limit, 0.0f, 1.0f)) {
        gwemu_settings_save();
    }
    ImGui::TextWrapped(
        "Note: not yet wired to real output attenuation -- this board's "
        "SAI1 audio path (hw/misc/gnw_h7b0_sai1.c) and QEMU's generic "
        "audio backend have no volume/gain API today. Persisted here for "
        "when that's added.");

    SectionTitle("Output Device");
    ImGui::TextWrapped(
        "Audio backend selection (coreaudio/pa/sdl/none) is chosen at "
        "launch via -audiodev / the GNW_AUDIODEV environment variable "
        "(see scripts/boot_qemu.sh) -- QEMU's audio backend is fixed for "
        "the life of the process and can't be changed live from here.");
}
