// neodax_editor.mm — 「必要なノードだけ」を編集する独立 GUI。
//
// これは NeoDAX.app の一部ではない。gui/build.sh からも呼ばない。
// 使うときだけ `bash gui/editor/build.sh` で別バイナリを作る。
//
// 画面:
//   左  : 検索セル / 必要ノード一覧 / ファイル番号ボックス
//   中央: 必要ノードのセルを縦に直結したエディタ
//   右  : 元ファイル (行単位で選択・ドラッグできる)
//
// 操作:
//   - ノードをクリック → そのセルへ移動
//   - セル内で ↑/↓ → 隣のセルへ。関数/クラス名の内側で ←/→ の後なら
//     その定義ブロックのセルを上下に追加
//   - 追加セル内で Esc → そのセルを削除
//   - Ctrl+Enter → 右の元ファイルビューで現在行を開く
//   - 右の行を選択して中央へドラッグ → 元コードセルを追加
//   - 右の「≡」をドラッグ → ファイル全体のセルを追加
//   - セルを空にすると、そのセル (と対応ノード) を削除

#import <Cocoa/Cocoa.h>

#include "nx_editor_model.hpp"

#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <string>
#include <vector>

static NSString *const NxLinePasteboardType = @"org.neodax.editor.line";
static NSString *const NxFilePasteboardType = @"org.neodax.editor.file";

static NSColor *NxBg(void)    { return [NSColor colorWithCalibratedRed:0.07 green:0.08 blue:0.10 alpha:1]; }
static NSColor *NxPanel(void) { return [NSColor colorWithCalibratedRed:0.10 green:0.11 blue:0.14 alpha:1]; }
static NSColor *NxCellBg(void){ return [NSColor colorWithCalibratedRed:0.12 green:0.13 blue:0.17 alpha:1]; }
static NSColor *NxLine(void)  { return [NSColor colorWithCalibratedRed:0.30 green:0.33 blue:0.40 alpha:1]; }
static NSColor *NxText(void)  { return [NSColor colorWithCalibratedRed:0.88 green:0.90 blue:0.94 alpha:1]; }
static NSColor *NxDim(void)   { return [NSColor colorWithCalibratedRed:0.55 green:0.59 blue:0.67 alpha:1]; }
static NSColor *NxAccent(void){ return [NSColor colorWithCalibratedRed:0.95 green:0.66 blue:0.22 alpha:1]; }

static NSString *NxTrim(NSString *s) {
    return [s stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

static NSInteger NxLineCount(NSString *s) {
    if (s.length == 0) return 1;
    NSInteger n = 1;
    for (NSUInteger i = 0; i < s.length; i++)
        if ([s characterAtIndex:i] == '\n' && i + 1 < s.length) n++;
    return n;
}

static NSInteger NxLineIndex(NSString *s, NSUInteger loc) {
    if (loc > s.length) loc = s.length;
    NSInteger n = 0;
    for (NSUInteger i = 0; i < loc; i++)
        if ([s characterAtIndex:i] == '\n') n++;
    return n;
}

static NSUInteger NxLineStart(NSString *s, NSInteger line) {
    if (line <= 0) return 0;
    NSInteger n = 0;
    for (NSUInteger i = 0; i < s.length; i++) {
        if ([s characterAtIndex:i] == '\n') {
            n++;
            if (n == line) return i + 1;
        }
    }
    return s.length;
}

static NSInteger NxLineLength(NSString *s, NSInteger line) {
    NSUInteger a = NxLineStart(s, line);
    if (a >= s.length) return 0;
    NSRange r = [s rangeOfString:@"\n" options:0 range:NSMakeRange(a, s.length - a)];
    return r.location == NSNotFound ? (NSInteger)(s.length - a) : (NSInteger)r.location;
}

@class NxEditorController;

// ------------------------------------------------------------------
// セル
// ------------------------------------------------------------------
typedef NS_ENUM(NSInteger, NxCellKind) {
    NxCellNode = 0,
    NxCellDerived = 1,
    NxCellOriginal = 2,
};

@interface NxEditorCell : NSObject
@property (nonatomic) NxCellKind kind;
@property (nonatomic, copy) NSString *title;
@property (nonatomic, copy) NSString *subtitle;
@property (nonatomic, copy) NSString *file;
@property (nonatomic) NSInteger startLine;
@property (nonatomic) NSInteger endLine;
@property (nonatomic) NSInteger nodeIndex;
@property (nonatomic, copy) NSString *originalText;
@end
@implementation NxEditorCell
@end

// ------------------------------------------------------------------
// 編集可能なコード
// ------------------------------------------------------------------
@interface NxCodeTextView : NSTextView
@property (nonatomic, weak) NxEditorController *controller;
@property (nonatomic, strong) NxEditorCell *cell;
@property (nonatomic) BOOL semanticNameActive;
- (void)updateSemanticName;
@end

// ------------------------------------------------------------------
// 1 セル分の枠
// ------------------------------------------------------------------
@interface NxCellView : NSView
@property (nonatomic, strong) NxEditorCell *cell;
@property (nonatomic, strong) NSTextView *textView;
- (instancetype)initWithCell:(NxEditorCell *)cell controller:(NxEditorController *)controller;
- (CGFloat)heightForWidth:(CGFloat)width;
- (void)applyCell;
@end

// ------------------------------------------------------------------
// セルを縦に直結するスタック
// ------------------------------------------------------------------
@interface NxCellStackView : NSView <NSDraggingDestination>
@property (nonatomic, weak) NxEditorController *controller;
@property (nonatomic, strong) NSMutableArray<NxCellView *> *cellViews;
- (void)insertCellView:(NxCellView *)v atIndex:(NSInteger)idx;
- (void)removeCellView:(NxCellView *)v;
- (void)relayout;
- (NSInteger)insertionIndexForPoint:(NSPoint)p;
@end

// ------------------------------------------------------------------
// 四角いウィンドウ
// ------------------------------------------------------------------
@interface NxFlippedView : NSView
@end
@implementation NxFlippedView
- (BOOL)isFlipped { return YES; }
@end

// Cocoa の丸い標準コントロール (NSSearchField / NSComboBox / NSButton) は
// 見た目に使わない。四角い自前 View を基本部品にする。
@interface NxBinaryView : NxFlippedView
@property (nonatomic, strong) NSColor *fillColor;
@property (nonatomic, strong) NSColor *borderColor;
@property (nonatomic) BOOL drawsBorder;
@end

@implementation NxBinaryView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.fillColor = NxPanel();
    self.borderColor = NxLine();
    self.drawsBorder = YES;
    return self;
}
- (void)drawRect:(NSRect)dirty {
    [(self.fillColor ?: NxPanel()) setFill];
    NSRectFill(self.bounds);
    if (self.drawsBorder) {
        [(self.borderColor ?: NxLine()) setStroke];
        NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
        p.lineWidth = 1;
        [p stroke];
    }
}
@end

@interface NxSquareField : NxBinaryView
@property (nonatomic, strong) NSTextField *field;
- (instancetype)initWithFrame:(NSRect)frame placeholder:(NSString *)placeholder;
- (NSString *)textValue;
- (void)setTextValue:(NSString *)s;
@end

@implementation NxSquareField
- (instancetype)initWithFrame:(NSRect)frame placeholder:(NSString *)placeholder {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.field = [[NSTextField alloc] initWithFrame:NSMakeRect(6, 2, frame.size.width - 12, frame.size.height - 4)];
    self.field.bezeled = NO;
    self.field.bordered = NO;
    self.field.drawsBackground = NO;
    self.field.focusRingType = NSFocusRingTypeNone;
    self.field.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    self.field.textColor = NxText();
    self.field.placeholderString = placeholder;
    self.field.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [self addSubview:self.field];
    return self;
}
- (NSString *)textValue { return self.field.stringValue ?: @""; }
- (void)setTextValue:(NSString *)s { self.field.stringValue = s ?: @""; }
@end

@interface NxSquareButton : NxBinaryView
@property (nonatomic, copy) NSString *title;
@property (nonatomic, weak) id target;
@property (nonatomic) SEL action;
@end

@implementation NxSquareButton
- (instancetype)initWithFrame:(NSRect)frame title:(NSString *)title {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.title = title;
    return self;
}
- (void)drawRect:(NSRect)dirty {
    [super drawRect:dirty];
    NSDictionary *a = @{NSForegroundColorAttributeName: NxText(),
                        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold]};
    NSSize sz = [self.title sizeWithAttributes:a];
    [self.title drawAtPoint:NSMakePoint((self.bounds.size.width - sz.width) / 2,
                                        (self.bounds.size.height - sz.height) / 2)
             withAttributes:a];
}
- (void)mouseDown:(NSEvent *)event {
    if (self.target && self.action && [self.target respondsToSelector:self.action])
        [NSApp sendAction:self.action to:self.target from:self];
}
@end

