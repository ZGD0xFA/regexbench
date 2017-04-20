#include "config.h"

#include <sys/resource.h>
#include <sys/time.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "BoostEngine.h"
#include "CPPEngine.h"
#ifdef HAVE_HYPERSCAN
#include "HyperscanEngine.h"
#endif
#ifdef HAVE_PCRE2
#include "PCRE2Engine.h"
#endif
#include "PcapSource.h"
#ifdef HAVE_RE2
#include "RE2Engine.h"
#endif
#ifdef HAVE_REMATCH
#include "REmatchEngine.h"
#endif
#include "Rule.h"
#include "regexbench.h"

using boost::property_tree::ptree;
using boost::property_tree::read_json;
using boost::property_tree::write_json;

namespace po = boost::program_options;

enum class EngineType : uint64_t {
  boost,
  std_regex,
  hyperscan,
  pcre2,
  pcre2_jit,
  re2,
  rematch,
  rematch2
};

struct Arguments {
  std::string output_file;
  std::string pcap_file;
  std::string rule_file;
  EngineType engine;
  int32_t repeat;
  uint32_t pcre2_concat;
  uint32_t rematch_session;
  uint32_t num_threads;
  std::vector<size_t> cores;
  bool reduce = {false};
  char paddings[3];
};

template <typename Derived, typename Base, typename Del>
std::unique_ptr<Derived, Del>
static_unique_ptr_cast(std::unique_ptr<Base, Del>&& p)
{
  auto d = static_cast<Derived*>(p.release());
  return std::unique_ptr<Derived, Del>(d, std::move(p.get_deleter()));
}

static bool endsWith(const std::string&, const char*);
static Arguments parse_options(int argc, const char* argv[]);

