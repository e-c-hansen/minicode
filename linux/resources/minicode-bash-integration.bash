# minicode's shell integration for bash, in the Linux port's terminals
# (linux/PARITY.md D10, linux/HANDOFF.md item 48).
#
# Terminal.cpp starts an interactive, non-login bash as
#   bash --rcfile <this file's copy>
# from a folder of the running app's own, so an older app never reads a
# newer install's script. bash reads /etc/bash.bashrc itself first
# (Debian's and Ubuntu's system file, which --rcfile does not replace);
# this file then reads ~/.bashrc, which is everything a non-login
# interactive bash reads, and adds two hooks that only print:
#   PROMPT_COMMAND  ours last, after the user's (and a first entry that
#                   keeps $? for theirs)
#   PS0             bash 4.4 and later: expanded once a line is read, before
#                   it runs; an arithmetic expansion moves the counter in
#                   the shell itself, a command substitution prints
# Each prints one string termprop VTE keeps for the app,
#   ESC ] 666 ; vte.ext.minicode.mark=p=<n>\sx=<n>\sd=<n>\ss=<status>\sc=<command>\smc=<nonce> ESC \
# (VTE splits OSC 666 at ';' and unescapes \s, \\ and \n in a string), with
# p prompts, x command lines started, d finished (set to x at the prompt
# after them), s the last one's status and c its text. The text comes from
# the history, so a line the history did not keep (a leading space under
# ignorespace, a repeat under ignoredups, history off) has none. The nonce
# (MINICODE_NONCE, 128 random bits per shell) is read and taken out of the
# environment before ~/.bashrc runs; the app believes no value without it.
# The user's prompt, history, options and their own PROMPT_COMMAND and PS0
# are left as they are. No DEBUG trap, which would run for every command.
#
# ShellIntegration.h describes the value; linux/tests/gui's shell_integration
# suite and make test's bash:integration run this file in a real bash.

__minicode_nonce=${MINICODE_NONCE-}
builtin unset MINICODE_NONCE

if [[ -r ~/.bashrc ]]; then
  builtin source ~/.bashrc
fi

if [[ -n $__minicode_nonce ]] &&
   (( BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4) )); then
  __minicode_p=0 __minicode_x=0 __minicode_d=0 __minicode_s= __minicode_rc=0 __minicode_h=0

  # The mark, with the command's text ($1): control characters blanked, at
  # most 500 characters (VTE drops a value over 1,024), then escaped.
  __minicode_mark() {
    local c=${1:0:500}
    c=${c//[[:cntrl:]]/ }
    c=${c//\\/\\\\}
    c=${c//;/\\s}
    builtin printf '\e]666;vte.ext.minicode.mark=p=%s\\sx=%s\\sd=%s\\ss=%s\\sc=%s\\smc=%s\e\\' \
      "$__minicode_p" "$__minicode_x" "$__minicode_d" "$__minicode_s" "$c" "$__minicode_nonce"
  }
  # First in PROMPT_COMMAND: the line's status, handed on unchanged.
  __minicode_status() {
    __minicode_rc=$?
    return $__minicode_rc
  }
  # Last in PROMPT_COMMAND: a line that started (x past d) has finished.
  __minicode_precmd() {
    if (( __minicode_x > __minicode_d )); then
      __minicode_d=$__minicode_x
      __minicode_s=$__minicode_rc
    fi
    __minicode_p=$((__minicode_p + 1))
    __minicode_h=$HISTCMD
    __minicode_mark ""
    return $__minicode_rc
  }
  # PS0's command substitution, in a subshell after the counter moved. The
  # history holds the line only when HISTCMD moved past the prompt's.
  __minicode_preexec() {
    local c=
    if (( HISTCMD > __minicode_h )); then
      c=$(HISTTIMEFORMAT= builtin history 1)
      if [[ $c =~ ^\ *[0-9]+[\ *]\ (.*)$ ]]; then
        c=${BASH_REMATCH[1]}
      else
        c=
      fi
    fi
    __minicode_mark "$c"
  }

  if (( BASH_VERSINFO[0] > 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] >= 1) )); then
    PROMPT_COMMAND=(__minicode_status ${PROMPT_COMMAND[@]+"${PROMPT_COMMAND[@]}"} __minicode_precmd)
  else
    PROMPT_COMMAND=$'__minicode_status\n'"${PROMPT_COMMAND-}"$'\n__minicode_precmd'
  fi
  PS0=${PS0-}'${BASH_VERSION:0:$((__minicode_x+=1,0))}$(__minicode_preexec)'
fi
