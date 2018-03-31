/////////////////////////////////////////////////////////////////////////
// $Id: usb_ehci.cc 12922 2016-05-16 17:25:29Z vruppert $
/////////////////////////////////////////////////////////////////////////
//
//  Experimental USB EHCI adapter (partly ported from Qemu)
//
//  Copyright (C) 2015  The Bochs Project
//
//  Copyright(c) 2008  Emutex Ltd. (address@hidden)
//  Copyright(c) 2011-2012 Red Hat, Inc.
//
//  Red Hat Authors:
//  Gerd Hoffmann <kraxel@redhat.com>
//  Hans de Goede <hdegoede@redhat.com>
//
//  EHCI project was started by Mark Burkley, with contributions by
//  Niels de Vos.  David S. Ahern continued working on it.  Kevin Wolf,
//  Jan Kiszka and Vincent Palatin contributed bugfixes.
//
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
/////////////////////////////////////////////////////////////////////////

// Define BX_PLUGGABLE in files that can be compiled into plugins.  For
// platforms that require a special tag on exported symbols, BX_PLUGGABLE
// is used to know when we are exporting symbols and when we are importing.
#define BX_PLUGGABLE

#include "iodev.h"

#if BX_SUPPORT_PCI && BX_SUPPORT_USB_EHCI

#include "pci.h"
#include "usb_common.h"
#include "uhci_core.h"
#include "qemu-queue.h"
#include "usb_ehci.h"

#define LOG_THIS theUSB_EHCI->

bx_usb_ehci_c* theUSB_EHCI = NULL;

#define USB_RET_PROCERR   (-99)

#define IO_SPACE_SIZE   256

#define OPS_REGS_OFFSET 0x20

#define USBSTS_INT       (1 << 0)      // USB Interrupt
#define USBSTS_ERRINT    (1 << 1)      // Error Interrupt
#define USBSTS_PCD       (1 << 2)      // Port Change Detect
#define USBSTS_FLR       (1 << 3)      // Frame List Rollover
#define USBSTS_HSE       (1 << 4)      // Host System Error
#define USBSTS_IAA       (1 << 5)      // Interrupt on Async Advance

#define USBINTR_MASK     0x0000003f

#define FRAME_TIMER_FREQ 1000
#define FRAME_TIMER_USEC (1000000 / FRAME_TIMER_FREQ)

#define MAX_QH           100      // Max allowable queue heads in a chain
#define MIN_FR_PER_TICK  3        // Min frames to process when catching up

/*  Internal periodic / asynchronous schedule state machine states
 */
typedef enum {
    EST_INACTIVE = 1000,
    EST_ACTIVE,
    EST_EXECUTING,
    EST_SLEEPING,
    /*  The following states are internal to the state machine function
    */
    EST_WAITLISTHEAD,
    EST_FETCHENTRY,
    EST_FETCHQH,
    EST_FETCHITD,
    EST_FETCHSITD,
    EST_ADVANCEQUEUE,
    EST_FETCHQTD,
    EST_EXECUTE,
    EST_WRITEBACK,
    EST_HORIZONTALQH
} EHCI_STATES;

/* macros for accessing fields within next link pointer entry */
#define NLPTR_GET(x)             ((x) & 0xffffffe0)
#define NLPTR_TYPE_GET(x)        (((x) >> 1) & 3)
#define NLPTR_TBIT(x)            ((x) & 1)  // 1=invalid, 0=valid

/* link pointer types */
#define NLPTR_TYPE_ITD           0     // isoc xfer descriptor
#define NLPTR_TYPE_QH            1     // queue head
#define NLPTR_TYPE_STITD         2     // split xaction, isoc xfer descriptor
#define NLPTR_TYPE_FSTN          3     // frame span traversal node

