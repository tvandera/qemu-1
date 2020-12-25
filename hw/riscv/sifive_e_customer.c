/*
 * QEMU RISC-V Board Compatible with SiFive Freedom E SDK (Customer Ver.)
 *
 * Copyright (c) 2021 SiFive, Inc.
 *
 * Provides a board compatible with the SiFive Freedom E SDK for customers:
 *
 * 0) UART
 * 1) CLINT (Core Level Interruptor)
 * 2) PLIC (Platform Level Interrupt Controller)
 * 3) PRCI (Power, Reset, Clock, Interrupt)
 * 4) Registers emulated as RAM: AON, GPIO, QSPI, PWM
 * 5) Flash memory emulated as RAM
 *
 * The Mask ROM reset vector jumps to the flash payload at 0x2040_0000.
 * The OTP ROM and Flash boot code will be emulated in a future version.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/char/serial.h"
#include "hw/misc/unimp.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/sifive_e_customer.h"
#include "hw/riscv/boot.h"
#include "hw/char/sifive_uart.h"
#include "hw/intc/sifive_clint.h"
#include "hw/intc/sifive_plic.h"
#include "hw/misc/sifive_e_prci.h"
#include "hw/misc/sifive_test.h"
#include "hw/misc/sifive_remapper.h"
#include "hw/misc/sifive_l2pf.h"
#include "hw/misc/beu.h"
#include "hw/misc/sifive_beu.h"
#include "hw/misc/sifive_err_dev.h"
#include "chardev/char.h"
#include "sysemu/arch_init.h"
#include "sysemu/sysemu.h"

static const MemMapEntry sifive_e_memmap[] = {
    [SIFIVE_E_CUSTOMER_DEV_DEBUG] =    {        0x0,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_MROM] =     {     0x1000,     0x2000 },
    [SIFIVE_E_CUSTOMER_DEV_ERR_DEV] =  {     0x3000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_OTP] =      {    0x20000,     0x2000 },
    [SIFIVE_E_CUSTOMER_DEV_TEST] =     {   0x100000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_CLINT] =    {  0x2000000,    0x10000 },
    [SIFIVE_E_CUSTOMER_DEV_L2PF] =     {  0x2030000,     0x2000 },
    [SIFIVE_E_CUSTOMER_DEV_REMAPPER] = {  0x3000000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_BEU] =      {  0x4000000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_PLIC] =     {  0xc000000,  0x4000000 },
    [SIFIVE_E_CUSTOMER_DEV_AON] =      { 0x10000000,     0x8000 },
    [SIFIVE_E_CUSTOMER_DEV_PRCI] =     { 0x10008000,     0x8000 },
    [SIFIVE_E_CUSTOMER_DEV_OTP_CTRL] = { 0x10010000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_GPIO0] =    { 0x10012000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_UART0] =    { 0x10013000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_QSPI0] =    { 0x10014000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_PWM0] =     { 0x10015000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_UART1] =    { 0x10023000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_QSPI1] =    { 0x10024000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_PWM1] =     { 0x10025000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_QSPI2] =    { 0x10034000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_PWM2] =     { 0x10035000,     0x1000 },
    [SIFIVE_E_CUSTOMER_DEV_XIP] =      { 0x20000000, 0x20000000 },
    [SIFIVE_E_CUSTOMER_DEV_DTIM] =     { 0x80000000,     0x4000 }
};

static void sifive_e_customer_machine_init(MachineState *machine)
{
    const MemMapEntry *memmap = sifive_e_memmap;

    SiFiveECustomerState *s = RISCV_E_CUSTOMER_MACHINE(machine);
    MemoryRegion *sys_mem = get_system_memory();
    MemoryRegion *main_mem = g_new(MemoryRegion, 1);
    int i;

    /* Initialize SoC */
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_RISCV_E_CUSTOMER_SOC);
    qdev_realize(DEVICE(&s->soc), NULL, &error_abort);

    /* Data Tightly Integrated Memory */
    memory_region_init_ram(main_mem, NULL, "riscv.sifive.e.customer.ram",
        memmap[SIFIVE_E_CUSTOMER_DEV_DTIM].size, &error_fatal);
    memory_region_add_subregion(sys_mem,
        memmap[SIFIVE_E_CUSTOMER_DEV_DTIM].base, main_mem);

    /* Mask ROM reset vector */
    uint32_t reset_vec[4];

    if (s->revb) {
        reset_vec[1] = 0x200102b7;  /* 0x1004: lui     t0,0x20010 */
    } else {
        reset_vec[1] = 0x204002b7;  /* 0x1004: lui     t0,0x20400 */
    }
    reset_vec[2] = 0x00028067;      /* 0x1008: jr      t0 */

    reset_vec[0] = reset_vec[3] = 0;

    /* copy in the reset vector in little_endian byte order */
    for (i = 0; i < sizeof(reset_vec) >> 2; i++) {
        reset_vec[i] = cpu_to_le32(reset_vec[i]);
    }
    rom_add_blob_fixed_as("mrom.reset", reset_vec, sizeof(reset_vec),
                          memmap[SIFIVE_E_CUSTOMER_DEV_MROM].base, &address_space_memory);

    if (machine->kernel_filename) {
        riscv_load_kernel(machine->kernel_filename,
                          memmap[SIFIVE_E_CUSTOMER_DEV_DTIM].base, NULL);
    }
}

