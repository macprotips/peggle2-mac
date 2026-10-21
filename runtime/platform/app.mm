#include "app.h"
#include "../kernel/xam.h"
#include "../log.h"
#include "../settings.h"
#include "folders.h"
#include "settings_window.h"
#include "input.h"
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#include <algorithm>
#import <Metal/Metal.h>

static CAMetalLayer* g_layer;
static __weak NSView* g_gameView;

@interface GameView : NSView
@end

@implementation GameView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer
{
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.device = MTLCreateSystemDefaultDevice();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.drawableSize = CGSizeMake(1280, 720);
    // The console output Rec.709/sRGB; untagged, a P3 display would show every colour oversaturated.
    layer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    // Letterbox the 16:9 frame (full screen on a 16:10 display would otherwise stretch it).
    layer.contentsGravity = kCAGravityResizeAspect;
    layer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    g_layer = layer;
    g_gameView = self;
    return layer;
}
- (BOOL)acceptsFirstResponder { return YES; }

// The guest frame is the largest 16:9 rectangle centred in the view (letterboxed in full screen); view
// points map linearly from it to 1280x720.
- (void)reportMouse:(NSEvent*)event
{
    NSPoint p = [self convertPoint:event.locationInWindow fromView:nil];
    NSRect b = self.bounds;
    const CGFloat scale = std::min(b.size.width / 1280.0, b.size.height / 720.0);
    if (scale <= 0)
        return;
    const CGFloat x = (p.x - (b.size.width - 1280.0 * scale) / 2) / scale;
    const CGFloat y = (p.y - (b.size.height - 720.0 * scale) / 2) / scale;
    rt::MouseMoved(float(std::clamp(x, 0.0, 1280.0)), float(std::clamp(720.0 - y, 0.0, 720.0)));
}
- (void)updateTrackingAreas
{
    [super updateTrackingAreas];
    for (NSTrackingArea* area in self.trackingAreas)
        [self removeTrackingArea:area];
    [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:self.bounds
                                                       options:NSTrackingMouseMoved | NSTrackingActiveInKeyWindow | NSTrackingInVisibleRect
                                                         owner:self userInfo:nil]];
}
- (void)mouseMoved:(NSEvent*)event
{
    [self reportMouse:event];
    [self showCursorBriefly];
}
// In full screen the pointer hides after two idle seconds, so it does not sit over the board.
- (void)showCursorBriefly
{
    [NSCursor setHiddenUntilMouseMoves:NO];
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(hideIdleCursor) object:nil];
    [self performSelector:@selector(hideIdleCursor) withObject:nil afterDelay:2.0];
}
- (void)hideIdleCursor
{
    if (self.window.styleMask & NSWindowStyleMaskFullScreen)
        [NSCursor setHiddenUntilMouseMoves:YES];
}
- (void)mouseDragged:(NSEvent*)event { [self reportMouse:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self reportMouse:event]; }
- (void)mouseDown:(NSEvent*)event { [self reportMouse:event]; rt::MouseButton(0, true); }
- (void)mouseUp:(NSEvent*)event { rt::MouseButton(0, false); }
- (void)rightMouseDown:(NSEvent*)event { rt::MouseButton(1, true); }
- (void)rightMouseUp:(NSEvent*)event { rt::MouseButton(1, false); }
- (void)otherMouseDown:(NSEvent*)event { rt::MouseButton(int(event.buttonNumber == 2 ? 2 : -1), true); }
- (void)otherMouseUp:(NSEvent*)event { rt::MouseButton(int(event.buttonNumber == 2 ? 2 : -1), false); }
- (void)scrollWheel:(NSEvent*)event
{
    if (event.phase == NSEventPhaseChanged || event.momentumPhase != NSEventPhaseNone)
        return;
    // A sideways trackpad swipe steps like the wheel (swiping left goes right, to the next item).
    const CGFloat delta = std::abs(event.scrollingDeltaX) > std::abs(event.scrollingDeltaY) ? event.scrollingDeltaX : event.scrollingDeltaY;
    if (delta != 0)
        rt::MouseScrolled(float(delta));
}
// Keys go to GameController's keyboard; swallow them so AppKit does not beep.
- (void)keyDown:(NSEvent*)event {}
@end

// The dashed drop target in the import screen; tinted while a file is dragged over the window.
@interface DropZone : NSView
@property(nonatomic) BOOL highlighted;
@end

@implementation DropZone
- (void)setHighlighted:(BOOL)highlighted
{
    _highlighted = highlighted;
    self.needsDisplay = YES;
}
- (void)drawRect:(NSRect)dirty
{
    NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 1, 1) xRadius:16 yRadius:16];
    NSColor* tint = self.highlighted ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor;
    [[tint colorWithAlphaComponent:self.highlighted ? 0.12 : 0.04] setFill];
    [shape fill];
    const CGFloat dash[] = { 8, 6 };
    [shape setLineDash:dash count:2 phase:0];
    shape.lineWidth = 2;
    [tint setStroke];
    [shape stroke];
}
@end

