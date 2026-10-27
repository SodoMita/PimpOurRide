#!/bin/sh
# Host setup for the ChromeDrive virtual steering wheel (src/wheel_input.c):
# gives one login user write access to /dev/uinput through a dedicated group
# and a udev rule, so the compositor can create its ABS_X/ABS_WHEEL joystick.
#
#   sudo tools/grant-uinput.sh            # grant for $SUDO_USER
#   sudo tools/grant-uinput.sh --user alice
#   sudo tools/grant-uinput.sh --dry-run  # print the plan, change nothing
#   sudo tools/grant-uinput.sh --remove   # undo the rule and the membership
#
# This is setup policy, not runtime behaviour. The compositor NEVER calls this
# script, never runs sudo/pkexec/askpass, and must not itself be started with
# sudo (AGENTS.md section 5). Denial of /dev/uinput only disables the virtual
# joystick; Wayland input and managed VT handling keep working.
set -eu
umask 022

RULE_DIR=${CHROMEDRIVE_UINPUT_RULE_DIR:-/etc/udev/rules.d}
RULE_NAME=99-chromedrive-uinput.rules
MODULES_CONF=${CHROMEDRIVE_UINPUT_MODULES_CONF:-/etc/modules-load.d/uinput.conf}
GROUP=uinput
TARGET_USER=
DRY_RUN=0
REMOVE=0

usage() {
	cat <<'EOF'
Usage: grant-uinput.sh [options]

Options:
  --user NAME         login user to grant (default: $SUDO_USER)
  --group NAME        group that owns /dev/uinput (default: uinput)
  --rule-name NAME    udev rule filename (default: 99-chromedrive-uinput.rules)
  --dry-run           print every action without performing it
  --remove            delete the udev rule, the module-load file and the
                      user's group membership again
  -h, --help          this text

Must be run as root (sudo). Never run the compositor itself through sudo.

CHROMEDRIVE_UINPUT_RULE_DIR and CHROMEDRIVE_UINPUT_MODULES_CONF override the
two install paths; they exist so this script can be exercised end to end in a
throwaway directory without touching the real host configuration.
EOF
}

die() { printf '%s: %s\n' "${0##*/}" "$*" >&2; exit 1; }
note() { printf '%s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

while [ $# -gt 0 ]; do
	case $1 in
	--user) [ $# -ge 2 ] || die '--user needs a value'; TARGET_USER=$2; shift 2 ;;
	--group) [ $# -ge 2 ] || die '--group needs a value'; GROUP=$2; shift 2 ;;
	--rule-name) [ $# -ge 2 ] || die '--rule-name needs a value'; RULE_NAME=$2; shift 2 ;;
	--dry-run) DRY_RUN=1; shift ;;
	--remove) REMOVE=1; shift ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; die "unknown argument: $1" ;;
	esac
done

