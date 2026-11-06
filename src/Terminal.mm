// Terminal.mm — shell panel on a pty. Objective-C++.
//
// Two modes, chosen per terminal when it opens (terminal.mode):
//
// Grid mode (the default) is a real terminal. zsh runs as `zsh -l -i` with
// its line editor (ZLE) on, so the user's prompt, Tab completion, Ctrl+R,
// autosuggestions and key bindings work as in Terminal.app. The cell grid
// (TerminalScreen + TerminalGridView) is the only view; it keeps a
// scrollback above the screen. The generated .zshenv (kZshGridIntegration)
// still loads the user's files and adds hooks, installed last at the first
// prompt, that only report: OSC 133;A (prompt), B (ZLE reads input, from a
// zle-line-init hook), C (a command runs, after an OSC 633;E with its
// command line), D;status (it finished) and OSC 7 (cwd). It never touches
// the prompt or the tty modes. TerminalStream still reads the bytes, for
// those events only. Commands the app types (Run File, Request a Change,
// Run in Terminal) go to ZLE after a private key that runs push-input, so
// whatever the user had typed at the prompt comes back at the next one.
//
// The marks are believed only with this shell's nonce (MINICODE_NONCE, read
// and unset by the .zshenv, never exported), since any program can print
// them, and nothing is typed unless the shell also holds the terminal's
// foreground (canTypeNow).
//
// Line mode, the older design, kept for a release as a fallback:
//   * zsh runs as `zsh -l -i +Z` (login, interactive, line editor off) on a
//     pseudo-terminal from forkpty(). The kernel's line discipline handles
//     Ctrl+C -> SIGINT, echo, and the window size, like any terminal.
//   * ZDOTDIR points at a tiny generated .zshenv that restores the user's own
//     ZDOTDIR, loads their startup files as usual, and installs precmd/preexec
//     hooks just before the first prompt. The hooks emit OSC 133 marks (prompt,
//     command start, command end + status) and OSC 7 (cwd), and blank the
//     prompt text, since the panel draws its own.
//   * TerminalStream (pure C++) turns the pty bytes into styled lines and
//     those events. The last line of the output view is "live": the stream
//     re-sends it whole whenever it changes (colors, \r progress lines,
//     erase-in-line), and the view replaces it until a newline ends it.
//   * Typed lines go to the pty. At the prompt they are commands; while a
//     command runs they are that program's input. Echo is off while zsh reads
//     a command (the panel shows it in the header instead) and on while a
//     command runs, so a program that turns echo off (a password prompt) gets
//     a secure input field.
//   * TerminalScreen (pure C++) reads the same bytes as a cell grid, all the
//     time, so it is current whenever it is needed. The panel shows it in
//     place of the log (grid mode) while the program is on the alternate
//     screen (vim, less, htop, man), or when a running command has kept the
//     pty out of canonical mode for a moment without one (git's less -X, a
//     REPL, ssh); the latter lasts until the command ends. In grid mode keys
//     go straight to the pty. The log stream leaves alternate-screen text
//     out, so the log is intact when the grid goes away.
#import "Terminal.h"
#import "AppSettings.h"
#import "TerminalGridView.h"
#import "TerminalFindBar.h"
#import "Appearance.h"
#import "EditorFont.h"
#include "TerminalStream.h"
#include "TerminalScreen.h"
#include "TermLinks.h"
#include "AgentIndex.h"

#include <util.h>
#include <termios.h>
#include <sys/ttydefaults.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/sysctl.h>
#include <libproc.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <string>
#include <vector>

extern char **environ;

// A process's command line ("sleep 100"), from KERN_PROCARGS2: argc, the
// executable's path, padding, then the arguments. The program's path is cut
// to its last part; nil when it cannot be read.
static NSString *ProcessCommandLine(pid_t pid) {
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    size_t size = 0;
    if (sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int)) return nil;
    std::vector<char> buf(size);
    if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) return nil;
    int argc = 0;
    memcpy(&argc, buf.data(), sizeof argc);
    size_t i = sizeof argc;
    while (i < size && buf[i]) i++;    // the executable's path
    while (i < size && !buf[i]) i++;   // padding
    NSMutableArray<NSString *> *words = [NSMutableArray array];
    for (int a = 0; a < argc && i < size; a++) {
        size_t start = i;
        while (i < size && buf[i]) i++;
        NSString *w = [[NSString alloc] initWithBytes:buf.data() + start length:i - start
                                             encoding:NSUTF8StringEncoding];
        if (w) [words addObject:a == 0 && [w hasPrefix:@"/"] ? w.lastPathComponent : w];
        i++;
    }
    NSString *line = [words componentsJoinedByString:@" "];
    if (line.length > 80) line = [[line substringToIndex:79] stringByAppendingString:@"…"];
    return line.length ? line : nil;
}

static NSColor *THex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8)  & 0xFF) / 255.0
                                blue:( rgb        & 0xFF) / 255.0
                               alpha:1.0];
}

// The generated .zshenv. zsh reads $ZDOTDIR/.zshenv first; this one hands
// ZDOTDIR back to the user straight away, so their .zprofile/.zshrc/.zlogin
// load normally and child shells never see ours, except a zsh that takes
// this one's place through `exec` (__minicode_exec_hook).
static const char *kZshIntegration = R"ZSH(# Generated by MiniCode for its terminal panel. Safe to delete.
typeset -g __minicode_zdotdir=$ZDOTDIR __minicode_exec_in=${MINICODE_EXEC-}
typeset -g __minicode_nonce=${MINICODE_NONCE-}
if [[ -n "${MINICODE_USER_ZDOTDIR+x}" ]]; then
  ZDOTDIR=$MINICODE_USER_ZDOTDIR
else
  unset ZDOTDIR
fi
unset MINICODE_USER_ZDOTDIR MINICODE_EXEC MINICODE_NONCE
[[ -f "${ZDOTDIR:-$HOME}/.zshenv" ]] && builtin source "${ZDOTDIR:-$HOME}/.zshenv"

