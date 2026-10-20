# root logs in with bash (post-build.sh): history, completion-friendly prompt
if [ -n "$BASH_VERSION" ]; then
  PS1='\[\e[1;32m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
  HISTFILE=/root/.bash_history HISTSIZE=1000 HISTCONTROL=ignoredups
  alias ll='ls -la'
fi
# station role at login
case "$-" in *i*) echo "Role: $(fw_printenv -n nexus_role 2>/dev/null || echo slave)  (nexus-role slave|master)"; echo ;; esac
