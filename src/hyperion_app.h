#pragma once

#include "options.h"
#include "server_panel.h"
#include "session_feed.h"

#include <memory>
#include <string>
#include <vector>

#include <Wt/WApplication.h>

namespace Wt {
class WEnvironment;
class WServer;
class WTabWidget;
class WText;
}

namespace hyperion {

class HyperionApp final : public Wt::WApplication {
 public:
  HyperionApp(const Wt::WEnvironment& env, Wt::WServer& server,
              Options options);
  ~HyperionApp() override;

  void deliver_snapshots(const std::vector<ServerSnapshot>& snaps);

 private:
  void build_ui();
  void on_pdf_for_server(const std::string& server_id);
  void on_pdf_all();
  void serve_pdf(const std::string& bytes, const std::string& filename);

  Wt::WServer& server_;
  Options options_;
  std::string session_id_;
  std::unique_ptr<SessionFeed> feed_;

  Wt::WText* banner_status_{};
  Wt::WTabWidget* tabs_{};
  std::vector<ServerPanel*> panels_;
  std::vector<ServerSnapshot> latest_;
};

}  // namespace hyperion
