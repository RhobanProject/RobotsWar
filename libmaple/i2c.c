/* *****************************************************************************
 * The MIT License
 *
 * Copyright (c) 2010 Perry Hung.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 * ****************************************************************************/

/**
 *  @brief 
 *  TODO: Rejigger hard fault handler and error throbs to turn off all
 *  interrupts and jump to error throb to reenable usb bootloader
 */

#include "libmaple.h"
#include "rcc.h"
#include "nvic.h"
#include "gpio.h"
#include "nvic.h"
#include "i2c.h"
#include "string.h"

/* 2/17 Started 10pm-4am
 * 2/19 Started 7pm-2am
 * 2/20 Started 7pm-7pm
 * 2/23 Started 8pm */

static inline int32 wait_for_state_change(i2c_dev *dev, i2c_state state);

i2c_dev i2c_dev1 = {
    .regs         = (i2c_reg_map*)I2C1_BASE,
    .gpio_port    = GPIOB_BASE,
    .sda_pin      = 7,
    .scl_pin      = 6,
    .clk_line     = RCC_I2C1,
    .ev_nvic_line = NVIC_I2C1_EV,
    .er_nvic_line = NVIC_I2C1_ER,
    .state        = I2C_STATE_IDLE
};

/**
 * @brief IRQ handler for i2c master. Handles transmission/reception.
 * @param dev i2c device
 */

void delay_us(uint32 us) {
    /* So (2^32)/12 micros max, or less than 6 minutes */
    us *= 12;

    /* fudge for function call overhead */
    us--;
    asm volatile("   mov r0, %[us]          \n\t"
            "1: subs r0, #1            \n\t"
            "   bhi 1b                 \n\t"
            :
            : [us] "r" (us)
            : "r0");
}
static inline void debug_toggle(uint32 delay) {
    gpio_write_bit(GPIOA_BASE, 5, 1);
    delay_us(delay);
    gpio_write_bit(GPIOA_BASE, 5, 0);
}

struct crumb {
    uint32 event;
    uint32 sr1;
    uint32 sr2;
};

static struct crumb crumbs[100];
static uint32 cur_crumb = 0;

static inline void leave_big_crumb(uint32 event, uint32 sr1, uint32 sr2) {
    if (cur_crumb < 100) {
        struct crumb *crumb = &crumbs[cur_crumb++];
        crumb->event = event;
        crumb->sr1 = sr1;
        crumb->sr2 = sr2;
    }
}

enum {
    IRQ_ENTRY           = 1,
    TXE_ONLY            = 2,
    TXE_BTF             = 3,
    STOP_SENT           = 4,
    TEST                = 5,
    RX_ADDR_START       = 6,
    RX_ADDR_STOP        = 7,
    RXNE_ONLY           = 8,
    RXNE_SENDING        = 9,
    RXNE_START_SENT     = 10,
    RXNE_STOP_SENT      = 11,
    RXNE_DONE           = 12,
};

