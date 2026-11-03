// Update checks on start, the "Update available" dialog and installing (Settings > Updates has the version list).
#include "app_impl.hpp"
#include "devscript.hpp"
#include <cstdlib>

bool App::Impl::updateEligible(const Release& r) {
    return compareVersions(r.version, appVersion()) > 0 && (!r.prerelease || store.setting("update.pre", "0") == "1");
}

void App::Impl::updateCheck(bool startup) {
    up.started = true, up.startup = startup;
    up.check.run([] {
        UpdateUi::Checked c;
        c.releases = fetchReleases(&c.err);
        return c;
    });
}

// now: quit as soon as the package is downloaded and the helper waits (it installs and restarts opcan);
// else (automatic mode) the helper installs whenever the user quits, without restarting.
void App::Impl::updateInstall(const Release& r, bool now) {
    up.installing = r.version, up.quitWhenReady = now;
    up.install.run([r, now] { return prepareInstall(r, /*silent*/ true, /*relaunch*/ now); });
}

void App::Impl::updateFrame() {
    auto& u = up;
    if (!u.started) {
        std::string mode = store.setting("update.mode", "ask");
        bool dev = devscript::active() || std::getenv("OPCAN_SHOT");  // screenshot checks stay offline
        if (mode != "off" && !dev) updateCheck(true);
        u.started = true;
    }
    if (u.check.busy() || u.install.busy()) animUntil = ImGui::GetTime() + 0.25;
    if (auto c = u.check.take()) {
        u.releases = std::move(c->releases), u.err = c->err, u.checked = true;
        const Release* best = nullptr;
        for (auto& r : u.releases)
            if (updateEligible(r)) {
                best = &r;
                break;
            }
        std::string mode = store.setting("update.mode", "ask");
        if (u.startup && best && mode != "off" && store.setting("update.skip") != best->version) {
            if (!installable(*best)) notify("opcan " + best->version + " is out: see Settings > Updates");
            else if (mode == "auto" && replacesInPlace(installKind())) updateInstall(*best, false);
            else u.offer = *best;
        }
        u.startup = false;
    }
    if (auto r = u.install.take()) {
        if (!r->empty()) notify("Update failed: " + *r);
        else if (u.stayAfter) store.setSetting("update.mode", "off");
        if (r->empty() && u.quitWhenReady) quit = true;  // the helper installs and restarts opcan
        else if (r->empty()) notify("opcan " + u.installing + " will be installed when you close opcan");
        u.stayAfter = false;
    }
    drawUpdateDialog();
}

void App::Impl::drawUpdateDialog() {
    auto& u = up;
    if (u.offer && !ImGui::IsPopupOpen("Update available")) ImGui::OpenPopup("Update available");
    ImGui::SetNextWindowSize({520, 0}, ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Update available", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize))
        return;
    auto& r = *u.offer;
    ImGui::PushFont(f.bold, 20);
    ImGui::Text("opcan %s", r.version.c_str());
    ImGui::PopFont();
    ImGui::TextDisabled("You have %s%s%s", appVersion(), r.published.empty() ? "" : "  ·  released ", r.published.c_str());
    if (auto notes = plainNotes(r.notes); !notes.empty()) {
        ImGui::Dummy({0, 4});
        float h = 16;
        for (auto& l : notes) h += l.text.empty() ? 8 : ImGui::CalcTextSize(l.text.c_str(), nullptr, false, 460).y + 4;
        ImGui::BeginChild("notes", {500, std::min(220.f, h)}, ImGuiChildFlags_Borders);
        for (auto& l : notes) {
            if (l.text.empty()) {
                ImGui::Dummy({0, 4});
                continue;
            }
            if (l.heading) ImGui::PushFont(f.bold, 0);
            ImGui::TextWrapped("%s", l.text.c_str());
            if (l.heading) ImGui::PopFont();
        }
        ImGui::EndChild();
    }
    ImGui::Dummy({0, 6});
    bool busy = u.install.busy();
    auto close = [&] {
        u.offer.reset();
        ImGui::CloseCurrentPopup();
    };
    if (busy) {
        spinner(ImGui::GetWindowDrawList(), {ImGui::GetCursorScreenPos().x + 8, ImGui::GetCursorScreenPos().y + 10}, 6, theme::accent);
        ImGui::Dummy({20, 20});
        ImGui::SameLine();
        ImGui::TextDisabled(installKind() == InstallKind::Nix ? "Building with Nix (can take a while)… opcan restarts when it's ready."
                                                              : "Downloading… opcan restarts when it's installed.");
    } else {
        if (accentButton("Install and restart", {180, 34})) updateInstall(r, true);
        ImGui::SameLine();
        if (ImGui::Button("Later", {100, 34}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close();
        ImGui::SameLine();
        if (ImGui::Button("Skip this version", {150, 34})) store.setSetting("update.skip", r.version), close();
        ImGui::TextDisabled("Settings > Updates: automatic updates, staying on a version, or any other version.");
    }
    ImGui::EndPopup();
}
