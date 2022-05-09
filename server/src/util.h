/*
 * Copyright (C) 2020 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#pragma once

#include <string>
#include <functional>

struct Util
{
  /// Return descriptive string like '5.6MiB' or similar.
  static std::string readable_size(l4_uint64_t size);

  /// Return descriptive string like '6.7MHz' or similar.
  static std::string readable_freq(l4_uint32_t freq);

  static char printable(char c) { return c >= ' ' ? c : ' '; }

  typedef std::function<bool ()> Poll_timeout_handler;
  static bool poll(l4_cpu_time_t us, Poll_timeout_handler handler,
                   char const *s);

  /// Only for tracing.
  static l4_uint64_t read_tsc()
  {
#ifdef ARCH_arm64
    l4_uint64_t v;
    asm volatile("mrs %0, CNTVCT_EL0" : "=r" (v));
    return v;
#else
    return 0;
#endif
  }

  /// Only for tracing.
  static l4_uint64_t freq_tsc()
  {
#ifdef ARCH_arm64
    l4_uint64_t v;
    asm volatile("mrs %0, CNTFRQ_EL0": "=r" (v));
    return v;
#else
    return 0;
#endif
  }

  static l4_uint64_t tsc_to_us(l4_uint64_t tsc)
  {
    l4_uint64_t freq = freq_tsc();
    return freq ? tsc * 1000000 / freq : 0;
  }

  static l4_uint64_t tsc_to_ms(l4_uint64_t tsc)
  {
    l4_uint64_t freq = freq_tsc();
    return freq ? tsc * 1000 / freq : 0;
  }

  static l4_uint64_t diff_tsc()
  {
    l4_uint64_t tsc_now = read_tsc();
    l4_uint64_t tsc_diff = tsc_now - tsc_last;
    tsc_last = tsc_now;
    return tsc_diff;
  }

  static l4_uint64_t tsc_last;
};
