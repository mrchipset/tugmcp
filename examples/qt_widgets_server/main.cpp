// Qt 5 Widgets host example for tugmcp.
//
// This example embeds a Streamable HTTP MCP server inside a Qt Widgets
// application. It demonstrates two host-integration patterns:
//
//   * ui_set_text / ui_get_state touch QWidget state only from the GUI thread
//     (marshalled with QMetaObject::invokeMethod + Qt::BlockingQueuedConnection).
//   * worker_job runs on its own std::thread, reports progress through
//     IProgressSink, and polls RequestContext::cancellation. The MCP handler
//     (which runs on a tugmcp executor thread) joins the worker, so neither the
//     GUI thread nor the HTTP thread performs the work.
//
// The MCP server itself runs on a dedicated std::thread. Closing the window
// cancels in-flight tools, stops the server, and joins every thread before the
// window and its widgets are destroyed.

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include <tugmcp/server.hpp>
#include <tugmcp/tool.hpp>
#include <tugmcp/transport/http.hpp>

namespace {

using namespace mcp::v1;

std::string threadIdString(std::thread::id id) {
  std::ostringstream out;
  out << id;
  return out.str();
}

std::string qtThreadIdString(Qt::HANDLE id) {
  std::ostringstream out;
  out << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(id);
  return out.str();
}

struct UiSnapshot {
  std::string text;
  std::uint64_t updateCounter = 0;
  std::uint64_t heartbeat = 0;
  std::string guiThreadId;
  std::string guiQtThreadId;
};

// Lives on the GUI thread. Every widget access happens in a slot/lambda that
// runs on QApplication's thread. State is mirrored under a mutex so the MCP
// handlers can read a consistent snapshot after the blocking invoke returns.
class GuiBridge final : public QObject {
public:
  GuiBridge(QLabel* textLabel, QLabel* heartbeatLabel, QPlainTextEdit* log,
            QObject* parent = nullptr)
      : QObject(parent),
        textLabel_(textLabel),
        heartbeatLabel_(heartbeatLabel),
        log_(log) {
    guiThreadId_ = threadIdString(std::this_thread::get_id());
    guiQtThreadId_ = qtThreadIdString(QThread::currentThreadId());
    updateHeartbeatLabel();
  }

  // GUI thread only.
  void applyText(const QString& text) {
    textLabel_->setText(text);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++updateCounter_;
    }
    appendLog(QStringLiteral("ui_set_text applied on GUI thread: %1").arg(text));
  }

  // GUI thread only (heartbeat timer).
  void tick() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++heartbeat_;
    }
    updateHeartbeatLabel();
  }

  // GUI thread only.
  void appendLog(const QString& message) {
    const QString line =
        QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")),
                 message);
    log_->appendPlainText(line);
  }

  // GUI thread only. Reads the live widget text so ui_get_state proves the
  // value really came from the QLabel, not from a shadow copy.
  UiSnapshot readStateOnGui() {
    UiSnapshot snapshot;
    snapshot.text = textLabel_->text().toStdString();
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot.updateCounter = updateCounter_;
    snapshot.heartbeat = heartbeat_;
    snapshot.guiThreadId = guiThreadId_;
    snapshot.guiQtThreadId = guiQtThreadId_;
    return snapshot;
  }

private:  void updateHeartbeatLabel() {
    std::uint64_t heartbeat = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      heartbeat = heartbeat_;
    }
    heartbeatLabel_->setText(
        QStringLiteral("GUI heartbeat (100 ms timer): %1").arg(heartbeat));
  }

  QLabel* textLabel_;
  QLabel* heartbeatLabel_;
  QPlainTextEdit* log_;

  mutable std::mutex mutex_;
  std::uint64_t updateCounter_ = 0;
  std::uint64_t heartbeat_ = 0;
  std::string guiThreadId_;
  std::string guiQtThreadId_;
};

