# minicode's shell integration for zsh, for both terminal modes.
#
# minicode starts its shells with ZDOTDIR at a folder of its own holding a
# small .zshenv (ShellIntegration.h writes it, Terminal.mm puts it there with
# a copy of this file) that names the mode, grid or line, and sources this
# file. zsh reads $ZDOTDIR/.zshenv first; this file hands ZDOTDIR back to the
# user straight away, so their .zprofile, .zshrc and .zlogin load normally
# and child shells never see ours, except a zsh that takes this one's place
# through `exec` (__minicode_exec_hook).
#
# Grid mode: hooks that only report what the shell is doing, so the user's
# prompt, options and tty modes are left alone.
#   precmd   OSC 133;D;status (only after a command ran), OSC 7, OSC 133;A
#   ZLE init OSC 133;B: the line editor reads input (add-zle-hook-widget, so
#            the user's own zle-line-init keeps working; without it, B
#            follows A from precmd)
#   preexec  OSC 633;E;<the command line, control characters blanked>, then
#            OSC 133;C
# and private keys (ESC [9999~ to 9995~, sent by Terminal.mm) bound to
# push-input and ZLE's own line widgets.
#
# Line mode: zsh runs without its line editor (+Z); precmd blanks the prompt
# and turns echo off while zsh reads, and the same marks, without B and E,
# tell the panel what happens.
#
# Every mark carries this shell's nonce (MINICODE_NONCE, read here and taken
# out of the environment), since any program can print marks.
#
# make test checks this file (tests/zsh_integration_tests.inc).

typeset -g __minicode_zdotdir=$ZDOTDIR __minicode_exec_in=${MINICODE_EXEC-}
typeset -g __minicode_nonce=${MINICODE_NONCE-}
typeset -g __minicode_venv=${MINICODE_VENV-} __minicode_venv_prompt=${MINICODE_VENV_PROMPT-}
[[ ${__minicode_mode-} == line ]] || typeset -g __minicode_mode=grid
if [[ -n "${MINICODE_USER_ZDOTDIR+x}" ]]; then
  ZDOTDIR=$MINICODE_USER_ZDOTDIR
else
  unset ZDOTDIR
fi
unset MINICODE_USER_ZDOTDIR MINICODE_EXEC MINICODE_NONCE MINICODE_VENV MINICODE_VENV_PROMPT
[[ -f "${ZDOTDIR:-$HOME}/.zshenv" ]] && builtin source "${ZDOTDIR:-$HOME}/.zshenv"