static bool sifive_e_machine_get_revb(Object *obj, Error **errp)
{
    SiFiveECustomerState *s = RISCV_E_CUSTOMER_MACHINE(obj);

    return s->revb;
}

static void sifive_e_machine_set_revb(Object *obj, bool value, Error **errp)
{
    SiFiveECustomerState *s = RISCV_E_CUSTOMER_MACHINE(obj);

    s->revb = value;
}

static void sifive_e_customer_machine_instance_init(Object *obj)
{
    SiFiveECustomerState *s = RISCV_E_CUSTOMER_MACHINE(obj);

    s->revb = false;
}

static void sifive_e_customer_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Board compatible with SiFive E SDK (Customer Ver.)";
    mc->init = sifive_e_customer_machine_init;
    mc->max_cpus = SIFIVE_E_CUSTOMER_CPUS_MAX;
    mc->default_cpu_type = SIFIVE_E_CPU;

    object_class_property_add_bool(oc, "revb", sifive_e_machine_get_revb,
                                   sifive_e_machine_set_revb);
    object_class_property_set_description(oc, "revb",
                                          "Set on to tell QEMU that it should model "
                                          "the revB HiFive1 board");
}

static const TypeInfo sifive_e_customer_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("sifive_e_customer"),
    .parent     = TYPE_MACHINE,
    .class_init = sifive_e_customer_machine_class_init,
    .instance_init = sifive_e_customer_machine_instance_init,
    .instance_size = sizeof(SiFiveECustomerState),
};

static void sifive_e_customer_machine_init_register_types(void)
{
    type_register_static(&sifive_e_customer_machine_typeinfo);
}

type_init(sifive_e_customer_machine_init_register_types)

static void sifive_e_customer_soc_init(Object *obj)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    SiFiveECustomerSoCState *s = RISCV_E_CUSTOMER_SOC(obj);
    g_autofree char *prop_name = NULL;

    object_initialize_child(obj, "cpus", &s->cpus, TYPE_RISCV_HART_ARRAY);
    object_property_set_int(OBJECT(&s->cpus), "num-harts", ms->smp.cpus,
                            &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "resetvec", 0x1004, &error_abort);
    object_initialize_child(obj, "riscv.sifive.e.customer.gpio0", &s->gpio,
                            TYPE_SIFIVE_GPIO);

    object_property_set_uint(OBJECT(&s->cpus), "len-nmi-interrupt-vector",
                             ms->smp.cpus, &error_abort);
    object_property_set_uint(OBJECT(&s->cpus), "len-nmi-exception-vector",
                             ms->smp.cpus, &error_abort);

    for (int i = 0; i < ms->smp.cpus; i++) {
        prop_name = g_strdup_printf("nmi-interrupt-vector[%d]", i);
        object_property_set_uint(OBJECT(&s->cpus), prop_name,
                                 SIFIVE_E_CUSTOMER_NMI_IRQVEC,
                                 &error_abort);
        prop_name = g_strdup_printf("nmi-exception-vector[%d]", i);
        object_property_set_uint(OBJECT(&s->cpus), prop_name,
                                 SIFIVE_E_CUSTOMER_NMI_EXCPVEC,
                                 &error_abort);
    }
}

