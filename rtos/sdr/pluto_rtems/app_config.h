/*
 * ADI no-OS ad9361 build configuration for the Pluto Nano (AD9363A, 1R1T, CMOS, RTEMS).
 * The device type itself is selected at run time (AD9361_InitParam.dev_sel = ID_AD9363A).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CONFIG_H_
#define CONFIG_H_

#define HAVE_SPLIT_GAIN_TABLE 1
#define HAVE_TDD_SYNTH_TABLE 1

#define AD9361_DEVICE 0
#define AD9364_DEVICE 0
#define AD9363A_DEVICE 1

/* errors and warnings of the no-OS driver go to the console (printf) */
#define HAVE_VERBOSE_MESSAGES

#endif
