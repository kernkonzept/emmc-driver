/*
 * Copyright (C) 2021, 2023-2024 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

#include <cstdio>
#include "mmio.h"

namespace Hw {

l4_uint64_t
Mmio_space_register_block_base::do_read(l4_addr_t addr, char log2_size) const
{
  l4_uint64_t v;
  if (_mmio_space->mmio_read(addr, log2_size, &v) == L4_EOK)
    return v;
  printf("do_read: %08lx not handled\n", addr);
  return 0;
}

void
Mmio_space_register_block_base::do_write(l4_uint64_t v,
                                         l4_addr_t addr, char log2_size) const
{
  if (_mmio_space->mmio_write(addr, log2_size, v) == L4_EOK)
    return;
  printf("do_write: %08lx not handled\n", addr);
}

} // namespace Hw
