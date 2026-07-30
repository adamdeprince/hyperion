#include "options.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace hyperion {
namespace {

std::string need_value(const std::string& flag, int& i, int argc, char** argv) {
  if (i + 1 >= argc) {
    throw std::runtime_error("missing value for " + flag);
  }
  return argv[++i];
}

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string strip_quotes(std::string s) {
  s = trim(std::move(s));
  if (s.size() >= 2) {
    const char a = s.front();
    const char b = s.back();
    if ((a == '"' && b == '"') || (a == '\'' && b == '\'')) {
      s = s.substr(1, s.size() - 2);
    }
  }
  return s;
}

// basename without directory; strip a single trailing extension if present.
std::string path_stem(const std::string& path) {
  std::string base = path;
  const auto slash = base.find_last_of("/\\");
  if (slash != std::string::npos) {
    base = base.substr(slash + 1);
  }
  const auto dot = base.find_last_of('.');
  if (dot != std::string::npos && dot > 0) {
    base = base.substr(0, dot);
  }
  return base;
}

std::unordered_map<std::string, std::string> parse_env_file(
    const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open account env: " + path);
  }
  std::unordered_map<std::string, std::string> kv;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::string t = trim(line);
    if (t.empty() || t[0] == '#') {
      continue;
    }
    if (t.rfind("export ", 0) == 0) {
      t = trim(t.substr(7));
    }
    const auto eq = t.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    std::string key = trim(t.substr(0, eq));
    std::string val = strip_quotes(t.substr(eq + 1));
    if (!key.empty()) {
      kv[key] = std::move(val);
    }
  }
  return kv;
}

const std::string* find_key(
    const std::unordered_map<std::string, std::string>& kv,
    std::initializer_list<const char*> keys) {
  for (const char* k : keys) {
    auto it = kv.find(k);
    if (it != kv.end() && !it->second.empty()) {
      return &it->second;
    }
  }
  return nullptr;
}

std::string require_key(const std::unordered_map<std::string, std::string>& kv,
                        std::initializer_list<const char*> keys,
                        const std::string& env_path) {
  if (const std::string* v = find_key(kv, keys)) {
    return *v;
  }
  std::string want;
  for (const char* k : keys) {
    if (!want.empty()) {
      want += " | ";
    }
    want += k;
  }
  throw std::runtime_error("account env " + env_path + " missing " + want);
}

// Client-side HTTP base: if jinghong binds 0.0.0.0, we still connect to loopback.
std::string http_base_from_env(
    const std::unordered_map<std::string, std::string>& kv) {
  std::string bind = "127.0.0.1";
  if (const std::string* b = find_key(kv, {"JINGHONG_HTTP_BIND", "HTTP_BIND"})) {
    bind = *b;
  }
  if (bind == "0.0.0.0" || bind == "::" || bind == "[::]" || bind == "*") {
    bind = "127.0.0.1";
  }
  // Strip brackets from IPv6 literals for display; keep as-is if already host.
  const std::string port =
      require_key(kv, {"JINGHONG_HTTP_PORT", "HTTP_PORT"}, "(account env)");
  return "http://" + bind + ":" + port;
}

std::string derive_alpaca_channel(const std::string& status_channel,
                                  const std::string& order_channel) {
  const std::string sp = "ACCOUNT:STATUS";
  if (status_channel.rfind(sp, 0) == 0) {
    return "ACCOUNT:ALPACA" + status_channel.substr(sp.size());
  }
  const std::string op = "ACCOUNT:ORDER";
  if (order_channel.rfind(op, 0) == 0) {
    return "ACCOUNT:ALPACA" + order_channel.substr(op.size());
  }
  return "ACCOUNT:ALPACA";
}

