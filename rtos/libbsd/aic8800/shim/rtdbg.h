/*
 * RT-Thread <rtdbg.h> for the AIC8800 port: LOG_x go to printf with the DBG_TAG prefix.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* deliberately no include guard: every source sets DBG_TAG/DBG_LVL before including it */
#include <stdio.h>
#ifndef DBG_ERROR
#define DBG_ERROR   0
#define DBG_WARNING 1
#define DBG_INFO    2
#define DBG_LOG     3
#endif
#ifndef DBG_TAG
#define DBG_TAG "aicwf"
#endif
#ifndef DBG_LVL
#define DBG_LVL DBG_WARNING
#endif
#undef LOG_E
#undef LOG_W
#undef LOG_I
#undef LOG_D
#undef LOG_RAW
#undef dbg_log
void rt_shim_log(int level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
#define LOG_E(...) do { if (DBG_LVL >= DBG_ERROR) rt_shim_log(DBG_ERROR, DBG_TAG, __VA_ARGS__); } while (0)
#define LOG_W(...) do { if (DBG_LVL >= DBG_WARNING) rt_shim_log(DBG_WARNING, DBG_TAG, __VA_ARGS__); } while (0)
#define LOG_I(...) do { if (DBG_LVL >= DBG_INFO) rt_shim_log(DBG_INFO, DBG_TAG, __VA_ARGS__); } while (0)
#define LOG_D(...) do { if (DBG_LVL >= DBG_LOG) rt_shim_log(DBG_LOG, DBG_TAG, __VA_ARGS__); } while (0)
#define LOG_RAW(...) printf(__VA_ARGS__)
#define dbg_log(level, ...) do { if (DBG_LVL >= (level)) rt_shim_log(level, DBG_TAG, __VA_ARGS__); } while (0)