static void i2c_irq_handler(i2c_dev *dev) {
    i2c_msg *msg = dev->msg;

    uint8 read = msg->flags & I2C_MSG_READ;

    uint32 sr1 = dev->regs->SR1;
    uint32 sr2 = dev->regs->SR2;
    leave_big_crumb(IRQ_ENTRY, sr1, sr2);

    /*
     * EV5: Start condition sent
     */
    if (sr1 & I2C_SR1_SB) {
        msg->xferred = 0;
        i2c_enable_irq(dev->regs, I2C_IRQ_BUFFER);

        /*
         * Master receiver
         */
        if (read) {
            i2c_enable_ack(dev->regs);
        }
        i2c_send_slave_addr(dev->regs, msg->addr, read);
        sr1 = sr2 = 0;
    }

    /*
     * EV6: Slave address sent
     */
    if (sr1 & I2C_SR1_ADDR) {
        /*
         * Special case event EV6_1 for master receiver.
         * Generate NACK and restart/stop condition after ADDR
         * is cleared.
         */
        if (read) {
            if (msg->length == 1) {
                i2c_disable_ack(dev->regs);
                dev->msgs_left--;
                if (dev->msgs_left) {
                    i2c_start_condition(dev->regs);
                    leave_big_crumb(RX_ADDR_START, 0, 0);
                } else {
                    i2c_stop_condition(dev->regs);
                    leave_big_crumb(RX_ADDR_STOP, 0, 0);
                }
            }
        } else {
            /*
             * Master transmitter: write first byte to fill shift register.
             * We should get another TXE interrupt immediately to fill DR again.
             */
            i2c_write(dev->regs, msg->data[msg->xferred++]);
        }
        sr1 = sr2 = 0;
    }

    /*
     * EV8: Master transmitter
     * Transmit buffer empty, but we haven't finished transmitting the last
     * byte written.
     */
    if ((sr1 & I2C_SR1_TXE) && !(sr1 & I2C_SR1_BTF)) {
        leave_big_crumb(TXE_ONLY, 0, 0);
        if (dev->msgs_left) {
            i2c_write(dev->regs, msg->data[msg->xferred++]);
            if (msg->xferred == msg->length) {
                /*
                 * End of this message. Turn off TXE/RXNE and wait for
                 * BTF to send repeated start or stop condition.
                 */
                i2c_disable_irq(dev->regs, I2C_IRQ_BUFFER);
                dev->msgs_left--;
            }
        } else {
            /*
             * This should be impossible...
             */
            throb();
        }
        sr1 = sr2 = 0;
    }

    /*
     * EV8_2: Master transmitter
     * Last byte sent, program repeated start/stop
     */
    if ((sr1 & I2C_SR1_TXE) && (sr1 & I2C_SR1_BTF)) {
        leave_big_crumb(TXE_BTF, 0, 0);
        if (dev->msgs_left) {
            leave_big_crumb(TEST, 0, 0);
            /*
             * Repeated start insanity: We can't disable ITEVTEN or else SB
             * won't interrupt, but if we don't disable ITEVTEN, BTF will
             * continually interrupt us. What the fuck ST?
             */
            i2c_start_condition(dev->regs);
            while (!(dev->regs->SR1 & I2C_SR1_SB))
                ;
            dev->msg++;
        } else {
            i2c_stop_condition(dev->regs);
            /*
             * Turn off event interrupts to keep BTF from firing until the end
             * of the stop condition. Why on earth they didn't have a start/stop
             * condition request clear BTF is beyond me.
             */
            i2c_disable_irq(dev->regs, I2C_IRQ_EVENT);
            leave_big_crumb(STOP_SENT, 0, 0);
            dev->state = I2C_STATE_XFER_DONE;
        }
        sr1 = sr2 = 0;
    }

    /*
     * EV7: Master Receiver
     */
    if (sr1 & I2C_SR1_RXNE) {
        leave_big_crumb(RXNE_ONLY, 0, 0);
        msg->data[msg->xferred++] = dev->regs->DR;

        /*
         * EV7_1: Second to last byte in the reception? Set NACK and generate
         * stop/restart condition in time for the last byte. We'll get one more
         * RXNE interrupt before shutting things down.
         */
        if (msg->xferred == (msg->length - 1)) {
            i2c_disable_ack(dev->regs);
            if (dev->msgs_left > 2) {
                i2c_start_condition(dev->regs);
                leave_big_crumb(RXNE_START_SENT, 0, 0);
            } else {
                i2c_stop_condition(dev->regs);
                leave_big_crumb(RXNE_STOP_SENT, 0, 0);
            }
        } else if (msg->xferred == msg->length) {
            dev->msgs_left--;
            if (dev->msgs_left == 0) {
                /*
                 * We're done.
                 */
                leave_big_crumb(RXNE_DONE, 0, 0);
                dev->state = I2C_STATE_XFER_DONE;
            } else {
                dev->msg++;
            }
        }
    }
}

void I2C1_EV_IRQHandler(void) {
   i2c_dev *dev = I2C1;
   i2c_irq_handler(dev);
}

