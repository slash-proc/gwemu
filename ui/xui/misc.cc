//
// GWemu User Interface
//
// Copyright (C) 2026 Matt Borgerson
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
#include "misc.hh"
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_platform_defines.h>
#include <filesystem>
#include <string>

static void RunOnMainThread(std::function<void()> &&func)
{
    auto p = new std::function<void()>(std::move(func));
    if (!SDL_RunOnMainThread(
            [](void *userdata) {
                std::unique_ptr<std::function<void()>> f(
                    static_cast<std::function<void()> *>(userdata));
                gwemu_main_loop_lock();
                (*f)();
                gwemu_main_loop_unlock();
            },
            p, false)) {
        delete p;
    }
}

static void FileDialogCallbackWrapper(void *userdata,
                                      const char *const *filelist, int filter)
{
    std::unique_ptr<FileDialogCallback> callback(
        static_cast<FileDialogCallback *>(userdata));
    if (filelist && filelist[0]) {
        std::string path = filelist[0];
        RunOnMainThread([callback = std::move(*callback),
                         path = std::move(path)]() { callback(path.c_str()); });
    }
}

// Workaround SDL3 default_location handling:
// - Linux: only supports folder paths, not file paths as documented
// - Windows/macOS: directories need trailing separator for proper display
static std::string NormalizeDefaultLocation(const char *default_location)
{
    if (!default_location || !*default_location) {
        return {};
    }

    try {
        std::filesystem::path path(default_location);
        // A nonexistent default makes the portal/dialog error out
        // ("Unable to find ...") on every platform -- and a RELATIVE
        // path is resolved by the portal against $HOME, not our cwd
        // (confirmed real: library dir "backup" from an empty cwd
        // produced "Unable to find /home/<user>/backup"). Absolutize
        // against our cwd first, drop it entirely if it doesn't exist.
        path = std::filesystem::absolute(path);
        if (!std::filesystem::exists(path)) {
            return {};
        }
#if defined(SDL_PLATFORM_LINUX)
        if (std::filesystem::is_regular_file(path)) {
            return path.parent_path().string();
        }
#elif defined(SDL_PLATFORM_WINDOWS) || defined(SDL_PLATFORM_MACOS)
        if (std::filesystem::is_directory(path)) {
            return (path / "").string();
        }
#endif
        return path.string();
    } catch (...) {
        return {};
    }
}

void ShowOpenFileDialog(const SDL_DialogFileFilter *filters, int nfilters,
                        const char *default_location,
                        FileDialogCallback callback)
{
    auto *cb = new FileDialogCallback(std::move(callback));
    std::string normalized = NormalizeDefaultLocation(default_location);
    SDL_ShowOpenFileDialog(FileDialogCallbackWrapper, cb, gwemu_get_window(),
                           filters, nfilters,
                           normalized.empty() ? nullptr : normalized.c_str(),
                           false);
}

void ShowSaveFileDialog(const SDL_DialogFileFilter *filters, int nfilters,
                        const char *default_location,
                        FileDialogCallback callback)
{
    auto *cb = new FileDialogCallback(std::move(callback));
    std::string normalized = NormalizeDefaultLocation(default_location);
    SDL_ShowSaveFileDialog(FileDialogCallbackWrapper, cb, gwemu_get_window(),
                           filters, nfilters,
                           normalized.empty() ? nullptr : normalized.c_str());
}

void ShowOpenFolderDialog(const char *default_location,
                          FileDialogCallback callback)
{
    auto *cb = new FileDialogCallback(std::move(callback));
    std::string normalized = NormalizeDefaultLocation(default_location);
    SDL_ShowOpenFolderDialog(FileDialogCallbackWrapper, cb,
                             gwemu_get_window(),
                             normalized.empty() ? nullptr : normalized.c_str(),
                             false);
}
