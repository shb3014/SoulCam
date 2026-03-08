#include "soullink/sync_engine.h"

#include "util/logger.h"

#include <cctype>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace sc::soullink {

namespace fs = std::filesystem;

namespace {

struct FileWrite {
    std::string path;
    std::string remote_ref;
    std::vector<uint8_t> content;
};

struct FileRename {
    std::string from_path;
    std::string to_path;
};

struct DiffBundle {
    bool delete_all = false;
    std::string cdn;
    std::vector<std::string> deletes;
    std::vector<FileRename> renames;
    std::vector<FileWrite> writes;
    std::string commit_id;
};

static std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

static bool run_capture(const std::string& command, std::string* out) {
    if (out == nullptr) return false;
    FILE* fp = popen(command.c_str(), "r");
    if (!fp) return false;
    out->clear();
    std::array<char, 4096> buf{};
    while (true) {
        size_t n = fread(buf.data(), 1, buf.size(), fp);
        if (n > 0) out->append(buf.data(), n);
        if (n < buf.size()) break;
    }
    int rc = pclose(fp);
    return rc == 0;
}

static bool fetch_url_bytes(const std::string& url, int timeout_sec, std::vector<uint8_t>* out) {
    if (out == nullptr) return false;
    std::ostringstream cmd;
    cmd << "curl -sS -f -m " << timeout_sec << " " << shell_quote(url);
    std::string body;
    if (!run_capture(cmd.str(), &body)) return false;
    out->assign(body.begin(), body.end());
    return true;
}

static std::string normalize_api(const std::string& api) {
    std::string trimmed = api;
    while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
    if (trimmed.size() >= 13 && trimmed.substr(trimmed.size() - 13) == "/api/git/get-diff") {
        return trimmed;
    }
    return trimmed + "/api/git/get-diff";
}

static std::vector<uint8_t> decode_base64(const std::string& in) {
    static const int8_t kLut[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::vector<uint8_t> out;
    out.reserve((in.size() * 3) / 4);

    int val = 0;
    int valb = -8;
    for (unsigned char c : in) {
        int8_t d = kLut[c];
        if (d == -1) continue;
        if (d == -2) break;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static bool ensure_parent(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    return !ec;
}

static bool path_within_root(const fs::path& root, const fs::path& p) {
    std::error_code ec;
    fs::path cr = fs::weakly_canonical(root, ec);
    if (ec) return false;
    fs::path cp = fs::weakly_canonical(p, ec);
    if (ec) return false;
    std::string rs = cr.string();
    std::string ps = cp.string();
    if (ps.size() < rs.size()) return false;
    return ps.compare(0, rs.size(), rs) == 0;
}

static fs::path resolve_safe(const fs::path& root, const std::string& rel) {
    fs::path p = root / fs::path(rel);
    if (!path_within_root(root, p)) {
        throw std::runtime_error("path escapes sync root: " + rel);
    }
    return p;
}

static bool extract_write_item(const JsonValue& item, FileWrite* out) {
    if (!item.is_object() || out == nullptr) return false;
    const auto* path_v = item.get("path");
    if (!path_v || !path_v->is_string()) return false;
    out->path = path_v->as_string();

    const auto* content_v = item.get("content");
    if (!content_v || !content_v->is_string()) return false;

    std::string encoding = "utf-8";
    if (const auto* enc_v = item.get("encoding"); enc_v && enc_v->is_string()) {
        encoding = enc_v->as_string();
    }
    if (encoding == "base64") {
        out->content = decode_base64(content_v->as_string());
    } else {
        const std::string& s = content_v->as_string();
        out->content.assign(s.begin(), s.end());
    }
    out->remote_ref = out->path;
    return true;
}

static bool extract_rename_item(const JsonValue& item, FileRename* out) {
    if (!item.is_object() || out == nullptr) return false;
    const auto* from_v = item.get("from");
    const auto* to_v = item.get("to");
    if (!from_v || !to_v || !from_v->is_string() || !to_v->is_string()) return false;
    out->from_path = from_v->as_string();
    out->to_path = to_v->as_string();
    return true;
}

static bool extract_path_string(const JsonValue& item, std::string* out) {
    if (out == nullptr) return false;
    if (item.is_string()) {
        *out = item.as_string();
        return true;
    }
    if (item.is_object()) {
        const auto* p = item.get("path");
        if (p && p->is_string()) {
            *out = p->as_string();
            return true;
        }
    }
    return false;
}

static bool parse_diff_bundle(const JsonValue& payload, DiffBundle* out) {
    if (!payload.is_object() || out == nullptr) return false;
    const JsonValue* body = &payload;
    if (const auto* data = payload.get("data"); data && data->is_object()) {
        body = data;
    }

    DiffBundle b;
    if (const auto* del_all = body->get("deleteAll"); del_all) {
        b.delete_all = del_all->as_bool(false);
    }
    if (const auto* cdn = body->get("cdn"); cdn && cdn->is_string()) {
        b.cdn = cdn->as_string();
    }
    if (const auto* commit = body->get("commitID"); commit && commit->is_string()) {
        b.commit_id = commit->as_string();
    } else if (const auto* commit2 = body->get("newCommitID"); commit2 && commit2->is_string()) {
        b.commit_id = commit2->as_string();
    }

    auto collect_delete = [&](const JsonValue* arr) {
        if (!arr || !arr->is_array()) return;
        for (const auto& item : arr->as_array()) {
            std::string p;
            if (extract_path_string(item, &p)) b.deletes.push_back(std::move(p));
        }
    };
    collect_delete(body->get("D"));

    if (const auto* ren = body->get("R"); ren && ren->is_array()) {
        for (const auto& item : ren->as_array()) {
            if (item.is_array() && item.as_array().size() >= 2 &&
                item.as_array()[0].is_string() && item.as_array()[1].is_string()) {
                // SoulFlow format: [newPath, oldPath]
                FileRename r;
                r.to_path = item.as_array()[0].as_string();
                r.from_path = item.as_array()[1].as_string();
                b.renames.push_back(std::move(r));
                continue;
            }
            FileRename r;
            if (extract_rename_item(item, &r)) b.renames.push_back(std::move(r));
        }
    }

    auto collect_write = [&](const JsonValue* arr) {
        if (!arr || !arr->is_array()) return;
        for (const auto& item : arr->as_array()) {
            if (item.is_array() && item.as_array().size() >= 2 &&
                item.as_array()[0].is_string() && item.as_array()[1].is_string()) {
                FileWrite w;
                w.path = item.as_array()[0].as_string();
                w.remote_ref = item.as_array()[1].as_string();
                b.writes.push_back(std::move(w));
                continue;
            }
            FileWrite w;
            if (extract_write_item(item, &w)) b.writes.push_back(std::move(w));
        }
    };
    collect_write(body->get("A"));
    collect_write(body->get("M"));

    if (const auto* files = body->get("files"); files && files->is_array()) {
        for (const auto& item : files->as_array()) {
            if (!item.is_object()) continue;
            std::string op;
            if (const auto* op_v = item.get("op"); op_v && op_v->is_string()) op = op_v->as_string();
            if (op == "D") {
                std::string p;
                if (extract_path_string(item, &p)) b.deletes.push_back(std::move(p));
            } else if (op == "R") {
                FileRename r;
                if (extract_rename_item(item, &r)) b.renames.push_back(std::move(r));
            } else if (op == "A" || op == "M") {
                FileWrite w;
                if (extract_write_item(item, &w)) b.writes.push_back(std::move(w));
            }
        }
    }

    *out = std::move(b);
    return true;
}

}  // namespace

SyncEngine::SyncEngine(const sc::SoullinkConfig& cfg) : cfg_(cfg) {}

std::string SyncEngine::load_commit_id() const {
    std::ifstream in(cfg_.sync_state_path);
    if (!in.good()) return {};
    std::string value;
    std::getline(in, value);
    return trim_copy(value);
}

bool SyncEngine::store_commit_id(const std::string& commit_id) const {
    if (commit_id.empty()) return true;
    fs::path p(cfg_.sync_state_path);
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    if (ec) return false;
    std::ofstream out(p);
    if (!out.good()) return false;
    out << commit_id;
    return true;
}

SyncResult SyncEngine::execute(
    const JsonValue& data,
    const std::function<void(const std::string& stage, const std::string& details)>& progress_cb) {
    if (!data.is_object()) {
        return {false, load_commit_id(), "syncFiles data must be object"};
    }
    const JsonValue* api_v = data.get("api");
    if (!api_v || !api_v->is_string()) {
        return {false, load_commit_id(), "syncFiles requires data.api"};
    }
    std::string commit_id = load_commit_id();
    if (const JsonValue* cid = data.get("commitID"); cid && cid->is_string()) {
        commit_id = cid->as_string();
    }

    if (progress_cb) progress_cb("sync.fetch", "requesting diff payload");

    const std::string url = normalize_api(api_v->as_string());
    JsonValue::Object req_obj;
    req_obj["commitID"] = JsonValue(commit_id);
    req_obj["asFile"] = JsonValue(false);
    std::string req_json = json_dump(JsonValue(req_obj));

    char req_tmp[] = "/tmp/soullink_req_XXXXXX";
    int req_fd = mkstemp(req_tmp);
    if (req_fd < 0) {
        return {false, commit_id, "failed to create temp request file"};
    }
    std::string req_path(req_tmp);
    if (write(req_fd, req_json.data(), req_json.size()) < 0) {
        close(req_fd);
        unlink(req_tmp);
        return {false, commit_id, "failed to write request file"};
    }
    close(req_fd);

    std::ostringstream cmd;
    cmd << "curl -sS -m " << cfg_.sync_timeout_sec
        << " -H 'Content-Type: application/json'"
        << " -X POST --data-binary @" << shell_quote(req_path)
        << " -w '\\n__HTTP_STATUS:%{http_code}\\n' "
        << shell_quote(url);

    std::string response;
    bool ok = run_capture(cmd.str(), &response);
    unlink(req_tmp);
    if (!ok || response.empty()) {
        return {false, commit_id, "syncFiles curl request failed"};
    }

    const std::string marker = "__HTTP_STATUS:";
    size_t marker_pos = response.rfind(marker);
    if (marker_pos == std::string::npos) {
        return {false, commit_id, "syncFiles missing HTTP status marker"};
    }
    std::string body = response.substr(0, marker_pos);
    std::string status_text = trim_copy(response.substr(marker_pos + marker.size()));
    int status = 0;
    try {
        status = std::stoi(status_text);
    } catch (...) {
        status = 0;
    }

    if (status == 204) {
        return {true, commit_id, "No update (HTTP 204)"};
    }
    if (status != 200) {
        std::ostringstream msg;
        msg << "syncFiles HTTP " << status;
        return {false, commit_id, msg.str()};
    }

    JsonValue diff_payload;
    std::string parse_err;
    if (!json_parse(body, &diff_payload, &parse_err)) {
        return {false, commit_id, "invalid diff payload JSON: " + parse_err};
    }

    DiffBundle bundle;
    if (!parse_diff_bundle(diff_payload, &bundle)) {
        return {false, commit_id, "failed to parse diff payload shape"};
    }
    if (!bundle.commit_id.empty()) commit_id = bundle.commit_id;

    fs::path sync_root(cfg_.sync_root);
    std::error_code ec;
    fs::create_directories(sync_root, ec);
    if (ec) {
        return {false, commit_id, "failed to create sync root"};
    }

    if (bundle.delete_all) {
        if (progress_cb) progress_cb("sync.apply", "deleteAll enabled");
        for (const auto& entry : fs::directory_iterator(sync_root, ec)) {
            if (ec) break;
            if (entry.path().filename() == ".git") continue;
            fs::remove_all(entry.path(), ec);
            if (ec) {
                return {false, commit_id, "deleteAll failed for " + entry.path().string()};
            }
        }
    }

    for (const auto& del : bundle.deletes) {
        fs::path p;
        try {
            p = resolve_safe(sync_root, del);
        } catch (const std::exception& ex) {
            return {false, commit_id, ex.what()};
        }
        fs::remove_all(p, ec);
        if (ec) return {false, commit_id, "delete failed: " + del};
    }
    for (const auto& ren : bundle.renames) {
        fs::path from, to;
        try {
            from = resolve_safe(sync_root, ren.from_path);
            to = resolve_safe(sync_root, ren.to_path);
        } catch (const std::exception& ex) {
            return {false, commit_id, ex.what()};
        }
        if (!fs::exists(from, ec)) {
            // tolerate reversed order from custom diff generators
            fs::path alt_from, alt_to;
            try {
                alt_from = resolve_safe(sync_root, ren.to_path);
                alt_to = resolve_safe(sync_root, ren.from_path);
            } catch (const std::exception&) {
                alt_from.clear();
            }
            if (!alt_from.empty() && fs::exists(alt_from, ec)) {
                from = alt_from;
                to = alt_to;
            }
        }
        if (!fs::exists(from, ec)) continue;
        if (!ensure_parent(to)) return {false, commit_id, "rename mkdir failed: " + ren.to_path};
        fs::rename(from, to, ec);
        if (ec) return {false, commit_id, "rename failed: " + ren.from_path};
    }

    auto build_file_url = [&](const std::string& remote_ref) -> std::string {
        if (remote_ref.rfind("http://", 0) == 0 || remote_ref.rfind("https://", 0) == 0) {
            return remote_ref;
        }
        if (bundle.cdn.empty()) return {};
        std::string ref = remote_ref;
        while (!ref.empty() && ref.front() == '/') ref.erase(ref.begin());
        return "http://" + bundle.cdn + "/" + ref;
    };

    for (const auto& wr : bundle.writes) {
        fs::path dst;
        try {
            dst = resolve_safe(sync_root, wr.path);
        } catch (const std::exception& ex) {
            return {false, commit_id, ex.what()};
        }
        if (!ensure_parent(dst)) return {false, commit_id, "write mkdir failed: " + wr.path};
        std::vector<uint8_t> bytes = wr.content;
        if (bytes.empty()) {
            std::string url = build_file_url(wr.remote_ref.empty() ? wr.path : wr.remote_ref);
            if (url.empty()) return {false, commit_id, "missing CDN info for file write: " + wr.path};
            if (progress_cb) progress_cb("sync.download", wr.path);
            if (!fetch_url_bytes(url, cfg_.sync_timeout_sec, &bytes)) {
                return {false, commit_id, "download failed: " + wr.path};
            }
        }
        fs::path tmp = dst;
        tmp += ".soullink_tmp";
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.good()) return {false, commit_id, "write failed: " + wr.path};
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        fs::rename(tmp, dst, ec);
        if (ec) return {false, commit_id, "replace failed: " + wr.path};
    }

    if (!store_commit_id(commit_id)) {
        SC_LOG_WARN("Soullink sync: failed to persist commit ID");
    }
    if (progress_cb) {
        std::ostringstream detail;
        detail << "D=" << bundle.deletes.size()
               << " R=" << bundle.renames.size()
               << " W=" << bundle.writes.size();
        progress_cb("sync.apply", detail.str());
    }
    return {true, commit_id, "syncFiles completed"};
}

}  // namespace sc::soullink