@interface NxTitleBar : NxFlippedView
@property (nonatomic, weak) NSWindow *hostWindow;
@property (nonatomic, strong) NSTextField *label;
@end

@interface NxResizeHandle : NSView
@property (nonatomic, weak) NSWindow *hostWindow;
@end

@interface NxFileDragHandle : NSView <NSDraggingSource>
@property (nonatomic, copy) NSString *path;
@end

@interface NxSquareWindow : NSWindow
@end

// ==================================================================
// コントローラ
// ==================================================================
@interface NxEditorController : NSObject <NSApplicationDelegate, NSTableViewDataSource,
    NSTableViewDelegate, NSTextFieldDelegate, NSTextViewDelegate> {
    nxedit::Project *_project;
    std::vector<bool> _nodeAlive;
    std::vector<NSInteger> _visibleNodeIndexes;
}
@property (nonatomic, strong) NxSquareWindow *window;
@property (nonatomic, strong) NxTitleBar *titleBar;
@property (nonatomic, strong) NSTableView *nodeTable;
@property (nonatomic, strong) NSTableView *lineTable;
@property (nonatomic, strong) NSScrollView *nodeScroll;
@property (nonatomic, strong) NSScrollView *lineScroll;
@property (nonatomic, strong) NSScrollView *cellScroll;
@property (nonatomic, strong) NxCellStackView *stack;
@property (nonatomic, strong) NxSquareField *searchBox;
@property (nonatomic, strong) NxSquareField *fileBox;
@property (nonatomic, strong) NxSquareButton *saveButton;
@property (nonatomic, strong) NSTextField *fileLabel;
@property (nonatomic, strong) NSTextField *status;
@property (nonatomic, strong) NxFileDragHandle *fileDrag;
@property (nonatomic, strong) NSMutableArray<NxEditorCell *> *cells;
@property (nonatomic, copy) NSString *rootPath;
@property (nonatomic, copy) NSString *mapPath;
@property (nonatomic) NSInteger currentFileId;
@property (nonatomic) BOOL loaded;

- (nxedit::Project *)project;
- (void)reloadNodeTable;
- (void)showNodeAtIndex:(NSInteger)idx;
- (void)showFileId:(NSInteger)fid line:(NSInteger)line;
- (void)addSemanticCellForTextView:(NxCodeTextView *)tv above:(BOOL)above;
- (void)focusCellFromTextView:(NxCodeTextView *)tv delta:(NSInteger)delta;
- (void)cellTextChanged:(NxCodeTextView *)tv;
- (void)removeCellViewForTextView:(NxCodeTextView *)tv;
- (NSInteger)insertIndexForDrop:(NSPoint)p;
- (void)acceptDropItems:(NSArray<NSPasteboardItem *> *)items atIndex:(NSInteger)idx;
- (void)saveAll;
@end

// ==================================================================
// NxCodeTextView
// ==================================================================
@implementation NxCodeTextView

- (void)keyDown:(NSEvent *)event {
    NSEventModifierFlags flags =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    BOOL ctrl = (flags & NSEventModifierFlagControl) != 0;
    BOOL cmd = (flags & NSEventModifierFlagCommand) != 0;
    unsigned short key = event.keyCode;

    if (cmd && [[event.charactersIgnoringModifiers lowercaseString] isEqualToString:@"s"]) {
        [self.controller saveAll];
        return;
    }

    if (ctrl && key == 36) { // Ctrl+Enter
        NSUInteger loc = self.selectedRange.location;
        NSInteger lineInCell = NxLineIndex(self.string, loc);
        NSInteger fileLine = self.cell.startLine + lineInCell;
        if (self.cell.file.length) {
            int fid = self.controller.project->file_id(self.cell.file.UTF8String);
            if (fid > 0) [self.controller showFileId:fid line:fileLine];
        }
        return;
    }

    if (key == 53) { // Esc
        if (self.cell.kind != NxCellNode) {
            [self.controller removeCellViewForTextView:self];
            return;
        }
    }

    if (key == 123 || key == 124) { // left / right
        [super keyDown:event];
        [self updateSemanticName];
        return;
    }

    if (key == 126 || key == 125) { // up / down
        if (self.semanticNameActive) {
            [self.controller addSemanticCellForTextView:self above:(key == 126)];
            return;
        }
        NSRange sel = self.selectedRange;
        NSInteger line = NxLineIndex(self.string, sel.location);
        NSInteger last = NxLineCount(self.string) - 1;
        if (key == 126 && line == 0) {
            [self.controller focusCellFromTextView:self delta:-1];
            return;
        }
        if (key == 125 && line >= last) {
            [self.controller focusCellFromTextView:self delta:1];
            return;
        }
    }

    [super keyDown:event];
}

