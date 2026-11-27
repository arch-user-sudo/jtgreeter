/* jtgreeterctl — persist the greeter accent color and read the shared rainbow flag.
 *
 * Usage:
 *   jtgreeterctl accent '#RRGGBB'   write accent to /etc/jt/accent.conf
 *   jtgreeterctl '#RRGGBB'          shorthand for the same
 *   jtgreeterctl rainbow [on|off|toggle]
 *                                  print /etc/jt/rainbow.conf (read-only)
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACCENT_CONF_PATH "/etc/jt/accent.conf"
#define RAINBOW_CONF_PATH "/etc/jt/rainbow.conf"

static int
hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int
parse_color(const char *s, unsigned *out)
{
	if (!s) return 0;
	if (*s == '#') s++;
	else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
	if (strlen(s) != 6) return 0;
	unsigned value = 0;
	for (int i = 0; i < 6; i++) {
		int nibble = hex_nibble(s[i]);
		if (nibble < 0) return 0;
		value = (value << 4) | (unsigned)nibble;
	}
	*out = value;
	return 1;
}

static int
rainbow_load(void)
{
	FILE *f = fopen(RAINBOW_CONF_PATH, "r");
	if (!f) return 0;
	char buf[64];
	int on = 0;
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
	return on;
}

static int
write_conf(const char *path, const char *line)
{
	/* Best-effort: directory/file permissions are expected to be set by install. */
	(void)mkdir("/etc/jt", 0755);

	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "jtgreeterctl: cannot write %s: %s\n", path, strerror(errno));
		return 1;
	}
	int ok = fprintf(f, "%s\n", line) > 0;
	if (fclose(f) != 0) ok = 0;
	if (!ok) {
		fprintf(stderr, "jtgreeterctl: failed to write %s\n", path);
		return 1;
	}
	return 0;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
	        "usage: %s accent '#RRGGBB' | rainbow [on|off|toggle]\n"
	        "       %s '#RRGGBB'\n"
	        "  accent COLOR   write the accent color to %s\n"
	        "  rainbow        print the shared flag in %s (read-only)\n",
	        prog, prog, ACCENT_CONF_PATH, RAINBOW_CONF_PATH);
}

int
main(int argc, char *argv[])
{
	if (argc < 2) { usage(argv[0]); return 1; }
	unsigned rgb = 0;
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(argv[0]);
		return 0;
	} else if (!strcmp(argv[1], "accent")) {
		if (argc != 3 || !parse_color(argv[2], &rgb)) {
			fprintf(stderr, "%s: accent requires a color like '#E8A03C'\n", argv[0]);
			return 2;
		}
	} else if (argc == 2 && parse_color(argv[1], &rgb)) {
		/* shorthand accent */
	} else if (!strcmp(argv[1], "rainbow")) {
		if (argc > 3) {
			usage(argv[0]);
			return 1;
		}
		printf("rainbow=%s\n", rainbow_load() ? "on" : "off");
		return 0;
	} else {
		usage(argv[0]);
		return 1;
	}

	char line[64];
	snprintf(line, sizeof(line), "#%06X", rgb & 0xFFFFFFu);
	return write_conf(ACCENT_CONF_PATH, line);
}