/* nifty macros from Arnon's EHCI version  */
#define get_field(data, field) \
    (((data) & field##_MASK) >> field##_SH)

#define set_field(data, newval, field) do { \
    Bit32u val = *data; \
    val &= ~ field##_MASK; \
    val |= ((newval) << field##_SH) & field##_MASK; \
    *data = val; \
    } while(0)

// builtin configuration handling functions

Bit32s usb_ehci_options_parser(const char *context, int num_params, char *params[])
{
  if (!strcmp(params[0], "usb_ehci")) {
    bx_list_c *base = (bx_list_c*) SIM->get_param(BXPN_USB_EHCI);
    for (int i = 1; i < num_params; i++) {
      if (!strncmp(params[i], "enabled=", 8)) {
        SIM->get_param_bool(BXPN_EHCI_ENABLED)->set(atol(&params[i][8]));
      } else if (!strncmp(params[i], "port", 4)) {
        if (SIM->parse_usb_port_params(context, 0, params[i], USB_EHCI_PORTS, base) < 0) {
          return -1;
        }
      } else if (!strncmp(params[i], "options", 7)) {
        if (SIM->parse_usb_port_params(context, 1, params[i], USB_EHCI_PORTS, base) < 0) {
          return -1;
        }
      } else {
        BX_ERROR(("%s: unknown parameter '%s' for usb_ehci ignored.", context, params[i]));
      }
    }
  } else {
    BX_PANIC(("%s: unknown directive '%s'", context, params[0]));
  }
  return 0;
}

Bit32s usb_ehci_options_save(FILE *fp)
{
  bx_list_c *base = (bx_list_c*) SIM->get_param(BXPN_USB_EHCI);
  SIM->write_usb_options(fp, USB_EHCI_PORTS, base);
  return 0;
}

// device plugin entry points

int CDECL libusb_ehci_LTX_plugin_init(plugin_t *plugin, plugintype_t type, int argc, char *argv[])
{
  theUSB_EHCI = new bx_usb_ehci_c();
  BX_REGISTER_DEVICE_DEVMODEL(plugin, type, theUSB_EHCI, BX_PLUGIN_USB_EHCI);
  // add new configuration parameter for the config interface
  SIM->init_usb_options("EHCI", "ehci", USB_EHCI_PORTS);
  // register add-on option for bochsrc and command line
  SIM->register_addon_option("usb_ehci", usb_ehci_options_parser, usb_ehci_options_save);
  return 0; // Success
}

void CDECL libusb_ehci_LTX_plugin_fini(void)
{
  SIM->unregister_addon_option("usb_ehci");
  bx_list_c *menu = (bx_list_c*)SIM->get_param("ports.usb");
  delete theUSB_EHCI;
  menu->remove("ehci");
}

// the device object

bx_usb_ehci_c::bx_usb_ehci_c()
{
  put("usb_ehci", "EHCI");
  memset((void*)&hub, 0, sizeof(bx_usb_ehci_t));
  device_buffer = NULL;
  rt_conf_id = -1;
  hub.frame_timer_index = BX_NULL_TIMER_HANDLE;
}

bx_usb_ehci_c::~bx_usb_ehci_c()
{
  char pname[16];
  int i;

  SIM->unregister_runtime_config_handler(rt_conf_id);
  if (BX_EHCI_THIS device_buffer != NULL)
    delete [] BX_EHCI_THIS device_buffer;

  for (i = 0; i < 3; i++) {
    if (BX_EHCI_THIS uhci[i] != NULL)
      delete BX_EHCI_THIS uhci[i];
  }

  for (i=0; i<USB_EHCI_PORTS; i++) {
    sprintf(pname, "port%d.device", i+1);
    SIM->get_param_string(pname, SIM->get_param(BXPN_USB_EHCI))->set_handler(NULL);
    remove_device(i);
  }

  SIM->get_bochs_root()->remove("usb_ehci");
  bx_list_c *usb_rt = (bx_list_c*)SIM->get_param(BXPN_MENU_RUNTIME_USB);
  usb_rt->remove("ehci");
  BX_DEBUG(("Exit"));
}

void bx_usb_ehci_c::init(void)
{
  unsigned i;
  char pname[6], lfname[10];
  bx_list_c *ehci, *port;
  bx_param_string_c *device;
  Bit8u devfunc;

  // Read in values from config interface
  ehci = (bx_list_c*) SIM->get_param(BXPN_USB_EHCI);
  // Check if the device is disabled or not configured
  if (!SIM->get_param_bool("enabled", ehci)->get()) {
    BX_INFO(("USB EHCI disabled"));
    // mark unused plugin for removal
    ((bx_param_bool_c*)((bx_list_c*)SIM->get_param(BXPN_PLUGIN_CTRL))->get_by_name("usb_ehci"))->set(0);
    return;
  }

  BX_EHCI_THIS device_buffer = new Bit8u[65536];

  // Call our frame timer routine every 1mS (1,024uS)
  // Continuous and active
  BX_EHCI_THIS hub.frame_timer_index = bx_pc_system.register_timer(this, ehci_frame_handler,
                                                 FRAME_TIMER_USEC, 1, 1, "ehci.frame_timer");

  BX_EHCI_THIS devfunc = 0x07;
  DEV_register_pci_handlers(this, &BX_EHCI_THIS devfunc, BX_PLUGIN_USB_EHCI,
                            "Experimental USB EHCI");

  // initialize readonly registers (same as QEMU)
  // 0x8086 = vendor (Intel)
  // 0x24cd = device (82801D)
  // revision number (0x10)
  init_pci_conf(0x8086, 0x24cd, 0x10, 0x0c0320, 0x00);
  BX_EHCI_THIS pci_conf[0x3d] = BX_PCI_INTD;
  BX_EHCI_THIS pci_conf[0x60] = 0x20;
  BX_EHCI_THIS pci_base_address[0] = 0x0;

  for (i = 0; i < 3; i++) {
    BX_EHCI_THIS uhci[i] = new bx_uhci_core_c();
    sprintf(lfname, "usb_uchi%d", i);
    sprintf(pname, "UHCI%d", i);
    BX_EHCI_THIS uhci[i]->put(lfname, pname);
  }
  devfunc = BX_EHCI_THIS devfunc & 0xf8;
  BX_EHCI_THIS uhci[0]->init_uhci(devfunc, 0x24c2, 0x80, BX_PCI_INTA);
  BX_EHCI_THIS uhci[1]->init_uhci(devfunc | 0x01, 0x24c4, 0x00, BX_PCI_INTB);
  BX_EHCI_THIS uhci[2]->init_uhci(devfunc | 0x02, 0x24c7, 0x00, BX_PCI_INTC);

  // initialize capability registers
  BX_EHCI_THIS hub.cap_regs.CapLength = OPS_REGS_OFFSET;
  BX_EHCI_THIS hub.cap_regs.HciVersion = 0x0100;
  BX_EHCI_THIS hub.cap_regs.HcsParams = 0x00103200 | USB_EHCI_PORTS;
  BX_EHCI_THIS hub.cap_regs.HccParams = 0x00006871;

  // initialize runtime configuration
  bx_list_c *usb_rt = (bx_list_c*)SIM->get_param(BXPN_MENU_RUNTIME_USB);
  bx_list_c *ehci_rt = new bx_list_c(usb_rt, "ehci", "EHCI Runtime Options");
  ehci_rt->set_options(ehci_rt->SHOW_PARENT | ehci_rt->USE_BOX_TITLE);
  for (i=0; i<USB_EHCI_PORTS; i++) {
    sprintf(pname, "port%d", i+1);
    port = (bx_list_c*)SIM->get_param(pname, ehci);
    ehci_rt->add(port);
    device = (bx_param_string_c*)port->get_by_name("device");
    device->set_handler(usb_param_handler);
    BX_EHCI_THIS hub.usb_port[i].device = NULL;
    BX_EHCI_THIS hub.usb_port[i].owner_change = 0;
    BX_EHCI_THIS hub.usb_port[i].portsc.ccs = 0;
    BX_EHCI_THIS hub.usb_port[i].portsc.csc = 0;
  }

  // register handler for correct device connect handling after runtime config
  BX_EHCI_THIS rt_conf_id = SIM->register_runtime_config_handler(BX_EHCI_THIS_PTR, runtime_config_handler);
  BX_EHCI_THIS device_change = 0;
  BX_EHCI_THIS maxframes = 128;

  BX_INFO(("USB EHCI initialized"));
}

void bx_usb_ehci_c::reset(unsigned type)
{
  unsigned i;

  for (i = 0; i < 3; i++) {
    uhci[i]->reset_uhci(type);
  }
  if (type == BX_RESET_HARDWARE) {
    static const struct reset_vals_t {
      unsigned      addr;
      unsigned char val;
    } reset_vals[] = {
      { 0x04, 0x00 }, { 0x05, 0x00 }, // command_io
      { 0x06, 0x90 }, { 0x07, 0x02 }, // status
      { 0x0C, 0x08 },                 // cache line size (should be done by BIOS)
      { 0x0D, 0x00 },                 // bus latency
      { 0x0F, 0x00 },                 // BIST is not supported

      // address space 0x10 - 0x17
      { 0x10, 0x00 }, { 0x11, 0x00 },
      { 0x12, 0x00 }, { 0x13, 0x00 }, //

      { 0x34, 0x50 },                 // Capabilities Pointer
      { 0x50, 0x01 },                 // PCI Power Management Capability ID
      { 0x51, 0x58 },                 // Next Item Pointer
      { 0x52, 0xc2 },                 // Power Management Capabilities
      { 0x53, 0xc9 },                 //
      { 0x54, 0x00 },                 // Power Management Control/Status
      { 0x55, 0x00 },                 //
      { 0x58, 0x0a },                 // Debug Port Capability ID
      { 0x59, 0x00 },                 // Next Item Pointer
      { 0x5a, 0x80 },                 // Debug Port Base Offset
      { 0x5b, 0x20 },                 //

      { 0x61, 0x20 },                 // Frame Length Adjustment

      { 0x62, 0x7f },                 // Port Wake Capability

      { 0x68, 0x01 },                 // USB EHCI Legacy Support Extended Capability
      { 0x69, 0x00 },                 //
      { 0x6a, 0x00 },                 //
      { 0x6b, 0x00 },                 //
      { 0x6c, 0x00 },                 // USB EHCI Legacy Support Extended Control/Status
      { 0x6d, 0x00 },                 //
      { 0x6e, 0x00 },                 //
      { 0x6f, 0x00 },                 //

      { 0x70, 0x00 },                 // Intel Specific USB EHCI SMI
      { 0x71, 0x00 },                 //
      { 0x72, 0x00 },                 //
      { 0x73, 0x00 },                 //

      { 0x80, 0x00 },                 // Access Control Register

      { 0xdc, 0x00 },                 // USB HS Reference Voltage
      { 0xdd, 0x00 },                 //
      { 0xde, 0x00 },                 //
      { 0xdf, 0x00 }                  //
    };

    for (i = 0; i < sizeof(reset_vals) / sizeof(*reset_vals); i++) {
        BX_EHCI_THIS pci_conf[reset_vals[i].addr] = reset_vals[i].val;
    }
  }

  BX_EHCI_THIS reset_hc();
}

void bx_usb_ehci_c::register_state(void)
{
  unsigned i;
  char tmpname[16];
  bx_list_c *hub, *op_regs, *port, *reg, *uhcic;

  bx_list_c *list = new bx_list_c(SIM->get_bochs_root(), "usb_ehci", "USB EHCI State");
  hub = new bx_list_c(list, "hub");
  BXRS_DEC_PARAM_FIELD(hub, usbsts_pending, BX_EHCI_THIS hub.usbsts_pending);
  BXRS_DEC_PARAM_FIELD(hub, usbsts_frindex, BX_EHCI_THIS hub.usbsts_frindex);
  BXRS_DEC_PARAM_FIELD(hub, pstate, BX_EHCI_THIS hub.pstate);
  BXRS_DEC_PARAM_FIELD(hub, astate, BX_EHCI_THIS hub.astate);
  BXRS_DEC_PARAM_FIELD(hub, last_run_usec, BX_EHCI_THIS hub.last_run_usec);
  BXRS_DEC_PARAM_FIELD(hub, async_stepdown, BX_EHCI_THIS hub.async_stepdown);
  op_regs = new bx_list_c(hub, "op_regs");
  reg = new bx_list_c(op_regs, "UsbCmd");
  BXRS_HEX_PARAM_FIELD(reg, itc, BX_EHCI_THIS hub.op_regs.UsbCmd.itc);
  BXRS_PARAM_BOOL(reg, iaad, BX_EHCI_THIS hub.op_regs.UsbCmd.iaad);
  BXRS_PARAM_BOOL(reg, ase, BX_EHCI_THIS hub.op_regs.UsbCmd.ase);
  BXRS_PARAM_BOOL(reg, pse, BX_EHCI_THIS hub.op_regs.UsbCmd.pse);
  BXRS_PARAM_BOOL(reg, hcreset, BX_EHCI_THIS hub.op_regs.UsbCmd.hcreset);
  BXRS_PARAM_BOOL(reg, rs, BX_EHCI_THIS hub.op_regs.UsbCmd.rs);
  reg = new bx_list_c(op_regs, "UsbSts");
  BXRS_PARAM_BOOL(reg, ass, BX_EHCI_THIS hub.op_regs.UsbSts.ass);
  BXRS_PARAM_BOOL(reg, pss, BX_EHCI_THIS hub.op_regs.UsbSts.pss);
  BXRS_PARAM_BOOL(reg, recl, BX_EHCI_THIS hub.op_regs.UsbSts.recl);
  BXRS_PARAM_BOOL(reg, hchalted, BX_EHCI_THIS hub.op_regs.UsbSts.hchalted);
  BXRS_HEX_PARAM_FIELD(reg, inti, BX_EHCI_THIS hub.op_regs.UsbSts.inti);
  BXRS_HEX_PARAM_FIELD(op_regs, UsbIntr, BX_EHCI_THIS hub.op_regs.UsbIntr);
  BXRS_HEX_PARAM_FIELD(op_regs, FrIndex, BX_EHCI_THIS hub.op_regs.FrIndex);
  BXRS_HEX_PARAM_FIELD(op_regs, CtrlDsSegment, BX_EHCI_THIS hub.op_regs.CtrlDsSegment);
  BXRS_HEX_PARAM_FIELD(op_regs, PeriodicListBase, BX_EHCI_THIS hub.op_regs.PeriodicListBase);
  BXRS_HEX_PARAM_FIELD(op_regs, AsyncListAddr, BX_EHCI_THIS hub.op_regs.AsyncListAddr);
  BXRS_HEX_PARAM_FIELD(op_regs, ConfigFlag, BX_EHCI_THIS hub.op_regs.ConfigFlag);
  for (i = 0; i < USB_EHCI_PORTS; i++) {
    sprintf(tmpname, "port%d", i+1);
    port = new bx_list_c(hub, tmpname);
    reg = new bx_list_c(port, "portsc");
    BXRS_PARAM_BOOL(reg, woe, BX_EHCI_THIS hub.usb_port[i].portsc.woe);
    BXRS_PARAM_BOOL(reg, wde, BX_EHCI_THIS hub.usb_port[i].portsc.wde);
    BXRS_PARAM_BOOL(reg, wce, BX_EHCI_THIS hub.usb_port[i].portsc.wce);
    BXRS_HEX_PARAM_FIELD(reg, ptc, BX_EHCI_THIS hub.usb_port[i].portsc.ptc);
    BXRS_HEX_PARAM_FIELD(reg, pic, BX_EHCI_THIS hub.usb_port[i].portsc.pic);
    BXRS_PARAM_BOOL(reg, po, BX_EHCI_THIS hub.usb_port[i].portsc.po);
    BXRS_HEX_PARAM_FIELD(reg, ls, BX_EHCI_THIS hub.usb_port[i].portsc.ls);
    BXRS_PARAM_BOOL(reg, pr, BX_EHCI_THIS hub.usb_port[i].portsc.pr);
    BXRS_PARAM_BOOL(reg, sus, BX_EHCI_THIS hub.usb_port[i].portsc.sus);
    BXRS_PARAM_BOOL(reg, fpr, BX_EHCI_THIS hub.usb_port[i].portsc.fpr);
    BXRS_PARAM_BOOL(reg, occ, BX_EHCI_THIS hub.usb_port[i].portsc.occ);
    BXRS_PARAM_BOOL(reg, oca, BX_EHCI_THIS hub.usb_port[i].portsc.oca);
    BXRS_PARAM_BOOL(reg, pec, BX_EHCI_THIS hub.usb_port[i].portsc.pec);
    BXRS_PARAM_BOOL(reg, ped, BX_EHCI_THIS hub.usb_port[i].portsc.ped);
    BXRS_PARAM_BOOL(reg, csc, BX_EHCI_THIS hub.usb_port[i].portsc.csc);
    BXRS_PARAM_BOOL(reg, ccs, BX_EHCI_THIS hub.usb_port[i].portsc.ccs);
  }
  for (i = 0; i < 3; i++) {
    sprintf(tmpname, "uhci%d", i);
    uhcic = new bx_list_c(list, tmpname);
    uhci[i]->register_state(uhcic);
  }

  register_pci_state(hub);
}

void bx_usb_ehci_c::after_restore_state(void)
{
  int i;

  if (DEV_pci_set_base_mem(BX_EHCI_THIS_PTR, read_handler, write_handler,
                         &BX_EHCI_THIS pci_base_address[0],
                         &BX_EHCI_THIS pci_conf[0x10],
                         256))  {
     BX_INFO(("new base address: 0x%04X", BX_EHCI_THIS pci_base_address[0]));
  }
  for (i=0; i<USB_EHCI_PORTS; i++) {
    if (BX_EHCI_THIS hub.usb_port[i].device != NULL) {
      BX_EHCI_THIS hub.usb_port[i].device->after_restore_state();
    }
  }
  for (i = 0; i < 3; i++) {
    uhci[i]->after_restore_state();
  }
}

void bx_usb_ehci_c::reset_hc()
{
  int i;
  char pname[6];

  BX_EHCI_THIS hub.op_regs.UsbCmd.itc = 0x08;
  BX_EHCI_THIS hub.op_regs.UsbCmd.iaad = 0;
  BX_EHCI_THIS hub.op_regs.UsbCmd.ase = 0;
  BX_EHCI_THIS hub.op_regs.UsbCmd.pse = 0;
  BX_EHCI_THIS hub.op_regs.UsbCmd.hcreset = 0;
  BX_EHCI_THIS hub.op_regs.UsbCmd.rs = 0;
  BX_EHCI_THIS hub.op_regs.UsbSts.ass = 0;
  BX_EHCI_THIS hub.op_regs.UsbSts.pss = 0;
  BX_EHCI_THIS hub.op_regs.UsbSts.recl = 0;
  BX_EHCI_THIS hub.op_regs.UsbSts.hchalted = 1;
  BX_EHCI_THIS hub.op_regs.UsbSts.inti = 0;
  BX_EHCI_THIS hub.op_regs.UsbIntr = 0x0;
  BX_EHCI_THIS hub.op_regs.FrIndex = 0x0;
  BX_EHCI_THIS hub.op_regs.CtrlDsSegment = 0x0;
  BX_EHCI_THIS hub.op_regs.PeriodicListBase = 0x0;
  BX_EHCI_THIS hub.op_regs.AsyncListAddr = 0x0;
  BX_EHCI_THIS hub.op_regs.ConfigFlag = 0x0;

  // Ports[x]
  for (i=0; i<USB_EHCI_PORTS; i++) {
    reset_port(i);
    if (BX_EHCI_THIS hub.usb_port[i].device == NULL) {
      sprintf(pname, "port%d", i+1);
      init_device(i, (bx_list_c*)SIM->get_param(pname, SIM->get_param(BXPN_USB_EHCI)));
    } else {
      set_connect_status(i, BX_EHCI_THIS hub.usb_port[i].device->get_type(), 1);
    }
  }

  BX_EHCI_THIS hub.usbsts_pending = 0;
  BX_EHCI_THIS hub.usbsts_frindex = 0;
  BX_EHCI_THIS hub.astate = EST_INACTIVE;
  BX_EHCI_THIS hub.pstate = EST_INACTIVE;
  BX_EHCI_THIS update_irq();
}

void bx_usb_ehci_c::reset_port(int p)
{
  BX_EHCI_THIS hub.usb_port[p].portsc.woe = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.wde = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.wce = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.ptc = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.pic = 0;
  if (!BX_EHCI_THIS hub.usb_port[p].portsc.po) {
    BX_EHCI_THIS hub.usb_port[p].owner_change = 1;
    BX_EHCI_THIS change_port_owner(p);
  }
  BX_EHCI_THIS hub.usb_port[p].portsc.pp  = 1;
  BX_EHCI_THIS hub.usb_port[p].portsc.ls  = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.pr  = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.sus = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.fpr = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.occ = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.oca = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.pec = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.ped = 0;
  BX_EHCI_THIS hub.usb_port[p].portsc.csc = 0;
}

void bx_usb_ehci_c::init_device(Bit8u port, bx_list_c *portconf)
{
  usbdev_type type;
  char pname[BX_PATHNAME_LEN];
  const char *devname = NULL;

  devname = ((bx_param_string_c*)portconf->get_by_name("device"))->getptr(); 
  if (devname == NULL) return; 
  if (!strlen(devname) || !strcmp(devname, "none")) return;

  if (BX_EHCI_THIS hub.usb_port[port].device != NULL) {
    BX_ERROR(("init_device(): port%d already in use", port+1));
    return;
  }
  sprintf(pname, "usb_ehci.hub.port%d.device", port+1);
  bx_list_c *sr_list = (bx_list_c*)SIM->get_param(pname, SIM->get_bochs_root());
  type = DEV_usb_init_device(portconf, BX_EHCI_THIS_PTR, &BX_EHCI_THIS hub.usb_port[port].device, sr_list);
  if (BX_EHCI_THIS hub.usb_port[port].device != NULL) {
    set_connect_status(port, type, 1);
  }
}

void bx_usb_ehci_c::remove_device(Bit8u port)
{
  char pname[BX_PATHNAME_LEN];

  if (BX_EHCI_THIS hub.usb_port[port].device != NULL) {
    sprintf(pname, "usb_ehci.hub.port%d.device", port+1);
    bx_list_c *devlist = (bx_list_c*)SIM->get_param(pname, SIM->get_bochs_root());
    if (devlist) devlist->clear();
    delete BX_EHCI_THIS hub.usb_port[port].device;
    BX_EHCI_THIS hub.usb_port[port].device = NULL;
  }
}

void bx_usb_ehci_c::set_connect_status(Bit8u port, int type, bx_bool connected)
{
  const bx_bool ccs_org = BX_EHCI_THIS hub.usb_port[port].portsc.ccs;
  const bx_bool ped_org = BX_EHCI_THIS hub.usb_port[port].portsc.ped;

  usb_device_c *device = BX_EHCI_THIS hub.usb_port[port].device;
  if (device != NULL) {
    if (device->get_type() == type) {
      if (connected) {
        if (BX_EHCI_THIS hub.usb_port[port].portsc.po) {
          BX_EHCI_THIS uhci[port >> 1]->set_port_device(port & 1, device);
          return;
        }
        if (device->get_speed() == USB_SPEED_SUPER) {
          BX_PANIC(("Super-speed device not supported on USB2 port."));
          return;
        }
        if (device->get_speed() == USB_SPEED_HIGH) {
          BX_PANIC(("High-speed device not yet supported by EHCI emulation"));
          return;
        }
        switch (device->get_speed()) {
          case USB_SPEED_LOW:
            BX_INFO(("Low speed device connected to port #%d", port+1));
            BX_EHCI_THIS hub.usb_port[port].portsc.ls = 0x1;
            BX_EHCI_THIS hub.usb_port[port].portsc.ped = 0;
            break;
          case USB_SPEED_FULL:
            BX_INFO(("Full speed device connected to port #%d", port+1));
            BX_EHCI_THIS hub.usb_port[port].portsc.ls = 0x2;
            BX_EHCI_THIS hub.usb_port[port].portsc.ped = 0;
            break;
          case USB_SPEED_HIGH:
            BX_INFO(("High speed device connected to port #%d", port+1));
            BX_EHCI_THIS hub.usb_port[port].portsc.ls = 0x0;
            BX_EHCI_THIS hub.usb_port[port].portsc.ped = 1;
            break;
          default:
            BX_ERROR(("device->get_speed() returned invalid speed value"));
        }
        BX_EHCI_THIS hub.usb_port[port].portsc.ccs = 1;
        if (!device->get_connected()) {
          if (!device->init()) {
            set_connect_status(port, type, 0);
            BX_ERROR(("port #%d: connect failed", port+1));
          } else {
            BX_INFO(("port #%d: connect: %s", port+1, device->get_info()));
          }
        }
      } else { // not connected
        if (BX_EHCI_THIS hub.usb_port[port].portsc.po) {
          BX_EHCI_THIS uhci[port >> 1]->set_port_device(port & 1, NULL);
          if ((!BX_EHCI_THIS hub.usb_port[port].owner_change) &&
              (BX_EHCI_THIS hub.op_regs.ConfigFlag & 1)) {
            BX_EHCI_THIS hub.usb_port[port].portsc.po = 0;
            BX_EHCI_THIS hub.usb_port[port].portsc.csc = 1;
          }
        } else {
          BX_EHCI_THIS hub.usb_port[port].portsc.ccs = 0;
          BX_EHCI_THIS hub.usb_port[port].portsc.ped = 0;
        }
        if (!BX_EHCI_THIS hub.usb_port[port].owner_change) {
          remove_device(port);
        }
        if (BX_EHCI_THIS hub.usb_port[port].portsc.po)
          return;
      }
    }
    if (ccs_org != BX_EHCI_THIS hub.usb_port[port].portsc.ccs)
      BX_EHCI_THIS hub.usb_port[port].portsc.csc = 1;
    if (ped_org != BX_EHCI_THIS hub.usb_port[port].portsc.ped)
      BX_EHCI_THIS hub.usb_port[port].portsc.pec = 1;

    BX_EHCI_THIS hub.op_regs.UsbSts.inti |= USBSTS_PCD;
    BX_EHCI_THIS update_irq();
  }
}

void bx_usb_ehci_c::change_port_owner(int port)
{
  if (port < 0) {
    for (int i=0; i<USB_EHCI_PORTS; i++) {
      change_port_owner(i);
    }
  } else {
    usb_device_c *device = BX_EHCI_THIS hub.usb_port[port].device;
    if (BX_EHCI_THIS hub.usb_port[port].owner_change) {
      BX_INFO(("port #%d: owner change to %s", port+1,
               BX_EHCI_THIS hub.usb_port[port].portsc.po ? "EHCI":"UHCI"));
      if (device != NULL) {
        set_connect_status(port, device->get_type(), 0);
      }
      BX_EHCI_THIS hub.usb_port[port].portsc.po ^= 1;
      if (device != NULL) {
        set_connect_status(port, device->get_type(), 1);
      }
    }
    BX_EHCI_THIS hub.usb_port[port].owner_change = 0;
  }
}

bx_bool bx_usb_ehci_c::read_handler(bx_phy_address addr, unsigned len, void *data, void *param)
{
  Bit32u val = 0, val_hi = 0;
  int port;
  const Bit32u offset = (Bit32u) (addr - BX_EHCI_THIS pci_base_address[0]);

  if (offset < OPS_REGS_OFFSET) {
    switch (offset) {
      case 0x00:
        val = BX_EHCI_THIS hub.cap_regs.CapLength;
        break;
      case 0x02:
        val = BX_EHCI_THIS hub.cap_regs.HciVersion;
        break;
      case 0x04:
        val = BX_EHCI_THIS hub.cap_regs.HcsParams;
        break;
      case 0x08:
        val = BX_EHCI_THIS hub.cap_regs.HccParams;
        break;
    }
  } else {
    switch (offset - OPS_REGS_OFFSET) {
      case 0x00:
        val = ((BX_EHCI_THIS hub.op_regs.UsbCmd.itc     << 16)
             | (BX_EHCI_THIS hub.op_regs.UsbCmd.iaad    << 6)
             | (BX_EHCI_THIS hub.op_regs.UsbCmd.ase     << 5)
             | (BX_EHCI_THIS hub.op_regs.UsbCmd.pse     << 4)
             | (BX_EHCI_THIS hub.op_regs.UsbCmd.hcreset << 1)
             |  BX_EHCI_THIS hub.op_regs.UsbCmd.rs);
        break;
      case 0x04:
        val = ((BX_EHCI_THIS hub.op_regs.UsbSts.ass      << 15)
             | (BX_EHCI_THIS hub.op_regs.UsbSts.pss      << 14)
             | (BX_EHCI_THIS hub.op_regs.UsbSts.recl     << 13)
             | (BX_EHCI_THIS hub.op_regs.UsbSts.hchalted << 12)
             |  BX_EHCI_THIS hub.op_regs.UsbSts.inti);
        break;
      case 0x08:
        val = BX_EHCI_THIS hub.op_regs.UsbIntr;
        break;
      case 0x0c:
        val = BX_EHCI_THIS hub.op_regs.FrIndex;
        break;
      case 0x10:
        val = BX_EHCI_THIS hub.op_regs.CtrlDsSegment;
        break;
      case 0x14:
        val = BX_EHCI_THIS hub.op_regs.PeriodicListBase;
        break;
      case 0x18:
        val = BX_EHCI_THIS hub.op_regs.AsyncListAddr;
        break;
      case 0x40:
        val = BX_EHCI_THIS hub.op_regs.ConfigFlag;
        break;
      default:
        port = (offset - OPS_REGS_OFFSET - 0x44) / 4;
        if (port < USB_EHCI_PORTS) {
          val = ((BX_EHCI_THIS hub.usb_port[port].portsc.woe << 22)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.wde << 21)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.wce << 20)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.ptc << 16)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.pic << 14)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.po << 13)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.pp << 12)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.ls << 10)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.pr << 8)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.sus << 7)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.fpr << 6)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.occ << 5)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.oca << 4)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.pec << 3)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.ped << 2)
               | (BX_EHCI_THIS hub.usb_port[port].portsc.csc << 1)
               | BX_EHCI_THIS hub.usb_port[port].portsc.ccs);
        }
    }
  }

  switch (len) {
    case 1:
      val &= 0xFF;
      *((Bit8u *) data) = (Bit8u) val;
      break;
    case 2:
      val &= 0xFFFF;
      *((Bit16u *) data) = (Bit16u) val;
      break;
    case 8:
      *((Bit32u *) ((Bit8u *) data + 4)) = val_hi;
    case 4:
      *((Bit32u *) data) = val;
      break;
  }
