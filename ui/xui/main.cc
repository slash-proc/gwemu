//
// GWemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <functional>
#include <assert.h>
#include <fpng.h>

extern "C" {
void gnw_h7b0_rtc_set_sync_host(bool sync_host);
}

#include <deque>
#include <vector>
#include <string>
#include <memory>

#include "actions.hh"
#include "common.hh"
#include "gwemu-hud.h"
#include "misc.hh"
#include "gl-helpers.hh"
#include "input-manager.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "scene.hh"
#include "scene-manager.hh"
#include "main-menu.hh"
#include "popup-menu.hh"
#include "notifications.hh"
#include "monitor.hh"
#include "welcome.hh"
#include "profile-wizard.hh"
#include "../gwemu-profiles.hh"
#include "menubar.hh"

bool g_screenshot_pending;
const char *g_snapshot_pending_load_name;

float g_main_menu_height;


static ImGuiContext *g_ctx_main = nullptr;
static ImGuiContext *g_ctx_settings = nullptr;
static SDL_Window *g_settings_window = nullptr;
static SDL_Renderer *g_settings_renderer = nullptr;

static ImGuiStyle g_base_style;
static float g_last_scale;
static float g_last_scale_settings;
static int g_vsync;
static SDL_Texture *g_tex;
static SDL_Renderer *g_renderer;
static bool g_flip_req;


static void InitializeStyle()
{
    g_font_mgr.Rebuild();

    ImGui::StyleColorsDark();
    ImVec4 *c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.98f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.11f, 0.11f, 0.11f, 0.60f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 0.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.13f, 0.87f, 0.16f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.25f, 0.75f, 0.10f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.83f, 0.49f, 0.04f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.26f, 0.67f, 0.23f, 0.95f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.21f, 0.54f, 0.19f, 0.99f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.24f, 0.60f, 0.21f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.16f, 0.16f, 0.16f, 0.73f);

    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0;
    s.FrameRounding = 6.0;
    s.PopupRounding = 6.0;
    g_base_style = s;
}

void gwemu_hud_init(SDL_Window* window, SDL_Renderer* renderer)
{
    gwemu_monitor_init();
    g_vsync = g_config.display.window.vsync;

    InitCustomRendering();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    g_ctx_main = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx_main);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = NULL;

    // Setup Platform/Renderer bindings
    g_renderer = renderer;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    ImPlot::CreateContext();

    g_last_scale = g_viewport_mgr.m_scale;
    InitializeStyle();
    // First-run: the profile-creation wizard IS the welcome experience
    // now (owner decision) -- the old FirstBootWindow only shows for the
    // profiles-already-exist case until it's removed entirely in Phase 3.
    if (g_config.general.show_welcome && g_profile_store.Profiles().empty()) {
        g_profile_wizard.Open();
        g_config.general.show_welcome = false;
    } else {
        first_boot_window.is_open = g_config.general.show_welcome;
    }
    gnw_h7b0_rtc_set_sync_host(g_config.sys.rtc_sync_host);
}

void gwemu_hud_cleanup(void)
{
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void gwemu_hud_process_sdl_events(SDL_Event *event)
{
    // Ignore inputs that are consumed by rebinding
    if (g_main_menu.ConsumeRebindEvent(event)) {
        return;
    }

    ImGui_ImplSDL3_ProcessEvent(event);
}

void gwemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    if (kbd) *kbd = io.WantCaptureKeyboard;
    if (mouse) *mouse = io.WantCaptureMouse;
}

void gwemu_hud_set_framebuffer_texture(SDL_Texture *tex, bool flip)
{
    g_tex = tex;
    g_flip_req = flip;
}

bool gwemu_hud_get_framebuffer_size(int *w, int *h)
{
    // Same technique RenderFramebuffer() already uses -- query the actual
    // texture dims rather than assuming a compile-time panel resolution
    // constant (none exists; LTDC's output size is register-configurable).
    if (!g_tex) {
        return false;
    }
    float fw = 0, fh = 0;
    SDL_GetTextureSize(g_tex, &fw, &fh);
    *w = (int)fw;
    *h = (int)fh;
    return *w > 0 && *h > 0;
}

