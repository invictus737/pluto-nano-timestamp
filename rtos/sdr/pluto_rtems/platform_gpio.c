/*
 * no-OS GPIO platform ops for the Zynq-7000 PS GPIO controller (0xe000a000) on RTEMS.
 * Numbering as the Linux zynq-gpio driver / the DT "gpio0 N": bank 0 = MIO 0..31, bank 1 =
 * MIO 32..53, bank 2 = EMIO 54..85, bank 3 = EMIO 86..117. Writes go through MASK_DATA so other
 * pins of the bank (USB PHY reset on bank 1, ...) are never touched.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <errno.h>

#include "no_os_alloc.h"
#include "no_os_error.h"
#include "platform_rtems.h"
#include "pluto_regs.h"

#define GPIO_REG(off) (*(volatile uint32_t *)(uintptr_t)(PLUTO_GPIO_BASE + (off)))
#define MASK_DATA_LSW(b) (0x000u + 8u * (b))
#define MASK_DATA_MSW(b) (0x004u + 8u * (b))
#define DATA_RO(b) (0x060u + 4u * (b))
#define DIRM(b) (0x204u + 0x40u * (b))
#define OEN(b) (0x208u + 0x40u * (b))

static int bank_pin(unsigned n, unsigned *bank, unsigned *pin)
{
  if (n < 32) { *bank = 0; *pin = n; }
  else if (n < 54) { *bank = 1; *pin = n - 32; }
  else if (n < 86) { *bank = 2; *pin = n - 54; }
  else if (n < 118) { *bank = 3; *pin = n - 86; }
  else return -1;
  return 0;
}

static void set_value(unsigned bank, unsigned pin, int value)
{
  uint32_t bit = pin & 15u;
  uint32_t v = ((~(1u << bit) & 0xffffu) << 16) | ((value ? 1u : 0u) << bit);
  GPIO_REG(pin < 16 ? MASK_DATA_LSW(bank) : MASK_DATA_MSW(bank)) = v;
}

void zynq_gpio_output(unsigned number, int value)
{
  unsigned bank, pin;
  if (bank_pin(number, &bank, &pin))
    return;
  set_value(bank, pin, value); /* value first: no glitch when the driver turns on */
  GPIO_REG(DIRM(bank)) |= 1u << pin;
  GPIO_REG(OEN(bank)) |= 1u << pin;
}

int zynq_gpio_get(unsigned number)
{
  unsigned bank, pin;
  if (bank_pin(number, &bank, &pin))
    return -1;
  return (GPIO_REG(DATA_RO(bank)) >> pin) & 1u;
}

static int32_t zgpio_get(struct no_os_gpio_desc **desc, const struct no_os_gpio_init_param *param)
{
  unsigned bank, pin;
  if (!param || param->number < 0 || bank_pin((unsigned)param->number, &bank, &pin))
    return -EINVAL;
  struct no_os_gpio_desc *d = no_os_calloc(1, sizeof(*d));
  if (!d)
    return -ENOMEM;
  d->port = param->port;
  d->number = param->number;
  d->pull = param->pull;
  d->platform_ops = param->platform_ops;
  d->extra = param->extra;
  *desc = d;
  return 0;
}

static int32_t zgpio_get_optional(struct no_os_gpio_desc **desc,
                                  const struct no_os_gpio_init_param *param)
{
  if (!param || param->number < 0) {
    *desc = NULL;
    return 0;
  }
  return zgpio_get(desc, param);
}

static int32_t zgpio_remove(struct no_os_gpio_desc *desc)
{
  no_os_free(desc);
  return 0;
}

static int32_t zgpio_direction_input(struct no_os_gpio_desc *desc)
{
  unsigned bank, pin;
  if (!desc || bank_pin((unsigned)desc->number, &bank, &pin))
    return -EINVAL;
  GPIO_REG(OEN(bank)) &= ~(1u << pin);
  GPIO_REG(DIRM(bank)) &= ~(1u << pin);
  return 0;
}

static int32_t zgpio_direction_output(struct no_os_gpio_desc *desc, uint8_t value)
{
  if (!desc)
    return -EINVAL;
  zynq_gpio_output((unsigned)desc->number, value);
  return 0;
}

static int32_t zgpio_get_direction(struct no_os_gpio_desc *desc, uint8_t *direction)
{
  unsigned bank, pin;
  if (!desc || bank_pin((unsigned)desc->number, &bank, &pin))
    return -EINVAL;
  *direction = (GPIO_REG(DIRM(bank)) >> pin) & 1u ? NO_OS_GPIO_OUT : NO_OS_GPIO_IN;
  return 0;
}

static int32_t zgpio_set_value(struct no_os_gpio_desc *desc, uint8_t value)
{
  unsigned bank, pin;
  if (!desc || bank_pin((unsigned)desc->number, &bank, &pin))
    return -EINVAL;
  set_value(bank, pin, value);
  return 0;
}

static int32_t zgpio_get_value(struct no_os_gpio_desc *desc, uint8_t *value)
{
  if (!desc)
    return -EINVAL;
  int v = zynq_gpio_get((unsigned)desc->number);
  if (v < 0)
    return -EINVAL;
  *value = (uint8_t)v;
  return 0;
}

const struct no_os_gpio_platform_ops pluto_zynq_gpio_ops = {
  .gpio_ops_get = zgpio_get,
  .gpio_ops_get_optional = zgpio_get_optional,
  .gpio_ops_remove = zgpio_remove,
  .gpio_ops_direction_input = zgpio_direction_input,
  .gpio_ops_direction_output = zgpio_direction_output,
  .gpio_ops_get_direction = zgpio_get_direction,
  .gpio_ops_set_value = zgpio_set_value,
  .gpio_ops_get_value = zgpio_get_value,
};