- (void)updateSemanticName {
    if (!self.cell || !self.controller) { self.semanticNameActive = NO; return; }
    NSUInteger loc = self.selectedRange.location;
    NSInteger lineInCell = NxLineIndex(self.string, loc);
    NSInteger col = (NSInteger)(loc - NxLineStart(self.string, lineInCell)) + 1;
    NSInteger line = self.cell.startLine + lineInCell;
    const nxedit::Symbol *s = self.controller.project->symbol_for_position(
        self.cell.file.UTF8String, (int)line, (int)col);
    self.semanticNameActive = (s != nullptr);
}

@end

// ==================================================================
// NxCellView
// ==================================================================
@implementation NxCellView

- (instancetype)initWithCell:(NxEditorCell *)cell controller:(NxEditorController *)controller {
    self = [super initWithFrame:NSMakeRect(0, 0, 600, 100)];
    if (!self) return nil;
    self.cell = cell;

    NSTextField *head = [NSTextField labelWithString:cell.title ?: @""];
    head.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold];
    head.textColor = NxText();
    head.lineBreakMode = NSLineBreakByTruncatingTail;
    head.frame = NSMakeRect(10, 4, 600 - 20, 14);
    head.autoresizingMask = NSViewWidthSizable;
    head.tag = 101;
    [self addSubview:head];

    NSTextField *sub = [NSTextField labelWithString:cell.subtitle ?: @""];
    sub.font = [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular];
    sub.textColor = NxDim();
    sub.lineBreakMode = NSLineBreakByTruncatingTail;
    sub.frame = NSMakeRect(10, 18, 600 - 20, 12);
    sub.autoresizingMask = NSViewWidthSizable;
    sub.tag = 102;
    [self addSubview:sub];

    NxCodeTextView *tv = [[NxCodeTextView alloc] initWithFrame:NSMakeRect(8, 34, 584, 60)];
    tv.controller = controller;
    tv.cell = cell;
    tv.delegate = controller;
    tv.string = cell.originalText ?: @"";
    tv.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    tv.textColor = NxText();
    tv.backgroundColor = NxCellBg();
    tv.insertionPointColor = NxAccent();
    tv.richText = NO;
    tv.automaticQuoteSubstitutionEnabled = NO;
    tv.automaticDashSubstitutionEnabled = NO;
    tv.automaticTextReplacementEnabled = NO;
    tv.automaticSpellingCorrectionEnabled = NO;
    tv.continuousSpellCheckingEnabled = NO;
    tv.grammarCheckingEnabled = NO;
    tv.verticallyResizable = YES;
    tv.horizontallyResizable = YES;
    tv.minSize = NSMakeSize(0, 0);
    tv.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    tv.textContainer.widthTracksTextView = NO;
    tv.textContainer.containerSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    tv.textContainerInset = NSMakeSize(4, 2);
    [self addSubview:tv];
    self.textView = tv;
    return self;
}

- (void)applyCell {
    NSTextField *head = [self viewWithTag:101];
    NSTextField *sub = [self viewWithTag:102];
    head.stringValue = self.cell.title ?: @"";
    sub.stringValue = self.cell.subtitle ?: @"";
    self.needsDisplay = YES;
}

- (CGFloat)heightForWidth:(CGFloat)width {
    NSInteger lines = NxLineCount(self.textView.string);
    CGFloat lh = 16.0;
    CGFloat h = 34 + std::max<NSInteger>(3, lines) * lh + 8;
    return h;
}

- (void)layout {
    [super layout];
    CGFloat w = self.bounds.size.width;
    NSTextField *head = [self viewWithTag:101];
    NSTextField *sub = [self viewWithTag:102];
    head.frame = NSMakeRect(10, 4, w - 20, 14);
    sub.frame = NSMakeRect(10, 18, w - 20, 12);
    CGFloat th = self.bounds.size.height - 34 - 6;
    self.textView.frame = NSMakeRect(8, 34, std::max<CGFloat>(20, w - 16), std::max<CGFloat>(20, th));
}

- (void)drawRect:(NSRect)dirty {
    [NxPanel() setFill];
    NSRectFill(self.bounds);

    NSColor *accent = self.cell.kind == NxCellNode ? NxAccent()
                    : self.cell.kind == NxCellDerived ? [NSColor colorWithCalibratedRed:0.40 green:0.70 blue:0.95 alpha:1]
                    : [NSColor colorWithCalibratedRed:0.45 green:0.75 blue:0.55 alpha:1];
    [accent setFill];
    NSRectFill(NSMakeRect(0, 0, 3, self.bounds.size.height));

    [NxLine() setStroke];
    NSBezierPath *p = [NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)];
    p.lineWidth = 1.0;
    [p stroke];
}

@end

// ==================================================================
// NxCellStackView
// ==================================================================
@implementation NxCellStackView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.cellViews = [NSMutableArray array];
    [self registerForDraggedTypes:@[NxLinePasteboardType, NxFilePasteboardType]];
    return self;
}

- (BOOL)isFlipped { return YES; }

- (void)insertCellView:(NxCellView *)v atIndex:(NSInteger)idx {
    if (idx < 0 || idx > (NSInteger)self.cellViews.count) idx = (NSInteger)self.cellViews.count;
    [self.cellViews insertObject:v atIndex:(NSUInteger)idx];
    [self addSubview:v];
    [self relayout];
}

- (void)removeCellView:(NxCellView *)v {
    [v removeFromSuperview];
    [self.cellViews removeObject:v];
    [self relayout];
}

- (void)relayout {
    CGFloat w = self.bounds.size.width;
    CGFloat y = 0;
    for (NxCellView *v in self.cellViews) {
        CGFloat h = [v heightForWidth:w];
        v.frame = NSMakeRect(0, y, w, h);
        [v layout];
        y += h;
    }
    CGFloat oldH = self.frame.size.height;
    if (std::abs(oldH - y) > 0.5 || self.frame.size.width != w) {
        self.frame = NSMakeRect(0, 0, w, std::max<CGFloat>(y, 1));
        [self.superview setNeedsLayout:YES];
    }
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)dirty {
    [NxBg() setFill];
    NSRectFill(self.bounds);
}

- (NSInteger)insertionIndexForPoint:(NSPoint)p {
    for (NSInteger i = 0; i < (NSInteger)self.cellViews.count; i++) {
        NxCellView *v = self.cellViews[(NSUInteger)i];
        if (p.y < NSMidY(v.frame)) return i;
    }
    return (NSInteger)self.cellViews.count;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return NSDragOperationCopy;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSPoint p = [self convertPoint:sender.draggingLocation fromView:nil];
    NSInteger idx = [self insertionIndexForPoint:p];
    [self.controller acceptDropItems:sender.draggingPasteboard.pasteboardItems atIndex:idx];
    return YES;
}

@end

