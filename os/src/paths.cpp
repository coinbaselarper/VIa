#include "internal.h"

#include <cctype>
#include <cerrno>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace gta6rt_os;

namespace {

struct Prefix {
    std::string win_lower; // '/' separators, no trailing separator, lower case
    std::string win_display;
    std::string host;      // no trailing '/'
};

pthread_mutex_t g_path_mx = PTHREAD_MUTEX_INITIALIZER;
std::vector<Prefix>* g_prefixes;
std::string* g_root;

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string strip_trailing_slashes(std::string s) {
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

// Splits on '/', folds "." and "..", and applies the Win32 trailing dot/space rule to each component.
std::string fold(const std::string& path, bool absolute, bool win_rules) {
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string c = path.substr(i, j - i);
        i = j + 1;
        if (c.empty() || c == ".") continue;
        if (c == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!absolute) parts.push_back("..");
            continue;
        }
        if (win_rules) {
            while (!c.empty() && (c.back() == '.' || c.back() == ' ')) c.pop_back();
            if (c.empty()) continue;
        }
        parts.push_back(c);
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); k++) {
        if (k) out += '/';
        out += parts[k];
    }
    if (out.empty()) out = ".";
    return out;
}

bool exists(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}

// Resolves components that do not exist with this spelling by a case-insensitive directory scan.
std::string resolve_case(const std::string& p) {
    if (exists(p)) return p;
    bool absolute = !p.empty() && p[0] == '/';
    std::string cur = absolute ? "/" : ".";
    size_t i = absolute ? 1 : 0;
    auto join = [](const std::string& a, const std::string& b) { return a == "/" ? "/" + b : a + "/" + b; };
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string comp = p.substr(i, j - i);
        i = j + 1;
        if (comp.empty()) continue;
        std::string cand = join(cur, comp);
        if (!exists(cand)) {
            std::string found;
            if (DIR* d = opendir(cur.c_str())) {
                while (dirent* e = readdir(d))
                    if (!strcasecmp(e->d_name, comp.c_str())) {
                        found = e->d_name;
                        break;
                    }
                closedir(d);
            }
            if (found.empty()) {
                if (i <= p.size()) cand += "/" + p.substr(i);
                cur = cand;
                break;
            }
            cand = join(cur, found);
        }
        cur = cand;
    }
    if (!absolute && cur.rfind("./", 0) == 0) cur = cur.substr(2);
    return cur;
}

bool translate(const std::string& in, std::string* out, uint32_t* err) {
    std::string s = in;
    for (char& c : s)
        if (c == '\\') c = '/';
    if (s.rfind("//?/UNC/", 0) == 0 || (s.rfind("//", 0) == 0 && s.rfind("//?/", 0) != 0 && s.rfind("//./", 0) != 0)) {
        *err = 53; // ERROR_BAD_NETPATH
        return false;
    }
    if (s.rfind("//?/", 0) == 0 || s.rfind("//./", 0) == 0) s = s.substr(4);
    pthread_mutex_lock(&g_path_mx);
    std::string ls = lower(s);
    const Prefix* best = nullptr;
    if (g_prefixes)
        for (const Prefix& p : *g_prefixes) {
            if (ls.compare(0, p.win_lower.size(), p.win_lower) != 0) continue;
            char next = ls.size() > p.win_lower.size() ? ls[p.win_lower.size()] : '/';
            if (next != '/' && p.win_lower.back() != ':') continue;
            if (!best || p.win_lower.size() > best->win_lower.size()) best = &p;
        }
    std::string host;
    bool ok = true;
    bool win_rules = true;
    if (best) {
        host = best->host + "/" + s.substr(best->win_lower.size());
    } else if (s.size() >= 2 && s[1] == ':' && isalpha((unsigned char)s[0])) {
        char d = (char)tolower((unsigned char)s[0]);
        std::string rest = s.substr(2);
        if (d == 'z') host = "/" + rest;
        else if (g_root && !g_root->empty()) host = *g_root + "/" + std::string(1, d) + "/" + rest;
        else ok = false;
    } else {
        host = s;
        win_rules = s.empty() || s[0] != '/';
    }
    pthread_mutex_unlock(&g_path_mx);
    if (!ok) {
        *err = ERROR_PATH_NOT_FOUND;
        return false;
    }
    bool absolute = !host.empty() && host[0] == '/';
    *out = resolve_case(fold(host, absolute, win_rules));
    return true;
}

} // namespace

