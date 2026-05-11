#
# This program source code file is part of Trace, an AI-native PCB design application.
#
# Copyright (C) 2025-2026 Trace Developers Team
# Copyright The Trace Developers, see TRACE_AUTHORS.txt for contributors.
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by the
# Free Software Foundation, either version 3 of the License, or (at your
# option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program.  If not, see <http://www.gnu.org/licenses/>.

"""
Progress Reporter for Trace Converters

Provides lightweight progress reporting for conversion phases.
The C++ process executor monitors these markers to detect stalls
and implement progress-based timeouts.

Output format:
- [PHASE] <name>  - Entering a new processing phase
- [HEARTBEAT]     - Still alive signal during long operations
"""

import sys
import time


class ProgressReporter:
    """Lightweight progress reporter for conversion phases.
    
    Outputs progress markers to stderr that the C++ process executor
    monitors to detect stalls. If no output is received for a configured
    timeout period (default 30 seconds), the process is terminated.
    
    Usage:
        progress = ProgressReporter()
        progress.phase("Loading symbols")
        for item in large_list:
            progress.heartbeat()  # Call in loops to prevent timeout
            process(item)
        progress.phase("Formatting output")
    """
    
    def __init__(self, heartbeat_interval=2.0):
        """Initialize the progress reporter.
        
        Args:
            heartbeat_interval: Minimum seconds between heartbeat outputs.
                               Prevents flooding stderr with heartbeats.
        """
        self.last_heartbeat = time.time()
        self.heartbeat_interval = heartbeat_interval
    
    def phase(self, name):
        """Report entering a new processing phase.
        
        Args:
            name: Description of the phase (e.g., "Loading symbols")
        """
        print(f"[PHASE] {name}", file=sys.stderr, flush=True)
        self.last_heartbeat = time.time()
    
    def heartbeat(self):
        """Send heartbeat if enough time has passed.
        
        Call this in loops to prevent the process from being killed
        due to stall timeout. The heartbeat is rate-limited to avoid
        flooding stderr.
        
        Returns:
            True if a heartbeat was sent, False if skipped due to rate limiting.
        """
        now = time.time()
        if now - self.last_heartbeat >= self.heartbeat_interval:
            print("[HEARTBEAT]", file=sys.stderr, flush=True)
            self.last_heartbeat = now
            return True
        return False


# Global instance for convenience
_global_reporter = None


def get_progress_reporter(heartbeat_interval=2.0):
    """Get or create the global progress reporter instance.
    
    Args:
        heartbeat_interval: Minimum seconds between heartbeat outputs.
        
    Returns:
        The global ProgressReporter instance.
    """
    global _global_reporter
    if _global_reporter is None:
        _global_reporter = ProgressReporter(heartbeat_interval)
    return _global_reporter
