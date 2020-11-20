/* SPDX-License-Identifier: GPL-2.0-only or License-Ref-kk-custom */
/*
 * Copyright (C) 2020 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 */

#include <string>

#include <l4/re/env.h>
#include <l4/re/error_helper>

#include "debug.h"
#include "util.h"

static Dbg info(Dbg::Info, "util");
static Dbg trace(Dbg::Trace, "util");

l4_uint64_t Util::tsc_last;

std::string
Util::readable_size(l4_uint64_t size)
{
  l4_uint32_t order = 1 << 30;
  for (unsigned i = 3;; --i, order >>= 10)
    {
      if (i == 1 || size >= order)
        {
          l4_uint64_t d2 = size / (order / 1024);
          l4_uint64_t d1 = d2 / (1 << 10);
          d2 = (d2 - d1 * 1024) * 10 / 1024;
          std::string s = std::to_string(d1);
          if (d2)
            s += "." + std::to_string(d2);
          return s + " KMG"[i] + "iB";
        }
    }
}

std::string
Util::readable_freq(l4_uint32_t freq)
{
  l4_uint32_t order = 1000000000;
  for (unsigned i = 3;; --i, order /= 1000)
    {
      if (i == 1 || freq >= order)
        {
          l4_uint64_t d2 = freq / (order / 1000);
          l4_uint64_t d1 = d2 / 1000;
          d2 = (d2 - d1 * 1000) / 100;
          std::string s = std::to_string(d1);
          if (d2)
            s += "." + std::to_string(d2);
          return s + " KMG"[i] + "Hz";
        }
    }
}

bool
Util::poll(l4_cpu_time_t us, Util::Poll_timeout_handler handler, char const *s)
{
  info.printf("Waiting for '%s'...\n", s);
  l4_uint64_t time = Util::read_tsc();
  if (!handler())
    {
      auto *kip = l4re_kip();
      l4_cpu_time_t end = l4_kip_clock(kip) + us;
      for (;;)
        {
          if (handler())
            break;
          if (l4_kip_clock(kip) >= end)
            {
              trace.printf("...timeout.\n");
              L4Re::throw_error(-L4_EIO, s);
            }
        }
    }

  time = Util::read_tsc() - time;
  if (Util::freq_tsc())
    {
      l4_uint64_t us = time * 1000000 / Util::freq_tsc();
      info.printf("...done %s(%lluus).\n", us >= 10 ? "\033[31;1m" : "", us);
    }
  else
    info.printf("...done.\n");
  return true;
}