// Shown until the game is installed: the user drops (or chooses) their own Peggle 2 package.
@interface ImportView : NSView
@property(copy) void (^onPath)(NSString* path);
@property(strong) DropZone* dropZone;
@property(strong) NSTextField* message;
@property(strong) NSProgressIndicator* progress;
@property(strong) NSButton* chooseButton;
- (void)setStatus:(NSString*)status error:(BOOL)error;
@end

@implementation ImportView
- (instancetype)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];

    NSTextField* title = [NSTextField labelWithString:@"Peggle 2"];
    title.font = [NSFont systemFontOfSize:44 weight:NSFontWeightBold];
    NSTextField* subtitle = [NSTextField labelWithString:@"Add your game to get started"];
    subtitle.font = [NSFont systemFontOfSize:17];
    subtitle.textColor = NSColor.secondaryLabelColor;
    NSStackView* header = [NSStackView stackViewWithViews:@[ title, subtitle ]];
    header.orientation = NSUserInterfaceLayoutOrientationVertical;
    header.spacing = 6;

    NSImageSymbolConfiguration* symbolSize = [NSImageSymbolConfiguration configurationWithPointSize:44 weight:NSFontWeightLight];
    NSImageView* icon = [NSImageView imageViewWithImage:[[NSImage imageWithSystemSymbolName:@"square.and.arrow.down"
                                                                    accessibilityDescription:nil] imageWithSymbolConfiguration:symbolSize]];
    icon.contentTintColor = NSColor.secondaryLabelColor;
    NSTextField* dropLabel = [NSTextField labelWithString:@"Drop your Peggle 2 package here"];
    dropLabel.font = [NSFont systemFontOfSize:17 weight:NSFontWeightMedium];
    NSTextField* orLabel = [NSTextField labelWithString:@"or"];
    orLabel.font = [NSFont systemFontOfSize:13];
    orLabel.textColor = NSColor.tertiaryLabelColor;
    self.chooseButton = [NSButton buttonWithTitle:@"Choose File…" target:self action:@selector(choose:)];
    self.chooseButton.controlSize = NSControlSizeLarge;
    self.chooseButton.keyEquivalent = @"\r";
    self.message = [NSTextField wrappingLabelWithString:@""];
    self.message.alignment = NSTextAlignmentCenter;
    self.message.font = [NSFont systemFontOfSize:13];
    self.message.hidden = YES;
    self.progress = [NSProgressIndicator new];
    self.progress.indeterminate = NO;
    self.progress.minValue = 0;
    self.progress.maxValue = 1;
    self.progress.hidden = YES;

    NSStackView* zoneContent = [NSStackView stackViewWithViews:@[ icon, dropLabel, orLabel, self.chooseButton, self.progress, self.message ]];
    zoneContent.orientation = NSUserInterfaceLayoutOrientationVertical;
    zoneContent.spacing = 10;
    [zoneContent setCustomSpacing:16 afterView:icon];
    [zoneContent setCustomSpacing:14 afterView:self.chooseButton];
    zoneContent.translatesAutoresizingMaskIntoConstraints = NO;
    self.dropZone = [DropZone new];
    [self.dropZone addSubview:zoneContent];

    NSTextField* footnote = [NSTextField wrappingLabelWithString:
        @"Use the package from your own Xbox 360, found in 58411436/000D0000.\nIt is installed once and stays on this Mac."];
    footnote.alignment = NSTextAlignmentCenter;
    footnote.font = [NSFont systemFontOfSize:12];
    footnote.textColor = NSColor.secondaryLabelColor;

    NSStackView* stack = [NSStackView stackViewWithViews:@[ header, self.dropZone, footnote ]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.spacing = 28;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [self.dropZone.widthAnchor constraintEqualToConstant:520],
        [self.dropZone.heightAnchor constraintEqualToConstant:260],
        [zoneContent.centerXAnchor constraintEqualToAnchor:self.dropZone.centerXAnchor],
        [zoneContent.centerYAnchor constraintEqualToAnchor:self.dropZone.centerYAnchor],
        [zoneContent.widthAnchor constraintEqualToConstant:440],
        [self.progress.widthAnchor constraintEqualToConstant:320],
        [footnote.widthAnchor constraintEqualToConstant:460],
    ]];
    return self;
}
- (void)drawRect:(NSRect)dirty
{
    NSGradient* backdrop = [[NSGradient alloc] initWithStartingColor:[NSColor colorWithWhite:0.16 alpha:1]
                                                         endingColor:[NSColor colorWithWhite:0.09 alpha:1]];
    [backdrop drawInRect:self.bounds angle:270];
}
- (void)choose:(id)sender
{
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;
    panel.message = @"Choose your Peggle 2 package or the folder that contains it.";
    if ([panel runModal] == NSModalResponseOK && panel.URL)
        self.onPath(panel.URL.path);
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
    if (!self.chooseButton.enabled)
        return NSDragOperationNone;
    self.dropZone.highlighted = YES;
    return NSDragOperationCopy;
}
- (void)draggingExited:(id<NSDraggingInfo>)sender { self.dropZone.highlighted = NO; }
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
    self.dropZone.highlighted = NO;
    NSURL* url = [NSURL URLFromPasteboard:sender.draggingPasteboard];
    if (!url.isFileURL)
        return NO;
    self.onPath(url.path);
    return YES;
}
- (void)setBusy:(BOOL)busy
{
    self.chooseButton.enabled = !busy;
    self.progress.hidden = !busy;
    self.progress.doubleValue = 0;
}
- (void)setStatus:(NSString*)status error:(BOOL)error
{
    self.message.stringValue = status;
    self.message.textColor = error ? NSColor.systemRedColor : NSColor.secondaryLabelColor;
    self.message.hidden = status.length == 0;
}
@end

