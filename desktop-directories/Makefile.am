directorydir = $(datadir)/cafe/desktop-directories

# Should be moved to cafe-control-center:
directory_in_controlcenterfiles = \
	cafe-hardware.directory.in \
	cafe-internet-and-network.directory.in \
	cafe-look-and-feel.directory.in \
	cafe-personal.directory.in \
	cafe-system.directory.in

directory_in_files = \
	cafe-audio-video.directory.in \
	cafe-development.directory.in \
	cafe-education.directory.in \
	cafe-game.directory.in \
	cafe-graphics.directory.in \
	cafe-network.directory.in \
	cafe-office.directory.in \
	cafe-settings.directory.in \
	cafe-settings-system.directory.in \
	cafe-system-tools.directory.in \
	cafe-utility.directory.in \
	cafe-utility-accessibility.directory.in \
	cafe-other.directory.in \
	cafe-menu-applications.directory.in \
	cafe-menu-system.directory.in \
	$(directory_in_controlcenterfiles)

directory_DATA = $(directory_in_files:.directory.in=.directory)

%.directory: %.directory.in
	$(AM_V_GEN) $(MSGFMT) --desktop --keyword= --keyword=Name --keyword=Description --template $< -d $(top_srcdir)/po -o $@

EXTRA_DIST= $(directory_in_files)

DISTCLEANFILES = $(directory_DATA)

-include $(top_srcdir)/git.mk
