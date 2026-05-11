# This is a stub port for gettext on Windows.
# The actual gettext tools (msgfmt, xgettext, etc.) are provided by pre-built binaries
# that are downloaded separately and added to PATH by the build system.
#
# This overlay port exists to work around vcpkg's gettext build issues on Windows
# (specifically the libintl.h header not being installed properly).

set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" 
"GNU gettext stub port.

The actual gettext tools are provided by pre-built binaries from:
https://github.com/mlocati/gettext-iconv-windows

These binaries are downloaded separately by the build system and added to PATH.
This vcpkg port is a stub that satisfies dependency requirements.

GNU gettext is free software released under the GNU General Public License (GPL).
For full license text, see: https://www.gnu.org/licenses/gpl-3.0.html
")

message(STATUS "Using stub gettext port - actual tools provided via PATH")
