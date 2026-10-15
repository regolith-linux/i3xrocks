bin_PROGRAMS = i3xrocks
i3xrocks_LDADD = -lxcb -lxcb-xrm
i3xrocks_SOURCES = \
	bar.c \
	bar.h \
	block.c \
	block.h \
	config.c \
	config.h \
	i3bar.c \
	ini.c \
	ini.h \
	json.c \
	json.h \
	line.c \
	line.h \
	log.h \
	main.c \
	map.c \
	map.h \
	sys.c \
	sys.h \
	term.h

dist_man1_MANS = \
	docs/i3xrocks.1

dist_sysconf_DATA = \
	i3xrocks.conf

TESTS = tests/xresource-missing.sh
EXTRA_DIST = $(TESTS)
