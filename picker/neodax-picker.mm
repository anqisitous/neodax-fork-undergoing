
/*
 * neodax-picker.mm â è§£æå¯¾è±¡ããã«ã¼ (åä¸ãã¡ã¤ã«ã»ä¾å­ã¼ã­)
 *
 *   å®è¡ä¸­ã¢ããª / æè¿ä½¿ã£ãã¢ããª / ã»ãã·ã§ã³åã®èµ·åã»çµäºã¤ãã³ã
 *   ãä¸è¦§è¡¨ç¤ºããé¸ãã ã¢ããªã®æ¬ä½ãã¤ããªã«å¯¾ãã¦ neodax ãå®è¡ãã¦
 *   çµæãè¡¨ç¤ºãããC++ ãã Objective-C++ æ¡å¼µã§ NSWorkspace / CoreFoundation
 *   ãç´æ¥å¼ã¶ãããSwift ãå¤é¨GUIã©ã¤ãã©ãªãä¸è¦ã
 *
 *   ãã«ã:
 *     clang++ -std=c++17 -ObjC++ -framework Cocoa -framework CoreServices \
 *            neodax-picker.mm -o neodax-picker
 *
 *   å®è¡:
 *     ./neodax-picker
 *     ï¼ä¸ã®ãã£ã¼ã«ãã« neodax ã¸ã®ãã¹ã¨å¼æ°ãæå® â è¡ãé¸æ â ãè§£æå®è¡ãï¼
 */

#import <Cocoa/Cocoa.h>
#include <vector>
#include <string>
#include <algorithm>

/* ââ ã¨ã³ããªã¢ãã« ââââââââââââââââââââââââââââââââââââââââââââ */

struct AppEntry {
    std::string name;
    std::string status;   /* "å®è¡ä¸­" / "å±¥æ­´" / "èµ·å(ä»å)" / "çµäº(ä»å)" */
    std::string binPath;  /* Contents/MacOS/ ä¸ã®æ¬ä½ãã¤ããª */
    int         rowKind;  /* 0=å®è¡ä¸­ 1=å±¥æ­´ 2=ä»åã®èµ·å 3=ä»åã®çµäº */
};

/* ãã³ãã«URL â å®è¡ãã¤ããªã®ãã¹ */
static std::string executablePathOfBundle(NSURL *bundleURL) {
    if (!bundleURL) return {};
    NSBundle *b = [NSBundle bundleWithURL:bundleURL];
    NSString *exe = b.executablePath;
    if (!exe) {
        /* å®è¡ä¸­ã¢ããªããç´æ¥åã */
        return {};
    }
    return exe.UTF8String;
}

/* ââ æè¿ä½¿ã£ãã¢ããª (com.apple.recentitems.plist) âââââââââââââ */

static std::vector<AppEntry> recentAppsFromPlist() {
    std::vector<AppEntry> out;
    NSString *plistPath =
        [@"~/Library/Preferences/com.apple.recentitems.plist"
            stringByExpandingTildeInPath];
    NSData *data = [NSData dataWithContentsOfFile:plistPath];
    if (!data) return out;

    CFErrorRef cfErr = NULL;
    CFPropertyListRef pl = CFPropertyListCreateWithData(
        NULL, (__bridge CFDataRef)data, kCFPropertyListImmutable,
        NULL, &cfErr);
    if (!pl) {
        if (cfErr) CFRelease(cfErr);
        return out;
    }

    NSDictionary *root = (__bridge NSDictionary *)pl;
    NSArray *items = root[@"RecentApplications"][@"CustomListItems"];
    for (NSDictionary *item in items) {
        NSString *name = item[@"Name"];
        NSData   *bookmark = item[@"Bookmark"];
        if (!name || !bookmark) continue;

        BOOL stale = NO;
        CFErrorRef bErr = NULL;
        CFURLRef cfURL = CFURLCreateByResolvingBookmarkData(
            NULL, (__bridge CFDataRef)bookmark, 0, NULL, NULL, NULL, &stale);
        NSURL *url = cfURL ? (__bridge NSURL *)cfURL : nil;
        if (cfURL) {
            std::string exe = executablePathOfBundle(url);
            AppEntry e;
            e.name = name.UTF8String;
            e.status = "å±¥æ­´";
            e.binPath = exe.empty()
                ? (std::string)url.path.UTF8String
                : exe;
            e.rowKind = 1;
            if (!e.binPath.empty()) out.push_back(e);
            CFRelease(cfURL);
            if (bErr) CFRelease(bErr);
        }
    }
    CFRelease(pl);
    return out;
}