// ==================================================================
// 四角いウィンドウ部品
// ==================================================================
@interface NxSquareRowView : NSTableRowView
@end
@implementation NxSquareRowView
- (void)drawSelectionInRect:(NSRect)dirty {
    if (!self.selected) return;
    [[NxAccent() colorWithAlphaComponent:0.22] setFill];
    NSRectFill(self.bounds);
    [NxAccent() setFill];
    NSRectFill(NSMakeRect(0, 0, 2, self.bounds.size.height));
}
- (void)drawBackgroundInRect:(NSRect)dirty {
    [NxPanel() setFill];
    NSRectFill(self.bounds);
}
@end

@implementation NxTitleBar

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;
    self.label = [NSTextField labelWithString:@"neodax editor — necessary nodes only"];
    self.label.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold];
    self.label.textColor = NxText();
    self.label.frame = NSMakeRect(10, 8, frame.size.width - 120, 16);
    self.label.autoresizingMask = NSViewWidthSizable;
    [self addSubview:self.label];

    NxSquareButton *close = [[NxSquareButton alloc] initWithFrame:NSMakeRect(frame.size.width - 82, 4, 24, 22) title:@"×"];
    close.target = self;
    close.action = @selector(closeWindow:);
    close.autoresizingMask = NSViewMinXMargin;
    [self addSubview:close];

    NxSquareButton *mini = [[NxSquareButton alloc] initWithFrame:NSMakeRect(frame.size.width - 56, 4, 24, 22) title:@"−"];
    mini.target = self;
    mini.action = @selector(miniWindow:);
    mini.autoresizingMask = NSViewMinXMargin;
    [self addSubview:mini];

    NxSquareButton *zoom = [[NxSquareButton alloc] initWithFrame:NSMakeRect(frame.size.width - 30, 4, 24, 22) title:@"+"];
    zoom.target = self;
    zoom.action = @selector(zoomWindow:);
    zoom.autoresizingMask = NSViewMinXMargin;
    [self addSubview:zoom];
    return self;
}

- (void)drawRect:(NSRect)dirty {
    [NxPanel() setFill];
    NSRectFill(self.bounds);
    [NxLine() setFill];
    NSRectFill(NSMakeRect(0, self.bounds.size.height - 1, self.bounds.size.width, 1));
}

- (void)mouseDown:(NSEvent *)event {
    [self.hostWindow performWindowDragWithEvent:event];
}
- (void)closeWindow:(id)s { [self.hostWindow close]; }
- (void)miniWindow:(id)s { [self.hostWindow miniaturize:nil]; }
- (void)zoomWindow:(id)s { [self.hostWindow zoom:nil]; }

@end

@implementation NxResizeHandle

- (void)drawRect:(NSRect)dirty {
    [NxLine() setStroke];
    NSBezierPath *p = [NSBezierPath bezierPath];
    [p moveToPoint:NSMakePoint(2, 2)];
    [p lineToPoint:NSMakePoint(2, 10)];
    [p moveToPoint:NSMakePoint(6, 2)];
    [p lineToPoint:NSMakePoint(6, 6)];
    p.lineWidth = 1;
    [p stroke];
}

- (void)mouseDown:(NSEvent *)event {
    NSPoint start = [NSEvent mouseLocation];
    NSRect f = self.hostWindow.frame;
    while (YES) {
        NSEvent *e = [self.hostWindow nextEventMatchingMask:NSEventMaskLeftMouseDragged |
                                                        NSEventMaskLeftMouseUp];
        if (!e || e.type == NSEventTypeLeftMouseUp) break;
        NSPoint p = [NSEvent mouseLocation];
        CGFloat dx = p.x - start.x;
        CGFloat dy = p.y - start.y;
        NSRect nf = f;
        nf.size.width = std::max<CGFloat>(820, f.size.width + dx);
        nf.size.height = std::max<CGFloat>(520, f.size.height - dy);
        nf.origin.y = f.origin.y + (f.size.height - nf.size.height);
        [self.hostWindow setFrame:nf display:YES];
    }
}

@end

@implementation NxFileDragHandle

- (void)drawRect:(NSRect)dirty {
    [NxPanel() setFill];
    NSRectFill(self.bounds);
    [NxLine() setStroke];
    [[NSBezierPath bezierPathWithRect:NSInsetRect(self.bounds, 0.5, 0.5)] stroke];
    NSDictionary *a = @{NSForegroundColorAttributeName: NxText(),
                        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightBold]};
    [@"≡" drawAtPoint:NSMakePoint(6, 3) withAttributes:a];
}

- (void)mouseDragged:(NSEvent *)event {
    if (!self.path.length) return;
    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    [item setString:self.path forType:NxFilePasteboardType];
    NSDraggingItem *di = [[NSDraggingItem alloc] initWithPasteboardWriter:item];
    di.draggingFrame = NSMakeRect(0, 0, 40, 24);
    [self beginDraggingSessionWithItems:@[di] event:event source:self];
}

- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    return NSDragOperationCopy;
}

@end

@implementation NxSquareWindow

- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }

@end

// ==================================================================
// Controller 本体
// ==================================================================
@implementation NxEditorController

- (nxedit::Project *)project { return _project; }