namespace gta6rt_os {

std::string win_to_host_path8(const char* p, uint32_t* err) {
    std::string out;
    uint32_t e = ERROR_PATH_NOT_FOUND;
    if (!p || !*p) {
        if (err) *err = ERROR_PATH_NOT_FOUND;
        return out;
    }
    if (!translate(p, &out, &e)) {
        if (err) *err = e;
        return std::string();
    }
    if (err) *err = 0;
    return out;
}

std::string win_to_host_path(const char16_t* p, uint32_t* err) {
    if (!p) {
        if (err) *err = ERROR_INVALID_PARAMETER;
        return std::string();
    }
    return win_to_host_path8(u16_to_utf8(p).c_str(), err);
}

std::u16string host_to_win_path(const std::string& host) {
    std::string h = host;
    std::string out;
    pthread_mutex_lock(&g_path_mx);
    const Prefix* best = nullptr;
    if (g_prefixes && !h.empty() && h[0] == '/')
        for (const Prefix& p : *g_prefixes) {
            if (h.compare(0, p.host.size(), p.host) != 0) continue;
            if (h.size() > p.host.size() && h[p.host.size()] != '/') continue;
            if (!best || p.host.size() > best->host.size()) best = &p;
        }
    if (best) {
        std::string rest = h.substr(best->host.size());
        out = best->win_display;
        if (out.back() != '/' && (rest.empty() || rest[0] != '/')) out += '/';
        out += rest;
        if (out.size() >= 2 && out.back() == '/' && out[out.size() - 2] != ':') out.pop_back();
    } else if (g_root && !g_root->empty() && h.compare(0, g_root->size() + 1, *g_root + "/") == 0 &&
               h.size() >= g_root->size() + 2 && isalpha((unsigned char)h[g_root->size() + 1]) &&
               (h.size() == g_root->size() + 2 || h[g_root->size() + 2] == '/')) {
        out = std::string(1, (char)toupper((unsigned char)h[g_root->size() + 1])) + ":" + h.substr(g_root->size() + 2);
        if (out.size() == 2) out += '/';
    } else if (!h.empty() && h[0] == '/') {
        out = "Z:" + h;
    } else {
        out = h;
    }
    pthread_mutex_unlock(&g_path_mx);
    for (char& c : out)
        if (c == '/') c = '\\';
    return utf8_to_u16(out.c_str());
}

} // namespace gta6rt_os

extern "C" {

gta6rt_status gta6rt_path_set_root(const char* host_dir_utf8_or_null) {
    pthread_mutex_lock(&g_path_mx);
    if (!g_root) g_root = new std::string();
    *g_root = host_dir_utf8_or_null ? strip_trailing_slashes(host_dir_utf8_or_null) : std::string();
    pthread_mutex_unlock(&g_path_mx);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_path_map_prefix(const char* win_prefix_utf8, const char* host_dir_utf8_or_null) {
    if (!win_prefix_utf8 || !*win_prefix_utf8) return GTA6RT_E_INVALID_ARG;
    std::string w = win_prefix_utf8;
    for (char& c : w)
        if (c == '\\') c = '/';
    while (w.size() > 1 && w.back() == '/') w.pop_back();
    std::string wl = lower(w);
    pthread_mutex_lock(&g_path_mx);
    if (!g_prefixes) g_prefixes = new std::vector<Prefix>();
    for (auto it = g_prefixes->begin(); it != g_prefixes->end(); ++it)
        if (it->win_lower == wl) {
            g_prefixes->erase(it);
            break;
        }
    if (host_dir_utf8_or_null) g_prefixes->push_back(Prefix{wl, w, strip_trailing_slashes(host_dir_utf8_or_null)});
    pthread_mutex_unlock(&g_path_mx);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_path_to_host(const char* win_path_utf8, char* out, size_t out_size) {
    uint32_t err = 0;
    std::string h = win_to_host_path8(win_path_utf8, &err);
    if (err) return GTA6RT_E_NOT_FOUND;
    if (!out || h.size() + 1 > out_size) return GTA6RT_E_INVALID_ARG;
    memcpy(out, h.c_str(), h.size() + 1);
    return GTA6RT_OK;
}

gta6rt_status gta6rt_path_to_win(const char* host_path_utf8, char* out, size_t out_size) {
    if (!host_path_utf8) return GTA6RT_E_INVALID_ARG;
    std::string w = u16_to_utf8(host_to_win_path(host_path_utf8).c_str());
    if (!out || w.size() + 1 > out_size) return GTA6RT_E_INVALID_ARG;
    memcpy(out, w.c_str(), w.size() + 1);
    return GTA6RT_OK;
}

} // extern "C"
