#ifndef DSI_PANEL_H
#define DSI_PANEL_H

/* TW220 AMOLED panel power control over the DSI controller's command DMA
 * engine (see dsi_panel.c).  Registered as fbdisplayd's dev.cmd handler;
 * "sleep"/"wake" send the DCS sleep-in/sleep-out sequence, "panel" reports
 * state.  Returns malloc'ed text (freed by libfbdisplayd), NULL when the
 * command is unknown. */
extern char* dsi_panel_dev_cmd(int from_pid, int argc, char** argv);

#endif