void apply_master_json(Options& opt, const nlohmann::json& j) {
  if (!j.is_object()) {
    throw std::runtime_error("master config root must be a JSON object");
  }
  auto str = [&](const char* k) -> std::string {
    if (!j.contains(k) || j[k].is_null()) {
      return {};
    }
    if (j[k].is_string()) {
      return j[k].get<std::string>();
    }
    throw std::runtime_error(std::string("config.") + k + " must be a string");
  };
  auto num = [&](const char* k, double def) {
    if (!j.contains(k) || j[k].is_null()) {
      return def;
    }
    if (j[k].is_number()) {
      return j[k].get<double>();
    }
    throw std::runtime_error(std::string("config.") + k + " must be a number");
  };
  auto inum = [&](const char* k, int def) {
    return static_cast<int>(num(k, static_cast<double>(def)));
  };

  if (auto r = str("redis_path"); !r.empty()) {
    opt.redis_path = std::move(r);
  }
  opt.ui_hz = num("ui_hz", opt.ui_hz);
  opt.equity_sample_hz = num("equity_hz", opt.equity_sample_hz);
  if (j.contains("equity_sample_hz")) {
    opt.equity_sample_hz = num("equity_sample_hz", opt.equity_sample_hz);
  }
  opt.account_poll_sec = inum("account_poll_sec", opt.account_poll_sec);
  opt.http_poll_sec = inum("http_poll_sec", opt.http_poll_sec);
  opt.alpaca_limit_qpm = inum("alpaca_limit_qpm", opt.alpaca_limit_qpm);
  if (j.contains("equity_history_points") && j["equity_history_points"].is_number()) {
    opt.equity_history_points =
        static_cast<std::size_t>(j["equity_history_points"].get<std::int64_t>());
  }
  if (j.contains("price_history_points") && j["price_history_points"].is_number()) {
    opt.price_history_points =
        static_cast<std::size_t>(j["price_history_points"].get<std::int64_t>());
  }

  if (!j.contains("accounts") || !j["accounts"].is_array() ||
      j["accounts"].empty()) {
    throw std::runtime_error(
        "master config must include a non-empty \"accounts\" array");
  }

  for (const auto& entry : j["accounts"]) {
    std::string env_path;
    std::string id_override;
    std::string label_override;
    if (entry.is_string()) {
      env_path = entry.get<std::string>();
    } else if (entry.is_object()) {
      if (entry.contains("env") && entry["env"].is_string()) {
        env_path = entry["env"].get<std::string>();
      } else if (entry.contains("path") && entry["path"].is_string()) {
        env_path = entry["path"].get<std::string>();
      } else {
        throw std::runtime_error(
            "accounts[] object needs \"env\" (path to account .env)");
      }
      if (entry.contains("id") && entry["id"].is_string()) {
        id_override = entry["id"].get<std::string>();
      }
      if (entry.contains("label") && entry["label"].is_string()) {
        label_override = entry["label"].get<std::string>();
      }
    } else {
      throw std::runtime_error(
          "accounts[] entries must be a string path or {env, id?, label?}");
    }
    ServerConfig sc = load_account_env(env_path);
    if (!id_override.empty()) {
      sc.id = id_override;
    }
    if (!label_override.empty()) {
      sc.label = label_override;
    }
    opt.servers.push_back(std::move(sc));
  }
}

}  // namespace

ServerConfig load_account_env(const std::string& env_path) {
  const auto kv = parse_env_file(env_path);
  ServerConfig sc;
  sc.env_path = env_path;

  if (const std::string* id =
          find_key(kv, {"HYPERION_ID", "JINGHONG_ID", "ACCOUNT_ID"})) {
    sc.id = *id;
  } else {
    sc.id = path_stem(env_path);
  }

  if (const std::string* lab = find_key(
          kv, {"HYPERION_LABEL", "TAB_NAME", "LABEL", "JINGHONG_LABEL"})) {
    sc.label = *lab;
  } else {
    sc.label = sc.id;
  }

  sc.order_channel =
      require_key(kv, {"JINGHONG_ORDER_CHANNEL", "ORDER_CHANNEL"}, env_path);
  sc.status_channel =
      require_key(kv, {"JINGHONG_STATUS_CHANNEL", "STATUS_CHANNEL"}, env_path);

  if (const std::string* ac =
          find_key(kv, {"JINGHONG_ALPACA_CHANNEL", "ALPACA_CHANNEL"})) {
    sc.alpaca_channel = *ac;
  } else {
    sc.alpaca_channel =
        derive_alpaca_channel(sc.status_channel, sc.order_channel);
  }

  if (const std::string* hb =
          find_key(kv, {"HYPERION_HTTP_BASE", "JINGHONG_HTTP_BASE"})) {
    sc.http_base = *hb;
  } else {
    try {
      sc.http_base = http_base_from_env(kv);
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string(e.what()) + " in " + env_path);
    }
  }

  return sc;
}

