#
# Make defs common to both 850C and SW102
#


# The integer build number for this release, MUST BE INCREMENTED FOR EACH RELEASE SO BOOTLOADER WILL INSTALL
# it is not user visible, but we must ensure it is monotonically increasing.
# The release workflow (tools/bump-version.py) bumps this by 1 each release.
# Kept ahead of the highest value already flashed to field devices (was 25 on
# the old `main`, 27 for the first hand-built OTA) so OTA downgrades aren't
# blocked by the bootloader's monotonic-version gate.
VERSION_NUM := 29

# User-visible SemVer string. Managed by the release workflow.
VERSION_STRING := 0.0.1-alpha.1

CFLAGS += -DVERSION_STRING=\"$(VERSION_STRING)\"
