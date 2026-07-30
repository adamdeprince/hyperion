#include "hyperion_app.h"

#include "hedge_pdf.h"

#include <Wt/WContainerWidget.h>
#include <Wt/WEnvironment.h>
#include <Wt/WLength.h>
#include <Wt/WMemoryResource.h>
#include <Wt/WPushButton.h>
#include <Wt/WServer.h>
#include <Wt/WTabWidget.h>
#include <Wt/WText.h>

#include <format>
#include <utility>

namespace hyperion {
namespace {

Wt::WText* plain(Wt::WContainerWidget* parent, std::string_view value,
                 std::string_view style = {}) {
  auto* w =
      parent->addNew<Wt::WText>(Wt::WString::fromUTF8(std::string(value)));
  w->setTextFormat(Wt::TextFormat::Plain);
  if (!style.empty()) {
    w->setStyleClass(std::string(style));
  }
  return w;
}

}  // namespace

HyperionApp::HyperionApp(const Wt::WEnvironment& env, Wt::WServer& server,
                         Options options)
    : Wt::WApplication(env),
      server_(server),
      options_(std::move(options)),
      session_id_(sessionId()) {
  setTitle("Hyperion — jinghong books");
  useStyleSheet("static/app.css");
  setCssTheme("");  // avoid default theme fighting our contrast colors
  root()->setStyleClass("app-shell");
  root()->setHeight(Wt::WLength(100, Wt::LengthUnit::ViewportHeight));
  enableUpdates(true);

  feed_ = std::make_unique<SessionFeed>(options_, server_, session_id_);
  build_ui();
  feed_->start([this](const std::vector<ServerSnapshot>& snaps) {
    deliver_snapshots(snaps);
  });
}

HyperionApp::~HyperionApp() {
  if (feed_) {
    feed_->stop();
    feed_.reset();
  }
  enableUpdates(false);
}

void HyperionApp::build_ui() {
  auto* mast = root()->addNew<Wt::WContainerWidget>();
  mast->setStyleClass("masthead");

  auto* brand = mast->addNew<Wt::WContainerWidget>();
  brand->setStyleClass("brand");
  plain(brand, "INTERNAL", "eyebrow");
  plain(brand, "Hyperion", "site-title");
  plain(brand, "Jinghong books · goblin-slurp marks · hedge PDF", "dek");

  auto* actions = mast->addNew<Wt::WContainerWidget>();
  actions->setStyleClass("mast-actions");
  banner_status_ = plain(actions, "starting…", "banner-status");
  auto* all_pdf = actions->addNew<Wt::WPushButton>("Hedge PDF (all tabs)");
  all_pdf->setStyleClass("hedge-btn");
  all_pdf->clicked().connect(this, &HyperionApp::on_pdf_all);

  tabs_ = root()->addNew<Wt::WTabWidget>();
  tabs_->setStyleClass("server-tabs");

  panels_.clear();
  panels_.reserve(options_.servers.size());
  for (const auto& sc : options_.servers) {
    auto panel = std::make_unique<ServerPanel>(sc, feed_.get());
    auto* raw = panel.get();
    raw->set_pdf_handler([this, id = sc.id] { on_pdf_for_server(id); });
    tabs_->addTab(std::move(panel), sc.label);
    panels_.push_back(raw);
  }
}

void HyperionApp::deliver_snapshots(const std::vector<ServerSnapshot>& snaps) {
  latest_ = snaps;

  bool any_redis = false;
  bool any_http = false;
  std::size_t unc = 0;
  std::string redis_note;
  for (const auto& s : snaps) {
    any_redis = any_redis || s.redis_ok;
    any_http = any_http || s.http_ok;
    unc += s.uncertain_count;
    if (!s.redis_ok && redis_note.empty() && !s.note.empty()) {
      redis_note = s.note;
    }
  }
  std::string redis_txt = any_redis ? "up" : "down";
  if (!any_redis && !redis_note.empty()) {
    redis_txt += " (" + redis_note + ")";
  }
  banner_status_->setText(Wt::WString::fromUTF8(std::format(
      "redis {} · jinghong http {} · {} uncertain across books", redis_txt,
      any_http ? "up" : "down", unc)));
  banner_status_->setStyleClass(any_redis ? "banner-status is-ok"
                                          : "banner-status is-warn");

  for (std::size_t i = 0; i < panels_.size() && i < snaps.size(); ++i) {
    // Match by server id in case order differs.
    const ServerSnapshot* match = nullptr;
    for (const auto& s : snaps) {
      if (s.server_id == panels_[i]->config().id) {
        match = &s;
        break;
      }
    }
    if (match) {
      panels_[i]->apply(*match);
    }
  }
  triggerUpdate();
}

void HyperionApp::on_pdf_for_server(const std::string& server_id) {
  for (const auto& s : latest_) {
    if (s.server_id == server_id) {
      serve_pdf(build_hedge_pdf(s),
                "hyperion-hedge-" + server_id + ".pdf");
      return;
    }
  }
  // Fallback empty report.
  ServerSnapshot empty;
  empty.server_id = server_id;
  empty.label = server_id;
  serve_pdf(build_hedge_pdf(empty), "hyperion-hedge-" + server_id + ".pdf");
}

void HyperionApp::on_pdf_all() {
  serve_pdf(build_hedge_pdf(latest_), "hyperion-hedge-all.pdf");
}

void HyperionApp::serve_pdf(const std::string& bytes,
                            const std::string& filename) {
  // Own the resource under the application so the URL stays valid.
  auto res = std::make_unique<Wt::WMemoryResource>("application/pdf");
  res->setData(std::vector<unsigned char>(bytes.begin(), bytes.end()));
  res->suggestFileName(filename, Wt::ContentDisposition::Attachment);
  auto* raw = addChild(std::move(res));
  doJavaScript("window.open('" + raw->url() + "', '_blank');");
}

}  // namespace hyperion
