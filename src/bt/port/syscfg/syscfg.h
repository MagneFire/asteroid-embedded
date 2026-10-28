/* NimBLE build configuration: our values first, the porting layer's
 * generated defaults (all #ifndef guarded) fill in the rest. */
#ifndef ASTEROID_SYSCFG_H
#define ASTEROID_SYSCFG_H

#define MYNEWT_VAL_BLE_TRANSPORT_LL__socket (0)
#define MYNEWT_VAL_BLE_TRANSPORT_LL__custom (1)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_SIZE (257)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_LL_COUNT (12)

#define MYNEWT_VAL_BLE_ROLE_BROADCASTER (1)
#define MYNEWT_VAL_BLE_ROLE_PERIPHERAL (1)
#define MYNEWT_VAL_BLE_ROLE_CENTRAL (0)
#define MYNEWT_VAL_BLE_ROLE_OBSERVER (0)
#define MYNEWT_VAL_BLE_MAX_CONNECTIONS (1)

#define MYNEWT_VAL_BLE_SM_LEGACY (1)
#define MYNEWT_VAL_BLE_SM_SC (1)
#define MYNEWT_VAL_BLE_SM_BONDING (1)
#define MYNEWT_VAL_BLE_SM_MITM (1)
#define MYNEWT_VAL_BLE_SM_IO_CAP (0x01)
#define MYNEWT_VAL_BLE_SM_OUR_KEY_DIST (0x03)
#define MYNEWT_VAL_BLE_SM_THEIR_KEY_DIST (0x03)
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS (4)
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS (16)

#define MYNEWT_VAL_BLE_ATT_PREFERRED_MTU (256)
#define MYNEWT_VAL_MSYS_1_BLOCK_COUNT (32)
#define MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME "sturgeon"
#define MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME_MAX_LENGTH (31)

#include "../../../../third_party/nimble/porting/nimble/include/syscfg/syscfg.h"
#endif
