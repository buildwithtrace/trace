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

# This file gets included in the WriteVersionHeader.cmake file to set
# the default Trace version when the source is provided in an archive
# file or git is not available on the build system.   When Trace is
# cloned using git, the git version can be used.  This version string should
# be set after each version tag is added to the git repo.  This will
# give developers a reasonable idea where which branch was used to build
# Trace.
#
# Note: This version string should follow the semantic versioning system
set( TRACE_SEMANTIC_VERSION "1.3.1" )

# Default the version to the semantic version.
# This can be overridden by the git repository tag though (if using git)
set( TRACE_VERSION "${TRACE_SEMANTIC_VERSION}" )

