#!/bin/sh
# Read-only diagnosis of /dev/uinput access for the ChromeDrive virtual
# steering wheel (src/wheel_input.c). Prints what the compositor will find,
# exits 0 when the joystick can start, 1 when it would be disabled.
#
# Never changes anything: no chmod, no chown, no group edits, no sudo.
# Host-setup remediation is tools/grant-uinput.sh (run by the human, never
# invoked by the compositor - AGENTS.md section 5).
set -eu
umask 022

QUIET=0
NODE=
while [ $# -gt 0 ]; do
	case $1 in
	-q|--quiet) QUIET=1; shift ;;
	--node) [ $# -ge 2 ] || { printf '--node needs a value\n' >&2; exit 2; }; NODE=$2; shift 2 ;;
	-h|--help)
		printf 'Usage: %s [--quiet] [--node PATH]\n' "${0##*/}"
		printf 'Exit 0 when this user can open /dev/uinput, 1 otherwise.\n'
		printf '--node checks one specific device node instead of the defaults\n'
		printf '(useful in containers or for tests).\n'
		exit 0 ;;
	*)
		printf 'Unknown argument: %s (try --help)\n' "$1" >&2
		exit 2 ;;
	esac
done

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
user=$(id -un)
groups=$(id -nG)
exit_code=0

say() { [ "$QUIET" = 1 ] || printf '%s\n' "$*"; }
note() { [ "$QUIET" = 1 ] || printf '        %s\n' "$*"; }
ok() { [ "$QUIET" = 1 ] || printf '  ok    %s\n' "$*"; }
warn() { [ "$QUIET" = 1 ] || printf '  warn  %s\n' "$*"; }
bad() { [ "$QUIET" = 1 ] || printf '  FAIL  %s\n' "$*"; exit_code=1; }

in_group() { # group
	case " $groups " in
	*" $1 "*) return 0 ;;
	*) return 1 ;;
	esac
}

say "ChromeDrive uinput access check for user '$user'"

# 1. Kernel support. The node cannot exist without the module, and the
#    compositor's best-effort mknod() needs root, so a missing module means a
#    missing node on every boot.
if [ -d /sys/module/uinput ]; then
	ok "uinput kernel module is loaded"
else
	bad "uinput kernel module is not loaded"
	say "        fix: echo uinput | sudo tee /etc/modules-load.d/uinput.conf && sudo modprobe uinput"
fi

# 2. Device node. src/wheel_input.c tries both paths, in this order.
node=
if [ -n "$NODE" ]; then
	if [ -e "$NODE" ]; then
		node=$NODE
	else
		bad "no such device node: $NODE"
		exit 1
	fi
else
	for candidate in /dev/uinput /dev/input/uinput; do
		if [ -e "$candidate" ]; then
			node=$candidate
			break
		fi
	done
fi
if [ -z "$node" ]; then
	bad "no /dev/uinput or /dev/input/uinput node exists"
	say "        fix: sudo modprobe uinput   (and load it at boot, see above)"
	say "        the compositor will not create it without root; the joystick stays off"
	say ""
	say "The compositor, Wayland clients and managed VT input still run without"
	say "the virtual steering wheel. Nothing else needs elevated privileges."
	exit 1
fi
ok "device node: $node"

# 3. Ownership and mode, then the two mechanisms that can actually grant write
#    access: the owning group, or a per-user ACL entry (systemd-logind uaccess).
owner=$(stat -c '%U' "$node" 2>/dev/null || echo '?')
group=$(stat -c '%G' "$node" 2>/dev/null || echo '?')
mode=$(stat -c '%A' "$node" 2>/dev/null || echo '?')
say "        owner=$owner group=$group mode=$mode"

writable=0
if ( exec 3<>"$node" ) 2>/dev/null; then
	writable=1
fi

if [ "$writable" = 1 ]; then
	ok "open($node, O_RDWR) succeeded as '$user'"
else
	bad "open($node, O_RDWR) denied for '$user'"
fi

# 4. Why it works (or does not). Either mechanism is sufficient, so these are
#    diagnostics, not pass/fail on their own. Note that with an ACL present the
#    group bits printed by ls(1) are the ACL *mask*, not the group's own rights:
#    'ls -l' can show rw- for a group whose members still get nothing.
acl_group=
acl_mask=
acl_user=
if command -v getfacl >/dev/null 2>&1 && acl=$(getfacl -p "$node" 2>/dev/null); then
	acl_group=$(printf '%s\n' "$acl" | sed -n 's/^group::\(.*\)$/\1/p')
	acl_mask=$(printf '%s\n' "$acl" | sed -n 's/^mask::\(.*\)$/\1/p')
	acl_user=$(printf '%s\n' "$acl" | sed -n "s/^user:$user:\(.*\)\$/\1/p")
fi

if [ -n "$acl_group" ]; then
	case $acl_group in
	*w*)
		if in_group "$group"; then
			ok "group '$group' grants write (acl group::$acl_group, mask::$acl_mask)"
		else
			ok "group '$group' has write (acl group::$acl_group); you are not in it"
		fi ;;
	*)
		if in_group "$group"; then
			if [ "$writable" = 1 ]; then
				note "you are in '$group' but its acl group entry is '$acl_group' - access comes from a per-user ACL, not the group"
			else
				warn "you are in '$group' but its acl group entry is '$acl_group' - membership alone grants nothing"
			fi
		else
			note "group '$group' has no write (acl group::$acl_group)"
		fi ;;
	esac
fi

if [ -n "$acl_user" ]; then
	ok "ACL grants user:$user:$acl_user (systemd-logind uaccess: needs an active seat session)"
elif [ -n "$acl_group" ]; then
	warn "no ACL entry for user:$user (uaccess not applied to this session)"
fi

if [ "$writable" = 1 ]; then
	say ""
	say "The virtual steering wheel (045e:028e, ABS_X + ABS_WHEEL) can start."
	say "Games will see it through SDL2/evdev; the compositor never grabs it."
	exit 0
fi

say ""
say "The virtual steering wheel would be disabled; everything else still runs."
say "Grant access as a host setup step (do NOT run the compositor through sudo):"
say ""
say "    sudo $here/grant-uinput.sh            # group + udev rule, idempotent"
say "    sudo $here/grant-uinput.sh --dry-run  # print the plan first"
say ""
say "Then start a NEW login session (group membership is read at login) and"
say "re-run this check. Manual equivalent:"
say ""
say "    sudo groupadd --system uinput"
say "    sudo usermod --append --groups uinput \"$user\""
say "    sudo tee /etc/udev/rules.d/99-chromedrive-uinput.rules >/dev/null <<'EOF'"
say "    KERNEL==\"uinput\", SUBSYSTEM==\"misc\", GROUP=\"uinput\", MODE=\"0660\""
say "    EOF"
say "    sudo udevadm control --reload && sudo udevadm trigger --name-match=uinput"
say ""
say "Do not chmod 666 /dev/uinput: any local user could then inject input."
exit "$exit_code"