case $RULE_NAME in
*.rules) ;;
*) die "--rule-name must end in .rules (udev only reads *.rules)" ;;
esac
case $RULE_NAME in
*/*) die "--rule-name must be a bare filename, not a path" ;;
esac
case $GROUP in
''|*[!A-Za-z0-9_-]*) die "--group must be a plain group name" ;;
esac
RULE=$RULE_DIR/$RULE_NAME

# --- who are we -------------------------------------------------------------
if [ "$(id -u)" -ne 0 ]; then
	die "must run as root: sudo ${0}${TARGET_USER:+ --user $TARGET_USER}"
fi

if [ -z "$TARGET_USER" ]; then
	TARGET_USER=${SUDO_USER:-}
	if [ -z "$TARGET_USER" ] || [ "$TARGET_USER" = root ]; then
		die 'cannot tell which user to grant; pass --user NAME (or run via sudo from that user)'
	fi
fi
id -u "$TARGET_USER" >/dev/null 2>&1 || die "no such user: $TARGET_USER"
[ "$TARGET_USER" != root ] || die 'root already has access; grant a normal login user instead'

run() {
	if [ "$DRY_RUN" = 1 ]; then
		printf '   would run: %s\n' "$*"
	else
		"$@"
	fi
}

# Write a root-owned config file atomically (temp file + rename) so a half
# written udev rule can never be picked up by udevadm.
write_file() { # path, content
	wf_path=$1
	wf_content=$2
	if [ "$DRY_RUN" = 1 ]; then
		printf '   would write %s:\n' "$wf_path"
		printf '%s\n' "$wf_content" | sed 's/^/     | /'
		return 0
	fi
	wf_tmp=$wf_path.chromedrive-tmp.$$
	mkdir -p "$(dirname "$wf_path")"
	printf '%s\n' "$wf_content" > "$wf_tmp"
	chmod 0644 "$wf_tmp"
	mv -f "$wf_tmp" "$wf_path"
}

has_group() { getent group "$1" >/dev/null 2>&1; }
user_in_group() { id -nG "$1" 2>/dev/null | tr ' ' '\n' | grep -qx "$2"; }

# --- undo -------------------------------------------------------------------
if [ "$REMOVE" = 1 ]; then
	step "removing uinput grant"
	if [ -f "$RULE" ]; then
		run rm -f "$RULE"
		if [ "$DRY_RUN" = 1 ]; then note "would remove $RULE"; else note "removed $RULE"; fi
	else
		note "no $RULE (nothing to remove)"
	fi
	if has_group "$GROUP" && user_in_group "$TARGET_USER" "$GROUP"; then
		if command -v gpasswd >/dev/null 2>&1; then
			run gpasswd -d "$TARGET_USER" "$GROUP" || remove_failed=1
		elif command -v deluser >/dev/null 2>&1; then
			run deluser "$TARGET_USER" "$GROUP" || remove_failed=1
		else
			note "no gpasswd/deluser here; remove $TARGET_USER from '$GROUP' manually"
			remove_failed=1
		fi
		if [ "${remove_failed:-0}" = 1 ]; then
			note "could not drop the membership; the udev rule removal above still stands"
		elif [ "$DRY_RUN" = 1 ]; then
			note "would drop $TARGET_USER from $GROUP"
		else
			note "dropped $TARGET_USER from $GROUP"
		fi
	else
		note "$TARGET_USER is not in group '$GROUP' (nothing to remove)"
	fi
	if [ -f "$MODULES_CONF" ]; then
		run rm -f "$MODULES_CONF"
		if [ "$DRY_RUN" = 1 ]; then
			note "would remove $MODULES_CONF"
		else
			note "removed $MODULES_CONF (uinput will load on first open instead)"
		fi
	fi
	if command -v udevadm >/dev/null 2>&1; then
		run udevadm control --reload
		run udevadm trigger --name-match=uinput
	fi
	step "done"
	note "Group '$GROUP' was left in place; remove it with 'groupdel $GROUP' if"
	note "nothing else uses it. Log out and back in for the group change to apply."
	exit "${remove_failed:-0}"
fi

# --- preconditions ----------------------------------------------------------
command -v udevadm >/dev/null 2>&1 || die 'udevadm not found: this recipe needs a udev/systemd host (adapt it to the distribution seat/ACL policy)'
command -v groupadd >/dev/null 2>&1 || die 'groupadd not found: create the group with the distribution tool, then re-run'
[ -d "$RULE_DIR" ] || die "$RULE_DIR does not exist; is udev installed?"

step "plan"
note "user:   $TARGET_USER"
note "group:  $GROUP"
note "rule:   $RULE"
if [ "$DRY_RUN" = 1 ]; then
	note "mode:   dry run (nothing will be changed)"
fi

# --- group ------------------------------------------------------------------
step "group '$GROUP'"
if has_group "$GROUP"; then
	note "already exists"
else
	if command -v groupadd >/dev/null 2>&1; then
		run groupadd --system "$GROUP"
	else
		run addgroup --system "$GROUP"
	fi
fi
if user_in_group "$TARGET_USER" "$GROUP"; then
	note "$TARGET_USER is already a member"
else
	run usermod --append --groups "$GROUP" "$TARGET_USER"
	note "added $TARGET_USER to $GROUP"
fi

# --- module -----------------------------------------------------------------
step "uinput kernel module"
if [ -f "$MODULES_CONF" ]; then
	note "$MODULES_CONF already present"
else
	write_file "$MODULES_CONF" 'uinput'
fi
if [ -d /sys/module/uinput ]; then
	note "module already loaded"
elif command -v modprobe >/dev/null 2>&1; then
	run modprobe uinput
else
	note "modprobe not found; the node appears on first open if the kernel has uinput"
fi

# --- udev rule --------------------------------------------------------------
step "udev rule"
rule_body="# ChromeDrive: let the '$GROUP' group create/use the virtual steering wheel.
# Installed by tools/grant-uinput.sh (host setup; the compositor never runs this).
KERNEL==\"uinput\", SUBSYSTEM==\"misc\", GROUP=\"$GROUP\", MODE=\"0660\"
ACTION==\"add\", SUBSYSTEM==\"input\", ATTRS{name}==\"ChromeDrive Virtual Steering Wheel\", GROUP=\"$GROUP\", MODE=\"0660\""

if [ -f "$RULE" ] && [ "$(cat "$RULE")" = "$rule_body" ]; then
	note "$RULE already up to date"
else
	write_file "$RULE" "$rule_body"
fi

# Later-sorting rules win in udev, so report anything that could override GROUP.
conflicts=
for f in "$RULE_DIR"/*.rules /usr/lib/udev/rules.d/*.rules; do
	[ -f "$f" ] || continue
	[ "${f##*/}" = "$RULE_NAME" ] && continue
	grep -q 'uinput' "$f" 2>/dev/null || continue
	grep -q 'GROUP=' "$f" 2>/dev/null || continue
	base=${f##*/}
	if [ "$(printf '%s\n%s\n' "$RULE_NAME" "$base" | LC_ALL=C sort | tail -n1)" = "$base" ]; then
		conflicts="$conflicts $f"
	fi
done
if [ -n "$conflicts" ]; then
	note "warning: these rules sort after $RULE_NAME and may override GROUP=:"
	for f in $conflicts; do note "  $f"; done
	note "  re-run with --rule-name 99-zz-chromedrive-uinput.rules to win instead"
fi

if command -v udevadm >/dev/null 2>&1; then
	run udevadm control --reload
	run udevadm trigger --name-match=uinput
fi

# --- verify -----------------------------------------------------------------
step "verify"
if [ "$DRY_RUN" = 1 ]; then
	note "skipped in dry run"
	exit 0
fi

node=
for candidate in /dev/uinput /dev/input/uinput; do
	if [ -e "$candidate" ]; then node=$candidate; break; fi
done
if [ -z "$node" ]; then
	note "no uinput node yet; it will appear after 'sudo modprobe uinput' or a reboot"
	note "the compositor will then start the joystick; without it only the joystick is disabled"
	exit 0
fi

note "$node: owner=$(stat -c '%U' "$node") group=$(stat -c '%G' "$node") mode=$(stat -c '%A' "$node")"
node_group=$(stat -c '%G' "$node")
node_gwrite=$(stat -c '%A' "$node" | cut -c6)
if [ "$node_group" != "$GROUP" ]; then
	note "the node is still in group '$node_group' - a later udev rule overrides GROUP= (see warning above)"
elif [ "$node_gwrite" != w ]; then
	note "group '$GROUP' has no write bit on the node - check for an ACL mask or a later rule"
elif command -v sg >/dev/null 2>&1 && sg "$GROUP" -c "test -w '$node'" 2>/dev/null; then
	note "writable for members of '$GROUP'"
else
	note "group '$GROUP' owns the node with write access; log in again to pick up"
	note "your new membership, then run ./tools/check-uinput.sh"
fi

step "next"
note "Log out and back in (group membership is read at login), then:"
note "  ./tools/check-uinput.sh"
note "Do not start the compositor with sudo, and do not chmod 666 the node."