#if BX_PHY_ADDRESS_LONG
    BX_DEBUG(("register read from offset 0x%04X:  0x%08X%08X (len=%i)", offset, (Bit32u) val_hi, (Bit32u) val, len));
#else
    BX_DEBUG(("register read from offset 0x%04X:  0x%08X (len=%i)", offset, (Bit32u) val, len));
#endif

  return 1;
}

bx_bool bx_usb_ehci_c::write_handler(bx_phy_address addr, unsigned len, void *data, void *param)
{
  Bit32u value = *((Bit32u *) data);
  Bit32u value_hi = *((Bit32u *) ((Bit8u *) data + 4));
  bx_bool oldcfg, oldpo, oldpr, oldfpr;
  int i, port;
  const Bit32u offset = (Bit32u) (addr - BX_EHCI_THIS pci_base_address[0]);

  // modify val and val_hi per len of data to write
  switch (len) {
    case 1:
      value &= 0xFF;
    case 2:
      value &= 0xFFFF;
    case 4:
      value_hi = 0;
      break;
  }

#if BX_PHY_ADDRESS_LONG
    BX_DEBUG(("register write to  offset 0x%04X:  0x%08X%08X (len=%i)", offset, value_hi, value, len));
#else
    BX_DEBUG(("register write to  offset 0x%04X:  0x%08X (len=%i)", offset, value, len));
#endif

  if (offset >= OPS_REGS_OFFSET) {
    switch (offset - OPS_REGS_OFFSET) {
      case 0x00:
        BX_EHCI_THIS hub.op_regs.UsbCmd.itc   = (value >> 16) & 0x7f;
        BX_EHCI_THIS hub.op_regs.UsbCmd.iaad  = (value >> 6) & 1;
        BX_EHCI_THIS hub.op_regs.UsbCmd.ase   = (value >> 5) & 1;
        BX_EHCI_THIS hub.op_regs.UsbCmd.pse   = (value >> 4) & 1;
        BX_EHCI_THIS hub.op_regs.UsbCmd.hcreset = (value >> 1) & 1;
        BX_EHCI_THIS hub.op_regs.UsbCmd.rs    = (value & 1);
        if (BX_EHCI_THIS hub.op_regs.UsbCmd.iaad) {
          BX_EHCI_THIS hub.async_stepdown = 0;
          // TODO
        }
        if (BX_EHCI_THIS hub.op_regs.UsbCmd.hcreset) {
          BX_EHCI_THIS reset_hc();
          BX_EHCI_THIS hub.op_regs.UsbCmd.hcreset = 0;
        }
        if (BX_EHCI_THIS hub.op_regs.UsbCmd.rs) {
          BX_EHCI_THIS hub.op_regs.UsbSts.hchalted = 0;
        } else {
          BX_EHCI_THIS hub.op_regs.UsbSts.hchalted = 1;
        }
        break;
      case 0x04:
        BX_EHCI_THIS hub.op_regs.UsbSts.inti ^= (value & USBINTR_MASK);
        BX_EHCI_THIS update_irq();
        break;
      case 0x08:
        BX_EHCI_THIS hub.op_regs.UsbIntr = (Bit8u)(value & USBINTR_MASK);
        break;
      case 0x0c:
        if (!BX_EHCI_THIS hub.op_regs.UsbCmd.rs) {
          BX_EHCI_THIS hub.op_regs.FrIndex = (value & 0x1fff);
        }
        break;
      case 0x10:
        BX_EHCI_THIS hub.op_regs.CtrlDsSegment = value;
        break;
      case 0x14:
        BX_EHCI_THIS hub.op_regs.PeriodicListBase = (value & 0xfffff000);
        break;
      case 0x18:
        BX_EHCI_THIS hub.op_regs.AsyncListAddr = (value & 0xffffffe0);
        break;
      case 0x40:
        oldcfg = (BX_EHCI_THIS hub.op_regs.ConfigFlag & 1);
        BX_EHCI_THIS hub.op_regs.ConfigFlag = (value & 1);
        if (!oldcfg && (value & 1)) {
          for (i=0; i<USB_EHCI_PORTS; i++) {
            BX_EHCI_THIS hub.usb_port[i].owner_change = BX_EHCI_THIS hub.usb_port[i].portsc.po;
          }
        } else if (!(value & 1)) {
          for (i=0; i<USB_EHCI_PORTS; i++) {
            BX_EHCI_THIS hub.usb_port[i].owner_change = !BX_EHCI_THIS hub.usb_port[i].portsc.po;
          }
        }
        BX_EHCI_THIS change_port_owner(-1);
        break;
      default:
        port = (offset - OPS_REGS_OFFSET - 0x44) / 4;
        if (port < USB_EHCI_PORTS) {
          oldpo = BX_EHCI_THIS hub.usb_port[port].portsc.po;
          oldpr = BX_EHCI_THIS hub.usb_port[port].portsc.pr;
          oldfpr = BX_EHCI_THIS hub.usb_port[port].portsc.fpr;
          BX_EHCI_THIS hub.usb_port[port].portsc.woe = (value >> 22) & 1;
          BX_EHCI_THIS hub.usb_port[port].portsc.wde = (value >> 21) & 1;
          BX_EHCI_THIS hub.usb_port[port].portsc.wce = (value >> 20) & 1;
          BX_EHCI_THIS hub.usb_port[port].portsc.ptc = (value >> 16) & 0xf;
          BX_EHCI_THIS hub.usb_port[port].portsc.pic = (value >> 14) & 3;
          BX_EHCI_THIS hub.usb_port[port].portsc.pr = (value >> 8) & 1;
          if ((value >> 7) & 1) BX_EHCI_THIS hub.usb_port[port].portsc.sus = 1;
          BX_EHCI_THIS hub.usb_port[port].portsc.fpr = (value >> 6) & 1;
          if ((value >> 5) & 1) BX_EHCI_THIS hub.usb_port[port].portsc.occ = 0;
          if ((value >> 3) & 1) BX_EHCI_THIS hub.usb_port[port].portsc.pec = 0;
          if (!((value >> 2) & 1)) BX_EHCI_THIS hub.usb_port[port].portsc.ped = 0;
          if ((value >> 1) & 1) BX_EHCI_THIS hub.usb_port[port].portsc.csc = 0;
          if (oldpo != ((value >> 13) & 1)) {
            BX_EHCI_THIS hub.usb_port[port].owner_change = 1;
            BX_EHCI_THIS change_port_owner(port);
          }
          if (oldpr && !BX_EHCI_THIS hub.usb_port[port].portsc.pr) {
            if (BX_EHCI_THIS hub.usb_port[port].device != NULL) {
              DEV_usb_send_msg(BX_EHCI_THIS hub.usb_port[port].device, USB_MSG_RESET);
              BX_EHCI_THIS hub.usb_port[port].portsc.csc = 0;
              if (BX_EHCI_THIS hub.usb_port[port].device->get_speed() == USB_SPEED_HIGH) {
                BX_EHCI_THIS hub.usb_port[port].portsc.ped = 1;
              }
            }
          }
          if (oldfpr && !BX_EHCI_THIS hub.usb_port[port].portsc.fpr) {
            BX_EHCI_THIS hub.usb_port[port].portsc.sus = 0;
          }
        }
    }
  }

  return 1;
}