- (void)applicationDidFinishLaunching:(NSNotification *)note {
    [self buildWindow];
    [self loadProject];
    [NSApp activateIgnoringOtherApps:YES];
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { return YES; }

- (void)buildWindow {
    NSRect frame = NSMakeRect(80, 80, 1280, 820);
    self.window = [[NxSquareWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskBorderless | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    self.window.backgroundColor = NxBg();
    self.window.hasShadow = YES;
    [self.window center];

    NxFlippedView *root = [[NxFlippedView alloc] initWithFrame:frame];
    root.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.window.contentView = root;

    self.titleBar = [[NxTitleBar alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, 30)];
    self.titleBar.hostWindow = self.window;
    self.titleBar.autoresizingMask = NSViewWidthSizable;
    [root addSubview:self.titleBar];

    NxResizeHandle *rh = [[NxResizeHandle alloc] initWithFrame:
        NSMakeRect(frame.size.width - 16, frame.size.height - 16, 16, 16)];
    rh.hostWindow = self.window;
    rh.autoresizingMask = NSViewMinXMargin | NSViewMaxYMargin;
    [root addSubview:rh];

    CGFloat top = 30;
    CGFloat H = frame.size.height - top;
    CGFloat leftW = 292, rightW = 390;
    CGFloat centerW = frame.size.width - leftW - rightW - 2;

    NxFlippedView *left = [[NxFlippedView alloc] initWithFrame:NSMakeRect(0, top, leftW, H)];
    left.autoresizingMask = NSViewHeightSizable;
    [root addSubview:left];

    NxFlippedView *center = [[NxFlippedView alloc] initWithFrame:
        NSMakeRect(leftW + 1, top, centerW, H)];
    center.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [root addSubview:center];

    NxFlippedView *right = [[NxFlippedView alloc] initWithFrame:
        NSMakeRect(frame.size.width - rightW, top, rightW, H)];
    right.autoresizingMask = NSViewMinXMargin | NSViewHeightSizable;
    [root addSubview:right];

    for (CGFloat x : {leftW, frame.size.width - rightW - 1}) {
        NxFlippedView *sep = [[NxFlippedView alloc] initWithFrame:NSMakeRect(x, top, 1, H)];
        sep.autoresizingMask = NSViewHeightSizable | ((x < 400) ? NSViewMaxXMargin : NSViewMinXMargin);
        sep.wantsLayer = YES;
        sep.layer.backgroundColor = NxLine().CGColor;
        [root addSubview:sep];
    }

    // ---- 左 ----
    self.searchBox = [[NxSquareField alloc] initWithFrame:NSMakeRect(10, 8, leftW - 20, 26)
                                               placeholder:@"検索セル (key / 関数 / ファイル)"];
    self.searchBox.field.delegate = self;
    self.searchBox.field.target = self;
    self.searchBox.field.action = @selector(searchChanged:);
    [left addSubview:self.searchBox];

    self.nodeTable = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, leftW - 20, H - 160)];
    self.nodeTable.dataSource = self;
    self.nodeTable.delegate = self;
    self.nodeTable.rowHeight = 46;
    self.nodeTable.headerView = nil;
    NSTableColumn *nc = [[NSTableColumn alloc] initWithIdentifier:@"node"];
    nc.width = leftW - 20;
    [self.nodeTable addTableColumn:nc];
    self.nodeScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(10, 40, leftW - 20, H - 160)];
    self.nodeScroll.hasVerticalScroller = NO;
    self.nodeScroll.hasHorizontalScroller = NO;
    self.nodeScroll.autohidesScrollers = YES;
    self.nodeScroll.documentView = self.nodeTable;
    self.nodeScroll.autoresizingMask = NSViewHeightSizable;
    self.nodeScroll.wantsLayer = YES;
    self.nodeScroll.layer.borderWidth = 1;
    self.nodeScroll.layer.borderColor = NxLine().CGColor;
    [left addSubview:self.nodeScroll];

    self.saveButton = [[NxSquareButton alloc] initWithFrame:NSMakeRect(10, H - 104, leftW - 20, 24)
                                                        title:@"保存 (Cmd+S)"];
    self.saveButton.target = self;
    self.saveButton.action = @selector(saveClicked:);
    [left addSubview:self.saveButton];

    self.fileBox = [[NxSquareField alloc] initWithFrame:NSMakeRect(10, H - 72, leftW - 20, 24)
                                             placeholder:@"ファイル番号で開く (例: 3)"];
    self.fileBox.field.delegate = self;
    self.fileBox.field.target = self;
    self.fileBox.field.action = @selector(fileBoxChanged:);
    [left addSubview:self.fileBox];
    self.status = [NSTextField labelWithString:@"読み込み中..."];
    self.status.font = [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular];
    self.status.textColor = NxDim();
    self.status.lineBreakMode = NSLineBreakByTruncatingTail;
    self.status.frame = NSMakeRect(10, H - 38, leftW - 20, 26);
    self.status.autoresizingMask = NSViewMaxYMargin;
    [left addSubview:self.status];

    // ---- 中央 ----
    self.stack = [[NxCellStackView alloc] initWithFrame:NSMakeRect(0, 0, centerW, H)];
    self.stack.controller = self;
    self.stack.autoresizingMask = NSViewWidthSizable;
    self.cellScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, centerW, H)];
    self.cellScroll.hasVerticalScroller = NO;
    self.cellScroll.hasHorizontalScroller = NO;
    self.cellScroll.autohidesScrollers = YES;
    self.cellScroll.drawsBackground = YES;
    self.cellScroll.backgroundColor = NxBg();
    self.cellScroll.documentView = self.stack;
    self.cellScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.cellScroll.wantsLayer = YES;
    self.cellScroll.layer.borderWidth = 1;
    self.cellScroll.layer.borderColor = NxLine().CGColor;
    [center addSubview:self.cellScroll];

    // ---- 右 ----
    self.fileLabel = [NSTextField labelWithString:@"元ファイル"];
    self.fileLabel.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightSemibold];
    self.fileLabel.textColor = NxText();
    self.fileLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.fileLabel.frame = NSMakeRect(8, 8, rightW - 48, 22);
    self.fileLabel.autoresizingMask = NSViewWidthSizable;
    [right addSubview:self.fileLabel];

    self.fileDrag = [[NxFileDragHandle alloc] initWithFrame:
        NSMakeRect(rightW - 34, 6, 26, 26)];
    self.fileDrag.autoresizingMask = NSViewMinXMargin;
    [right addSubview:self.fileDrag];

    self.lineTable = [[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, rightW, H - 40)];
    self.lineTable.dataSource = self;
    self.lineTable.delegate = self;
    self.lineTable.rowHeight = 16;
    self.lineTable.headerView = nil;
    self.lineTable.allowsMultipleSelection = YES;
    self.lineTable.allowsEmptySelection = YES;
    self.lineTable.usesAlternatingRowBackgroundColors = NO;
    NSTableColumn *lc = [[NSTableColumn alloc] initWithIdentifier:@"line"];
    lc.width = rightW;
    [self.lineTable addTableColumn:lc];
    [self.lineTable setDraggingSourceOperationMask:NSDragOperationCopy forLocal:YES];
    self.lineScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 40, rightW, H - 40)];
    self.lineScroll.hasVerticalScroller = NO;
    self.lineScroll.hasHorizontalScroller = NO;
    self.lineScroll.autohidesScrollers = YES;
    self.lineScroll.documentView = self.lineTable;
    self.lineScroll.autoresizingMask = NSViewHeightSizable;
    self.lineScroll.wantsLayer = YES;
    self.lineScroll.layer.borderWidth = 1;
    self.lineScroll.layer.borderColor = NxLine().CGColor;
    [right addSubview:self.lineScroll];

    [self.window makeKeyAndOrderFront:nil];
}

- (void)loadProject {
    _project = new nxedit::Project();
    _nodeAlive.clear();
    self.cells = [NSMutableArray array];

    std::string err;
    std::string map = self.mapPath.length ? self.mapPath.UTF8String : "";
    if (!_project->open(self.rootPath.UTF8String, map, err)) {
        self.status.stringValue = [NSString stringWithFormat:@"解析できない: %s", err.c_str()];
        self.loaded = NO;
        return;
    }
    _nodeAlive.assign(_project->nodes().size(), true);
    self.loaded = YES;
    [self reloadNodeTable];
    [self reloadFileBox];
    [self showFileId:1 line:1];

    // 必要なノードを最初からセルとして並べる。
    for (NSInteger i = 0; i < (NSInteger)_project->nodes().size(); i++) {
        if (_nodeAlive[(size_t)i]) [self appendNodeCell:i atIndex:-1];
    }
    self.titleBar.label.stringValue = [NSString stringWithFormat:
        @"neodax editor — %ld nodes / %ld files",
        (long)_project->nodes().size(), (long)_project->files().size()];
    self.status.stringValue = [NSString stringWithFormat:
        @"nodes=%ld files=%ld  Ctrl+Enter=元ファイル",
        (long)_project->nodes().size(), (long)_project->files().size()];
}

