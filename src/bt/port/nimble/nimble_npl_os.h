#ifndef ASTEROID_NIMBLE_NPL_OS_H
#define ASTEROID_NIMBLE_NPL_OS_H
#include <stdbool.h>
#include <stdint.h>

#define BLE_NPL_OS_ALIGNMENT	4
#define BLE_NPL_TIME_FOREVER	UINT32_MAX

typedef uint32_t ble_npl_time_t;
typedef int32_t ble_npl_stime_t;

struct ble_npl_event {
	struct ble_npl_event *next;
	ble_npl_event_fn *fn;
	void *arg;
	bool queued;
};

struct ble_npl_eventq {
	struct ble_npl_event *head, *tail;
};

struct ble_npl_callout {
	struct ble_npl_callout *next;
	struct ble_npl_event ev;
	struct ble_npl_eventq *evq;
	ble_npl_time_t expiry;
	bool active, linked;
};

struct ble_npl_mutex {
	uint16_t depth;
};

struct ble_npl_sem {
	uint16_t tokens;
};
#endif
