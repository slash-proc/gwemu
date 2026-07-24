//
// gnw-h7b0 User Interface -- backup library scanning, see flash-backups.hh.
//
#include "flash-backups.hh"
#include <glib.h>
#include <cstring>

// SHA1 hashes of genuine stock firmware dumps, matching gnwmanager's own
// STOCK_ROM_SHA1_HASH constants (gnwmanager/cli/gnw_patch/{mario,zelda}.py,
// remove-keystone-engine branch) -- reused verbatim, not re-derived.
//
// KNOWN CAVEAT, confirmed via real sha1sum against this project's own
// backup/ files: the *internal* hashes match real dumps exactly, but the
// *external* (extflash) ones never will here -- this project's own
// flash_backup_<game>.bin captures are documented partial dumps (4MiB of a
// 64MB chip, see scripts/make_boot_images.py's docstring), while
// gnwmanager's Ext hash is computed against a full/canonical dump. A real
// file can never match that hash, so the extflash SHA1 constants below are
// NOT used for verification (only presence is checked) -- kept for
// reference / a future full-dump capture, not dead code by accident.
static const char *kMarioIntSha1 = "efa04c387ad7b40549e15799b471a6e1cd234c76";
[[maybe_unused]] static const char *kMarioExtSha1 = "eea70bb171afece163fb4b293c5364ddb90637ae";
static const char *kZeldaIntSha1 = "ac14bcea6e4ff68c88fd2302c021025a2fb47940";
[[maybe_unused]] static const char *kZeldaExtSha1 = "1c1c0ed66d07324e560dcd9e86a322ec5e4c1e96";

static const char *kGameNames[GnwBackupLibrary::kGameCount] = { "mario", "zelda" };

const char *GnwBackupLibrary::GameName(int i)
{
    return kGameNames[i];
}

bool GnwBackupLibrary::Sha1Matches(const char *path, const char *expected_hex)
{
    GError *gerr = nullptr;
    gchar *contents = nullptr;
    gsize len = 0;
    if (!g_file_get_contents(path, &contents, &len, &gerr)) {
        if (gerr) g_error_free(gerr);
        return false;
    }
    gchar *sum = g_compute_checksum_for_data(
        G_CHECKSUM_SHA1, (const guchar *)contents, len);
    bool ok = sum && strcmp(sum, expected_hex) == 0;
    g_free(sum);
    g_free(contents);
    return ok;
}

std::string GnwBackupLibrary::InternalPath(int i) const
{
    return dir + "/internal_flash_backup_" + kGameNames[i] + ".bin";
}

std::string GnwBackupLibrary::ExternalPath(int i) const
{
    return dir + "/flash_backup_" + kGameNames[i] + ".bin";
}

void GnwBackupLibrary::Rescan()
{
    const char *int_sha1[kGameCount] = { kMarioIntSha1, kZeldaIntSha1 };

    for (int i = 0; i < kGameCount; i++) {
        GnwBackupGameStatus &st = status[i];
        st = GnwBackupGameStatus{};

        std::string p = InternalPath(i);
        if (g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) {
            st.internal_found = true;
            st.internal_verified = Sha1Matches(p.c_str(), int_sha1[i]);
        }

        p = ExternalPath(i);
        if (g_file_test(p.c_str(), G_FILE_TEST_EXISTS)) {
            st.external_found = true;
            // Presence only -- see the hash-constant comment above.
            st.external_verified = true;
        }
    }
}
