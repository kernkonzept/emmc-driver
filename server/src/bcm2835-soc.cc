/*
 * Copyright (C) 2024 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
/**
 * \file
 * Simple SoC driver for bcm2835 providing access to board-specific features.
 */

#include <l4/sys/l4int.h>

#include "bcm2835-soc.h"

Bcm2835_soc::Bcm2835_soc(L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma)
: mbox(dma)
{}

l4_uint32_t
Bcm2835_soc::get_fw_gpio(unsigned offset)
{
  Bcm2835_mbox::Message<Bcm2835_mbox::Tag::Type::Get_gpio_state> msg;
  msg.data[0] = 128 + offset;
  mbox.send(msg.raw());
  return msg.data[1];
}

void
Bcm2835_soc::set_fw_gpio(unsigned offset, l4_uint32_t value)
{
  Bcm2835_mbox::Message<Bcm2835_mbox::Tag::Type::Set_gpio_state> msg;
  msg.data[0] = 128 + offset;
  msg.data[1] = value;
  mbox.send(msg.raw());
}

l4_uint32_t
Bcm2835_soc::get_board_rev()
{
  Bcm2835_mbox::Message<Bcm2835_mbox::Tag::Type::Get_board_rev> msg;
  mbox.send(msg.raw());
  return msg.data[0];
}
