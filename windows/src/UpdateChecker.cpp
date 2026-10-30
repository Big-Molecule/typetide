#include "UpdateChecker.h"
#include "Http.h"
#include "Json.h"
#include "Settings.h"
#include "Util.h"
#include <shellapi.h>
#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace updatechecker {

namespace {

std::mutex g_mu;
std::wstring g_latestUrl;

/// "v1.2.3" 之类的 tag 拆成数字段
std::vector<int> parseVersion(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    std::vector<int> parts;
    std::string cur;
    for (char c : v + ".") {
        if (c == '.') {
            parts.push_back(cur.empty() ? 0 : atoi(cur.c_str()));
            cur.clear();
        } else if (isdigit((unsigned char)c)) {
            cur += c;
        }
    }
    return parts;
}

bool isNewer(const std::string& latest, const std::string& current) {
    auto l = parseVersion(latest), c = parseVersion(current);
    size_t n = std::max(l.size(), c.size());
    for (size_t i = 0; i < n; ++i) {
        int lp = i < l.size() ? l[i] : 0;
        int cp = i < c.size() ? c[i] : 0;
        if (lp != cp) return lp > cp;
    }
    return false;
}

void checkWorker(HWND notifyWindow, UINT notifyMessage, bool alwaysNotify) {
    // 仓库由编译期常量决定（CMake 的 TYPETIDE_UPDATE_REPO），默认指向本项目 fork
    const std::wstring repo = util::Widen(TYPETIDE_UPDATE_REPO);
    http::Result r = http::Get(L"https://api.github.com/repos/" + repo + L"/releases/latest",
                               L"Accept: application/vnd.github.v3+json\r\n");
    if (!r.ok) {
        if (alwaysNotify && notifyWindow) PostMessageW(notifyWindow, notifyMessage, 2, 0);
        return;
    }
    bool ok = false;
    json::Value v = json::Parse(r.body, &ok);
    if (!ok || v["prerelease"].asBool()) {
        if (alwaysNotify && notifyWindow) PostMessageW(notifyWindow, notifyMessage, 2, 0);
        return;
    }

    std::string tag = v["tag_name"].asString();
    if (tag.empty()) {
        if (alwaysNotify && notifyWindow) PostMessageW(notifyWindow, notifyMessage, 2, 0);
        return;
    }
    const bool newer = isNewer(tag, TYPETIDE_VERSION_STRING);

    if (newer) {
        std::lock_guard lock(g_mu);
        g_latestUrl = util::Widen(v["html_url"].asString());
    }
    if (notifyWindow && (newer || alwaysNotify))
        PostMessageW(notifyWindow, notifyMessage, newer ? 1 : 0, 0);
}

} // namespace

void CheckNow(HWND notifyWindow, UINT notifyMessage) {
    std::thread(checkWorker, notifyWindow, notifyMessage, false).detach();
}

void CheckNowInteractive(HWND notifyWindow, UINT notifyMessage) {
    std::thread(checkWorker, notifyWindow, notifyMessage, true).detach();
}

void CheckIfDue(HWND notifyWindow, UINT notifyMessage) {
    Settings& s = Settings::shared();
    long long now = (long long)time(nullptr);
    if (now - s.lastUpdateCheck < 24 * 3600) return;
    s.lastUpdateCheck = now;
    s.save();
    CheckNow(notifyWindow, notifyMessage);
}

std::wstring ReleasesURL() {
    std::lock_guard lock(g_mu);
    if (!g_latestUrl.empty()) return g_latestUrl;
    return L"https://github.com/" + util::Widen(TYPETIDE_UPDATE_REPO) + L"/releases";
}

void OpenReleasesPage() {
    ShellExecuteW(nullptr, L"open", ReleasesURL().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace updatechecker
