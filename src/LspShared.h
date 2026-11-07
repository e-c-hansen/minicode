// LspShared.h — the small helpers the Lsp files (Lsp.mm and its
// Lsp+<Concern>.mm categories, through Lsp+Private.h; CodeTextView.mm,
// LspServer.mm, LspPopups.mm) share. Only they import it.
#pragma once
#import "Lsp.h"
#import "EditorFont.h"
#import "AppSettings.h"
#import "Terminal.h"   // MCTerminalCommandDidFinishNotification
#import "MultiCaretView.h"
#import "FoldView.h"
#import "LspPicker.h"
#include "Formatter.h"
#include "InstallHints.h"
#include "LspClient.h"
#include "MarkdownParser.h"
#include "PythonEnvs.h"
#include "SymbolOutline.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <memory>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

// ------------------------------------------------------------------ helpers
static inline NSColor *LHex(unsigned int rgb) {
    return [NSColor colorWithSRGBRed:((rgb >> 16) & 0xFF) / 255.0
                               green:((rgb >> 8) & 0xFF) / 255.0
                                blue:(rgb & 0xFF) / 255.0
                               alpha:1.0];
}

static inline NSString *NS(const std::string &s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding] ?: @"";
}

static inline std::string Std(NSString *s) {
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

static inline NSColor *SeverityColor(NSInteger severity) {
    switch (severity) {
    case 1: return LHex(0xF14C4C);   // error
    case 2: return LHex(0xCCA700);   // warning
    case 3: return LHex(0x3794FF);   // information
    default: return LHex(0x9CA3AF);  // hint
    }
}
