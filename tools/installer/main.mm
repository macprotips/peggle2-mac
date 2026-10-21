// Peggle 2 Installer: installs the player's copy of the game and builds Peggle 2.app from it on their Mac
// (Resources/sdk/build_game.sh), then opens it. Once built, opening the installer just opens the game, and a
// newer installer rebuilds it.
#import <AppKit/AppKit.h>
#include "game_import.h"
#include "platform/folders.h"
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace rt
{
    void LogLine(const std::string& line) // game_import's log; the installer has none
    {
        fprintf(stderr, "%s\n", line.c_str());
    }
}

static fs::path DataDir()
{
    return rt::UserDataDirectory();
}

// /Applications when it is writable (usually, for an admin account), otherwise ~/Applications.
static fs::path AppPath()
{
    if (access("/Applications", W_OK) == 0)
        return "/Applications/Peggle 2.app";
    const char* home = getenv("HOME");
    return fs::path(home ? home : ".") / "Applications/Peggle 2.app";
}

// Identifies this installer's contents (written by make_sdk.sh), so any change to the port rebuilds the game.
static NSString* Version()
{
    NSString* path = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"sdk/build-id"];
    NSString* id = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    return [id stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"unknown";
}

static bool BuiltAppIsCurrent()
{
    NSString* stamp = [NSString stringWithContentsOfFile:@((AppPath() / "Contents/Resources/installer-version").c_str())
                                                encoding:NSUTF8StringEncoding error:nil];
    return [[stamp stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] isEqualToString:Version()];
}

static bool HaveCompiler()
{
    NSTask* task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
    task.arguments = @[ @"--find", @"clang++" ];
    task.standardOutput = NSFileHandle.fileHandleWithNullDevice;
    task.standardError = NSFileHandle.fileHandleWithNullDevice;
    if (![task launchAndReturnError:nil])
        return false;
    [task waitUntilExit];
    return task.terminationStatus == 0;
}

// The drop target: a rounded well that also opens a file chooser when clicked.
@interface DropWell : NSView
@property(copy) void (^chosen)(NSURL*);
@property(nonatomic) BOOL enabled;
@property(nonatomic) BOOL highlighted;
@end

@implementation DropWell
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame]))
    {
        [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
        _enabled = YES;
    }
    return self;
}
- (void)setHighlighted:(BOOL)highlighted
{
    _highlighted = highlighted;
    self.needsDisplay = YES;
}
- (void)drawRect:(NSRect)rect
{
    NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 1, 1) xRadius:14 yRadius:14];
    [(self.highlighted ? [NSColor.controlAccentColor colorWithAlphaComponent:0.12] : [NSColor.labelColor colorWithAlphaComponent:0.04]) setFill];
    [shape fill];
    if (!self.enabled)
        return;
    shape.lineWidth = 1.5;
    CGFloat dash[] = { 6, 5 };
    [shape setLineDash:dash count:2 phase:0];
    [(self.highlighted ? NSColor.controlAccentColor : [NSColor.labelColor colorWithAlphaComponent:0.25]) setStroke];
    [shape stroke];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    if (!self.enabled)
        return NSDragOperationNone;
    self.highlighted = YES;
    return NSDragOperationCopy;
}
- (void)draggingExited:(id<NSDraggingInfo>)sender
{
    self.highlighted = NO;
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    self.highlighted = NO;
    NSURL* url = [NSURL URLFromPasteboard:sender.draggingPasteboard];
    if (!self.enabled || !url)
        return NO;
    self.chosen(url);
    return YES;
}
- (void)mouseUp:(NSEvent*)event
{
    if (!self.enabled)
        return;
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.message = @"Choose your Peggle 2 game file, or a folder that contains it.";
    panel.prompt = @"Choose";
    [panel beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response) {
        if (response == NSModalResponseOK && panel.URL)
            self.chosen(panel.URL);
    }];
}
- (void)resetCursorRects
{
    if (self.enabled)
        [self addCursorRect:self.bounds cursor:NSCursor.pointingHandCursor];
}
@end

