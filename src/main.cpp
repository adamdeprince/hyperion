#include "hyperion_app.h"
#include "options.h"

#include <Wt/WServer.h>

#include <csignal>
#include <curl/curl.h>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  try {
    auto command_line = hyperion::parse_command_line(argc, argv);
    if (command_line.help_requested) {
      std::cout << hyperion::command_line_help();
      curl_global_cleanup();
      return 0;
    }

    std::vector<char*> wt_argv;
    wt_argv.reserve(command_line.wt_arguments.size());
    for (auto& argument : command_line.wt_arguments) {
      wt_argv.push_back(argument.data());
    }

    Wt::WServer server(command_line.wt_arguments.front());
    server.setServerConfiguration(static_cast<int>(wt_argv.size()),
                                  wt_argv.data(), WTHTTP_CONFIGURATION);

    const auto options = command_line.options;
    server.addEntryPoint(
        Wt::EntryPointType::Application,
        [&server, options](const Wt::WEnvironment& environment) {
          return std::make_unique<hyperion::HyperionApp>(environment, server,
                                                         options);
        });

    if (!server.start()) {
      std::cerr << "hyperion: Wt server failed to start\n";
      curl_global_cleanup();
      return 1;
    }

    std::cerr << "hyperion: listening (redis=" << options.redis_path
              << ", servers=" << options.servers.size();
    if (!options.config_path.empty()) {
      std::cerr << ", config=" << options.config_path;
    }
    std::cerr << ")\n";
    for (const auto& sc : options.servers) {
      std::cerr << "  " << sc.id << "  label=\"" << sc.label << "\""
                << "  http=" << sc.http_base
                << "  order=" << sc.order_channel
                << "  status=" << sc.status_channel;
      if (!sc.env_path.empty()) {
        std::cerr << "  env=" << sc.env_path;
      }
      std::cerr << "\n";
    }

    const int signal = Wt::WServer::waitForShutdown();
    server.stop();
    curl_global_cleanup();
    if (signal == 0 || signal == SIGINT || signal == SIGTERM) {
      return 0;
    }
    return 128 + signal;
  } catch (const std::exception& exception) {
    std::cerr << "hyperion: " << exception.what() << "\n\n"
              << hyperion::command_line_help();
    curl_global_cleanup();
    return 1;
  }
}