if [[ -o interactive ]]; then
  # No "%" end-of-line marker; these print before precmd, so set them now.
  unsetopt PROMPT_SP PROMPT_CR

  # `exec zsh` replaces this shell, and the new one would start without
  # these hooks: no prompt marks, so the tab stayed busy for good. For the
  # length of a command that execs, ZDOTDIR names this file's folder again
  # (the user's own is passed along and handed back, as at the start), and
  # MINICODE_EXEC tells the new shell to report the command finished. The
  # next prompt puts both back.
  typeset -gi __minicode_execed=0
  # An exec counts where a command starts: after a separator, `then`,
  # `do`, `else`, `{` or `!`, and after `builtin`, `command`, `noglob` or
  # `nocorrect` in front of it. One in a subshell does not replace this
  # shell.
  __minicode_exec_hook() {
    local -a w; w=(${(z)1})
    local i prev=';'
    for (( i = 1; i <= $#w; i++ )); do
      if [[ $prev == (';'|';;'|$'\n'|'&&'|'||'|'&'|'|'|'&|'|'&!'|'{'|'!'|then|do|else|elif) ]]; then
        [[ $w[i] == (builtin|command|noglob|nocorrect|-) ]] && continue
        if [[ $w[i] == exec ]]; then
          if [[ -n ${ZDOTDIR+x} ]]; then export MINICODE_USER_ZDOTDIR=$ZDOTDIR
          else unset MINICODE_USER_ZDOTDIR; fi
          export ZDOTDIR=$__minicode_zdotdir MINICODE_EXEC=1
          # The nonce only for a shell, so a zsh started from it can mark
          # again; another program (exec claude) would keep it in its
          # environment for all it runs.
          local j=$(( i + 1 ))
          while [[ $w[j] == -* ]]; do (( j++ )); done
          [[ ${${w[j]:t}#-} == (zsh|bash|sh|dash|ksh|mksh|fish|tcsh|csh|yash) ]] &&
            export MINICODE_NONCE=$__minicode_nonce
          __minicode_execed=1
          return 0
        fi
      fi
      prev=$w[i]
    done
    return 0
  }
  __minicode_exec_undo() {
    (( __minicode_execed )) || return 0
    if [[ -n ${MINICODE_USER_ZDOTDIR+x} ]]; then ZDOTDIR=$MINICODE_USER_ZDOTDIR
    else unset ZDOTDIR; fi
    unset MINICODE_USER_ZDOTDIR MINICODE_EXEC MINICODE_NONCE
    __minicode_execed=0
  }
  # A mark, with the nonce the terminal checks: marks without it, which
  # any program can print, are not believed.
  __minicode_mark() { builtin print -rn -- $'\e]'"$1;mc=$__minicode_nonce"$'\a' }
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
  # Runs at the first prompt, after the user's startup files, so our hook is
  # last in line and has the final say over the prompt.
  __minicode_install() {
    precmd_functions=(${precmd_functions:#__minicode_install} __minicode_precmd)
    preexec_functions+=(__minicode_preexec)
    setopt HIST_IGNORE_SPACE
    unsetopt PROMPT_SP PROMPT_CR
    unsetopt ZLE   # off already (+Z), but not in a zsh started by `exec zsh`
    __minicode_precmd
  }
  precmd_functions+=(__minicode_install)
fi
)ZSH";

// Grid mode's .zshenv: the same hand-back of ZDOTDIR, then hooks that only
// report what the shell is doing, so the user's prompt, options and tty
// modes are left alone.
//   precmd   OSC 133;D;status (only after a command ran), OSC 7, OSC 133;A
//   ZLE init OSC 133;B: the line editor reads input (add-zle-hook-widget, so
//            the user's own zle-line-init keeps working; without it, B
//            follows A from precmd)
//   preexec  OSC 633;E;<the command line, control characters blanked>, then
//            OSC 133;C
// The private key kZlePushKey runs push-input: what the user had typed goes
// onto ZLE's buffer stack and comes back at the next prompt, so a command
// the app types never runs together with a half-typed one. Out of vi's
// command mode first, so the command is typed, not taken as vi commands.
static const char *kZlePushKey = "\x1b[9999~";
// More private keys, bound to ZLE's own widgets, for Command keys in the
// grid while ZLE reads input: Cmd+Left, Cmd+Right and Cmd+Delete (the start
// and end of the line, delete back to its start, in vi's modes too, where
// Ctrl+A, Ctrl+E and Ctrl+U do something else), and Cmd+K (clear-screen,
// so a prompt of several lines is drawn again whole).
static const char *kZleLineStartKey = "\x1b[9998~";
static const char *kZleLineEndKey = "\x1b[9997~";
static const char *kZleKillToStartKey = "\x1b[9996~";
static const char *kZleClearKey = "\x1b[9995~";
static const char *kZshGridIntegration = R"ZSH(# Generated by MiniCode for its terminal panel (grid mode). Safe to delete.
typeset -g __minicode_zdotdir=$ZDOTDIR __minicode_exec_in=${MINICODE_EXEC-}
typeset -g __minicode_nonce=${MINICODE_NONCE-}
if [[ -n "${MINICODE_USER_ZDOTDIR+x}" ]]; then
  ZDOTDIR=$MINICODE_USER_ZDOTDIR
else
  unset ZDOTDIR
fi
unset MINICODE_USER_ZDOTDIR MINICODE_EXEC MINICODE_NONCE
[[ -f "${ZDOTDIR:-$HOME}/.zshenv" ]] && builtin source "${ZDOTDIR:-$HOME}/.zshenv"

if [[ -o interactive ]]; then
  typeset -gi __minicode_ran=0 __minicode_no_b=0
  [[ -n $__minicode_exec_in ]] && __minicode_ran=1   # D for the `exec zsh`
  # `exec zsh` replaces this shell, and the new one would start without
  # these hooks: no prompt marks, so the tab stayed busy for good. For the
  # length of a command that execs, ZDOTDIR names this file's folder again
  # (the user's own is passed along and handed back, as at the start), and
  # MINICODE_EXEC tells the new shell to report the command finished. The
  # next prompt puts both back.
  typeset -gi __minicode_execed=0
  # An exec counts where a command starts: after a separator, `then`,
  # `do`, `else`, `{` or `!`, and after `builtin`, `command`, `noglob` or
  # `nocorrect` in front of it. One in a subshell does not replace this
  # shell.
  __minicode_exec_hook() {
    local -a w; w=(${(z)1})
    local i prev=';'
    for (( i = 1; i <= $#w; i++ )); do
      if [[ $prev == (';'|';;'|$'\n'|'&&'|'||'|'&'|'|'|'&|'|'&!'|'{'|'!'|then|do|else|elif) ]]; then
        [[ $w[i] == (builtin|command|noglob|nocorrect|-) ]] && continue
        if [[ $w[i] == exec ]]; then
          if [[ -n ${ZDOTDIR+x} ]]; then export MINICODE_USER_ZDOTDIR=$ZDOTDIR
          else unset MINICODE_USER_ZDOTDIR; fi
          export ZDOTDIR=$__minicode_zdotdir MINICODE_EXEC=1
          # The nonce only for a shell, so a zsh started from it can mark
          # again; another program (exec claude) would keep it in its
          # environment for all it runs.
          local j=$(( i + 1 ))
          while [[ $w[j] == -* ]]; do (( j++ )); done
          [[ ${${w[j]:t}#-} == (zsh|bash|sh|dash|ksh|mksh|fish|tcsh|csh|yash) ]] &&
            export MINICODE_NONCE=$__minicode_nonce
          __minicode_execed=1
          return 0
        fi
      fi
      prev=$w[i]
    done
    return 0
  }
  __minicode_exec_undo() {
    (( __minicode_execed )) || return 0
    if [[ -n ${MINICODE_USER_ZDOTDIR+x} ]]; then ZDOTDIR=$MINICODE_USER_ZDOTDIR
    else unset ZDOTDIR; fi
    unset MINICODE_USER_ZDOTDIR MINICODE_EXEC MINICODE_NONCE
    __minicode_execed=0
  }
  # A mark, with the nonce the terminal checks: marks without it, which
  # any program can print, are not believed.
  __minicode_mark() { builtin print -rn -- $'\e]'"$1;mc=$__minicode_nonce"$'\a' }
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
  __minicode_push() {
    [[ -n $BUFFER$PREBUFFER ]] && zle push-input
    [[ $KEYMAP == vicmd ]] && zle -K viins
    return 0
  }
  # Runs at the first prompt, after the user's startup files, so our hooks
  # are last in line and the bindings survive a `bindkey -v` there.
  __minicode_install() {
    precmd_functions=(${precmd_functions:#__minicode_install} __minicode_precmd)
    preexec_functions+=(__minicode_preexec)
    setopt HIST_IGNORE_SPACE
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
    __minicode_precmd
  }
  precmd_functions+=(__minicode_install)
fi
)ZSH";

// Write the integration file; returns its directory, or nil on failure.
// Each mode has its own, since terminals of both kinds can be open at once.
static NSString *IntegrationDirectory(BOOL grid) {
    NSString *dir = [NSTemporaryDirectory()
        stringByAppendingPathComponent:grid ? @"MiniCode-zsh-grid" : @"MiniCode-zsh"];
    NSFileManager *fm = [NSFileManager defaultManager];
    if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES
                        attributes:@{NSFilePosixPermissions: @0700} error:nil])
        return nil;
    NSString *file = [dir stringByAppendingPathComponent:@".zshenv"];
    NSString *body = [NSString stringWithUTF8String:grid ? kZshGridIntegration
                                                         : kZshIntegration];
    if (![body writeToFile:file atomically:YES encoding:NSUTF8StringEncoding
                     error:nil])
        return nil;
    return dir;
}

NSNotificationName const MCTerminalCommandDidFinishNotification =
    @"MCTerminalCommandDidFinish";

// Flow control between the pty and the main thread. The reader (on the
// terminal's I/O queue) counts the bytes it has handed over and not yet seen
// parsed; past kReadHigh it suspends itself, so the kernel's pty buffer
// fills and the program writing blocks, as on any terminal. The main thread
// parses in slices of about kSliceSeconds, letting events and drawing in
// between, and resumes the reader once it is below kReadLow.
static const long kReadHigh = 1 << 20, kReadLow = 256 << 10;
static const size_t kSliceBytes = 16 << 10;
static const CFTimeInterval kSliceSeconds = 0.012;

// One shell's pty master, and all that touches it off the main thread.
// Reading, writing and closing run on the terminal's serial I/O queue, so
// they never race one another.
//   Reading: a read source hands the bytes to the main thread, suspended
//   while it is more than kReadHigh ahead (above).
//   Writing: the master is non-blocking, and what the pty will not take yet
//   waits in `out`, in order, while a write source waits for room. write()
//   used to block the main thread: a paste bigger than the pty's input
//   queue (about 1 KB) into a program not reading froze the app, for good
//   if that program was itself blocked on output the reader had stopped
//   taking.
//   Closing (`shutdown`, also on end of file): both sources are cancelled,
//   a suspended one resumed so its cancel can finish, unwritten bytes
//   dropped. When both are gone the main thread lets go of the descriptor
//   (`released`), then it is closed and the shell reaped (`exited`): a
//   shell still there 2 s after the hang-up gets SIGKILL. Cancelling from
//   the main thread used to race the reader suspending itself, and a
//   source cancelled while suspended never finished: the fd stayed open
//   and the shell was never reaped.
@interface MCPty : NSObject {
@public
    int fd;
    pid_t pid;
    std::atomic<long> queued;   // handed to the main thread, not yet parsed
    // The I/O queue's alone:
    BOOL readSuspended, writeArmed, closing;
    int sources;                // sources whose cancel has not finished
    std::string out;            // bytes waiting for room in the pty
    size_t outStart;            // written bytes at the front of `out`
}
@property(nonatomic, strong) dispatch_queue_t queue;
@property(nonatomic, strong) dispatch_source_t readSource, writeSource;
@property(nonatomic, copy) void (^deliver)(NSData *data);   // I/O queue
@property(nonatomic, copy) void (^released)(void);          // main thread
@property(nonatomic, copy) void (^exited)(void);            // main thread
@end

@implementation MCPty

- (instancetype)initWithFd:(int)master pid:(pid_t)shell queue:(dispatch_queue_t)queue {
    if ((self = [super init])) {
        fd = master;
        pid = shell;
        _queue = queue;
        int flags = fcntl(master, F_GETFL);
        if (flags != -1) fcntl(master, F_SETFL, flags | O_NONBLOCK);
        _readSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
                                             (uintptr_t)master, 0, queue);
        _writeSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE,
                                              (uintptr_t)master, 0, queue);
        sources = 2;
        // The handlers hold the object until the cancels finish, when
        // dispatch lets go of them.
        dispatch_source_set_event_handler(_readSource, ^{ [self readable]; });
        dispatch_source_set_event_handler(_writeSource, ^{ [self flush]; });
        dispatch_source_set_cancel_handler(_readSource, ^{ [self sourceGone]; });
        dispatch_source_set_cancel_handler(_writeSource, ^{ [self sourceGone]; });
    }
    return self;
}

// Main thread, once the handlers are set: start reading. The write source
// stays suspended until a write meets a full pty.
- (void)start {
    dispatch_resume(_readSource);
}

- (void)readable {
    if (closing) return;
    char buf[65536];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
        long total = queued.fetch_add(n) + n;
        if (self.deliver) self.deliver([NSData dataWithBytes:buf length:(NSUInteger)n]);
        // Far behind: stop reading until the main thread catches up.
        if (total > kReadHigh && !readSuspended) {
            readSuspended = YES;
            dispatch_suspend(_readSource);
        }
    } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        // EIO: every process holding the pty's other end has gone.
        [self shutdown];
    }
}

// Any thread: the main thread has caught up.
- (void)resumeReading {
    dispatch_async(_queue, ^{
        if (self->readSuspended && !self->closing) {
            self->readSuspended = NO;
            dispatch_resume(self.readSource);
        }
    });
}

// Any thread: bytes for the program, written in the order given.
- (void)send:(NSData *)data {
    dispatch_async(_queue, ^{
        if (self->closing) return;
        self->out.append((const char *)data.bytes, data.length);
        [self flush];
    });
}

- (void)flush {
    while (!closing && outStart < out.size()) {
        ssize_t n = write(fd, out.data() + outStart, out.size() - outStart);
        if (n > 0) { outStart += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            // What is written goes from the front only once it is half the
            // buffer: erasing at every 64 KB moved a 100 MB paste's tail
            // some 1,500 times into a slow reader.
            if (outStart > (64u << 10) && outStart > out.size() / 2) {
                out.erase(0, outStart);
                outStart = 0;
            }
            if (!writeArmed) { writeArmed = YES; dispatch_resume(_writeSource); }
            return;
        }
        break;   // the pty is gone (EIO): what is left goes nowhere
    }
    std::string().swap(out);
    outStart = 0;
    if (writeArmed && !closing) { writeArmed = NO; dispatch_suspend(_writeSource); }
}

// I/O queue; a second call does nothing.
- (void)shutdown {
    if (closing) return;
    closing = YES;
    std::string().swap(out);
    outStart = 0;
    dispatch_source_cancel(_readSource);
    dispatch_source_cancel(_writeSource);
    if (readSuspended) { readSuspended = NO; dispatch_resume(_readSource); }
    if (!writeArmed) { writeArmed = YES; dispatch_resume(_writeSource); }
}

- (void)sourceGone {
    if (--sources > 0) return;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.released) self.released();
        self.released = nil;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            close(self->fd);
            [self reap:0];
        });
    });
}

// A global queue: wait for the shell without tying up a thread, as closing
// the master hangs it up.
- (void)reap:(int)tries {
    int status = 0;
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r < 0 && errno == EINTR) r = 0;
    if (r == 0) {
        if (tries == 40) kill(pid, SIGKILL);   // 2 s: it ignored the hang-up
        if (tries < 100) {
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC),
                           dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                [self reap:tries + 1];
            });
            return;
        }
        waitpid(pid, &status, 0);   // SIGKILL has not taken yet: block
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.exited) self.exited();
        self.exited = nil;
    });
}

@end

@implementation MCTermLinkMatch
@end

@implementation MCTermLink
@end

// Read-only output view. A plain click focuses the input line; a drag still
// selects text for copying; Cmd+click opens the link under the pointer.
//
// While Cmd is held over a link the link is underlined. TextKit 2 ignores
// underline rendering attributes (see Lsp.mm), so the underline is a real
// storage attribute, added and taken off again; whatever underline the
// program printed there is put back. Output arriving clears it first, since
// the live line is replaced wholesale.
@interface TerminalOutputView : NSTextView
@property(nonatomic, copy) void (^onPlainClick)(void);
// The link at a character of the output, and its span there (UTF-16).
@property(nonatomic, copy) MCTermLink *(^linkAtIndex)(NSUInteger index, NSRange *range);
@property(nonatomic, copy) void (^openLink)(MCTermLink *link);
- (void)updateLinkHover;
- (void)clearLinkHover;
@end
@implementation TerminalOutputView {
    NSRange _hoverRange;                        // location NSNotFound: none
    NSMutableArray<NSArray *> *_savedUnderline; // [range value, underline] pairs
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) _hoverRange = NSMakeRange(NSNotFound, 0);
    return self;
}

- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSMenu *menu = [super menuForEvent:event];
    MCAddAdjustSettingsItem(menu, @"terminal", self);
    return menu;
}

// The link under a point in the view, or nil. The character index is
// checked against its own glyph rect, because characterIndexForPoint:
// answers with the nearest character even far past the end of a line.
- (MCTermLink *)linkAtPoint:(NSPoint)p range:(NSRange *)range {
    if (!self.linkAtIndex || !self.window) return nil;
    NSPoint screen = [self.window convertPointToScreen:
                      [self convertPoint:p toView:nil]];
    NSUInteger i = [self characterIndexForPoint:screen];
    if (i == NSNotFound || i >= self.string.length) return nil;
    NSRect glyph = [self firstRectForCharacterRange:NSMakeRange(i, 1)
                                        actualRange:NULL];
    if (!NSPointInRect(screen, NSInsetRect(glyph, -1, -1))) return nil;
    return self.linkAtIndex(i, range);
}

- (void)clearLinkHover {
    if (_hoverRange.location == NSNotFound) return;
    NSTextStorage *ts = self.textStorage;
    if (NSMaxRange(_hoverRange) <= ts.length) {
        [ts beginEditing];
        [ts removeAttribute:NSUnderlineStyleAttributeName range:_hoverRange];
        for (NSArray *pair in _savedUnderline) {
            NSRange r = [pair[0] rangeValue];
            if (NSMaxRange(r) <= ts.length)
                [ts addAttribute:NSUnderlineStyleAttributeName value:pair[1] range:r];
        }
        [ts endEditing];
    }
    _hoverRange = NSMakeRange(NSNotFound, 0);
    _savedUnderline = nil;
}

- (void)updateLinkHover {
    NSRange range = NSMakeRange(NSNotFound, 0);
    MCTermLink *link = nil;
    if (NSEvent.modifierFlags & NSEventModifierFlagCommand) {
        NSPoint p = [self convertPoint:[self.window mouseLocationOutsideOfEventStream]
                              fromView:nil];
        if (NSPointInRect(p, self.visibleRect)) link = [self linkAtPoint:p range:&range];
    }
    if (!link) {
        BOOL was = _hoverRange.location != NSNotFound;
        [self clearLinkHover];
        if (was) [self.window invalidateCursorRectsForView:self];
        return;
    }
    if (NSEqualRanges(range, _hoverRange)) { [[NSCursor pointingHandCursor] set]; return; }
    [self clearLinkHover];
    NSTextStorage *ts = self.textStorage;
    _savedUnderline = [NSMutableArray array];
    [ts enumerateAttribute:NSUnderlineStyleAttributeName inRange:range options:0
                usingBlock:^(id value, NSRange r, BOOL *stop) {
        (void)stop;
        if (value) [self->_savedUnderline addObject:@[[NSValue valueWithRange:r], value]];
    }];
    [ts beginEditing];
    [ts addAttribute:NSUnderlineStyleAttributeName
               value:@(NSUnderlineStyleSingle) range:range];
    [ts endEditing];
    _hoverRange = range;
    [[NSCursor pointingHandCursor] set];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea *a in self.trackingAreas)
        if (a.owner == self && a.userInfo[@"links"]) [self removeTrackingArea:a];
    [self addTrackingArea:[[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
               owner:self userInfo:@{@"links": @YES}]];
}

