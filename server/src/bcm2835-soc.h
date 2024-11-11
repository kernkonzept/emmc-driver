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

#include <l4/cxx/bitfield>
#include "bcm2835-mbox.h"

class Bcm2835_soc
{
public:
  enum
  {
    Raspi_exp_gpio_bt = 0,              // active high
    Raspi_exp_gpio_wifi = 1,            // active low
    Raspi_exp_gpio_led_pwr = 2,         // active low
    Raspi_exp_gpio_vdd_sd_io_sel = 4,   // active high
    Raspi_exp_gpio_cam1 = 5,            // active high
    Raspi_exp_gpio_vcc_sd = 6,          // active high
  };

  Bcm2835_soc(L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma);

  /**
   * Get GPIO pin to a certain value.
   * In the Linux device tree, look for `expgpio` entries.
   */
  l4_uint32_t get_fw_gpio(unsigned offset);

  /**
   * Set GPIO pin to a certain value.
   * In the Linux device tree, look for `expgpio` entries.
   */
  void set_fw_gpio(unsigned offset, l4_uint32_t value);

  /** Get board revision. */
  l4_uint32_t get_board_rev();

private:
  Bcm2835_mbox mbox;
};

/**
 * bcm2835 SoC revision number decoding.
 *
 * See  See https://github.com/raspberrypi/documentation/blob/develop/
 *      documentation/asciidoc/computers/raspberry-pi/revision-codes.adoc
 */
struct Bcm2835_soc_rev
{
  l4_uint32_t raw;
  /** Overvoltage allowed. */
  CXX_BITFIELD_MEMBER(31, 31, overvoltage, raw);
  /** OTP programming allowed. */
  CXX_BITFIELD_MEMBER(30, 30, otp_program, raw);
  /** OTP reading allowed. */
  CXX_BITFIELD_MEMBER(29, 29, otp_read, raw);
  /** Warranty has been voided by overclocking. */
  CXX_BITFIELD_MEMBER(25, 25, warranty, raw);
  /** New-style revision. */
  CXX_BITFIELD_MEMBER(23, 23, new_style, raw);
  /** Memory size. 0=256MB, 1=512MB, 2=1GB, 3=2GB, 4=4GB, 8=8GB. */
  CXX_BITFIELD_MEMBER(20, 22, memory_size, raw);
  /** Manufacturer: 0=Sony UK, 1=Egoman, 2=Embest, 3=Sony Japan, ... */
  CXX_BITFIELD_MEMBER(16, 19, manufacturer, raw);
  /** Processor: 0=BCM2835, 1=BCM2836, 2=BCM2837, 3=BCM2711, 4=BCM2712 */
  CXX_BITFIELD_MEMBER(12, 15, processor, raw);
  /** Type. */
  CXX_BITFIELD_MEMBER(4, 11, type, raw);
  /** Revision. */
  CXX_BITFIELD_MEMBER(0, 3, revision, raw);
};
