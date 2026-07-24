//
// gnw-h7b0 User Interface -- shared style tokens for the firmware/profile
// flow (merged from the flow + visual design specs, 2026-07-24; where the
// two tables differed the visual spec's values won by arbitration).
// Route EVERY color in the firmware cards/strip through these -- no
// inline ImVec4s in that code.
//
#pragma once
#include "imgui.h"

// Accent family (derived from InitializeStyle's ButtonActive/CheckMark)
static const ImVec4 GNW_COL_ACCENT      = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
static const ImVec4 GNW_COL_ACCENT_DIM  = ImVec4(0.26f, 0.66f, 0.23f, 0.35f); // complete-card border
static const ImVec4 GNW_COL_ACCENT_WASH = ImVec4(0.26f, 0.66f, 0.23f, 0.06f); // complete-card bg tint

// Accent button states (Select folder... / lit Continue)
static const ImVec4 GNW_COL_ACCENT_HOVER  = ImVec4(0.32f, 0.76f, 0.28f, 1.00f);
static const ImVec4 GNW_COL_ACCENT_ACTIVE = ImVec4(0.20f, 0.52f, 0.18f, 1.00f);

// Card neutrals (between WindowBg .10 and FrameBg .18)
static const ImVec4 GNW_COL_CARD_BG     = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
static const ImVec4 GNW_COL_CARD_BORDER = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);

// Amber: partial sets only -- deliberately NOT red (missing != error) and
// NOT the theme green. Red is reserved for hash-mismatch, the one true
// error in the flow.
static const ImVec4 GNW_COL_WARN = ImVec4(0.85f, 0.62f, 0.18f, 1.00f);
static const ImVec4 GNW_COL_ERR  = ImVec4(0.80f, 0.28f, 0.24f, 1.00f);

// Spacing/geometry (multiply by g_viewport_mgr.m_scale at use site)
static const float GNW_PAD_CARD    = 12.0f;
static const float GNW_GAP_SECTION = 18.0f;
static const float GNW_RADIUS_CARD = 6.0f;
static const float GNW_CARD_H      = 64.0f;
static const float GNW_GAP_GUTTER  = 12.0f;   // between the two cards

// Verified compiled FontAwesome subset for this flow (the min.otf holds
// only 55 glyphs): CHECK, XMARK, CIRCLE_INFO, FOLDER, MICROCHIP, GAMEPAD,
// DOWNLOAD. CIRCLE_CHECK / TRIANGLE_EXCLAMATION / SD_CARD / FOLDER_OPEN /
// HARD_DRIVE are NOT in the subset and render as boxes -- regenerating
// the font subset is a separate task if they're ever wanted.