/* ââ å®è¡ä¸­ã¢ããª âââââââââââââââââââââââââââââââââââââââââââââââ */

static std::vector<AppEntry> runningApps() {
    std::vector<AppEntry> out;
    for (NSRunningApplication *ra in
             [NSWorkspace sharedWorkspace].runningApplications) {
        if (ra.activationPolicy != NSApplicationActivationPolicyRegular)
            continue;   /* Launchdããã«ãã¼ã¯é¤å¤ */
        NSString *exe = ra.executableURL.path;
        if (!exe) continue;
        AppEntry e;
        e.name    = ra.localizedName ? ra.localizedName.UTF8String : "?";
        e.status  = "å®è¡ä¸­";
        e.binPath = exe.UTF8String;
        e.rowKind = 0;
        out.push_back(e);
    }
    return out;
}

/* ââ ã¢ããªããªã²ã¼ã + UI ââââââââââââââââââââââââââââââââââââââ */

@interface PickerAppDelegate : NSObject <NSApplicationDelegate,
                                            NSTableViewDataSource,
                                            NSTableViewDelegate>
@property (nonatomic, strong) NSWindow        *window;
@property (nonatomic, strong) NSTableView     *table;
@property (nonatomic, strong) NSTextField     *pathLabel;
@property (nonatomic, strong) NSTextField     *neodaxPathField;
@property (nonatomic, strong) NSTextField     *argsField;
@property (nonatomic, strong) NSTextView      *outputView;
@property (nonatomic, assign) std::vector<AppEntry> entries;
- (void)runAnalysis:(id)sender;
- (void)reloadData;
@end