template <typename Fn>
bool invokeOnGui(GuiBridge* bridge, Fn&& fn) {
  if (bridge == nullptr) {
    return false;
  }
  if (QThread::currentThread() == bridge->thread()) {
    fn();
    return true;
  }
  return QMetaObject::invokeMethod(bridge, std::forward<Fn>(fn),
                                   Qt::BlockingQueuedConnection);
}

void postLog(GuiBridge* bridge, std::string message) {
  if (bridge == nullptr) {
    return;
  }
  QMetaObject::invokeMethod(
      bridge,
      [bridge, message = std::move(message)]() {
        bridge->appendLog(QString::fromStdString(message));
      },
      Qt::QueuedConnection);
}

Result<ToolOutcome> invalidParams(std::string message) {
  return Result<ToolOutcome>::failure(McpError::make(
      ErrorCode::InvalidParams, std::move(message)));
}

ToolResult textResult(std::string text, Json structured) {
  ToolResult result;
  result.content.push_back(ContentBlock{ContentType::Text, std::move(text)});
  result.structuredContent = std::move(structured);
  return result;
}

class SetTextTool final : public IToolHandler {
public:
  explicit SetTextTool(GuiBridge* bridge) : bridge_(bridge) {}

  Result<ToolOutcome> invoke(const Json& arguments, RequestContext&,
                             IProgressSink&) override {
    if (!arguments.contains("text") || !arguments.at("text").is_string()) {
      return invalidParams("ui_set_text requires a string 'text' argument");
    }
    const std::string text = arguments.at("text").get<std::string>();

    struct Applied {
      bool ok = false;
      UiSnapshot snapshot;
    };
    auto applied = std::make_shared<Applied>();
    const bool invoked = invokeOnGui(bridge_, [bridge = bridge_, text,
                                               applied]() {
      bridge->applyText(QString::fromStdString(text));
      applied->snapshot = bridge->readStateOnGui();
      applied->ok = true;
    });
    if (!invoked || !applied->ok) {
      return Result<ToolOutcome>::failure(McpError::make(
          ErrorCode::InternalError, "failed to marshal ui_set_text to GUI thread"));
    }

    std::cerr << "tugmcp-qt: ui_set_text text=" << text
              << " guiThreadId=" << applied->snapshot.guiThreadId << "\n";
    Json structured{
        {"text", applied->snapshot.text},
        {"guiThreadId", applied->snapshot.guiThreadId},
        {"guiQtThreadId", applied->snapshot.guiQtThreadId},
        {"updateCounter", applied->snapshot.updateCounter},
        {"applied", true}};
    return ToolOutcome(textResult(
        "ui_set_text applied on GUI thread " + applied->snapshot.guiThreadId,
        std::move(structured)));
  }

private:
  GuiBridge* bridge_;
};

class GetStateTool final : public IToolHandler {
public:
  explicit GetStateTool(GuiBridge* bridge) : bridge_(bridge) {}

  Result<ToolOutcome> invoke(const Json&, RequestContext&,
                             IProgressSink&) override {
    auto snapshot = std::make_shared<UiSnapshot>();
    const bool invoked = invokeOnGui(bridge_, [bridge = bridge_, snapshot]() {
      *snapshot = bridge->readStateOnGui();
    });
    if (!invoked) {
      return Result<ToolOutcome>::failure(McpError::make(
          ErrorCode::InternalError, "failed to marshal ui_get_state to GUI thread"));
    }

    Json structured{
        {"text", snapshot->text},
        {"guiThreadId", snapshot->guiThreadId},
        {"guiQtThreadId", snapshot->guiQtThreadId},
        {"updateCounter", snapshot->updateCounter},
        {"heartbeat", snapshot->heartbeat}};
    return ToolOutcome(textResult(
        "ui_get_state read on GUI thread " + snapshot->guiThreadId,
        std::move(structured)));
  }

private:
  GuiBridge* bridge_;
};

class WorkerJobTool final : public IToolHandler {
public:
  explicit WorkerJobTool(GuiBridge* bridge) : bridge_(bridge) {}

