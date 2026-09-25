#!/bin/bash
set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
AUR_DIR="aur"
# The AUR repo's local branch and the remote/branch it is published to
# (the AUR only accepts pushes to master). Set AUR_PUSH=0 to commit the AUR
# update without pushing it.
AUR_REMOTE="${AUR_REMOTE:-aur}"
AUR_LOCAL_BRANCH="${AUR_LOCAL_BRANCH:-main}"
AUR_REMOTE_BRANCH="${AUR_REMOTE_BRANCH:-master}"
AUR_PUSH="${AUR_PUSH:-1}"
MAIN_BRANCH="master"
TAG_REMOTE="mxlinux"
ANNOTATION=""

# Helper functions
print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  MX Package Installer Release Script${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo
}

print_step() {
    echo -e "${GREEN}➤ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠️  $1${NC}"
}

print_error() {
    echo -e "${RED}❌ $1${NC}"
}

print_success() {
    echo -e "${GREEN}✅ $1${NC}"
}

# Validate version format (semantic versioning or YY.MM format)
validate_version() {
    local version=$1

    # Remove 'v' prefix if present for validation
    local clean_version=${version#v}

    # Check semantic version format (major.minor.patch) or YY.MM format
    if ! [[ $clean_version =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
        print_error "Invalid version format: $version"
        echo "Expected formats: 1.0.0, v1.0.0, or YY.MM (like 26.01)"
        exit 1
    fi

    echo "$version"
}

# Check if tag already exists locally or on the remote
tag_exists() {
    local version=$1

    if git show-ref --verify --quiet "refs/tags/${version}"; then
        return 0
    fi

    if GIT_SSH_COMMAND="ssh -o BatchMode=yes -o ConnectTimeout=10" \
        git ls-remote --exit-code --tags --refs "$TAG_REMOTE" "refs/tags/${version}" > /dev/null 2>&1; then
        return 0
    fi

    return 1
}

# Get the annotation from an existing local annotated tag, if available
get_tag_annotation() {
    local version=$1
    git for-each-ref --format='%(contents)' "refs/tags/${version}" 2>/dev/null || true
}

# Get latest tag version for comparison (plain major.minor[.patch] tags only).
# Sorted by tag creation date rather than version number, since a stray
# mistyped tag (e.g. "36.03.1") would otherwise outrank real releases.
get_latest_tag() {
    git fetch --tags --quiet "$TAG_REMOTE" 2>/dev/null || true

    git for-each-ref --sort=-creatordate --format='%(refname:short)' refs/tags/ | \
        while read -r tag; do
            local clean_tag=${tag#v}
            if [[ $clean_tag =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
                printf '%s\n' "$clean_tag"
                break
            fi
        done
}

# Compare versions (returns 0 if new > old, 1 if new <= old)
compare_versions() {
    local new_version=$1
    local old_version=$2

    # Remove 'v' prefix for comparison
    new_version=${new_version#v}
    old_version=${old_version#v}

    # Use sort -V for semantic version comparison
    if [ "$new_version" = "$(echo -e "$new_version\n$old_version" | sort -V | tail -n1)" ] && [ "$new_version" != "$old_version" ]; then
        return 0  # new > old
    else
        return 1  # new <= old
    fi
}

# Prompt user for annotation
prompt_annotation() {
    local version=$1

    echo
    print_step "Enter release annotation/notes"

    local annotation=""
    local tmpfile
    tmpfile=$(mktemp) || {
        print_error "Failed to create temp file"
        exit 1
    }
    cat > "$tmpfile" <<EOF
## Release $version

- Feature 1
- Bug fix 2
- Other changes
EOF

    if [ -n "${EDITOR:-}" ]; then
        "${EDITOR}" "$tmpfile"
    else
        echo "No EDITOR set. Enter a single-line annotation and press Enter:"
        echo "(Multi-line notes require setting EDITOR.)"
        read -r annotation
        if [ -n "$annotation" ]; then
            printf "%s\n" "$annotation" > "$tmpfile"
        fi
    fi

    annotation=$(sed '/^[[:space:]]*$/d' "$tmpfile")
    rm -f "$tmpfile"

    if [ -z "$annotation" ]; then
        print_error "Annotation cannot be empty"
        exit 1
    fi

    ANNOTATION="$annotation"
}

# Create annotated tag
create_tag() {
    local version=$1
    local annotation=$2

    print_step "Creating annotated tag '$version'..."

    # Create annotated tag
    git tag -a "$version" -m "$annotation"

    print_success "Tag '$version' created successfully"
    echo
    git show "$version" --stat
}

# Update AUR package
update_aur_package() {
    local version=$1
    local annotation=$2

    print_step "Updating AUR package..."

    # Check if aur directory exists
    if [ ! -d "$AUR_DIR" ]; then
        print_error "AUR directory '$AUR_DIR' not found"
        exit 1
    fi

    local pkgbuild="$AUR_DIR/PKGBUILD"
    local srcinfo="$AUR_DIR/.SRCINFO"

    # Update PKGBUILD pkgver to match tag and remove pkgver() if present
    if [ -f "$pkgbuild" ]; then
        print_step "Updating PKGBUILD pkgver to $version..."

        if grep -q "^pkgver=" "$pkgbuild"; then
            sed -i "s/^pkgver=.*/pkgver=${version}/" "$pkgbuild"
        else
            awk -v ver="$version" '
                /^pkgname=/ { print; print "pkgver=" ver; next }
                { print }
            ' "$pkgbuild" > "$pkgbuild.tmp" && mv "$pkgbuild.tmp" "$pkgbuild"
        fi

        if grep -q "^pkgver()" "$pkgbuild"; then
            awk '
                BEGIN { in_pkgver = 0 }
                /^pkgver\(\)/ { in_pkgver = 1; next }
                in_pkgver && /^}/ { in_pkgver = 0; next }
                !in_pkgver { print }
            ' "$pkgbuild" > "$pkgbuild.tmp" && mv "$pkgbuild.tmp" "$pkgbuild"
        fi
    else
        print_error "PKGBUILD not found in $AUR_DIR"
        exit 1
    fi

    # Convert to tarball source and calculate checksum
    print_step "Converting to tarball source and calculating checksum..."
    local tarball_url="https://github.com/MX-Linux/mx-packageinstaller/archive/refs/tags/${version}.tar.gz"

    # Update source in PKGBUILD
    sed -i "s|source=.*|source=(\"${tarball_url}\")|" "$pkgbuild"

    # Remove git from makedepends if present
    sed -i '/makedepends=.*git/d' "$pkgbuild"

    # Download tarball and calculate checksum (with retry)
    local checksum=""
    local retries=5
    for i in $(seq 1 $retries); do
        print_step "Attempting to download tarball (attempt $i/$retries)..."
        if curl -L --fail --silent --show-error "$tarball_url" -o "/tmp/${version}.tar.gz" 2>/dev/null; then
            checksum=$(sha256sum "/tmp/${version}.tar.gz" | cut -d' ' -f1)
            if [ -n "$checksum" ]; then
                print_success "Checksum calculated: ${checksum:0:16}..."
                break
            fi
        fi

        if [ "$i" -lt "$retries" ]; then
            print_warning "Failed to download tarball, waiting 5 seconds before retry..."
            sleep 5
        fi
    done

    if [ -z "$checksum" ]; then
        print_error "Failed to download tarball after $retries attempts"
        print_warning "Nothing was committed; $AUR_DIR/PKGBUILD has uncommitted edits. Re-run once the tag tarball is reachable."
        exit 1
    fi

    # Update checksum in PKGBUILD
    sed -i "s/sha256sums=.*/sha256sums=('${checksum}')/" "$pkgbuild"

    # Regenerate .SRCINFO from PKGBUILD
    print_step "Regenerating .SRCINFO..."
    (cd "$AUR_DIR" && makepkg --printsrcinfo) > "$srcinfo"
    # Sanity-check what is about to be committed (and pushed)
    if ! awk -v v="${version}" '$1 == "pkgver" && $3 == v { f = 1 } END { exit !f }' "$srcinfo"; then
        print_error ".SRCINFO does not report pkgver = ${version}"
        exit 1
    fi
    if ! awk -v c="$checksum" '$1 == "sha256sums" && $3 == c { f = 1 } END { exit !f }' "$srcinfo"; then
        print_error ".SRCINFO does not carry the downloaded tarball checksum"
        exit 1
    fi

    # Check if there are any changes to commit
    print_step "Checking for AUR package changes..."

    if git -C "$AUR_DIR" diff --quiet && git -C "$AUR_DIR" diff --staged --quiet; then
        print_warning "No changes in AUR package - skipping commit"
    else
        print_step "Committing AUR package changes..."

        # Add all changes
        git -C "$AUR_DIR" add .

        # Commit changes
        git -C "$AUR_DIR" commit -m "$annotation"

        print_success "AUR package updated and committed"
        echo
        git -C "$AUR_DIR" show --stat HEAD
    fi

    # Clean up downloaded tarball
    rm -f "/tmp/${version}.tar.gz"
}

# Push the AUR repo if it has commits the AUR doesn't
push_aur_package() {
    print_step "Pushing AUR package..."

    local aur_toplevel
    aur_toplevel=$(git -C "$AUR_DIR" rev-parse --show-toplevel 2>/dev/null) || aur_toplevel=""
    if [ "$aur_toplevel" != "$(realpath "$AUR_DIR")" ]; then
        print_error "$AUR_DIR is not a separate git checkout"
        return 1
    fi

    if ! git -C "$AUR_DIR" diff --quiet HEAD -- PKGBUILD .SRCINFO; then
        print_error "$AUR_DIR has uncommitted PKGBUILD/.SRCINFO changes; commit them first"
        return 1
    fi

    local current_branch
    current_branch=$(git -C "$AUR_DIR" branch --show-current)
    if [ "$current_branch" != "$AUR_LOCAL_BRANCH" ]; then
        print_error "$AUR_DIR is on branch '$current_branch', expected '$AUR_LOCAL_BRANCH'"
        return 1
    fi

    if ! git -C "$AUR_DIR" fetch --quiet "$AUR_REMOTE" "$AUR_REMOTE_BRANCH"; then
        print_error "Could not fetch $AUR_REMOTE/$AUR_REMOTE_BRANCH"
        return 1
    fi
    local ahead
    ahead=$(git -C "$AUR_DIR" rev-list --count "$AUR_REMOTE/$AUR_REMOTE_BRANCH..$AUR_LOCAL_BRANCH")
    if [ "$ahead" -eq 0 ]; then
        print_success "AUR already up to date; nothing to push"
        return 0
    fi

    if ! git -C "$AUR_DIR" push "$AUR_REMOTE" "$AUR_LOCAL_BRANCH:$AUR_REMOTE_BRANCH"; then
        print_error "Pushing to $AUR_REMOTE failed"
        return 1
    fi
    print_success "Pushed $ahead commit(s) to $AUR_REMOTE/$AUR_REMOTE_BRANCH"
}

# Push to the AUR, or print the manual steps when pushing is disabled or fails
publish_aur_package() {
    if [ "$AUR_PUSH" = 1 ] && push_aur_package; then
        return 0
    fi
    show_push_instructions "$@"
    # A failed push is an error; a disabled one is not
    [ "$AUR_PUSH" != 1 ]
}

# Show manual push instructions
show_push_instructions() {
    local version=$1
    local tag_status=${2:-created}

    echo
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  MANUAL PUSH REQUIRED${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo
    print_warning "Please run this command manually:"
    echo
    echo "# Push AUR package update:"
    echo -e "${YELLOW}git -C $AUR_DIR push $AUR_REMOTE $AUR_LOCAL_BRANCH:$AUR_REMOTE_BRANCH${NC}"
    echo
    if [ "$tag_status" = "created" ]; then
        print_step "The tag has been created and pushed automatically."
    else
        print_step "The existing tag '$version' was reused; no tag was created."
    fi
    print_step "After pushing the AUR changes, the package will be ready for AUR submission."
}

# Main script
main() {
    local version=""
    local tag_status="created"

    # Parse arguments
    while [ $# -gt 0 ]; do
        case "$1" in
            --update|--force)
                print_warning "$1 is no longer needed; existing tags are handled automatically"
                ;;
            --*)
                print_error "Unknown option: $1"
                exit 1
                ;;
            *)
                if [ -n "$version" ]; then
                    print_error "Only one version may be specified"
                    exit 1
                fi
                version=$1
                ;;
        esac
        shift
    done

    print_header

    # Check if we're in a git repository
    if ! git rev-parse --git-dir > /dev/null 2>&1; then
        print_error "Not in a git repository"
        exit 1
    fi

    # With no version, prepare the most recent release tag.
    if [ -z "$version" ]; then
        version=$(get_latest_tag)
        if [ -z "$version" ]; then
            print_error "No version tag found; specify a version explicitly"
            exit 1
        fi
        print_step "No version specified; using latest tag '$version'"
    fi

    # Validate version format
    print_step "Validating version format..."
    version=$(validate_version "$version")
    print_success "Version format valid: $version"

    # Check if on main branch
    local current_branch
    current_branch=$(git branch --show-current)
    if [ "$current_branch" != "$MAIN_BRANCH" ]; then
        print_warning "Not on $MAIN_BRANCH branch (currently on: $current_branch)"
        echo "Continue anyway? (y/N)"
        read -r response
        if [[ ! "$response" =~ ^[Yy]$ ]]; then
            print_step "Aborted by user"
            exit 0
        fi
    fi

    # Reuse an existing tag instead of failing or attempting to recreate it.
    print_step "Checking if tag '$version' already exists..."
    if tag_exists "$version"; then
        print_warning "Tag '$version' already exists; no new tag will be created"
        tag_status="existing"
        ANNOTATION=$(get_tag_annotation "$version")
        if [ -z "$ANNOTATION" ]; then
            ANNOTATION="Update AUR package to $version"
        fi
    else
        print_success "Tag '$version' is available"

        # Compare with latest tag only when creating a new tag.
        print_step "Checking version progression..."
        local latest_tag
        latest_tag=$(get_latest_tag)
        local clean_version=${version#v}

        if [ -n "$latest_tag" ] && ! compare_versions "$clean_version" "$latest_tag"; then
            print_error "Version $clean_version is not higher than latest tag $latest_tag"
            exit 1
        fi
        if [ -n "$latest_tag" ]; then
            print_success "Version $version > $latest_tag"
        fi

        # Prompt for annotation
        prompt_annotation "$version"
    fi
    local annotation="$ANNOTATION"

    if [ "$tag_status" = "created" ]; then
        # Confirm before proceeding
        echo
        print_warning "About to create tag '$version' with annotation:"
        echo "$annotation"
        echo
        echo "Continue? (y/N)"
        read -r response
        if [[ ! "$response" =~ ^[Yy]$ ]]; then
            print_step "Aborted by user"
            exit 0
        fi

        # Create and push the tag (needed for checksum calculation)
        create_tag "$version" "$annotation"
        print_step "Pushing tag to GitHub..."
        git push "$TAG_REMOTE" "$version"
        print_success "Tag pushed to GitHub"
    fi

    # Update AUR package (now with real checksum)
    update_aur_package "$version" "$annotation"

    # Show manual push instructions (only AUR now)
    publish_aur_package "$version" "$tag_status"

    echo
    print_success "Release preparation complete!"
}

# Run main function with all arguments
main "$@"
