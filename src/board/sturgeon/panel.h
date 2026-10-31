/* RM69080: entering idle mode selects command page 0 and sends a NOP
 * followed by 150 ms before the idle command. */
static const struct dcs_step panel_idle_on[] = {
	{ 0xfe, 1, 0, { 0x00 } },
	{ 0x00, 1, 150, { 0x00 } },
	{ 0x39, 0, 0, { 0 } },
};
static const struct dcs_step panel_idle_off[] = {
	{ 0xfe, 1, 0, { 0x00 } },
	{ 0x38, 0, 0, { 0 } },
};
