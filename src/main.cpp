#include "JackTransportLink.hpp"

#include <OptionParser.h>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <thread>

#include <ip/UdpSocket.h>
#include <nlohmann/json.hpp>
#include <osc/OscPacketListener.h>
#include <osc/OscReceivedElements.h>

#include <iostream>

static std::string defaultConfigPath() {
  namespace fs = std::filesystem;
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  fs::path base;
  if (xdg && *xdg) {
    base = fs::path(xdg);
  } else {
    const char* home = std::getenv("HOME");
    if (!home || !*home) {
      std::cerr << "warning: HOME and XDG_CONFIG_HOME are unset; "
                   "automatic config persistence is disabled\n";
      return {};
    }
    base = fs::path(home) / ".config";
  }
  return (base / "jack-transport-link" / "config.json").string();
}

static nlohmann::json loadConfig(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return nlohmann::json::object();
  try {
    auto config = nlohmann::json::parse(f);
    if (!config.is_object()) {
      std::cerr << "warning: config " << path
                << " must contain a JSON object, ignoring\n";
      return nlohmann::json::object();
    }
    return config;
  } catch (...) {
    std::cerr << "warning: failed to parse config " << path << ", ignoring\n";
    return nlohmann::json::object();
  }
}

template <typename T>
static T configValue(const nlohmann::json& config, const char* key,
                     T fallback) {
  const auto it = config.find(key);
  if (it == config.end())
    return fallback;
  try {
    return it->get<T>();
  } catch (const nlohmann::json::exception&) {
    std::cerr << "warning: config key \"" << key
              << "\" has the wrong type, using the default\n";
    return fallback;
  }
}

// TODO windows?
std::atomic<bool> run = true;
void signal_handler(int signal) { run.store(false); }

// the current session (server instance)
std::atomic<bool> runSession = true;
void shutdown_handler(void *) {
  std::cout << "shutdown" << std::endl;
  runSession.store(false);
}

