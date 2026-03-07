# Stub port for gettext-libintl on Windows
# The actual gettext functionality is provided by pre-built binaries
# that are downloaded separately by the build system and added to PATH.
#
# This overlay port exists to work around vcpkg's gettext build issues on Windows.
# It provides a minimal libintl.h header to satisfy compile-time dependencies.

set(VCPKG_POLICY_EMPTY_PACKAGE enabled)

# Create a minimal libintl.h header
# This provides the standard GNU gettext API declarations
file(WRITE "${CURRENT_PACKAGES_DIR}/include/libintl.h" [[
/* libintl.h - GNU gettext internationalization library interface
 * Stub header for Windows builds using pre-built gettext binaries.
 * Pre-built binaries from: https://github.com/mlocati/gettext-iconv-windows
 */

#ifndef _LIBINTL_H
#define _LIBINTL_H 1

#ifdef __cplusplus
extern "C" {
#endif

extern char *gettext(const char *__msgid);
extern char *dgettext(const char *__domainname, const char *__msgid);
extern char *dcgettext(const char *__domainname, const char *__msgid, int __category);
extern char *ngettext(const char *__msgid1, const char *__msgid2, unsigned long int __n);
extern char *dngettext(const char *__domainname, const char *__msgid1, const char *__msgid2, unsigned long int __n);
extern char *dcngettext(const char *__domainname, const char *__msgid1, const char *__msgid2, unsigned long int __n, int __category);
extern char *textdomain(const char *__domainname);
extern char *bindtextdomain(const char *__domainname, const char *__dirname);
extern char *bind_textdomain_codeset(const char *__domainname, const char *__codeset);

#ifdef __cplusplus
}
#endif

#endif /* _LIBINTL_H */
]])

# Install copyright
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright"
"GNU gettext libintl - stub port for Windows.

This is a stub port that provides only the libintl.h header.
The actual gettext runtime is provided by pre-built binaries from:
https://github.com/mlocati/gettext-iconv-windows

These binaries are downloaded separately by the build system.

GNU gettext is free software released under the GNU Lesser General Public License (LGPL).
For full license text, see: https://www.gnu.org/licenses/lgpl-2.1.html
")

message(STATUS "Using stub gettext-libintl port - provides header only")
