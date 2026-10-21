// Peggle 2 > Settings… (⌘,): the port's options, laid out as macOS settings windows are: right-aligned
// labels, left-aligned controls, 20 pt margins. Changes apply at once and are saved.
#include "settings_window.h"
#include "../settings.h"
#import <AppKit/AppKit.h>

@interface SettingsWindowController : NSWindowController
@property(strong) NSPopUpButton* frameRate;
@property(strong) NSPopUpButton* resolution;
@property(strong) NSButton* aimWhileBallInPlay;
@property(strong) NSButton* skipIntroVideos;
@end

static NSTextField* RowLabel(NSString* text)
{
    NSTextField* label = [NSTextField labelWithString:text];
    label.alignment = NSTextAlignmentRight;
    return label;
}

static NSPopUpButton* PopUp(NSArray<NSString*>* titles, NSArray<NSNumber*>* tags, id target, SEL action)
{
    NSPopUpButton* button = [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
    for (NSUInteger i = 0; i < titles.count; i++)
    {
        [button addItemWithTitle:titles[i]];
        button.lastItem.tag = tags[i].integerValue;
    }
    button.target = target;
    button.action = action;
    [button.widthAnchor constraintEqualToConstant:200].active = YES;
    return button;
}

@implementation SettingsWindowController
- (instancetype)init
{
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSZeroRect styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                     backing:NSBackingStoreBuffered defer:YES];
    window.title = @"Settings";
    if (!(self = [super initWithWindow:window]))
        return nil;

    self.frameRate = PopUp(@[ @"30 fps (Original)", @"60 fps" ], @[ @30, @60 ], self, @selector(changeFrameRate:));
    self.resolution = PopUp(@[ @"1280 × 720 (Original)", @"2560 × 1440", @"3840 × 2160 (4K)" ], @[ @1, @2, @3 ], self,
                            @selector(changeResolution:));
    self.aimWhileBallInPlay = [NSButton checkboxWithTitle:@"Aim while the ball is in play" target:self action:@selector(changeAim:)];
    self.skipIntroVideos = [NSButton checkboxWithTitle:@"Skip intro videos" target:self action:@selector(changeSkipIntro:)];

    NSGridView* grid = [NSGridView gridViewWithViews:@[
        @[ RowLabel(@"Frame rate:"), self.frameRate ],
        @[ RowLabel(@"Resolution:"), self.resolution ],
        @[ RowLabel(@"Gameplay:"), self.aimWhileBallInPlay ],
        @[ [NSGridCell emptyContentView], self.skipIntroVideos ],
    ]];
    grid.translatesAutoresizingMaskIntoConstraints = NO;
    grid.rowSpacing = 8;
    grid.columnSpacing = 8;
    grid.rowAlignment = NSGridRowAlignmentFirstBaseline;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementLeading;
    [grid rowAtIndex:2].topPadding = 12; // the gameplay options are a group of their own

    NSView* content = window.contentView;
    [content addSubview:grid];
    [NSLayoutConstraint activateConstraints:@[
        [grid.topAnchor constraintEqualToAnchor:content.topAnchor constant:20],
        [grid.bottomAnchor constraintEqualToAnchor:content.bottomAnchor constant:-20],
        [grid.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:20],
        [grid.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-20],
    ]];
    [window layoutIfNeeded];
    [window center];
    window.frameAutosaveName = @"Settings";
    return self;
}

- (void)refresh
{
    [self.frameRate selectItemWithTag:rt::FrameRate()];
    [self.resolution selectItemWithTag:rt::RenderScale()];
    self.aimWhileBallInPlay.state = rt::AimWhileBallInPlay() ? NSControlStateValueOn : NSControlStateValueOff;
    self.skipIntroVideos.state = rt::SkipIntroVideos() ? NSControlStateValueOn : NSControlStateValueOff;
}

// The frame rate and resolution change from the next frame.
- (void)changeFrameRate:(NSPopUpButton*)sender
{
    rt::SetFrameRate(int(sender.selectedTag));
    [NSUserDefaults.standardUserDefaults setInteger:sender.selectedTag forKey:@"FrameRate"];
}
- (void)changeResolution:(NSPopUpButton*)sender
{
    rt::SetRenderScale(int(sender.selectedTag));
    [NSUserDefaults.standardUserDefaults setInteger:sender.selectedTag forKey:@"RenderScale"];
}
- (void)changeAim:(NSButton*)sender
{
    rt::SetAimWhileBallInPlay(sender.state == NSControlStateValueOn);
    [NSUserDefaults.standardUserDefaults setBool:rt::AimWhileBallInPlay() forKey:@"AimWhileBallInPlay"];
}
- (void)changeSkipIntro:(NSButton*)sender
{
    rt::SetSkipIntroVideos(sender.state == NSControlStateValueOn);
    [NSUserDefaults.standardUserDefaults setBool:rt::SkipIntroVideos() forKey:@"SkipIntroVideos"];
}
@end

namespace rt
{
    void LoadSettings()
    {
        NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
        if (NSInteger fps = [defaults integerForKey:@"FrameRate"])
            SetFrameRate(int(fps));
        if (NSInteger scale = [defaults integerForKey:@"RenderScale"])
            SetRenderScale(int(scale));
        SetAimWhileBallInPlay([defaults boolForKey:@"AimWhileBallInPlay"] || AimWhileBallInPlay());
        SetSkipIntroVideos([defaults boolForKey:@"SkipIntroVideos"] || SkipIntroVideos());
    }

    void ShowSettings()
    {
        static SettingsWindowController* controller = [SettingsWindowController new];
        [controller refresh];
        [controller showWindow:nil];
    }
}