// A macOS-style icon from the installed game's artwork. The banner is a light logo panel with white margins:
// crop to the panel, fill a rounded square with the panel's corner colour, and fade the panel's edges into
// it. Falls back to the square thumbnail. Used for the Dock at run time and for AppIcon.icns at build time.
static NSImage* ComposeIcon(NSString* bannerPath, NSString* thumbnailPath)
{
    NSImage* thumbnail = [[NSImage alloc] initWithContentsOfFile:thumbnailPath];
    NSData* bannerData = [NSData dataWithContentsOfFile:bannerPath];
    NSBitmapImageRep* banner = bannerData ? [NSBitmapImageRep imageRepWithData:bannerData] : nil;
    if (!banner && !thumbnail)
        return nil;

    NSRect panel = NSZeroRect;
    NSColor* background = [NSColor colorWithSRGBRed:0.47 green:0.78 blue:0.98 alpha:1];
    if (banner)
    {
        NSInteger minX = banner.pixelsWide, maxX = -1;
        for (NSInteger y = 0; y < banner.pixelsHigh; y += 2)
        {
            for (NSInteger x = 0; x < banner.pixelsWide; x++)
            {
                NSColor* c = [[banner colorAtX:x y:y] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
                if (c.redComponent < 0.96 || c.greenComponent < 0.96 || c.blueComponent < 0.96)
                {
                    minX = MIN(minX, x);
                    maxX = MAX(maxX, x);
                }
            }
        }
        if (maxX > minX)
        {
            panel = NSMakeRect(minX, 0, maxX - minX + 1, banner.pixelsHigh);
            background = [[banner colorAtX:minX + 2 y:1] colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
        }
    }

    const CGFloat size = 1024;
    return [NSImage imageWithSize:NSMakeSize(size, size) flipped:NO drawingHandler:^BOOL(NSRect) {
        // Apple's icon grid: an 824-point rounded square inside the 1024 canvas.
        NSRect tile = NSMakeRect(100, 100, 824, 824);
        NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:tile xRadius:185 yRadius:185];
        [background setFill];
        [shape fill];
        [NSGraphicsContext currentContext].imageInterpolation = NSImageInterpolationHigh;
        if (!NSIsEmptyRect(panel))
        {
            CGImageRef full = banner.CGImage;
            // CGImage rows run top-down; the panel spans the full height, so the crop is the same.
            CGImageRef crop = CGImageCreateWithImageInRect(full, CGRectMake(panel.origin.x, 0, panel.size.width, panel.size.height));
            CGFloat width = 800, height = width * panel.size.height / panel.size.width;
            NSRect target = NSMakeRect(512 - width / 2, 512 - height / 2, width, height);
            CGContextRef context = NSGraphicsContext.currentContext.CGContext;
            CGContextSaveGState(context);
            [shape addClip];
            CGContextDrawImage(context, target, crop);
            CGImageRelease(crop);
            // Fade the panel's edges into the background: left, right, bottom, top.
            NSGradient* fade = [[NSGradient alloc] initWithStartingColor:background endingColor:[background colorWithAlphaComponent:0]];
            const CGFloat feather = 60;
            [fade drawInRect:NSMakeRect(NSMinX(target) - 1, NSMinY(target), feather, height) angle:0];
            [fade drawInRect:NSMakeRect(NSMaxX(target) - feather + 1, NSMinY(target), feather, height) angle:180];
            [fade drawInRect:NSMakeRect(NSMinX(target), NSMinY(target) - 1, width, feather) angle:90];
            [fade drawInRect:NSMakeRect(NSMinX(target), NSMaxY(target) - feather + 1, width, feather) angle:270];
            CGContextRestoreGState(context);
        }
        else
            [thumbnail drawInRect:NSInsetRect(tile, 120, 120)];
        return YES;
    }];
}

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(strong) NSWindow* window;
@property(assign) rt::AppCallbacks* callbacks;
@property(assign) BOOL gameStarted;
@property(strong) id<NSObject> activity;
@property(assign) BOOL displayAwake;
@property(assign) IOPMAssertionID displayAssertion;
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification
{
    rt::MigrateSettings();
    [self buildMenu];
    rt::LoadSettings();
    NSRect frame = NSMakeRect(0, 0, 1280, 720);
    self.window = [[NSWindow alloc] initWithContentRect:frame
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Peggle 2";
    self.window.delegate = self;
    self.window.contentAspectRatio = NSMakeSize(16, 9);
    self.window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
    [self.window center];
    // Restores the last size and position; the first launch stays centred.
    [self.window setFrameAutosaveName:@"Peggle2Window"];
    // PEGGLE2_BACKGROUND (test runs) opens behind other windows without taking focus.
    if (getenv("PEGGLE2_BACKGROUND"))
        [self.window orderBack:nil];
    else
    {
        [self.window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    rt::StartInput();

    if (self.callbacks->gameInstalled())
        [self startGame];
    else
        [self showImport];
}
- (void)buildMenu
{
    NSMenu* bar = [NSMenu new];
    NSMenuItem* appItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* appMenu = [NSMenu new];
    [appMenu addItemWithTitle:@"About Peggle 2" action:@selector(orderFrontStandardAboutPanel:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Settings…" action:@selector(showSettings:) keyEquivalent:@","];
    [appMenu addItemWithTitle:@"Achievements…" action:@selector(showAchievements:) keyEquivalent:@"a"].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagShift;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Hide Peggle 2" action:@selector(hide:) keyEquivalent:@"h"];
    [appMenu addItemWithTitle:@"Hide Others" action:@selector(hideOtherApplications:) keyEquivalent:@"h"].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagOption;
    [appMenu addItemWithTitle:@"Show All" action:@selector(unhideAllApplications:) keyEquivalent:@""];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit Peggle 2" action:@selector(terminate:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenuItem* windowItem = [bar addItemWithTitle:@"" action:nil keyEquivalent:@""];
    NSMenu* windowMenu = [[NSMenu alloc] initWithTitle:@"Window"];
    [windowMenu addItemWithTitle:@"Minimize" action:@selector(performMiniaturize:) keyEquivalent:@"m"];
    [windowMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
    [windowMenu addItem:[NSMenuItem separatorItem]];
    [windowMenu addItemWithTitle:@"Toggle Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask =
        NSEventModifierFlagCommand | NSEventModifierFlagControl;
    windowItem.submenu = windowMenu;
    NSApp.windowsMenu = windowMenu;

    NSApp.mainMenu = bar;
}

// Controllers do not count as user activity, so the display would sleep mid-game; hold it awake while
// the game runs in the foreground. Rumble stops when the app goes to the background.
- (void)applicationDidBecomeActive:(NSNotification*)notification
{
    [self holdDisplayAwake];
}
- (void)holdDisplayAwake
{
    if (!self.gameStarted || self.displayAwake || !NSApp.isActive)
        return;
    IOPMAssertionID assertion;
    if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep, kIOPMAssertionLevelOn, CFSTR("Playing Peggle 2"),
                                    &assertion) == kIOReturnSuccess)
    {
        self.displayAssertion = assertion;
        self.displayAwake = YES;
    }
}
- (void)applicationWillResignActive:(NSNotification*)notification
{
    if (self.displayAwake)
        IOPMAssertionRelease(self.displayAssertion);
    self.displayAwake = NO;
    rt::StopRumble();
}
// Quitting waits (briefly) for saves the game has open, then exits without running C++ static
// destructors under the still-running game threads.
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender
{
    if (rt::SavesInProgress() == 0)
        return NSTerminateNow;
    rt::Log("Quit: waiting for {} save(s) to finish", rt::SavesInProgress().load());
    const NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:3];
    [NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer* timer) {
        if (rt::SavesInProgress() == 0 || [deadline timeIntervalSinceNow] <= 0)
        {
            [timer invalidate];
            [NSApp replyToApplicationShouldTerminate:YES];
        }
    }];
    return NSTerminateLater;
}
- (void)applicationWillTerminate:(NSNotification*)notification
{
    rt::Log("App: quitting");
    rt::StopRumble();
    fflush(stderr);
    _exit(0);
}
- (void)showImport
{
    ImportView* view = [[ImportView alloc] initWithFrame:self.window.contentView.bounds];
    __weak ImportView* weakView = view;
    __weak AppDelegate* weakSelf = self;
    view.onPath = ^(NSString* path) {
        [weakView setBusy:YES];
        [weakView setStatus:@"Installing…" error:NO];
        std::string dropped = path.UTF8String;
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            std::string error = weakSelf.callbacks->installGame(dropped, [weakView](double fraction) {
                dispatch_async(dispatch_get_main_queue(), ^{ weakView.progress.doubleValue = fraction; });
            });
            dispatch_async(dispatch_get_main_queue(), ^{
                if (error.empty())
                {
                    [weakSelf startGame];
                    return;
                }
                [weakView setBusy:NO];
                [weakView setStatus:@(error.c_str()) error:YES];
            });
        });
    };
    self.window.contentView = view;
}
- (void)startGame
{
    // The game keeps console timing (60 Hz vblank, audio, the GPU's interrupt handler) whether or not its
    // window is visible; App Nap would throttle those threads and slow the game to a crawl behind other
    // windows.
    self.activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical
                                                                 reason:@"Running Peggle 2"];
    self.gameStarted = YES;
    [self holdDisplayAwake];
    // Test runs: PEGGLE2_TEST_TOAST=<id> shows that achievement's toast once the game is up.
    if (const char* id = getenv("PEGGLE2_TEST_TOAST"))
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            for (const auto& a : rt::achievements::All())
                if (a.id == atoi(id))
                    rt::ShowAchievementUnlocked(a);
        });
    [self applyIcon];
    GameView* view = [[GameView alloc] initWithFrame:self.window.contentView.bounds];
    view.wantsLayer = YES;
    self.window.contentView = view;
    [self.window makeFirstResponder:view];
    self.callbacks->startGame();
}
- (void)applyIcon
{
    // The Dock icon only: the bundle is never modified at run time (that would break its signature);
    // the build puts the same icon in the bundle as AppIcon.icns.
    if (NSImage* icon = ComposeIcon(@(self.callbacks->bannerPath().c_str()), @(self.callbacks->iconPath().c_str())))
        NSApp.applicationIconImage = icon;
}

