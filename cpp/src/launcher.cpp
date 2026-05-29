// Play.exe launcher.
//
// Behaves like a Steam game shortcut: when double-clicked it walks the
// project, sees if any source file changed since the last build, runs
// build.bat if needed (showing a console with progress so failures are
// visible), then launches vlrgui.exe.
//
// If launched from outside the project tree (e.g. a Desktop shortcut), it
// scans up to ~5 parent directories looking for build.bat / src / include
// to anchor itself.

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

bool is_project_root(const fs::path& d) {
    return fs::exists(d / "build.bat")
        && fs::exists(d / "src")
        && fs::exists(d / "include");
}

fs::path find_project_root() {
    char exepath[MAX_PATH];
    GetModuleFileNameA(nullptr, exepath, MAX_PATH);
    fs::path here = fs::absolute(fs::path(exepath)).parent_path();

    // Walk up the directory tree looking for the project root. Hard cap at
    // ~6 levels so an exe sitting on a network drive can't run forever.
    for (int i = 0; i < 6; ++i) {
        if (is_project_root(here)) return here;
        fs::path parent = here.parent_path();
        if (parent == here) break;
        here = parent;
    }

    // Hard fallback: well-known install path.
    fs::path hard = "C:/Users/fulls/Desktop/Valosim/cpp";
    if (is_project_root(hard)) return hard;

    return {};
}

bool any_source_newer(const fs::path& root, const fs::path& exe) {
    if (!fs::exists(exe)) return true;
    auto exe_t = fs::last_write_time(exe);
    auto check_dir = [&](const fs::path& d) {
        if (!fs::exists(d)) return false;
        for (auto& e : fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied)) {
            if (!e.is_regular_file()) continue;
            std::error_code ec;
            auto t = fs::last_write_time(e.path(), ec);
            if (!ec && t > exe_t) return true;
        }
        return false;
    };
    if (check_dir(root / "src"))     return true;
    if (check_dir(root / "include")) return true;
    if (fs::exists(root / "build.bat")) {
        std::error_code ec;
        auto t = fs::last_write_time(root / "build.bat", ec);
        if (!ec && t > exe_t) return true;
    }
    return false;
}

// Run build.bat in a visible console so the user can see progress / errors.
// Wait for it to finish; return its exit code.
DWORD run_build(const fs::path& root) {
    std::string cmd_line = "cmd.exe /c \"" + (root / "build.bat").string() + "\" release";
    STARTUPINFOA si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    std::vector<char> mut_cmd(cmd_line.begin(), cmd_line.end());
    mut_cmd.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,
        mut_cmd.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NEW_CONSOLE,
        nullptr,
        root.string().c_str(),
        &si, &pi);
    if (!ok) return ~0u;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exit_code;
}

void launch_game(const fs::path& exe) {
    SHELLEXECUTEINFOA sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "open";
    std::string s = exe.string();
    sei.lpFile = s.c_str();
    std::string dir = exe.parent_path().string();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    if (ShellExecuteExA(&sei) && sei.hProcess) {
        // Don't wait — the launcher exits, the game keeps running.
        CloseHandle(sei.hProcess);
    }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    fs::path root = find_project_root();

    // Standalone fallback: a sibling vlrgui.exe with no source tree.
    if (root.empty()) {
        char exepath[MAX_PATH];
        GetModuleFileNameA(nullptr, exepath, MAX_PATH);
        fs::path sibling = fs::path(exepath).parent_path() / "vlrgui.exe";
        if (fs::exists(sibling)) {
            launch_game(sibling);
            return 0;
        }
        MessageBoxA(nullptr,
            "Could not find the project source.\n\n"
            "Place Play.exe inside the cpp/ folder, or next to vlrgui.exe.",
            "VLR Manager", MB_OK | MB_ICONERROR);
        return 1;
    }

    fs::path exe = root / "build" / "vlrgui.exe";
    bool need_build = any_source_newer(root, exe);

    if (need_build) {
        // Quick toast so users with a slow build don't think nothing happened.
        // Non-blocking — shown for ~700ms via a separate thread is overkill;
        // we just rely on build.bat's own console window.
        DWORD rc = run_build(root);
        if (rc != 0) {
            MessageBoxA(nullptr,
                "Build failed. The build console window has the details.\n"
                "Fix the errors and try again.",
                "VLR Manager", MB_OK | MB_ICONERROR);
            return 1;
        }
    }

    if (!fs::exists(exe)) {
        MessageBoxA(nullptr,
            "Build succeeded but vlrgui.exe is missing.",
            "VLR Manager", MB_OK | MB_ICONERROR);
        return 1;
    }
    launch_game(exe);
    return 0;
}
