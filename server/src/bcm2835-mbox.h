/*
 * Copyright (C) 2024 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */
/**
 * \file
 */

#pragma once

#include <l4/re/dma_space>
#include <l4/sys/l4int.h>
#include <l4/drivers/hw_mmio_register_block>

#include "inout_buffer.h"

class Bcm2835_mbox
{
public:
  enum class Chan : l4_uint32_t
  {
    Property = 8,
    Max = 15,
  };

  struct Hdr
  {
    enum class Status : l4_uint32_t
    {
      Request = 0U,
      Success = 0x80000000U,
      Error   = 0x80000001U,
    };
    l4_uint32_t size;
    Status status;
  };

  struct Tag
  {
    enum class Type : l4_uint32_t
    {
      Get_board_rev  = 0x00010002,
      Get_gpio_state = 0x00030041,
      Set_gpio_state = 0x00038041,
    };
    Type tag;                   // firmware property tag: tag
    l4_uint32_t size_put;       // firmware property tag: value size
    l4_uint32_t size_get;       // firmware property tag: response size
  };

  static constexpr unsigned tag_words(Tag::Type tag_type)
  {
    switch (tag_type)
      {
      case Tag::Type::Get_board_rev: return 1;
      case Tag::Type::Get_gpio_state: return 2;
      case Tag::Type::Set_gpio_state: return 2;
      default: return 0;
      }
  };

  template <Tag::Type tag_type>
  struct Message
  {
    l4_uint32_t *raw() { return &hdr.size; }
    Hdr hdr = { 4 * (tag_words(tag_type) + 6), Hdr::Status::Request };
    Tag tag = { tag_type, 4 * tag_words(tag_type), 0 };
    l4_uint32_t data[tag_words(tag_type)] = { 0, };
    l4_uint32_t terminator = 0;
  };

  Bcm2835_mbox(L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma);
  void send(l4_uint32_t *msg);

private:
  void status_wait_bit(unsigned bit);
  void send_mail(l4_uint32_t letter, Chan channel);
  l4_uint32_t read_mail(Chan channel);

  L4drivers::Register_block<32> _regs;
  Inout_buffer _data;
  l4_addr_t _data_phys;
  void *_data_virt;
  bool busy = false;
};