static void i2c_bus_reset(i2c_dev *dev) {
    /* Release both lines */
    gpio_write_bit(dev->gpio_port, dev->scl_pin, 1);
    gpio_write_bit(dev->gpio_port, dev->sda_pin, 1);
    gpio_set_mode(dev->gpio_port, dev->scl_pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_mode(dev->gpio_port, dev->sda_pin, GPIO_MODE_OUTPUT_OD);

    /*
     * Make sure the bus is free by clocking it until any slaves release the
     * bus.
     */
    while (!gpio_read_bit(dev->gpio_port, dev->sda_pin)) {
        /* Wait for any clock stretching to finish */
        while (!gpio_read_bit(dev->gpio_port, dev->scl_pin))
            ;
        delay_us(10);

        /* Pull low */
        gpio_write_bit(dev->gpio_port, dev->scl_pin, 0);
        delay_us(10);

        /* Release high again */
        gpio_write_bit(dev->gpio_port, dev->scl_pin, 1);
        delay_us(10);
    }

    /* Generate start then stop condition */
    gpio_write_bit(dev->gpio_port, dev->sda_pin, 0);
    delay_us(10);
    gpio_write_bit(dev->gpio_port, dev->scl_pin, 0);
    delay_us(10);
    gpio_write_bit(dev->gpio_port, dev->scl_pin, 1);
    delay_us(10);
    gpio_write_bit(dev->gpio_port, dev->sda_pin, 1);
}

/**
 * @brief Initialize an i2c device as bus master
 * @param device to enable
 * @param flags bitwise or of the following I2C options:
 *      I2C_FAST_MODE: 400 khz operation
 *      I2C_10BIT_ADDRESSING: Enable 10-bit addressing
 */
void i2c_master_enable(i2c_dev *dev, uint32 flags) {
#define STANDARD_CCR           (PCLK1/(100000*2))
#define STANDARD_TRISE         37
    /* Reset the bus. Clock out any hung slaves. */
    i2c_bus_reset(dev);

    /* Turn on clock and set GPIO modes */
    rcc_reset_dev(dev->clk_line);
    rcc_clk_enable(dev->clk_line);
    gpio_set_mode(dev->gpio_port, dev->sda_pin, GPIO_MODE_AF_OUTPUT_OD);
    gpio_set_mode(dev->gpio_port, dev->scl_pin, GPIO_MODE_AF_OUTPUT_OD);

    /* I2C1 and I2C2 are fed from APB1, clocked at 36MHz */
    i2c_set_input_clk(dev->regs, 36);

    /* 100 khz only for now */
    i2c_set_clk_control(dev->regs, STANDARD_CCR);

    /*
     * Set scl rise time, standard mode for now.
     * Max rise time in standard mode: 1000 ns
     * Max rise time in fast mode: 300ns
     */
    i2c_set_trise(dev->regs, STANDARD_TRISE);

    /* Enable event and buffer interrupts */
    nvic_irq_enable(dev->ev_nvic_line);
    i2c_enable_irq(dev->regs, I2C_IRQ_EVENT | I2C_IRQ_BUFFER);

    /*
     * Important STM32 Errata:
     *
     * See STM32F10xx8 and STM32F10xxB Errata sheet (Doc ID 14574 Rev 8),
     * Section 2.11.1, 2.11.2.
     *
     * 2.11.1:
     * When the EV7, EV7_1, EV6_1, EV6_3, EV2, EV8, and EV3 events are not
     * managed before the current byte is being transferred, problems may be
     * encountered such as receiving an extra byte, reading the same data twice
     * or missing data.
     *
     * 2.11.2:
     * In Master Receiver mode, when closing the communication using
     * method 2, the content of the last read data can be corrupted.
     *
     * If the user software is not able to read the data N-1 before the STOP
     * condition is generated on the bus, the content of the shift register
     * (data N) will be corrupted. (data N is shifted 1-bit to the left).
     *
     * ----------------------------------------------------------------------
     *
     * In order to ensure that events are not missed, the i2c interrupt must
     * not be preempted. We set the i2c interrupt priority to be the highest
     * interrupt in the system (priority level 0). All other interrupts have
     * been initialized to priority level 16. See nvic_init().
     */
    nvic_irq_set_priority(dev->ev_nvic_line, 0);

    /* Make it go! */
    i2c_peripheral_enable(dev->regs);
}


int32 i2c_master_xfer(i2c_dev *dev, i2c_msg *msgs, uint16 num) {
    int32 rc;
    static int times = 0;

    dev->msg = msgs;
    dev->msgs_left = num;

    memset(crumbs, 0, sizeof crumbs);

    dev->regs->CR2 |= I2C_CR2_ITEVTEN;

    /* Is this necessary? */
    while (dev->regs->SR2 & I2C_SR2_BUSY)
        ;

    dev->state = I2C_STATE_BUSY;

    i2c_start_condition(dev->regs);
    rc = wait_for_state_change(dev, I2C_STATE_XFER_DONE);
    if (rc < 0) {
        goto out;
    }

    dev->state = I2C_STATE_IDLE;
    rc = num;
out:
    return rc;
}

static inline int32 wait_for_state_change(i2c_dev *dev, i2c_state state) {
    int32 rc;
    i2c_state tmp;

    while (1) {
        tmp = dev->state;
        if ((tmp == state) || (tmp == I2C_STATE_ERROR)) {
            return (tmp == I2C_STATE_ERROR) ? -1 : 0;
        }
    }
}


/*
 * Low level register twiddling functions
 */

/**
 * @brief turn on an i2c peripheral
 * @param map i2c peripheral register base
 */
void i2c_peripheral_enable(i2c_reg_map *regs) {
    regs->CR1 |= I2C_CR1_PE;
}

/**
 * @brief turn off an i2c peripheral
 * @param map i2c peripheral register base
 */
void i2c_peripheral_disable(i2c_reg_map *regs) {
    regs->CR1 &= ~I2C_CR1_PE;
}

/**
 * @brief Set input clock frequency, in mhz
 * @param device to configure
 * @param freq frequency in megahertz (2-36)
 */
void i2c_set_input_clk(i2c_reg_map *regs, uint32 freq) {
    uint32 cr2 = regs->CR2;
    cr2 &= ~I2C_CR2_FREQ;
    cr2 |= freq;
    regs->CR2 = freq;
}

void i2c_set_clk_control(i2c_reg_map *regs, uint32 val) {
    uint32 ccr = regs->CCR;
    ccr &= ~I2C_CCR_CCR;
    ccr |= val;
    regs->CCR = ccr;
}

void i2c_set_fast_mode(i2c_reg_map *regs) {
    regs->CCR |= I2C_CCR_FS;
}

void i2c_set_standard_mode(i2c_reg_map *regs) {
    regs->CCR &= ~I2C_CCR_FS;
}

/**
 * @brief Set SCL rise time
 * @param
 */

void i2c_set_trise(i2c_reg_map *regs, uint32 trise) {
    regs->TRISE = trise;
}

void i2c_start_condition(i2c_reg_map *regs) {
    regs->CR1 |= I2C_CR1_START;
}

void i2c_stop_condition(i2c_reg_map *regs) {
    regs->CR1 |= I2C_CR1_STOP;
}

void i2c_send_slave_addr(i2c_reg_map *regs, uint32 addr, uint32 rw) {
    regs->DR = (addr << 1) | rw;
}

void i2c_enable_irq(i2c_reg_map *regs, uint32 irqs) {
    regs->CR2 |= irqs;
}

void i2c_disable_irq(i2c_reg_map *regs, uint32 irqs) {
    regs->CR2 &= ~irqs;
}

void i2c_enable_ack(i2c_reg_map *regs) {
    regs->CR1 |= I2C_CR1_ACK;
}

void i2c_disable_ack(i2c_reg_map *regs) {
    regs->CR1 &= ~I2C_CR1_ACK;
}



