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
