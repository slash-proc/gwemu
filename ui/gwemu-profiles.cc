/*
 * GWemu device profile store -- see gwemu-profiles.hh for the model and
 * on-disk layout. glib/gstdio for all file ops (Windows path/UTF-8
 * correctness); toml++ for profile.toml both ways.
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

#include "qemu/osdep.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <toml++/toml.h>

#include <cctype>
#include <fstream>
#include <sstream>

#include "gwemu-profiles.hh"
#include "gwemu-settings.h"

GwProfileStore g_profile_store;

/* ------------------------------------------------------------------ */
/* Name generation                                                     */

/* Docker-style adjective_noun, gaming-flavored (owner request: "maybe
 * make it more gaming related for the lulz"). Keep both lists slug-safe
 * (lowercase ascii, no separators). */
static const char *const kNameAdjectives[] = {
    "hungry", "sleepy", "dashing", "pixelated", "blocky", "speedy",
    "sneaky", "mighty", "tiny", "giant", "golden", "rusty", "glitchy",
    "turbo", "retro", "cursed", "blessed", "spicy", "chill", "feral",
    "cozy", "epic", "sus", "legendary", "wandering", "screaming",
};
static const char *const kNameNouns[] = {
    "octorok", "goomba", "moblin", "koopa", "keese", "boo", "toad",
    "deku", "wizzrobe", "lakitu", "peahat", "chuchu", "tektite",
    "shyguy", "stalfos", "leever", "bokoblin", "piranha", "darknut",
    "gibdo", "zora", "dodongo",
};

std::string GwProfileStore::GenerateName()
{
    const char *a = kNameAdjectives[g_random_int_range(0, G_N_ELEMENTS(kNameAdjectives))];
    const char *n = kNameNouns[g_random_int_range(0, G_N_ELEMENTS(kNameNouns))];
    return std::string(a) + "_" + n;
}