static void sifive_e_customer_soc_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    const MemMapEntry *memmap = sifive_e_memmap;
    SiFiveECustomerSoCState *s = RISCV_E_CUSTOMER_SOC(dev);
    MemoryRegion *sys_mem = get_system_memory();

    object_property_set_str(OBJECT(&s->cpus), "cpu-type", ms->cpu_type,
                            &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->cpus), &error_abort);

    /* Mask ROM */
    memory_region_init_rom(&s->mask_rom, OBJECT(dev), "riscv.sifive.e.customer.mrom",
                           memmap[SIFIVE_E_CUSTOMER_DEV_MROM].size, &error_fatal);
    memory_region_add_subregion(sys_mem,
        memmap[SIFIVE_E_CUSTOMER_DEV_MROM].base, &s->mask_rom);

    /* MMIO */
    s->plic = sifive_plic_create(memmap[SIFIVE_E_CUSTOMER_DEV_PLIC].base,
        (char *)SIFIVE_E_CUSTOMER_PLIC_HART_CONFIG, 0,
        SIFIVE_E_CUSTOMER_PLIC_NUM_SOURCES,
        SIFIVE_E_CUSTOMER_PLIC_NUM_PRIORITIES,
        SIFIVE_E_CUSTOMER_PLIC_PRIORITY_BASE,
        SIFIVE_E_CUSTOMER_PLIC_PENDING_BASE,
        SIFIVE_E_CUSTOMER_PLIC_ENABLE_BASE,
        SIFIVE_E_CUSTOMER_PLIC_ENABLE_STRIDE,
        SIFIVE_E_CUSTOMER_PLIC_CONTEXT_BASE,
        SIFIVE_E_CUSTOMER_PLIC_CONTEXT_STRIDE,
        memmap[SIFIVE_E_CUSTOMER_DEV_PLIC].size);
    sifive_clint_create(memmap[SIFIVE_E_CUSTOMER_DEV_CLINT].base,
        memmap[SIFIVE_E_CUSTOMER_DEV_CLINT].size, 0, ms->smp.cpus,
        SIFIVE_SIP_BASE, SIFIVE_TIMECMP_BASE, SIFIVE_TIME_BASE,
        SIFIVE_CLINT_TIMEBASE_FREQ, false);
    create_unimplemented_device("riscv.sifive.e.customer.aon",
        memmap[SIFIVE_E_CUSTOMER_DEV_AON].base, memmap[SIFIVE_E_CUSTOMER_DEV_AON].size);
    sifive_e_prci_create(memmap[SIFIVE_E_CUSTOMER_DEV_PRCI].base);

    /* GPIO */

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }

    /* Map GPIO registers */
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, memmap[SIFIVE_E_CUSTOMER_DEV_GPIO0].base);

    /* Pass all GPIOs to the SOC layer so they are available to the board */
    qdev_pass_gpios(DEVICE(&s->gpio), dev, NULL);

    /* Connect GPIO interrupts to the PLIC */
    for (int i = 0; i < 32; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio), i,
                           qdev_get_gpio_in(DEVICE(s->plic),
                                            SIFIVE_E_CUSTOMER_GPIO0_IRQ0 + i));
    }

    sifive_uart_create(sys_mem, memmap[SIFIVE_E_CUSTOMER_DEV_UART0].base,
        serial_hd(0), qdev_get_gpio_in(DEVICE(s->plic), SIFIVE_E_CUSTOMER_UART0_IRQ));
    create_unimplemented_device("riscv.sifive.e.customer.qspi0",
        memmap[SIFIVE_E_CUSTOMER_DEV_QSPI0].base, memmap[SIFIVE_E_CUSTOMER_DEV_QSPI0].size);
    create_unimplemented_device("riscv.sifive.e.customer.pwm0",
        memmap[SIFIVE_E_CUSTOMER_DEV_PWM0].base, memmap[SIFIVE_E_CUSTOMER_DEV_PWM0].size);
    sifive_uart_create(sys_mem, memmap[SIFIVE_E_CUSTOMER_DEV_UART1].base,
        serial_hd(1), qdev_get_gpio_in(DEVICE(s->plic), SIFIVE_E_CUSTOMER_UART1_IRQ));
    create_unimplemented_device("riscv.sifive.e.customer.qspi1",
        memmap[SIFIVE_E_CUSTOMER_DEV_QSPI1].base, memmap[SIFIVE_E_CUSTOMER_DEV_QSPI1].size);
    create_unimplemented_device("riscv.sifive.e.customer.pwm1",
        memmap[SIFIVE_E_CUSTOMER_DEV_PWM1].base, memmap[SIFIVE_E_CUSTOMER_DEV_PWM1].size);
    create_unimplemented_device("riscv.sifive.e.customer.qspi2",
        memmap[SIFIVE_E_CUSTOMER_DEV_QSPI2].base, memmap[SIFIVE_E_CUSTOMER_DEV_QSPI2].size);
    create_unimplemented_device("riscv.sifive.e.customer.pwm2",
        memmap[SIFIVE_E_CUSTOMER_DEV_PWM2].base, memmap[SIFIVE_E_CUSTOMER_DEV_PWM2].size);

    /* Flash memory */
    memory_region_init_rom(&s->xip_mem, OBJECT(dev), "riscv.sifive.e.customer.xip",
                           memmap[SIFIVE_E_CUSTOMER_DEV_XIP].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[SIFIVE_E_CUSTOMER_DEV_XIP].base,
        &s->xip_mem);

    /* SiFive Test MMIO device */
    sifive_test_create(memmap[SIFIVE_E_CUSTOMER_DEV_TEST].base);

    /* TileLink Address Remapper */
    sifive_remapper_create(memmap[SIFIVE_E_CUSTOMER_DEV_REMAPPER].base,
                           SIFIVE_REMAPPER_VERSION_REVISITED,
                           SIFIVE_REMAPPER_MAX_ENTRIES_REVISED);

    /* L2 Stride Prefetcher (SPF) */
    for (int i = 0; i < ms->smp.cpus; i++) {
        sifive_l2pf_create(memmap[SIFIVE_E_CUSTOMER_DEV_L2PF].base +
                           i * SIFIVE_E_CUSTOMER_L2PF_STRIDE);
    }

    /* Bus error unit */
    DeviceState *beu = sifive_beu_create(memmap[SIFIVE_E_CUSTOMER_DEV_BEU].base,
                                         memmap[SIFIVE_E_CUSTOMER_DEV_BEU].size,
                                         false,
                                         qdev_get_gpio_in(DEVICE(s->plic),
                                             SIFIVE_E_CUSTOMER_BEU_IRQ),
                                         SIFIVE_E_CUSTOMER_BEU_RNMI, 0);

    /* Add and set BEU link directly to CPU 0. */
    object_property_add_link(OBJECT(&s->cpus.harts[0]), "buserror",
                             TYPE_BEU_INTERFACE,
                             (Object **)&s->cpus.harts[0].buserror,
                             object_property_allow_set_link,
                             OBJ_PROP_LINK_STRONG);
    object_property_set_link(OBJECT(&s->cpus.harts[0]), "buserror",
                             OBJECT(beu), errp);

    /* Error Device */
    sifive_err_dev_create(memmap[SIFIVE_E_CUSTOMER_DEV_ERR_DEV].base,
                          memmap[SIFIVE_E_CUSTOMER_DEV_ERR_DEV].size);
}

static void sifive_e_customer_soc_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = sifive_e_customer_soc_realize;
    /* Reason: Uses serial_hds in realize function, thus can't be used twice */
    dc->user_creatable = false;
}

static const TypeInfo sifive_e_customer_soc_type_info = {
    .name = TYPE_RISCV_E_CUSTOMER_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SiFiveECustomerSoCState),
    .instance_init = sifive_e_customer_soc_init,
    .class_init = sifive_e_customer_soc_class_init,
};

static void sifive_e_customer_soc_register_types(void)
{
    type_register_static(&sifive_e_customer_soc_type_info);
}

type_init(sifive_e_customer_soc_register_types)