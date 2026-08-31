# Runs every per-ISA suite (test/<isa>/run.sh); stops on the first failing
# suite. Per-suite output comes from each runner.
set -e
dir=$(cd "$(dirname "$0")" && pwd)
for isa in "$dir"/*/; do
  [ -f "$isa/run.sh" ] || continue
  echo "== $(basename "$isa") =="
  sh "$isa/run.sh"
done
