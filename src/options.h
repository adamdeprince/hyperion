#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hyperion {

// One jinghong order-server / account tab.
// Populated from a per-account .env (and optional master-config overrides).
struct ServerConfig {
  std::string id;              // stable id (filename stem or HYPERION_ID)
  std::string label;           // UI tab title (HYPERION_LABEL / TAB_NAME / …)
  std::string order_channel;   // e.g. ACCOUNT:ORDER:paper3
  std::string status_channel;  // e.g. ACCOUNT:STATUS:paper3
  std::string alpaca_channel;  // e.g. ACCOUNT:ALPACA:paper3
  std::string http_base;       // e.g. http://127.0.0.1:8768
  std::string env_path;        // path to the account .env (for diagnostics)
};

// Runtime options. Infrastructure topology comes from --config (JSON).
// No host-specific paths or account lists are baked into the binary defaults.
struct Options {
  std::string config_path;  // master config used (empty if only CLI servers)
  std::string redis_path;   // goblin-core / Redis UDS (required)
  std::vector<ServerConfig> servers;
  double ui_hz = 4.0;
  double equity_sample_hz = 1.0;
  int http_poll_sec = 15;     // legacy CLI alias; unused for full status
  int account_poll_sec = 60;  // GET {jinghong}/v1/account
  std::size_t equity_history_points = 900;
  std::size_t price_history_points = 300;
  int alpaca_limit_qpm = 200;
};

struct CommandLine {
  Options options;
  std::vector<std::string> wt_arguments;
  bool help_requested = false;
};

CommandLine parse_command_line(int argc, char** argv);
std::string command_line_help();

// Load master JSON config (redis path, rates, account env list).
Options load_master_config(const std::string& path);

// Load one account from a jinghong-style KEY=VALUE env file.
ServerConfig load_account_env(const std::string& env_path);

}  // namespace hyperion
