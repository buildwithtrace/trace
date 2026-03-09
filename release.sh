#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RELEASES_DIR="$SCRIPT_DIR/releases"
# Original KiCad-to-Trace fork commit; all public releases branch from this
PUBLIC_BASE_COMMIT="3ac37aa0da8bd4fefb44e69c03ab8f13c89d888a"
DRY_RUN=false
UPDATE_MAIN=false
VERSION=""

usage() {
    echo "Usage: ./release.sh [--dry-run] [--update-main] <version>"
    echo ""
    echo "Examples:"
    echo "  ./release.sh 1.2.0          # Stable: push to origin + squash to public"
    echo "  ./release.sh 1.2.0-beta     # Pre-release: push to origin only"
    echo "  ./release.sh --update-main 1.2.0   # Also force-push public/main to the release"
    echo "  ./release.sh --dry-run 1.2.0"
    exit 1
}

for arg in "$@"; do
    case $arg in
        --dry-run) DRY_RUN=true ;;
        --update-main) UPDATE_MAIN=true ;;
        --help|-h) usage ;;
        *)
            if [ -z "$VERSION" ]; then
                VERSION="$arg"
            else
                echo "Error: unexpected argument '$arg'"
                usage
            fi
            ;;
    esac
done

[ -z "$VERSION" ] && usage

is_prerelease() {
    [[ "$1" == *-* ]]
}

# --- YAML parsing via Python (no external deps) ---

YAML_FILE="$RELEASES_DIR/$VERSION.yml"
if [ ! -f "$YAML_FILE" ]; then
    echo "Error: release manifest not found at $YAML_FILE"
    echo "Create it first (see releases/template.yml)"
    exit 1
fi

