To run these tests manually, you may need to set some or all of
the following environment variables:

# Replace with the path to trace-cli in your build environment
TRACE_CLI=/path/to/kicad/cmake-build-debug/kicad/Trace.app/Contents/MacOS/trace-cli
KICAD_RUN_FROM_BUILD_DIR=1

On macOS you will also need to take the following steps if you want to
test trace-cli from the build dir:

1) Symlink the contents of the folder:

    /path/to/kicad-mac-builder/build/python-dest/Library/Frameworks/Python.framework/Versions/3.9/

   from your kicad-mac-builder folder into the folder:

   /path/to/kicad/cmake-build-debug/kicad/Trace.app/Contents/Frameworks/Python.framework/Versions/3.9/

   inside your Trace build directory.

2) Set the following environment variable so that the Python dll is found:

   DYLD_LIBRARY_PATH=/path/to/kicad-mac-builder/build/python-dest/