- (void)showAchievements:(id)sender { rt::ShowAchievements(); }
- (void)showSettings:(id)sender { rt::ShowSettings(); }
- (BOOL)validateMenuItem:(NSMenuItem*)item
{
    if (item.action == @selector(showAchievements:))
        return !rt::achievements::All().empty();
    return YES;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender { return YES; }
@end


// --- Achievements ------------------------------------------------------------------------------

static NSImage* AchievementImage(const rt::achievements::Achievement& a)
{
    if (a.image.empty())
        return [NSImage imageWithSystemSymbolName:@"trophy.fill" accessibilityDescription:nil];
    return [[NSImage alloc] initWithData:[NSData dataWithBytes:a.image.data() length:a.image.size()]];
}

static NSString* Text(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

// PEGGLE2_UI_SNAPSHOT=<dir>: writes each toast and the achievements window as PNG (for test runs, whose
// windows sit behind others).
static void SnapshotView(NSView* view, NSString* name)
{
    const char* dir = getenv("PEGGLE2_UI_SNAPSHOT");
    if (!dir)
        return;
    [view layoutSubtreeIfNeeded];
    NSBitmapImageRep* content = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    if (!content)
        return;
    [view cacheDisplayInRect:view.bounds toBitmapImageRep:content];
    // Over the window background, in the view's appearance (labels are white in dark mode).
    NSBitmapImageRep* bitmap = [view bitmapImageRepForCachingDisplayInRect:view.bounds];
    [NSGraphicsContext saveGraphicsState];
    NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
    [view.effectiveAppearance performAsCurrentDrawingAppearance:^{
        [NSColor.windowBackgroundColor setFill];
        NSRectFill(NSMakeRect(0, 0, bitmap.pixelsWide, bitmap.pixelsHigh));
    }];
    [content drawInRect:NSMakeRect(0, 0, bitmap.size.width, bitmap.size.height)];
    [NSGraphicsContext restoreGraphicsState];
    NSString* path = [@(dir) stringByAppendingPathComponent:name];
    [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
}

// The toast's rounded dark panel, drawn (not a layer property) so snapshots include it.
@interface ToastPanel : NSView
@end
@implementation ToastPanel
- (void)drawRect:(NSRect)dirty
{
    NSRect r = NSInsetRect(self.bounds, 0.5, 0.5);
    NSBezierPath* shape = [NSBezierPath bezierPathWithRoundedRect:r xRadius:r.size.height / 2 yRadius:r.size.height / 2];
    [[NSColor colorWithWhite:0.08 alpha:0.9] setFill];
    [shape fill];
    [[NSColor colorWithWhite:1 alpha:0.15] setStroke];
    [shape stroke];
}
@end

// The console's unlock toast: icon, "Achievement unlocked", gamerscore and name, at the bottom of the
// game frame for a few seconds. Toasts that arrive together are shown one after another.
@interface AchievementToasts : NSObject
@property(strong) NSMutableArray<NSView*>* queue;
@property(assign) BOOL showing;
@end

@implementation AchievementToasts
+ (instancetype)shared
{
    static AchievementToasts* toasts = [AchievementToasts new];
    return toasts;
}
- (NSView*)toastFor:(const rt::achievements::Achievement&)a
{
    NSView* toast = [[ToastPanel alloc] initWithFrame:NSMakeRect(0, 0, 380, 72)];

    NSImageView* icon = [NSImageView imageViewWithImage:AchievementImage(a)];
    icon.frame = NSMakeRect(12, 12, 48, 48);
    icon.wantsLayer = YES;
    icon.layer.cornerRadius = 24;
    icon.layer.masksToBounds = YES;
    [toast addSubview:icon];

    NSTextField* heading = [NSTextField labelWithString:@"Achievement unlocked"];
    heading.font = [NSFont systemFontOfSize:12 weight:NSFontWeightMedium];
    heading.textColor = [NSColor colorWithWhite:1 alpha:0.7];
    heading.frame = NSMakeRect(72, 38, 290, 18);
    [toast addSubview:heading];

    NSTextField* name = [NSTextField labelWithString:[NSString stringWithFormat:@"%uG – %@", a.gamerscore, Text(a.name)]];
    name.font = [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold];
    name.textColor = NSColor.whiteColor;
    name.lineBreakMode = NSLineBreakByTruncatingTail;
    name.frame = NSMakeRect(72, 16, 290, 22);
    [toast addSubview:name];
    // Fit the panel to its text so it reads as centred whatever the name's length.
    const CGFloat textWidth = std::ceil(std::max(heading.fittingSize.width, name.fittingSize.width));
    const CGFloat width = std::clamp(72 + textWidth + 24, CGFloat(260), CGFloat(560));
    [toast setFrameSize:NSMakeSize(width, 72)];
    heading.frame = NSMakeRect(72, 38, width - 96, 18);
    name.frame = NSMakeRect(72, 16, width - 96, 22);
    return toast;
}
- (void)show:(const rt::achievements::Achievement&)a
{
    if (!self.queue)
        self.queue = [NSMutableArray array];
    [self.queue addObject:[self toastFor:a]];
    [self next];
}
- (void)next
{
    NSView* host = g_gameView;
    if (self.showing || self.queue.count == 0 || !host)
        return;
    NSView* toast = self.queue.firstObject;
    [self.queue removeObjectAtIndex:0];
    self.showing = YES;

    // Bottom centre of the 16:9 game frame, which is letterboxed in the view.
    const NSRect b = host.bounds;
    const CGFloat frameHeight = std::min(b.size.height, b.size.width * 9 / 16);
    const CGFloat bottom = (b.size.height - frameHeight) / 2 + frameHeight * 0.06;
    [toast setFrameOrigin:NSMakePoint(std::round((b.size.width - toast.frame.size.width) / 2), std::round(bottom))];
    toast.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMaxYMargin;
    toast.alphaValue = 0;
    [host addSubview:toast];
    [[NSSound soundNamed:@"Glass"] play];
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
        context.duration = 0.3;
        toast.animator.alphaValue = 1;
    } completionHandler:^{
        SnapshotView(toast, [NSString stringWithFormat:@"toast_%llu.png", (unsigned long long)NSDate.date.timeIntervalSince1970]);
    }];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [NSAnimationContext runAnimationGroup:^(NSAnimationContext* context) {
            context.duration = 0.4;
            toast.animator.alphaValue = 0;
        } completionHandler:^{
            [toast removeFromSuperview];
            self.showing = NO;
            [self next];
        }];
    });
}
@end