  Result<ToolOutcome> invoke(const Json& arguments, RequestContext& context,
                             IProgressSink& progress) override {
    int iterations = 10;
    int stepMs = 50;
    std::string error;
    if (!readInt(arguments, "iterations", 1, 1000, iterations, error) ||
        !readInt(arguments, "step_ms", 0, 5000, stepMs, error)) {
      return invalidParams(std::move(error));
    }

    const std::string handlerThreadId = threadIdString(std::this_thread::get_id());

    // Read the GUI id through the same marshalling path used by the UI tools.
    auto guiSnapshot = std::make_shared<UiSnapshot>();
    if (!invokeOnGui(bridge_, [bridge = bridge_, guiSnapshot]() {
          *guiSnapshot = bridge->readStateOnGui();
        })) {
      return Result<ToolOutcome>::failure(McpError::make(
          ErrorCode::InternalError, "failed to read GUI state"));
    }

    std::atomic<int> reports{0};
    std::atomic<bool> cancelled{false};
    int completed = 0;
    std::string workerThreadId;

    const auto started = std::chrono::steady_clock::now();

    // The worker owns no QObject and never touches widgets. It only reports
    // progress through the request-scoped sink and polls cancellation.
    std::thread worker([&]() {
      workerThreadId = threadIdString(std::this_thread::get_id());
      for (int index = 1; index <= iterations; ++index) {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(stepMs);
        while (std::chrono::steady_clock::now() < deadline) {
          if (context.cancellation && context.cancellation->cancelled()) {
            cancelled.store(true, std::memory_order_relaxed);
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (cancelled.load(std::memory_order_relaxed)) {
          break;
        }
        if (context.cancellation && context.cancellation->cancelled()) {
          cancelled.store(true, std::memory_order_relaxed);
          break;
        }

        progress.report(static_cast<double>(index) / iterations,
                        "worker_job iteration " + std::to_string(index));
        reports.fetch_add(1, std::memory_order_relaxed);
        completed = index;
      }
    });
    worker.join();

    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count();
    const bool wasCancelled = cancelled.load(std::memory_order_relaxed);

    std::cerr << "tugmcp-qt: worker_job iterations=" << iterations
              << " stepMs=" << stepMs
              << " handlerThreadId=" << handlerThreadId
              << " workerThreadId=" << workerThreadId
              << " guiThreadId=" << guiSnapshot->guiThreadId
              << " reports=" << reports.load()
              << " cancelled=" << (wasCancelled ? "true" : "false") << "\n";

    Json structured{
        {"handlerThreadId", handlerThreadId},
        {"workerThreadId", workerThreadId},
        {"guiThreadId", guiSnapshot->guiThreadId},
        {"guiQtThreadId", guiSnapshot->guiQtThreadId},
        {"progressReports", reports.load()},
        {"cancelled", wasCancelled},
        {"completed", !wasCancelled},
        {"iterationsRequested", iterations},
        {"iterationsCompleted", completed},
        {"stepMs", stepMs},
        {"elapsedMs", elapsedMs}};

    std::string summary =
        "worker_job finished on worker thread " + workerThreadId +
        " (handler " + handlerThreadId + "): reports=" +
        std::to_string(reports.load()) +
        (wasCancelled ? ", cancelled=true" : ", completed=true");
    return ToolOutcome(textResult(std::move(summary), std::move(structured)));
  }

private:
  static bool readInt(const Json& arguments, const char* name, int minimum,
                      int maximum, int& value, std::string& error) {
    if (!arguments.contains(name)) {
      return true;
    }
    const Json& item = arguments.at(name);
    if (!item.is_number_integer()) {
      error = std::string(name) + " must be an integer";
      return false;
    }
    const long long parsed = item.get<long long>();
    if (parsed < minimum || parsed > maximum) {
      error = std::string(name) + " must be between " +
              std::to_string(minimum) + " and " + std::to_string(maximum);
      return false;
    }
    value = static_cast<int>(parsed);
    return true;
  }

  GuiBridge* bridge_;
};

bool registerTools(McpServer& server, GuiBridge* bridge) {
  {
    ToolDescriptor descriptor;
    descriptor.name = "ui_set_text";
    descriptor.title = "Set UI text";
    descriptor.description =
        "Replace the QLabel text from the GUI thread and return the new state";
    descriptor.inputSchema = Json::parse(
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})");
    if (!server.tools().add(descriptor, std::make_shared<SetTextTool>(bridge)).ok) {
      return false;
    }
  }
  {
    ToolDescriptor descriptor;
    descriptor.name = "ui_get_state";
    descriptor.title = "Get UI state";
    descriptor.description =
        "Read the QLabel text, GUI thread id, update counter, and heartbeat";
    descriptor.inputSchema =
        Json::parse(R"({"type":"object","properties":{}})");
    if (!server.tools().add(descriptor, std::make_shared<GetStateTool>(bridge))
             .ok) {
      return false;
    }
  }
  {
    ToolDescriptor descriptor;
    descriptor.name = "worker_job";
    descriptor.title = "Worker job";
    descriptor.description =
        "Run iterations on a non-GUI thread, reporting progress and observing cancellation";
    descriptor.inputSchema = Json::parse(
        R"({"type":"object","properties":{"iterations":{"type":"integer","minimum":1,"maximum":1000,"default":10},"step_ms":{"type":"integer","minimum":0,"maximum":5000,"default":50}}})");
    if (!server.tools().add(descriptor, std::make_shared<WorkerJobTool>(bridge))
             .ok) {
      return false;
    }
  }
  return true;
}

// Owns the transport, the server, and the server/shutdown threads. Widgets are
// not touched here: callers pass a GuiBridge* whose lifetime outlives this
// object.
class ServerHost {
public:
  ServerHost(GuiBridge* bridge, std::string bind, int port)
      : bridge_(bridge), bind_(std::move(bind)), port_(port) {}

