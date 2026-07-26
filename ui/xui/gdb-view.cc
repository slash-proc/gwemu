//
// gnw-h7b0 User Interface -- GDB stub settings section (System tab).
//
#include "common.hh"
#include "gdb-view.hh"
#include "widgets.hh"
#include "gnw-style-tokens.hh"
#include "viewport-manager.hh"
#include "../gwemu-settings.h"
#include "../gwemu-notifications.h"
#include "../gwemu-gdbstub.h"

#include <string>

#define GDB_ADDR_MAX 64

static char g_addr_buf[GDB_ADDR_MAX];
static bool g_addr_buf_init;
static std::string g_gdb_error;

static void SyncAddrBuf()
{
    const char *cur = g_config.sys.gdb.address ? g_config.sys.gdb.address : "";
    snprintf(g_addr_buf, sizeof(g_addr_buf), "%s", cur);
    g_addr_buf_init = true;
}

// gdbserver_start() only creates a listening socket (wait=off, server=on)
// -- it does not block waiting for a client -- so calling it straight
// from the widget handler is safe under the repo's async discipline.
static void ApplyNow()
{
    char err[256];
    bool ok = gwemu_gdbstub_apply(g_config.sys.gdb.enable,
                                  g_config.sys.gdb.address,
                                  g_config.sys.gdb.port,
                                  err, sizeof(err));
    if (ok) {
        g_gdb_error.clear();
    } else {
        g_gdb_error = err;
        // Don't leave the toggle claiming a stub that isn't there.
        g_config.sys.gdb.enable = false;
        gwemu_queue_error_message(err);
    }
    gwemu_settings_save();
}

static bool g_startup_pending;

void ApplyGdbSettingsAtStartup()
{
    // The HUD is initialised before machine init has created any CPU, and
    // gdbserver_start() refuses to attach without one -- so only arm it
    // here and let GdbSettingsTick() do the work once a CPU exists.
    g_startup_pending = g_config.sys.gdb.enable;
}

void GdbSettingsTick()
{
    if (!g_startup_pending || !gwemu_gdbstub_machine_ready()) {
        return;
    }
    g_startup_pending = false;

    char err[256];
    if (!gwemu_gdbstub_apply(true, g_config.sys.gdb.address,
                             g_config.sys.gdb.port, err, sizeof(err))) {
        g_gdb_error = err;
        g_config.sys.gdb.enable = false;
        fprintf(stderr, "gwemu: %s\n", err);
    }
}

void DrawGdbSettings()
{
    if (!g_addr_buf_init) {
        SyncAddrBuf();
    }

    SectionTitle("GDB Debugging");

    if (Toggle("Enable GDB stub", &g_config.sys.gdb.enable,
               "Listen for a GDB remote connection (target remote "
               "<address>:<port>). Takes effect immediately -- no "
               "restart needed.")) {
        // Commit a pending address edit before (re)starting.
        gwemu_settings_set_string(&g_config.sys.gdb.address, g_addr_buf);
        ApplyNow();
    }

    ImGui::SetNextItemWidth(200.0f * g_viewport_mgr.m_scale);
    if (ImGui::InputTextWithHint("Listen address", "127.0.0.1", g_addr_buf,
                                 sizeof(g_addr_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        gwemu_settings_set_string(&g_config.sys.gdb.address, g_addr_buf);
        ApplyNow();
    } else if (ImGui::IsItemDeactivatedAfterEdit()) {
        gwemu_settings_set_string(&g_config.sys.gdb.address, g_addr_buf);
        ApplyNow();
    }
    ImGui::SameLine();
    HelpMarker("Leave as 127.0.0.1 unless you really need to debug from "
               "another machine. Empty binds every interface. IPv6 "
               "literals must be bracketed, e.g. [::1].");

    ImGui::SetNextItemWidth(200.0f * g_viewport_mgr.m_scale);
    int port = g_config.sys.gdb.port;
    if (ImGui::InputInt("Port", &port)) {
        if (port < 1) port = 1;
        if (port > 65535) port = 65535;
        g_config.sys.gdb.port = port;
        ApplyNow();
    }

    ImGui::Spacing();

    if (gwemu_gdbstub_is_running()) {
        ImGui::TextColored(GNW_COL_ACCENT, "Listening on %s",
                           gwemu_gdbstub_device_string(g_config.sys.gdb.address,
                                                       g_config.sys.gdb.port));
    } else {
        ImGui::TextDisabled("Stub not running.");
    }

    if (!g_gdb_error.empty()) {
        ImGui::TextColored(GNW_COL_ERR, "%s", g_gdb_error.c_str());
    }

    ImGui::Spacing();
    ImGui::TextColored(GNW_COL_WARN,
        "Note: the whole machine halts while a debugger is attached.");
    ImGui::TextWrapped(
        "That is normal QEMU gdbstub behaviour -- connecting stops the "
        "guest, and every command except plain memory read/write keeps it "
        "stopped until you 'continue'. The emulator will look frozen; it "
        "isn't. Also note the stub is unauthenticated: anyone who can "
        "reach the port gets full read/write access to guest memory, "
        "which is why the address defaults to loopback.");
}
