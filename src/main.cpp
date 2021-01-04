#include "JackTransportLink.hpp"

#include <chrono>
#include <csignal>
#include <OptionParser.h>

//TODO windows?
std::atomic<bool> run = true;
void signal_handler(int signal) {
  run.store(false);
}

int main(int argc, char * argv[]) {
  //the period with which we check for the program exit condition
  const auto runPollPeriod = std::chrono::milliseconds(10);

  //setup options
  auto parser = optparse::OptionParser().description("Jack Transport Link");
  parser
    .add_option("-s", "--start-stop-sync")
    .type("bool")
    .help("synchronize starts and stops with other start/stop enabled link clients, default: %default")
    .action("store")
    .dest("start_stop_sync")
    .set_default("true");
  parser
    .add_option("-j", "--start-server")
    .type("bool")
    .help("start the jack server if it isn't already running, default: %default")
    .action("store")
    .dest("start_server")
    .set_default("false");
  parser
    .add_option("-p", "--server-poll-period")
    .type("int")
    .help("the period, in seconds, between attempts to create a jack client, default: %default")
    .action("store")
    .dest("poll_seconds")
    .set_default("2");
  parser
    .add_option("-b", "--initial-bpm")
    .type("double")
    .help("the initial BPM to set the transport to, if it isn't already set, default: %default")
    .action("store")
    .dest("bpm")
    .set_default("100.0");
  parser
    .add_option("-q", "--initial-quantum")
    .type("double")
    .help("the initial quantum (time signature numerator) to set the transport to, if it isn't already set, default: %default")
    .action("store")
    .dest("quantum")
    .set_default("4.0");
  parser
    .add_option("-d", "--initial-denom")
    .type("double")
    .help("the initial time signature denominator to set the transport to, if it isn't already set, default: %default")
    .action("store")
    .dest("denom")
    .set_default("4.0");
  parser
    .add_option("-t", "--initial-ticks-per-beat")
    .type("double")
    .help("the initial ticks per beat use for the transport, if it isn't already set, default: %default")
    .action("store")
    .dest("ticks")
    .set_default("1920.0");
  parser
    .add_option("-n", "--jack-client-name")
    .type("string")
    .help("the name to give to the jack client, default: %default")
    .action("store")
    .dest("name")
    .set_default("jack-transport-link");

  //process args
	optparse::Values options = parser.parse_args(argc, argv);
	std::vector<std::string> args = parser.args();

  //setup signal handlers
  std::signal(SIGINT, signal_handler);
  std::signal(SIGTERM, signal_handler);

  //setup initial conditions and read in options
  jack_options_t jackOptions = (bool)options.get("start_server") ? JackOptions::JackNullOption : JackOptions::JackNoStartServer;
  std::chrono::duration serverPollPeriod = std::chrono::seconds((long)options.get("poll_seconds"));

  bool enableStartStopSync = options.get("start_stop_sync");
  double initialBPM = options.get("bpm");
  double initialQuantum = options.get("quantum");
  float initialTimeSigDenom = options.get("denom");
  double initialTicksPerBeat = options.get("ticks");
  std::string name = options["name"];

  if (initialBPM <= 0.0 || initialQuantum < 1.0 || initialTimeSigDenom < 1.0 || initialTicksPerBeat < 1.0) {
    std::cerr << "one or more numeric options are out of range" << std::endl;
    return -1;
  }

  while (run.load()) {
    jack_status_t status;
    auto client = jack_client_open(name.c_str(), jackOptions, &status);
    if (client != nullptr) {
      JackTransportLink j(
          client,
          enableStartStopSync,
          initialBPM,
          initialQuantum,
          initialTimeSigDenom,
          initialTicksPerBeat
          );
      while (run.load()) {
        std::this_thread::sleep_for(runPollPeriod);
      }
    } else {
      //sleep and check for poll period timeout
      using std::chrono::system_clock;
      auto timeout = system_clock::now() + serverPollPeriod;
      while (run.load() && system_clock::now() < timeout) {
        std::this_thread::sleep_for(runPollPeriod);
      }
    }
  }
  return 0;
}
