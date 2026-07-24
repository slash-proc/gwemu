/*
 * GWemu device profiles -- named, uniquely-identified sets representing a
 * whole Game & Watch device: bank1 + bank2 + extflash flash images (owned
 * copies) plus an optional SD card image.
 *
 * Layout under the app-data base path (gwemu_settings_get_base_path()):
 *   profiles/<id>/profile.toml            metadata (see GwProfile)
 *   profiles/<id>/bank1.bin               owned copy, 0xFF-padded
 *   profiles/<id>/bank2.bin
 *   profiles/<id>/extflash.bin
 *   profiles/<id>/sdcard.qcow2            only when sd.mode == Bundled
 *   sd-cards/<name>.qcow2 + <name>.toml   shared-SD registry (sidecar has
 *                                         shareable flag + display name)
 *
 * <id> = <slug>-<4 hex> -- immutable directory name; display_name lives in
 * profile.toml and renames never move the directory. Images are opaque
 * blobs throughout: no content inspection, ever (owner decision).
 *
 * Pure backend: no ImGui. Long operations (image copies, blank
 * generation) run via GwProfileAsyncOp on a worker thread; UI code polls
 * the atomics per frame (async discipline -- never block a draw handler).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef GWEMU_PROFILES_HH
#define GWEMU_PROFILES_HH

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

enum class GwSdMode {
    None,
    Bundled,   /* sdcard.qcow2 inside the profile dir */
    Shared,    /* references <base>/sd-cards/<image> */
};

struct GwProfileSd {
    GwSdMode mode = GwSdMode::None;
    std::string image;   /* Bundled: filename in profile dir; Shared:
                          * filename under sd-cards/ */
};

struct GwProfile {
    std::string id;           /* directory name, immutable */
    std::string dir;          /* absolute path of the profile directory */
    std::string display_name; /* may differ from id; user-renamable */
    std::string created;      /* ISO-8601, informational */

    /* Filenames relative to dir (fixed today, kept in the toml so the
     * format can evolve without breaking old profiles). */
    std::string bank1 = "bank1.bin";
    std::string bank2 = "bank2.bin";
    std::string extflash = "extflash.bin";

    /* Informational provenance labels for UI display only --
     * "ofw-mario" | "patched-zelda" | "user-file" | "blank" | "". */
    std::string prov_bank1, prov_bank2, prov_extflash;

    GwProfileSd sd;

    uint64_t disk_bytes = 0;  /* filled by Scan()/RecalcDiskUsage() */

    std::string Bank1Path() const { return dir + "/" + bank1; }
    std::string Bank2Path() const { return dir + "/" + bank2; }
    std::string ExtflashPath() const { return dir + "/" + extflash; }
    /* Absolute path of the SD image, or "" when mode == None. */
    std::string SdPath() const;
};

/* Entry in the shared-SD registry (<base>/sd-cards/). */
struct GwSharedSd {
    std::string image;        /* filename under sd-cards/ */
    std::string path;         /* absolute path */
    std::string display_name;
    bool shareable = false;
    uint64_t disk_bytes = 0;
};

class GwProfileStore {
public:
    /* Re-enumerate <base>/profiles/ and <base>/sd-cards/ from disk.
     * Creates both directories if missing. Safe to call repeatedly. */
    void Scan();

    const std::vector<GwProfile> &Profiles() const { return m_profiles; }
    const std::vector<GwSharedSd> &SharedSds() const { return m_shared_sds; }

    GwProfile *Find(const std::string &id);

    /* Create a new empty profile directory + profile.toml. display_name
     * may be empty -> a generated name is used. Returns the id, or ""
     * on failure (err filled). No images are written -- callers copy /
     * generate them (usually via GwProfileAsyncOp) and then Save(). */
    std::string Create(const std::string &display_name, std::string &err);

    /* Rewrite <dir>/profile.toml from p. */
    bool Save(const GwProfile &p, std::string &err);

    /* Recursively delete the profile directory. Never touches a Shared
     * SD image. */
    bool Delete(const std::string &id, std::string &err);

    bool Rename(const std::string &id, const std::string &new_display_name,
                std::string &err);

    uint64_t TotalDiskBytes() const;
    static uint64_t RecalcDiskUsage(GwProfile &p);

    /* "adjective_noun" gaming-themed generator (docker-style). */
    static std::string GenerateName();
    /* Lowercase, [a-z0-9-], collapsed; falls back to "profile". */
    static std::string Slugify(const std::string &name);

    static std::string ProfilesRoot();
    static std::string SdCardsRoot();

private:
    std::vector<GwProfile> m_profiles;
    std::vector<GwSharedSd> m_shared_sds;
};

/* The single store instance the GUI uses. */
extern GwProfileStore g_profile_store;

/*
 * One async file operation (copy or blank-fill), worker-thread backed.
 * Usage: StartCopy()/StartBlank() from the UI thread; poll state() each
 * frame; when Done/Failed, call Join() (cheap once finished). A GwAsyncOp
 * must outlive its worker -- keep it as a member, not a stack local.
 */
class GwProfileAsyncOp {
public:
    enum State { Idle, Running, Done, Failed };

    ~GwProfileAsyncOp() { Join(); }

    /* Chunked copy src -> dst with progress. */
    bool StartCopy(const std::string &src, const std::string &dst);
    /* Write `size` bytes of 0xFF to dst. */
    bool StartBlank(const std::string &dst, uint64_t size);

    State state() const { return (State)m_state.load(); }
    uint64_t bytes_done() const { return m_done.load(); }
    uint64_t bytes_total() const { return m_total.load(); }
    /* Valid only after state() == Failed and Join(). */
    const std::string &error() const { return m_error; }
    void Join();

private:
    bool StartWorker(std::function<bool(GwProfileAsyncOp *, std::string &)> fn);

    std::atomic<int> m_state{Idle};
    std::atomic<uint64_t> m_done{0};
    std::atomic<uint64_t> m_total{0};
    std::string m_error;          /* written by worker before state flip */
    std::thread m_thread;

    friend struct GwProfileAsyncOpImpl;
};

#endif
