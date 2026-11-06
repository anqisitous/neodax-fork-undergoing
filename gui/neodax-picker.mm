
/*
 * neodax-picker.mm — 解析対象ピッカー (単一ファイル・依存ゼロ)
 *
 *   実行中アプリ / 最近使ったアプリ / セッション内の起動・終了イベント
 *   を一覧表示し、選んだアプリの本体バイナリに対して neodax を実行して
 *   結果を表示する。C++ から Objective-C++ 拡張で NSWorkspace / CoreFoundation
 *   を直接呼ぶため、Swift も外部GUIライブラリも不要。
 *
 *   ビルド:
 *     clang++ -std=c++17 -ObjC++ -framework Cocoa -framework CoreServices \
 *            neodax-picker.mm -o neodax-picker
 *
 *   実行:
 *     ./neodax-picker
 *     （上のフィールドに neodax へのパスと引数を指定 → 行を選択 → 「解析実行」）
 */

#import <Cocoa/Cocoa.h>
#include <vector>
#include <string>
#include <algorithm>

/* ── エントリモデル ──────────────────────────────────────────── */

struct AppEntry {
    std::string name;
    std::string status;   /* "実行中" / "履歴" / "起動(今回)" / "終了(今回)" */
    std::string binPath;  /* Contents/MacOS/ 下の本体バイナリ */
    int         rowKind;  /* 0=実行中 1=履歴 2=今回の起動 3=今回の終了 */
};

/* バンドルURL → 実行バイナリのパス */
static std::string executablePathOfBundle(NSURL *bundleURL) {
    if (!bundleURL) return {};
    NSBundle *b = [NSBundle bundleWithURL:bundleURL];
    NSString *exe = b.executablePath;
    if (!exe) {
        /* 実行中アプリから直接取る */
        return {};
    }
    return exe.UTF8String;
}

/* ── 最近使ったアプリ (com.apple.recentitems.plist) ───────────── */

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

        Boolean isStale = false;
        CFErrorRef bErr = NULL;
        CFURLRef cfURL = CFURLCreateByResolvingBookmarkData(
            kCFAllocatorDefault, (__bridge CFDataRef)bookmark,
            kCFURLBookmarkResolutionWithoutUIMask, NULL, NULL,
            &isStale, &bErr);
        if (!cfURL) {
            if (bErr) CFRelease(bErr);
            continue;
        }
        NSURL *url = (__bridge NSURL *)cfURL;
        std::string exe = executablePathOfBundle(url);
        AppEntry e;
        e.name = name.UTF8String;
        e.status = "履歴";
        e.binPath = exe.empty()
            ? (std::string)url.path.UTF8String
            : exe;
        e.rowKind = 1;
        if (!e.binPath.empty()) out.push_back(e);
        CFRelease(cfURL);
    }
    CFRelease(pl);
    return out;
}

/* ── 実行中アプリ ─────────────────────────────────────────────── */

static std::vector<AppEntry> runningApps() {
    std::vector<AppEntry> out;
    for (NSRunningApplication *ra in
             [NSWorkspace sharedWorkspace].runningApplications) {
        if (ra.activationPolicy != NSApplicationActivationPolicyRegular)
            continue;   /* Launchdやヘルパーは除外 */
        NSString *exe = ra.executableURL.path;
        if (!exe) continue;
        AppEntry e;
        e.name    = ra.localizedName ? ra.localizedName.UTF8String : "?";
        e.status  = "実行中";
        e.binPath = exe.UTF8String;
        e.rowKind = 0;
        out.push_back(e);
    }
    return out;
}

/* ── アプリデリゲート + UI ────────────────────────────────────── */

@interface PickerAppDelegate : NSObject <NSApplicationDelegate,
                                            NSTableViewDataSource,
                                            NSTableViewDelegate> {
    std::vector<AppEntry> _entries;
}
@property (nonatomic, strong) NSWindow        *window;
@property (nonatomic, strong) NSTableView     *table;
@property (nonatomic, strong) NSTextField     *pathLabel;
@property (nonatomic, strong) NSTextField     *neodaxPathField;
@property (nonatomic, strong) NSTextField     *argsField;
@property (nonatomic, strong) NSTextView      *outputView;
- (void)runAnalysis:(id)sender;
- (void)reloadData;
@end