- (void)reloadNodeTable {
    _visibleNodeIndexes.clear();
    if (!_project) { [self.nodeTable reloadData]; return; }
    NSString *q = NxTrim(self.searchBox.textValue);
    std::string qs = q.length ? q.lowercaseString.UTF8String : "";
    for (NSInteger i = 0; i < (NSInteger)_project->nodes().size(); i++) {
        if (!_nodeAlive[(size_t)i]) continue;
        const nxedit::Node &n = _project->nodes()[(size_t)i];
        if (!qs.empty()) {
            std::string hay = n.key + "\t" + n.name + "\t" + n.file + "\t" +
                              std::to_string(n.line) + "\t" + n.contract;
            std::transform(hay.begin(), hay.end(), hay.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (hay.find(qs) == std::string::npos) continue;
        }
        _visibleNodeIndexes.push_back(i);
    }
    [self.nodeTable reloadData];
}

- (void)reloadFileBox {
    if (!_project) return;
    self.fileBox.field.placeholderString = [NSString stringWithFormat:
        @"ファイル番号 1-%ld で開く", (long)_project->files().size()];
}

- (void)saveClicked:(id)s {
    [self saveAll];
}

- (void)saveAllAction:(id)s {
    [self saveAll];
}

- (void)searchChanged:(id)s {
    [self reloadNodeTable];
}

- (void)fileBoxChanged:(id)s {
    NSString *v = NxTrim(self.fileBox.textValue);
    if (!v.length) return;
    NSInteger fid = v.integerValue;
    if (fid <= 0) {
        // "3\tpath" の形なら先頭の番号を使う。
        NSString *first = [v componentsSeparatedByString:@"\t"].firstObject;
        fid = first.integerValue;
    }
    if (fid > 0) [self showFileId:fid line:1];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)tv {
    if (tv == self.nodeTable) return (NSInteger)_visibleNodeIndexes.size();
    if (tv == self.lineTable) {
        if (!_project) return 0;
        const nxedit::SourceFile *f = _project->file_by_id((int)self.currentFileId);
        return f ? (NSInteger)f->lines.size() : 0;
    }
    return 0;
}

- (NSView *)tableView:(NSTableView *)tv viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    if (tv == self.nodeTable) {
        NSTableCellView *c = [tv makeViewWithIdentifier:@"ncell" owner:self];
        if (!c) {
            c = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 270, 46)];
            c.identifier = @"ncell";
            NSTextField *a = [[NSTextField alloc] initWithFrame:NSMakeRect(8, 24, 254, 16)];
            a.editable = NO; a.bordered = NO; a.drawsBackground = NO; a.tag = 1;
            a.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightSemibold];
            a.textColor = NxText();
            [c addSubview:a];
            NSTextField *b = [[NSTextField alloc] initWithFrame:NSMakeRect(8, 8, 254, 14)];
            b.editable = NO; b.bordered = NO; b.drawsBackground = NO; b.tag = 2;
            b.font = [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular];
            b.textColor = NxDim();
            [c addSubview:b];
            c.textField = a;
        }
        if (row < 0 || row >= (NSInteger)_visibleNodeIndexes.size()) return c;
        NSInteger idx = _visibleNodeIndexes[(size_t)row];
        const nxedit::Node &n = _project->nodes()[(size_t)idx];
        NSTextField *a = [c viewWithTag:1];
        NSTextField *b = [c viewWithTag:2];
        a.stringValue = [NSString stringWithFormat:@"%s  %s", n.key.c_str(), n.name.c_str()];
        b.stringValue = [NSString stringWithFormat:@"%s:%d  [%s]",
                         n.file.c_str(), n.line, n.confidence.c_str()];
        return c;
    }

    NSTableCellView *c = [tv makeViewWithIdentifier:@"lcell" owner:self];
    if (!c) {
        c = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 380, 16)];
        c.identifier = @"lcell";
        NSTextField *a = [[NSTextField alloc] initWithFrame:NSMakeRect(4, 0, 370, 16)];
        a.editable = NO; a.bordered = NO; a.drawsBackground = NO; a.tag = 1;
        a.font = [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular];
        a.textColor = NxText();
        a.lineBreakMode = NSLineBreakByClipping;
        [c addSubview:a];
        c.textField = a;
    }
    c.textField.stringValue = @"";
    if (_project) {
        const nxedit::SourceFile *f = _project->file_by_id((int)self.currentFileId);
        if (f && row >= 0 && row < (NSInteger)f->lines.size()) {
            c.textField.stringValue = [NSString stringWithFormat:@"%5ld │ %s",
                (long)row + 1, f->lines[(size_t)row].c_str()];
        }
    }
    return c;
}

- (NSTableRowView *)tableView:(NSTableView *)tv rowViewForRow:(NSInteger)row {
    NxSquareRowView *v = [[NxSquareRowView alloc] initWithFrame:NSMakeRect(0, 0, tv.bounds.size.width, tv.rowHeight)];
    return v;
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
    if (note.object != self.nodeTable) return;
    NSInteger row = self.nodeTable.selectedRow;
    if (row < 0 || row >= (NSInteger)_visibleNodeIndexes.size()) return;
    [self showNodeAtIndex:_visibleNodeIndexes[(size_t)row]];
}

- (id<NSPasteboardWriting>)tableView:(NSTableView *)tv
                pasteboardWriterForRow:(NSInteger)row {
    if (tv != self.lineTable || !_project) return nil;
    const nxedit::SourceFile *f = _project->file_by_id((int)self.currentFileId);
    if (!f) return nil;
    NSPasteboardItem *item = [[NSPasteboardItem alloc] init];
    NSString *v = [NSString stringWithFormat:@"%s\n%ld", f->path.c_str(), (long)row];
    [item setString:v forType:NxLinePasteboardType];
    return item;
}

- (void)showFileId:(NSInteger)fid line:(NSInteger)line {
    if (!_project) return;
    const nxedit::SourceFile *f = _project->file_by_id((int)fid);
    if (!f) return;
    self.currentFileId = fid;
    self.fileLabel.stringValue = [NSString stringWithFormat:@"[%d] %s",
                                  f->id, f->path.c_str()];
    self.fileDrag.path = [NSString stringWithUTF8String:f->path.c_str()];
    [self.lineTable reloadData];
    if (line > 0 && line <= (NSInteger)f->lines.size()) {
        [self.lineTable selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)(line - 1)]
                    byExtendingSelection:NO];
        [self.lineTable scrollRowToVisible:(NSInteger)(line - 1)];
    }
    [self.fileBox setTextValue:[NSString stringWithFormat:@"%d", f->id]];
}