  ~ServerHost() { waitForShutdown(); }

  bool start(std::string& error) {
    StreamableHttpServerOptions httpOptions;
    httpOptions.bind = bind_;
    httpOptions.port = port_;
    // worker_job may legitimately run for several seconds; keep the request
    // timeout well above the example's own limits.
    httpOptions.requestTimeout = std::chrono::milliseconds(60000);

    auto created = makeStreamableHttpServerTransport(httpOptions);
    if (!created.hasValue()) {
      error = created.error().message;
      if (created.error().data.is_object() &&
          created.error().data.contains("reason")) {
        error += " (" + created.error().data.at("reason").dump() + ")";
      }
      return false;
    }
    transport_ = created.value();
    http_ = std::dynamic_pointer_cast<HttpServerTransport>(transport_);
    if (!http_) {
      error = "unexpected HTTP transport type";
      return false;
    }

    ServerOptions options;
    options.name = "tugmcp-qt-widgets";
    options.version = "0.1.0";
    // This demo has no schema-validation target linked. Production hosts
    // should inject options.schemaValidator and leave this false.
    options.allowUnvalidatedTools = true;

    server_ = std::make_unique<McpServer>(options, transport_);
    if (!registerTools(*server_, bridge_)) {
      error = "failed to register tools";
      server_.reset();
      transport_.reset();
      return false;
    }

    const Status started = transport_->start();
    if (!started) {
      error = started.error.message;
      server_.reset();
      transport_.reset();
      return false;
    }
    port_ = http_->boundPort();

    std::cerr << "tugmcp-qt: listening on http://" << bind_ << ":" << port_
              << "/mcp\n"
              << std::flush;

    serverThread_ = std::thread([this]() {
      const Status status = server_->run();
      if (!status) {
        std::cerr << "tugmcp-qt: server.run failed: " << status.error.message
                  << "\n";
      }
    });
    return true;
  }

  int port() const { return port_; }
  std::string url() const {
    return "http://" + bind_ + ":" + std::to_string(port_) + "/mcp";
  }