int main(int argc, char *argv[]) {
  // the period with which we check for the program exit condition
  const auto runPollPeriod = std::chrono::milliseconds(10);

  // setup options
  auto parser = optparse::OptionParser().description("Jack Transport Link");
  parser.set_defaults("start_stop_sync", "1");
  parser.set_defaults("start_server", "0");

  parser.add_option("-s", "--start-stop-sync")
      .help("synchronize starts and stops with other start/stop enabled link "
            "clients")
      .action("store_true")
      .dest("start_stop_sync");
  parser.add_option("-S", "--no-start-stop-sync")
      .help("do not synchronize starts and stops with other start/stop enabled "
            "link client")
      .action("store_false")
      .dest("start_stop_sync");

  parser.add_option("-j", "--start-server")
      .help("start the jack server if it isn't already running")
      .action("store_true")
      .dest("start_server");

  parser.add_option("-J", "--no-start-server")
      .help("do not start the jack server if it isn't already running")
      .action("store_false")
      .dest("start_server");

  parser.add_option("-p", "--server-poll-period")
      .type("int")
      .help("the period, in seconds, between attempts to create a jack client, "
            "default: %default")
      .action("store")
      .dest("poll_seconds")
      .set_default("2");
  parser.add_option("-b", "--initial-bpm")
      .type("double")
      .help("the initial BPM to set the transport to, if it isn't already set, "
            "default: %default")
      .action("store")
      .dest("bpm")
      .set_default("100.0");
  parser.add_option("-q", "--initial-quantum")
      .type("double")
      .help("the initial quantum (time signature numerator) to set the "
            "transport to, if it isn't already set, default: %default")
      .action("store")
      .dest("quantum")
      .set_default("4.0");
  parser.add_option("-d", "--initial-denom")
      .type("double")
      .help("the initial time signature denominator to set the transport to, "
            "if it isn't already set, default: %default")
      .action("store")
      .dest("denom")
      .set_default("4.0");
  parser.add_option("-t", "--initial-ticks-per-beat")
      .type("double")
      .help("the initial ticks per beat use for the transport, if it isn't "
            "already set, default: %default")
      .action("store")
      .dest("ticks")
      .set_default("1920.0");
  parser.add_option("-n", "--jack-client-name")
      .type("string")
      .help("the name to give to the jack client, default: %default")
      .action("store")
      .dest("name")
      .set_default("jack-transport-link");
  parser.add_option("-N", "--link-name")
      .type("string")
      .help("the Link peer name to broadcast (identifies this device in Ableton "
            "Live and to other Link peers); empty uses the hostname, default: %default")
      .action("store")
      .dest("link_name")
      .set_default("");
  parser.add_option("-o", "--osc-port")
      .type("int")
      .help("the name to give to the jack client, default: %default")
      .action("store")
      .dest("oscport")
      .set_default("-1");

  parser.add_option("-c", "--config")
      .type("string")
      .dest("config")
      .set_default("")
      .help("path to config file (default: ~/.config/jack-transport-link/config.json)");

  parser.add_option("-A", "--no-link-audio")
      .action("store_false")
      .dest("link_audio")
      .set_default("1")
      .help("Disable Link Audio (network audio streaming). Enabled by default.");

  parser.add_option("--capture-latency-trim-ms")
      .type("double")
      .dest("capture_latency_trim_ms")
      .set_default("0.0")
      .help("Trim (ms) added to JACK's auto-detected capture latency for Link Audio send "
            "alignment; covers converter latency JACK can't see. default: %default.");

  parser.add_option("--playback-latency-trim-ms")
      .type("double")
      .dest("playback_latency_trim_ms")
      .set_default("0.0")
      .help("Trim (ms) added to JACK's auto-detected playback latency for Link Audio receive "
            "alignment; covers converter latency JACK can't see. default: %default.");

  parser.add_option("--latency-ms")
      .type("double")
      .dest("latency_ms")
      .set_default("100.0")
      .help("Link Audio receiver playout buffer in milliseconds (converted to beats at the "
            "current tempo), range 0-2000. default: %default.");

  parser.set_defaults("sync_to_incoming", "0");
  parser.add_option("--sync-to-incoming")
      .action("store_true")
      .dest("sync_to_incoming")
      .help("Sync to Incoming Audio: delay the local transport timeline by the receive buffer so "
            "transport-locked generators align with incoming audio (disabled by default). The "
            "receive buffer is always applied regardless of this option.");
  parser.add_option("--no-sync-to-incoming")
      .action("store_false")
      .dest("sync_to_incoming")
      .help("Disable Sync to Incoming Audio: the local transport runs live; incoming audio is "
            "still buffered and audible, just not aligned with local generators.");

  parser.set_defaults("link_enabled", "1");
  parser.add_option("--link")
      .action("store_true")
      .dest("link_enabled")
      .help("Join the Ableton Link session (enabled by default).");
  parser.add_option("--no-link")
      .action("store_false")
      .dest("link_enabled")
      .help("Do not join the Ableton Link session: peers don't see this device, and tempo sync "
            "and Link Audio are inactive; jtl still runs as the local JACK transport master.");

  // process args
  optparse::Values options = parser.parse_args(argc, argv);
  std::vector<std::string> args = parser.args();

  // setup signal handlers
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  // setup initial conditions and read in options
  jack_options_t jackOptions = (bool)options.get("start_server")
                                   ? JackOptions::JackNullOption
                                   : JackOptions::JackNoStartServer;
  std::chrono::duration serverPollPeriod =
      std::chrono::seconds((long)options.get("poll_seconds"));

  std::string configPath = options.is_set_by_user("config")
      ? options["config"] : defaultConfigPath();
  auto cfg = loadConfig(configPath);

  auto cfgDouble = [&](const char* key, double fallback) -> double {
    return options.is_set_by_user(key) ? (double)options.get(key)
                                       : configValue(cfg, key, fallback);
  };

  bool enableStartStopSync = options.is_set_by_user("start_stop_sync")
      ? (bool)options.get("start_stop_sync")
      : configValue(cfg, "start_stop_sync",
                    (bool)options.get("start_stop_sync"));
  double initialBPM        = cfgDouble("bpm",     100.0);
  double initialQuantum    = cfgDouble("quantum",   4.0);
  float  initialTimeSigDenom = options.is_set_by_user("denom")
      ? (float)(double)options.get("denom")
      : (float)configValue(cfg, "time_sig_denom",
                           (double)options.get("denom"));
  double initialTicksPerBeat = options.is_set_by_user("ticks")
      ? (double)options.get("ticks")
      : configValue(cfg, "ticks_per_beat", (double)options.get("ticks"));
  std::string name = options["name"];
  int oscport = options.get("oscport");
  bool enableLinkAudio = options.is_set_by_user("link_audio")
      ? static_cast<bool>(options.get("link_audio"))
      : configValue(cfg, "link_audio_enabled",
                    static_cast<bool>(options.get("link_audio")));
  bool initialSyncLink = configValue(cfg, "sync", true);

  // Explicit sink/source lists, in display order. Slot keys aren't persisted — jack_transport_link
  // re-derives them from these identities, so the JACK port names come back the same.
  std::vector<std::string> sinkNames;
  if (cfg.contains("sinks") && cfg["sinks"].is_array()) {
    for (const auto& v : cfg["sinks"]) {
      if (!v.is_object()) continue;
      auto name = v.value("name", std::string());
      if (!name.empty())
        sinkNames.push_back(std::move(name));
    }
  }
  std::vector<std::pair<std::string, std::string>> sources;
  if (cfg.contains("sources") && cfg["sources"].is_array()) {
    for (const auto& v : cfg["sources"]) {
      if (!v.is_object()) continue;
      auto channel = v.value("channel", std::string());
      if (channel.empty()) continue;
      sources.emplace_back(v.value("peer", std::string()), std::move(channel));
    }
  }

  // Link peer-name override (empty = auto/hostname): CLI wins over config.
  std::string linkPeerName = options.is_set_by_user("link_name")
      ? options["link_name"]
      : configValue(cfg, "link_peer_name", std::string());

  // Per-direction I/O latency trims (ms), added to JACK's auto-detected latency: CLI wins over config.
  double captureLatencyTrimMs = options.is_set_by_user("capture_latency_trim_ms")
      ? (double)options.get("capture_latency_trim_ms")
      : configValue(cfg, "link_audio_capture_latency_trim_ms",
                    (double)options.get("capture_latency_trim_ms"));
  double playbackLatencyTrimMs = options.is_set_by_user("playback_latency_trim_ms")
      ? (double)options.get("playback_latency_trim_ms")
      : configValue(cfg, "link_audio_playback_latency_trim_ms",
                    (double)options.get("playback_latency_trim_ms"));
  double latencyMs = options.is_set_by_user("latency_ms")
      ? (double)options.get("latency_ms")
      : configValue(cfg, "link_audio_latency_ms",
                    (double)options.get("latency_ms"));
  bool syncToIncomingAudio = options.is_set_by_user("sync_to_incoming")
      ? (bool)options.get("sync_to_incoming")
      : configValue(cfg, "link_audio_sync_to_incoming",
                    (bool)options.get("sync_to_incoming"));
  bool linkEnabled = options.is_set_by_user("link_enabled")
      ? (bool)options.get("link_enabled")
      : configValue(cfg, "link_enabled",
                    (bool)options.get("link_enabled"));

  if (initialBPM <= 0.0 || initialQuantum < 1.0 || initialTimeSigDenom < 1.0 ||
      initialTicksPerBeat < 1.0) {
    std::cerr << "one or more numeric options are out of range" << std::endl;
    return -1;
  }

  std::unique_ptr<oscpack::UdpListeningReceiveSocket> oscsocket;
  while (run.load()) {
    jack_status_t status;
    auto client = jack_client_open(name.c_str(), jackOptions, &status);
    if (client != nullptr) {
      runSession.store(true);
      jack_on_shutdown(client, shutdown_handler, nullptr);
      JackTransportLink j(client, enableStartStopSync, initialBPM,
                          initialQuantum, initialTimeSigDenom,
                          initialTicksPerBeat,
                          enableLinkAudio,
                          initialSyncLink, configPath,
                          sinkNames, sources, linkPeerName,
                          captureLatencyTrimMs, playbackLatencyTrimMs, latencyMs,
                          syncToIncomingAudio, linkEnabled);

      if (oscport > 0) {
        try {
          oscpack::IpEndpointName oscendpoint(
              oscpack::IpEndpointName::ANY_ADDRESS, oscport);
          oscsocket = std::make_unique<oscpack::UdpListeningReceiveSocket>(
              oscendpoint, &j);
        } catch (std::runtime_error &e) {
          std::cerr << "error creating osc socket " << e.what() << std::endl;
        }
      }

      // run osc in a thread
      std::thread oscthread;
      if (oscsocket) {
        oscthread = std::thread([&oscsocket]() { oscsocket->Run(); });
      }

      while (run.load() && runSession.load()) {
        std::this_thread::sleep_for(runPollPeriod);
        j.processEvents();
      }

      // cleanup osc
      if (oscthread.joinable() && oscsocket) {
        oscsocket->AsynchronousBreak();
        oscthread.join();
      }
      oscsocket.reset();
    } else {
      // sleep and check for poll period timeout
      using std::chrono::system_clock;
      auto timeout = system_clock::now() + serverPollPeriod;
      while (run.load() && system_clock::now() < timeout) {
        std::this_thread::sleep_for(runPollPeriod);
      }
    }
  }
  return 0;
}
