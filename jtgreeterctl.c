/* jtgreeterctl — persist jtgreeter accent color for greeter/lock screens.
 *
 * Usage:
 *   jtgreeterctl accent '#RRGGBB'   write accent to /etc/jt/accent.conf
 *   jtgreeterctl '#RRGGBB'          shorthand for the same
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACCENT_CONF_PATH "/etc/jt/accent.conf"

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

static void
usage(const char *prog)
{
	fprintf(stderr,
	        "usage: %s accent '#RRGGBB'\n"
	        "       %s '#RRGGBB'\n"
	        "  accent COLOR   write the accent color to %s\n",
	        prog, prog, ACCENT_CONF_PATH);
}

int
main(int argc, char *argv[])
{
	if (argc < 2) { usage(argv[0]); return 1; }
	unsigned rgb;
	if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(argv[0]);
		return 0;
	} else if (!strcmp(argv[1], "accent")) {
		if (argc != 3 || !parse_color(argv[2], &rgb)) {
			fprintf(stderr, "%s: accent requires a color like '#E8A03C'\n", argv[0]);
			return 2;
		}
	} else if (argc == 2 && parse_color(argv[1], &rgb)) {
		/* ok */
	} else {
		usage(argv[0]);
		return 1;
	}

	/* Best-effort: directory/file permissions are expected to be set by install. */
	(void)mkdir("/etc/jt", 0777);

	FILE *f = fopen(ACCENT_CONF_PATH, "w");
	if (!f) {
		fprintf(stderr, "%s: cannot write %s: %s\n", argv[0], ACCENT_CONF_PATH, strerror(errno));
		return 1;
	}
	int ok = fprintf(f, "#%06X\n", rgb & 0xFFFFFFu) > 0;
	if (fclose(f) != 0) ok = 0;
	if (!ok) {
		fprintf(stderr, "%s: failed to write %s\n", argv[0], ACCENT_CONF_PATH);
		return 1;
	}
	return 0;
}
