#pragma once
#include <stdbool.h>

// Call once at boot. If credentials are missing, starts AP+portal until saved.
// Returns immediately; use wifi_manager_is_connected() to wait.
void wifi_manager_start(void);

// True after STA got IP
bool wifi_manager_is_connected(void);
