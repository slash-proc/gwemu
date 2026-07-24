//
// gnw-h7b0 User Interface -- SD Card tab (Phase 2)
//
#include "sdcard-view.hh"
#include "widgets.hh"
#include <cstdio>
#include <cstring>

// This project's device-model tools (gnw-make-boot-images etc.) only ever
// take fixed/hardcoded game-name arguments, so string-concatenating a
// popen() shell command has been safe so far. m_content_dir is different:
// it comes directly from a user-typed path or a native file-picker dialog,
// so it needs real shell quoting -- wrap in single quotes, escaping any
// embedded single quote as '\'' (close quote, escaped literal quote,
// reopen quote), the standard POSIX-shell-safe technique.
static std::string ShellQuote(const std::string &s)
{
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

GnwSdCardView::~GnwSdCardView()
{
    if (m_worker.joinable()) m_worker.join();
}

void GnwSdCardView::StartBuild()
{
    if (m_content_dir.empty()) {
        std::lock_guard<std::mutex> lock(m_output_mutex);
        m_output = "no SD card content folder set";
        m_state = BuildState::Failed;
        return;
    }
    if (m_worker.joinable()) m_worker.join(); // previous run already Done/Failed

    static const char *kSizes[3] = { "8G", "16G", "32G" };
    std::string cmd = "python3 scripts/make_sdcard_image.py --content " +
                       ShellQuote(m_content_dir) + " --size " + kSizes[m_size_choice] +
                       " --out backup/qemu-images/sdcard.qcow2 2>&1";

    m_state = BuildState::Running;
    {
        std::lock_guard<std::mutex> lock(m_output_mutex);
        m_output.clear();
    }

    m_worker = std::thread([this, cmd]() {
        FILE *p = popen(cmd.c_str(), "r");
        if (!p) {
            std::lock_guard<std::mutex> lock(m_output_mutex);
            m_output = "failed to launch make_sdcard_image.py";
            m_state = BuildState::Failed;
            return;
        }
        char buf[256];
        std::string output;
        while (fgets(buf, sizeof(buf), p)) output += buf;
        int rc = pclose(p);

        std::lock_guard<std::mutex> lock(m_output_mutex);
        if (rc != 0) {
            m_output = "make_sdcard_image.py failed: " + output;
            m_state = BuildState::Failed;
        } else {
            m_output = "Built.";
            m_state = BuildState::Done;
        }
    });
}

void GnwSdCardView::JoinWorkerIfDone()
{
    BuildState s = m_state.load();
    if ((s == BuildState::Done || s == BuildState::Failed) && m_worker.joinable()) {
        m_worker.join(); // thread function already returned, this is instant
    }
}

void GnwSdCardView::Draw()
{
    JoinWorkerIfDone();

    SectionTitle("SD Card");
    static char dir_buf[512] = "";
    strncpy(dir_buf, m_content_dir.c_str(), sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = 0;

    bool running = (m_state.load() == BuildState::Running);
    ImGui::BeginDisabled(running);

    if (ImGui::InputText("Content Folder", dir_buf, sizeof(dir_buf))) {
        m_content_dir = dir_buf;
    }
    ImGui::SameLine();
    InlineFileField("##contentdir", m_content_dir.c_str(), nullptr, 0, true,
                    [this](const char *p) { m_content_dir = p; });

    const char *size_items[3] = { "8G", "16G", "32G" };
    ImGui::Combo("Size", &m_size_choice, size_items, 3);

    if (ImGui::Button("Build SD Image")) {
        StartBuild();
    }
    ImGui::EndDisabled();

    if (running) {
        ImGui::SameLine();
        ImGui::Text("Building... this can take a few minutes.");
    }

    std::string status;
    bool is_error;
    {
        std::lock_guard<std::mutex> lock(m_output_mutex);
        status = m_output;
        is_error = (m_state.load() == BuildState::Failed);
    }
    if (!status.empty() && !running) {
        ImGui::TextColored(is_error ? ImVec4(1, 0.3f, 0.3f, 1) : ImVec4(0.3f, 1, 0.3f, 1),
                            "%s", status.c_str());
    }
}