if [[ -o interactive ]]; then
  # Line mode: no "%" end-of-line marker; these print before precmd, so
  # set them now.
  [[ $__minicode_mode == line ]] && unsetopt PROMPT_SP PROMPT_CR
  typeset -gi __minicode_ran=0 __minicode_no_b=0
  [[ -n $__minicode_exec_in ]] && __minicode_ran=1   # D for the `exec zsh`

  # `exec zsh` replaces this shell, and the new one would start without
  # these hooks: no prompt marks, so the tab stayed busy for good. The exec
  # itself (and nothing else on its line) gets ZDOTDIR at this file's
  # folder again (the user's own is passed along and handed back, as at
  # the start), MINICODE_EXEC, which tells the new shell to report the
  # command finished, and the nonce when the new shell will take it.
  # preexec runs before the whole line, so for a line with a plain exec it
  # puts a function named exec in front of the builtin (__minicode_exec),
  # which sets them just before exec runs: exported from preexec, `make &&
  # exec zsh` gave make the nonce, and everything make ran. The next
  # prompt takes the function away again.
  typeset -gi __minicode_execed=0 __minicode_exec_fn=0

  # Whether a zsh started with these arguments reads its startup files, and
  # so ours, which takes the nonce out of its environment: not with -f
  # (alone or in a cluster such as -fi), --no-rcs, -o norcs or +o rcs,
  # unless a later option turns them back on. Option names as zsh reads
  # them: any case, `_` and `-` ignored. Options end at the first word that
  # is not one, and at `--` or a lone `-`.
  __minicode_reads_rcs() {
    local rcs=1 a sign flags name c i
    while (( $# )); do
      a=$1; shift
      case $a in
        (--|-|+) break ;;
        (--*)
          name=${${(L)a#--}//[-_]/}
          if [[ $name == norcs ]]; then rcs=0
          elif [[ $name == rcs ]]; then rcs=1; fi ;;
        ([-+]?*)
          sign=$a[1] flags=$a[2,-1]
          for (( i = 1; i <= $#flags; i++ )); do
            c=$flags[i]
            if [[ $c == o ]]; then
              name=$flags[i+1,-1]
              if [[ -z $name ]]; then name=${1-}; (( $# )) && shift; fi
              name=${${(L)name}//[-_]/}
              # -o sets the option, +o unsets it; NO_RCS is RCS unset.
              if [[ $name == rcs ]]; then [[ $sign == - ]] && rcs=1 || rcs=0
              elif [[ $name == norcs ]]; then [[ $sign == - ]] && rcs=0 || rcs=1; fi
              break
            elif [[ $c == f ]]; then
              # -f is NO_RCS; +f turns it off again.
              [[ $sign == - ]] && rcs=0 || rcs=1
            fi
          done ;;
        (*) break ;;
      esac
    done
    (( rcs ))
  }

  # Whether the program exec's arguments start ($@: exec's own options
  # -c, -l and -a with its name, then the program and its arguments) is a
  # zsh that reads $ZDOTDIR/.zshenv, ours, which reads the nonce and takes
  # it out of its environment. Any other program (exec bash, exec claude)
  # would keep it in its environment for everything it runs, and any of
  # those could print marks the terminal believes; a zsh started from such
  # a shell later does not mark. A zsh emulating sh or ksh reads .profile
  # and $ENV instead: one started with --emulate, or whose name (exec -a's,
  # else the program's) begins with s, b or k after an optional r, as zsh
  # decides at startup (exec -a sh zsh). And one that reads no startup
  # files at all (__minicode_reads_rcs).
  __minicode_takes_nonce() {
    local name=
    while [[ ${1-} == -* ]]; do
      if [[ $1 == -- ]]; then shift; break; fi
      if [[ $1 == -*a ]]; then name=${2-}; shift; fi
      shift
    done
    (( $# )) || return 1
    [[ ${${1:t}#-} == zsh ]] || return 1
    [[ -n $name ]] || name=$1
    name=${${name:t}#-}
    [[ ${name#r} == [sbk]* ]] && return 1
    shift
    local a
    for a in "$@"; do
      [[ $a == --emulate(|=*) ]] && return 1
      [[ $a == [-+]* ]] || break
    done
    __minicode_reads_rcs "$@"
  }

  # The environment an exec's program gets ($@: exec's arguments), and
  # the shell's own back (only while MINICODE_EXEC says it was changed).
  __minicode_exec_env() {
    if [[ -n ${ZDOTDIR+x} ]]; then export MINICODE_USER_ZDOTDIR=$ZDOTDIR
    else unset MINICODE_USER_ZDOTDIR; fi
    export ZDOTDIR=$__minicode_zdotdir MINICODE_EXEC=1
    __minicode_takes_nonce "$@" && export MINICODE_NONCE=$__minicode_nonce
    return 0
  }
  __minicode_exec_restore() {
    [[ -n ${MINICODE_EXEC+x} ]] || return 0
    if [[ -n ${MINICODE_USER_ZDOTDIR+x} ]]; then ZDOTDIR=$MINICODE_USER_ZDOTDIR
    else unset ZDOTDIR; fi
    unset MINICODE_USER_ZDOTDIR MINICODE_EXEC MINICODE_NONCE
    return 0
  }
  # The function named exec while a line with an exec runs. An exec that
  # fails comes back here, and the line goes on with the shell's own
  # environment.
  __minicode_exec() {
    __minicode_exec_env "$@"
    builtin exec "$@"
    local rc=$?
    __minicode_exec_restore
    return $rc
  }

  # An exec counts where a command starts: after a separator, `then`,
  # `do`, `else`, `{` or `!`, and after `noglob`, `nocorrect` or `-` in
  # front of it. One in a subshell does not replace this shell. One after
  # `builtin` skips functions, so its environment is set here, and only
  # when it is the line's first command, which nothing on the line runs
  # before (`command exec` finds no exec at all without POSIX_BUILTINS). A
  # line with an exec of no program (exec >log, exec 3<&-) gets no
  # function: as a function call, its redirections would last only for
  # the call.
  __minicode_exec_hook() {
    local -a w; w=(${(z)1})
    local i j prev=';' builtin=0 seen=0 plain=0 direct=0
    for (( i = 1; i <= $#w; i++ )); do
      if [[ $prev == (';'|';;'|$'\n'|'&&'|'||'|'&'|'|'|'&|'|'&!'|'{'|'!'|then|do|else|elif) ]]; then
        if [[ $w[i] == (builtin|command) ]]; then builtin=1; continue; fi
        [[ $w[i] == (noglob|nocorrect|-) ]] && continue
        if [[ $w[i] == exec ]]; then
          # exec's own options: -c, -l, and -a with its argument.
          j=$(( i + 1 ))
          while [[ $w[j] == -* ]]; do
            if [[ $w[j] == -- ]]; then (( j++ )); break; fi
            [[ $w[j] == -*a ]] && (( j++ ))
            (( j++ ))
          done
          if (( j > $#w )) || [[ $w[j] == ((<->|)[\<\>]*|';'|';;'|$'\n'|'&&'|'||'|'&'|'|'|'&|'|'&!'|'}') ]]; then
            plain=-1
          elif (( builtin )); then
            (( seen )) || direct=$i
          elif (( plain == 0 )); then
            plain=1
          fi
        fi
        seen=1 builtin=0
      fi
      prev=$w[i]
    done
    if (( direct )); then
      __minicode_exec_env "${(@Q)w[direct+1,-1]}"
      __minicode_execed=1
    fi
    if (( plain == 1 )) && (( ! $+functions[exec] )); then
      functions[exec]=$functions[__minicode_exec]
      __minicode_exec_fn=1
    fi
    return 0
  }
  __minicode_exec_undo() {
    if (( __minicode_execed )); then
      __minicode_exec_restore
      __minicode_execed=0
    fi
    if (( __minicode_exec_fn )); then
      unfunction exec 2>/dev/null
      __minicode_exec_fn=0
    fi
    return 0
  }
  # A mark, with the nonce the terminal checks: marks without it, which
  # any program can print, are not believed.
  __minicode_mark() { builtin print -rn -- $'\e]'"$1;mc=$__minicode_nonce"$'\a' }
  # The Linux port (__minicode_term=vte in the .zshenv): VTE reads no OSC
  # 133 or 633, so the same calls keep counts and print one string termprop
  # instead (linux/PARITY.md D10, ShellIntegration.h's parseVteMark):
  #   ESC ] 666 ; vte.ext.minicode.mark=p=<prompts>\sx=<started>\sd=<done>\ss=<status>\sc=<command>\smc=<nonce> ESC \
  # at A (a prompt) and C (a command starts); D and E only update the
  # counts and the text; B prints nothing; OSC 7 goes out plain, since VTE
  # takes everything after "7;" as the address.
  if [[ ${__minicode_term-} == vte ]]; then
    typeset -gi __minicode_p=0 __minicode_x=0 __minicode_d=0
    typeset -g __minicode_s= __minicode_c=
    __minicode_mark() {
      case $1 in
        ('133;A') (( ++__minicode_p )) ;;
        ('133;C') (( ++__minicode_x )) ;;
        ('133;D;'*) __minicode_d=$__minicode_x __minicode_s=${1#'133;D;'}; return 0 ;;
        ('633;E;'*) __minicode_c=${1#'633;E;'}; return 0 ;;
        ('7;'*) builtin print -rn -- $'\e]'"$1"$'\a'; return 0 ;;
        (*) return 0 ;;
      esac
      local c=${__minicode_c[1,500]}
      c=${c//\\/\\\\}
      c=${c//;/\\s}
      builtin print -rn -- $'\e]666;vte.ext.minicode.mark=p='"$__minicode_p"'\sx='"$__minicode_x"'\sd='"$__minicode_d"'\ss='"$__minicode_s"'\sc='"$c"'\smc='"$__minicode_nonce"$'\e\\'
      return 0
    }
  fi
  # OSC 7 for $PWD: %, control characters and ; percent-encoded, so a
  # folder whose name holds BEL or ESC cannot end the mark early.
  __minicode_cwd() {
    local p=${PWD//\%/%25} out= c i
    p=${p//;/%3B}
    if [[ $p == *[$'\x01'-$'\x1f'$'\x7f']* ]]; then
      for (( i = 1; i <= $#p; i++ )); do
        c=$p[i]
        if [[ $c == [$'\x01'-$'\x1f'$'\x7f'] ]]; then
          builtin printf -v c '%%%02X' "'$c"
        fi
        out+=$c
      done
      p=$out
    fi
    __minicode_mark "7;file://$HOST$p"
  }

  if [[ $__minicode_mode == line ]]; then
    __minicode_precmd() {
      local rc=$?
      __minicode_exec_undo
      __minicode_mark "133;D;$rc"
      __minicode_cwd
      /bin/stty icanon isig -echo 2>/dev/null
      PS1='' PS2='' RPS1='' RPS2=''
      __minicode_mark "133;A"
    }
    __minicode_preexec() {
      __minicode_exec_hook "${3:-$1}"
      /bin/stty echo 2>/dev/null
      __minicode_mark "133;C"
    }
  else
    __minicode_precmd() {
      local rc=$?
      __minicode_exec_undo
      if (( __minicode_ran )); then
        __minicode_mark "133;D;$rc"
        __minicode_ran=0
      fi
      __minicode_cwd
      __minicode_mark "133;A"
      if (( __minicode_no_b )); then
        __minicode_mark "133;B"
      else
        # A plugin that sets zle-line-init up in a precmd of its own (as
        # zsh-vi-mode does at the first prompt) replaces the hook's
        # dispatcher, and B stopped for good; it is put back at every
        # prompt, keeping the plugin's widget (add-zle-hook-widget wraps it,
        # but only if ours is not still on its list, hence the -d first).
        local -a __mc_hooks
        zstyle -a zle-line-init widgets __mc_hooks
        if [[ ${widgets[zle-line-init]-} != user:azhw:zle-line-init ]]; then
          { add-zle-hook-widget -d line-init __minicode_line_init
            add-zle-hook-widget line-init __minicode_line_init; } 2>/dev/null
        elif [[ -z ${(M)__mc_hooks:#(<->:|)__minicode_line_init} ]]; then
          add-zle-hook-widget line-init __minicode_line_init 2>/dev/null
        fi
      fi
      return $rc
    }
    __minicode_preexec() {
      __minicode_ran=1
      __minicode_exec_hook "${3:-$1}"
      local c=${1:-$3}
      __minicode_mark "633;E;${c//[[:cntrl:]]/ }"
      __minicode_mark "133;C"
    }
    # B only for the command line itself (and its continuation lines), not
    # for vared or a select menu, which read input while a command runs.
    __minicode_line_init() {
      [[ $CONTEXT == (start|cont) ]] && __minicode_mark "133;B"
      return 0
    }
    # The private key ESC [9999~: what the user had typed goes onto ZLE's
    # buffer stack and comes back at the next prompt, so a command the app
    # types never runs together with a half-typed one. Out of vi's command
    # mode first, so the command is typed, not taken as vi commands.
    __minicode_push() {
      [[ -n $BUFFER$PREBUFFER ]] && zle push-input
      [[ $KEYMAP == vicmd ]] && zle -K viins
      return 0
    }
  fi

  # terminal.activate-venv: the folder's Python environment made active as
  # its bin/activate would make it, without running anything of the
  # environment's: VIRTUAL_ENV, its bin first on PATH, PYTHONHOME unset, its
  # name before the prompt (unless VIRTUAL_ENV_DISABLE_PROMPT), and a
  # deactivate that puts them back. At the first prompt, after the user's
  # startup files, so a PATH they set (or macOS's path_helper) cannot push
  # the environment's bin back down.
  __minicode_activate() {
    local v=$__minicode_venv
    unset __minicode_venv
    [[ -n $v && -d $v/bin ]] || return 0
    [[ ${VIRTUAL_ENV-} == "$v" && $path[1] == "$v/bin" ]] && return 0
    typeset -g _MINICODE_OLD_PATH=$PATH
    if [[ -n ${PYTHONHOME+x} ]]; then
      typeset -g _MINICODE_OLD_PYTHONHOME=$PYTHONHOME
      unset PYTHONHOME
    fi
    export VIRTUAL_ENV=$v VIRTUAL_ENV_PROMPT=$__minicode_venv_prompt
    path=("$v/bin" $path)
    if [[ -z ${VIRTUAL_ENV_DISABLE_PROMPT-} && -n $__minicode_venv_prompt ]]; then
      typeset -g _MINICODE_OLD_PS1=$PS1
      PS1="($__minicode_venv_prompt) $PS1"
    fi
    deactivate() {
      [[ -n ${_MINICODE_OLD_PATH+x} ]] && export PATH=$_MINICODE_OLD_PATH
      [[ -n ${_MINICODE_OLD_PYTHONHOME+x} ]] && export PYTHONHOME=$_MINICODE_OLD_PYTHONHOME
      [[ -n ${_MINICODE_OLD_PS1+x} ]] && PS1=$_MINICODE_OLD_PS1
      unset VIRTUAL_ENV VIRTUAL_ENV_PROMPT _MINICODE_OLD_PATH _MINICODE_OLD_PYTHONHOME \
            _MINICODE_OLD_PS1
      [[ ${1-} == nondestructive ]] || unfunction deactivate
      rehash
      return 0
    }
    rehash
  }

  # Runs at the first prompt, after the user's startup files, so our hooks
  # are last in line, have the final say over the prompt (line mode), and
  # the bindings survive a `bindkey -v` there (grid mode).
  __minicode_install() {
    __minicode_activate
    precmd_functions=(${precmd_functions:#__minicode_install} __minicode_precmd)
    preexec_functions+=(__minicode_preexec)
    setopt HIST_IGNORE_SPACE
    if [[ $__minicode_mode == line ]]; then
      unsetopt PROMPT_SP PROMPT_CR
      unsetopt ZLE   # off already (+Z), but not in a zsh started by `exec zsh`
    else
      zle -N __minicode_push
      local m
      for m in emacs viins vicmd; do
        bindkey -M $m $'\e[9999~' __minicode_push
        bindkey -M $m $'\e[9998~' beginning-of-line
        bindkey -M $m $'\e[9997~' end-of-line
        bindkey -M $m $'\e[9996~' backward-kill-line
        bindkey -M $m $'\e[9995~' clear-screen
      done
      if ! { autoload -Uz add-zle-hook-widget &&
             add-zle-hook-widget line-init __minicode_line_init; } 2>/dev/null; then
        __minicode_no_b=1
      fi
    fi
    __minicode_precmd
  }
  precmd_functions+=(__minicode_install)
fi
