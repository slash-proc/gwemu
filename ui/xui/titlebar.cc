//
// gwemu User Interface -- custom window titlebar
//
// See titlebar.hh for why this exists (no reliable system decorations to
// depend on).
//
#include "common.hh"
#include "titlebar.hh"
#include "actions.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "gwemu-hud.h"

static const float kResizeBorderPx = 6.0f; // unscaled; scaled below like everything else

// Screen-space rect of the titlebar's draggable area, updated each frame by
// DrawTitlebar() and read back by TitlebarHitTest(). Excludes the button
// area on the right so the buttons themselves stay clickable, not
// draggable.
static ImVec2 g_titlebar_drag_min, g_titlebar_drag_max;
static float g_titlebar_height = 0;

float DrawTitlebar()
{
    ImGuiIO &io = ImGui::GetIO();
    float scale = g_viewport_mgr.m_scale;
    float height = 28.0f * scale;
    float btn_w = 32.0f * scale;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, height));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                              ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * scale, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("##titlebar", nullptr, flags);

    ImGui::PushFont(g_font_mgr.m_menu_font_small);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("GWemu");
    ImGui::PopFont();

    // The draggable area is everything left of the button cluster -- record
    // it now, buttons get drawn (and excluded) below.
    ImVec2 win_pos = ImGui::GetWindowPos();
    g_titlebar_drag_min = win_pos;
    g_titlebar_drag_max = ImVec2(win_pos.x + io.DisplaySize.x - 3 * btn_w, win_pos.y + height);

    ImGui::SameLine(io.DisplaySize.x - 3 * btn_w + 8 * scale);
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32_BLACK_TRANS);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.25f));

    if (ImGui::Button(ICON_FA_WINDOW_MINIMIZE, ImVec2(btn_w, height))) {
        SDL_MinimizeWindow(gwemu_get_window());
    }
    ImGui::SameLine(0, 0);
    if (ImGui::Button(ICON_FA_WINDOW_MAXIMIZE, ImVec2(btn_w, height))) {
        SDL_WindowFlags wf = SDL_GetWindowFlags(gwemu_get_window());
        if (wf & SDL_WINDOW_MAXIMIZED) {
            SDL_RestoreWindow(gwemu_get_window());
        } else {
            SDL_MaximizeWindow(gwemu_get_window());
        }
    }
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button(ICON_FA_XMARK, ImVec2(btn_w, height))) {
        ActionShutdown();
    }
    ImGui::PopStyleColor(4);

    ImGui::End();
    ImGui::PopStyleVar(2);

    g_titlebar_height = height;
    return height;
}

static SDL_HitTestResult SDLCALL TitlebarHitTest(SDL_Window *win, const SDL_Point *area, void *data)
{
    (void)data;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(win, &ww, &wh);

    float border = kResizeBorderPx * g_viewport_mgr.m_scale;
    bool left   = area->x < border;
    bool right  = area->x > ww - border;
    bool top    = area->y < border;
    bool bottom = area->y > wh - border;

    if (top && left)     return SDL_HITTEST_RESIZE_TOPLEFT;
    if (top && right)    return SDL_HITTEST_RESIZE_TOPRIGHT;
    if (bottom && left)  return SDL_HITTEST_RESIZE_BOTTOMLEFT;
    if (bottom && right) return SDL_HITTEST_RESIZE_BOTTOMRIGHT;
    if (left)   return SDL_HITTEST_RESIZE_LEFT;
    if (right)  return SDL_HITTEST_RESIZE_RIGHT;
    if (top)    return SDL_HITTEST_RESIZE_TOP;
    if (bottom) return SDL_HITTEST_RESIZE_BOTTOM;

    // Titlebar drag region, in window-local coordinates -- g_titlebar_drag_*
    // is in ImGui/SDL screen space, which for the main window is the same
    // origin as window-local space (the window's own top-left is (0,0)).
    if (g_titlebar_height > 0 &&
        area->x >= g_titlebar_drag_min.x && area->x < g_titlebar_drag_max.x &&
        area->y >= g_titlebar_drag_min.y && area->y < g_titlebar_drag_max.y) {
        return SDL_HITTEST_DRAGGABLE;
    }

    return SDL_HITTEST_NORMAL;
}

extern "C" void titlebar_install_hittest(struct SDL_Window *window)
{
    SDL_SetWindowHitTest((SDL_Window *)window, TitlebarHitTest, nullptr);
}
