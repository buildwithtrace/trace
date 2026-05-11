#
#  This program source code file is part of TRACE, a fork of KiCad EDA CAD application.
#
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, you may find one here:
#  http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
#  or you may search the http://www.gnu.org website for the version 2 license,
#  or you may write to the Free Software Foundation, Inc.,
#  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
#

# This file will create the full Trace version string. The base of this string
# will be either the git tag followed by the commit hash (if built inside a git
# repository), or the version from TraceVersion.cmake. The user-provided
# TRACE_VERSION_EXTRA is then appended to the base version string.

# For now, use the version from TraceVersion.cmake directly.
# In the future, this could use git to determine the version string if available.
# include( ${KICAD_CMAKE_MODULE_PATH}/CreateGitVersionHeader.cmake )
# create_git_version_header( ${SRC_PATH} )

# $TRACE_VERSION is set in TraceVersion.cmake
set( TRACE_VERSION_FULL "${TRACE_VERSION}" )

# Optional user version information defined at configuration.
if( TRACE_VERSION_EXTRA )
    set( TRACE_VERSION_FULL "${TRACE_VERSION_FULL}-${TRACE_VERSION_EXTRA}" )
endif()