// Scroll content that starts at the top.
@interface FlippedView : NSView
@end
@implementation FlippedView
- (BOOL)isFlipped { return YES; }
@end

// The Guide's achievements list, as a window: every achievement with its icon, how it is earned, its
// gamerscore and when it was unlocked. The title is told a system UI is showing while it is open.
@interface AchievementsWindow : NSObject <NSWindowDelegate>
@property(strong) NSPanel* panel;
@end

@implementation AchievementsWindow
+ (instancetype)shared
{
    static AchievementsWindow* window = [AchievementsWindow new];
    return window;
}
- (NSView*)rowFor:(const rt::achievements::Achievement&)a
{
    const int64_t unlocked = rt::achievements::UnlockTime(a.id);
    const bool hidden = !unlocked && a.Secret();

    NSImageView* icon = [NSImageView imageViewWithImage:hidden ? [NSImage imageWithSystemSymbolName:@"lock.fill" accessibilityDescription:@"Locked"]
                                                               : AchievementImage(a)];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    icon.alphaValue = unlocked ? 1.0 : 0.35;
    [icon.widthAnchor constraintEqualToConstant:56].active = YES;
    [icon.heightAnchor constraintEqualToConstant:56].active = YES;

    NSTextField* name = [NSTextField labelWithString:hidden ? @"Secret Achievement" : Text(a.name)];
    name.font = [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold];
    NSString* detail = unlocked ? Text(a.description) : hidden ? @"Continue playing to unlock this secret achievement." : Text(a.unachieved);
    NSTextField* description = [NSTextField wrappingLabelWithString:detail];
    description.font = [NSFont systemFontOfSize:12];
    description.textColor = NSColor.secondaryLabelColor;
    NSStackView* text = [NSStackView stackViewWithViews:@[ name, description ]];
    text.orientation = NSUserInterfaceLayoutOrientationVertical;
    text.alignment = NSLayoutAttributeLeading;
    text.spacing = 2;

    NSTextField* score = [NSTextField labelWithString:[NSString stringWithFormat:@"%uG", a.gamerscore]];
    score.font = [NSFont monospacedDigitSystemFontOfSize:14 weight:NSFontWeightSemibold];
    score.textColor = unlocked ? NSColor.labelColor : NSColor.tertiaryLabelColor;
    score.alignment = NSTextAlignmentRight;
    NSTextField* date = [NSTextField labelWithString:@""];
    if (unlocked)
        date.stringValue = [NSDateFormatter localizedStringFromDate:[NSDate dateWithTimeIntervalSince1970:unlocked]
                                                          dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterNoStyle];
    date.font = [NSFont systemFontOfSize:11];
    date.textColor = NSColor.secondaryLabelColor;
    date.alignment = NSTextAlignmentRight;
    NSStackView* right = [NSStackView stackViewWithViews:@[ score, date ]];
    right.orientation = NSUserInterfaceLayoutOrientationVertical;
    right.alignment = NSLayoutAttributeTrailing;
    right.spacing = 2;
    [right setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    [text setContentHuggingPriority:NSLayoutPriorityDefaultLow - 1 forOrientation:NSLayoutConstraintOrientationHorizontal];
    NSStackView* row = [NSStackView stackViewWithViews:@[ icon, text, right ]];
    row.distribution = NSStackViewDistributionFill;
    row.spacing = 12;
    row.alignment = NSLayoutAttributeCenterY;
    row.edgeInsets = NSEdgeInsetsMake(8, 16, 8, 16);
    return row;
}
- (void)reload
{
    if (!self.panel)
        return;
    const auto& all = rt::achievements::All();
    unsigned unlockedCount = 0, score = 0, total = 0;
    NSMutableArray<NSView*>* rows = [NSMutableArray array];
    for (const auto& a : all)
    {
        total += a.gamerscore;
        if (rt::achievements::UnlockTime(a.id))
            unlockedCount++, score += a.gamerscore;
        [rows addObject:[self rowFor:a]];
    }
    NSTextField* summary = [NSTextField labelWithString:[NSString stringWithFormat:@"%u of %zu unlocked · %u of %uG", unlockedCount, all.size(), score, total]];
    summary.font = [NSFont systemFontOfSize:13 weight:NSFontWeightMedium];
    summary.textColor = NSColor.secondaryLabelColor;

    NSStackView* list = [NSStackView stackViewWithViews:rows];
    list.orientation = NSUserInterfaceLayoutOrientationVertical;
    list.alignment = NSLayoutAttributeLeading;
    list.spacing = 0;
    list.translatesAutoresizingMaskIntoConstraints = NO;
    for (NSView* row in rows)
        [row.widthAnchor constraintEqualToAnchor:list.widthAnchor].active = YES;

    NSScrollView* scroll = [NSScrollView new];
    scroll.hasVerticalScroller = YES;
    scroll.drawsBackground = NO;
    NSView* document = [FlippedView new];
    document.translatesAutoresizingMaskIntoConstraints = NO;
    [document addSubview:list];
    scroll.documentView = document;
    [NSLayoutConstraint activateConstraints:@[
        [list.topAnchor constraintEqualToAnchor:document.topAnchor],
        [list.leadingAnchor constraintEqualToAnchor:document.leadingAnchor],
        [list.trailingAnchor constraintEqualToAnchor:document.trailingAnchor],
        [list.bottomAnchor constraintEqualToAnchor:document.bottomAnchor],
        [document.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
    ]];

    NSStackView* content = [NSStackView stackViewWithViews:@[ summary, scroll ]];
    content.orientation = NSUserInterfaceLayoutOrientationVertical;
    content.alignment = NSLayoutAttributeLeading;
    content.edgeInsets = NSEdgeInsetsMake(12, 0, 0, 0);
    [summary.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:16].active = YES;
    [scroll.widthAnchor constraintEqualToAnchor:content.widthAnchor].active = YES;
    self.panel.contentView = content;
}
- (void)show
{
    if (rt::achievements::All().empty())
        return;
    if (!self.panel)
    {
        self.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 520, 640)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
        self.panel.title = @"Achievements";
        self.panel.delegate = self;
        self.panel.hidesOnDeactivate = NO;
        self.panel.releasedWhenClosed = NO;
        self.panel.minSize = NSMakeSize(400, 300);
        [self.panel center];
    }
    [self reload];
    if (!self.panel.visible)
        rt::SystemUIShown(true);
    [self.panel makeKeyAndOrderFront:nil];
    if (getenv("PEGGLE2_UI_SNAPSHOT"))
    {
        // Test runs: capture once laid out, then close as a player would.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
            SnapshotView(self.panel.contentView, @"achievements.png");
            [self.panel performClose:nil];
        });
    }
}
- (void)windowWillClose:(NSNotification*)notification
{
    rt::SystemUIShown(false);
}
@end