Options load_master_config(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open config: " + path);
  }
  nlohmann::json j;
  try {
    in >> j;
  } catch (const std::exception& e) {
    throw std::runtime_error("invalid JSON in " + path + ": " + e.what());
  }
  Options opt;
  opt.config_path = path;
  apply_master_json(opt, j);
  return opt;
}

std::string command_line_help() {
  return R"HELP(hyperion — jinghong multi-account book monitor (Wt)

Usage:
  hyperion --config /etc/hyperion/hyperion.json [options] -- [wt options]
  hyperion --config ./hyperion.json --http-listen 0.0.0.0:8110 --docroot ./static

Infrastructure (Redis UDS, account list, channels) lives in a master JSON
config. Each account is a jinghong-style KEY=VALUE env file.

Master config (JSON):
  {
    "redis_path": "/run/goblin-core/goblin.sock",
    "ui_hz": 4,
    "equity_hz": 1,
    "account_poll_sec": 60,
    "alpaca_limit_qpm": 200,
    "accounts": [
      "/etc/jinghong/paper1.env",
      { "env": "/etc/jinghong/paper3.env", "label": "Paper 3" }
    ]
  }

Account env (same file jinghong uses) — required:
  JINGHONG_ORDER_CHANNEL, JINGHONG_STATUS_CHANNEL, JINGHONG_HTTP_PORT
Optional:
  JINGHONG_HTTP_BIND, JINGHONG_ALPACA_CHANNEL, HYPERION_HTTP_BASE
  HYPERION_ID / JINGHONG_ID          (default: env filename stem)
  HYPERION_LABEL / TAB_NAME / LABEL  (UI tab title; default: id)

Hyperion options:
  --config PATH          Master JSON config (or env HYPERION_CONFIG)
  --redis PATH           Override redis_path from config
  --account-env PATH     Append one account env (repeatable; no master needed)
  --ui-hz N              UI refresh rate (default from config or 4)
  --equity-hz N          Equity sample rate (default from config or 1)
  --account-poll SEC     Broker cash reconcile interval (default 60)
  --http-poll SEC        legacy alias (ignored for full status)
  --server ID,HTTP,ORDER,STATUS[,ALPACA]
                         Ad-hoc account without an env file (repeatable)
  -h, --help             This message

Everything after the first unknown flag, or after `--`, is forwarded to Wt
(e.g. --docroot, --http-listen, --resources-dir).
)HELP";
}

