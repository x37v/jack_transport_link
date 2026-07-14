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
  fs::path base = xdg ? fs::path(xdg)
                      : fs::path(std::getenv("HOME")) / ".config";
  return (base / "jack-transport-link" / "config.json").string();
}

static nlohmann::json loadConfig(const std::string& path) {
  std::ifstream f(path);
  if (!f.is_open()) return nlohmann::json::object();
  try {
    return nlohmann::json::parse(f);
  } catch (...) {
    std::cerr << "warning: failed to parse config " << path << ", ignoring\n";
    return nlohmann::json::object();
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

  parser.add_option("--link-audio-in-stereo-channels")
      .type("int")
      .dest("link_audio_in_stereo_channels")
      .set_default("1")
      .help("Number of Link Audio stereo send pairs, default: %default.");

  parser.add_option("--link-audio-out-stereo-channels")
      .type("int")
      .dest("link_audio_out_stereo_channels")
      .set_default("0")
      .help("Number of Link Audio stereo receive pairs, default: %default.");

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
                                       : cfg.value(key, (double)options.get(key));
  };

  bool enableStartStopSync = options.is_set_by_user("start_stop_sync")
      ? (bool)options.get("start_stop_sync")
      : cfg.value("start_stop_sync", (bool)options.get("start_stop_sync"));
  double initialBPM        = cfgDouble("bpm",     100.0);
  double initialQuantum    = cfgDouble("quantum",   4.0);
  float  initialTimeSigDenom = options.is_set_by_user("denom")
      ? (float)(double)options.get("denom")
      : (float)cfg.value("time_sig_denom", (double)options.get("denom"));
  double initialTicksPerBeat = options.is_set_by_user("ticks")
      ? (double)options.get("ticks")
      : cfg.value("ticks_per_beat", (double)options.get("ticks"));
  std::string name = options["name"];
  int oscport = options.get("oscport");
  bool enableLinkAudio = options.is_set_by_user("link_audio")
      ? static_cast<bool>(options.get("link_audio"))
      : cfg.value("link_audio_enabled", static_cast<bool>(options.get("link_audio")));
  size_t linkAudioInChannels = options.is_set_by_user("link_audio_in_stereo_channels")
      ? static_cast<size_t>(static_cast<int>(options.get("link_audio_in_stereo_channels")))
      : static_cast<size_t>(cfg.value("in_stereo_channels",
            static_cast<int>(options.get("link_audio_in_stereo_channels"))));
  size_t linkAudioOutChannels = options.is_set_by_user("link_audio_out_stereo_channels")
      ? static_cast<size_t>(static_cast<int>(options.get("link_audio_out_stereo_channels")))
      : static_cast<size_t>(cfg.value("out_stereo_channels",
            static_cast<int>(options.get("link_audio_out_stereo_channels"))));
  bool initialSyncLink = cfg.value("sync", true);

  auto loadNames = [&](const char* key) -> std::vector<std::string> {
    std::vector<std::string> out;
    if (cfg.contains(key) && cfg[key].is_array()) {
      for (const auto& v : cfg[key])
        out.push_back(v.is_string() ? v.get<std::string>() : std::string());
    }
    return out;
  };
  std::vector<std::string> sinkNames   = loadNames("sink_names");
  std::vector<std::string> sourceNames = loadNames("source_names");

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
                          enableLinkAudio, linkAudioInChannels,
                          linkAudioOutChannels,
                          initialSyncLink, configPath,
                          sinkNames, sourceNames);
      if (cfg.contains("source_filters"))
        j.applySourceFiltersFromConfig(cfg["source_filters"].dump());

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