namespace rt
{
    void ShowAchievementUnlocked(const achievements::Achievement& achievement)
    {
        const achievements::Achievement* a = &achievement; // lives as long as the table
        dispatch_async(dispatch_get_main_queue(), ^{
            [[AchievementToasts shared] show:*a];
            [[AchievementsWindow shared] reload];
        });
    }

    void ShowMessageBox(const std::string& title, const std::string& text, const std::vector<std::string>& buttons, int focused,
                        std::function<void(int)> done)
    {
        NSString* heading = Text(title), *body = Text(text);
        NSMutableArray<NSString*>* labels = [NSMutableArray array];
        for (const std::string& b : buttons)
            [labels addObject:Text(b)];
        if (labels.count == 0)
            [labels addObject:@"OK"];
        // The focused button becomes the default (Return); the others keep the title's order.
        const NSUInteger first = focused >= 0 && NSUInteger(focused) < labels.count ? NSUInteger(focused) : 0;
        dispatch_async(dispatch_get_main_queue(), ^{
            NSAlert* alert = [NSAlert new];
            alert.messageText = heading.length ? heading : @"Peggle 2";
            alert.informativeText = body;
            NSMutableArray<NSNumber*>* order = [NSMutableArray arrayWithObject:@(first)];
            for (NSUInteger i = 0; i < labels.count; i++)
                if (i != first)
                    [order addObject:@(i)];
            for (NSNumber* index in order)
                [alert addButtonWithTitle:labels[index.unsignedIntegerValue]];
            void (^finish)(NSModalResponse) = ^(NSModalResponse response) {
                const NSInteger position = response - NSAlertFirstButtonReturn;
                done(position >= 0 && position < NSInteger(order.count) ? order[position].intValue : int(first));
            };
            NSWindow* window = g_gameView.window;
            if (window)
                [alert beginSheetModalForWindow:window completionHandler:finish];
            else
                finish([alert runModal]);
        });
    }