void bx_usb_ehci_c::update_irq(void)
{
  bx_bool level = 0;

  if ((BX_EHCI_THIS hub.op_regs.UsbSts.inti & BX_EHCI_THIS hub.op_regs.UsbIntr) > 0) {
    level = 1;
    BX_DEBUG(("Interrupt Fired."));
  }
  DEV_pci_set_irq(BX_EHCI_THIS devfunc, BX_EHCI_THIS pci_conf[0x3d], level);
}

void bx_usb_ehci_c::raise_irq(Bit8u intr)
{
  if (intr & (USBSTS_PCD | USBSTS_FLR | USBSTS_HSE)) {
    BX_EHCI_THIS hub.op_regs.UsbSts.inti |= intr;
    BX_EHCI_THIS update_irq();
  } else {
    BX_EHCI_THIS hub.usbsts_pending |= intr;
  }
}

void bx_usb_ehci_c::commit_irq(void)
{
  Bit32u itc;

  if (!BX_EHCI_THIS hub.usbsts_pending) {
    return;
  }
  if (BX_EHCI_THIS hub.usbsts_frindex > BX_EHCI_THIS hub.op_regs.FrIndex) {
    return;
  }

  itc = BX_EHCI_THIS hub.op_regs.UsbCmd.itc;
  BX_EHCI_THIS hub.op_regs.UsbSts.inti |= BX_EHCI_THIS hub.usbsts_pending;
  BX_EHCI_THIS hub.usbsts_pending = 0;
  BX_EHCI_THIS hub.usbsts_frindex = BX_EHCI_THIS hub.op_regs.FrIndex + itc;
  BX_EHCI_THIS update_irq();
}

