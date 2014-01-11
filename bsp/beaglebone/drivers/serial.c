/*
 * File      : serial.c
 * This file is part of RT-Thread RTOS
 * COPYRIGHT (C) 2013, RT-Thread Development Team
 *
 * The license and distribution terms for this file may be
 * found in the file LICENSE in this distribution or at
 * http://www.rt-thread.org/license/LICENSE
 *
 * Change Logs:
 * Date           Author       Notes
 * 2013-07-06     Bernard    the first version
 * 2014-01-11     RTsien     add definitions of UART0 to UART5
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>

#include <am33xx.h>
#include <interrupt.h>
#include "serial.h"
#include "serial_reg.h"

struct am33xx_uart
{
    unsigned long base;
    int irq;
};

static void am33xx_uart_isr(int irqno, void* param)
{
    rt_uint32_t iir;
    struct am33xx_uart* uart;
    struct rt_serial_device *serial;

    serial = (struct rt_serial_device*)param;
    uart = (struct am33xx_uart *)serial->parent.user_data;

    iir = UART_IIR_REG(uart->base);

	if ((iir & (0x02 << 1)) || (iir & (0x6 << 1)))
    {
        rt_hw_serial_isr(serial);
    }
}

#define NOT_IMPLEMENTED() RT_ASSERT(0)

static rt_err_t am33xx_configure(struct rt_serial_device *serial, struct serial_configure *cfg)
{
    struct am33xx_uart* uart;
    unsigned long base;

    RT_ASSERT(serial != RT_NULL);
    uart = (struct am33xx_uart *)serial->parent.user_data;
    RT_ASSERT(uart);
    base = uart->base;

#define __LCR UART_LCR_REG(base)

    if (cfg->data_bits == DATA_BITS_8)
        __LCR |= 3;
    else
        NOT_IMPLEMENTED();

    if (cfg->stop_bits == STOP_BITS_1)
        __LCR &= ~(1<<2);
    else
        __LCR |=  (1<<2);

    if (cfg->parity == PARITY_NONE)
        __LCR &= ~(1<<3);
    else
        __LCR |=  (1<<3);

    __LCR |=  (1<<7);
    if (cfg->baud_rate == BAUD_RATE_115200)
    {
        UART_DLL_REG(base) = 26;
        UART_DLH_REG(base) = 0;
    }
    else
    {
        NOT_IMPLEMENTED();
    }
    __LCR &= ~(1<<7);

    UART_MDR1_REG(base) = 0;
    UART_MDR2_REG(base) = 0;

#undef __LCR
    return RT_EOK;
}

static rt_err_t am33xx_control(struct rt_serial_device *serial, int cmd, void *arg)
{
    struct am33xx_uart* uart;

    RT_ASSERT(serial != RT_NULL);
    uart = (struct am33xx_uart *)serial->parent.user_data;

    switch (cmd)
    {
    case RT_DEVICE_CTRL_CLR_INT:
        /* disable rx irq */
        rt_hw_interrupt_mask(uart->irq);
        break;
    case RT_DEVICE_CTRL_SET_INT:
        /* enable rx irq */
        rt_hw_interrupt_umask(uart->irq);
        break;
    }

    return RT_EOK;
}

int printkc(char c)
{
    int base = 0xf9e09000;

    while (!(UART_LSR_REG(base) & 0x20));
    UART_THR_REG(base) = c;

    return 1;
}

static int am33xx_putc(struct rt_serial_device *serial, char c)
{
    struct am33xx_uart* uart;

    RT_ASSERT(serial != RT_NULL);
    uart = (struct am33xx_uart *)serial->parent.user_data;

    while (!(UART_LSR_REG(uart->base) & 0x20));
    UART_THR_REG(uart->base) = c;

    return 1;
}

