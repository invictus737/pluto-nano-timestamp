/*
 * RT-Thread <ipc/completion.h> for the AIC8800 port.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef AICWF_COMPLETION_H
#define AICWF_COMPLETION_H
#include <rtthread.h>
struct rt_completion
{
    struct rt_shim_wait wait;
    rt_uint32_t flag;
    rt_uint32_t initialized;
};
void rt_completion_init(struct rt_completion *completion);
rt_err_t rt_completion_wait(struct rt_completion *completion, rt_int32_t timeout);
void rt_completion_done(struct rt_completion *completion);
#endif
