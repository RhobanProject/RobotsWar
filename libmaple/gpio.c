/******************************************************************************
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
 *****************************************************************************/

/**
 * @brief GPIO initialization routine
 */

#include "gpio.h"
#include "rcc.h"

/*
 * GPIO devices
 */

gpio_dev gpioa = {
    .regs   = GPIOA_BASE,
    .clk_id = RCC_GPIOA
};
gpio_dev* const GPIOA = &gpioa;

gpio_dev gpiob = {
    .regs   = GPIOB_BASE,
    .clk_id = RCC_GPIOB
};
gpio_dev* const GPIOB = &gpiob;

gpio_dev gpioc = {
    .regs   = GPIOC_BASE,
    .clk_id = RCC_GPIOC
};
gpio_dev* const GPIOC = &gpioc;

gpio_dev gpiod = {
    .regs   = GPIOD_BASE,
    .clk_id = RCC_GPIOD
};
gpio_dev* const GPIOD = &gpiod;

#ifdef STM32_HIGH_DENSITY
gpio_dev gpioe = {
    .regs   = GPIOE_BASE,
    .clk_id = RCC_GPIOE
};
gpio_dev* const GPIOE = &gpioe;

gpio_dev gpiof = {
    .regs   = GPIOF_BASE,
    .clk_id = RCC_GPIOF
};
gpio_dev* const GPIOF = &gpiof;

gpio_dev gpiog = {
    .regs   = GPIOG_BASE,
    .clk_id = RCC_GPIOG
};
gpio_dev* const GPIOG = &gpiog;
#endif

/*
 * GPIO convenience routines
 */

/**
 * Initialize a GPIO device.
 *
 * Enables the clock for and resets the given device.
 *
 * @param dev GPIO device to initialize.
 */
void gpio_init(gpio_dev *dev) {
    rcc_clk_enable(dev->clk_id);
    rcc_reset_dev(dev->clk_id);
}

/**
 * Initialize and reset all available GPIO devices.
 */
void gpio_init_all(void) {
    gpio_init(GPIOA);
    gpio_init(GPIOB);
    gpio_init(GPIOC);
    gpio_init(GPIOD);
#ifdef STM32_HIGH_DENSITY
    gpio_init(GPIOE);
    gpio_init(GPIOF);
    gpio_init(GPIOG);
#endif
}

/**
 * Set the mode of a GPIO pin.
 *
 * @param dev GPIO device.
 * @param pin Pin on the device whose mode to set, 0--15.
 * @param mode General purpose or alternate function mode to set the pin to.
 * @see gpio_pin_mode
 */
void gpio_set_mode(gpio_dev *dev, uint8 pin, gpio_pin_mode mode) {
    gpio_reg_map *regs = dev->regs;
    __io uint32 *cr = &regs->CRL + (pin >> 3);
    uint32 shift = (pin & 0x7) * 4;
    uint32 tmp = *cr;

    tmp &= ~(0xF << shift);
    tmp |= (mode == GPIO_INPUT_PU ? GPIO_INPUT_PD : mode) << shift;
    *cr = tmp;

    if (mode == GPIO_INPUT_PD) {
        regs->ODR &= ~BIT(pin);
    } else if (mode == GPIO_INPUT_PU) {
        regs->ODR |= BIT(pin);
    }
}

/*
 * AFIO
 */

/**
 * Initialize the AFIO clock, and reset the AFIO registers.
 */
void afio_init(void) {
    rcc_clk_enable(RCC_AFIO);
    rcc_reset_dev(RCC_AFIO);
}

#define AFIO_EXTI_SEL_MASK 0xF

/**
 * Select a source input for an external interrupt.
 *
 * @param exti      External interrupt.  One of: AFIO_EXTI_0,
 *                  AFIO_EXTI_1, ..., AFIO_EXTI_15.
 * @param gpio_port Port which contains pin to use as source input.
 *                  One of: AFIO_EXTI_PA, AFIO_EXTI_PB, AFIO_EXTI_PC,
 *                  AFIO_EXTI_PD, and, on high density devices,
 *                  AFIO_EXTI_PE, AFIO_EXTI_PF, AFIO_EXTI_PG.
 * @see exti_port
 */
void afio_exti_select(afio_exti_num exti, afio_exti_port gpio_port) {
    __io uint32 *exti_cr = &AFIO_BASE->EXTICR1 + exti / 4;
    uint32 shift = 4 * (exti % 4);
    uint32 cr = *exti_cr;

    cr &= ~(AFIO_EXTI_SEL_MASK << shift);
    cr |= gpio_port << shift;
    *exti_cr = cr;
}
