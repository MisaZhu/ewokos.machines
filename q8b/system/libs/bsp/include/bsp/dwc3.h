#ifndef __Q8B_DWC3_H
#define __Q8B_DWC3_H

#include <ewoksys/ewokdef.h>

/* Take over a firmware-powered USB-C DWC3 core in the board's host mode.
 * base covers the core's xHCI, global and device register apertures. */
int dwc3_host_prepare(int id, ewokos_addr_t base);

#endif
