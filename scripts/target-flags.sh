# Sourced by kernel build scripts. Selects common/reims_target.hpp identity.
#   REIMS_TARGET=adlp (default)  Alder Lake-P 8086:46a3 rev 0c, upstream
#   REIMS_TARGET=rpls            Raptor Lake-S UHD 770 8086:a780 rev 04
#   REIMS_PCI_PATH=...           optional exact IOService path override
target_flags=()
case "${REIMS_TARGET:-adlp}" in
    adlp) ;;
    rpls) target_flags+=(-DREIMS_TARGET_RPLS) ;;
    *) echo "Unknown REIMS_TARGET=${REIMS_TARGET} (expected adlp or rpls)" >&2; exit 64 ;;
esac
if [ -n "${REIMS_PCI_PATH:-}" ]; then
    target_flags+=("-DREIMS_PCI_PATH=\"${REIMS_PCI_PATH}\"")
fi
# Expand as ${target_flags[@]+"${target_flags[@]}"}: macOS ships bash 3.2,
# where an empty "${target_flags[@]}" trips `set -u`.
