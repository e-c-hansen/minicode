# mutible's shell integration for bash, in the Linux port's terminals
# (linux/PARITY.md D10, linux/HANDOFF.md item 48; SESSION-DESIGN.md stage 5).
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
# They print the marks the Mac's zsh integration prints, which the session's
# host reads from the pty's bytes (TerminalStream): at the prompt
#   ESC ] 133;D;<status> BEL   when a line ran since the last prompt
#   ESC ] 7;file://<host><folder> BEL, then ESC ] 133;A BEL
# and when a line starts
#   ESC ] 633;E;<the line> BEL, then ESC ] 133;C BEL
# each ending in ";mc=<nonce>" before its BEL. The line comes from the
# history, so one the history did not keep (a leading space under
# ignorespace, a repeat under ignoredups, history off) has none; control
# characters in it are blanked. The nonce (MUTIBLE_NONCE, 128 random bits
# per shell) is read and taken out of the environment before ~/.bashrc
# runs; the host believes no mark without it. The user's prompt, history,
# options and their own PROMPT_COMMAND and PS0 are left as they are. No
# DEBUG trap, which would run for every command.
#
# linux/tests/gui's shell_integration suite and make test's bash:integration
# run this file in a real bash.

__mutible_nonce=${MUTIBLE_NONCE-}
builtin unset MUTIBLE_NONCE

if [[ -r ~/.bashrc ]]; then
  builtin source ~/.bashrc
fi

if [[ -n $__mutible_nonce ]] &&
   (( BASH_VERSINFO[0] > 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] >= 4) )); then
  __mutible_x=0 __mutible_d=0 __mutible_rc=0 __mutible_h=0

  # A mark, with the nonce the host checks.
  __mutible_mark() {
    builtin printf '\e]%s;mc=%s\a' "$1" "$__mutible_nonce"
  }
  # First in PROMPT_COMMAND: the line's status, handed on unchanged.
  __mutible_status() {
    __mutible_rc=$?
    return $__mutible_rc
  }
  # The folder for OSC 7: %, ; and control characters percent-encoded, so a
  # folder whose name holds BEL or ESC cannot end the mark early.
  __mutible_cwd() {
    local p=$PWD out= c i
    if [[ $p == *[%\;[:cntrl:]]* ]]; then
      for (( i = 0; i < ${#p}; i++ )); do
        c=${p:i:1}
        if [[ $c == [%\;[:cntrl:]] ]]; then
          builtin printf -v c '%%%02X' "'$c"
        fi
        out+=$c
      done
      p=$out
    fi
    __mutible_mark "7;file://${HOSTNAME-}$p"
  }
  # Last in PROMPT_COMMAND: a line that started (x past d) has finished.
  __mutible_precmd() {
    if (( __mutible_x > __mutible_d )); then
      __mutible_d=$__mutible_x
      __mutible_mark "133;D;$__mutible_rc"
    fi
    __mutible_h=$HISTCMD
    __mutible_cwd
    __mutible_mark "133;A"
    return $__mutible_rc
  }
  # PS0's command substitution, in a subshell after the counter moved. The
  # history holds the line only when HISTCMD moved past the prompt's.
  __mutible_preexec() {
    local c=
    if (( HISTCMD > __mutible_h )); then
      c=$(HISTTIMEFORMAT= builtin history 1)
      if [[ $c =~ ^\ *[0-9]+[\ *]\ (.*)$ ]]; then
        c=${BASH_REMATCH[1]}
      else
        c=
      fi
    fi
    c=${c:0:4000}
    c=${c//[[:cntrl:]]/ }
    __mutible_mark "633;E;$c"
    __mutible_mark "133;C"
  }

  # The half-typed line (the Mac's ESC [9999~, zsh's push-input). The app
  # sends ESC [9999~ before it types a command at a prompt: the line goes
  # on a stack and the line is emptied, so ours is the whole line. Once the
  # command has finished and a prompt is back, the app sends ESC [9998~,
  # which puts the newest set-aside text in front of the line. readline's
  # kill ring would also do it, but a user's own Ctrl+Y or yank-pop in
  # between would change what comes back, and a kill would lose the text.
  __mutible_set=()
  __mutible_push() {
    if [[ -n $READLINE_LINE ]]; then
      __mutible_set+=("$READLINE_LINE")
      READLINE_LINE=
      READLINE_POINT=0
    fi
  }
  __mutible_pop() {
    local n=${#__mutible_set[@]}
    (( n )) || return 0
    local t=${__mutible_set[n-1]}
    builtin unset '__mutible_set[n-1]'
    READLINE_LINE=$t$READLINE_LINE
    READLINE_POINT=$((${#t} + READLINE_POINT))
  }
  for __mutible_m in emacs vi-insert vi-command; do
    builtin bind -m $__mutible_m -x '"\e[9999~": __mutible_push' 2>/dev/null
    builtin bind -m $__mutible_m -x '"\e[9998~": __mutible_pop' 2>/dev/null
  done
  builtin unset __mutible_m

  if (( BASH_VERSINFO[0] > 5 || (BASH_VERSINFO[0] == 5 && BASH_VERSINFO[1] >= 1) )); then
    PROMPT_COMMAND=(__mutible_status ${PROMPT_COMMAND[@]+"${PROMPT_COMMAND[@]}"} __mutible_precmd)
  else
    PROMPT_COMMAND=$'__mutible_status\n'"${PROMPT_COMMAND-}"$'\n__mutible_precmd'
  fi
  # PS0 and the prompts are expanded only with promptvars; a .bashrc that
  # turned it off would see our text printed, so it is put back.
  shopt -s promptvars 2>/dev/null
  # The counter moves in an arithmetic expansion inside an OSC the terminal
  # ignores (7777), so even a bash that printed it would show nothing; no
  # parameter expansion (`${...}`) is left in the text, since ESC before
  # `$` `{` is a charset escape that eats them (CI's picture, November 2026).
  PS0=${PS0-}$'\e]7777;''$((__mutible_x+=1))'$'\a''$(__mutible_preexec)'
fi