std::string GwProfileStore::Slugify(const std::string &name)
{
    std::string out;
    bool last_dash = true; /* suppress leading dash */
    for (char c : name) {
        unsigned char u = (unsigned char)c;
        if (std::isalnum(u)) {
            out += (char)std::tolower(u);
            last_dash = false;
        } else if (!last_dash) {
            out += '-';
            last_dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "profile";
    }
    if (out.size() > 48) {
        out.resize(48);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */

std::string GwProfileStore::ProfilesRoot()
{
    return std::string(gwemu_settings_get_base_path()) + "profiles";
}

std::string GwProfileStore::SdCardsRoot()
{
    return std::string(gwemu_settings_get_base_path()) + "sd-cards";
}

std::string GwProfile::SdPath() const
{
    switch (sd.mode) {
    case GwSdMode::Bundled:
        return dir + "/" + sd.image;
    case GwSdMode::Shared:
        return GwProfileStore::SdCardsRoot() + "/" + sd.image;
    default:
        return "";
    }
}

static uint64_t file_size_or_zero(const std::string &path)
{
    GStatBuf st;
    if (g_stat(path.c_str(), &st) == 0) {
        return (uint64_t)st.st_size;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* profile.toml round-trip                                             */

static bool load_profile_toml(GwProfile &p, std::string &err)
{
    std::string path = p.dir + "/profile.toml";
    try {
        toml::table t = toml::parse_file(path);
        p.display_name = t["display_name"].value_or(std::string(p.id));
        p.created = t["created"].value_or(std::string(""));
        if (auto *flash = t["flash"].as_table()) {
            p.bank1 = (*flash)["bank1"].value_or(std::string("bank1.bin"));
            p.bank2 = (*flash)["bank2"].value_or(std::string("bank2.bin"));
            p.extflash = (*flash)["extflash"].value_or(std::string("extflash.bin"));
            if (auto *prov = (*flash)["provenance"].as_table()) {
                p.prov_bank1 = (*prov)["bank1"].value_or(std::string(""));
                p.prov_bank2 = (*prov)["bank2"].value_or(std::string(""));
                p.prov_extflash = (*prov)["extflash"].value_or(std::string(""));
            }
        }
        if (auto *sd = t["sd"].as_table()) {
            std::string mode = (*sd)["mode"].value_or(std::string("none"));
            p.sd.mode = mode == "bundled" ? GwSdMode::Bundled
                      : mode == "shared"  ? GwSdMode::Shared
                                          : GwSdMode::None;
            p.sd.image = (*sd)["image"].value_or(std::string(""));
        }
        return true;
    } catch (const std::exception &e) {
        err = std::string("cannot parse ") + path + ": " + e.what();
        return false;
    }
}

bool GwProfileStore::Save(const GwProfile &p, std::string &err)
{
    toml::table prov{
        {"bank1", p.prov_bank1},
        {"bank2", p.prov_bank2},
        {"extflash", p.prov_extflash},
    };
    toml::table flash{
        {"bank1", p.bank1},
        {"bank2", p.bank2},
        {"extflash", p.extflash},
        {"provenance", prov},
    };
    toml::table t{
        {"version", 1},
        {"display_name", p.display_name},
        {"created", p.created},
        {"flash", flash},
    };
    if (p.sd.mode != GwSdMode::None) {
        t.insert("sd", toml::table{
            {"mode", p.sd.mode == GwSdMode::Bundled ? "bundled" : "shared"},
            {"image", p.sd.image},
        });
    }

    std::ostringstream ss;
    ss << t << "\n";
    std::string data = ss.str();

    /* Atomic-ish: write sidecar then rename (g_rename replaces on all
     * platforms glib supports). */
    std::string path = p.dir + "/profile.toml";
    std::string tmp = path + ".new";
    GError *gerr = NULL;
    if (!g_file_set_contents(tmp.c_str(), data.data(), (gssize)data.size(), &gerr)) {
        err = gerr ? gerr->message : "write failed";
        g_clear_error(&gerr);
        return false;
    }
    if (g_rename(tmp.c_str(), path.c_str()) != 0) {
        err = "cannot replace " + path;
        g_unlink(tmp.c_str());
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Store operations                                                    */

uint64_t GwProfileStore::RecalcDiskUsage(GwProfile &p)
{
    uint64_t total = 0;
    GDir *d = g_dir_open(p.dir.c_str(), 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            total += file_size_or_zero(p.dir + "/" + name);
        }
        g_dir_close(d);
    }
    p.disk_bytes = total;
    return total;
}

void GwProfileStore::Scan()
{
    m_profiles.clear();
    m_shared_sds.clear();

    std::string root = ProfilesRoot();
    std::string sdroot = SdCardsRoot();
    g_mkdir_with_parents(root.c_str(), 0755);
    g_mkdir_with_parents(sdroot.c_str(), 0755);

    GDir *d = g_dir_open(root.c_str(), 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            GwProfile p;
            p.id = name;
            p.dir = root + "/" + name;
            if (!g_file_test((p.dir + "/profile.toml").c_str(), G_FILE_TEST_EXISTS)) {
                continue; /* not a profile dir; leave it alone */
            }
            std::string err;
            if (!load_profile_toml(p, err)) {
                /* Corrupt toml: keep the profile listed (files intact,
                 * user can delete/repair) but surface the id as name. */
                fprintf(stderr, "profile %s: %s\n", name, err.c_str());
                p.display_name = p.id + " (unreadable)";
            }
            RecalcDiskUsage(p);
            m_profiles.push_back(std::move(p));
        }
        g_dir_close(d);
    }

    d = g_dir_open(sdroot.c_str(), 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            if (!g_str_has_suffix(name, ".qcow2")) {
                continue;
            }
            GwSharedSd sd;
            sd.image = name;
            sd.path = sdroot + "/" + name;
            sd.display_name = name;
            sd.disk_bytes = file_size_or_zero(sd.path);
            std::string sidecar = sd.path.substr(0, sd.path.size() - 6) + ".toml";
            try {
                if (g_file_test(sidecar.c_str(), G_FILE_TEST_EXISTS)) {
                    toml::table t = toml::parse_file(sidecar);
                    sd.shareable = t["shareable"].value_or(false);
                    sd.display_name = t["display_name"].value_or(std::string(name));
                }
            } catch (const std::exception &e) {
                fprintf(stderr, "sd-card sidecar %s: %s\n", sidecar.c_str(), e.what());
            }
            m_shared_sds.push_back(std::move(sd));
        }
        g_dir_close(d);
    }
}

GwProfile *GwProfileStore::Find(const std::string &id)
{
    for (auto &p : m_profiles) {
        if (p.id == id) {
            return &p;
        }
    }
    return nullptr;
}

std::string GwProfileStore::Create(const std::string &display_name, std::string &err)
{
    GwProfile p;
    p.display_name = display_name.empty() ? GenerateName() : display_name;

    /* id = slug + 4 hex; retry on the (unlikely) collision. */
    std::string root = ProfilesRoot();
    g_mkdir_with_parents(root.c_str(), 0755);
    for (int attempt = 0; attempt < 8; attempt++) {
        char suffix[8];
        g_snprintf(suffix, sizeof(suffix), "%04x", g_random_int_range(0, 0x10000));
        p.id = Slugify(p.display_name) + "-" + suffix;
        p.dir = root + "/" + p.id;
        if (g_mkdir(p.dir.c_str(), 0755) == 0) {
            GDateTime *now = g_date_time_new_now_local();
            char *iso = g_date_time_format_iso8601(now);
            p.created = iso ? iso : "";
            g_free(iso);
            g_date_time_unref(now);

            if (!Save(p, err)) {
                g_rmdir(p.dir.c_str());
                return "";
            }
            m_profiles.push_back(p);
            return p.id;
        }
    }
    err = "cannot create profile directory under " + root;
    return "";
}

std::string GwProfileStore::Duplicate(const std::string &source_id_ref, std::string &err)
{
    std::string source_id = source_id_ref; // Copy to avoid dangling ref if m_profiles reallocates
    GwProfile *src = Find(source_id);
    if (!src) {
        err = "Source profile not found";
        return "";
    }
    
    std::string base_name = src->display_name;
    // Strip trailing "(copy X)" if present
    size_t copy_pos = base_name.find(" (copy ");
    if (copy_pos != std::string::npos && base_name.back() == ')') {
        base_name = base_name.substr(0, copy_pos);
    }
    
    std::string test_name;
    for (int i = 1; i < 1000; i++) {
        test_name = base_name + " (copy " + std::to_string(i) + ")";
        bool exists = false;
        for (const auto &p : m_profiles) {
            if (p.display_name == test_name) {
                exists = true;
                break;
            }
        }
        if (!exists) break;
    }
    
    std::string new_id = Create(test_name, err);
    if (new_id.empty()) return "";
    
    // Re-fetch src since Create might have reallocated m_profiles
    src = Find(source_id);
    GwProfile *dst = Find(new_id);
    if (!src || !dst) return ""; // should not happen

    // Copy contents of directory
    GDir *d = g_dir_open(src->dir.c_str(), 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            if (strcmp(name, "profile.toml") == 0) continue; // Skip toml, Save() writes it later
            std::string src_file = src->dir + "/" + name;
            std::string dst_file = dst->dir + "/" + name;
            
            // Fast synchronous copy
            FILE *in = g_fopen(src_file.c_str(), "rb");
            if (in) {
                FILE *out = g_fopen(dst_file.c_str(), "wb");
                if (out) {
                    const size_t kChunk = 1 << 20;
                    std::vector<uint8_t> buf(kChunk);
                    size_t n;
                    while ((n = fread(buf.data(), 1, kChunk, in)) > 0) {
                        if (fwrite(buf.data(), 1, n, out) != n) break;
                    }
                    fclose(out);
                }
                fclose(in);
            }
        }
        g_dir_close(d);
    }
    
    // Mirror the metadata from source, but keep the new ID, dir, created, display_name
    dst->bank1 = src->bank1;
    dst->bank2 = src->bank2;
    dst->extflash = src->extflash;
    dst->prov_bank1 = src->prov_bank1;
    dst->prov_bank2 = src->prov_bank2;
    dst->prov_extflash = src->prov_extflash;
    dst->sd = src->sd;
    
    Save(*dst, err);
    RecalcDiskUsage(*dst);
    
    return new_id;
}

bool GwProfileStore::Delete(const std::string &id, std::string &err)
{
    GwProfile *p = Find(id);
    if (!p) {
        err = "no such profile: " + id;
        return false;
    }
    /* Flat delete -- profile dirs have no subdirectories by design.
     * Shared SD images live outside the dir and are untouched. */
    GDir *d = g_dir_open(p->dir.c_str(), 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d)) != NULL) {
            g_unlink((p->dir + "/" + name).c_str());
        }
        g_dir_close(d);
    }
    if (g_rmdir(p->dir.c_str()) != 0) {
        err = "cannot remove " + p->dir;
        return false;
    }
    for (size_t i = 0; i < m_profiles.size(); i++) {
        if (m_profiles[i].id == id) {
            m_profiles.erase(m_profiles.begin() + i);
            break;
        }
    }
    return true;
}