static int am33xx_getc(struct rt_serial_device *serial)
{
    int ch;
    struct am33xx_uart* uart;

    RT_ASSERT(serial != RT_NULL);
    uart = (struct am33xx_uart *)serial->parent.user_data;

    ch = -1;
    if (UART_LSR_REG(uart->base) & 0x01)
    {
        ch = UART_RHR_REG(uart->base) & 0xff;
    }

    return ch;
}

static const struct rt_uart_ops am33xx_uart_ops =
{
    am33xx_configure,
    am33xx_control,
    am33xx_putc,
    am33xx_getc,
};

/* UART device driver structure */
struct serial_ringbuffer uart_int_rx[6];
struct am33xx_uart uart[6] =
{
    {UART0_BASE,UART0_INT},
    {UART1_BASE,UART1_INT},
    {UART2_BASE,UART2_INT},
    {UART3_BASE,UART3_INT},
    {UART4_BASE,UART4_INT},
    {UART5_BASE,UART5_INT}
};
struct rt_serial_device serial[6];

#define write_reg(base, value) *(int*)(base) = value
#define read_reg(base)         *(int*)(base)

#define PRM_PER_INTRANSLATION     (1 << 20)
#define PRM_PER_POWSTATEOFF       (0)
#define PRM_PER_PERMEMSTATEOFF    (0)

static void poweron_per_domain(void)
{
    unsigned long prcm_base;
    unsigned long prm_state;

    prcm_base = AM33XX_PRCM_REGS;

    /* wait for ongoing translations */
    for (prm_state = PRM_PER_PWRSTST_REG(prcm_base);
         prm_state & PRM_PER_INTRANSLATION;
         prm_state = PRM_PER_PWRSTST_REG(prcm_base))
        ;

    /* check power state */
    if ((prm_state & 0x03) == PRM_PER_POWSTATEOFF)
        /* power on PER domain */
        PRM_PER_PWRSTCTRL_REG(prcm_base) |= 0x3;

    /* check per mem state */
    if ((prm_state & 0x03) == PRM_PER_PERMEMSTATEOFF)
        /* power on PER domain */
        PRM_PER_PWRSTCTRL_REG(prcm_base) |= 0x3 << 25;

    while (PRM_PER_PWRSTST_REG(prcm_base) & PRM_PER_INTRANSLATION)
        ;
}

static void start_uart_clk(void)
{
    unsigned long prcm_base;

    prcm_base = AM33XX_PRCM_REGS;

    /* software forced wakeup */
    CM_PER_L4LS_CLKSTCTRL_REG(prcm_base) |= 0x2;

    /* Waiting for the L4LS clock */
    while (!(CM_PER_L4LS_CLKSTCTRL_REG(prcm_base) & (1<<8)))
        ;

    /* enable uart1 */
#ifdef RT_USING_UART1
    CM_PER_UART1_CLKCTRL_REG(prcm_base) |= 0x2;

    /* wait for uart1 clk */
    while ((CM_PER_UART1_CLKCTRL_REG(prcm_base) & (0x3<<16)) != 0)
        ;
#endif

#ifdef RT_USING_UART2
    CM_PER_UART2_CLKCTRL_REG(prcm_base) |= 0x2;

    /* wait for uart2 clk */
    while ((CM_PER_UART2_CLKCTRL_REG(prcm_base) & (0x3<<16)) != 0)
        ;
#endif

#ifdef RT_USING_UART3
    CM_PER_UART3_CLKCTRL_REG(prcm_base) |= 0x2;

    /* wait for uart3 clk */
    while ((CM_PER_UART3_CLKCTRL_REG(prcm_base) & (0x3<<16)) != 0)
        ;
#endif

#ifdef RT_USING_UART4
    CM_PER_UART4_CLKCTRL_REG(prcm_base) |= 0x2;

    /* wait for uart4 clk */
    while ((CM_PER_UART4_CLKCTRL_REG(prcm_base) & (0x3<<16)) != 0)
        ;
#endif

#ifdef RT_USING_UART5
    CM_PER_UART5_CLKCTRL_REG(prcm_base) |= 0x2;

    /* wait for uart5 clk */
    while ((CM_PER_UART5_CLKCTRL_REG(prcm_base) & (0x3<<16)) != 0)
        ;
#endif

    /* Waiting for the L4LS UART clock */
    while (!(CM_PER_L4LS_CLKSTCTRL_REG(prcm_base) & (1<<10)))
        ;
}

