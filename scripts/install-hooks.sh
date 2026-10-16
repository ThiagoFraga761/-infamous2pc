#!/usr/bin/env bash
# Run scripts/check_clean.sh before every commit and push of this clone.
set -eu
root="$(git rev-parse --show-toplevel)"
hooks="$(git rev-parse --git-path hooks)"
mkdir -p "$hooks"
printf '#!/usr/bin/env bash\nexec "%s/scripts/check_clean.sh"\n' "$root" > "$hooks/pre-commit"
printf '#!/usr/bin/env bash\nexec "%s/scripts/check_clean.sh" --tracked\n' "$root" > "$hooks/pre-push"
chmod +x "$hooks/pre-commit" "$hooks/pre-push"
echo "hooks installed in $hooks"