  // Idempotent. Runs stop/join off the GUI thread so a handler blocked in a
  // BlockingQueuedConnection can still be serviced by the GUI event loop.
  void beginShutdown() {
    bool expected = false;
    if (!shutdownStarted_.compare_exchange_strong(expected, true)) {
      return;
    }
    shutdownThread_ = std::thread([this]() {
      if (server_) {
        server_->stop();
      }
      if (serverThread_.joinable()) {
        serverThread_.join();
      }
      server_.reset();
      transport_.reset();
      http_.reset();
      {
        std::lock_guard<std::mutex> lock(doneMutex_);
        shutdownDone_ = true;
      }
      doneCv_.notify_all();

      if (QCoreApplication::instance() != nullptr) {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(), []() { QCoreApplication::quit(); },
            Qt::QueuedConnection);
      }
    });
  }

  void waitForShutdown() {
    beginShutdown();
    {
      std::unique_lock<std::mutex> lock(doneMutex_);
      while (!shutdownDone_) {
        lock.unlock();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
      }
    }
    if (shutdownThread_.joinable()) {
      shutdownThread_.join();
    }
  }

private:
  GuiBridge* bridge_;
  std::string bind_;
  int port_ = 0;

  std::shared_ptr<ITransport> transport_;
  std::shared_ptr<HttpServerTransport> http_;
  std::unique_ptr<McpServer> server_;

  std::thread serverThread_;
  std::thread shutdownThread_;
  std::atomic<bool> shutdownStarted_{false};
  bool shutdownDone_ = false;
  std::mutex doneMutex_;
  std::condition_variable doneCv_;
};

class MainWindow final : public QMainWindow {
public:
  MainWindow(std::string bind, int port)
      : bind_(std::move(bind)), port_(port) {
    setWindowTitle(QStringLiteral("tugmcp Qt Widgets MCP Server"));
    resize(720, 480);

    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    statusLabel_ = new QLabel(QStringLiteral("Starting MCP server..."), central);
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    textLabel_ =
        new QLabel(QStringLiteral("Qt UI ready"), central);
    textLabel_->setWordWrap(true);
    layout->addWidget(textLabel_);

    heartbeatLabel_ = new QLabel(central);
    layout->addWidget(heartbeatLabel_);

    log_ = new QPlainTextEdit(central);
    log_->setReadOnly(true);
    layout->addWidget(log_, 1);

    auto* quitButton = new QPushButton(QStringLiteral("Stop server and quit"), central);
    layout->addWidget(quitButton);

    setCentralWidget(central);

    bridge_ = std::make_unique<GuiBridge>(textLabel_, heartbeatLabel_, log_, this);
    heartbeatTimer_ = new QTimer(this);
    connect(heartbeatTimer_, &QTimer::timeout, this,
            [this]() { bridge_->tick(); });
    heartbeatTimer_->start(100);

    connect(quitButton, &QPushButton::clicked, this,
            [this]() { requestClose(); });
  }

  ~MainWindow() override {
    if (server_) {
      server_->waitForShutdown();
    }
  }

  bool startServer(std::string& error) {
    server_ = std::make_unique<ServerHost>(bridge_.get(), bind_, port_);
    if (!server_->start(error)) {
      statusLabel_->setText(QStringLiteral("MCP server failed: %1")
                                .arg(QString::fromStdString(error)));
      bridge_->appendLog(QStringLiteral("server start failed: %1")
                             .arg(QString::fromStdString(error)));
      return false;
    }
    statusLabel_->setText(
        QStringLiteral("MCP server: %1").arg(QString::fromStdString(server_->url())));
    bridge_->appendLog(QStringLiteral("MCP server listening on %1")
                           .arg(QString::fromStdString(server_->url())));
    return true;
  }

  int boundPort() const { return server_ ? server_->port() : 0; }

  // Idempotent. Joins the server and shutdown threads; safe to call after
  // app.exec() has returned.
  void waitForShutdown() {
    if (server_) {
      server_->waitForShutdown();
    }
  }