@implementation PickerAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;

    /* ── メニュー (Cmd-Q 用の最小構成) ── */
    NSMenu *menuBar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"NeoDAX Picker を終了"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    appItem.submenu = appMenu;
    [menuBar addItem:appItem];
    NSApp.menu = menuBar;

    /* ── ウィンドウ ── */
    NSRect frame = NSMakeRect(0, 0, 980, 640);
    self.window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                   NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"NeoDAX Picker — 解析対象を選択";
    [self.window center];

    NSView *c = self.window.contentView;

    /* ── テーブル ── */
    NSTableColumn *colName = [[NSTableColumn alloc]
        initWithIdentifier:@"name"];
    colName.width = 260;
    [colName.headerCell setStringValue:@"アプリ"];
    NSTableColumn *colStatus = [[NSTableColumn alloc]
        initWithIdentifier:@"status"];
    colStatus.width = 120;
    [colStatus.headerCell setStringValue:@"状態"];
    NSTableColumn *colPath = [[NSTableColumn alloc]
        initWithIdentifier:@"path"];
    colPath.width = 560;
    [colPath.headerCell setStringValue:@"バイナリ"];

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

    /* ── 選択中パス表示 ── */
    self.pathLabel = [NSTextField labelWithString:@"—"];
    self.pathLabel.frame = NSMakeRect(16, 236, frame.size.width - 32, 20);
    self.pathLabel.font = [NSFont systemFontOfSize:12];
    [c addSubview:self.pathLabel];

    /* ── neodax パス / 引数 ── */
    NSTextField *lp = [NSTextField labelWithString:@"neodax:"];
    lp.frame = NSMakeRect(16, 206, 60, 20);
    [c addSubview:lp];
    self.neodaxPathField = [[NSTextField alloc]
        initWithFrame:NSMakeRect(80, 204, 260, 24)];
    self.neodaxPathField.stringValue = @"./neodax";
    [c addSubview:self.neodaxPathField];

    NSTextField *la = [NSTextField labelWithString:@"引数:"];
    la.frame = NSMakeRect(352, 206, 42, 20);
    [c addSubview:la];
    self.argsField = [[NSTextField alloc]
        initWithFrame:NSMakeRect(396, 204, 440, 24)];
    self.argsField.stringValue = @"--funcs --callgraph --depflow";
    [c addSubview:self.argsField];

    /* ── 実行ボタン ── */
    NSButton *run = [[NSButton alloc]
        initWithFrame:NSMakeRect(844, 202, 120, 28)];
    run.title = @"解析実行";
    run.font = [NSFont boldSystemFontOfSize:13];
    [run setTarget:self];
    [run setAction:@selector(runAnalysis:)];
    [c addSubview:run];

    /* ── 出力ビュー ── */
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

    /* ── 初期データ: 実行中 + 履歴 ── */
    _entries = runningApps();
    {
        std::vector<AppEntry> rec = recentAppsFromPlist();
        for (auto &e : rec) {
            auto dup = std::find_if(_entries.begin(), _entries.end(),
                [&](const AppEntry &r){ return r.binPath == e.binPath; });
            if (dup == _entries.end()) _entries.push_back(e);
        }
    }
    [self reloadData];

    /* ── セッション内の起動/終了イベントを拾う ── */
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

/* 起動イベント */
- (void)appDidLaunch:(NSNotification *)n {
    NSRunningApplication *ra = n.userInfo[NSWorkspaceApplicationKey];
    if (!ra || ra.activationPolicy != NSApplicationActivationPolicyRegular)
        return;
    AppEntry e;
    e.name    = ra.localizedName ? ra.localizedName.UTF8String : "?";
    e.status  = "起動(今回)";
    e.binPath = ra.executableURL.path.UTF8String;
    e.rowKind = 2;
    if (e.binPath.empty()) return;
    _entries.insert(_entries.begin(), e);
    [self reloadData];
}

/* 終了イベント */
- (void)appDidTerminate:(NSNotification *)n {
    NSRunningApplication *ra = n.userInfo[NSWorkspaceApplicationKey];
    if (!ra) return;
    AppEntry e;
    e.name    = ra.localizedName ? ra.localizedName.UTF8String : "?";
    e.status  = "終了(今回)";
    e.binPath = ra.executableURL.path.UTF8String;
    e.rowKind = 3;
    if (e.binPath.empty()) return;
    _entries.insert(_entries.begin(), e);
    [self reloadData];
}

- (void)reloadData {
    /* 状態列の再計算: 終了済みなら実行中フラグを外す */
    [self.table reloadData];
}

/* ── NSTableViewDataSource ── */
- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    (void)tv;
    return (NSInteger)_entries.size();
}

- (id)tableView:(NSTableView *)tv
        objectValueForTableColumn:(NSTableColumn *)col
        row:(NSInteger)row {
    (void)tv;
    if (row < 0 || (size_t)row >= _entries.size()) return @"";
    AppEntry e = _entries[(size_t)row];
    NSString *id = col.identifier;
    if ([id isEqualToString:@"name"])   return @(e.name.c_str());
    if ([id isEqualToString:@"status"]) return @(e.status.c_str());
    return @(e.binPath.c_str());
}

- (void)tableViewSelectionDidChange:(NSNotification *)n {
    (void)n;
    NSInteger r = self.table.selectedRow;
    if (r < 0 || (size_t)r >= _entries.size()) return;
    self.pathLabel.stringValue =
        @(_entries[(size_t)r].binPath.c_str());
}

/* ── neodax 実行 ── */
- (void)runAnalysis:(id)sender {
    (void)sender;
    NSInteger r = self.table.selectedRow;
    if (r < 0 || (size_t)r >= _entries.size()) {
        self.outputView.string =
            @"行を選択してから「解析実行」を押してください。";
        return;
    }
    AppEntry e = _entries[(size_t)r];

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

/* ── main ─────────────────────────────────────────────────────── */

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
