// Bit-exactness of pluto_ticks_to_ns / pluto_ns_to_ticks against SoapySDR 0.8.1 (lib/TimeC.cpp,
// the implementation behind SoapySDR::ticksToTimeNs / timeNsToTicks that nexus2 and the gadget
// streamers use). Built only when SOAPY_SRC points at a SoapySDR source tree.
//
// SPDX-License-Identifier: GPL-2.0-or-later
#include <SoapySDR/Time.hpp>
#include <cstdio>

#include "pluto_framing.h"

int main()
{
  const double rates[] = {200000.0, 600000.0, 1000000.0, 1600000.0, 599999.0, 599999.7, 65105.0,
                          3.84e6, 30.72e6, 61.44e6};
  long checks = 0, failures = 0;
  for (double rate : rates) {
    for (long long t = 0; t < (1LL << 42); t = t * 5 / 4 + 3) {
      ++checks;
      if (pluto_ticks_to_ns(t, rate) != SoapySDR::ticksToTimeNs(t, rate)) {
        if (failures++ < 5)
          std::printf("ticks->ns differs at rate %f ticks %lld\n", rate, t);
      }
      long long ns = t * 997;
      ++checks;
      if (pluto_ns_to_ticks(ns, rate) != SoapySDR::timeNsToTicks(ns, rate)) {
        if (failures++ < 5)
          std::printf("ns->ticks differs at rate %f ns %lld\n", rate, ns);
      }
    }
  }
  std::printf("test_time_soapy: %ld checks, %ld failures\n", checks, failures);
  return failures ? 1 : 0;
}