void bx_usb_ehci_c::update_halt(void)
{
  if (BX_EHCI_THIS hub.op_regs.UsbCmd.rs) {
    BX_EHCI_THIS hub.op_regs.UsbSts.hchalted = 0;
  } else {
    if (BX_EHCI_THIS hub.astate == EST_INACTIVE && BX_EHCI_THIS hub.pstate == EST_INACTIVE) {
      BX_EHCI_THIS hub.op_regs.UsbSts.hchalted = 1;
    }
  }
}

void bx_usb_ehci_c::set_state(int async, int state)
{
  if (async) {
    BX_EHCI_THIS hub.astate = state;
    if (BX_EHCI_THIS hub.astate == EST_INACTIVE) {
      BX_EHCI_THIS hub.op_regs.UsbSts.ass = 0;
      BX_EHCI_THIS update_halt();
    } else {
      BX_EHCI_THIS hub.op_regs.UsbSts.ass = 1;
    }
  } else {
    BX_EHCI_THIS hub.pstate = state;
    if (BX_EHCI_THIS hub.pstate == EST_INACTIVE) {
      BX_EHCI_THIS hub.op_regs.UsbSts.pss = 0;
      BX_EHCI_THIS update_halt();
    } else {
      BX_EHCI_THIS hub.op_regs.UsbSts.pss = 1;
    }
  }
}

int bx_usb_ehci_c::get_state(int async)
{
  return async ? BX_EHCI_THIS hub.astate : BX_EHCI_THIS hub.pstate;
}

void bx_usb_ehci_c::set_fetch_addr(int async, Bit32u addr)
{
  if (async) {
    BX_EHCI_THIS hub.a_fetch_addr = addr;
  } else {
    BX_EHCI_THIS hub.p_fetch_addr = addr;
  }
}

int bx_usb_ehci_c::get_fetch_addr(int async)
{
  return async ? BX_EHCI_THIS hub.a_fetch_addr : BX_EHCI_THIS hub.p_fetch_addr;
}

