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
// Historically these helpers were raw OpenGL (shaders, FBOs, decal
// blits) inherited from xemu, whose Xbox GPU emulation genuinely needs
// GL. This fork's GUI only blits the guest framebuffer and draws an
// ImGui HUD, so everything now goes through SDL_Renderer (D3D11 on
// Windows, Metal on macOS, Vulkan/OpenGL on Linux, software renderer as
// universal fallback) -- no hard GPU-API requirement anywhere. The
// filenames are kept to avoid build-system churn.
#pragma once
#include <vector>
#include <SDL3/SDL.h>
#include "common.hh"
#include "../gwemu-input.h"

extern SDL_Texture *g_logo_tex;

void InitCustomRendering(void);
void RenderFramebuffer(SDL_Texture *tex, int width, int height, bool flip);
bool RenderFramebufferToPng(SDL_Texture *tex, bool flip,
                            std::vector<uint8_t> &png, int max_width = 0,
                            int max_height = 0);
void SaveScreenshot(SDL_Texture *tex, bool flip);
void ScaleDimensions(int src_width, int src_height, int max_width,
                     int max_height, int *out_width, int *out_height);
SDL_Texture *LoadTextureFromMemory(const unsigned char *buf,
                                   unsigned int size);