- (void)mouseMoved:(NSEvent *)event {
    [super mouseMoved:event];
    [self updateLinkHover];
}
- (void)mouseExited:(NSEvent *)event {
    [super mouseExited:event];
    [self clearLinkHover];
}
- (void)cursorUpdate:(NSEvent *)event {
    if (_hoverRange.location != NSNotFound) [[NSCursor pointingHandCursor] set];
    else [super cursorUpdate:event];
}

- (void)mouseDown:(NSEvent *)event {
    if (event.modifierFlags & NSEventModifierFlagCommand) {
        NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
        MCTermLink *link = [self linkAtPoint:p range:NULL];
        if (link) {
            [self clearLinkHover];
            if (self.openLink) self.openLink(link);
            return;
        }
    }
    [super mouseDown:event];   // runs selection tracking until mouseUp
    if (self.selectedRange.length == 0 && self.onPlainClick) self.onPlainClick();
}
@end

// Marks output drawn in the default foreground (value: whether it is dim), so
// a settings change can recolor it in place.
static NSString *const kDefaultFgAttribute = @"MCTerminalDefaultForeground";

@interface TerminalView () <NSTextFieldDelegate> {
    NSString *_cwd;
    NSMutableArray<NSString *> *_history;
    NSInteger _historyIdx;

    TerminalStream _stream;
    dispatch_queue_t _ioQueue;
    MCPty *_pty;                 // this shell's pty, nil when none
    NSMutableData *_backlog;     // read from the pty, not yet parsed
    NSUInteger _backlogStart;    // parsed bytes at the front of _backlog
    BOOL _drainScheduled;
    NSUInteger _logLines;        // line mode: lines in the output view
    int _master;                 // pty master fd, -1 when no shell
    pid_t _shellPid;             // 0 when no shell
    int _generation;             // bumps per shell, so stale output is ignored
    NSDate *_shellStarted;
    BOOL _integrated;            // the shell sends the integration's marks: the
                                 // file was written and the shell is still zsh
    BOOL _integrationFile;       // the shell integration file was written
    NSTimer *_shellWatch;        // while a command runs: is the shell still zsh?
    struct winsize _winsize;
    CGFloat _fontSize;           // the font size the views were last given

    BOOL _atPrompt;              // zsh is waiting for a command
    BOOL _commandStarted;        // saw 133;C for the command in flight
    BOOL _userCommand;           // the command in flight came from the input
    BOOL _seenPrompt;            // this shell has reached its first prompt
    NSString *_runningCommand;   // what was entered, until the next prompt
    NSString *_programTitle;     // OSC 0 or 2 from the running program, until the prompt
    NSString *_pendingCommand;   // runCommand: waiting for the prompt
    BOOL _closed;                // close was called: never start a shell again
    BOOL _lineMode;              // terminal.mode = line when this one opened
    BOOL _zleReady;              // grid mode: ZLE reads input (133;B, no C since)
    NSString *_pendingTyped;     // grid mode: typeCommand: waiting for ZLE
    std::string _markNonce;      // this shell's marks carry it (setMarkNonce)
    CFTimeInterval _promptTime;  // when the last 133;A came
    BOOL _retryScheduled;        // flushPendingInput will run again soon
    BOOL _screenMade;            // grid mode: the screen (and its scrollback)
                                 // outlives a shell that exits
    NSUInteger _liveStart;       // where the live line begins in the output

    TerminalScreen _screen;      // the same output as a cell grid
    BOOL _gridMode;              // the grid is showing instead of the log
    BOOL _stickyGrid;            // this command went raw: grid until it ends
    int _rawTicks;               // consecutive polls that found raw mode
    NSTimer *_rawTimer;          // polls termios while a command runs

    // Cmd+click links: a path as written -> [matches, roots tried, when].
    // An entry is good for a second (a quarter for "not found"), so a file
    // a command has just created is found, and emptied when the directory
    // changes. It used to be
    // emptied by every slice of output, so with a program printing all the
    // time (Claude Code's spinner) every hover looked on disk again.
    NSMutableDictionary<NSString *, id> *_linkCache;
    NSArray<NSString *> *_linkRoots;   // extraLinkRoots, as last asked
    CFTimeInterval _linkRootsTime;
    id _flagsMonitor;            // Cmd down/up re-evaluates the link under the pointer
}
@property(nonatomic, strong) TerminalGridView *grid;
@property(nonatomic, strong) MCTerminalFindBar *findBar;   // made at the first Cmd+F
@property(nonatomic, strong) NSScrollView *outScroll;
@property(nonatomic, strong) TerminalOutputView *output;
@property(nonatomic, strong) NSTextField *prompt;
@property(nonatomic, strong) NSTextField *input;
@property(nonatomic, strong) NSSecureTextField *secureInput;
@end

@implementation TerminalView

- (instancetype)initWithDirectory:(NSString *)dir {
    if ((self = [super initWithFrame:NSMakeRect(0, 0, 400, 220)])) {
        _cwd = dir.length ? dir : NSHomeDirectory();
        _history = [NSMutableArray array];
        _historyIdx = 0;
        _master = -1;
        _lineMode = [AppSettings shared].settings.terminalLineMode();
        _ioQueue = dispatch_queue_create("minicode.terminal.io",
                                         DISPATCH_QUEUE_SERIAL);
        self.wantsLayer = YES;
        [self buildViews];
        [self applySettings];
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(applySettings)
                   name:MCSettingsDidChangeNotification object:nil];
        [[NSNotificationCenter defaultCenter]
            addObserver:self selector:@selector(applyFontSize)
                   name:MCEditorZoomDidChangeNotification object:nil];
        if (_lineMode)
            [self appendLine:@"MiniCode terminal — zsh on a pty. Ctrl+C interrupts, "
                              "`clear` resets the panel."
                       color:THex(0x6A9955)];
        [self updatePrompt];
        [self startShell];
    }
    return self;
}

- (NSTextField *)makeInputField:(Class)cls {
    NSTextField *f = [[cls alloc] init];
    f.font = MCTerminalFont(NO, NO);
    f.bezeled = NO;
    f.focusRingType = NSFocusRingTypeNone;
    f.bordered = NO;
    // The input line is part of the panel: the panel's own layer paints
    // behind it, as it does behind the log. A background of its own showed as
    // a lighter strip that stopped short of the edges, and on a see-through
    // panel it doubled the opacity under the field.
    f.drawsBackground = NO;
    f.backgroundColor = [NSColor clearColor];
    f.delegate = self;
    f.target = self;
    f.action = @selector(inputEntered:);
    [self addSubview:f];
    return f;
}

- (void)buildViews {
    // Output (read-only, monospaced, selectable).
    self.outScroll = [[NSScrollView alloc] init];
    self.outScroll.hasVerticalScroller = YES;
    self.outScroll.drawsBackground = NO;
    self.output = [[TerminalOutputView alloc] initWithFrame:self.bounds];
    __weak TerminalView *weakSelf = self;
    self.output.onPlainClick = ^{ [weakSelf focusInput]; };
    self.output.linkAtIndex = ^MCTermLink *(NSUInteger index, NSRange *range) {
        return [weakSelf linkInOutputAt:index range:range];
    };
    self.output.openLink = ^(MCTermLink *link) {
        TerminalView *s = weakSelf;
        if (s.onOpenLink) s.onOpenLink(link);
    };
    self.output.editable = NO;
    self.output.selectable = YES;
    self.output.richText = YES;
    self.output.drawsBackground = NO;
    self.output.textContainerInset = NSMakeSize(6, 6);
    // No extra padding, so the log wraps at the same column count the pty
    // reports (updateWindowSize measures with the grid's metrics).
    self.output.textContainer.lineFragmentPadding = 0;
    self.output.verticallyResizable = YES;
    self.output.horizontallyResizable = NO;
    self.output.textContainer.widthTracksTextView = YES;
    self.output.autoresizingMask = NSViewWidthSizable;
    self.outScroll.documentView = self.output;
    [self addSubview:self.outScroll];

    // Prompt label + input field (and a secure twin for hidden input).
    self.prompt = [NSTextField labelWithString:@"❯"];
    self.prompt.font = MCTerminalFont(YES, NO);
    self.prompt.textColor = THex(0x4EC9B0);
    self.prompt.backgroundColor = [NSColor clearColor];
    [self addSubview:self.prompt];

    self.input = [self makeInputField:[MCSurfaceTextField class]];
    ((MCSurfaceTextField *)self.input).appearanceSurface = @"terminal";
    self.input.placeholderString = @"Enter a command…";
    self.secureInput = (NSSecureTextField *)
        [self makeInputField:[NSSecureTextField class]];
    self.secureInput.placeholderString = @"Hidden input (the program turned echo off)";
    self.secureInput.hidden = YES;

    // The cell grid for full-screen programs, over everything else.
    self.grid = [[TerminalGridView alloc] initWithFrame:self.bounds];
    self.grid.screen = &_screen;
    self.grid.hidden = YES;
    self.grid.onInput = ^(NSData *bytes) {
        TerminalView *s = weakSelf;
        if (!s) return;
        // A shell that died at startup waits for Return before another.
        if (s->_shellPid <= 0 && !s->_closed && !s->_lineMode &&
            memchr(bytes.bytes, '\r', bytes.length)) {
            [s startShell];
            return;
        }
        [s sendBytes:(const char *)bytes.bytes length:bytes.length];
    };
    self.grid.linkAt = ^id(const std::string &line, size_t byte) {
        return [weakSelf linkInLine:line atByte:byte];
    };
    self.grid.openLink = ^(id link) {
        TerminalView *s = weakSelf;
        if (s.onOpenLink) s.onOpenLink(link);
    };
    self.grid.onLineEdit = ^(MCLineEdit edit) { [weakSelf lineEdit:edit]; };
    self.grid.onFindChanged = ^{ [weakSelf updateFindCount]; };
    [self addSubview:self.grid];
    if (!_lineMode) {
        // The grid is the terminal: always shown, the log and its input
        // line never are.
        _gridMode = YES;
        self.grid.hidden = NO;
        self.grid.textInput = YES;
        self.outScroll.hidden = YES;
        self.prompt.hidden = YES;
        self.input.hidden = YES;
    }
}

// A right click on the grid, the prompt or the panel's edges.
- (NSMenu *)menuForEvent:(NSEvent *)event {
    (void)event;
    return MCAdjustSettingsMenu(@"terminal", self);
}

// Colors from the settings file. Output already on screen in the default
// foreground is recolored too; explicitly colored output keeps its colors.
- (void)applySettings {
    AppSettings *cfg = [AppSettings shared];
    self.layer.backgroundColor = [cfg background:Surface::Terminal].CGColor;
    NSColor *inputText = MCColor(cfg.settings.terminalInputText());
    for (NSTextField *f in @[self.input, self.secureInput])
        f.textColor = inputText;
    NSTextStorage *ts = self.output.textStorage;
    NSColor *fg = [cfg text:Surface::Terminal];
    [ts beginEditing];
    [ts enumerateAttribute:kDefaultFgAttribute inRange:NSMakeRange(0, ts.length)
                   options:0 usingBlock:^(id dim, NSRange r, BOOL *stop) {
        (void)stop;
        if (!dim) return;
        NSColor *c = [dim boolValue]
            ? [fg colorWithAlphaComponent:fg.alphaComponent * 0.6] : fg;
        [ts addAttribute:NSForegroundColorAttributeName value:c range:r];
    }];
    [ts endEditing];
    if (!_lineMode) {
        _screen.setScrollbackLimit(cfg.settings.terminalScrollback());
        self.grid.optionAsMeta = cfg.settings.terminalOptionAsMeta();
    }
    [self.findBar applySettings];
    [self applyFontSize];
    [self.grid screenChanged];
}

// terminal.font-size plus the editor's zoom. On a change the input line,
// the prompt and the log take the new size (each run keeps its weight and
// slant), the panel is laid out again, and the pty gets the rows and
// columns the new cell fits, which rewraps the grid's lines.
- (void)applyFontSize {
    const CGFloat size = MCTerminalFontSize();
    if (size == _fontSize) return;
    const BOOL first = _fontSize == 0;
    _fontSize = size;
    for (NSTextField *f in @[self.input, self.secureInput]) f.font = MCTerminalFont(NO, NO);
    self.prompt.font = MCTerminalFont(YES, NO);
    if (!first) {
        NSTextStorage *ts = self.output.textStorage;
        [ts beginEditing];
        [ts enumerateAttribute:NSFontAttributeName inRange:NSMakeRange(0, ts.length)
                       options:0 usingBlock:^(NSFont *font, NSRange r, BOOL *stop) {
            (void)stop;
            NSFontDescriptorSymbolicTraits t = font.fontDescriptor.symbolicTraits;
            [ts addAttribute:NSFontAttributeName
                       value:MCTerminalFont((t & NSFontDescriptorTraitBold) != 0,
                                            (t & NSFontDescriptorTraitItalic) != 0)
                       range:r];
        }];
        [ts endEditing];
    }
    [self layoutChildren];
    [self.grid screenChanged];
    self.grid.needsDisplay = YES;
}

