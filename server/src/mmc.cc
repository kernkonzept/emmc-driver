/* SPDX-License-Identifier: GPL-2.0-only or License-Ref-kk-custom */
/*
 * Copyright (C) 2020 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 */

#include <cstdio>

#include "mmc.h"

namespace Emmc {

void Mmc::Reg_csd::dump() const
{
  for (unsigned i = 0; i < sizeof(*this); ++i)
    {
      printf("%02x ", ((l4_uint8_t const *)this)[i]);
      if (!((i + 1) % 16))
        printf("\n");
    }
}

void Mmc::Reg_ecsd::dump() const
{
  for (unsigned i = 0; i < sizeof(*this); ++i)
    {
      printf("%02x ", ((l4_uint8_t const *)this)[i]);
      if (!((i + 1) % 16))
        printf("\n");
    }
}

} // namespace Emmc
