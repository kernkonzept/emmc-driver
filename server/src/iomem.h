/* SPDX-License-Identifier: GPL-2.0-only or License-Ref-kk-custom */
/*
 * Copyright (C) 2020-2021 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *            Jakub Jermar <jakub.jermar@kernkonzept.com>
 */

#pragma once

#include <l4/re/env>
#include <l4/re/error_helper>

/**
 * Self-attaching IO memory.
 */
struct Iomem
{
  L4Re::Rm::Unique_region<l4_addr_t> vaddr;

  Iomem(l4_addr_t phys_addr, L4::Cap<L4Re::Dataspace> iocap)
  {
    auto *e = L4Re::Env::env();
    L4Re::chksys(e->rm()->attach(&vaddr, 4096,
                                 L4Re::Rm::F::Search_addr
                                 | L4Re::Rm::F::Cache_uncached
                                 | L4Re::Rm::F::RW,
                                 L4::Ipc::make_cap_rw(iocap), phys_addr,
                                 L4_PAGESHIFT),
                 "Attach in/out buffer.");
  }
};