// Manual layout — reliable regardless of how the host sizes us.
- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self layoutChildren];
}

- (void)layoutChildren {
    CGFloat W = self.bounds.size.width, H = self.bounds.size.height;
    // 18 and 26 points at the default 12-point font, growing with it.
    const CGFloat fieldH = ceil([TerminalGridView cellSize].height) + 3;
    const CGFloat inH = fieldH + 8, promptW = ceil(20 * MAX(_fontSize, 12) / 12);
    self.outScroll.frame = NSMakeRect(0, inH, W, MAX(0, H - inH));
    // The prompt and the field share a row, so the glyph sits on the text's
    // baseline (same font size, same unbezeled cell).
    NSRect field = NSMakeRect(6 + promptW, 4, MAX(0, W - promptW - 12), fieldH);
    self.prompt.frame = NSMakeRect(6, field.origin.y, promptW, field.size.height);
    self.input.frame = field;
    self.secureInput.frame = field;
    self.grid.frame = self.bounds;
    if (self.findBar) {
        NSSize size = [MCTerminalFindBar preferredSize];
        CGFloat w = MIN(size.width, MAX(0, W - 16));
        self.findBar.frame = NSMakeRect(W - w - 8, H - size.height - 6, w, size.height);
    }
    [self updateWindowSize];
}

- (NSTextField *)activeInput {
    return self.secureInput.hidden ? self.input : self.secureInput;
}

- (void)focusInput {
    if (_gridMode) [self.window makeFirstResponder:self.grid];
    else [self.window makeFirstResponder:[self activeInput]];
}

// Is keyboard focus somewhere in this panel?
- (BOOL)hasFocus {
    id fr = self.window.firstResponder;
    return fr == self || fr == self.grid || [self isEditing:self.input] ||
           [self isEditing:self.secureInput] || [self.findBar isEditing];
}

// ------------------------------------------------- find, clear, line keys
// The grid is what shows (always in grid mode; in line mode while a
// program has the screen).
- (BOOL)gridShowing { return _gridMode && !self.grid.hidden; }

// Edit > Find (Cmd+F), Find Next and Find Previous (Cmd+G, Shift+Cmd+G),
// reached through the responder chain while the grid has the keyboard. The
// tags are NSTextFinderAction values, as the menu sets them for the editor.
- (void)performTextFinderAction:(id)sender {
    if (![self gridShowing]) return;
    NSInteger tag = [sender respondsToSelector:@selector(tag)] ? [sender tag] : 0;
    switch (tag) {
    case NSTextFinderActionShowFindInterface:
        [self showFind];
        break;
    case NSTextFinderActionNextMatch:
    case NSTextFinderActionPreviousMatch:
        if (self.findBar.hidden || !self.findBar || !self.findBar.field.stringValue.length)
            [self showFind];
        else
            [self.grid findNext:tag == NSTextFinderActionNextMatch];
        break;
    default:
        break;
    }
}

- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(performTextFinderAction:))
        return [self gridShowing] && item.tag >= NSTextFinderActionShowFindInterface &&
               item.tag <= NSTextFinderActionPreviousMatch;
    if (item.action == @selector(clearTerminal:)) return !_closed;
    return [self respondsToSelector:item.action];
}

- (void)showFind {
    if (!self.findBar) {
        MCTerminalFindBar *bar = [[MCTerminalFindBar alloc]
            initWithFrame:(NSRect){NSZeroPoint, [MCTerminalFindBar preferredSize]}];
        __weak TerminalView *weakSelf = self;
        __weak MCTerminalFindBar *weakBar = bar;
        bar.onChange = ^{
            TerminalView *s = weakSelf;
            MCTerminalFindBar *b = weakBar;
            if (!s || !b) return;
            if (b.field.stringValue.length) [s.grid findText:b.field.stringValue matchCase:b.matchCase];
            else [s.grid endFind];
            [s updateFindCount];
        };
        bar.onNext = ^(BOOL forward) { [weakSelf.grid findNext:forward]; };
        bar.onClose = ^{ [weakSelf closeFind]; };
        self.findBar = bar;
        [self addSubview:bar positioned:NSWindowAbove relativeTo:self.grid];
        [self layoutChildren];
    }
    BOOL wasHidden = self.findBar.hidden;
    self.findBar.hidden = NO;
    // Opened again: the last search is shown again.
    if (wasHidden && self.findBar.field.stringValue.length)
        [self.grid findText:self.findBar.field.stringValue matchCase:self.findBar.matchCase];
    [self updateFindCount];
    [self.findBar focusField];
}

// Esc in the bar: the highlights go and the keyboard returns to the grid.
- (void)closeFind {
    if (!self.findBar || self.findBar.hidden) return;
    BOOL hadFocus = [self.findBar isEditing];
    self.findBar.hidden = YES;
    [self.grid endFind];
    if (hadFocus && _gridMode) [self.window makeFirstResponder:self.grid];
}

- (void)updateFindCount {
    [self.findBar showCount:self.grid.findCount index:self.grid.findIndex
                      limit:self.grid.findLimit];
}

// Terminal > Clear Terminal (Cmd+K), as in Terminal.app and VS Code: the
// scrollback goes with the screen. At zsh's prompt the screen is erased and
// ZLE draws its prompt and what was typed again (clear-screen, through a
// private key); while a program runs, nothing is sent to it, and the
// cursor's line becomes the top one, as VS Code does, so it goes on
// drawing where it was. In line mode the log is emptied, as `clear` does.
- (void)clearTerminal:(id)sender {
    (void)sender;
    if (_closed) return;
    if (_lineMode && !_gridMode) {
        [self.output clearLinkHover];
        self.output.string = @"";
        _liveStart = 0;
        _logLines = 0;
        _stream.breakLine();
        _screen.feed("\x1b[H\x1b[2J");
        return;
    }
    if (!_lineMode && _integrated && _zleReady && _shellPid > 0 && !_screen.altScreen()) {
        _screen.clearAll();
        [self sendBytes:kZleClearKey length:strlen(kZleClearKey)];
    } else {
        _screen.clearToCursorLine();
    }
    [self.grid contentCleared];
}

// Cmd+Left, Cmd+Right, Cmd+Delete in the grid: ZLE's own widgets while it
// reads input; otherwise Ctrl+A, Ctrl+E and Ctrl+U, which readline, Python's
// prompt and Claude Code read that way.
- (void)lineEdit:(MCLineEdit)edit {
    BOOL zle = _integrated && _zleReady && !_screen.altScreen();
    const char *bytes;
    switch (edit) {
    case MCLineEditStart: bytes = zle ? kZleLineStartKey : "\x01"; break;
    case MCLineEditEnd: bytes = zle ? kZleLineEndKey : "\x05"; break;
    case MCLineEditDeleteToStart: bytes = zle ? kZleKillToStartKey : "\x15"; break;
    }
    [self sendBytes:bytes length:strlen(bytes)];
}

// Show the grid while the program needs a screen, the log otherwise.
- (void)updateMode {
    if (!_lineMode) return;      // grid mode: the grid, always
    BOOL want = _master >= 0 && (_screen.altScreen() || _stickyGrid);
    if (want == _gridMode) return;
    BOOL hadFocus = [self hasFocus];
    if (!want) [self closeFind];
    _gridMode = want;
    self.grid.hidden = !want;
    self.outScroll.hidden = want;
    self.prompt.hidden = want;
    self.input.hidden = want;
    self.secureInput.hidden = YES;
    if (!want) {
        [self updateSecureInput];
        [self scrollToEnd];
    }
    [self.grid screenChanged];
    if (hadFocus) [self focusInput];
}

// While a command runs, watch for it taking the pty out of canonical mode
// without switching to the alternate screen (git's `less -X`, a REPL, ssh).
// It must stay that way for two polls, so a program that only flips modes
// briefly (less -F on short output) never flashes the grid.
- (void)startRawWatch {
    if (_rawTimer) return;
    _rawTicks = 0;
    __weak TerminalView *weakSelf = self;
    _rawTimer = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES
                                                  block:^(NSTimer *t) {
        (void)t;
        [weakSelf checkRawMode];
    }];
}

- (void)stopRawWatch {
    [_rawTimer invalidate];
    _rawTimer = nil;
    _rawTicks = 0;
}

- (void)checkRawMode {
    if (_master < 0 || !_commandStarted || _atPrompt) { _rawTicks = 0; return; }
    struct termios t;
    BOOL raw = tcgetattr(_master, &t) == 0 && (t.c_lflag & ICANON) == 0;
    _rawTicks = (raw && !_screen.altScreen()) ? _rawTicks + 1 : 0;
    if (_rawTicks >= 2 && !_stickyGrid) {
        _stickyGrid = YES;
        [self updateMode];
    }
}

// Clicks on the terminal's own background (padding/gaps) also focus input.
- (void)mouseDown:(NSEvent *)event {
    [super mouseDown:event];
    [self focusInput];
}

- (void)setDirectory:(NSString *)dir {
    if (!dir.length) return;
    _cwd = dir;
    [_linkCache removeAllObjects];
    [self updatePrompt];
    if (!_lineMode) {
        // At ZLE's prompt, as a typed command (space: kept out of history);
        // whatever was typed there comes back at the next prompt.
        if (_integrated && [self canTypeNow]) {
            _zleReady = NO;
            _atPrompt = NO;
            [self typeLine:[NSString stringWithFormat:@" cd %@", [self quote:dir]] run:YES];
        }
        return;
    }
    if ([self canSubmitNow] && _integrated) {   // move the live shell too (space: kept out of history)
        _atPrompt = NO;
        [self sendText:[NSString stringWithFormat:@" cd %@\n", [self quote:dir]]];
    }
}

- (void)updatePrompt {
    self.prompt.toolTip = _cwd;
}

- (BOOL)atPrompt { return _atPrompt; }
// Line mode: the grid is showing for a full-screen program. Grid mode,
// where the grid always shows: a program has the alternate screen.
- (BOOL)gridMode { return _lineMode ? _gridMode : _screen.altScreen(); }

- (NSString *)recentText:(NSUInteger)maxLines {
    std::vector<std::string> lines;
    if (!_lineMode || _gridMode) {
        // The grid, scrollback above it; blank rows at the bottom dropped,
        // and rows the terminal wrapped joined into the lines they were.
        int last = _screen.rows() - 1;
        while (last >= 0 && _screen.lineText(last).empty()) last--;
        int first = -_screen.historySize();
        for (int line = last; line >= first && lines.size() < maxLines; line--) {
            int start = line;
            while (start > first && _screen.lineWraps(start - 1)) start--;
            lines.push_back(_screen.textBetween(start, 0, line, _screen.cols() - 1));
            line = start;
        }
        std::reverse(lines.begin(), lines.end());
    } else {
        NSArray<NSString *> *all = [self.output.string componentsSeparatedByString:@"\n"];
        NSUInteger from = all.count > maxLines ? all.count - maxLines : 0;
        for (NSUInteger i = from; i < all.count; i++) lines.push_back(all[i].UTF8String ?: "");
    }
    std::string out;
    for (const std::string &l : lines) out += l + "\n";
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
    return [NSString stringWithUTF8String:out.c_str()] ?: @"";
}
- (BOOL)lineMode { return _lineMode; }
- (NSString *)directory { return _cwd; }

- (BOOL)busy {
    if (_pendingCommand) return YES;
    if (_shellPid <= 0) return NO;
    if (_integrated && _seenPrompt && !_atPrompt) return YES;
    return [self foregroundIsOther];
}

// A program other than the shell holds the terminal's foreground (vim, ssh,
// a server), whatever the marks say. Not while the shell is still starting
// with the integration (its startup files may run programs).
- (BOOL)foregroundIsOther {
    if (_shellPid <= 0 || _master < 0) return NO;
    pid_t fg = tcgetpgrp(_master);
    return fg > 0 && fg != _shellPid && (_seenPrompt || !_integrated);
}

// The shell itself holds the terminal's foreground: what is typed now goes
// to it, not to a program it runs.
- (BOOL)shellOwnsTerminal {
    return _shellPid > 0 && _master >= 0 && tcgetpgrp(_master) == _shellPid;
}