bool GwProfileStore::Rename(const std::string &id, const std::string &new_display_name,
                            std::string &err)
{
    GwProfile *p = Find(id);
    if (!p) {
        err = "no such profile: " + id;
        return false;
    }
    std::string old = p->display_name;
    p->display_name = new_display_name.empty() ? GenerateName() : new_display_name;
    if (!Save(*p, err)) {
        p->display_name = old;
        return false;
    }
    return true;
}

uint64_t GwProfileStore::TotalDiskBytes() const
{
    uint64_t total = 0;
    for (const auto &p : m_profiles) {
        total += p.disk_bytes;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Async ops                                                           */

bool GwProfileAsyncOp::StartWorker(std::function<bool(GwProfileAsyncOp *, std::string &)> fn)
{
    if (m_state.load() == Running) {
        return false;
    }
    Join();
    m_error.clear();
    m_done.store(0);
    m_total.store(0);
    m_state.store(Running);
    m_thread = std::thread([this, fn]() {
        std::string err;
        bool ok = fn(this, err);
        if (!ok) {
            m_error = err; /* before the state flip; UI reads after Join */
        }
        m_state.store(ok ? Done : Failed);
    });
    return true;
}

void GwProfileAsyncOp::Join()
{
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool GwProfileAsyncOp::StartCopy(const std::string &src, const std::string &dst)
{
    return StartWorker([src, dst](GwProfileAsyncOp *op, std::string &err) {
        FILE *in = g_fopen(src.c_str(), "rb");
        if (!in) {
            err = "cannot open " + src;
            return false;
        }
        /* Temp-then-rename: a failed copy must not destroy the file already
         * bound to the profile (the store holds copies, not references). */
        std::string tmp = dst + ".tmp";
        FILE *out = g_fopen(tmp.c_str(), "wb");
        if (!out) {
            fclose(in);
            err = "cannot create " + tmp;
            return false;
        }
        fseek(in, 0, SEEK_END);
        op->m_total.store((uint64_t)ftell(in));
        fseek(in, 0, SEEK_SET);

        const size_t kChunk = 1 << 20;
        std::vector<uint8_t> buf(kChunk);
        bool ok = true;
        size_t n;
        while ((n = fread(buf.data(), 1, kChunk, in)) > 0) {
            if (fwrite(buf.data(), 1, n, out) != n) {
                err = "short write to " + tmp;
                ok = false;
                break;
            }
            op->m_done.fetch_add(n);
        }
        if (ok && ferror(in)) {
            err = "read error on " + src;
            ok = false;
        }
        fclose(in);
        if (fclose(out) != 0 && ok) {
            err = "close failed on " + tmp;
            ok = false;
        }
        if (ok && g_rename(tmp.c_str(), dst.c_str()) != 0) {
            err = "cannot replace " + dst;
            ok = false;
        }
        if (!ok) {
            g_unlink(tmp.c_str());
        }
        return ok;
    });
}

bool GwProfileAsyncOp::StartBlank(const std::string &dst, uint64_t size)
{
    return StartWorker([dst, size](GwProfileAsyncOp *op, std::string &err) {
        std::string tmp = dst + ".tmp";
        FILE *out = g_fopen(tmp.c_str(), "wb");
        if (!out) {
            err = "cannot create " + tmp;
            return false;
        }
        op->m_total.store(size);
        const size_t kChunk = 1 << 20;
        std::vector<uint8_t> buf(kChunk, 0xFF);
        uint64_t left = size;
        bool ok = true;
        while (left > 0) {
            size_t n = left < kChunk ? (size_t)left : kChunk;
            if (fwrite(buf.data(), 1, n, out) != n) {
                err = "short write to " + tmp;
                ok = false;
                break;
            }
            left -= n;
            op->m_done.fetch_add(n);
        }
        if (fclose(out) != 0 && ok) {
            err = "close failed on " + tmp;
            ok = false;
        }
        if (ok && g_rename(tmp.c_str(), dst.c_str()) != 0) {
            err = "cannot replace " + dst;
            ok = false;
        }
        if (!ok) {
            g_unlink(tmp.c_str());
        }
        return ok;
    });
}