EHCIPacket *bx_usb_ehci_c::alloc_packet(EHCIQueue *q)
{
  EHCIPacket *p = new EHCIPacket;
  p->queue = q;
  // TODO: usb_packet_init(&p->packet);
  QTAILQ_INSERT_TAIL(&q->packets, p, next);
  return p;
}

void bx_usb_ehci_c::free_packet(EHCIPacket *p)
{
  if (p->async == EHCI_ASYNC_FINISHED) {
    int state = BX_EHCI_THIS get_state(p->queue->async);
    /* This is a normal, but rare condition (cancel racing completion) */
    BX_ERROR(("EHCI: Warning packet completed but not processed"));
    BX_EHCI_THIS state_executing(p->queue);
    BX_EHCI_THIS state_writeback(p->queue);
    BX_EHCI_THIS set_state(p->queue->async, state);
    /* state_writeback recurses into us with async == EHCI_ASYNC_NONE!! */
    return;
  }
  if (p->async == EHCI_ASYNC_INITIALIZED) {
    // TODO: usb_packet_unmap(&p->packet, &p->sgl);
    // TODO: qemu_sglist_destroy(&p->sgl);
  }
  if (p->async == EHCI_ASYNC_INFLIGHT) {
    usb_cancel_packet(&p->packet);
    // TODO: usb_packet_unmap(&p->packet, &p->sgl);
    // TODO: qemu_sglist_destroy(&p->sgl);
  }
  QTAILQ_REMOVE(&p->queue->packets, p, next);
  // TODO: usb_packet_cleanup(&p->packet);
  delete p;
}

EHCIQueue *bx_usb_ehci_c::alloc_queue(Bit32u addr, int async)
{
  EHCIQueueHead *head = async ? &BX_EHCI_THIS hub.aqueues : &BX_EHCI_THIS hub.pqueues;
  EHCIQueue *q;

  q = new EHCIQueue;
  q->ehci = &BX_EHCI_THIS hub;
  q->qhaddr = addr;
  q->async = async;
  QTAILQ_INIT(&q->packets);
  QTAILQ_INSERT_HEAD(head, q, next);
  return q;
}

int bx_usb_ehci_c::cancel_queue(EHCIQueue *q)
{
  EHCIPacket *p;
  int packets = 0;

  p = QTAILQ_FIRST(&q->packets);
  if (p == NULL) {
    return 0;
  }

  do {
    free_packet(p);
    packets++;
  } while ((p = QTAILQ_FIRST(&q->packets)) != NULL);
  return packets;
}

int bx_usb_ehci_c::reset_queue(EHCIQueue *q)
{
  int packets = BX_EHCI_THIS cancel_queue(q);
  q->dev = NULL;
  q->qtdaddr = 0;
  return packets;
}

void bx_usb_ehci_c::free_queue(EHCIQueue *q, const char *warn)
{
  EHCIQueueHead *head = q->async ? &q->ehci->aqueues : &q->ehci->pqueues;
  int cancelled;

  cancelled = BX_EHCI_THIS cancel_queue(q);
  if (warn && cancelled > 0) {
//    ehci_trace_guest_bug(q->ehci, warn);
  }
  QTAILQ_REMOVE(head, q, next);
  free(q);
}

EHCIQueue *bx_usb_ehci_c::find_queue_by_qh(Bit32u addr, int async)
{
  EHCIQueueHead *head = async ? &BX_EHCI_THIS hub.aqueues : &BX_EHCI_THIS hub.pqueues;
  EHCIQueue *q;

  QTAILQ_FOREACH(q, head, next) {
    if (addr == q->qhaddr) {
      return q;
    }
  }
  return NULL;
}

void bx_usb_ehci_c::queues_rip_unused(int async)
{
  EHCIQueueHead *head = async ? &BX_EHCI_THIS hub.aqueues : &BX_EHCI_THIS hub.pqueues;
  const char *warn = async ? "guest unlinked busy QH" : NULL;
  Bit64u maxage = FRAME_TIMER_USEC * BX_EHCI_THIS maxframes * 4;
  EHCIQueue *q, *tmp;

  QTAILQ_FOREACH_SAFE(q, head, next, tmp) {
    if (q->seen) {
      q->seen = 0;
      q->ts = BX_EHCI_THIS hub.last_run_usec;
      continue;
    }
    if (BX_EHCI_THIS hub.last_run_usec < q->ts + maxage) {
      continue;
    }
    BX_EHCI_THIS free_queue(q, warn);
  }
}

void bx_usb_ehci_c::queues_rip_unseen(int async)
{
  EHCIQueueHead *head = async ? &BX_EHCI_THIS hub.aqueues : &BX_EHCI_THIS hub.pqueues;
  EHCIQueue *q, *tmp;

  QTAILQ_FOREACH_SAFE(q, head, next, tmp) {
    if (!q->seen) {
      BX_EHCI_THIS free_queue(q, NULL);
    }
  }
}

void bx_usb_ehci_c::queues_rip_device(usb_device_c *dev, int async)
{
  EHCIQueueHead *head = async ? &BX_EHCI_THIS hub.aqueues : &BX_EHCI_THIS hub.pqueues;
  EHCIQueue *q, *tmp;

  QTAILQ_FOREACH_SAFE(q, head, next, tmp) {
    if (q->dev != dev) {
      continue;
    }
    BX_EHCI_THIS free_queue(q, NULL);
  }
}

void bx_usb_ehci_c::queues_rip_all(int async)
{
  EHCIQueueHead *head = async ? &BX_EHCI_THIS hub.aqueues : &BX_EHCI_THIS hub.pqueues;
  const char *warn = async ? "guest stopped busy async schedule" : NULL;
  EHCIQueue *q, *tmp;

  QTAILQ_FOREACH_SAFE(q, head, next, tmp) {
    BX_EHCI_THIS free_queue(q, warn);
  }
}

usb_device_c *bx_usb_ehci_c::find_device(Bit8u addr)
{
  usb_device_c *dev = NULL;

  for (int i = 0; i < USB_EHCI_PORTS; i++) {
    if (!BX_EHCI_THIS hub.usb_port[i].portsc.ped) {
      BX_DEBUG(("Port %d not enabled", i));
      continue;
    }
    // TODO: dev = usb_find_device(i, addr);
    if (dev != NULL) {
      return dev;
    }
  }
  return NULL;
}

void bx_usb_ehci_c::flush_qh(EHCIQueue *q)
{
    Bit32u *qh = (Bit32u *) &q->qh;
    Bit32u dwords = sizeof(EHCIqh) >> 2;
    Bit32u addr = NLPTR_GET(q->qhaddr);

    // TODO: put_dwords(q->ehci, addr + 3 * sizeof(uint32_t), qh + 3, dwords - 3);
}

int bx_usb_ehci_c::qh_do_overlay(EHCIQueue *q)
{
  EHCIPacket *p = QTAILQ_FIRST(&q->packets);
  int i;
  int dtoggle;
  int ping;
  int eps;
  int reload;

  assert(p != NULL);
  assert(p->qtdaddr == q->qtdaddr);

  dtoggle = q->qh.token & QTD_TOKEN_DTOGGLE;
  ping    = q->qh.token & QTD_TOKEN_PING;

  q->qh.current_qtd = p->qtdaddr;
  q->qh.next_qtd    = p->qtd.next;
  q->qh.altnext_qtd = p->qtd.altnext;
  q->qh.token       = p->qtd.token;


  eps = get_field(q->qh.epchar, QH_EPCHAR_EPS);
  if (eps == EHCI_QH_EPS_HIGH) {
    q->qh.token &= ~QTD_TOKEN_PING;
    q->qh.token |= ping;
  }

  reload = get_field(q->qh.epchar, QH_EPCHAR_RL);
  set_field(&q->qh.altnext_qtd, reload, QH_ALTNEXT_NAKCNT);

  for (i = 0; i < 5; i++) {
    q->qh.bufptr[i] = p->qtd.bufptr[i];
  }

  if (!(q->qh.epchar & QH_EPCHAR_DTC)) {
    q->qh.token &= ~QTD_TOKEN_DTOGGLE;
    q->qh.token |= dtoggle;
  }

  q->qh.bufptr[1] &= ~BUFPTR_CPROGMASK_MASK;
  q->qh.bufptr[2] &= ~BUFPTR_FRAMETAG_MASK;

  BX_EHCI_THIS flush_qh(q);

  return 0;
}

int bx_usb_ehci_c::init_transfer(EHCIPacket *p)
{
  Bit32u cpage, offset, bytes, plen;
  Bit64u page;

  cpage  = get_field(p->qtd.token, QTD_TOKEN_CPAGE);
  bytes  = get_field(p->qtd.token, QTD_TOKEN_TBYTES);
  offset = p->qtd.bufptr[0] & ~QTD_BUFPTR_MASK;
  // TODO: pci_dma_sglist_init(&p->sgl, &p->queue->ehci->dev, 5);

  while (bytes > 0) {
    if (cpage > 4) {
      BX_ERROR(("cpage out of range (%d)", cpage));
      return USB_RET_PROCERR;
    }

    page  = p->qtd.bufptr[cpage] & QTD_BUFPTR_MASK;
    page += offset;
    plen  = bytes;
    if (plen > 4096 - offset) {
      plen = 4096 - offset;
      offset = 0;
      cpage++;
    }

    // TODO: qemu_sglist_add(&p->sgl, page, plen);
    bytes -= plen;
  }
  return 0;
}