// Grid mode: whether a line typed now reaches ZLE at the shell's own prompt.
// The marks say where the shell is (133;B: ZLE reads input), and the
// foreground says nobody else has the terminal: marks with the nonce could
// still be stale, and a program the marks missed would get the keys. With
// no B (a plugin replaced the hook at the first prompt, before ours was put
// back), a prompt counts as ready once ZLE has the terminal in raw mode,
// 0.3 s after 133;A. Without the integration (after `exec bash`), the
// foreground alone decides.
- (BOOL)canTypeNow {
    if (_closed || ![self shellOwnsTerminal]) return NO;
    if (!_integrated) return YES;
    if (_screen.altScreen()) return NO;
    if (_zleReady) return YES;
    if (!_atPrompt || _commandStarted || CACurrentMediaTime() - _promptTime < 0.3) return NO;
    struct termios t;
    if (tcgetattr(_master, &t) != 0 || (t.c_lflag & ICANON)) return NO;
    _zleReady = YES;
    return YES;
}

// Line mode: whether a command can be submitted now (zsh at its prompt with
// its line editor off, no program on the grid).
- (BOOL)canSubmitNow {
    if (_closed || ![self shellOwnsTerminal]) return NO;
    return !_integrated || (_atPrompt && !_gridMode);
}

- (NSString *)pendingCommand { return _closed ? nil : _pendingCommand; }

// Everything closing this terminal would end or drop: the command in the
// foreground, a command waiting for the first prompt, and the shell's
// background jobs. Empty for a shell idle at its prompt with no jobs, or one
// still starting with nothing queued.
- (NSArray<NSString *> *)runningCommands {
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    if (_closed) return out;
    NSString *fg = [self foregroundCommand];
    if (fg) [out addObject:fg];
    if (_pendingCommand)
        [out addObject:[_pendingCommand stringByAppendingString:@" (waiting to start)"]];
    [out addObjectsFromArray:[self backgroundJobs]];
    return out;
}

- (NSString *)runningCommand { return self.runningCommands.firstObject; }

- (pid_t)shellPid { return _shellPid; }

// The shell's jobs that are not in the foreground: every process whose
// controlling terminal is this pty, in a process group other than the
// shell's and the foreground one, so "sleep 100 &" counts, one entry per
// job (a pipeline is one group), named by the group's leader. zsh hangs
// these up when it is closed. A job disowned with &! is counted too, since
// nothing outside the shell can tell it apart; a daemon that left the
// terminal (setsid, nohup's detach) is not.
- (NSArray<NSString *> *)backgroundJobs {
    if (_shellPid <= 0 || _master < 0 || (_integrated && !_seenPrompt)) return @[];
    pid_t fg = tcgetpgrp(_master);
    // The pty, as the shell's controlling terminal (macOS answers nothing
    // for KERN_PROC_SESSION).
    struct kinfo_proc shell = {};
    size_t size = sizeof shell;
    int pmib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, _shellPid};
    if (sysctl(pmib, 4, &shell, &size, nullptr, 0) != 0 || size == 0 ||
        shell.kp_eproc.e_tdev == NODEV)
        return @[];
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_TTY, (int)shell.kp_eproc.e_tdev};
    size = 0;
    if (sysctl(mib, 4, nullptr, &size, nullptr, 0) != 0 || size == 0) return @[];
    size += 16 * sizeof(struct kinfo_proc);   // room for a few more since
    std::vector<struct kinfo_proc> procs(size / sizeof(struct kinfo_proc));
    if (sysctl(mib, 4, procs.data(), &size, nullptr, 0) != 0) return @[];
    procs.resize(size / sizeof(struct kinfo_proc));
    std::map<pid_t, std::pair<pid_t, BOOL>> groups;   // pgid -> (who names it, stopped)
    for (const struct kinfo_proc &k : procs) {
        pid_t pid = k.kp_proc.p_pid, pgid = k.kp_eproc.e_pgid;
        if (pid == _shellPid || pgid == _shellPid || pgid == fg) continue;
        if (k.kp_proc.p_stat == SZOMB) continue;
        BOOL stopped = k.kp_proc.p_stat == SSTOP;
        auto it = groups.find(pgid);
        if (it == groups.end()) groups[pgid] = {pid, stopped};
        else {
            if (pid == pgid) it->second.first = pid;   // the leader names the job
            it->second.second = it->second.second && stopped;
        }
    }
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (const auto &g : groups) {
        NSString *name = ProcessCommandLine(g.second.first);
        if (!name) {
            char n[2 * MAXCOMLEN + 1] = {0};
            name = proc_name(g.second.first, n, sizeof n) > 0 ? @(n) : @"A job";
        }
        [out addObject:[name stringByAppendingString:g.second.second ? @" (stopped)"
                                                                     : @" (in the background)"]];
    }
    return out;
}