@implementation PickerAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;

    /* ââ ã¡ãã¥ã¼ (Cmd-Q ç¨ã®æå°æ§æ) ââ */
    NSMenu *menuBar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"NeoDAX Picker ãçµäº"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [menuBar addItem:appItem];
    NSApp.menu = menuBar;

    /* ââ ã¦ã£ã³ãã¦ ââ */
    NSRect frame = NSMakeRect(0, 0, 980, 640);
    self.window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"NeoDAX Picker â è§£æå¯¾è±¡ãé¸æ";
    self.window.center;

    NSView *c = self.window.contentView;

    /* ââ ãã¼ãã« ââ */
    NSTableColumn *colName = [[NSTableColumn alloc]
        initWithIdentifier:@"name"];
    colName.width = 260;
    [colName.headerCell setStringValue:@"ã¢ããª"];
    NSTableColumn *colStatus = [[NSTableColumn alloc]
        initWithIdentifier:@"status"];
    colStatus.width = 120;
    [colStatus.headerCell setStringValue:@"ç¶æ"];
    NSTableColumn *colPath = [[NSTableColumn alloc]
        initWithIdentifier:@"path"];
    colPath.width = 560;
    [colPath.headerCell setStringValue:@"ãã¤ããª"];

    NSScrollView *tableScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(16, 260, frame.size.width - 32, 250)];
    self.table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.usesAlternatingRowBackgroundColors = YES;
    self.table.allowsMultipleSelection = NO;
    self.table.rowHeight = 24;
    [self.table addTableColumn:colName];
    [self.table addTableColumn:colStatus];
    [self.table addTableColumn:colPath];
    tableScroll.documentView = self.table;
    tableScroll.hasVerticalScroller = YES;
    [c addSubview:tableScroll];

    /* ââ é¸æä¸­ãã¹è¡¨ç¤º ââ */
    self.pathLabel = [NSTextField labelWithString:@"â"];
    self.pathLabel.frame = NSMakeRect(16, 236, frame.size.width - 32, 20);
    self.pathLabel.font = [NSFont systemFontOfSize:12];
    [c addSubview:self.pathLabel];

    /* ââ neodax ãã¹ / å¼æ° ââ */
    NSTextField *lp = [NSTextField labelWithString:@"neodax:"];
    lp.frame = NSMakeRect(16, 206, 60, 20);
    [c addSubview:lp];
    self.neodaxPathField = [[NSTextField alloc]
        initWithFrame:NSMakeRect(80, 204, 260, 24)];
    self.neodaxPathField.stringValue = @"./neodax";
    [c addSubview:self.neodaxPathField];

    NSTextField *la = [NSTextField labelWithString:@"å¼æ°:"];
    la.frame = NSMakeRect(352, 206, 42, 20);
    [c addSubview:la];
    self.argsField = [[NSTextField alloc]
        initWithFrame:NSMakeRect(396, 204, 440, 24)];
    self.argsField.stringValue = @"--funcs --callgraph --depflow";
    [c addSubview:self.argsField];

    /* ââ å®è¡ãã¿ã³ ââ */
    NSButton *run = [[NSButton alloc]
        initWithFrame:NSMakeRect(844, 202, 120, 28)];
    run.title = @"è§£æå®è¡";
    run.bezelStyle = NSBezelStyleRoundedBevel;
    run.font = [NSFont boldSystemFontOfSize:13];
    [run setTarget:self];
    [run setAction:@selector(runAnalysis:)];
    [c addSubview:run];

    /* ââ åºåãã¥ã¼ ââ */
    NSScrollView *outScroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(16, 16, frame.size.width - 32, 178)];
    self.outputView = [[NSTextView alloc]
        initWithFrame:NSMakeRect(0, 0,
            frame.size.width - 32, 178)];
    self.outputView.editable = NO;
    self.outputView.font = [NSFont fontWithName:@"Menlo" size:11];
    outScroll.documentView = self.outputView;
    outScroll.hasVerticalScroller = YES;
    [c addSubview:outScroll];

    /* ââ åæãã¼ã¿: å®è¡ä¸­ + å±¥æ­´ ââ */
    self.entries = runningApps();
    {
        std::vector<AppEntry> rec = recentAppsFromPlist();
        for (auto &e : rec) {
            auto dup = std::find_if(self.entries.begin(), self.entries.end(),
                [&](const AppEntry &r){ return r.binPath == e.binPath; });
            if (dup == self.entries.end()) self.entries.push_back(e);
        }
    }
    [self reloadData];

    /* ââ ã»ãã·ã§ã³åã®èµ·å/çµäºã¤ãã³ããæ¾ã ââ */
    NSWorkspace *ws = [NSWorkspace sharedWorkspace];
    [[ws notificationCenter]
        addObserver:self
        selector:@selector(appDidLaunch:)
        name:NSWorkspaceDidLaunchApplicationNotification
        object:nil];
    [[ws notificationCenter]
        addObserver:self
        selector:@selector(appDidTerminate:)
        name:NSWorkspaceDidTerminateApplicationNotification
        object:nil];

    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

/* èµ·åã¤ãã³ã */
- (void)appDidLaunch:(NSNotification *)n {
    NSRunningApplication *ra = n.userInfo[NSWorkspaceApplicationKey];
    if (!ra || ra.activationPolicy != NSApplicationActivationPolicyRegular)
        return;
    AppEntry e;
    e.name    = ra.localizedName ? ra.localizedName.UTF8String : "?";
    e.status  = "èµ·å(ä»å)";
    e.binPath = ra.executableURL.path.UTF8String;
    e.rowKind = 2;
    if (e.binPath.empty()) return;
    self.entries.insert(self.entries.begin(), e);
    [self reloadData];
}

/* çµäºã¤ãã³ã */
- (void)appDidTerminate:(NSNotification *)n {
    NSRunningApplication *ra = n.userInfo[NSWorkspaceApplicationKey];
    if (!ra) return;
    AppEntry e;
    e.name    = ra.localizedName ? ra.localizedName.UTF8String : "?";
    e.status  = "çµäº(ä»å)";
    e.binPath = ra.executableURL.path.UTF8String;
    e.rowKind = 3;
    if (e.binPath.empty()) return;
    self.entries.insert(self.entries.begin(), e);
    [self reloadData];
}