  // Public hook for --exit-after-ms and the quit button. close() is ignored by
  // closeEvent and turned into an asynchronous, thread-safe shutdown.
  void requestClose() { close(); }

protected:
  void closeEvent(QCloseEvent* event) override {
    if (shutdownRequested_) {
      event->ignore();
      return;
    }
    shutdownRequested_ = true;
    event->ignore();
    hide();
    heartbeatTimer_->stop();
    statusLabel_->setText(QStringLiteral("Stopping MCP server..."));
    if (bridge_) {
      bridge_->appendLog(QStringLiteral("Stopping MCP server..."));
    }
    if (server_) {
      server_->beginShutdown();
    } else {
      QCoreApplication::quit();
    }
  }

private:
  std::string bind_;
  int port_ = 0;

  QLabel* statusLabel_ = nullptr;
  QLabel* textLabel_ = nullptr;
  QLabel* heartbeatLabel_ = nullptr;
  QPlainTextEdit* log_ = nullptr;
  QTimer* heartbeatTimer_ = nullptr;

  std::unique_ptr<GuiBridge> bridge_;
  std::unique_ptr<ServerHost> server_;
  bool shutdownRequested_ = false;
};

struct CliOptions {
  std::string bind = "127.0.0.1";
  int port = 0;
  int exitAfterMs = 0;
  bool help = false;
};

void printUsage() {
  std::cerr
      << "usage: tugmcp_qt_widgets_server [--bind ADDR] [--port N]\n"
      << "       [--exit-after-ms N] [--help]\n"
      << "\n"
      << "Starts a Streamable HTTP MCP server (2026-07-28, 2025-11-25 fallback)\n"
      << "inside a Qt 5 Widgets window and prints the bound /mcp URL to stderr.\n"
      << "\n"
      << "  --bind ADDR          listen address (default 127.0.0.1)\n"
      << "  --port N             listen port, 0 picks an ephemeral port (default 0)\n"
      << "  --exit-after-ms N    test hook: close automatically after N ms (0 = off)\n"
      << "  --help               show this help\n";
}

bool parseArgs(int argc, char** argv, CliOptions& options, std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help") {
      options.help = true;
      return true;
    }
    if (argument == "--bind" && index + 1 < argc) {
      options.bind = argv[++index];
      continue;
    }
    if (argument == "--port" && index + 1 < argc) {
      char* end = nullptr;
      const long parsed = std::strtol(argv[++index], &end, 10);
      if (end == nullptr || *end != '\0' || parsed < 0 || parsed > 65535) {
        error = "invalid --port value";
        return false;
      }
      options.port = static_cast<int>(parsed);
      continue;
    }
    if (argument == "--exit-after-ms" && index + 1 < argc) {
      char* end = nullptr;
      const long parsed = std::strtol(argv[++index], &end, 10);
      if (end == nullptr || *end != '\0' || parsed < 0) {
        error = "invalid --exit-after-ms value";
        return false;
      }
      options.exitAfterMs = static_cast<int>(parsed);
      continue;
    }
    error = "unknown or incomplete argument: " + argument;
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char** argv) {
  CliOptions options;
  std::string argumentError;
  if (!parseArgs(argc, argv, options, argumentError)) {
    std::cerr << "tugmcp-qt: " << argumentError << "\n\n";
    printUsage();
    return 2;
  }
  if (options.help) {
    printUsage();
    return 0;
  }

  QApplication app(argc, argv);
  // Closing the window is handled explicitly; the hidden window must not quit
  // the event loop before the server threads have been joined.
  app.setQuitOnLastWindowClosed(false);

  MainWindow window(options.bind, options.port);
  std::string error;
  if (!window.startServer(error)) {
    std::cerr << "tugmcp-qt: " << error << "\n";
    return 1;
  }
  window.show();

  if (options.exitAfterMs > 0) {
    QTimer::singleShot(options.exitAfterMs, &window,
                       [&window]() { window.requestClose(); });
  }

  const int result = app.exec();
  // Normal close paths start shutdown from closeEvent; this also covers a
  // quit triggered some other way.
  window.waitForShutdown();
  std::cerr << "tugmcp-qt: stopped\n";
  return result;
}