- (NxEditorCell *)cellForNodeIndex:(NSInteger)idx {
    const nxedit::Node &n = _project->nodes()[(size_t)idx];
    int s = 0, e = 0;
    _project->node_range(n, s, e);
    NxEditorCell *c = [[NxEditorCell alloc] init];
    c.kind = NxCellNode;
    c.title = [NSString stringWithFormat:@"%s  %s", n.key.c_str(), n.name.c_str()];
    c.subtitle = [NSString stringWithFormat:@"%s:%d  body %d-%d  [%s]",
                  n.file.c_str(), n.line, s, e, n.confidence.c_str()];
    c.file = [NSString stringWithUTF8String:n.file.c_str()];
    c.startLine = s;
    c.endLine = e;
    c.nodeIndex = idx;
    c.originalText = [NSString stringWithFormat:@"%s",
        _project->range_text(n.file, s, e).c_str()];
    return c;
}

- (NxCellView *)appendNodeCell:(NSInteger)idx atIndex:(NSInteger)at {
    NxEditorCell *c = [self cellForNodeIndex:idx];
    return [self addCell:c atIndex:at];
}

- (NxCellView *)addCell:(NxEditorCell *)c atIndex:(NSInteger)at {
    if (at < 0) at = (NSInteger)self.cells.count;
    NxCellView *v = [[NxCellView alloc] initWithCell:c controller:self];
    [self.cells insertObject:c atIndex:(NSUInteger)at];
    [self.stack insertCellView:v atIndex:at];
    [self.stack relayout];
    return v;
}

- (void)scrollToCellView:(NxCellView *)v {
    [self.stack layoutSubtreeIfNeeded];
    [self.stack scrollRectToVisible:v.frame];
    [self.window makeFirstResponder:v.textView];
}

- (void)showNodeAtIndex:(NSInteger)idx {
    for (NxCellView *v in self.stack.cellViews) {
        if (v.cell.nodeIndex == idx && v.cell.kind == NxCellNode) {
            [self scrollToCellView:v];
            return;
        }
    }
    NxCellView *v = [self appendNodeCell:idx atIndex:-1];
    [self scrollToCellView:v];
}

- (void)focusCellFromTextView:(NxCodeTextView *)tv delta:(NSInteger)delta {
    NSInteger i = -1;
    for (NSInteger k = 0; k < (NSInteger)self.stack.cellViews.count; k++)
        if (self.stack.cellViews[(NSUInteger)k].textView == tv) { i = k; break; }
    if (i < 0) return;
    NSInteger j = i + delta;
    if (j < 0 || j >= (NSInteger)self.stack.cellViews.count) {
        NSBeep();
        return;
    }
    NxCellView *next = self.stack.cellViews[(NSUInteger)j];
    NSTextView *ntv = next.textView;
    NSRange sel = tv.selectedRange;
    NSInteger line = NxLineIndex(tv.string, sel.location);
    NSInteger col = (NSInteger)(sel.location - NxLineStart(tv.string, line));
    NSInteger targetLine = delta > 0 ? 0 : NxLineCount(ntv.string) - 1;
    NSInteger targetCol = std::min<NSInteger>(col, NxLineLength(ntv.string, targetLine));
    NSUInteger loc = NxLineStart(ntv.string, targetLine) + (NSUInteger)targetCol;
    [self.window makeFirstResponder:ntv];
    [ntv setSelectedRange:NSMakeRange(loc, 0)];
    [self.stack scrollRectToVisible:next.frame];
}

- (void)addSemanticCellForTextView:(NxCodeTextView *)tv above:(BOOL)above {
    if (!tv.cell || !_project) return;
    NSRange sel = tv.selectedRange;
    NSInteger lineInCell = NxLineIndex(tv.string, sel.location);
    NSInteger col = (NSInteger)(sel.location - NxLineStart(tv.string, lineInCell)) + 1;
    NSInteger line = tv.cell.startLine + lineInCell;
    const nxedit::Symbol *s = _project->symbol_for_position(tv.cell.file.UTF8String, (int)line, (int)col);
    if (!s) return;

    NxEditorCell *c = [[NxEditorCell alloc] init];
    c.kind = NxCellDerived;
    c.title = [NSString stringWithFormat:@"%s  (%s)", s->name.c_str(),
               s->kind == nxedit::Symbol::Function ? "function" : "class/block"];
    c.subtitle = [NSString stringWithFormat:@"%s:%d-%d  Esc で削除",
                  tv.cell.file.UTF8String, s->start_line, s->end_line];
    c.file = tv.cell.file;
    c.startLine = s->start_line;
    c.endLine = s->end_line;
    c.nodeIndex = -1;
    c.originalText = [NSString stringWithFormat:@"%s",
        _project->range_text(tv.cell.file.UTF8String, s->start_line, s->end_line).c_str()];

    NSInteger i = -1;
    for (NSInteger k = 0; k < (NSInteger)self.stack.cellViews.count; k++)
        if (self.stack.cellViews[(NSUInteger)k].textView == tv) { i = k; break; }
    if (i < 0) return;
    NSInteger at = above ? i : i + 1;
    NxCellView *v = [self addCell:c atIndex:at];
    [self scrollToCellView:v];
}

- (void)cellTextChanged:(NxCodeTextView *)tv {
    if (!tv.cell) return;
    [self.stack relayout];
    if (NxTrim(tv.string).length == 0) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (NxTrim(tv.string).length == 0) [self removeCellViewForTextView:tv];
        });
    }
}

- (void)textDidChange:(NSNotification *)note {
    if ([note.object isKindOfClass:[NxCodeTextView class]])
        [self cellTextChanged:(NxCodeTextView *)note.object];
}

- (void)removeCellViewForTextView:(NxCodeTextView *)tv {
    NSInteger i = -1;
    for (NSInteger k = 0; k < (NSInteger)self.stack.cellViews.count; k++)
        if (self.stack.cellViews[(NSUInteger)k].textView == tv) { i = k; break; }
    if (i < 0) return;
    NxCellView *v = self.stack.cellViews[(NSUInteger)i];
    NxEditorCell *c = v.cell;
    if (c.kind == NxCellNode && c.nodeIndex >= 0 && c.nodeIndex < (NSInteger)_nodeAlive.size()) {
        _nodeAlive[(size_t)c.nodeIndex] = false;
        [self reloadNodeTable];
    }
    [self.cells removeObject:c];
    [self.stack removeCellView:v];
    self.status.stringValue = @"セルを削除しました";
}

- (NSInteger)insertIndexForDrop:(NSPoint)p {
    NSPoint local = [self.stack convertPoint:p fromView:nil];
    return [self.stack insertionIndexForPoint:local];
}