    bool WriteAppIcon(const std::string& bannerPath, const std::string& thumbnailPath, const std::string& pngPath)
    {
        @autoreleasepool
        {
            NSImage* icon = ComposeIcon(@(bannerPath.c_str()), @(thumbnailPath.c_str()));
            if (!icon)
                return false;
            NSBitmapImageRep* bitmap = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr pixelsWide:1024 pixelsHigh:1024 bitsPerSample:8
                samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
            [NSGraphicsContext saveGraphicsState];
            NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:bitmap];
            [icon drawInRect:NSMakeRect(0, 0, 1024, 1024)];
            [NSGraphicsContext restoreGraphicsState];
            return [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:@(pngPath.c_str()) atomically:YES];
        }
    }

    void QuitApplication()
    {
        Log("App: the game asked to quit");
        dispatch_async(dispatch_get_main_queue(), ^{ [NSApp terminate:nil]; });
    }

    void ShowAchievements()
    {
        dispatch_async(dispatch_get_main_queue(), ^{ [[AchievementsWindow shared] show]; });
    }

    // A fatal error: tell the player (on the main thread; the failing thread waits up to a minute), then
    // the app ends with a crash report.
    static void ShowFatalError(const std::string& message)
    {
        NSString* text = Text(message);
        void (^show)() = ^{
            NSAlert* alert = [NSAlert new];
            alert.alertStyle = NSAlertStyleCritical;
            alert.messageText = @"Peggle 2 has stopped because of an error";
            alert.informativeText = [NSString stringWithFormat:@"%@\n\nDetails are in ~/Library/Logs/Peggle 2.", text];
            [alert runModal];
        };
        if (NSThread.isMainThread)
            show();
        else
        {
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            dispatch_async(dispatch_get_main_queue(), ^{ show(); dispatch_semaphore_signal(done); });
            dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC));
        }
    }

    void ShowStartupError(const std::string& title, const std::string& message)
    {
        @autoreleasepool
        {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp activateIgnoringOtherApps:YES];
            NSAlert* alert = [NSAlert new];
            alert.messageText = Text(title);
            alert.informativeText = Text(message);
            [alert runModal];
        }
    }

    void RunApplication(AppCallbacks callbacks)
    {
        @autoreleasepool
        {
            SetFatalHook(ShowFatalError);
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            static AppCallbacks stored = std::move(callbacks);
            AppDelegate* delegate = [AppDelegate new];
            delegate.callbacks = &stored;
            NSApp.delegate = delegate;
            [NSApp run];
        }
        exit(0);
    }

    void* MetalLayer()
    {
        return (__bridge void*)g_layer;
    }
}