CommandLine parse_command_line(int argc, char** argv) {
  CommandLine out;
  out.wt_arguments.push_back(argv[0]);

  bool passthrough = false;
  bool have_config = false;
  bool custom_servers = false;
  std::string config_path;
  std::vector<std::string> extra_env_paths;
  std::string redis_override;
  bool ui_hz_set = false;
  bool equity_hz_set = false;
  bool account_poll_set = false;

  // CLI pass 1: collect flags (may reference config path).
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (passthrough) {
      out.wt_arguments.push_back(arg);
      continue;
    }
    if (arg == "--") {
      passthrough = true;
      continue;
    }
    if (arg == "-h" || arg == "--help") {
      out.help_requested = true;
      continue;
    }
    if (arg == "--config") {
      config_path = need_value(arg, i, argc, argv);
      continue;
    }
    if (arg == "--redis") {
      redis_override = need_value(arg, i, argc, argv);
      continue;
    }
    if (arg == "--account-env") {
      extra_env_paths.push_back(need_value(arg, i, argc, argv));
      continue;
    }
    if (arg == "--ui-hz") {
      out.options.ui_hz = std::stod(need_value(arg, i, argc, argv));
      ui_hz_set = true;
      continue;
    }
    if (arg == "--equity-hz") {
      out.options.equity_sample_hz = std::stod(need_value(arg, i, argc, argv));
      equity_hz_set = true;
      continue;
    }
    if (arg == "--http-poll") {
      out.options.http_poll_sec = std::stoi(need_value(arg, i, argc, argv));
      continue;
    }
    if (arg == "--account-poll") {
      out.options.account_poll_sec = std::stoi(need_value(arg, i, argc, argv));
      account_poll_set = true;
      continue;
    }
    if (arg == "--server") {
      const std::string spec = need_value(arg, i, argc, argv);
      std::vector<std::string> parts;
      std::stringstream ss(spec);
      std::string piece;
      while (std::getline(ss, piece, ',')) {
        parts.push_back(piece);
      }
      if (parts.size() != 4 && parts.size() != 5) {
        throw std::runtime_error(
            "--server expects id,http_base,order,status[,alpaca]");
      }
      if (!custom_servers && !have_config && extra_env_paths.empty()) {
        out.options.servers.clear();
      }
      custom_servers = true;
      ServerConfig sc;
      sc.id = parts[0];
      sc.label = parts[0];
      sc.http_base = parts[1];
      sc.order_channel = parts[2];
      sc.status_channel = parts[3];
      if (parts.size() == 5) {
        sc.alpaca_channel = parts[4];
      } else {
        sc.alpaca_channel =
            derive_alpaca_channel(sc.status_channel, sc.order_channel);
      }
      out.options.servers.push_back(std::move(sc));
      continue;
    }

    out.wt_arguments.push_back(arg);
  }

  if (out.help_requested) {
    return out;
  }

  if (config_path.empty()) {
    if (const char* env = std::getenv("HYPERION_CONFIG"); env && *env) {
      config_path = env;
    }
  }

  // Load master config when provided (primary path for production).
  if (!config_path.empty()) {
    Options from_file = load_master_config(config_path);
    // Preserve CLI rate overrides applied above.
    const double cli_ui = out.options.ui_hz;
    const double cli_eq = out.options.equity_sample_hz;
    const int cli_acct = out.options.account_poll_sec;
    const int cli_http = out.options.http_poll_sec;
    std::vector<ServerConfig> cli_servers = std::move(out.options.servers);

    out.options = std::move(from_file);
    have_config = true;

    if (ui_hz_set) {
      out.options.ui_hz = cli_ui;
    }
    if (equity_hz_set) {
      out.options.equity_sample_hz = cli_eq;
    }
    if (account_poll_set) {
      out.options.account_poll_sec = cli_acct;
    }
    out.options.http_poll_sec = cli_http;

    // --server after config appends (or we could replace — append is safer).
    for (auto& sc : cli_servers) {
      out.options.servers.push_back(std::move(sc));
    }
  }

  for (const auto& env_path : extra_env_paths) {
    out.options.servers.push_back(load_account_env(env_path));
  }

  if (!redis_override.empty()) {
    out.options.redis_path = redis_override;
  } else if (const char* env = std::getenv("HYPERION_REDIS_PATH");
             env && *env && out.options.redis_path.empty()) {
    out.options.redis_path = env;
  }

  if (out.options.redis_path.empty()) {
    throw std::runtime_error(
        "redis_path required (master config \"redis_path\", --redis, or "
        "HYPERION_REDIS_PATH)");
  }
  if (out.options.ui_hz <= 0.0) {
    throw std::runtime_error("--ui-hz must be positive");
  }
  if (out.options.equity_sample_hz <= 0.0) {
    throw std::runtime_error("--equity-hz must be positive");
  }
  if (out.options.servers.empty()) {
    throw std::runtime_error(
        "no accounts configured — use --config with \"accounts\", "
        "--account-env, or --server");
  }
  return out;
}

}  // namespace hyperion
