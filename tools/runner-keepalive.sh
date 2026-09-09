#!/bin/bash
#
# Restart the self-hosted GitHub Actions listener if it is not running.
#
#   */10 * * * * $HOME/actions-runner/keepalive.sh
#
# WHY THIS EXISTS.  playhouse3's crontab had one entry, `@reboot`, which covers
# a reboot and nothing else.  On 2026-08-27 the listener lost its broker
# connection and exited -- `_diag/Runner_*.log` ends with
#
#     [ERR  BrokerServer] Catch exception during request
#
# -- and stayed dead for thirteen days.  GitHub reported the runner `offline`
# while emulator.yml queued run after run against it, which is the whole of
# that tier's "0 passes in 25 runs".  Not a broken workflow: a dead process
# nothing was watching.  A reboot-only guard cannot see a mid-life death.
#
# A system unit would be tidier and needs root; `systemctl --user` needs
# lingering, which also needs root.  cron is what is available unprivileged.
#
# THREE DETAILS, EACH OF WHICH WAS WRONG FIRST:
#
#   flock -- two cron ticks must not race a second listener into existence.
#
#   The pgrep pattern is `bin/Runner\.Listener run`, not "actions-runner": a
#   directory-name pattern matches THIS SCRIPT'S own command line and reports
#   the runner alive when it is dead.  That self-match produced a wrong
#   diagnosis on the day this was written.  Nor is it `bin/Runner.Listener`
#   alone, which also matches a Runner.Worker executing a job -- the guard
#   would then go quiet exactly while a job is running.
#
#   The stamp file, because this script logs only when it STARTS something.
#   With no stamp, "nothing in the log" reads identically whether cron ran it
#   and found the runner healthy or cron never ran it at all -- the same
#   ambiguity that let the tier sit dead behind an @reboot line that was
#   present, correct, and never fired.  A fresh stamp means the guard is alive;
#   a stale one means the guard is what broke.
#
# SPDX-License-Identifier: MIT

set -u

exec 9> /tmp/anxd-runner-keepalive.lock
flock -n 9 || exit 0

date -Is > "$HOME/actions-runner/keepalive.stamp"

pgrep -u "$(id -u)" -f "bin/Runner\.Listener run" > /dev/null 2>&1 && exit 0

cd "$HOME/actions-runner" || exit 1
echo "[$(date -Is)] listener not running, starting it" >> "$HOME/actions-runner/keepalive.log"
setsid nohup ./run.sh >> "$HOME/actions-runner/run.log" 2>&1 < /dev/null &