void gwemu_hud_update(void)
{
    ImGuiIO& io = ImGui::GetIO();
    uint32_t now = SDL_GetTicks();

    g_viewport_mgr.Update();
    g_font_mgr.Update();
    if (g_last_scale != g_viewport_mgr.m_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(g_viewport_mgr.m_scale);
        g_last_scale = g_viewport_mgr.m_scale;
    }

    if (!first_boot_window.is_open) {
        int ww, wh;
        SDL_GetWindowSizeInPixels(gwemu_get_window(), &ww, &wh);
        RenderFramebuffer(g_tex, ww, wh, g_flip_req);
    }

    ImGui_ImplSDLRenderer3_NewFrame();
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_NewFrame();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_input_mgr.Update();

    ImGui::NewFrame();
    ProcessKeyboardShortcuts();

#if defined(CONFIG_RENDERDOC)
    if (g_capture_renderdoc_frame) {
        nv2a_dbg_renderdoc_capture_frames(1, false);
        g_capture_renderdoc_frame = false;
    }
#endif

    if (g_config.display.ui.show_menubar && !first_boot_window.is_open) {
        // Auto-hide main menu after 5s of inactivity
        static uint32_t last_check = 0;
        float alpha = 1.0;
        const uint32_t timeout = 5000;
        const float fade_duration = 1000.0;
        bool menu_wakeup = g_input_mgr.MouseMoved();
        if (menu_wakeup) {
            last_check = now;
        }
        if ((now-last_check) > timeout) {
            if (g_config.display.ui.use_animations) {
                float t = fmin((float)((now-last_check)-timeout)/fade_duration, 1);
                alpha = 1-t;
                if (t >= 1) {
                    alpha = 0;
                }
            } else {
                alpha = 0;
            }
        }
        if (alpha > 0.0) {
            ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Text];
            tc.w = alpha;
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
            ImGui::SetNextWindowBgAlpha(alpha);
            ShowMainMenu();
            ImGui::PopStyleColor();
        } else {
            g_main_menu_height = 0;
        }
    }

    static uint32_t last_mouse_move = 0;
    if (g_input_mgr.MouseMoved()) {
        last_mouse_move = now;
    }

    // FIXME: Handle time wrap around
    bool settings_visible = g_settings_window && !(SDL_GetWindowFlags(g_settings_window) & SDL_WINDOW_HIDDEN);
    if (g_config.display.ui.hide_cursor && !settings_visible && (now - last_mouse_move) > 3000) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
        !g_scene_mgr.IsDisplayingScene()) {

        // If the guide button is pressed, wake the ui
        bool menu_button = false;
        uint32_t buttons = g_input_mgr.CombinedButtons();
        if (buttons & CONTROLLER_BUTTON_GUIDE) {
            menu_button = true;
        }

        // Allow controllers without a guide button to also work
        if ((buttons & CONTROLLER_BUTTON_BACK) &&
            (buttons & CONTROLLER_BUTTON_START)) {
            menu_button = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
            gwemu_settings_hud_show();
        } else if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (menu_button ||
                   (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    !ImGui::IsAnyItemFocused() && !ImGui::IsAnyItemHovered())) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            gwemu_toggle_fullscreen();
        }

    }

    // First-boot wizard lives in the settings window -- pop that window
    // once, after all init (gwemu_settings_hud_init runs after
    // gwemu_hud_init, so showing it from there would no-op).
    static bool wizard_autoshow_done = false;
    if (!wizard_autoshow_done && g_profile_wizard.is_open) {
        gwemu_settings_hud_show();
        wizard_autoshow_done = true;
    }

    first_boot_window.Draw();
    monitor_window.Draw();
    g_scene_mgr.Draw();
    if (!first_boot_window.is_open) notification_manager.Draw();

    // static bool show_demo = true;
    // if (show_demo) ImGui::ShowDemoWindow(&show_demo);
}

void gwemu_hud_render()
{
    ImGui::Render();
    /*
     * imgui_impl_sdlrenderer3 does NOT scale vertex geometry itself: it
     * expects the app to map ImGui's point coordinate space to the
     * renderer's pixel space via SDL_SetRenderScale (it only pre-scales
     * CLIP rects, and only when the renderer scale is 1). Without this,
     * any backend where window points != pixels (Wayland with
     * fractional/HiDPI scale; FramebufferScale != 1) draws the UI at
     * points-into-pixels size (too small, top-left) with clip rects
     * scaled separately (chopped/invisible text) -- confirmed live on a
     * 1.5x-fractional Wayland desktop while x11 (scale 1) was fine.
     * Scale during ImGui rendering only; the guest-framebuffer blit
     * (RenderFramebuffer) deliberately works in raw pixels.
     */
    ImGuiIO &io_r = ImGui::GetIO();
    SDL_SetRenderScale(g_renderer, io_r.DisplayFramebufferScale.x,
                       io_r.DisplayFramebufferScale.y);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_renderer);
    SDL_SetRenderScale(g_renderer, 1.0f, 1.0f);

    // Update/render any ImGui windows (e.g. Settings) that got dragged out
    // into their own real OS-level window -- see ImGuiConfigFlags_ViewportsEnable
    // in gwemu_hud_init(). RenderPlatformWindowsDefault() makes each platform
    // window's own GL context current in turn as it goes, so the main
    // window's context has to be restored afterward before our caller
    // (ui/gwemu.c) swaps the main window.
    ImGuiIO &io_vp = ImGui::GetIO();
    if (io_vp.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    if (g_vsync != g_config.display.window.vsync) {
        g_vsync = g_config.display.window.vsync;
        SDL_SetRenderVSync(g_renderer,
                           g_vsync ? 1 : SDL_RENDERER_VSYNC_DISABLED);
    }

    if (g_screenshot_pending) {
        SaveScreenshot(g_tex, g_flip_req);
        g_screenshot_pending = false;
    }
}