// A command the shell integration says is running (133;C seen, no prompt
// since), or any program that holds the terminal's foreground (vim, a
// server, Claude Code), even one the marks missed. nil for a shell at its
// prompt, or still starting.
- (NSString *)foregroundCommand {
    if (_closed || _shellPid <= 0 || _master < 0) return nil;
    pid_t fg = tcgetpgrp(_master);
    BOOL other = fg > 0 && fg != _shellPid && (_seenPrompt || !_integrated);
    BOOL marked = _integrated && _seenPrompt && !_atPrompt && _commandStarted;
    if (!other && !marked) return nil;
    NSString *typed = [_runningCommand stringByTrimmingCharactersInSet:
                       [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (typed.length) return typed;
    if (other) {
        char name[2 * MAXCOMLEN + 1] = {0};
        if (proc_name(fg, name, sizeof name) > 0) return @(name);
    }
    return @"A command";
}

// A path in the command shows as its last part, so "/x/.venv/bin/python
// /x/app/main.py" reads "python main.py".
- (void)setFixedTitle:(NSString *)title {
    _fixedTitle = title.length ? [title copy] : nil;
    [self titleChanged];
}

- (NSString *)title {
    if (_fixedTitle) return _fixedTitle;     // minicode term --title
    if (_programTitle.length) return _programTitle;
    if (_runningCommand.length) {
        NSMutableArray *words = [NSMutableArray array];
        for (NSString *w in [_runningCommand componentsSeparatedByString:@" "]) {
            NSString *bare = [w stringByTrimmingCharactersInSet:
                [NSCharacterSet characterSetWithCharactersInString:@"'\""]];
            [words addObject:[bare hasPrefix:@"/"] && bare.length > 1
                                 ? bare.lastPathComponent : w];
        }
        return [words componentsJoinedByString:@" "];
    }
    if ([_cwd isEqualToString:NSHomeDirectory()]) return @"~";
    return _cwd.lastPathComponent.length ? _cwd.lastPathComponent : @"zsh";
}

- (void)titleChanged {
    if (self.onTitleChange) self.onTitleChange();
}

// OSC 0 or 2. A program's title (vim, ssh, Claude Code) names the tab while
// it runs; the shell's own, set at its prompt, does not, so only titles
// that come between a command's start (133;C) and the next prompt count.
// Without the shell integration there are no marks, and every title counts.
- (void)programTitled:(const std::string &)text {
    if (_integrated && !_commandStarted) return;
    NSString *title = [[NSString alloc] initWithBytes:text.data() length:text.size()
                                             encoding:NSUTF8StringEncoding];
    title = [title stringByTrimmingCharactersInSet:
             [NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (!title.length) title = nil;
    if (title == _programTitle || [title isEqualToString:_programTitle]) return;
    _programTitle = title;
    [self titleChanged];
}

// The prompt is back: the program's title goes, and so does mouse
// reporting or focus reporting it left on (a program killed before it
// could turn them off), or the next click would type into the prompt.
- (void)promptReturned {
    if (_programTitle) {
        _programTitle = nil;
        [self titleChanged];
    }
    if (!_screen.altScreen() && (_screen.mouseTracking() || _screen.focusEvents()))
        _screen.resetMouseModes();
    if (!_screen.altScreen() && _screen.synchronizedOutput())
        _screen.feed("\x1b[?2026l");    // a frame a killed program never ended
}

- (void)runCommand:(NSString *)line {
    if (_closed || !line.length) return;
    if (_shellPid <= 0) {
        _pendingCommand = line;
        [self startShell];
        return;
    }
    if (!_lineMode) {
        if ([self canTypeNow]) {
            // The prompt is used up: until the command's own marks arrive,
            // a second command must wait (or go to a new tab), not be typed
            // into the first one's program.
            _zleReady = NO;
            if (_integrated) _atPrompt = NO;
            [self typeLine:line run:YES];
        } else {
            _pendingCommand = line;
            [self schedulePendingRetry];
        }
        return;
    }
    if ([self canSubmitNow]) {
        if (_integrated) _atPrompt = NO;
        [self submitCommand:line];
    } else {
        _pendingCommand = line;
        [self schedulePendingRetry];
    }
}

// What waits to be typed (runCommand:, typeCommand:) goes in once the shell
// can take it; until then this looks again every 0.1 s, since the foreground
// or the raw tty that canTypeNow waits on sends no event of its own.
- (void)flushPendingInput {
    if (_closed || (!_pendingCommand && !_pendingTyped)) return;
    if (_lineMode) {
        if (_pendingCommand && [self canSubmitNow]) {
            NSString *line = _pendingCommand;
            _pendingCommand = nil;
            if (_integrated) _atPrompt = NO;
            [self submitCommand:line];
        }
    } else if ([self canTypeNow]) {
        if (_pendingCommand) {
            NSString *line = _pendingCommand;
            _pendingCommand = nil;
            _zleReady = NO;
            if (_integrated) _atPrompt = NO;
            [self typeLine:line run:YES];
        } else if (_pendingTyped) {
            NSString *line = _pendingTyped;
            _pendingTyped = nil;
            [self typeLine:line run:NO];
        }
    }
    [self schedulePendingRetry];
}

- (void)schedulePendingRetry {
    if (_retryScheduled || _closed || _shellPid <= 0 || (!_pendingCommand && !_pendingTyped))
        return;
    _retryScheduled = YES;
    __weak TerminalView *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{
        TerminalView *s = weakSelf;
        if (!s) return;
        s->_retryScheduled = NO;
        [s flushPendingInput];
    });
}

// Grid mode: type a line into ZLE, as if the user had, after the private key
// that puts aside what they had typed (push-input). `run` presses Return;
// otherwise the line is left at the prompt, pasted in brackets when ZLE asked
// for them (so a newline in it is text, not Return), else with any newline
// turned into a space.
- (void)typeLine:(NSString *)line run:(BOOL)run {
    std::string out = _integrated ? kZlePushKey : "";
    const char *utf8 = line.UTF8String;
    if (!utf8) return;
    // Typed outside brackets, a control character is a key: ESC would start
    // a ZLE binding, DEL rub out, Ctrl+C interrupt. Only line breaks (run:
    // Return; otherwise a space) and, when run, tabs stay.
    std::string text;
    for (const char *c = utf8; *c; c++) {
        unsigned char b = (unsigned char)*c;
        if (b == '\n' || b == '\r') text += run ? (char)b : ' ';
        else if (b == '\t') text += run ? '\t' : ' ';
        else if (b < 0x20 || b == 0x7F) continue;
        else text += (char)b;
    }
    if (run) {
        out += TerminalScreen::encodePaste(text, false);
        out += "\r";
    } else if (_screen.bracketedPaste()) {
        out += TerminalScreen::encodePaste(utf8, true);
    } else {
        out += text;
    }
    [self.grid scrollToBottom];
    [self sendBytes:out.data() length:out.size()];
}

// `exec bash` (or fish, sh, ...) replaces zsh with a shell that sends none
// of the integration's marks, and the tab stayed busy for good, waiting for
// a prompt that never came. While a command runs, the shell's process is
// looked at twice a second; once it is another shell, this terminal works
// as one without the integration: busy only while a program other than the
// shell holds the foreground (foregroundCommand), commands typed at once.
// A zsh exec'd from there loads the integration again (the exported
// ZDOTDIR), and its first prompt mark turns it back on.
static BOOL MCIsOtherShell(pid_t pid) {
    char name[2 * MAXCOMLEN + 1] = {0};
    if (proc_name(pid, name, sizeof name) <= 0) return NO;
    NSString *n = [@(name) lowercaseString];
    if ([n hasPrefix:@"-"]) n = [n substringFromIndex:1];
    static NSSet<NSString *> *shells;
    if (!shells)
        shells = [NSSet setWithArray:@[@"bash", @"sh", @"dash", @"fish", @"ksh", @"mksh",
                                       @"oksh", @"tcsh", @"csh", @"nu", @"elvish", @"xonsh",
                                       @"pwsh", @"yash", @"ion", @"oil", @"osh"]];
    return [shells containsObject:n];
}

- (void)watchShell {
    if (_shellWatch || !_integrated || _shellPid <= 0) return;
    __weak TerminalView *weakSelf = self;
    _shellWatch = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:YES
                                                    block:^(NSTimer *t) {
        TerminalView *s = weakSelf;
        if (!s) { [t invalidate]; return; }
        [s checkShell];
    }];
}

- (void)checkShell {
    if (!_integrated || _atPrompt || _shellPid <= 0) {
        [_shellWatch invalidate];
        _shellWatch = nil;
        return;
    }
    if (!MCIsOtherShell(_shellPid)) return;
    [_shellWatch invalidate];
    _shellWatch = nil;
    _integrated = NO;
    _commandStarted = NO;
    _zleReady = NO;
    _atPrompt = NO;
    _runningCommand = nil;
    _programTitle = nil;
    [self stopRawWatch];
    _stickyGrid = NO;
    [self titleChanged];
    [self postCommandFinished];
    // What waited for a prompt goes in once the new shell has the terminal.
    if (_lineMode) [self updateMode];
    [self flushPendingInput];
}

// A prompt mark after the marks stopped: a zsh with the integration is the
// shell again (exec zsh from bash).
- (void)integrationReturned {
    if (_integrated || !_integrationFile) return;
    _integrated = YES;
}

- (void)postCommandFinished {
    [[NSNotificationCenter defaultCenter]
        postNotificationName:MCTerminalCommandDidFinishNotification object:self];
}

- (BOOL)typeCommand:(NSString *)line {
    if (!_lineMode) {
        if (_closed || !line.length || _screen.altScreen() || self.busy) return NO;
        if (_shellPid <= 0) [self startShell];
        if ([self canTypeNow]) [self typeLine:line run:NO];
        else {
            _pendingTyped = line;   // typed when ZLE starts reading
            [self schedulePendingRetry];
        }
        [self.window makeFirstResponder:self.grid];
        return YES;
    }
    if (_closed || !line.length || _gridMode || self.busy) return NO;
    if (_shellPid <= 0) [self startShell];
    self.input.stringValue = line;
    [self.window makeFirstResponder:self.input];
    // The field editor selects everything it is given; the caret goes to
    // the end instead, so a key typed by accident does not replace it.
    NSText *editor = self.input.currentEditor;
    if (editor) editor.selectedRange = NSMakeRange(line.length, 0);
    return YES;
}

// As if typed into the input line and entered; whatever the user had typed
// there is kept.
- (void)submitCommand:(NSString *)line {
    NSString *draft = self.input.stringValue;
    self.input.stringValue = line;
    [self inputEntered:self.input];
    self.input.stringValue = draft;
}

- (void)close {
    _closed = YES;
    _pendingCommand = nil;
    _pendingTyped = nil;
    if (_flagsMonitor) { [NSEvent removeMonitor:_flagsMonitor]; _flagsMonitor = nil; }
    [self stopShell];
}

// ---------------------------------------------------------------- input
- (void)inputEntered:(id)sender {
    NSTextField *field = sender;
    NSString *line = field.stringValue;
    field.stringValue = @"";

    if (_shellPid <= 0) {   // the shell exited and wasn't restarted yet
        if (!_closed) [self startShell];
        return;
    }

    if (_integrated && !_atPrompt) {
        // Input for the running program, or the next line of a multi-line
        // command zsh is still reading. Once the command runs the pty echoes
        // what we send (unless the program disabled echo); before that, echo
        // is off, so show the line ourselves.
        if (!_commandStarted && field == self.input)
            [self appendLine:line color:THex(0x569CD6)];
        [self sendText:[line stringByAppendingString:@"\n"]];
        return;
    }

    NSString *abbrev = [_cwd stringByAbbreviatingWithTildeInPath];
    [self ensureNewline];
    [self appendLine:[NSString stringWithFormat:@"%@ ❯ %@", abbrev, line]
               color:THex(0x569CD6)];

    NSString *trimmed = [line stringByTrimmingCharactersInSet:
                         [NSCharacterSet whitespaceCharacterSet]];
    if (!trimmed.length) return;
    [_history addObject:line];
    _historyIdx = _history.count;
    if ([trimmed isEqualToString:@"clear"]) {   // handled locally
        [self.output clearLinkHover];
        self.output.string = @"";
        _liveStart = 0;
        _logLines = 0;
        _stream.breakLine();
        _screen.feed("\x1b[H\x1b[2J");
        return;
    }

    if (_integrated) {
        _atPrompt = NO;
        _commandStarted = NO;
        _userCommand = YES;
        _runningCommand = trimmed;
        [self titleChanged];
    }
    [self sendText:[line stringByAppendingString:@"\n"]];
}

// Is `field`'s field editor the current first responder?
- (BOOL)isEditing:(NSTextField *)field {
    id fr = self.window.firstResponder;
    return [fr isKindOfClass:[NSTextView class]] &&
           ((NSTextView *)fr).delegate == (id)field;
}

// Up/Down arrow -> command history. Control keys -> the pty.
- (BOOL)control:(NSControl *)control textView:(NSTextView *)tv
    doCommandBySelector:(SEL)sel {
    NSEvent *ev = NSApp.currentEvent;
    NSEventModifierFlags mods = ev.modifierFlags &
        NSEventModifierFlagDeviceIndependentFlagsMask;
    if (ev.type == NSEventTypeKeyDown &&
        (mods & NSEventModifierFlagControl) &&
        !(mods & (NSEventModifierFlagCommand | NSEventModifierFlagOption))) {
        NSString *key = ev.charactersIgnoringModifiers.lowercaseString;
        NSTextField *field = (NSTextField *)control;
        BOOL running = _integrated ? !_atPrompt : YES;
        if ([key isEqualToString:@"c"]) {
            field.stringValue = @"";
            if (running) [self sendControl:0x03];   // SIGINT via the tty
            return YES;
        }
        if ([key isEqualToString:@"d"] && field.stringValue.length == 0) {
            if (running) [self sendControl:0x04];   // EOF for the program
            return YES;                             // never exits the shell
        }
        if ([key isEqualToString:@"z"] && running) {
            [self sendControl:0x1A];                // SIGTSTP
            return YES;
        }
        if ([key isEqualToString:@"\\"] && running) {
            [self sendControl:0x1C];                // SIGQUIT
            return YES;
        }
        // The shell's own line editing, which the input line stands in for:
        // Ctrl+W deletes the word before the caret (up to the space before
        // it, as bash and zsh do), Ctrl+U everything before the caret. A
        // selection goes first, whole.
        if ([key isEqualToString:@"w"] || [key isEqualToString:@"u"]) {
            NSString *s = tv.string;
            NSRange sel = tv.selectedRange;
            NSRange cut = sel;
            if (sel.length == 0) {
                NSUInteger p = MIN(sel.location, s.length), q = p;
                if ([key isEqualToString:@"u"]) {
                    q = 0;
                } else {
                    while (q > 0 && [s characterAtIndex:q - 1] == ' ') q--;
                    while (q > 0 && [s characterAtIndex:q - 1] != ' ') q--;
                }
                cut = NSMakeRange(q, p - q);
            }
            if (cut.length && [tv shouldChangeTextInRange:cut replacementString:@""]) {
                [tv replaceCharactersInRange:cut withString:@""];
                [tv didChangeText];
                tv.selectedRange = NSMakeRange(cut.location, 0);
            }
            return YES;
        }
    }

    if (control == self.secureInput) return NO;   // no history for secrets
    if (sel == @selector(moveUp:)) {
        if (_historyIdx > 0) {
            _historyIdx--;
            self.input.stringValue = _history[_historyIdx];
        }
        return YES;
    }
    if (sel == @selector(moveDown:)) {
        if (_historyIdx < (NSInteger)_history.count - 1) {
            _historyIdx++;
            self.input.stringValue = _history[_historyIdx];
        } else {
            _historyIdx = _history.count;
            self.input.stringValue = @"";
        }
        return YES;
    }
    return NO;
}

// Show the secure field while a running program has echo turned off.
- (void)updateSecureInput {
    if (_gridMode) return;       // the grid takes keys itself
    BOOL hidden = NO;
    if (_master >= 0 && _commandStarted && !_atPrompt) {
        struct termios t;
        if (tcgetattr(_master, &t) == 0) hidden = (t.c_lflag & ECHO) == 0;
    }
    if (hidden == !self.secureInput.hidden) return;
    NSTextField *from = [self activeInput];
    BOOL hadFocus = [self isEditing:from];
    self.secureInput.hidden = !hidden;
    self.input.hidden = hidden;
    NSTextField *to = [self activeInput];
    to.stringValue = @"";
    from.stringValue = @"";
    if (hadFocus) [self.window makeFirstResponder:to];
}

// ------------------------------------------------------------ the pty shell
- (void)dealloc {
    if (_flagsMonitor) [NSEvent removeMonitor:_flagsMonitor];
    [self stopShell];
}

// ---------------------------------------------------------------- links
// Cmd+click on terminal output. TermLinks (pure C++) finds what looks like a
// URL or a file reference in a line; this resolves a file against the
// shell's directory and then the project folder, and keeps only files that
// exist, so "e.g." or a word that happens to have a dot never lights up.

// Pressing or releasing Cmd without moving the mouse still shows or hides
// the underline. flagsChanged: only reaches the first responder, which is
// usually the input line, so a monitor watches for it instead.
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (_flagsMonitor) { [NSEvent removeMonitor:_flagsMonitor]; _flagsMonitor = nil; }
    if (!self.window) return;
    __weak TerminalView *weakSelf = self;
    _flagsMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskFlagsChanged
                                                          handler:^NSEvent *(NSEvent *e) {
        TerminalView *s = weakSelf;
        if (s && e.window == s.window && !s.hidden) {
            if (s->_gridMode) [s.grid updateLinkHover];
            else [s.output updateLinkHover];
        }
        return e;
    }];
}

// Every place a path as written exists, newest first, looked for under the
// shell's folder, the window's, then extraLinkRoots (AgentIndex::resolveTarget);
// *tried gets the roots looked in. Cached until the next output or cwd change.
- (NSArray<MCTermLinkMatch *> *)matchesForPath:(NSString *)written tried:(NSArray **)tried {
    if (!_linkCache) _linkCache = [NSMutableDictionary dictionary];
    CFTimeInterval now = CACurrentMediaTime();
    NSArray *cached = _linkCache[written];
    // Not found is believed for a moment only: the file may be on its way.
    if (cached && now - [cached[2] doubleValue] < ([cached[0] count] ? 1.0 : 0.25)) {
        if (tried) *tried = cached[1];
        return cached[0];
    }
    AgentIndex::Roots roots;
    roots.cwd = _cwd.UTF8String ?: "";
    roots.folder = self.projectRoot.UTF8String ?: "";
    // The worktrees (which list a folder) at most every two seconds.
    if (self.extraLinkRoots && (!_linkRoots || now - _linkRootsTime > 2.0)) {
        _linkRoots = [self.extraLinkRoots() copy];
        _linkRootsTime = now;
    }
    for (NSString *r in _linkRoots) roots.worktrees.push_back(r.UTF8String ?: "");
    roots.home = NSHomeDirectory().UTF8String ?: "";
    AgentIndex::Resolution res =
        AgentIndex::resolveTarget(written.UTF8String ?: "", 0, 0, roots, AgentTranscript::posixFs());
    // A file under the shell's own folder is the one meant, whatever copies
    // agents' worktrees hold: those used to make every such link a picker,
    // the copy an agent touched last first.
    std::string cwd = AgentIndex::normalizePath(roots.cwd);
    bool inCwd = false;
    for (const AgentIndex::Match &m : res.matches)
        if (!cwd.empty() && m.root == cwd) inCwd = true;
    NSMutableArray<MCTermLinkMatch *> *matches = [NSMutableArray array];
    NSMutableSet<NSString *> *seen = [NSMutableSet set];
    for (const AgentIndex::Match &m : res.matches) {
        if (inCwd && m.root != cwd) continue;
        // Spelled the way the window spells its root (/tmp, not the
        // /private/tmp the shell reports), so a file inside the folder is
        // recognised as inside it.
        NSString *path = [@(m.path.c_str()) stringByStandardizingPath].stringByResolvingSymlinksInPath;
        if (!path.length || [seen containsObject:path]) continue;
        [seen addObject:path];
        MCTermLinkMatch *x = [MCTermLinkMatch new];
        x.path = path;
        x.modified = m.mtime;
        x.isDirectory = m.isDir;
        [matches addObject:x];
    }
    NSMutableArray<NSString *> *looked = [NSMutableArray array];
    for (const std::string &t : res.tried) [looked addObject:@(t.c_str())];
    if (_linkCache.count > 500) [_linkCache removeAllObjects];
    _linkCache[written] = @[matches, looked, @(now)];
    if (tried) *tried = looked;
    return matches;
}

- (MCTermLink *)linkInLine:(const std::string &)line atByte:(size_t)byte {
    std::vector<TermLinks::Link> links = TermLinks::find(line);
    const TermLinks::Link *l = TermLinks::at(links, byte);
    if (!l) return nil;
    NSString *target = [[NSString alloc] initWithBytes:l->target.data()
                                                length:l->target.size()
                                              encoding:NSUTF8StringEncoding];
    if (!target.length) return nil;
    MCTermLink *link = [MCTermLink new];
    link.byteStart = l->start;
    link.byteLength = l->length;
    link.line = l->line;
    link.column = l->column;
    if (l->kind == TermLinks::Link::Url) {
        link.isURL = YES;
        link.target = target;
        return link;
    }
    NSArray *tried = nil;
    NSArray<MCTermLinkMatch *> *matches = [self matchesForPath:target tried:&tried];
    link.matches = matches;
    if (!matches.count) {
        // Found nowhere: still a link when it is plainly a path, so a click
        // can say where it was looked for.
        if (l->line <= 0 && [target rangeOfString:@"/"].location == NSNotFound) return nil;
        link.target = target;
        link.tried = tried;
        return link;
    }
    link.target = matches.firstObject.path;
    link.isDirectory = matches.firstObject.isDirectory;
    return link;
}