- (void)reloadData {
    /* ç¶æåã®åè¨ç®: çµäºæ¸ã¿ãªãå®è¡ä¸­ãã©ã°ãå¤ã */
    [self.table reloadData];
}

/* ââ NSTableViewDataSource ââ */
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)self.entries.size();
}

- (id)tableView:(NSTableView *)tv
        objectValueForTableColumn:(NSTableColumn *)col
        row:(NSInteger)row {
    (void)tv;
    if (row < 0 || (size_t)row >= self.entries.size()) return @"";
    const AppEntry &e = self.entries[(size_t)row];
    NSString *id = col.identifier;
    if ([id isEqualToString:@"name"])   return @(e.name.c_str());
    if ([id isEqualToString:@"status"]) return @(e.status.c_str());
    return @(e.binPath.c_str());
}

- (void)tableViewSelectionDidChange:(NSNotification *)n {
    (void)n;
    NSInteger r = self.table.selectedRow;
    if (r < 0 || (size_t)r >= self.entries.size()) return;
    self.pathLabel.stringValue =
        @(self.entries[(size_t)r].binPath.c_str());
}

/* ââ neodax å®è¡ ââ */
- (void)runAnalysis:(id)sender {
    (void)sender;
    NSInteger r = self.table.selectedRow;
    if (r < 0 || (size_t)r >= self.entries.size()) {
        self.outputView.string =
            @"è¡ãé¸æãã¦ãããè§£æå®è¡ããæ¼ãã¦ãã ããã";
        return;
    }
    const AppEntry &e = self.entries[(size_t)r];

    NSString *neodaxPath = self.neodaxPathField.stringValue;
    NSArray  *args = [self.argsField.stringValue
        componentsSeparatedByString:@" "];
    args = [args filteredArrayUsingPredicate:
        [NSPredicate predicateWithFormat:@"length > 0"]];
    NSMutableArray *all = [args mutableCopy];
    [all addObject:@(e.binPath.c_str())];

    NSString *cmdLine = [NSString stringWithFormat:@"%@ %@ %@",
        neodaxPath, [args componentsJoinedByString:@" "],
        @(e.binPath.c_str())];
    [self.outputView insertText:
        [NSString stringWithFormat:@"$ %@\n", cmdLine]
        replacementRange:NSMakeRange([self.outputView.string length], 0)];

    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        NSTask *task = [[NSTask alloc] init];
        task.executableURL = [NSURL fileURLWithPath:neodaxPath];
        task.arguments = all;

        NSPipe *outPipe = [NSPipe pipe];
        NSPipe *errPipe = [NSPipe pipe];
        task.standardOutput = outPipe;
        task.standardError  = errPipe;
        task.currentDirectoryPath = NSHomeDirectory();

        [task launch];
        NSData *outD = [[outPipe fileHandleForReading]
            readDataToEndOfFile];
        NSData *errD = [[errPipe fileHandleForReading]
            readDataToEndOfFile];
        [task waitUntilExit];

        NSString *outS = outD.length
            ? [[NSString alloc] initWithData:outD
                encoding:NSUTF8StringEncoding] : @"";
        NSString *errS = errD.length
            ? [[NSString alloc] initWithData:errD
                encoding:NSUTF8StringEncoding] : @"";
        int rc = (int)task.terminationStatus;

        dispatch_async(dispatch_get_main_queue(), ^{
            NSString *blob = [NSString stringWithFormat:
                @"%@%@\n[exit %d]\n\n", outS, errS, rc];
            [self.outputView insertText:blob
                replacementRange:NSMakeRange([self.outputView.string length], 0)];
        });
    });
}

@end

/* ââ main âââââââââââââââââââââââââââââââââââââââââââââââââââââââ */

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        PickerAppDelegate *del = [[PickerAppDelegate alloc] init];
        [app setDelegate:del];
        [app run];
    }
    return 0;
}