static NSTextField* Label(NSString* text, CGFloat size, NSFontWeight weight, NSColor* color)
{
    NSTextField* label = [NSTextField wrappingLabelWithString:text];
    label.font = [NSFont systemFontOfSize:size weight:weight];
    label.textColor = color;
    label.alignment = NSTextAlignmentCenter;
    label.selectable = NO;
    return label;
}

@interface Installer : NSObject <NSApplicationDelegate>
@property(strong) NSWindow* window;
@property(strong) DropWell* well;
@property(strong) NSImageView* icon;
@property(strong) NSTextField* prompt;
@property(strong) NSTextField* detail;
@property(strong) NSProgressIndicator* progress;
@property(strong) NSButton* button;
@end

@implementation Installer
- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    if (rt::IsGameInstalled(DataDir() / "game") && BuiltAppIsCurrent())
    {
        [self openGame];
        return;
    }
    [self buildWindow];
    if (!HaveCompiler())
    {
        [self showIdle:@"Command Line Tools needed"
                detail:@"Peggle 2 is built on your Mac with Apple's free Command Line Tools. Install them, then open this installer again."
                symbol:@"hammer"];
        self.well.enabled = NO;
        self.button.title = @"Install Command Line Tools";
        self.button.hidden = NO;
        [self layoutButton];
        return;
    }
    if (rt::IsGameInstalled(DataDir() / "game"))
        [self build]; // the game is installed but the app is missing or from an older installer
}

- (void)buildWindow
{
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 540, 356)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskFullSizeContentView
                                                backing:NSBackingStoreBuffered defer:NO];
    self.window.titlebarAppearsTransparent = YES;
    self.window.titleVisibility = NSWindowTitleHidden;
    self.window.movableByWindowBackground = YES;
    NSView* content = self.window.contentView;

    NSTextField* name = Label(@"Peggle 2", 28, NSFontWeightBold, NSColor.labelColor);
    name.frame = NSMakeRect(40, 282, 460, 36);
    [content addSubview:name];
    NSTextField* tagline = Label(@"for Mac", 13, NSFontWeightRegular, NSColor.secondaryLabelColor);
    tagline.frame = NSMakeRect(40, 262, 460, 18);
    [content addSubview:tagline];

    self.well = [[DropWell alloc] initWithFrame:NSMakeRect(40, 36, 460, 206)];
    __weak Installer* weakSelf = self;
    self.well.chosen = ^(NSURL* url) { [weakSelf install:url]; };
    [content addSubview:self.well];

    self.icon = [NSImageView new];
    self.icon.frame = NSMakeRect(205, 132, 50, 50);
    self.icon.contentTintColor = NSColor.secondaryLabelColor;
    self.icon.symbolConfiguration = [NSImageSymbolConfiguration configurationWithPointSize:36 weight:NSFontWeightLight];
    [self.well addSubview:self.icon];

    self.prompt = Label(@"", 15, NSFontWeightSemibold, NSColor.labelColor);
    self.prompt.frame = NSMakeRect(20, 100, 420, 22);
    [self.well addSubview:self.prompt];
    self.detail = Label(@"", 12, NSFontWeightRegular, NSColor.secondaryLabelColor);
    self.detail.frame = NSMakeRect(30, 30, 400, 62);
    [self.well addSubview:self.detail];

    self.progress = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(80, 44, 300, 12)];
    self.progress.style = NSProgressIndicatorStyleBar;
    self.progress.indeterminate = NO;
    self.progress.controlSize = NSControlSizeSmall;
    self.progress.hidden = YES;
    [self.well addSubview:self.progress];

    self.button = [NSButton buttonWithTitle:@"" target:self action:@selector(buttonPressed:)];
    self.button.bezelStyle = NSBezelStyleRounded;
    self.button.keyEquivalent = @"\r";
    self.button.hidden = YES;
    [self.well addSubview:self.button]; // shown only for the Command Line Tools, in the well's lower part


    [self showIdle:@"Drop your Peggle 2 game file here" detail:@"or click to choose it"
            symbol:@"arrow.down.app"];
    [self.window center];
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