// The link at a character of the log view, and its span there in UTF-16.
- (MCTermLink *)linkInOutputAt:(NSUInteger)index range:(NSRange *)range {
    NSString *all = self.output.string;
    if (index >= all.length) return nil;
    NSRange lineRange = [all lineRangeForRange:NSMakeRange(index, 0)];
    NSUInteger end = NSMaxRange(lineRange);
    while (end > lineRange.location && [all characterAtIndex:end - 1] == '\n') end--;
    if (index >= end) return nil;
    NSString *line = [all substringWithRange:
                      NSMakeRange(lineRange.location, end - lineRange.location)];
    // Start of the composed character, so the prefix never ends inside a
    // surrogate pair.
    NSUInteger offset = [line rangeOfComposedCharacterSequenceAtIndex:
                         index - lineRange.location].location;
    NSUInteger byte = [[line substringToIndex:offset]
                       lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    const char *utf8 = line.UTF8String;
    if (!utf8) return nil;
    std::string text(utf8);
    MCTermLink *link = [self linkInLine:text atByte:byte];
    if (!link) return nil;
    if (range) {
        NSString *before = [[NSString alloc] initWithBytes:text.data()
                                                    length:link.byteStart
                                                  encoding:NSUTF8StringEncoding];
        NSString *span = [[NSString alloc] initWithBytes:text.data() + link.byteStart
                                                  length:link.byteLength
                                                encoding:NSUTF8StringEncoding];
        if (!before || !span) return nil;
        *range = NSMakeRange(lineRange.location + before.length, span.length);
    }
    return link;
}

- (void)stopShell {
    _generation++;                     // ignore anything still in flight
    [_shellWatch invalidate];
    _shellWatch = nil;
    [self stopRawWatch];
    MCPty *pty = _pty;                 // closes the fd and reaps the shell
    if (pty) dispatch_async(_ioQueue, ^{ [pty shutdown]; });
    _pty = nil;
    _backlog = nil;
    _backlogStart = 0;
    if (_shellPid > 0) kill(_shellPid, SIGHUP);
    _shellPid = 0;
    _master = -1;
}

// Rows and columns of grid cells that fit the panel, pushed to the pty so
// programs that format for the width (ls, git) and full-screen programs get
// it right; the kernel sends SIGWINCH to the foreground job on a change. The
// same size serves both modes, so switching never resizes the program.
- (void)updateWindowSize {
    NSSize cell = [TerminalGridView cellSize];
    CGFloat pad = [TerminalGridView padding];
    NSSize size = self.bounds.size;
    // A hidden dock (height 0), a collapsed pane (width 0) or a view not
    // laid out yet keeps the last size, rather than squeezing the screen
    // (and cutting its lines) down to nothing. Anything with room for a
    // couple of cells gets its true size, however narrow: a pane of a
    // three-way split in a small window used to keep the size it had before
    // the split, and its lines ran off its right edge.
    if (size.width < 2 * pad + 2 * cell.width || size.height < 2 * pad + 2 * cell.height) {
        if (_winsize.ws_col) return;
        size = NSMakeSize(MAX(size.width, 400), MAX(size.height, 220));
    }
    struct winsize ws = {};
    ws.ws_col = (unsigned short)MAX(2, floor((size.width - 2 * pad) / cell.width));
    ws.ws_row = (unsigned short)MAX(2, floor((size.height - 2 * pad) / cell.height));
    ws.ws_xpixel = (unsigned short)(ws.ws_col * cell.width);
    ws.ws_ypixel = (unsigned short)(ws.ws_row * cell.height);
    if (ws.ws_col == _winsize.ws_col && ws.ws_row == _winsize.ws_row) return;
    _winsize = ws;
    if (self.grid.screen) [self.grid resizeScreenToCols:ws.ws_col rows:ws.ws_row];
    else _screen.resize(ws.ws_col, ws.ws_row);
    [self.grid screenChanged];
    if (_master >= 0) ioctl(_master, TIOCSWINSZ, &_winsize);
}

- (void)startShell {
    [self stopShell];

    NSString *zdot = IntegrationDirectory(!_lineMode);
    _integrated = zdot != nil;
    _integrationFile = _integrated;
    if (!_integrated)
        [self note:@"Could not write the shell integration file; "
                    "commands still run, but without exit status or "
                    "directory tracking." color:0xF48771];

    // Everything the child needs is prepared before fork(): in a threaded
    // process the child may only make async-signal-safe calls until exec.
    std::vector<std::string> env;
    std::string userZdotdir;
    bool hadZdotdir = false, hasLocale = false;
    for (char **e = environ; *e; e++) {
        std::string kv = *e;
        std::string key = kv.substr(0, kv.find('='));
        if (key == "ZDOTDIR") {
            hadZdotdir = true;
            userZdotdir = kv.substr(kv.find('=') + 1);
            if (_integrated) continue;
        }
        if (key == "TERM" || key == "TERM_PROGRAM" ||
            key == "TERM_PROGRAM_VERSION" || key == "TERM_SESSION_ID" ||
            key == "COLORTERM" ||
            key == "COLUMNS" || key == "LINES" ||
            key == "MINICODE_USER_ZDOTDIR" || key == "MINICODE_NONCE" ||
            key == "MINICODE_EXEC")
            continue;
        if (key == "LANG" || key == "LC_ALL" || key == "LC_CTYPE")
            hasLocale = true;
        env.push_back(kv);
    }
    // Colors, cursor movement and full-screen programs are all understood:
    // the log follows ordinary output and the grid takes over for programs
    // that draw a screen, so pagers are left to the user's own settings.
    env.push_back("TERM=xterm-256color");
    env.push_back("COLORTERM=truecolor");
    env.push_back("TERM_PROGRAM=MiniCode");
    if (!hasLocale) env.push_back("LANG=en_US.UTF-8");  // apps get no LANG
    // A nonce per shell for the integration's marks (TerminalStream's
    // setMarkNonce): 128 random bits as hex.
    unsigned char raw[16];
    arc4random_buf(raw, sizeof raw);
    static const char hex[] = "0123456789abcdef";
    std::string nonce;
    for (unsigned char b : raw) { nonce += hex[b >> 4]; nonce += hex[b & 15]; }
    if (_integrated) {
        env.push_back(std::string("ZDOTDIR=") + zdot.fileSystemRepresentation);
        if (hadZdotdir) env.push_back("MINICODE_USER_ZDOTDIR=" + userZdotdir);
        env.push_back("MINICODE_NONCE=" + nonce);
    }
    std::vector<char *> envp;
    for (std::string &s : env) envp.push_back(&s[0]);
    envp.push_back(nullptr);

    // Leading '-': login shell. Line mode turns the line editor off (+Z);
    // grid mode keeps it.
    char arg0[] = "-zsh", argI[] = "-i", argZ[] = "+Z";
    char *argv[] = {arg0, argI, _lineMode ? argZ : nullptr, nullptr};
    std::string cwd = _cwd.fileSystemRepresentation;
    long maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) maxFd = 65536;

    _stream = TerminalStream();
    _markNonce = nonce;
    // Without the integration file no mark is ours, so none is believed.
    _stream.setMarkNonce(nonce);
    _stream.setEventsOnly(!_lineMode);   // grid mode: the screen draws, the stream reports
    _promptTime = 0;
    _seenPrompt = NO;
    _runningCommand = nil;
    _programTitle = nil;
    _atPrompt = NO;
    _commandStarted = NO;
    _userCommand = NO;
    _stickyGrid = NO;
    _zleReady = NO;
    _winsize = {};
    [self updateWindowSize];
    struct winsize ws = _winsize;
    if (_lineMode || !_screenMade) {
        _screen = TerminalScreen(ws.ws_col, ws.ws_row);
        if (!_lineMode)
            _screen.setScrollbackLimit([AppSettings shared].settings.terminalScrollback());
        _screenMade = YES;
    }

    // The terminal's modes, given to forkpty so the child starts with them:
    // the usual defaults plus IUTF8 (Backspace over a multi-byte character).
    // Set on the master after forkpty returned, it raced the child's own
    // tcsetattr.
    struct termios tio = {};
    tio.c_iflag = TTYDEF_IFLAG | IUTF8;
    tio.c_oflag = TTYDEF_OFLAG;
    tio.c_cflag = TTYDEF_CFLAG;
    tio.c_lflag = TTYDEF_LFLAG;
    tio.c_cc[VEOF] = CEOF;
    tio.c_cc[VEOL] = _POSIX_VDISABLE;
    tio.c_cc[VEOL2] = _POSIX_VDISABLE;
    tio.c_cc[VERASE] = CERASE;
    tio.c_cc[VWERASE] = CWERASE;
    tio.c_cc[VKILL] = CKILL;
    tio.c_cc[VREPRINT] = CREPRINT;
    tio.c_cc[VINTR] = CINTR;
    tio.c_cc[VQUIT] = CQUIT;
    tio.c_cc[VSUSP] = CSUSP;
    tio.c_cc[VDSUSP] = CDSUSP;
    tio.c_cc[VSTART] = CSTART;
    tio.c_cc[VSTOP] = CSTOP;
    tio.c_cc[VLNEXT] = CLNEXT;
    tio.c_cc[VDISCARD] = CDISCARD;
    tio.c_cc[VMIN] = CMIN;
    tio.c_cc[VTIME] = CTIME;
    tio.c_cc[VSTATUS] = CSTATUS;
    cfsetispeed(&tio, TTYDEF_SPEED);
    cfsetospeed(&tio, TTYDEF_SPEED);

    int master = -1;
    pid_t pid = forkpty(&master, nullptr, &tio, &ws);
    if (pid == 0) {
        // Child: restore default signal handling, drop inherited descriptors.
        int sigs[] = {SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU, SIGPIPE,
                      SIGCHLD, SIGHUP, SIGTERM, SIGALRM, SIGWINCH};
        for (int s : sigs) signal(s, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        for (long fd = 3; fd < maxFd; fd++) close((int)fd);
        if (chdir(cwd.c_str()) != 0) (void)chdir("/");
        execve("/bin/zsh", argv, envp.data());
        _exit(127);
    }
    if (pid < 0) {
        [self note:[NSString stringWithFormat:@"Could not start shell: %s",
                    strerror(errno)] color:0xF48771];
        return;
    }

    fcntl(master, F_SETFD, FD_CLOEXEC);

    _master = master;
    _shellPid = pid;
    _shellStarted = [NSDate date];
    int generation = ++_generation;

    __weak TerminalView *weakSelf = self;
    MCPty *pty = [[MCPty alloc] initWithFd:master pid:pid queue:_ioQueue];
    __weak MCPty *weakPty = pty;
    pty.deliver = ^(NSData *data) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf enqueue:data pty:weakPty generation:generation];
        });
    };
    pty.released = ^{
        TerminalView *s = weakSelf;
        if (s && s->_pty && s->_pty == weakPty) s->_master = -1;
    };
    pty.exited = ^{ [weakSelf shellExited:generation]; };
    _pty = pty;
    _backlog = [NSMutableData data];
    _backlogStart = 0;
    [pty start];
}

- (void)shellExited:(int)generation {
    if (generation != _generation || _closed) return;   // replaced or torn down
    // The last of its output first, so the note goes below it.
    while (_backlog.length > _backlogStart) [self drainBacklog];
    _pty = nil;
    _master = -1;
    _shellPid = 0;
    _atPrompt = NO;
    _zleReady = NO;
    _stickyGrid = NO;
    [self stopRawWatch];
    [self updateMode];           // back to the log (_master is -1 now)
    [self updateSecureInput];
    if (_lineMode) [self ensureNewline];
    else _screen.feed("\x1b[?1049l\x1b[!p\x1b[?2004l\r\n");   // out of whatever it left
    _commandStarted = NO;
    [self promptReturned];       // no program title, no mouse reporting left on
    // Restart right away, unless the shell is dying at startup; then wait for
    // Enter so a broken startup file can't spin in a loop.
    if (-[_shellStarted timeIntervalSinceNow] > 2.0) {
        [self note:@"[shell exited — restarting]" color:0x6A9955];
        [self startShell];
    } else {
        [self note:@"[shell exited — press Enter to start a new one]" color:0xF48771];
    }
}

