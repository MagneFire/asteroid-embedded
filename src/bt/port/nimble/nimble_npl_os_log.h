#ifndef ASTEROID_NIMBLE_NPL_OS_LOG_H
#define ASTEROID_NIMBLE_NPL_OS_LOG_H
#include <stdarg.h>

void bt_npl_vlog(const char *fmt, va_list ap);

#define BT_NPL_LOG_DEBUG	0
#define BT_NPL_LOG_INFO		1
#define BT_NPL_LOG_WARN		1
#define BT_NPL_LOG_ERROR	1
#define BT_NPL_LOG_CRITICAL	1

#define BLE_NPL_LOG_IMPL(lvl)                                                                  \
	static inline void                                                                     \
	_BLE_NPL_LOG_CAT(BLE_NPL_LOG_MODULE, _BLE_NPL_LOG_CAT(_, lvl))(const char *fmt, ...) \
	{                                                                                      \
		if (!BT_NPL_LOG_##lvl)                                                         \
			return;                                                                \
		va_list ap;                                                                    \
		va_start(ap, fmt);                                                             \
		bt_npl_vlog(fmt, ap);                                                          \
		va_end(ap);                                                                    \
	}
#endif
