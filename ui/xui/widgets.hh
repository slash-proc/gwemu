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
#pragma once
#include <functional>
#include <stdint.h>
#include <SDL3/SDL_dialog.h>
#include "common.hh"

// Tracks a window's natural content height and emits a resize request only
// after the measured height has settled. This keeps dynamic ImGui forms from
// needing a hand-maintained list of every state that can add a row, and avoids
// fighting the user while they resize a window.
class ContentFitHeight {
public:
    explicit ContentFitHeight(uint64_t settle_ms = 32)
        : m_settle_ms(settle_ms) {}

    void Reset();
    void Observe(float height, uint64_t now_ms);
    bool TakeRequest(float *height);

private:
    uint64_t m_settle_ms;
    uint64_t m_changed_ms = 0;
    float m_candidate = -1.0f;
    float m_reported = -1.0f;
    bool m_pending = false;
};

void Separator();
void SectionTitle(const char *title);
float GetWidgetTitleDescriptionHeight(const char *title,
                                      const char *description);
void WidgetTitleDescription(const char *title, const char *description,
                            ImVec2 pos);
void WidgetTitleDescriptionItem(const char *str_id,
                                const char *description = nullptr);
float GetSliderRadius(ImVec2 size);
float GetSliderTrackXOffset(ImVec2 size);
float GetSliderTrackWidth(ImVec2 size);
float GetSliderValueForMousePos(ImVec2 mouse, ImVec2 pos, ImVec2 size);
void DrawSlider(float v, bool hovered, ImVec2 pos, ImVec2 size);
void DrawToggle(bool enabled, bool hovered, ImVec2 pos, ImVec2 size);
bool Toggle(const char *str_id, bool *v, const char *description = nullptr);
void Slider(const char *str_id, float *v, const char *description = nullptr);
std::string EllipsizeMiddle(const std::string &s, float max_w);

void InlineFileField(const char *str_id, const char *current_path,
                     const SDL_DialogFileFilter *filters, int nfilters, bool dir,
                     std::function<void(const char *new_path)> on_select);
void DrawComboChevron();
void PrepareComboTitleDescription(const char *label, const char *description,
                                  float combo_size_ratio);
bool ChevronCombo(const char *label, int *current_item,
                  bool (*items_getter)(void *, int, const char **), void *data,
                  int items_count, const char *description = NULL);
bool ChevronCombo(const char* label, int* current_item, const char* items_separated_by_zeros, const char *description = NULL);
void Hyperlink(const char *text, const char *url);
void HelpMarker(const char* desc);
void Logo();
