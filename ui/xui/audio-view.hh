//
// gnw-h7b0 User Interface -- Audio settings tab
//
// Structural reference only: xemu's MainMenuAudioView (upstream xemu
// tree) exposes Xbox APU-specific options (DSP passthrough/HLE toggle,
// HRTF, voice-processor worker count) that have no equivalent here --
// this board's audio path is SAI1 (hw/misc/gnw_h7b0_sai1.c) feeding
// QEMU's generic audio backend, not an emulated DSP. Only the volume
// control is kept, and it is NOT yet wired to real attenuation anywhere
// in the audio pipeline -- see this file's .cc for the honest caveat.
//
#pragma once
// Deliberately does NOT #include "main-menu.hh" -- see display-view.hh's
// comment, same circular-include hazard/resolution.

class MainMenuAudioView : public virtual MainMenuTabView
{
public:
    void Draw() override;
};
