/*
 * Copyright (C) 2020-2021 Kernkonzept GmbH.
 * Author(s): Frank Mehnert <frank.mehnert@kernkonzept.com>
 *
 * License: see LICENSE.spdx (in this directory or the directories above)
 */

/**
 * \file backend for SDHCI used by i.MX8.
 */

#include "cmd.h"
#include "debug.h"
#include "drv_sdhci.h"
#include "mmc.h"
#include "util.h"
#include "l4/sys/cache.h"

namespace Emmc {

Sdhci::Sdhci(int nr,
             L4::Cap<L4Re::Dataspace> iocap,
             L4::Cap<L4Re::Mmio_space> mmio_space,
             l4_addr_t mmio_base, Type type,
             L4Re::Util::Shared_cap<L4Re::Dma_space> const &dma,
             l4_uint32_t host_clock, Receive_irq receive_irq)
: Drv(iocap, mmio_space, mmio_base, receive_irq),
  _adma2_desc_mem("sdhci_adma_buf", 4096, dma,
                  L4Re::Dma_space::Direction::From_device,
                  L4Re::Rm::F::Cache_uncached),
  _adma2_desc_phys(_adma2_desc_mem.pget()),
  _adma2_desc(_adma2_desc_mem.get<Adma2_desc_64>()),
  _type(type),
  _host_clock(host_clock),
  warn(Dbg::Warn, "sdhci", nr),
  info(Dbg::Info, "sdhci", nr),
  trace(Dbg::Trace, "sdhci", nr),
  trace2(Dbg::Trace2, "sdhci", nr)
{
  trace.printf("Assuming %s eMMC controller.\n", type_name(type));

  Reg_cap1_sdhci cap1(this);
  if (_type == Type::Iproc)
    {
      // Fine-grained clock required for Reg_write_delay::write_delayed().
      if (!Util::tsc_available())
        L4Re::throw_error(-L4_EINVAL, "Iproc requires fine-grained clock");

      _write_delay = 10; // 2.5 SD clock write cycles @ 400 KHz
      if (cap1.base_freq() > 0) // field limits frequency to 255 MHz
        {
          Reg_sys_ctrl sc(this);

          _host_clock = 1'000'000 * cap1.base_freq();
          l4_uint32_t sd_clock = _host_clock / sc.clock_base_divider10();
          _write_delay = (4'000'000 + sd_clock - 1) / sd_clock;
          warn.printf("\033[33mActually using host clock of %s.\033[m\n",
              Util::readable_freq(host_clock).c_str());
        }
    }

  info.printf("SDHCI controller capabilities: %08x (%d-bit). SDHCI version %s.\n",
              cap1.raw, cap1.bit64_v3() ? 64 : 32,
              Reg_host_version(this).spec_version());

  if (cap1.bit64_v3())
    _adma2_64 = true;
}

void
Sdhci::init()
{
  Reg_sys_ctrl sc(this);
  sc.dtocv() = Reg_sys_ctrl::Data_timeout::Sdclk_max;
  sc.write(this);
  sc.read(this);

  Reg_vend_spec2 vs2(this);
  vs2.acmd23_argu2_en() = 1;
  vs2.write(this);

  sc.rsta() = 1;
  if (_type == Type::Usdhc)
    sc.raw |= 0xf;
  sc.write(this);

  Util::poll(10000, [this] { return !Reg_sys_ctrl(this).rsta(); },
             "Software reset all");

  if (_type == Type::Usdhc)
    {
      Reg_host_ctrl_cap cc(this);
      trace.printf("Host controller capabilities (%08x): sdr50=%d, sdr104=%d, ddr50=%d\n",
                   cc.raw,
                   (unsigned)cc.sdr50_support(), (unsigned)cc.sdr104_support(),
                   (unsigned)cc.ddr50_support());

      Reg_mmc_boot().write(this);
      Reg_mix_ctrl().write(this);
      Reg_autocmd12_err_status().write(this);
      Reg_clk_tune_ctrl_status().write(this);
      Reg_dll_ctrl().write(this);
      if (Usdhc_std_tuning)
        {
          Reg_tuning_ctrl tc(this);
          tc.tuning_start_tap() = 0x14;
          tc.disable_crc_on_tuning() = 1;
          tc.write(this);
        }
      Reg_vend_spec vs;
      vs.ext_dma_en() = 1; // XXX required?
      vs.peren() = 1;
      vs.hcken() = 1;
      vs.ipgen() = 1;
      vs.cken()  = 1;
      vs.raw |= 0x30000000;
      vs.write(this);

      Reg_vend_spec2 vs2(this); // XXX required?
      vs2.en_busy_irq() = 1;
      vs2.write(this);

      Reg_prot_ctrl pc(this);
      pc.dmasel() = dma_adma2() ? pc.Dma_adma2 : pc.Dma_simple;
      pc.write(this);

      Reg_tuning_ctrl tc(this);
      if (Usdhc_std_tuning)
        {
          tc.std_tuning_en() = 1;
          tc.tuning_start_tap() = 20; // XXX Linux device tree: "tuning-start-tap"
          tc.tuning_step() = 2;       // XXX Linux device tree: "tuning-step"
          tc.disable_crc_on_tuning() = 1;
        }
      else
        tc.std_tuning_en() = 0;
      tc.write(this);
    }
  else
    {
      if (_type == Type::Iproc)
        {
          // SD Host Controller Simplified Specification, Figure 3-3
          sc.raw = 0;
          sc.icen() = 1;
          sc.write(this);
          Util::poll(10'000, [this] { return !!Reg_sys_ctrl(this).icst(); },
                     "Clock stable");
          sc.sdcen() = 1;
          sc.pllen() = 1;
          sc.write(this);
          Util::poll(10'000, [this] { return !!Reg_sys_ctrl(this).icst(); },
                     "PLL clock stable");
        }
      else
        {
          sc.icen()  = 1;
          sc.icst()  = 1; // XXX internal clock stable
          sc.sdcen() = 1;
          sc.pllen() = 1;
          sc.write(this);
        }
      Reg_autocmd12_err_status().write(this);
      Reg_clk_tune_ctrl_status().write(this);

      Reg_host_ctrl hc(this);
      if (_type == Type::Iproc)
        {
          hc.voltage_sel() = Reg_host_ctrl::Voltage_33;
          hc.bus_power() = 1;
        }
      hc.dmamod() = dma_adma2() ? hc.Adma32 : hc.Sdma;
      hc.write(this);
    }
}

Cmd *
Sdhci::handle_irq()
{
  Cmd *cmd = _cmd_queue.working();
  if (cmd)
    {
      Reg_int_status is(this);

      if (cmd->status == Cmd::Progress_cmd)
        handle_irq_cmd(cmd, is);

      if (cmd->status == Cmd::Progress_data)
        handle_irq_data(cmd, is);

      if (is.read(this))
        trace.printf("after handle_irq: is = \033[31m%08x\033[m\n", is.raw);

      if (cmd->status == Cmd::Success)
        cmd_fetch_response(cmd);
    }
  // else polling

  // for driver "bottom-half handling"
  return cmd;
}

void
Sdhci::handle_irq_cmd(Cmd *cmd, Reg_int_status is)
{
  Reg_int_status is_ack;
  if (trace.is_active()) // otherwise don't read Reg_int_status
    trace.printf("handle_irq_cmd: is = %08x, isen = %08x\n",
                 is.raw, Reg_int_status_en().read(this));
  if (is.ctoe())
    {
      is_ack.ctoe() = 1;
      is_ack.cc() = is.cc();
      if (_type == Type::Usdhc)
        {
          Reg_pres_state ps(this);
          if (ps.cihb())
            {
              ps.cihb() = 0;
              ps.write(this);
            }
        }
      cmd->status = Cmd::Cmd_timeout;
    }
  else if (is.cmd_error())
    {
      is_ack.copy_cmd_error(is);
      cmd->status = Cmd::Cmd_error;
    }
  else if (is.ac12e())
    {
      Reg_autocmd12_err_status ec(this);
      trace.printf("AC12 status = %08x\n", ec.raw);
      cmd->status = Cmd::Cmd_error;
    }
  else if (   cmd->cmd == Mmc::Cmd19_send_tuning_block
           || cmd->cmd == Mmc::Cmd21_send_tuning_block)
    {
      if (is.cc())
        {
          is_ack.cc() = 1;
          is_ack.write(this);
        }
      if (is.brr())
        {
          is_ack.brr() = 1;
          Reg_autocmd12_err_status es(this);
          if (es.execute_tuning())
            cmd->status = Cmd::Tuning_progress;
          else if (es.smp_clk_sel())
            cmd->status = Cmd::Success;
          else
            cmd->status = Cmd::Tuning_failed;
        }
    }
  else if (is.cc())
    {
      is_ack.cc() = 1;
      cmd->status = cmd->flags.has_data() ? Cmd::Progress_data : Cmd::Success;
    }

  if (is_ack.raw)
    is_ack.write(this);

  if (cmd->error())
    {
      Reg_sys_ctrl sc(this);
      sc.rstc() = 1;
      sc.write(this);
      Util::poll(10000, [this] { return !Reg_sys_ctrl(this).rstc(); },
                 "Software reset for CMD line");
    }
}

void
Sdhci::handle_irq_data(Cmd *cmd, Reg_int_status is)
{
  Reg_int_status is_ack;
  if (trace.is_active()) // otherwise don't read Reg_int_status
    trace.printf("handle_irq_data: is = %08x, isen = %08x\n",
                 is.raw, Reg_int_status_en().read(this));
  if (is.data_error())
    {
      is_ack.copy_data_error(is);
      cmd->status = Cmd::Data_error;
    }
  else if (is.tc())
    {
      is_ack.tc() = 1;
      is_ack.dint() = is.dint();
      cmd->status = Cmd::Success;
    }
  else if (is.dint())
    {
      is_ack.dint() = 1;
      l4_uint32_t blks_to_xfer = Reg_blk_att(this).blkcnt();
      if (blks_to_xfer)
        {
          if (dma_adma2())
            L4Re::throw_error(-L4_EINVAL,
                              "Implement aborted transfer in ADMA2 mode");
          is_ack.write(this);
          l4_uint32_t blks_xferred = cmd->blockcnt - blks_to_xfer;
          l4_uint32_t data_xferred = blks_xferred * cmd->blocksize;
          cmd->blockcnt  -= blks_xferred;
          cmd->data_phys += data_xferred;
          if (_type == Type::Usdhc)
            for (;;)
              if (!Reg_pres_state(this).dla())
                break;
          Reg_ds_addr(cmd->data_phys).write(this);
          is_ack.raw = 0;
        }
    }

  if (is_ack.raw)
    is_ack.write(this);
}

/**
 * Wait for the bus being idle before submitting another MMC command to the
 * controller.
 */
void
Sdhci::cmd_wait_available(Cmd const *cmd, bool sleep)
{
  bool need_data = cmd->flags.has_data() || (cmd->cmd & Mmc::Rsp_check_busy);
  if (   cmd->cmd == Mmc::Cmd12_stop_transmission_rd
      || cmd->cmd == Mmc::Cmd12_stop_transmission_wr)
    need_data = false;
  l4_uint64_t time = Util::read_tsc();
  for (;;)
    {
      Reg_pres_state ps(this);
      if (!ps.cihb() && (!need_data || !ps.cdihb()))
        break;
      trace.printf("cmd_wait_available: ps = %08x, is = %08x\n",
                   ps.raw, Reg_int_status(this).raw);
      if (sleep)
        l4_ipc_sleep_ms(1);
    }
  time = Util::read_tsc() - time;
  _time_sleep += time;
  l4_uint64_t us = Util::tsc_to_us(time);
  if (us >= 10)
    trace.printf("cmd_wait_available took \033[1m%lluus.\033[m\n", us);
}

/**
 * Send an MMC command to the controller.
 */
void
Sdhci::cmd_submit(Cmd *cmd)
{
  if (cmd->status != Cmd::Ready_for_submit)
    L4Re::throw_error(-L4_EINVAL, "Invalid command submit status");

  Reg_cmd_xfr_typ xt;  // SDHCI + uSDHC
  Reg_mix_ctrl mc;     // uSDHC

  if (_type == Type::Usdhc)
    mc.read(this);

  xt.cmdinx() = cmd->cmd_idx();
  xt.cccen() = !!(cmd->cmd & Mmc::Rsp_check_crc);
  xt.cicen() = !!(cmd->cmd & Mmc::Rsp_has_opcode);
  if (cmd->cmd & Mmc::Rsp_136_bits)
    xt.rsptyp() = Reg_cmd_xfr_typ::Resp::Length_136;
  else if (cmd->cmd & Mmc::Rsp_check_busy)
    xt.rsptyp() = Reg_cmd_xfr_typ::Resp::Length_48_check_busy;
  else if (cmd->cmd & Mmc::Rsp_present)
    xt.rsptyp() = Reg_cmd_xfr_typ::Resp::Length_48;
  else
    xt.rsptyp() = Reg_cmd_xfr_typ::Resp::No;
  if (   cmd->cmd == Mmc::Cmd12_stop_transmission_rd
      || cmd->cmd == Mmc::Cmd12_stop_transmission_wr)
    xt.cmdtyp() = Reg_cmd_xfr_typ::Cmd52_abort;

  L4Re::Dma_space::Dma_addr dma_addr = ~0ULL;

  if (cmd->flags.has_data())
    {
      switch (_type)
        {
        case Type::Usdhc:
          {
            Reg_wtmk_lvl wml(this);
            wml.rd_wml() = Reg_wtmk_lvl::Wml_dma;
            wml.wr_wml() = Reg_wtmk_lvl::Wml_dma;
            wml.rd_brst_len() = Reg_wtmk_lvl::Brst_dma;
            wml.wr_brst_len() = Reg_wtmk_lvl::Brst_dma;
            wml.write(this);
            mc.ac12en() = Auto_cmd12 && cmd->flags.inout_cmd12();
            break;
          }
        default:
          xt.ac12en() = Auto_cmd12 && cmd->flags.inout_cmd12();
          break;
        }

      if (dma_adma2())
        {
          // `cmd` refers to a list of blocks (cmd->blocks != nullptr).
          if (cmd->blocks)
            adma2_set_descs_blocks(cmd);
          else
            adma2_set_descs_memory_region(cmd->data_phys, cmd->blocksize);
          dma_addr = _adma2_desc_phys;
        }
      else
        {
          // `cmd` refers either to a single block (cmd->blocks != nullptr) or
          // to a region (cmd->data_phys / cmd->blocksize set).
          l4_size_t blk_size = cmd->blocksize * cmd->blockcnt;
          if (cmd->blocks) // this implies cmd->inout() == true
            {
              if (provided_bounce_buffer()
                  && region_requires_bounce_buffer(cmd->blocks->dma_addr, blk_size))
                {
                  if (cmd->flags.inout_read())
                    {
                      l4_cache_inv_data(_bb_virt, _bb_virt + blk_size);
                      cmd->flags.read_from_bounce_buffer() = 1;
                    }
                  else
                    {
                      memcpy((void *)_bb_virt, cmd->blocks->virt_addr, blk_size);
                      l4_cache_flush_data(_bb_virt, _bb_virt + blk_size);
                    }
                  dma_addr = cmd->data_phys = _bb_phys;
                }
              else
                dma_addr = cmd->data_phys = cmd->blocks->dma_addr;
            }
          else
            dma_addr = cmd->data_phys;
          trace2.printf("SDMA: addr=%08llx size=%08zx\n", dma_addr, blk_size);
        }

      Reg_blk_att ba;
      ba.blkcnt() = cmd->blockcnt;
      if (ba.blkcnt() != cmd->blockcnt)
        L4Re::throw_error(-L4_EINVAL, "Number of data blocks to transfer");
      ba.blksize() = cmd->blocksize;
      if (ba.blksize() != cmd->blocksize)
        L4Re::throw_error(-L4_EINVAL, "Size of data blocks to transfer");
      ba.write(this);

      // XXX Timeout ...

      xt.dpsel() = 1;
      if (_type == Type::Usdhc)
        mc.dmaen() = 1;
      else
        xt.dmaen() = 1;

      if (_type == Type::Usdhc)
        {
          mc.bcen() = !!(cmd->blockcnt > 1);
          mc.msbsel() = !!(cmd->blockcnt > 1);
        }
      else
        {
          xt.bcen() = !!(cmd->blockcnt > 1);
          xt.msbsel() = !!(cmd->blockcnt > 1);
        }

      if (_type == Type::Usdhc)
        mc.dtdsel() = !!(cmd->cmd & Mmc::Dir_read);
      else
        xt.dtdsel() = !!(cmd->cmd & Mmc::Dir_read);
    }
  else // no data
    {
      if (_type == Type::Usdhc)
        {
          mc.ac12en() = 0;
          mc.ac23en() = 0;
        }
      else
        {
          xt.ac12en() = 0;
          xt.ac23en() = 0;
        }
    }

  if (   cmd->cmd == Mmc::Cmd19_send_tuning_block
      || cmd->cmd == Mmc::Cmd21_send_tuning_block)
    {
      l4_uint8_t blksize = cmd->cmd == Mmc::Cmd19_send_tuning_block ? 64 : 128;
      if (_type == Type::Iproc)
        {
          Reg_blk_size bz;
          bz.blksize() = blksize;
          bz.blkcnt() = 0; // ???
          bz.sdma_buf_bndry() = 7;
          bz.write(this);
        }
      else
        {
          Reg_blk_att ba;
          ba.blkcnt() = 1;
          ba.blksize() = blksize;
          ba.write(this);
        }

      Reg_wtmk_lvl wml(this);
      wml.rd_wml() = blksize;
      wml.wr_wml() = blksize;
      wml.rd_brst_len() = Reg_wtmk_lvl::Brst_dma;
      wml.wr_brst_len() = Reg_wtmk_lvl::Brst_dma;
      wml.write(this);

      switch (_type)
        {
        case Type::Usdhc:
          {
            mc.dmaen() = 0;
            mc.bcen() = 0;
            mc.ac12en() = 0;
            mc.dtdsel() = 1;
            mc.msbsel() = 0;
            mc.ac23en() = 0;
            mc.auto_tune_en() = 1;
            mc.fbclk_sel() = 1;

            Reg_autocmd12_err_status es(this);
            es.smp_clk_sel() = 0;
            es.execute_tuning() = 1;
            es.write(this);
            break;
          }
        case Type::Iproc:
          {
            Reg_autocmd12_err_status es(this);
            es.smp_clk_sel() = 0;
            es.execute_tuning() = 1;
            es.write(this);
            xt.dtdsel() = 1;
            break;
          }
        default:
          xt.ac12en() = 0;
          xt.dtdsel() = 1;
          break;
        }
      xt.dpsel() = 1;
    }

  if (dma_addr != ~0ULL)
    {
      if (dma_adma2())
        {
          switch (_type)
            {
            case Type::Usdhc:
              if (cmd->flags.auto_cmd23())
                {
                  assert(auto_cmd23());
                  mc.ac23en() = 1;
                  while (Reg_pres_state(this).dla())
                    ;
                  Reg_cmd_arg2(cmd->blockcnt).write(this);
                }
              else
                mc.ac23en() = 0;
              break;
            case Type::Iproc:
              if (cmd->flags.auto_cmd23())
                {
                  assert(auto_cmd23());
                  xt.ac23en() = 1;
                  Reg_cmd_arg2(cmd->blockcnt).write(this);
                }
              else
                xt.ac23en() = 0;
              break;
            default:
              break; // This cannot happen, see auto_cmd23()
            }
          Reg_adma_sys_addr_lo(dma_addr & 0xffffffff).write(this);
          Reg_adma_sys_addr_hi(dma_addr >> 32).write(this);
        }
      else
        {
          if (_type == Type::Usdhc)
            for (;;)
              if (!Reg_pres_state(this).dla())
                break;
          Reg_ds_addr(dma_addr).write(this);
        }
    }

  Reg_cmd_arg(cmd->arg).write(this);

  // clear all IRQs
  Reg_int_status(~0U).write(this);
  // enable IRQ status
  Reg_int_status_en se;
  se.enable_ints(cmd);
  se.write(this);
  // unmask IRQs
  Reg_int_signal_en ie;
  ie.enable_ints(cmd);
  ie.write(this);

  // send the command
  if (cmd->cmd == Mmc::Cmd6_switch)
    trace.printf("Send \033[33mCMD%d / %d (arg=%08x) -- %s\033[m\n",
                 cmd->cmd_idx(), (cmd->arg >> 16) & 0xff, cmd->arg,
                 cmd->cmd_to_str().c_str());
  else
    trace.printf("Send \033[32mCMD%d (arg=%08x) -- %s\033[m\n",
                 cmd->cmd_idx(), cmd->arg, cmd->cmd_to_str().c_str());

  if (_type == Type::Usdhc)
    mc.write(this);

  xt.write(this);

  cmd->status = Cmd::Progress_cmd;
}

/**
 * Wait for completion of command send phase.
 */
void
Sdhci::cmd_wait_cmd_finished(Cmd *cmd, bool verbose)
{
  l4_uint64_t time = Util::read_tsc();
  while (cmd->status == Cmd::Progress_cmd)
    {
      _receive_irq(false);
      handle_irq_cmd(cmd, Reg_int_status(this));
    }
  time = Util::read_tsc() - time;
  _time_sleep += time;
  l4_uint64_t us = Util::tsc_to_us(time);
  if ((verbose && us >= 1000) || cmd->error())
    {
      char const *s = cmd->error()
                        ? cmd->flags.expected_error()
                          ? " (failed, expected)"
                          : " \033[31m(failed)\033[m"
                        : "";
      info.printf("CMD%d took \033[1m%lluus%s.\033[m\n", cmd->cmd_idx(), us, s);
    }
}

/**
 * Wait for command completion.
 */
void
Sdhci::cmd_wait_data_finished(Cmd *cmd)
{
  l4_uint64_t time = Util::read_tsc();
  while (cmd->status == Cmd::Progress_data)
    {
      _receive_irq(true);
      handle_irq_data(cmd, Reg_int_status(this));
    }
  time = Util::read_tsc() - time;
  _time_sleep += time;
  l4_uint64_t us = Util::tsc_to_us(time);
  if (us >= 1000)
    warn.printf("CMD%d data took \033[1m%lluus.\033[m\n", cmd->cmd_idx(), us);
}

/**
 * Fetch response after a command was successfully executed.
 */
void
Sdhci::cmd_fetch_response(Cmd *cmd)
{
  if (cmd->cmd & Mmc::Rsp_136_bits)
    {
      Reg_cmd_rsp0 rsp0(this);
      Reg_cmd_rsp1 rsp1(this);
      Reg_cmd_rsp2 rsp2(this);
      Reg_cmd_rsp3 rsp3(this);
      cmd->resp[0] = (rsp3.raw << 8) | (rsp2.raw >> 24);
      cmd->resp[1] = (rsp2.raw << 8) | (rsp1.raw >> 24);
      cmd->resp[2] = (rsp1.raw << 8) | (rsp0.raw >> 24);
      cmd->resp[3] = (rsp0.raw << 8);
    }
  else
    {
      cmd->resp[0] = Reg_cmd_rsp0(this).raw;
      cmd->flags.has_r1_response() = 1;
      Mmc::Device_status s = cmd->mmc_status();
      if (s.current_state() != s.Transfer)
        trace.printf("\033[35mCommand response R1 (%s)\033[m\n", s.str());
    }

  if (cmd->flags.read_from_bounce_buffer()
      && (   cmd->cmd == Mmc::Cmd17_read_single_block
          || cmd->cmd == Mmc::Cmd18_read_multiple_block))
    {
      l4_uint32_t offset = 0;
      for (auto const *b = cmd->blocks; b; b = b->next.get())
        {
          l4_uint32_t b_size = b->num_sectors << 9;
          if (region_requires_bounce_buffer(b->dma_addr, b_size))
            {
              l4_cache_inv_data(_bb_virt + offset, _bb_virt + offset + b_size);
              memcpy(b->virt_addr, (void *)(_bb_virt + offset), b_size);
              offset += b_size;
            }
        }
    }
}

/**
 * Disable all interrupt sources.
 */
void
Sdhci::mask_interrupts()
{
  Reg_int_signal_en se;
  se.write(this);
}

void
Sdhci::show_interrupt_status(char const *s) const
{
  Reg_int_status is(this);
  trace.printf("\033[35%sm%s%08x\033[m\n",
               is.raw ? "" : ";1", s, is.raw);
}

void
Sdhci::set_strobe_dll()
{
  Reg_strobe_dll_ctrl dc;
  dc.strobe_dll_ctrl_reset() = 1;
  dc.write(this);

  dc.raw = 0;
  dc.strobe_dll_ctrl_enable() = 1;
  dc.strobe_dll_ctrl_slv_update_int() = 4;
  dc.strobe_dll_ctrl_slv_dly_target() = 7;
  dc.write(this);

  Util::poll(10000, [this]
               {
                 Reg_strobe_dll_status s(this);
                 return     s.strobe_dll_sts_slv_lock()
                         && s.strobe_dll_sts_ref_lock();
               },
             "REV/SLV");
}

void
Sdhci::set_clock_and_timing(l4_uint32_t freq, Mmc::Timing timing, bool strobe)
{
  clock_disable();
  if (!freq)
    {
      info.printf("\033[33mClock disabled.\033[m\n");
      return;
    }

  if (   timing == Mmc::Mmc_hs400
      || timing == Mmc::Uhs_ddr50
      || timing == Mmc::Mmc_ddr52)
    _ddr_active = true;
  else
    _ddr_active = false;
  if (_type == Type::Iproc)
    {
      Reg_host_ctrl hc(this);
      if (   timing == Mmc::Mmc_hs400
          || timing == Mmc::Mmc_hs200
          || timing == Mmc::Mmc_ddr52
          || timing == Mmc::Uhs_ddr50
          || timing == Mmc::Uhs_sdr104
          || timing == Mmc::Uhs_sdr50
          || timing == Mmc::Uhs_sdr25
          || timing == Mmc::Hs)
        hc.hispd() = 1;
      else
        hc.hispd() = 0;
      hc.write(this);

      Reg_host_ctrl2 hc2(this);
      if (timing == Mmc::Mmc_hs200 || timing == Mmc::Uhs_sdr104)
        hc2.uhsmode() = Reg_host_ctrl2::Ctrl_uhs_sdr104;
      else if (timing == Mmc::Uhs_sdr12)
        hc2.uhsmode() = Reg_host_ctrl2::Ctrl_uhs_sdr12;
      else if (timing == Mmc::Uhs_sdr25)
        hc2.uhsmode() = Reg_host_ctrl2::Ctrl_uhs_sdr25;
      else if (timing == Mmc::Uhs_sdr50)
        hc2.uhsmode() = Reg_host_ctrl2::Ctrl_uhs_sdr50;
      else if (timing == Mmc::Uhs_ddr50 || timing == Mmc::Mmc_ddr52)
        hc2.uhsmode() = Reg_host_ctrl2::Ctrl_uhs_ddr50;
      else if (timing == Mmc::Mmc_hs400)
        hc2.uhsmode() = Reg_host_ctrl2::Ctrl_hs400;
      else
        hc2.uhsmode() = 0;
      hc2.write(this);
    }
  set_clock(freq);
  if (_type == Type::Usdhc)
    {
      Reg_mix_ctrl mc(this);
      mc.ddr_en() = 0;
      mc.hs400_mo() = 0;
      mc.en_hs400_mo() = 0;

      Reg_strobe_dll_ctrl(0U).write(this);

      switch (timing)
        {
        case Mmc::Hs:
        case Mmc::Uhs_sdr12:
        case Mmc::Uhs_sdr25:
        case Mmc::Uhs_sdr50:
        case Mmc::Uhs_sdr104:
        case Mmc::Mmc_hs200:
          mc.write(this);
          break;
        case Mmc::Uhs_ddr50:
        case Mmc::Mmc_ddr52:
          mc.ddr_en() = 1;
          mc.write(this);
          break;
        case Mmc::Mmc_hs400:
          mc.ddr_en() = 1;
          mc.hs400_mo() = 1;
          mc.write(this);
          set_strobe_dll();
          if (strobe)
            mc.en_hs400_mo() = 1;
          mc.write(this);
          break;
        case Mmc::Legacy:
          reset_tuning();
          mc.write(this);
          break;
        default:
          L4Re::throw_error(-L4_EINVAL, "Invalid driver timing");
        }
    }
  clock_enable();
}

void
Sdhci::set_clock(l4_uint32_t freq)
{
  switch (_type)
    {
    case Type::Iproc:
      {
        Reg_sys_ctrl sc;
        sc.write(this);

        l4_uint32_t div;
        if (_host_clock <= freq)
          div = 1;
        else
          for (div = 2; div < 2046; div += 2)
            if ((_host_clock / div) <= freq)
              break;
        div >>= 1;

        sc.icen() = 1;
        sc.clk_freq8() = div & 0xff;
        sc.clk_freq_ms2() = (div >> 8) & 0x3;
        sc.write(this);

        _write_delay = (4'000'000 + freq - 1) / freq;

        // Minimum waiting time!
        delay(5);
        // Timeout: max 150ms (SD host A2 3.2.1)
        Util::poll(150'000, [this] { return !!Reg_sys_ctrl(this).icst(); },
                   "Clock stable");

        sc.read(this);
        sc.sdcen() = 1;
        sc.write(this);

        info.printf("\033[33mSet clock to %s%s (host=%s, divider=%d).\033[m\n",
                    Util::readable_freq(freq).c_str(), _ddr_active ? " (DDR)" : "",
                    Util::readable_freq(_host_clock).c_str(),
                    sc.clock_base_divider10());
        break;
      }
    default:
      {
        // this code is primarily for uSDHC
        Reg_sys_ctrl sc(this);
        sc.icen() = 0;
        sc.icst() = 0;
        sc.sdcen() = 0;
        sc.dvs() = 0;
        sc.sdclkfs() = 0;
        sc.write(this);

        l4_uint32_t ddr_pre_div = _ddr_active ? 2 : 1;
        l4_uint32_t pre_div = 1;
        l4_uint32_t div = 1;
        while (_host_clock / (16 * pre_div * ddr_pre_div) > freq && pre_div < 256)
          pre_div <<= 1;
        while (_host_clock / (div * pre_div * ddr_pre_div) > freq && div < 16)
          ++div;
        pre_div >>= 1;
        --div;

        sc.read(this);
        sc.icen() = 1;
        sc.icst() = 1;
        sc.sdcen() = 1;
        sc.dvs() = div;
        sc.sdclkfs() = pre_div;
        sc.write(this);

        info.printf("\033[33mSet clock to %s%s (host=%s, divider=%d).\033[m\n",
                    Util::readable_freq(freq).c_str(), _ddr_active ? " (DDR)" : "",
                    Util::readable_freq(_host_clock).c_str(),
                    _ddr_active ? sc.clock_divider_ddr() : sc.clock_divider_sdr());
        break;
      }
    }
}

void
Sdhci::set_bus_width(Mmc::Bus_width bus_width)
{
  switch (_type)
    {
    case Type::Usdhc:
      {
        Reg_prot_ctrl pc(this);
        pc.set_bus_width(bus_width);
        pc.write(this);
        info.printf("\033[33mSet bus width to %s.\033[m\n", pc.str_bus_width());
        break;
      }
    default:
      {
        Reg_host_ctrl hc(this);
        hc.set_bus_width(bus_width);
        hc.write(this);
        info.printf("\033[33mSet bus width to %s.\033[m\n", hc.str_bus_width());
        break;
      }
    }
}

void
Sdhci::set_voltage(Mmc::Voltage voltage)
{
  if (voltage != Mmc::Voltage_330 && voltage != Mmc::Voltage_180)
    {
      warn.printf("\033[31mInvalid voltage %s!\033[m",
                  Mmc::str_voltage(voltage));
      return;
    }

  switch (_type)
    {
    case Type::Usdhc:
      {
        Reg_vend_spec vs(this);
        if (voltage == Mmc::Voltage_330)
          vs.vselect() = 0;
        else
          vs.vselect() = 1;
        vs.write(this);
        break;
      }
    case Type::Iproc:
      {
        Reg_host_ctrl2 hc2(this);
        hc2.v18() = 1;
        hc2.write(this);
        break;
      }
    default:
      break; // 0x3E: SDHCI: Host Control 2 Register bit 3
    }

  info.printf("\033[33mSet voltage to %s.\033[m\n", Mmc::str_voltage(voltage));
}

void
Sdhci::clock_disable()
{
  if (_type == Type::Usdhc)
    {
      // uSDHC: 10.3.6.7
      Reg_vend_spec vs(this);
      vs.frc_sdclk_on() = 0;
      vs.write(this);

      Util::poll(10'000, [this] { return !!Reg_pres_state(this).sdoff(); },
                 "Clock gate off");
    }
}

void
Sdhci::clock_enable()
{
  if (_type == Type::Usdhc)
    {
      Reg_vend_spec vs(this);
      vs.frc_sdclk_on() = 1;
      vs.write(this);

      Util::poll(10'000, [this] { return !!Reg_pres_state(this).sdstb(); },
                 "Clock stable after enable");
    }
}

void
Sdhci::reset_tuning()
{
  if (_type == Type::Usdhc)
    {
      if (Usdhc_std_tuning)
        {
          Reg_autocmd12_err_status a12s(this);
          a12s.execute_tuning() = 0;
          a12s.smp_clk_sel() = 0;
          a12s.write(this);
        }
    }
}

bool
Sdhci::tuning_finished(bool *success)
{
  Reg_autocmd12_err_status es(this);
  if (es.execute_tuning())
    return false;

  *success = es.smp_clk_sel();
  return true;
}

void
Sdhci::dump() const
{
  warn.printf("Registers:\n");
  for (unsigned i = 0; i < 0x100; i += 4)
    warn.printf("  %04x: %08x\n", i, unsigned{_regs[i]});
}

/**
 * Set up one or more ADMA2 descriptors for a single memory block (either
 * client memory or bounce buffer).
 *
 * \param desc       First descriptor to be written.
 * \param phys       Physical address of memory region for DMA.
 * \param size       Size of the memory region for DMA.
 * \param terminate  True for writing the final descriptor.
 * \return Pointer to the next descriptor.
 *
 * \note The descriptor memory is mapped uncached so cache flush not required!
 */
template<typename T>
T*
Sdhci::adma2_set_descs_mem_region(T *desc, l4_uint64_t phys, l4_uint32_t size,
                                  bool terminate)
{
  for (; size; ++desc)
    {
      trace2.printf("  addr=%08llx size=%08x\n", phys, size);
      if (desc > _adma2_desc + _adma2_desc_mem.size() / sizeof(T) - 1)
        L4Re::throw_error(-L4_EINVAL, "Too many ADMA2 descriptors");
      if (phys >= T::get_max_addr())
        L4Re::throw_error(-L4_EINVAL, "Implement 64-bit ADMA2 mode");
      desc->reset();
      desc->valid() = 1;
      desc->act() = Adma2_desc_32::Act_tran;
      // XXX SD spec also defines 26-bit data length mode
      l4_uint32_t desc_length = cxx::min(size, 32768U);
      desc->length() = desc_length;
      desc->set_addr(phys);
      phys += desc_length;
      size -= desc_length;
      if (!size && terminate)
        desc->end() = 1;
    }

  return desc;
}

/**
 * Set up ADMA2 descriptor table using the memory provided in the In/out blocks
 * as DMA memory.
 *
 * Test for each block if the bounce buffer is required.
 */
template<typename T>
void
Sdhci::adma2_set_descs(T *descs, Cmd *cmd)
{
  trace2.printf("adma2_set_descs @ %08lx:\n", (l4_addr_t)descs);

  l4_uint32_t bb_offs = 0;
  auto *d = descs;

  for (auto const *b = cmd->blocks; b; b = b->next.get())
    {
      l4_uint64_t b_addr = b->dma_addr;
      l4_uint32_t b_size = b->num_sectors << 9;
      if (provided_bounce_buffer()
          && region_requires_bounce_buffer(b_addr, b_size))
        {
          if (bb_offs + b_size > _bb_size)
            L4Re::throw_error(-L4_EINVAL, "Bounce buffer too small");
          if (!cmd->flags.inout_read())
            {
              memcpy((void *)(_bb_virt + bb_offs), b->virt_addr, b_size);
              l4_cache_flush_data(_bb_virt + bb_offs, _bb_virt + bb_offs + b_size);
            }
          b_addr = _bb_phys + bb_offs;
          bb_offs += b_size;
        }

      d = adma2_set_descs_mem_region(d, b_addr, b_size, !b->next.get());
    }

  if (bb_offs > 0)                      // bounce buffer used
    if (cmd->flags.inout_read())        // read command
      cmd->flags.read_from_bounce_buffer() = 1;
}

/**
 * Set up an ADMA2 descriptor table for `inout_data()` requests.
 *
 * Each descriptor occupies 8 bytes (with 32-bit addresses) so we are able to
 * handle up to 512 blocks (using a 4K descriptor page).
 */
void
Sdhci::adma2_set_descs_blocks(Cmd *cmd)
{
  if (_adma2_64)
    adma2_set_descs<Adma2_desc_64>(_adma2_desc, cmd);
  else
    adma2_set_descs<Adma2_desc_32>(_adma2_desc, cmd);
}

/**
 * Set up an ADMA2 descriptor table for internal commands (for example CMD8).
 */
void
Sdhci::adma2_set_descs_memory_region(l4_addr_t phys, l4_uint32_t size)
{
  if (_adma2_64)
    adma2_set_descs_mem_region<Adma2_desc_64>(_adma2_desc, phys, size);
  else
    adma2_set_descs_mem_region<Adma2_desc_32>(_adma2_desc, phys, size);
}

template<typename T>
void
Sdhci::adma2_dump_descs(T const *desc) const
{
  for (;; ++desc)
    {
      trace.printf(" %u: %08x:%08x: addr=%08llx, size=%08x, valid=%d, end=%d\n",
                   (unsigned)(desc - (T*)_adma2_desc), desc->word1, desc->word0,
                   desc->get_addr(), (l4_uint32_t)desc->length(),
                   (unsigned)desc->valid(), (unsigned)desc->end());
      if (desc->end())
        break;
    }
}

void
Sdhci::adma2_dump_descs() const
{
  trace.printf("ADMA descriptors (%d-bit) at phys=%08llx / virt=%08lx\n",
               _adma2_64 ? 64 : 32, _adma2_desc_phys, (l4_addr_t)_adma2_desc);
  if (_adma2_64)
    adma2_dump_descs<Adma2_desc_64>(_adma2_desc);
  else
    adma2_dump_descs<Adma2_desc_32>(_adma2_desc);
}

void
Sdhci::sdio_reset(Cmd *cmd)
{
  if (_type == Type::Iproc)
    {
      const int SDIO_CCCR_ABORT = 0x6; // I/O card reset
      Mmc::Arg_cmd52_io_rw_direct a52;
      a52.address() = SDIO_CCCR_ABORT;
      a52.function() = 0;
      a52.write() = 0;
      cmd->init_arg(Mmc::Cmd52_io_rw_direct, a52.raw);
      cmd->flags.expected_error() = true;
      cmd_exec(cmd);
      if (!cmd->error())
        L4Re::throw_error(-L4_EIO, "IO_RW_DIRECT (read) succeeded");

      a52.raw = 0;
      a52.write_data() = 0x8;
      a52.address() = SDIO_CCCR_ABORT;
      a52.function() = 0;
      a52.write() = 1;

      cmd->init_arg(Mmc::Cmd52_io_rw_direct, a52.raw);
      cmd->flags.expected_error() = true;
      cmd_exec(cmd);
    }
}

void
Sdhci::Reg_write_delay::write_delayed(Sdhci *sdhci, Regs offs, l4_uint32_t val)
{
  sdhci->write_delay();
  sdhci->_regs[offs] = val;
  sdhci->update_last_write();
}

} // namespace Emmc