- (NSString *)quote:(NSString *)s {   // single-quote for the shell
    return [NSString stringWithFormat:@"'%@'",
            [s stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

// Never blocks: the bytes are queued and written on the I/O queue as the
// pty takes them, in order (see MCPty).
- (void)sendBytes:(const char *)bytes length:(size_t)length {
    if (!_pty || _master < 0 || length == 0) return;
    [_pty send:[NSData dataWithBytes:bytes length:length]];
}

- (void)sendText:(NSString *)text {
    const char *utf8 = text.UTF8String;
    if (utf8) [self sendBytes:utf8 length:strlen(utf8)];
}

- (void)sendControl:(char)byte {
    [self sendBytes:&byte length:1];
}

// ---------------------------------------------------------------- output
// Bytes from the reader join the backlog, which is parsed in slices.
- (void)enqueue:(NSData *)data pty:(MCPty *)pty generation:(int)generation {
    if (generation != _generation || !pty || pty != _pty || !_backlog) return;
    [_backlog appendData:data];
    if (!_drainScheduled) {
        _drainScheduled = YES;
        // In the common modes, so output keeps coming while a divider is
        // dragged or a menu is open.
        [self performSelector:@selector(drainBacklog) withObject:nil afterDelay:0
                      inModes:@[NSRunLoopCommonModes]];
    }
}

// Parses the backlog for about kSliceSeconds, then lets the run loop draw and
// handle events before the next slice. A whole slice is drawn once.
- (void)drainBacklog {
    _drainScheduled = NO;
    NSUInteger total = _backlog.length;
    if (total <= _backlogStart) return;
    CFTimeInterval start = CACurrentMediaTime();
    NSUInteger from = _backlogStart;
    const char *bytes = (const char *)_backlog.bytes;
    MCPty *pty = _pty;
    int generation = _generation;
    while (_backlogStart < total) {
        NSUInteger n = MIN((NSUInteger)kSliceBytes, total - _backlogStart);
        [self receive:[NSData dataWithBytesNoCopy:(void *)(bytes + _backlogStart) length:n
                                     freeWhenDone:NO]
           generation:generation];
        if (_backlog == nil || generation != _generation) return;   // the shell went
        _backlogStart += n;
        // A turn ends between frames where it can: in the middle of a
        // synchronized frame (mode 2026) parsing goes on a while longer,
        // since the view will not show the screen until the frame ends.
        CFTimeInterval spent = CACurrentMediaTime() - start;
        if (spent > kSliceSeconds &&
            (!_screen.synchronizedOutput() || spent > 3 * kSliceSeconds)) break;
    }
    if (pty) {
        long left = pty->queued.fetch_sub((long)(_backlogStart - from)) -
                    (long)(_backlogStart - from);
        if (left < kReadLow) [pty resumeReading];
    }
    if (_backlogStart >= _backlog.length) {
        _backlog.length = 0;
        _backlogStart = 0;
    } else {
        if (_backlogStart > (256u << 10)) {   // drop what is parsed, now and then
            [_backlog replaceBytesInRange:NSMakeRange(0, _backlogStart) withBytes:NULL length:0];
            _backlogStart = 0;
        }
        if (!_drainScheduled) {
            _drainScheduled = YES;
            [self performSelector:@selector(drainBacklog) withObject:nil afterDelay:0
                          inModes:@[NSRunLoopCommonModes]];
        }
    }
}

- (void)receive:(NSData *)data generation:(int)generation {
    if (generation != _generation) return;
    _screen.feed((const char *)data.bytes, data.length);
    std::string replies = _screen.takeReplies();   // e.g. cursor position
    if (!replies.empty()) [self sendBytes:replies.data() length:replies.size()];
    auto events = _stream.feed((const char *)data.bytes, data.length);
    if (!_lineMode) {
        [self receiveGridEvents:events];
        return;
    }
    [self.output clearLinkHover];   // the live line is about to be replaced
    NSTextStorage *ts = self.output.textStorage;
    [ts beginEditing];
    for (const TermEvent &e : events) {
        switch (e.kind) {
        case TermEvent::Line:
            [self replaceLiveLine:e.runs ended:e.ended];
            break;
        case TermEvent::PromptStart:
            _atPrompt = YES;
            _seenPrompt = YES;
            _commandStarted = NO;
            _promptTime = CACurrentMediaTime();
            _stickyGrid = NO;
            [self stopRawWatch];
            [self integrationReturned];
            [self promptReturned];
            if (_runningCommand) { _runningCommand = nil; [self titleChanged]; }
            break;
        case TermEvent::CommandStart:
            _commandStarted = YES;
            [self startRawWatch];
            [self watchShell];
            break;
        case TermEvent::CommandEnd:
            // After this chunk, once the view is up to date.
            [self performSelector:@selector(postCommandFinished) withObject:nil afterDelay:0];
            if (_userCommand) {
                _userCommand = NO;
                [self ensureNewline];
                if (e.status != 0)
                    [self appendLine:[NSString stringWithFormat:@"exit %d",
                                      e.status] color:THex(0xF48771)];
            }
            break;
        case TermEvent::Directory:
            _cwd = [[NSFileManager defaultManager]
                stringWithFileSystemRepresentation:e.text.c_str()
                                            length:e.text.size()];
            [_linkCache removeAllObjects];
            [self updatePrompt];
            [self titleChanged];
            break;
        case TermEvent::Title:
            [self programTitled:e.text];
            break;
        case TermEvent::InputStart:   // grid mode's integration only
            break;
        }
    }
    [self trimLog];
    [ts endEditing];
    [self updateMode];
    if (_gridMode) [self.grid screenChanged];
    else [self scrollToEnd];
    [self updateSecureInput];
    if (_pendingCommand) [self flushPendingInput];
}

// Grid mode: the bytes are already on the screen; the stream's events say
// where the shell is. Its lines are not used.
- (void)receiveGridEvents:(const std::vector<TermEvent> &)events {
    for (const TermEvent &e : events) {
        switch (e.kind) {
        case TermEvent::Line:
            break;
        case TermEvent::PromptStart:
            _atPrompt = YES;
            _seenPrompt = YES;
            _commandStarted = NO;
            _zleReady = NO;
            _promptTime = CACurrentMediaTime();
            [self integrationReturned];
            [self promptReturned];
            if (_runningCommand) { _runningCommand = nil; [self titleChanged]; }
            break;
        case TermEvent::InputStart:
            // Only at a prompt: B while a command runs (a stray one) says
            // nothing about the shell.
            if (_commandStarted) break;
            _atPrompt = YES;
            _seenPrompt = YES;
            _zleReady = YES;
            break;
        case TermEvent::CommandStart: {
            _atPrompt = NO;
            _zleReady = NO;
            _commandStarted = YES;
            NSString *typed = [[NSString alloc] initWithBytes:e.text.data()
                                                       length:e.text.size()
                                                     encoding:NSUTF8StringEncoding];
            typed = [typed stringByTrimmingCharactersInSet:
                     [NSCharacterSet whitespaceAndNewlineCharacterSet]];
            _runningCommand = typed.length ? typed : @"";
            [self titleChanged];
            [self watchShell];
            break;
        }
        case TermEvent::CommandEnd:
            [self performSelector:@selector(postCommandFinished) withObject:nil afterDelay:0];
            break;
        case TermEvent::Directory:
            _cwd = [[NSFileManager defaultManager]
                stringWithFileSystemRepresentation:e.text.c_str()
                                            length:e.text.size()];
            [_linkCache removeAllObjects];
            [self updatePrompt];
            [self titleChanged];
            break;
        case TermEvent::Title:
            [self programTitled:e.text];
            break;
        }
    }
    [self.grid screenChanged];
    // What waited for the prompt goes in once ZLE reads input, unless a
    // program has the screen or the terminal.
    if (_pendingCommand || _pendingTyped) [self flushPendingInput];
}

// A line from the panel itself: in the log (line mode), or written on the
// screen in its color (grid mode).
- (void)note:(NSString *)line color:(unsigned int)rgb {
    if (_lineMode) { [self appendLine:line color:THex(rgb)]; return; }
    NSString *s = [NSString stringWithFormat:@"\x1b[38;2;%u;%u;%um%@\x1b[m\r\n",
                   (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, line];
    const char *utf8 = s.UTF8String;
    if (utf8) _screen.feed(utf8);
    [self.grid screenChanged];
}

// ---------------------------------------------------------------- styling
- (NSDictionary *)attributesForStyle:(const TermStyle &)st {
    const Settings &cfg = [AppSettings shared].settings;
    bool defaultFg = st.fg.kind == TermColor::Default;
    Rgba fg = defaultFg ? cfg.text(Surface::Terminal) : Rgba::hex(st.fg.rgb());
    bool hasBg = st.bg.kind != TermColor::Default;
    // Inverse text gets a solid background even on a see-through panel.
    Rgba bg = Rgba::hex(hasBg ? st.bg.rgb()
                              : cfg.background(Surface::Terminal).rgb());
    if (st.inverse) { std::swap(fg, bg); hasBg = true; defaultFg = false; }

    NSMutableDictionary *a = [NSMutableDictionary dictionary];
    a[NSFontAttributeName] = MCTerminalFont(st.bold, st.italic);
    NSColor *fgColor = MCColor(fg);
    if (st.dim) fgColor = [fgColor colorWithAlphaComponent:fg.a * 0.6];
    a[NSForegroundColorAttributeName] = fgColor;
    if (defaultFg) a[kDefaultFgAttribute] = @(st.dim);
    if (hasBg) a[NSBackgroundColorAttributeName] = MCColor(bg);
    if (st.underline) a[NSUnderlineStyleAttributeName] = @(NSUnderlineStyleSingle);
    if (st.strike) a[NSStrikethroughStyleAttributeName] = @(NSUnderlineStyleSingle);
    return a;
}

// ---------------------------------------------------------------- output
// Replace the live line (everything from _liveStart on) with `runs`. If the
// line ended, add its newline and start the next live line after it.
- (void)replaceLiveLine:(const std::vector<TermRun> &)runs ended:(bool)ended {
    NSMutableAttributedString *line = [[NSMutableAttributedString alloc] init];
    for (const TermRun &r : runs) {
        NSString *s = [[NSString alloc] initWithBytes:r.text.data()
                                               length:r.text.size()
                                             encoding:NSUTF8StringEncoding];
        if (!s.length) continue;
        [line appendAttributedString:[[NSAttributedString alloc]
            initWithString:s attributes:[self attributesForStyle:r.style]]];
    }
    if (ended)
        [line appendAttributedString:[[NSAttributedString alloc]
            initWithString:@"\n" attributes:[self attributesForStyle:TermStyle()]]];

    NSTextStorage *ts = self.output.textStorage;
    if (_liveStart > ts.length) _liveStart = ts.length;
    [ts replaceCharactersInRange:NSMakeRange(_liveStart, ts.length - _liveStart)
            withAttributedString:line];
    if (ended) { _liveStart = ts.length; _logLines++; }
}

// The log keeps terminal.scrollback lines (at least 1000), so a long-running
// program's output, or the redraw frames of a progress bar, cannot grow it
// without end. Trimmed in steps of a tenth, so not on every line.
- (void)trimLog {
    NSUInteger limit = (NSUInteger)MAX(1000, [AppSettings shared].settings.terminalScrollback());
    if (_logLines <= limit + limit / 10) return;
    NSTextStorage *ts = self.output.textStorage;
    NSString *text = ts.string;
    NSUInteger drop = _logLines - limit, cut = 0, len = text.length;
    while (drop > 0 && cut < len) {
        NSRange nl = [text rangeOfString:@"\n" options:NSLiteralSearch
                                   range:NSMakeRange(cut, len - cut)];
        if (nl.location == NSNotFound) break;
        cut = NSMaxRange(nl);
        drop--;
        _logLines--;
    }
    if (cut > _liveStart) cut = _liveStart;   // never into the live line
    if (!cut) return;
    [self.output clearLinkHover];
    [ts deleteCharactersInRange:NSMakeRange(0, cut)];
    _liveStart -= cut;
}

// Panel text (headers, statuses) goes below a live line that has content, so
// end that line first and tell the stream it now starts afresh.
- (void)ensureNewline {
    NSTextStorage *ts = self.output.textStorage;
    if (_liveStart < ts.length) {
        [ts appendAttributedString:[[NSAttributedString alloc]
            initWithString:@"\n" attributes:[self attributesForStyle:TermStyle()]]];
        _stream.breakLine();
        _logLines++;
    }
    _liveStart = ts.length;
}

- (void)appendLine:(NSString *)line color:(NSColor *)color {
    [self ensureNewline];
    NSDictionary *attrs = @{
        NSFontAttributeName: MCTerminalFont(NO, NO),
        NSForegroundColorAttributeName: color,
    };
    NSTextStorage *ts = self.output.textStorage;
    [ts appendAttributedString:[[NSAttributedString alloc]
        initWithString:[line stringByAppendingString:@"\n"] attributes:attrs]];
    _liveStart = ts.length;
    _logLines += [line componentsSeparatedByString:@"\n"].count;
    [self trimLog];
    [self scrollToEnd];
}

- (void)scrollToEnd {
    [self.output scrollRangeToVisible:
        NSMakeRange(self.output.textStorage.length, 0)];
}

@end
