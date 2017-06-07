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

#include "regexbench.h"

using namespace regexbench;

static std::map<std::string, size_t>
make_statistic(const uint32_t sec, const struct ResultInfo& stat);
static struct ResultInfo realtime(std::vector<MatchResult>& results);
static struct ResultInfo total(std::vector<MatchResult>& results);

void regexbench::statistic(const uint32_t sec,
                           std::vector<MatchResult>& results, realtimeFunc func)
{
  struct ResultInfo stat = sec ? realtime(results) : total(results);
  std::map<std::string, size_t> m = make_statistic(sec, stat);

  if (func == nullptr)
    realtimeReport(m);
  else
    func(m);
}

static struct ResultInfo realtime(std::vector<MatchResult>& results)
{
  struct ResultInfo stat;

  for (auto& r : results) {
    const auto cur = r.cur;
    auto& old = (r.old);

    stat.nmatches += cur.nmatches - old.nmatches;
    stat.nmatched_pkts += cur.nmatched_pkts - old.nmatched_pkts;
    stat.npkts += cur.npkts - old.npkts;
    stat.nbytes += cur.nbytes - old.nbytes;

    old = cur;
  }

  return stat;
}

static struct ResultInfo total(std::vector<MatchResult>& results)
{
  struct ResultInfo stat;

  for (auto& r : results) {
    const struct ResultInfo& cur = r.cur;

    stat.nmatches += cur.nmatches;
    stat.nmatched_pkts += cur.nmatched_pkts;
    stat.npkts += cur.npkts;
    stat.nbytes += cur.nbytes;
  }

  return stat;
}

static std::map<std::string, size_t>
make_statistic(const uint32_t sec, const struct ResultInfo& stat)
{
  std::map<std::string, size_t> m;

  m["Sec"] = sec;
  m["Matches"] = stat.nmatches;
  m["MatchedPackets"] = stat.nmatched_pkts;
  m["Packets"] = stat.npkts;
  m["Bytes"] = stat.nbytes;

  return m;
}