- (void)acceptDropItems:(NSArray<NSPasteboardItem *> *)items atIndex:(NSInteger)idx {
    if (!_project || !items.count) return;
    NSString *path = nil;
    NSInteger minRow = NSIntegerMax, maxRow = -1;
    BOOL wholeFile = NO;
    for (NSPasteboardItem *item in items) {
        NSString *line = [item stringForType:NxLinePasteboardType];
        if (line.length) {
            NSRange r = [line rangeOfString:@"\n" options:NSBackwardsSearch];
            if (r.location != NSNotFound) {
                path = [line substringToIndex:r.location];
                NSInteger row = [[line substringFromIndex:r.location + 1] integerValue];
                minRow = std::min(minRow, row);
                maxRow = std::max(maxRow, row);
            }
        }
        NSString *f = [item stringForType:NxFilePasteboardType];
        if (f.length) { path = f; wholeFile = YES; }
    }
    if (!path.length) return;
    const nxedit::SourceFile *sf = _project->file_by_path(path.UTF8String);
    if (!sf) return;
    NSInteger a = wholeFile ? 1 : minRow + 1;
    NSInteger b = wholeFile ? (NSInteger)sf->lines.size() : maxRow + 1;
    if (a < 1) a = 1;
    if (b < a) return;

    NxEditorCell *c = [[NxEditorCell alloc] init];
    c.kind = NxCellOriginal;
    c.title = wholeFile ? [NSString stringWithFormat:@"元コード  %s (全体)", sf->path.c_str()]
                        : [NSString stringWithFormat:@"元コード  %s:%ld-%ld",
                           sf->path.c_str(), (long)a, (long)b];
    c.subtitle = @"ドラッグで追加したセル  Esc で削除";
    c.file = [NSString stringWithUTF8String:sf->path.c_str()];
    c.startLine = a;
    c.endLine = b;
    c.nodeIndex = -1;
    c.originalText = [NSString stringWithFormat:@"%s",
        _project->range_text(sf->path, (int)a, (int)b).c_str()];
    NxCellView *v = [self addCell:c atIndex:idx];
    [self scrollToCellView:v];
}

- (void)saveAll {
    if (!_project) return;
    NSMutableDictionary<NSString *, NSMutableArray<NSDictionary *> *> *byFile =
        [NSMutableDictionary dictionary];
    for (NxEditorCell *c in self.cells) {
        if (c.kind == NxCellOriginal) continue;
        if (!c.file.length || c.endLine < c.startLine) continue;
        NxCellView *v = nil;
        for (NxCellView *x in self.stack.cellViews)
            if (x.cell == c) { v = x; break; }
        if (!v) continue;
        NSString *text = v.textView.string;
        if ([text isEqualToString:c.originalText ?: @""]) continue;
        if (NxTrim(text).length == 0) continue;
        NSMutableArray *arr = byFile[c.file];
        if (!arr) { arr = [NSMutableArray array]; byFile[c.file] = arr; }
        [arr addObject:@{@"cell": c, @"start": @(c.startLine),
                         @"end": @(c.endLine), @"text": text}];
    }

    NSInteger saved = 0;
    NSMutableString *errors = [NSMutableString string];
    for (NSString *file in byFile) {
        NSMutableArray *arr = byFile[file];
        [arr sortUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
            return [b[@"start"] compare:a[@"start"]];
        }];
        NSInteger lastStart = NSIntegerMax;
        for (NSDictionary *d in arr) {
            NSInteger a = [d[@"start"] integerValue];
            NSInteger b = [d[@"end"] integerValue];
            NSString *text = d[@"text"];
            if (b >= lastStart) {
                [errors appendFormat:@"重なりをスキップ: %@:%ld-%ld\n", file, (long)a, (long)b];
                continue;
            }
            std::string err;
            if (!_project->replace_lines(file.UTF8String, (int)a, (int)b,
                                         text.UTF8String, err)) {
                [errors appendFormat:@"%s\n", err.c_str()];
                continue;
            }
            NSInteger newLines = NxLineCount(text);
            NSInteger delta = newLines - (b - a + 1);
            NxEditorCell *c = d[@"cell"];
            c.originalText = text;
            c.endLine = a + newLines - 1;
            for (NxEditorCell *other in self.cells) {
                if (other == c || ![other.file isEqualToString:file]) continue;
                if (other.startLine > b) {
                    other.startLine += delta;
                    other.endLine += delta;
                }
            }
            lastStart = a;
            saved++;
        }
        std::string err;
        if (!_project->write_file(file.UTF8String, err))
            [errors appendFormat:@"%s\n", err.c_str()];
    }
    [self showFileId:self.currentFileId line:1];
    self.status.stringValue = [NSString stringWithFormat:@"保存: %ld セル%@",
        (long)saved, errors.length ? [NSString stringWithFormat:@" / %@", errors] : @""];
}

@end

// ==================================================================
// main
// ==================================================================
static void build_menu(NxEditorController *ctl) {
    NSMenu *bar = [[NSMenu alloc] init];
    NSMenuItem *appItem = [[NSMenuItem alloc] init];
    [bar addItem:appItem];
    NSMenu *appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"保存" action:@selector(saveAllAction:) keyEquivalent:@"s"].target = ctl;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"終了" action:@selector(terminate:) keyEquivalent:@"q"].target = NSApp;
    appItem.submenu = appMenu;

    NSMenuItem *winItem = [[NSMenuItem alloc] init];
    [bar addItem:winItem];
    NSMenu *winMenu = [[NSMenu alloc] initWithTitle:@"ウィンドウ"];
    [winMenu addItemWithTitle:@"閉じる" action:@selector(performClose:) keyEquivalent:@"w"].target = nil;
    winItem.submenu = winMenu;
    NSApp.mainMenu = bar;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        NSString *root = nil, *map = nil;
        for (int i = 1; i < argc; i++) {
            if (!std::strcmp(argv[i], "--map") && i + 1 < argc) map = [NSString stringWithUTF8String:argv[++i]];
            else if (argv[i][0] != '-') root = [NSString stringWithUTF8String:argv[i]];
        }
        if (!root) root = [[NSFileManager defaultManager] currentDirectoryPath];

        // Finder 起動でも reviewer を見つけられるように、実行ファイルから
        // リポジトリ根を探して cwd を合わせる。見つからなければ現状維持。
        NSFileManager *fm = [NSFileManager defaultManager];
        if (![fm fileExistsAtPath:@"tools/java-review/neodax-java-review"]) {
            NSString *d = [[[NSBundle mainBundle] executablePath] stringByDeletingLastPathComponent];
            for (int i = 0; i < 6 && d.length > 1; i++) {
                NSString *probe = [d stringByAppendingPathComponent:@"tools/java-review/neodax-java-review"];
                if ([fm fileExistsAtPath:probe]) { chdir(d.UTF8String); break; }
                d = [d stringByDeletingLastPathComponent];
            }
        }

        NxEditorController *ctl = [[NxEditorController alloc] init];
        ctl.rootPath = root;
        ctl.mapPath = map;

        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        build_menu(ctl);
        app.delegate = ctl;
        [app run];
    }
    return 0;
}