void bx_usb_ehci_c::finish_transfer(EHCIQueue *q, int status)
{
  Bit32u cpage, offset;

  if (status > 0) {
    cpage  = get_field(q->qh.token, QTD_TOKEN_CPAGE);
    offset = q->qh.bufptr[0] & ~QTD_BUFPTR_MASK;

    offset += status;
    cpage  += offset >> QTD_BUFPTR_SH;
    offset &= ~QTD_BUFPTR_MASK;

    set_field(&q->qh.token, cpage, QTD_TOKEN_CPAGE);
    q->qh.bufptr[0] &= QTD_BUFPTR_MASK;
    q->qh.bufptr[0] |= offset;
  }
}

void bx_usb_ehci_c::async_complete_packet(USBPacket *packet)
{
  // TODO
}

void bx_usb_ehci_c::execute_complete(EHCIQueue *q)
{
  // TODO
}

int bx_usb_ehci_c::execute(EHCIPacket *p, const char *action)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::process_itd(EHCIitd *itd, Bit32u addr)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::state_waitlisthead(int async)
{
  EHCIqh qh;
  int i = 0;
  int again = 0;
  Bit32u entry = BX_EHCI_THIS hub.op_regs.AsyncListAddr;

  /* set reclamation flag at start event (4.8.6) */
  if (async) {
    BX_EHCI_THIS hub.op_regs.UsbSts.recl = 1;
  }

  BX_EHCI_THIS queues_rip_unused(async);

  /*  Find the head of the list (4.9.1.1) */
  for (i = 0; i < MAX_QH; i++) {
    // TODO
//    get_dwords(NLPTR_GET(entry), (Bit32u *) &qh,
//               sizeof(EHCIqh) >> 2);

    if (qh.epchar & QH_EPCHAR_H) {
      if (async) {
        entry |= (NLPTR_TYPE_QH << 1);
      }

      BX_EHCI_THIS set_fetch_addr(async, entry);
      BX_EHCI_THIS set_state(async, EST_FETCHENTRY);
      again = 1;
      goto out;
    }

    entry = qh.next;
    if (entry == BX_EHCI_THIS hub.op_regs.AsyncListAddr) {
      break;
    }
  }

  /* no head found for list. */

  BX_EHCI_THIS set_state(async, EST_ACTIVE);

out:
  return again;
}

int bx_usb_ehci_c::state_fetchentry(int async)
{
  // TODO
  return 0;
}

EHCIQueue *bx_usb_ehci_c::state_fetchqh(int async)
{
  // TODO
  return NULL;
}

int bx_usb_ehci_c::state_fetchitd(int async)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::state_fetchsitd(int async)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::state_advqueue(EHCIQueue *q)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::state_fetchqtd(EHCIQueue *q)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::state_horizqh(EHCIQueue *q)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::fill_queue(EHCIPacket *p)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::state_execute(EHCIQueue *q)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::state_executing(EHCIQueue *q)
{
  // TODO
  return 0;
}

int bx_usb_ehci_c::state_writeback(EHCIQueue *q)
{
  // TODO
  return 0;
}

void bx_usb_ehci_c::advance_state(int async)
{
  EHCIQueue *q = NULL;
  int again;

  do {
    switch (BX_EHCI_THIS get_state(async)) {
      case EST_WAITLISTHEAD:
        again = BX_EHCI_THIS state_waitlisthead(async);
        break;

      case EST_FETCHENTRY:
        again = BX_EHCI_THIS state_fetchentry(async);
        break;

      case EST_FETCHQH:
        q = BX_EHCI_THIS state_fetchqh(async);
        if (q != NULL) {
          assert(q->async == async);
          again = 1;
        } else {
          again = 0;
        }
        break;

      case EST_FETCHITD:
        again = BX_EHCI_THIS state_fetchitd(async);
        break;

      case EST_FETCHSITD:
        again = BX_EHCI_THIS state_fetchsitd(async);
        break;

      case EST_ADVANCEQUEUE:
        again = BX_EHCI_THIS state_advqueue(q);
        break;

      case EST_FETCHQTD:
        again = BX_EHCI_THIS state_fetchqtd(q);
        break;

      case EST_HORIZONTALQH:
        again = BX_EHCI_THIS state_horizqh(q);
        break;

      case EST_EXECUTE:
        again = BX_EHCI_THIS state_execute(q);
        if (async) {
          BX_EHCI_THIS hub.async_stepdown = 0;
        }
        break;

      case EST_EXECUTING:
        assert(q != NULL);
        if (async) {
          BX_EHCI_THIS hub.async_stepdown = 0;
        }
        again = BX_EHCI_THIS state_executing(q);
        break;

      case EST_WRITEBACK:
        assert(q != NULL);
        again = BX_EHCI_THIS state_writeback(q);
        break;

      default:
        BX_ERROR(("Bad state!"));
        again = -1;
        break;
    }

    if (again < 0) {
        BX_ERROR(("processing error - resetting ehci HC"));
        BX_EHCI_THIS reset_hc();
        again = 0;
    }
  } while (again);
}

void bx_usb_ehci_c::advance_async_state(void)
{
  const int async = 1;

  switch (BX_EHCI_THIS get_state(async)) {
    case EST_INACTIVE:
      if (!BX_EHCI_THIS hub.op_regs.UsbCmd.ase) {
        break;
      }
      BX_EHCI_THIS set_state(async, EST_ACTIVE);
    // No break, fall through to ACTIVE

    case EST_ACTIVE:
      if (!BX_EHCI_THIS hub.op_regs.UsbCmd.ase) {
        BX_EHCI_THIS queues_rip_all(async);
        BX_EHCI_THIS set_state(async, EST_INACTIVE);
        break;
      }

      /* make sure guest has acknowledged the doorbell interrupt */
      /* TO-DO: is this really needed? */
      if (BX_EHCI_THIS hub.op_regs.UsbSts.inti & USBSTS_IAA) {
        BX_DEBUG(("IAA status bit still set."));
        break;
      }

      /* check that address register has been set */
      if (BX_EHCI_THIS hub.op_regs.AsyncListAddr == 0) {
        break;
      }

      BX_EHCI_THIS set_state(async, EST_WAITLISTHEAD);
      BX_EHCI_THIS advance_state(async);

      /* If the doorbell is set, the guest wants to make a change to the
       * schedule. The host controller needs to release cached data.
       * (section 4.8.2)
       */
      if (BX_EHCI_THIS hub.op_regs.UsbCmd.iaad) {
        /* Remove all unseen qhs from the async qhs queue */
        BX_EHCI_THIS queues_rip_unseen(async);
        BX_EHCI_THIS hub.op_regs.UsbCmd.iaad = 0;
        BX_EHCI_THIS raise_irq(USBSTS_IAA);
      }
      break;

    default:
      /* this should only be due to a developer mistake */
      BX_PANIC(("Bad asynchronous state %d. Resetting to active", BX_EHCI_THIS hub.astate));
  }
}

void bx_usb_ehci_c::advance_periodic_state(void)
{
  Bit32u entry;
  Bit32u list;
  const int async = 0;

    // 4.6

  switch (BX_EHCI_THIS get_state(async)) {
    case EST_INACTIVE:
      if (!(BX_EHCI_THIS hub.op_regs.FrIndex & 7) && BX_EHCI_THIS hub.op_regs.UsbCmd.pse) {
        BX_EHCI_THIS set_state(async, EST_ACTIVE);
        // No break, fall through to ACTIVE
      } else
        break;

    case EST_ACTIVE:
      if (!(BX_EHCI_THIS hub.op_regs.FrIndex & 7) && !BX_EHCI_THIS hub.op_regs.UsbCmd.pse) {
        BX_EHCI_THIS queues_rip_all(async);
        BX_EHCI_THIS set_state(async, EST_INACTIVE);
        break;
      }

      list = BX_EHCI_THIS hub.op_regs.PeriodicListBase & 0xfffff000;
      /* check that register has been set */
      if (list == 0) {
        break;
      }
      list |= ((BX_EHCI_THIS hub.op_regs.FrIndex & 0x1ff8) >> 1);

      DEV_MEM_READ_PHYSICAL(list, 4, (Bit8u*)&entry);

      BX_DEBUG(("PERIODIC state adv fr=%d.  [%08X] -> %08X",
                BX_EHCI_THIS hub.op_regs.FrIndex / 8, list, entry));
      BX_EHCI_THIS set_fetch_addr(async, entry);
      BX_EHCI_THIS set_state(async, EST_FETCHENTRY);
      BX_EHCI_THIS advance_state(async);
      BX_EHCI_THIS queues_rip_unused(async);
      break;

    default:
      /* this should only be due to a developer mistake */
      BX_PANIC(("Bad periodic state %d. Resetting to active", BX_EHCI_THIS hub.pstate));
  }
}