- (void)setSymbol:(NSString*)symbol
{
    self.icon.image = [NSImage imageWithSystemSymbolName:symbol accessibilityDescription:nil];
}

- (void)showIdle:(NSString*)prompt detail:(NSString*)detail symbol:(NSString*)symbol
{
    [self setSymbol:symbol];
    self.prompt.stringValue = prompt;
    self.detail.stringValue = detail;
    self.progress.hidden = YES;
    self.button.hidden = YES;
    [self layoutButton];
}

- (void)showWorking:(NSString*)prompt detail:(NSString*)detail
{
    [self setSymbol:@"gearshape.2"];
    self.prompt.stringValue = prompt;
    self.detail.stringValue = detail;
    self.progress.hidden = NO;
    self.well.enabled = NO;
    self.well.needsDisplay = YES;
    [self.window invalidateCursorRectsForView:self.well];
}

- (void)showError:(NSString*)prompt detail:(NSString*)detail retry:(BOOL)retry
{
    [self setSymbol:@"exclamationmark.triangle"];
    self.icon.contentTintColor = NSColor.systemOrangeColor;
    self.prompt.stringValue = prompt;
    self.detail.stringValue = detail;
    self.progress.hidden = YES;
    self.well.enabled = retry;
    self.well.needsDisplay = YES;
    [self.window invalidateCursorRectsForView:self.well];
}

- (void)layoutButton
{
    [self.button sizeToFit];
    NSRect frame = self.button.frame;
    frame.size.width = MAX(frame.size.width + 24, 140);
    frame.origin = NSMakePoint(std::round((460 - frame.size.width) / 2), 16);
    self.button.frame = frame;
}

- (void)buttonPressed:(id)sender
{
    if ([self.button.title hasPrefix:@"Install"])
    {
        [NSTask launchedTaskWithLaunchPath:@"/usr/bin/xcode-select" arguments:@[ @"--install" ]];
        [NSApp terminate:nil];
    }
}

- (void)install:(NSURL*)url
{
    self.icon.contentTintColor = NSColor.secondaryLabelColor;
    const fs::path package = rt::FindGamePackage(url.path.fileSystemRepresentation);
    if (package.empty())
    {
        [self showError:@"That isn't the Peggle 2 game file" detail:@"Drop the Peggle 2 file from your Xbox 360, or click to choose it." retry:YES];
        return;
    }
    self.progress.doubleValue = 0;
    [self showWorking:@"Copying the game…" detail:@""];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        std::string error = rt::InstallGame(package, DataDir() / "game", [self](double fraction) {
            dispatch_async(dispatch_get_main_queue(), ^{ self.progress.doubleValue = fraction * 100; });
        });
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!error.empty())
            {
                [self showError:@"The game could not be installed" detail:@(error.c_str()) retry:YES];
                return;
            }
            [self build];
        });
    });
}

