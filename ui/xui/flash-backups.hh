//
// gnw-h7b0 User Interface -- backup-dir library scanning + stock SHA1
// verification, shared by the Flash tab (flash-storage-view) and the
// profile-creation wizard (profile-wizard). Extracted from
// flash-storage-view so both consume one implementation.
//
// This is deliberately the entire extent of "library scanning": filename
// conventions + stock-hash verification. No content inspection of any
// blob, ever (owner decision -- images are opaque).
//
#pragma once
#include <string>

struct GnwBackupGameStatus {
    bool internal_found = false, internal_verified = false;
    bool external_found = false, external_verified = false;
    bool Available() const { return internal_found && internal_verified; }
};

class GnwBackupLibrary {
public:
    static constexpr int kGameCount = 2; // 0=mario, 1=zelda
    static const char *GameName(int i);

    std::string dir = "backup";
    GnwBackupGameStatus status[kGameCount];

    // Re-check the two gnwmanager-convention dumps per game:
    // internal_flash_backup_<game>.bin (SHA1-verified against gnwmanager's
    // STOCK_ROM_SHA1_HASH constants) and flash_backup_<game>.bin (presence
    // only -- our extflash captures are documented-partial 4MiB dumps, the
    // canonical full-dump hash can never match; see the .cc comment).
    void Rescan();

    std::string InternalPath(int i) const;
    std::string ExternalPath(int i) const;

    static bool Sha1Matches(const char *path, const char *expected_hex);
};