static void config_pinmux(void)
{
    unsigned long ctlm_base;

    ctlm_base = AM33XX_CTLM_REGS;

    /* make sure the pin mux is OK for uart */
#ifdef RT_USING_UART1
    REG32(ctlm_base + 0x800 + 0x180) = 0x20;
    REG32(ctlm_base + 0x800 + 0x184) = 0x00;
#endif

#ifdef RT_USING_UART2
    REG32(ctlm_base + 0x800 + 0x150) = 0x20;
    REG32(ctlm_base + 0x800 + 0x154) = 0x00;
#endif

#ifdef RT_USING_UART3
    REG32(ctlm_base + 0x800 + 0x164) = 0x01;
#endif

#ifdef RT_USING_UART4
    REG32(ctlm_base + 0x800 + 0x070) = 0x26;
    REG32(ctlm_base + 0x800 + 0x074) = 0x06;
#endif

#ifdef RT_USING_UART5
    REG32(ctlm_base + 0x800 + 0x0C4) = 0x24;
    REG32(ctlm_base + 0x800 + 0x0C0) = 0x04;
#endif


}

int rt_hw_serial_init(void)
{
    struct serial_configure config[6];

    poweron_per_domain();
    start_uart_clk();
    config_pinmux();

#ifdef RT_USING_UART0
    config[0].baud_rate = BAUD_RATE_115200;
    config[0].bit_order = BIT_ORDER_LSB;
    config[0].data_bits = DATA_BITS_8;
    config[0].parity    = PARITY_NONE;
    config[0].stop_bits = STOP_BITS_1;
    config[0].invert    = NRZ_NORMAL;

    serial[0].ops    = &am33xx_uart_ops;
    serial[0].int_rx = &uart_int_rx[0];
    serial[0].config = config[0];
    /* enable RX interrupt */
    UART_IER_REG(uart[0].base) = 0x01;
    /* install ISR */
    rt_hw_interrupt_install(uart[0].irq, am33xx_uart_isr, &serial[0], "uart0");
    rt_hw_interrupt_control(uart[0].irq, 0, 0);
    rt_hw_interrupt_mask(uart[0].irq);
    /* register UART0 device */
    rt_hw_serial_register(&serial[0], "uart0",
            RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM,
            &uart[0]);
#endif

#ifdef RT_USING_UART1
    config[1].baud_rate = BAUD_RATE_115200;
    config[1].bit_order = BIT_ORDER_LSB;
    config[1].data_bits = DATA_BITS_8;
    config[1].parity    = PARITY_NONE;
    config[1].stop_bits = STOP_BITS_1;
    config[1].invert    = NRZ_NORMAL;
   
    serial[1].ops    = &am33xx_uart_ops;
    serial[1].int_rx = &uart_int_rx[1];
    serial[1].config = config[1];
    /* enable RX interrupt */
    UART_IER_REG(uart[1].base) = 0x01;
    /* install ISR */
    rt_hw_interrupt_install(uart[1].irq, am33xx_uart_isr, &serial[1], "uart1");
    rt_hw_interrupt_control(uart[1].irq, 0, 0);
    rt_hw_interrupt_mask(uart[1].irq);
    /* register UART0 device */
    rt_hw_serial_register(&serial[1], "uart1",
            RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM,
            &uart[1]);
#endif

#ifdef RT_USING_UART2
    config[2].baud_rate = BAUD_RATE_115200;
    config[2].bit_order = BIT_ORDER_LSB;
    config[2].data_bits = DATA_BITS_8;
    config[2].parity    = PARITY_NONE;
    config[2].stop_bits = STOP_BITS_1;
    config[2].invert    = NRZ_NORMAL;
   
    serial[2].ops    = &am33xx_uart_ops;
    serial[2].int_rx = &uart_int_rx[2];
    serial[2].config = config[2];
    /* enable RX interrupt */
    UART_IER_REG(uart[2].base) = 0x01;
    /* install ISR */
    rt_hw_interrupt_install(uart[2].irq, am33xx_uart_isr, &serial[2], "uart2");
    rt_hw_interrupt_control(uart[2].irq, 0, 0);
    rt_hw_interrupt_mask(uart[2].irq);
    /* register UART2 device */
    rt_hw_serial_register(&serial[2], "uart2",
            RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM,
            &uart[2]);
#endif

#ifdef RT_USING_UART3
    config[3].baud_rate = BAUD_RATE_115200;
    config[3].bit_order = BIT_ORDER_LSB;
    config[3].data_bits = DATA_BITS_8;
    config[3].parity    = PARITY_NONE;
    config[3].stop_bits = STOP_BITS_1;
    config[3].invert    = NRZ_NORMAL;
    serial[3].ops    = &am33xx_uart_ops;
    serial[3].int_rx = &uart_int_rx[3];
    serial[3].config = config[3];
    /* enable RX interrupt */
    UART_IER_REG(uart[3].base) = 0x01;
    /* install ISR */
    rt_hw_interrupt_install(uart[3].irq, am33xx_uart_isr, &serial[3], "uart3");
    rt_hw_interrupt_control(uart[3].irq, 0, 0);
    rt_hw_interrupt_mask(uart[3].irq);
    /* register UART3 device */
    rt_hw_serial_register(&serial[3], "uart3",
            RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM,
            &uart[3]);
#endif

#ifdef RT_USING_UART4
    config[4].baud_rate = BAUD_RATE_115200;
    config[4].bit_order = BIT_ORDER_LSB;
    config[4].data_bits = DATA_BITS_8;
    config[4].parity    = PARITY_NONE;
    config[4].stop_bits = STOP_BITS_1;
    config[4].invert    = NRZ_NORMAL;
   
    serial[4].ops    = &am33xx_uart_ops;
    serial[4].int_rx = &uart_int_rx[4];
    serial[4].config = config[4];
    /* enable RX interrupt */
    UART_IER_REG(uart[4].base) = 0x01;
    /* install ISR */
    rt_hw_interrupt_install(uart[4].irq, am33xx_uart_isr, &serial[4], "uart4");
    rt_hw_interrupt_control(uart[4].irq, 0, 0);
    rt_hw_interrupt_mask(uart[4].irq);
    /* register UART4 device */
    rt_hw_serial_register(&serial[4], "uart4",
            RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM,
            &uart[4]);
#endif

#ifdef RT_USING_UART5
    config[5].baud_rate = BAUD_RATE_115200;
    config[5].bit_order = BIT_ORDER_LSB;
    config[5].data_bits = DATA_BITS_8;
    config[5].parity    = PARITY_NONE;
    config[5].stop_bits = STOP_BITS_1;
    config[5].invert    = NRZ_NORMAL;
  
    serial[5].ops    = &am33xx_uart_ops;
    serial[5].int_rx = &uart_int_rx[5];
    serial[5].config = config[5];
    /* enable RX interrupt */
    UART_IER_REG(uart[5].base) = 0x01;
    /* install ISR */
    rt_hw_interrupt_install(uart[5].irq, am33xx_uart_isr, &serial[5], "uart5");
    rt_hw_interrupt_control(uart[5].irq, 0, 0);
    rt_hw_interrupt_mask(uart[5].irq);
    /* register UART4 device */
    rt_hw_serial_register(&serial[5], "uart5",
            RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_STREAM,
            &uart[5]);
#endif

    return 0;
}
INIT_BOARD_EXPORT(rt_hw_serial_init);