void gwemu_settings_hud_init(SDL_Window *window, SDL_Renderer *renderer)
{
    g_settings_window = window;
    g_settings_renderer = renderer;

    g_ctx_settings = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ctx_settings);
    
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    
    ImGuiStyle &s = ImGui::GetStyle();
    s = g_base_style;
    
    if (g_ctx_main) ImGui::SetCurrentContext(g_ctx_main);
}

void gwemu_settings_hud_cleanup(void)
{
    if (g_ctx_settings) {
        ImGui::SetCurrentContext(g_ctx_settings);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(g_ctx_settings);
        g_ctx_settings = nullptr;
    }
}

void gwemu_settings_hud_show(void)
{
    if (g_settings_window) {
        if ((SDL_GetWindowFlags(g_settings_window) & SDL_WINDOW_HIDDEN) &&
            g_settings_renderer) {
            // The renderer still holds the last frame this window ever
            // presented (e.g. the settings menu) -- showing it now would
            // flash that stale content for a frame before the first real
            // draw. Present a clean frame first.
            SDL_SetRenderDrawColor(g_settings_renderer, 25, 25, 25, 255);
            SDL_RenderClear(g_settings_renderer);
            SDL_RenderPresent(g_settings_renderer);
        }
        SDL_ShowWindow(g_settings_window);
        SDL_RaiseWindow(g_settings_window);
    }
}

void gwemu_settings_hud_process_sdl_events(SDL_Event *event)
{
    if (g_ctx_settings) {
        ImGui::SetCurrentContext(g_ctx_settings);
        // Ignore inputs that are consumed by rebinding
        if (g_main_menu.ConsumeRebindEvent(event)) {
            ImGui::SetCurrentContext(g_ctx_main);
            return;
        }
        ImGui_ImplSDL3_ProcessEvent(event);
        ImGui::SetCurrentContext(g_ctx_main);
    }
}

void gwemu_settings_hud_update(void)
{
    if (!g_ctx_settings || !g_settings_window) return;
    
    // Only update if visible
    if (!(SDL_GetWindowFlags(g_settings_window) & SDL_WINDOW_HIDDEN)) {
        ImGui::SetCurrentContext(g_ctx_settings);

        if (g_last_scale_settings != g_viewport_mgr.m_scale) {
            g_font_mgr.Rebuild();
            ImGuiStyle &style = ImGui::GetStyle();
            style = g_base_style;
            style.ScaleAllSizes(g_viewport_mgr.m_scale);
            g_last_scale_settings = g_viewport_mgr.m_scale;
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        if (g_profile_wizard.is_open) {
            // The wizard owns the settings window while open (owner
            // decision: it IS the first-run settings experience). The
            // normal settings menu is hidden; the wizard's own
            // Cancel button closes it to fall through here.
            g_profile_wizard.Draw();

            // Event-driven height tracking: exactly ONE resize request
            // per genuine layout change (accordion toggle, template/
            // validation rows) -- whatever size the WM then grants is
            // accepted as-is. Per-frame height-delta resizing fought
            // user drag-resizes and compositor size grants and
            // degraded the window over time. Height only -- the user's
            // chosen width is always respected.
            if (g_profile_wizard.TakeContentChanged()) {
                int cur_w, cur_h;
                SDL_GetWindowSize(g_settings_window, &cur_w, &cur_h);
                int want = (int)(g_profile_wizard.DesiredHeight() + 0.5f);
                if (want < 260) want = 260;
                if (want > 1000) want = 1000;
                if (SDL_abs(want - cur_h) > 8) {
                    SDL_SetWindowSize(g_settings_window, cur_w, want);
                }
            }

            if (!g_profile_wizard.is_open && g_profile_wizard.WasCompleted()) {
                SDL_HideWindow(g_settings_window);
            }
        } else {
            bool is_open = g_main_menu.Draw();

            if (!is_open) {
                SDL_HideWindow(g_settings_window);
            }
        }

        if (g_ctx_main) ImGui::SetCurrentContext(g_ctx_main);
    }
}

void gwemu_settings_hud_render(void)
{
    if (!g_ctx_settings || !g_settings_window) return;
    
    if (!(SDL_GetWindowFlags(g_settings_window) & SDL_WINDOW_HIDDEN)) {
        ImGui::SetCurrentContext(g_ctx_settings);
        ImGui::Render();
        
        ImGuiIO &io_r = ImGui::GetIO();
        SDL_SetRenderScale(g_settings_renderer, io_r.DisplayFramebufferScale.x, io_r.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(g_settings_renderer, 0, 0, 0, 255);
        SDL_RenderClear(g_settings_renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_settings_renderer);
        SDL_SetRenderScale(g_settings_renderer, 1.0f, 1.0f);
        
        if (g_ctx_main) ImGui::SetCurrentContext(g_ctx_main);
    }
}
