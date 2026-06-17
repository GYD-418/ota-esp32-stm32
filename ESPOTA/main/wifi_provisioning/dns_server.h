#pragma once

#include "esp_err.h"
#include "lwip/ip4_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dns_server_start(ip4_addr_t gateway);
void dns_server_stop(void);

#ifdef __cplusplus
}
#endif