- (void)build
{
    self.icon.contentTintColor = NSColor.secondaryLabelColor;
    self.progress.doubleValue = 0;
    if (fs::exists(AppPath()))
        [self showWorking:@"Updating Peggle 2…" detail:@"Your saves and progress are kept. This takes a few minutes."];
    else
        [self showWorking:@"Building Peggle 2…" detail:@"This takes a few minutes and only happens once."];

    NSString* sdk = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"sdk"];
    // Build files are temporary: in the installer's own Caches folder (earlier versions used Caches/Peggle2).
    NSURL* caches = [NSFileManager.defaultManager URLForDirectory:NSCachesDirectory inDomain:NSUserDomainMask appropriateForURL:nil create:YES error:nil];
    const fs::path cachesDir = caches ? fs::path(caches.fileSystemRepresentation) : fs::path(getenv("HOME") ?: ".") / "Library/Caches";
    std::error_code removeError;
    fs::remove_all(cachesDir / "Peggle2", removeError);
    const fs::path work = cachesDir / "io.github.macprotips.peggle2.installer/build";
    const fs::path log = DataDir() / "install.log";
    fs::create_directories(DataDir());

    NSTask* task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/bash"];
    task.arguments = @[ [sdk stringByAppendingPathComponent:@"build_game.sh"], sdk, @((DataDir() / "game").c_str()), @(work.c_str()),
                        @(AppPath().c_str()), [NSBundle.mainBundle.bundlePath stringByAppendingPathComponent:@"Contents"], Version() ];
    NSPipe* pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    [NSFileManager.defaultManager createFileAtPath:@(log.c_str()) contents:nil attributes:nil];
    task.standardError = [NSFileHandle fileHandleForWritingAtPath:@(log.c_str())];

    __block NSMutableString* pending = [NSMutableString new];
    __block double total = 1, done = 0;
    pipe.fileHandleForReading.readabilityHandler = ^(NSFileHandle* handle) {
        NSData* data = handle.availableData;
        if (!data.length)
            return;
        NSString* text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        dispatch_async(dispatch_get_main_queue(), ^{
            [pending appendString:text];
            NSArray* lines = [pending componentsSeparatedByString:@"\n"];
            [pending setString:lines.lastObject];
            for (NSUInteger i = 0; i + 1 < lines.count; i++)
            {
                NSString* line = lines[i];
                if ([line hasPrefix:@"STEP "])
                    self.prompt.stringValue = [[line substringFromIndex:5] stringByAppendingString:@"…"];
                else if ([line hasPrefix:@"TOTAL "])
                    total = MAX(1, [line substringFromIndex:6].doubleValue);
                else if ([line isEqualToString:@"DONE"])
                    self.progress.doubleValue = 5 + 90 * (++done / total);
            }
        });
    };
    task.terminationHandler = ^(NSTask* finished) {
        dispatch_async(dispatch_get_main_queue(), ^{
            pipe.fileHandleForReading.readabilityHandler = nil;
            if (finished.terminationStatus == 0)
            {
                self.progress.doubleValue = 100;
                [self openGame];
            }
            else
                [self showError:@"Peggle 2 could not be built"
                         detail:[NSString stringWithFormat:@"The details are in %s. Please include that file if you report the problem.", log.c_str()]
                          retry:NO];
        });
    };
    NSError* error;
    if (![task launchAndReturnError:&error])
        [self showError:@"Peggle 2 could not be built" detail:error.localizedDescription retry:NO];
}

- (void)openGame
{
    [NSWorkspace.sharedWorkspace openApplicationAtURL:[NSURL fileURLWithPath:@(AppPath().c_str())]
                                        configuration:[NSWorkspaceOpenConfiguration configuration]
                                    completionHandler:^(NSRunningApplication* app, NSError* error) {
                                        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
                                    }];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender
{
    return YES;
}
@end

int main()
{
    @autoreleasepool
    {
        NSApplication* app = [NSApplication sharedApplication];
        app.activationPolicy = NSApplicationActivationPolicyRegular;
        app.applicationIconImage = [NSImage imageNamed:@"InstallerIcon"]; // also when started outside Finder
        static Installer* delegate = [Installer new];
        app.delegate = delegate;
        NSMenu* bar = [NSMenu new];
        NSMenu* appMenu = [NSMenu new];
        [appMenu addItemWithTitle:@"Quit Peggle 2 Installer" action:@selector(terminate:) keyEquivalent:@"q"];
        [bar addItemWithTitle:@"" action:nil keyEquivalent:@""].submenu = appMenu;
        app.mainMenu = bar;
        [app run];
    }
    return 0;
}
