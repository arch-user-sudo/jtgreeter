#include "ui.h"

#include <strings.h>
#include <sys/stat.h>

unsigned accent_rgb = 0xE8A03C;
unsigned accent_hi_rgb = 0xF2B25C;
unsigned accent_lo_rgb = 0xCE8A2F;
unsigned on_accent_rgb = 0x14100A;
unsigned accent_base_rgb = 0xE8A03C;
int rainbow_mode = 0;
double rainbow_start = 0.0;

time_t rainbow_conf_sec = (time_t)-1;
long rainbow_conf_nsec = -1;
off_t rainbow_conf_size = (off_t)-1;

int
rainbow_load_default(void)
{
	int on = 0;
	FILE *f = fopen(RAINBOW_CONF_PATH, "r");
	if (f) {
		char buf[64];
		if (fgets(buf, sizeof(buf), f)) {
			char *nl = strpbrk(buf, "\r\n");
			if (nl) *nl = '\0';
			static const char prefix[] = "rainbow=";
			if (!strncmp(buf, prefix, sizeof(prefix) - 1)) {
				const char *value = buf + sizeof(prefix) - 1;
				if (!strcmp(value, "on"))
					on = 1;
				else if (!strcmp(value, "off"))
					on = 0;
			}
		}
		fclose(f);
	}
	rainbow_set(on, ui_mono_sec());
	return on;
}

void
rainbow_conf_snapshot(void)
{
	struct stat st;
	if (stat(RAINBOW_CONF_PATH, &st) == 0) {
		rainbow_conf_sec = st.st_mtim.tv_sec;
		rainbow_conf_nsec = st.st_mtim.tv_nsec;
		rainbow_conf_size = st.st_size;
	} else {
		rainbow_conf_sec = (time_t)-1;
		rainbow_conf_nsec = -1;
		rainbow_conf_size = (off_t)-1;
	}
}

int
rainbow_poll_config(void)
{
	struct stat st;
	if (stat(RAINBOW_CONF_PATH, &st) != 0) {
		if (rainbow_conf_sec == (time_t)-1)
			return 0;
		rainbow_conf_snapshot();
		rainbow_set(0, ui_mono_sec());
		return 1;
	}
	if (rainbow_conf_sec == st.st_mtim.tv_sec &&
	    rainbow_conf_nsec == st.st_mtim.tv_nsec &&
	    rainbow_conf_size == st.st_size)
		return 0;

	rainbow_conf_snapshot();
	rainbow_load_default();
	return 1;
}