void bx_usb_ehci_c::update_frindex(int frames)
{
  int i;

  if (!BX_EHCI_THIS hub.op_regs.UsbCmd.rs) {
    return;
  }

  for (i = 0; i < frames; i++) {
    BX_EHCI_THIS hub.op_regs.FrIndex += 8;

    if (BX_EHCI_THIS hub.op_regs.FrIndex == 0x00002000) {
      BX_EHCI_THIS raise_irq(USBSTS_FLR);
    }

    if (BX_EHCI_THIS hub.op_regs.FrIndex == 0x00004000) {
      BX_EHCI_THIS raise_irq(USBSTS_FLR);
      BX_EHCI_THIS hub.op_regs.FrIndex = 0;
      if (BX_EHCI_THIS hub.usbsts_frindex >= 0x00004000) {
        BX_EHCI_THIS hub.usbsts_frindex -= 0x00004000;
      } else {
        BX_EHCI_THIS hub.usbsts_frindex = 0;
      }
    }
  }
}

void bx_usb_ehci_c::async_bh(void)
{
  // TODO
}

void bx_usb_ehci_c::ehci_frame_handler(void *this_ptr)
{
  bx_usb_ehci_c *class_ptr = (bx_usb_ehci_c *) this_ptr;
  class_ptr->ehci_frame_timer();
}

// Called once every 1.000 msec
void bx_usb_ehci_c::ehci_frame_timer(void)
{
  Bit64u t_now;
  Bit64u usec_elapsed;
  int frames, skipped_frames;
  int i;

  t_now = bx_pc_system.time_usec();
  usec_elapsed = t_now - BX_EHCI_THIS hub.last_run_usec;
  frames = usec_elapsed / FRAME_TIMER_USEC;

  if (BX_EHCI_THIS hub.op_regs.UsbCmd.rs || (BX_EHCI_THIS hub.pstate != EST_INACTIVE)) {
    BX_EHCI_THIS hub.async_stepdown = 0;

    if (frames > (int)BX_EHCI_THIS maxframes) {
      skipped_frames = frames - BX_EHCI_THIS maxframes;
      BX_EHCI_THIS update_frindex(skipped_frames);
      BX_EHCI_THIS hub.last_run_usec += FRAME_TIMER_USEC * skipped_frames;
      frames -= skipped_frames;
      BX_DEBUG(("WARNING - EHCI skipped %d frames", skipped_frames));
    }

    for (i = 0; i < frames; i++) {
      if (i >= MIN_FR_PER_TICK) {
        BX_EHCI_THIS commit_irq();
        if ((BX_EHCI_THIS hub.op_regs.UsbSts.inti & BX_EHCI_THIS hub.op_regs.UsbIntr) > 0){
          break;
        }
      }
      BX_EHCI_THIS update_frindex(1);
      BX_EHCI_THIS advance_periodic_state();
      BX_EHCI_THIS hub.last_run_usec += FRAME_TIMER_USEC;
    }
  } else {
    if (BX_EHCI_THIS hub.async_stepdown < BX_EHCI_THIS maxframes / 2) {
      BX_EHCI_THIS hub.async_stepdown++;
    }
    BX_EHCI_THIS update_frindex(frames);
    BX_EHCI_THIS hub.last_run_usec += FRAME_TIMER_USEC * frames;
  }

  if (BX_EHCI_THIS hub.op_regs.UsbCmd.rs || (BX_EHCI_THIS hub.astate != EST_INACTIVE)) {
    BX_EHCI_THIS advance_async_state();
  }

  BX_EHCI_THIS commit_irq();
  if (BX_EHCI_THIS hub.usbsts_pending) {
    BX_EHCI_THIS hub.async_stepdown = 0;
  }
}

void bx_usb_ehci_c::runtime_config_handler(void *this_ptr)
{
  bx_usb_ehci_c *class_ptr = (bx_usb_ehci_c *) this_ptr;
  class_ptr->runtime_config();
}

void bx_usb_ehci_c::runtime_config(void)
{
  int i;
  char pname[6];
  usbdev_type type = USB_DEV_TYPE_NONE;

  for (i = 0; i < USB_EHCI_PORTS; i++) {
    // device change support
    if ((BX_EHCI_THIS device_change & (1 << i)) != 0) {
      if (BX_EHCI_THIS hub.usb_port[i].device == NULL) {
        BX_INFO(("USB port #%d: device connect", i+1));
        sprintf(pname, "port%d", i + 1);
        init_device(i, (bx_list_c*)SIM->get_param(pname, SIM->get_param(BXPN_USB_EHCI)));
      } else {
        BX_INFO(("USB port #%d: device disconnect", i+1));
        if (BX_EHCI_THIS hub.usb_port[i].device != NULL) {
          type = BX_EHCI_THIS hub.usb_port[i].device->get_type();
        }
        set_connect_status(i, type, 0);
      }
      BX_EHCI_THIS device_change &= ~(1 << i);
    }
    // forward to connected device
    if (BX_EHCI_THIS hub.usb_port[i].device != NULL) {
      BX_EHCI_THIS hub.usb_port[i].device->runtime_config();
    }
  }
}

// pci configuration space read callback handler
Bit32u bx_usb_ehci_c::pci_read_handler(Bit8u address, unsigned io_len)
{
  Bit32u value = 0;

  for (unsigned i=0; i<io_len; i++) {
    value |= (BX_EHCI_THIS pci_conf[address+i] << (i*8));
  }

  if (io_len == 1)
    BX_DEBUG(("read  PCI register 0x%02X value 0x%02X (len=1)", address, value));
  else if (io_len == 2)
    BX_DEBUG(("read  PCI register 0x%02X value 0x%04X (len=2)", address, value));
  else if (io_len == 4)
    BX_DEBUG(("read  PCI register 0x%02X value 0x%08X (len=4)", address, value));

  return value;
}

// pci configuration space write callback handler
void bx_usb_ehci_c::pci_write_handler(Bit8u address, Bit32u value, unsigned io_len)
{
  Bit8u value8, oldval;
  bx_bool baseaddr_change = 0;

  if (((address >= 0x14) && (address <= 0x3b)) || (address > 0x80))
    return;

  for (unsigned i=0; i<io_len; i++) {
    value8 = (value >> (i*8)) & 0xFF;
    oldval = BX_EHCI_THIS pci_conf[address+i];
    switch (address+i) {
      case 0x04:
        value8 &= 0x06; // (bit 0 is read only for this card) (we don't allow port IO)
        BX_EHCI_THIS pci_conf[address+i] = value8;
        break;
      case 0x05: // disallowing write to command hi-byte
      case 0x06: // disallowing write to status lo-byte (is that expected?)
      case 0x0d: //
      case 0x3d: //
      case 0x3e: //
      case 0x3f: //
      case 0x60: //
        break;
      case 0x3c:
        if (value8 != oldval) {
          BX_INFO(("new irq line = %d", value8));
          BX_EHCI_THIS pci_conf[address+i] = value8;
        }
        break;
      case 0x10:  // low 8 bits of BAR are R/O
        value8 = 0x00;
      case 0x11:
      case 0x12:
      case 0x13:
        baseaddr_change |= (value8 != oldval);
        BX_EHCI_THIS pci_conf[address+i] = value8;
        break;
      case 0x2c:
      case 0x2d:
      case 0x2e:
      case 0x2f:
        if (BX_EHCI_THIS pci_conf[0x80] & 1) {
          BX_EHCI_THIS pci_conf[address+i] = value8;
        }
        break;
      case 0x61:
        value8 &= 0x3f;
      default:
        BX_EHCI_THIS pci_conf[address+i] = value8;
    }
  }
  if (baseaddr_change) {
    if (DEV_pci_set_base_mem(BX_EHCI_THIS_PTR, read_handler, write_handler,
                             &BX_EHCI_THIS pci_base_address[0],
                             &BX_EHCI_THIS pci_conf[0x10],
                             IO_SPACE_SIZE)) {
      BX_INFO(("new base address: 0x%08x", BX_EHCI_THIS pci_base_address[0]));
    }
  }

  if (io_len == 1)
    BX_DEBUG(("write PCI register 0x%02X value 0x%02X (len=1)", address, value));
  else if (io_len == 2)
    BX_DEBUG(("write PCI register 0x%02X value 0x%04X (len=2)", address, value));
  else if (io_len == 4)
    BX_DEBUG(("write PCI register 0x%02X value 0x%08X (len=4)", address, value));
}

// USB runtime parameter handler
const char *bx_usb_ehci_c::usb_param_handler(bx_param_string_c *param, int set,
                                             const char *oldval, const char *val, int maxlen)
{
  int portnum;

  if (set) {
    portnum = atoi((param->get_parent())->get_name()+4) - 1;
    bx_bool empty = ((strlen(val) == 0) || (!strcmp(val, "none")));
    if ((portnum >= 0) && (portnum < USB_EHCI_PORTS)) {
      if (empty && (BX_EHCI_THIS hub.usb_port[portnum].device != NULL)) {
        BX_EHCI_THIS device_change |= (1 << portnum);
      } else if (!empty && (BX_EHCI_THIS hub.usb_port[portnum].device == NULL)) {
        BX_EHCI_THIS device_change |= (1 << portnum);
      }
    } else {
      BX_PANIC(("usb_param_handler called with unexpected parameter '%s'", param->get_name()));
    }
  }
  return val;
}

#endif // BX_SUPPORT_PCI && BX_SUPPORT_USB_EHCI
