# KiCad HTTP / Provider Test Server

This module implements a FastAPI fixture server for two paths:

- KiCad HTTP library smoke tests
- embedded remote provider panel tests

FastAPI gives you a test interface at the `/docs` endpoint to observe the responses for debug.

## Dependencies / Setup

### Using uv (Recommended)

[uv](https://github.com/astral-sh/uv) is a fast Python package installer and resolver written in Rust. **uv works best for isolation** by automatically managing virtual environments and dependencies without affecting your system Python.

**Install uv:**
```bash
# On macOS/Linux
curl -LsSf https://astral.sh/uv/install.sh | sh

# On Windows
powershell -c "irm https://astral.sh/uv/install.ps1 | iex"

# Or with pip
pip install uv
```

**Install dependencies:**
```bash
cd qa/tests/eeschema
uv sync
```

That's it! `uv sync` automatically creates an isolated virtual environment and installs all dependencies.

### Using pip

```bash
# Navigate to directory
cd qa/tests/eeschema

# Create virtual environment (recommended for isolation)
python -m venv .venv
source .venv/bin/activate  # On Unix/macOS
.venv\Scripts\activate     # On Windows

# Install dependencies
pip install -r requirements.txt
```

## Usage

Run the HTTP/provider test server:

```bash
uv run http_lib_test_server.py
```

Or use:
```bash
python http_lib_test_server.py
```

The server will start on `http://127.0.0.1:8000`

The server implements both:

- the KiCad HTTP Library API in the simplest use case
- provider metadata, OAuth stubs, session bootstrap, embedded provider page, download endpoints
- compatibility search/manifest endpoints used by lower-level tests

Optional flags:

```bash
uv run http_lib_test_server.py --host 127.0.0.1 --port 8000
```

## Testing with KiCad

1. Start the test server:
   ```bash
   uv run http_lib_test_server.py
   ```

2. For HTTP library testing, load `http_test.kicad_httplib` in KiCad.

3. For remote provider testing, add a provider whose metadata URL is:
   `http://127.0.0.1:8000/.well-known/kicad-remote-provider`

4. Open the remote symbol panel. The provider page is rendered inside the embedded webview.

5. Use `Sign In` to exercise OAuth loopback + KiCad session bootstrap.

## License

This script is part of KiCad, licensed under the GNU General Public License v3.0.

For development and testing purposes only. Not intended for production use.