int main(int argc, const char* argv[])
{
  try {
    auto args = parse_options(argc, argv);
    std::unique_ptr<regexbench::Engine> engine;
    size_t nsessions = 0;
    regexbench::PcapSource pcap(args.pcap_file);
    auto match_info = buildMatchMeta(pcap, nsessions);

    switch (args.engine) {
    case EngineType::boost:
      engine = std::make_unique<regexbench::BoostEngine>();
      engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
      break;
    case EngineType::std_regex:
      engine = std::make_unique<regexbench::CPPEngine>();
      engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
      break;
#ifdef HAVE_HYPERSCAN
    case EngineType::hyperscan:
      if (args.rematch_session) {
        engine = std::make_unique<regexbench::HyperscanEngineStream>();
        engine->init(nsessions);
      } else {
        engine = std::make_unique<regexbench::HyperscanEngine>();
      }
      engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
      break;
#endif
#ifdef HAVE_PCRE2
    case EngineType::pcre2:
      engine = std::make_unique<regexbench::PCRE2Engine>();
      engine->init(args.pcre2_concat);
      engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
      break;
    case EngineType::pcre2_jit:
      engine = std::make_unique<regexbench::PCRE2JITEngine>();
      engine->init(args.pcre2_concat);
      engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
      break;
#endif
#ifdef HAVE_RE2
    case EngineType::re2:
      engine = std::make_unique<regexbench::RE2Engine>();
      engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
      break;
#endif
#ifdef HAVE_REMATCH
    case EngineType::rematch:
      if (args.rematch_session) {
#ifndef REMATCH_WITHOUT_SESSION
        engine = std::make_unique<regexbench::REmatchAutomataEngineSession>();
        engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
#endif
      } else if (endsWith(args.rule_file, ".nfa")) {
        engine = std::make_unique<regexbench::REmatchAutomataEngine>();
        engine->load(args.rule_file, args.num_threads);
      } else if (endsWith(args.rule_file, ".so")) {
        engine = std::make_unique<regexbench::REmatchSOEngine>();
        engine->load(args.rule_file, args.num_threads);
      } else {
        engine =
            std::make_unique<regexbench::REmatchAutomataEngine>(args.reduce);
        engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
      }
      engine->init(nsessions);
      break;
    case EngineType::rematch2:
      if (endsWith(args.rule_file, ".nfa")) {
        engine = std::make_unique<regexbench::REmatch2AutomataEngine>();
        engine->load(args.rule_file, args.num_threads);
      } else {
        engine =
            std::make_unique<regexbench::REmatch2AutomataEngine>(args.reduce);
        engine->compile(regexbench::loadRules(args.rule_file), args.num_threads);
      }
      break;
#endif
    }

    std::string reportFields[]{
        "TotalMatches", "TotalMatchedPackets",  "UserTime",     "SystemTime",
        "TotalTime",    "TotalBytes",           "TotalPackets", "Mbps",
        "Mpps",         "MaximumMemoryUsed(kB)"};
    std::string prefix = "regexbench.";

    std::vector<regexbench::MatchResult> results =
        match(*engine, pcap, args.repeat, args.cores, match_info);

    auto coreIter = args.cores.begin();
    coreIter++; // get rid of main thread
    boost::property_tree::ptree pt;
    for (const auto& result : results) {
      std::stringstream ss;
      ss << "thread" << *coreIter++ << ".";
      std::string corePrefix = prefix + ss.str();
      pt.put(corePrefix + "TotalMatches", result.nmatches);
      pt.put(corePrefix + "TotalMatchedPackets", result.nmatched_pkts);
      ss.str("");
      auto t = result.udiff.tv_sec + result.udiff.tv_usec * 1e-6;
      ss << t;
      pt.put(corePrefix + "UserTime", ss.str());
      ss.str("");
      t = result.sdiff.tv_sec + result.sdiff.tv_usec * 1e-6;
      ss << t;
      pt.put(corePrefix + "SystemTime", ss.str());
      ss.str("");
      struct timeval total;
      timeradd(&result.udiff, &result.sdiff, &total);
      t = total.tv_sec + total.tv_usec * 1e-6;
      ss << t;
      pt.put(corePrefix + "TotalTime", ss.str());
      pt.put(corePrefix + "TotalBytes", pcap.getNumberOfBytes());
      pt.put(corePrefix + "TotalPackets", pcap.getNumberOfPackets());
      ss.str("");
      ss << std::fixed << std::setprecision(6)
        << (static_cast<double>(pcap.getNumberOfBytes() *
              static_cast<unsigned long>(args.repeat)) /
            (total.tv_sec + total.tv_usec * 1e-6) / 1000000 * 8);
      pt.put(corePrefix + "Mbps", ss.str());

      ss.str("");
      ss << std::fixed << std::setprecision(6)
        << (static_cast<double>(pcap.getNumberOfPackets() *
              static_cast<unsigned long>(args.repeat)) /
            (total.tv_sec + total.tv_usec * 1e-6) / 1000000);
      pt.put(corePrefix + "Mpps", ss.str());
      struct rusage stat;
      getrusage(RUSAGE_SELF, &stat);
      pt.put(corePrefix + "MaximumMemoryUsed(kB)", stat.ru_maxrss / 1000);

      for (const auto& it : reportFields) {
        std::cout << it << " : " << pt.get<std::string>(corePrefix + it) << "\n";
      }
      std::cout << std::endl;
    }

    std::ostringstream buf;
    write_json(buf, pt, true);
    std::ofstream outputFile(args.output_file, std::ios_base::trunc);
    outputFile << buf.str();
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

bool endsWith(const std::string& obj, const char* end)
{
  auto r = obj.rfind(end);
  if ((r != std::string::npos) && (r == obj.size() - std::strlen(end)))
    return true;
  return false;
}



static std::vector<size_t> setup_affinity(size_t num, std::string& arg)
{
  auto ncpus = std::thread::hardware_concurrency();

  // num : user specified number of threads of matchers
  //   (main thread is not included here)
  // arg : user specified description of core assignment
  //   (comma separated decimals w/o space in between)

  num += 1; // main thread included
  // firstly, parse comma separated input
  std::replace(arg.begin(), arg.end(), ',', ' ');
  std::istringstream is(arg);

  std::vector<size_t> cores(num); // to return
  int maxCore = static_cast<int>(ncpus - 1);
  try {
    std::istream_iterator<int> iter = std::istream_iterator<int>(is);
    int last = 0;
    std::generate(cores.begin(), cores.end(), [&iter, &last, maxCore]() {
          int core = 0;
          if (iter != std::istream_iterator<int>()) {
            core = std::min(std::max(*iter, 0), maxCore);
            ++iter;
          } else
            core = std::min(last + 1, maxCore);
          last = core;
          return static_cast<size_t>(core);
        });
  } catch (const std::exception&) {
    // some formatting error
    std::cerr << "User provided affinity assignment format error" << std::endl;
    // go with default assignment scheme
    for (int i = 0; i < num; ++i)
      cores[i] = static_cast<size_t>((i > maxCore) ? maxCore : i);
  }
  return cores;
}


Arguments parse_options(int argc, const char* argv[])
{
  Arguments args;
  std::string engine;
  std::string affinity;

  po::options_description posargs;
  posargs.add_options()("rule_file", po::value<std::string>(&args.rule_file),
                        "Rule (regular expression) file name");
  posargs.add_options()("pcap_file", po::value<std::string>(&args.pcap_file),
                        "pcap file name");
  po::positional_options_description positions;
  positions.add("rule_file", 1).add("pcap_file", 1);

  po::options_description optargs("Options");
  optargs.add_options()("help,h", "Print usage information.");
  optargs.add_options()(
      "engine,e", po::value<std::string>(&engine)->default_value("hyperscan"),
      "Matching engine to run.");
  optargs.add_options()("repeat,r",
                        po::value<int32_t>(&args.repeat)->default_value(1),
                        "Repeat pcap multiple times.");
  optargs.add_options()(
      "concat,c", po::value<uint32_t>(&args.pcre2_concat)->default_value(0),
      "Concatenate PCRE2 rules.");
  optargs.add_options()(
      "session,s", po::value<uint32_t>(&args.rematch_session)->default_value(0),
      "Rematch session mode.");
  optargs.add_options()(
      "output,o",
      po::value<std::string>(&args.output_file)->default_value("output.json"),
      "Output JSON file.");
  optargs.add_options()(
      "threads,n", po::value<uint32_t>(&args.num_threads)->default_value(1),
      "Number of threads.");
  optargs.add_options()(
      "affinity,a", po::value<std::string>(&affinity)->default_value("0"),
      "Core affinity assignment (starting from main thread)");
  optargs.add_options()("reduce,R",
                        po::value<bool>(&args.reduce)->default_value(false),
                        "Use REduce with REmatch, default is false");
  po::options_description cliargs;
  cliargs.add(posargs).add(optargs);
  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv)
                .options(cliargs)
                .positional(positions)
                .run(),
            vm);
  po::notify(vm);

  if (vm.count("help")) {
    std::cout << "Usage: regexbench <rule_file> <pcap_file>" << std::endl;
    std::cout << posargs << "\n" << optargs << "\n";
    std::exit(EXIT_SUCCESS);
  }
  if (engine == "boost")
    args.engine = EngineType::boost;
  else if (engine == "cpp")
    args.engine = EngineType::std_regex;
#ifdef HAVE_HYPERSCAN
  else if (engine == "hyperscan")
    args.engine = EngineType::hyperscan;
#endif
#ifdef HAVE_PCRE2
  else if (engine == "pcre2")
    args.engine = EngineType::pcre2;
  else if (engine == "pcre2jit")
    args.engine = EngineType::pcre2_jit;
#endif
#ifdef HAVE_RE2
  else if (engine == "re2")
    args.engine = EngineType::re2;
#endif
#ifdef HAVE_REMATCH
  else if (engine == "rematch")
    args.engine = EngineType::rematch;
  else if (engine == "rematch2")
    args.engine = EngineType::rematch2;
#endif
  else {
    std::cerr << "unknown engine: " << engine << std::endl;
    std::exit(EXIT_FAILURE);
  }
  if (args.repeat <= 0) {
    std::cerr << "invalid repeat value: " << args.repeat << std::endl;
    std::exit(EXIT_FAILURE);
  }
  if (args.num_threads < 1) {
    std::cerr << "invalid number of threads: " << args.num_threads << std::endl;
    std::cerr << " (should be >= 1 .. overriding it to 1" << std::endl;
    args.num_threads = 1;
  }
  std::cout << "number of threads : " << args.num_threads << std::endl;
  args.cores = setup_affinity(args.num_threads, affinity);
  std::cout << "affinity setup is ..." << std::endl;
  for (auto core : args.cores)
    std::cout << " " << core;
  std::cout << std::endl;

#ifdef REMATCH_WITHOUT_SESSION
  if ((engine == "rematch" || engine == "rematch2") && args.rematch_session) {
    std::cerr << "not supporting session mode for now" << std::endl;
    args.rematch_session = 0;
  }
#endif

  if (!vm.count("rule_file")) {
    std::cerr << "error: no rule file" << std::endl;
    std::exit(EXIT_FAILURE);
  }
  if (!vm.count("pcap_file")) {
    std::cerr << "error: no pcap file" << std::endl;
    std::exit(EXIT_FAILURE);
  }
  return args;
}
