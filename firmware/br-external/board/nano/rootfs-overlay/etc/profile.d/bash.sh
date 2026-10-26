# root logs in with bash (post-build.sh): history, completion-friendly prompt
if [ -n "$BASH_VERSION" ]; then
  PS1='\[\e[1;32m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
  HISTFILE=/root/.bash_history HISTSIZE=1000 HISTCONTROL=ignoredups
  alias ll='ls -la'
fi
# station role at login
# (master|light|slave; an unknown operating_mode counts as slave, as in S97app)
case "$-" in *i*) r=$(fw_printenv -n operating_mode 2>/dev/null); case "$r" in master|light) ;; *) r=slave ;; esac
	echo "Role: $r  -  configuration: nexus-config"; echo; unset r ;; esac