parse_yaml() {
    python3 -c "
import sys, json

def parse_yaml_simple(path):
    \"\"\"Minimal YAML parser for our flat release manifest format.\"\"\"
    with open(path) as f:
        lines = f.readlines()

    result = {}
    current_list_key = None
    current_category = None
    items = []
    changes = []

    for line in lines:
        stripped = line.rstrip()
        if not stripped or stripped.startswith('#'):
            continue

        # Top-level scalar: key: \"value\" or key: value
        if not line.startswith(' ') and not line.startswith('\t') and ':' in stripped:
            if stripped.startswith('changes:'):
                current_list_key = 'changes'
                continue
            key, val = stripped.split(':', 1)
            val = val.strip().strip('\"').strip(\"'\")
            result[key.strip()] = val
            continue

        # Inside changes list
        if current_list_key == 'changes':
            indent = len(line) - len(line.lstrip())
            content = stripped.lstrip('- ').strip()

            if indent <= 4 and 'category:' in stripped:
                if current_category and items:
                    changes.append({'category': current_category, 'items': items})
                    items = []
                current_category = content.split(':', 1)[1].strip().strip('\"').strip(\"'\")
            elif 'items:' in stripped:
                continue
            elif stripped.lstrip().startswith('- '):
                item = stripped.lstrip().lstrip('- ').strip().strip('\"').strip(\"'\")
                items.append(item)

    if current_category and items:
        changes.append({'category': current_category, 'items': items})

    result['changes'] = changes
    return result

data = parse_yaml_simple('$YAML_FILE')
print(json.dumps(data))
" 2>/dev/null
}

YAML_JSON=$(parse_yaml)

yaml_get() {
    echo "$YAML_JSON" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('$1',''))"
}

YAML_VERSION=$(yaml_get "version")
SUMMARY=$(yaml_get "summary")

if [ "$YAML_VERSION" != "$VERSION" ]; then
    echo "Error: version in YAML ($YAML_VERSION) does not match argument ($VERSION)"
    exit 1
fi

build_commit_message() {
    python3 -c "
import sys, json

data = json.loads('''$YAML_JSON''')
lines = []
lines.append('Release ' + data['version'])
lines.append('')
if data.get('summary'):
    lines.append(data['summary'])
    lines.append('')
for group in data.get('changes', []):
    lines.append('## ' + group['category'])
    for item in group['items']:
        lines.append('  - ' + item)
    lines.append('')
print('\n'.join(lines))
"
}

# --- Validation ---

VERSION_CMAKE="$SCRIPT_DIR/cmake/TraceVersion.cmake"

echo "=== Trace Release: $VERSION ==="
echo ""

if is_prerelease "$VERSION"; then
    echo "Type: PRE-RELEASE (origin only)"
else
    echo "Type: STABLE (origin + public)"
fi
echo "Summary: $SUMMARY"
echo ""

CURRENT_BRANCH=$(git symbolic-ref --short HEAD 2>/dev/null || echo "detached")
if [ "$CURRENT_BRANCH" != "main" ]; then
    echo "Error: you must be on the 'main' branch to release (currently on '$CURRENT_BRANCH')"
    exit 1
fi

CMAKE_VERSION=$(grep 'set( TRACE_SEMANTIC_VERSION' "$VERSION_CMAKE" | sed 's/.*"\(.*\)".*/\1/')
BASE_VERSION="${VERSION%%-*}"
if [ "$CMAKE_VERSION" != "$BASE_VERSION" ]; then
    echo "Error: TraceVersion.cmake has version '$CMAKE_VERSION' but releasing '$BASE_VERSION'"
    echo "Update cmake/TraceVersion.cmake first."
    exit 1
fi

if [ -n "$(git status --porcelain)" ]; then
    echo "Error: working tree is not clean. Commit or stash changes first."
    exit 1
fi

git remote get-url origin >/dev/null 2>&1 || { echo "Error: remote 'origin' not found"; exit 1; }

if git show-ref --verify --quiet "refs/heads/$VERSION" 2>/dev/null; then
    echo "Error: branch '$VERSION' already exists locally. Delete it first or pick a different version."
    exit 1
fi

if ! is_prerelease "$VERSION"; then
    git remote get-url public >/dev/null 2>&1 || { echo "Error: remote 'public' not found"; exit 1; }

    git cat-file -e "$PUBLIC_BASE_COMMIT^{commit}" 2>/dev/null || {
        echo "Error: base commit $PUBLIC_BASE_COMMIT not found. Fetch origin."
        exit 1
    }

    if git show-ref --verify --quiet "refs/remotes/public/$VERSION" 2>/dev/null; then
        echo "Error: branch '$VERSION' already exists on public remote"
        exit 1
    fi
fi

# --- Confirmation ---

if $DRY_RUN; then
    echo "[DRY RUN] Would create branch '$VERSION' from main"
    echo "[DRY RUN] Would push '$VERSION' to origin"
    if ! is_prerelease "$VERSION"; then
        echo "[DRY RUN] Would create squashed branch '$VERSION' on public from base $PUBLIC_BASE_COMMIT"
    fi
    if $UPDATE_MAIN; then
        echo "[DRY RUN] Would force-push public/main to $VERSION"
    fi
    if ! is_prerelease "$VERSION"; then
        echo ""
        echo "Commit message would be:"
        echo "---"
        build_commit_message
        echo "---"
    fi
    echo ""
    echo "Dry run complete. No changes made."
    exit 0
fi

echo "Proceed? [y/N] "
read -r CONFIRM
if [[ ! "$CONFIRM" =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

# --- Step 1: Create release branch from main ---

echo ""
echo "Creating branch '$VERSION' from main..."
git branch "$VERSION" main
echo "Done."

# --- Step 2: Push to origin ---

echo "Pushing '$VERSION' to origin..."
git push origin "$VERSION"
echo "Done."

# --- Step 3: Squash to public (stable only) ---

if is_prerelease "$VERSION"; then
    echo ""
    echo "=== Release complete ==="
    echo "  origin: branch '$VERSION' pushed (full history)"
    echo "  public: skipped (pre-release)"
    exit 0
fi

echo ""
echo "Creating squashed branch for public..."

git fetch origin --quiet

TEMP_BRANCH="release/$VERSION"

# Branch from the original KiCad-to-Trace fork commit; squash all changes
# from the release branch into one commit on top of that base.
git checkout -b "$TEMP_BRANCH" "$PUBLIC_BASE_COMMIT" --quiet

git merge --squash "$VERSION" --quiet

COMMIT_MSG=$(build_commit_message)
git commit -m "$COMMIT_MSG" --quiet

echo "Pushing '$VERSION' to public..."
git push public "$TEMP_BRANCH:$VERSION"

if $UPDATE_MAIN; then
    echo ""
    echo "Updating public/main to $VERSION..."
    git push public "$TEMP_BRANCH:main" --force
    echo "Done."
fi

git checkout main --quiet
git branch -D "$TEMP_BRANCH" --quiet 2>/dev/null

echo ""
echo "=== Release complete ==="
echo "  origin: branch '$VERSION' pushed (full history)"
echo "  public: branch '$VERSION' pushed (squashed from base $PUBLIC_BASE_COMMIT)"
$UPDATE_MAIN && echo "  public/main: force-pushed to $VERSION"
