/*
 * neodax-viewer.mm — native NeoDAX viewer (Objective-C++ / AppKit, CPU Core Graphics)
 *
 * No web, no serialized format: the analyzer is linked in-process.
 * Shows a FUNCTION-LEVEL call graph with a 3-level hierarchy:
 *
 *   root  ->  role (behaviour group)  ->  ring (mutual-recursion SCC)  ->  function
 *
 *   click         select the top-level role under the cursor
 *   Tab           descend (role -> ring -> function)
 *   Shift+Tab     ascend  (function -> ring -> role -> root)
 *   Q / Shift+Q   next / previous sibling at the current level
 *
 *   Build:  bash gui/build.sh
 *   Run:    ./build_neodax/neodax-viewer
 */

#import <Cocoa/Cocoa.h>

extern "C" {
#include "integration/neodax_bridge.h"
}

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

/* ── palette ───────────────────────────────────────────────────── */

static const int kPaletteN = 11;
static const float kPalette[kPaletteN][3] = {
    {0.35f,0.72f,1.00f}, {0.36f,0.90f,0.55f}, {1.00f,0.60f,0.35f},
    {0.80f,0.55f,1.00f}, {1.00f,0.85f,0.35f}, {0.95f,0.45f,0.55f},
    {0.55f,1.00f,0.85f}, {0.70f,0.70f,0.78f}, {0.45f,0.55f,0.70f},
    {0.60f,0.60f,0.45f}, {0.45f,0.48f,0.52f}
};
static const char *kRoleName[kPaletteN] = {
    "Dispatcher","Worker","Sync","BeamSearch","Board","MoveGen",
    "Evaluator","Search","Decoder","Loader","misc"
};
static int rolePal(const std::string &r) {
    for (int i = 0; i < 10; i++) if (r == kRoleName[i]) return i;
    return 10;
}

/* ── view ──────────────────────────────────────────────────────── */

struct GNode { uint64_t addr; int role, ring; float x, y; std::string name; };
struct GEdge { int a, b; int origin; };

typedef void (^SelBlock)(int role, int ring, int func);
typedef void (^FnBlock)(int func);
@interface GraphView : NSView
@property (copy) SelBlock onSelect;
@property (copy) FnBlock onToggleCode;   /* fired on a click that hits a function point */
- (void)setNodes:(const std::vector<GNode> &)nodes edges:(const std::vector<GEdge> &)edges;
- (void)setMarks:(NSSet *)marks;   /* keys: f:0x.. / g:<role> / r:<role>:<ring> */
- (void)selectFunction:(int)idx;
- (void)selectRole:(int)role;
- (void)selectRing:(int)role ring:(int)ring;
@end

@implementation GraphView {
    std::vector<GNode> _nodes;
    std::vector<GEdge> _edges;
    std::vector<NSRect>  _roleBox;
    std::vector<int>     _roleRole;   // role index of each role box
    std::vector<NSRect>  _ringBox;
    std::vector<int>     _ringRole;   // role index of each ring box
    std::vector<int>     _ringId;     // ring id of each ring box
    std::vector<int>     _ringCount;  // member count (1 => just a point)
    std::vector<std::string> _ringName;
    NSSet *_marks;

    int _selRole, _selRing, _selFunc;   // -1 = not selected
    float _scale, _panx, _pany;
    NSPoint _last;
}
- (instancetype)initWithFrame:(NSRect)f {
    self = [super initWithFrame:f];
    if (self) { _scale = 2.0f; _panx = _pany = 0; _selRole = _selRing = _selFunc = -1; }
    return self;
}
- (BOOL)isFlipped { return NO; }
- (BOOL)acceptsFirstResponder { return YES; }

/* ---------- layout (computed once) ---------- */
- (void)layout {
    _roleBox.clear(); _roleRole.clear(); _ringBox.clear(); _ringRole.clear();
    _ringId.clear(); _ringCount.clear(); _ringName.clear();

    std::vector<std::vector<int>> roleMem(kPaletteN);
    for (size_t i = 0; i < _nodes.size(); i++)
        roleMem[_nodes[i].role < 0 ? 10 : _nodes[i].role].push_back((int)i);

    const float PAD = 1.6f, GAP = 3.0f, RGAP = 1.2f;
    float gx = 0, gy = 0, rowH = 0, WRAP = 34.0f;

    for (int g = 0; g < kPaletteN; g++) {
        if (roleMem[g].empty()) continue;

        /* distinct ring ids in this role, in order of appearance */
        std::vector<int> rings;
        for (int idx : roleMem[g])
            if (std::find(rings.begin(), rings.end(), _nodes[idx].ring) == rings.end())
                rings.push_back(_nodes[idx].ring);
        std::sort(rings.begin(), rings.end());

        /* local layout: one row per ring (y decreasing) */
        float localW = 0, localY = 0;
        struct Sub { int ring; NSRect r; std::vector<int> members; };
        std::vector<Sub> subs;
        for (int r : rings) {
            std::vector<int> m;
            for (int idx : roleMem[g]) if (_nodes[idx].ring == r) m.push_back(idx);
            int n = (int)m.size();
            for (int k = 0; k < n; k++) {
                _nodes[m[k]].x = 0 + k;
                _nodes[m[k]].y = localY;
            }
            float w = n + PAD * 2;
            NSRect box = NSMakeRect(0, localY - PAD, w, 1 + PAD * 2);
            subs.push_back({ r, box, m });
            if (w > localW) localW = w;
            localY -= (1 + PAD * 2 + RGAP);
        }
        float localH = -localY;

        if (gx + localW > WRAP && gx > 0) { gx = 0; gy -= rowH + GAP; rowH = 0; }

        /* offset role-local coords to global and record boxes */
        for (auto &s : subs) {
            for (int idx : s.members) { _nodes[idx].x += gx; _nodes[idx].y += gy; }
            NSRect r = s.r; r.origin.x += gx; r.origin.y += gy;
            _ringBox.push_back(r);
            _ringRole.push_back(g);
            _ringId.push_back(s.ring);
            _ringCount.push_back((int)s.members.size());
            char nm[48];
            if (s.ring < 0) snprintf(nm, sizeof(nm), "seq");
            else            snprintf(nm, sizeof(nm), "ring %d%s", s.ring,
                                    s.members.size() > 1 ? " *" : "");
            _ringName.push_back(nm);
        }
        _roleBox.push_back(NSMakeRect(gx, gy - localH, localW, localH));
        _roleRole.push_back(g);
        gx += localW + GAP;
        if (localH > rowH) rowH = localH;

        /* role label stored in _ringName? keep role boxes parallel */
    }
    /* global centring */
    float minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
    for (auto &n : _nodes) { minx = std::min(minx, n.x); maxx = std::max(maxx, n.x);
                             miny = std::min(miny, n.y); maxy = std::max(maxy, n.y); }
    float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
    for (auto &n : _nodes) { n.x -= cx; n.y -= cy; }
    for (auto &r : _roleBox) r.origin.x -= cx, r.origin.y -= cy;
    for (auto &r : _ringBox) r.origin.x -= cx, r.origin.y -= cy;
}

- (void)setNodes:(const std::vector<GNode> &)nodes edges:(const std::vector<GEdge> &)edges {
    _nodes = nodes; _edges = edges;
    _selRole = _selRing = _selFunc = -1;
    [self layout];
    [self setNeedsDisplay:YES];
}
- (void)setMarks:(NSSet *)marks { _marks = marks; [self setNeedsDisplay:YES]; }
- (BOOL)isMarkedFn:(uint64_t)addr {
    return [_marks containsObject:[NSString stringWithFormat:@"f:0x%llx", (unsigned long long)addr]];
}

/* ---------- helpers ---------- */
- (NSPoint)worldFromView:(NSPoint)p {
    float cx = (float)self.bounds.size.width * 0.5f + _panx;
    float cy = (float)self.bounds.size.height * 0.5f + _pany;
    return NSMakePoint((p.x - cx) / _scale, (p.y - cy) / _scale);
}
- (int)nodeNearWorld:(NSPoint)w {
    int best = -1; float bd = 6.0f;
    for (size_t i = 0; i < _nodes.size(); i++) {
        float dx = _nodes[i].x - (float)w.x, dy = _nodes[i].y - (float)w.y;
        float d = dx*dx + dy*dy;
        if (d < bd) { bd = d; best = (int)i; }
    }
    return best;
}
- (int)roleOfCursor {
    NSPoint w = [self worldFromView:[self convertPoint:self.window.mouseLocationOutsideOfEventStream fromView:nil]];
    int i = [self nodeNearWorld:w];
    return i >= 0 ? _nodes[i].role : -1;
}
- (std::vector<int>)ringsInRole:(int)role {
    std::vector<int> v;
    for (size_t i = 0; i < _ringRole.size(); i++)
        if (_ringRole[i] == role && std::find(v.begin(), v.end(), _ringId[i]) == v.end())
            v.push_back(_ringId[i]);
    return v;
}
- (int)nodeInRing:(int)role ring:(int)ring {
    for (size_t i = 0; i < _nodes.size(); i++)
        if (_nodes[i].role == role && _nodes[i].ring == ring) return (int)i;
    return -1;
}

/* ---------- selection ---------- */
- (void)descend {
    if (_selRole < 0) {
        int r = [self roleOfCursor];
        if (r < 0 && !_nodes.empty()) r = _nodes[0].role;
        _selRole = r; _selRing = -1; _selFunc = -1;
    } else if (_selRing < 0) {
        std::vector<int> rr = [self ringsInRole:_selRole];
        if (!rr.empty()) { _selRing = rr[0]; _selFunc = -1; }
    } else if (_selFunc < 0) {
        _selFunc = [self nodeInRing:_selRole ring:_selRing];
    }
    if (self.onSelect) self.onSelect(_selRole, _selRing, _selFunc);
}
- (void)ascend {
    if (_selFunc >= 0) _selFunc = -1;
    else if (_selRing >= 0) _selRing = -1;
    else if (_selRole >= 0) _selRole = -1;
    if (self.onSelect) self.onSelect(_selRole, _selRing, _selFunc);
}
- (void)cycleSibling:(int)dir {
    if (_selRole < 0) return;
    if (_selRing < 0) {
        std::vector<int> roles;
        for (int g = 0; g < kPaletteN; g++)
            for (size_t i = 0; i < _nodes.size(); i++) if (_nodes[i].role == g) { roles.push_back(g); break; }
        if (roles.size() < 2) return;
        int idx = 0; for (size_t k = 0; k < roles.size(); k++) if (roles[k] == _selRole) idx = (int)k;
        _selRole = roles[((idx + dir) % (int)roles.size() + (int)roles.size()) % (int)roles.size()];
    } else if (_selFunc < 0) {
        std::vector<int> rr = [self ringsInRole:_selRole];
        if (rr.size() < 2) return;
        int idx = 0; for (size_t k = 0; k < rr.size(); k++) if (rr[k] == _selRing) idx = (int)k;
        _selRing = rr[((idx + dir) % (int)rr.size() + (int)rr.size()) % (int)rr.size()];
    } else {
        std::vector<int> mem;
        for (size_t i = 0; i < _nodes.size(); i++)
            if (_nodes[i].role == _selRole && _nodes[i].ring == _selRing) mem.push_back((int)i);
        if (mem.size() < 2) return;
        int idx = 0; for (size_t k = 0; k < mem.size(); k++) if (mem[k] == _selFunc) idx = (int)k;
        _selFunc = mem[((idx + dir) % (int)mem.size() + (int)mem.size()) % (int)mem.size()];
    }
    if (self.onSelect) self.onSelect(_selRole, _selRing, _selFunc);
}
- (void)keyDown:(NSEvent *)e {
    unichar c = e.charactersIgnoringModifiers.length ? [e.charactersIgnoringModifiers characterAtIndex:0] : 0;
    BOOL shift = (e.modifierFlags & NSEventModifierFlagShift) != 0;
    if (c == 0x09 || [e keyCode] == 48) { if (shift) [self ascend]; else [self descend]; [self setNeedsDisplay:YES]; }
    else if (c == 'q' || c == 'Q') { [self cycleSibling:shift ? -1 : 1]; [self setNeedsDisplay:YES]; }
    else [super keyDown:e];
}

/* ---------- draw ---------- */
- (void)drawRect:(NSRect)dirty {
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    CGContextSetRGBFillColor(ctx, 0.05, 0.07, 0.09, 1.0);
    CGContextFillRect(ctx, self.bounds);
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, self.bounds.size.width * 0.5 + _panx,
                               self.bounds.size.height * 0.5 + _pany);
    CGContextScaleCTM(ctx, _scale, _scale);

    /* edges */
    for (size_t i = 0; i < _edges.size(); i++) {
        const GNode &a = _nodes[_edges[i].a], &b = _nodes[_edges[i].b];
        bool vis = (_selRole < 0) || (a.role == _selRole);
        CGContextSetRGBStrokeColor(ctx, 0.3f, 0.35f, 0.45f, vis ? 0.4f : 0.12f);
        CGContextSetLineWidth(ctx, 1.0f / _scale);
        CGContextMoveToPoint(ctx, a.x, a.y);
        CGContextAddLineToPoint(ctx, b.x, b.y);
        CGContextStrokePath(ctx);
    }

    /* rings: hollow circles (filled would hide the points and edges) */
    for (size_t i = 0; i < _ringBox.size(); i++) {
        if (_ringCount[i] <= 1) continue;         /* singleton ring => just a point */
        NSRect b = _ringBox[i];
        float cx = b.origin.x + b.size.width * 0.5f;
        float cy = b.origin.y + b.size.height * 0.5f;
        float rr = 0.5f * sqrtf(b.size.width * b.size.width + b.size.height * b.size.height) + 0.15f;
        bool on = (_selRole == _ringRole[i] && _selRing == _ringId[i]);
        bool mk = [_marks containsObject:[NSString stringWithFormat:@"r:%d:%d", _ringRole[i], _ringId[i]]];
        if (mk) CGContextSetRGBStrokeColor(ctx, 1.0f, 1.0f, 1.0f, 0.95f);
        else CGContextSetRGBStrokeColor(ctx, on ? 1.0f : 0.62f, on ? 0.90f : 0.72f,
                                        on ? 0.50f : 0.85f, on ? 0.95f : 0.55f);
        CGContextSetLineWidth(ctx, (on ? 2.5f : 1.2f) / _scale);
        CGContextStrokeEllipseInRect(ctx, CGRectMake(cx - rr, cy - rr, 2 * rr, 2 * rr));
        CGContextSetLineWidth(ctx, 1.0f / _scale);
        CGContextStrokeEllipseInRect(ctx, CGRectMake(cx - rr + 0.35f, cy - rr + 0.35f,
                                                     2 * rr - 0.7f, 2 * rr - 0.7f));
    }
    /* roles: a coloured spine line */
    NSMutableDictionary *lat = [@{ NSForegroundColorAttributeName:[NSColor colorWithCalibratedWhite:0.85 alpha:1],
                                   NSFontAttributeName:[NSFont monospacedSystemFontOfSize:10.0f / _scale weight:NSFontWeightRegular] } mutableCopy];
    for (size_t i = 0; i < _roleBox.size(); i++) {
        NSRect b = _roleBox[i];
        int g = _roleRole[i];
        bool on = ((int)g == _selRole);
        const float *p = kPalette[g < 0 ? 10 : g];
        if ([_marks containsObject:[NSString stringWithFormat:@"g:%d", g]])
            CGContextSetRGBStrokeColor(ctx, 1.0f, 1.0f, 1.0f, 1.0f);
        else
            CGContextSetRGBStrokeColor(ctx, p[0], p[1], p[2], on ? 1.0f : 0.75f);
        CGContextSetLineWidth(ctx, (on ? 3.0f : 1.5f) / _scale);
        CGContextMoveToPoint(ctx, b.origin.x, b.origin.y + b.size.height);
        CGContextAddLineToPoint(ctx, b.origin.x + b.size.width, b.origin.y + b.size.height);
        CGContextStrokePath(ctx);
        NSString *s = [NSString stringWithUTF8String:kRoleName[g < 0 ? 10 : g]];
        [s drawAtPoint:NSMakePoint(b.origin.x, b.origin.y + b.size.height + 0.2f) withAttributes:lat];
    }

    /* Function points: progressive disclosure. Default is rings only; the
     * points of the OPEN ring are shown (and stay shown while you move across
     * siblings) until the ring is closed by ascending. */
    if (_selRing >= 0 || _selFunc >= 0) {
        for (size_t i = 0; i < _nodes.size(); i++) {
            const GNode &n = _nodes[i];
            if (!(n.role == _selRole && n.ring == _selRing)) continue;
            if ([self isMarkedFn:n.addr])
                CGContextSetRGBFillColor(ctx, 1.0f, 1.0f, 1.0f, 1.0f);   /* marked */
            else {
                const float *p = kPalette[n.role < 0 ? 10 : n.role];
                CGContextSetRGBFillColor(ctx, p[0], p[1], p[2], 1.0f);
            }
            CGContextFillRect(ctx, CGRectMake(n.x - 0.55, n.y - 0.55, 1.1, 1.1));
            if ((int)i == _selFunc) {
                CGContextSetRGBStrokeColor(ctx, 1, 1, 1, 1);
                CGContextSetLineWidth(ctx, 2.5f / _scale);
                CGContextStrokeRect(ctx, CGRectMake(n.x - 1.3, n.y - 1.3, 2.6, 2.6));
            }
        }
    }
    CGContextRestoreGState(ctx);
}

- (void)selectRole:(int)role {
    _selRole = role; _selRing = -1; _selFunc = -1;
    _panx = _pany = 0;
    if (self.onSelect) self.onSelect(_selRole, _selRing, _selFunc);
    [self setNeedsDisplay:YES];
}
- (void)selectRing:(int)role ring:(int)ring {
    _selRole = role; _selRing = ring; _selFunc = -1;
    for (size_t i = 0; i < _ringBox.size(); i++)
        if (_ringRole[i] == role && _ringId[i] == ring) {
            float cx = _ringBox[i].origin.x + _ringBox[i].size.width * 0.5f;
            float cy = _ringBox[i].origin.y + _ringBox[i].size.height * 0.5f;
            _panx = -cx * _scale; _pany = -cy * _scale;
            break;
        }
    if (self.onSelect) self.onSelect(_selRole, _selRing, _selFunc);
    [self setNeedsDisplay:YES];
}
- (void)selectFunction:(int)idx {
    if (idx < 0 || idx >= (int)_nodes.size()) return;
    _selRole = _nodes[idx].role;
    _selRing = _nodes[idx].ring;
    _selFunc = idx;
    _panx = -_nodes[idx].x * _scale;      /* centre the node */
    _pany = -_nodes[idx].y * _scale;
    if (self.onSelect) self.onSelect(_selRole, _selRing, _selFunc);
    [self setNeedsDisplay:YES];
}

- (void)scrollWheel:(NSEvent *)e {
    _scale *= (1.0f + (float)e.scrollingDeltaY * 0.01f);
    if (_scale < 0.1f) _scale = 0.1f;
    if (_scale > 30.0f) _scale = 30.0f;
    [self setNeedsDisplay:YES];
}
- (void)mouseDown:(NSEvent *)e {
    NSPoint v = [self convertPoint:e.locationInWindow fromView:nil];
    NSPoint w = [self worldFromView:v];
    int i = (_selRing >= 0) ? [self nodeNearWorld:w] : -1;   /* points are visible only when a ring is open */
    if (i >= 0) {
        _selFunc = i;                                        /* hit a function point */
        if (self.onToggleCode) self.onToggleCode(i);
    } else {
        int j = [self nodeNearWorld:w];
        _selRole = (j >= 0) ? _nodes[j].role : -1;           /* click selects top-level role */
        _selRing = -1; _selFunc = -1;
    }
    _last = v;
    [self.window makeFirstResponder:self];
    if (self.onSelect) self.onSelect(_selRole, _selRing, _selFunc);
    [self setNeedsDisplay:YES];
}
- (void)mouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    _panx += (float)(p.x - _last.x); _pany += (float)(p.y - _last.y); _last = p;
    [self setNeedsDisplay:YES];
}
@end

/* ── app ───────────────────────────────────────────────────────── */

static NSString *supportDir() {
    NSArray *p = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString *d = [[p firstObject] stringByAppendingPathComponent:@"NeoDAX"];
    [[NSFileManager defaultManager] createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil];
    return d;
}

@interface AppDelegate : NSObject <NSApplicationDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property (strong) NSWindow    *window;
@property (strong) NSTextField *pathField;
@property (strong) NSTextView  *flowView;
@property (strong) GraphView   *graph;
@property (strong) NSTableView *table;
@property (strong) NSScrollView  *tableScroll;
@property (strong) NSScrollView  *codeScroll;
@property (strong) NSTextField   *status;
@property (strong) NSString      *workflowAll;   // full generated workflow
@property (nonatomic) BOOL        codeVisible;
@property (nonatomic) int         viewMode;      // 0 code, 1 asm, 2 both (not persisted)
@property (strong) NSMutableSet  *marked;        // marked keys: f:/g:/r: (persisted)
@property (strong) NSButton      *viewBtn;
@property (strong) NSButton      *curBtn;      // "currently selected" window (click => mark)
@property (strong) NSString      *curMarkKey;  // key of the current role/ring (nil if none)
@property (nonatomic) int          curFunc;     // currently selected function
@property (nonatomic) BOOL         analyzing;
@property (strong) NSMutableArray *fns;       // {name, role, roleIdx, ring, address}
@property (strong) NSMutableArray *edges;     // {from, to}
@property (strong) NSMutableArray *paneRows;  // inspector rows {kind, name, info}
@property (strong) NSString       *lastPath;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1200, 780)
        styleMask:(NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    self.window.title = @"NeoDAX Viewer"; [self.window center];
    NSView *cv = self.window.contentView;

    self.pathField = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 740, 880, 26)];
    self.pathField.placeholderString = @"/path/to/Mach-O binary or .app";
    [cv addSubview:self.pathField];
    NSButton *go = [[NSButton alloc] initWithFrame:NSMakeRect(900, 738, 130, 30)];
    go.title = @"Analyze"; go.bezelStyle = NSBezelStyleRounded;
    go.target = self; go.action = @selector(onAnalyze:); [cv addSubview:go];
    NSButton *open = [[NSButton alloc] initWithFrame:NSMakeRect(1038, 738, 150, 30)];
    open.title = @"Open .app…"; open.bezelStyle = NSBezelStyleRounded;
    open.target = self; open.action = @selector(onOpen:); [cv addSubview:open];

    self.viewBtn = [[NSButton alloc] initWithFrame:NSMakeRect(560, 738, 150, 30)];
    self.viewBtn.title = @"View: Code"; self.viewBtn.bezelStyle = NSBezelStyleRounded;
    self.viewBtn.target = self; self.viewBtn.action = @selector(onViewMode:);
    [cv addSubview:self.viewBtn];

    self.status = [[NSTextField alloc] initWithFrame:NSMakeRect(12, 714, 1176, 20)];
    self.status.editable = NO; self.status.bordered = NO; self.status.bezeled = NO;
    self.status.stringValue = @"Tab:下位  Shift+Tab:上位  Q:同階層  クリック:最上位を選択";
    [cv addSubview:self.status];

    self.graph = [[GraphView alloc] initWithFrame:NSMakeRect(12, 268, 780, 440)];
    [cv addSubview:self.graph];

    self.curBtn = [[NSButton alloc] initWithFrame:NSMakeRect(800, 682, 388, 26)];
    self.curBtn.title = @"現在選択中"; self.curBtn.bezelStyle = NSBezelStyleRounded;
    self.curBtn.target = self; self.curBtn.action = @selector(onMarkCurrent:);
    self.curBtn.hidden = YES;
    [cv addSubview:self.curBtn];

    NSScrollView *tv = [[NSScrollView alloc] initWithFrame:NSMakeRect(800, 268, 388, 410)];
    self.tableScroll = tv;
    tv.hasVerticalScroller = YES; tv.borderType = NSBezelBorder;
    self.table = [[NSTableView alloc] initWithFrame:tv.bounds];
    for (NSString *t in @[@"kind", @"name", @"info"]) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:t];
        c.title = t; [self.table addTableColumn:c];
    }
    self.table.dataSource = self; self.table.delegate = self;
    self.table.target = self; self.table.action = @selector(onRowClick:);
    tv.documentView = self.table; [cv addSubview:tv];

    NSScrollView *fv = [[NSScrollView alloc] initWithFrame:NSMakeRect(12, 12, 1176, 248)];
    self.codeScroll = fv;
    fv.hasVerticalScroller = YES; fv.borderType = NSBezelBorder;
    self.flowView = [[NSTextView alloc] initWithFrame:fv.bounds];
    self.flowView.editable = NO;
    self.flowView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    fv.documentView = self.flowView; [cv addSubview:fv];

    self.codeVisible = NO;
    [self setCodeVisible:NO];

    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.graph];
    [NSApp activateIgnoringOtherApps:YES];

    self.lastPath = [[NSUserDefaults standardUserDefaults] stringForKey:@"lastPath"];
    if (self.lastPath.length && [[NSFileManager defaultManager] fileExistsAtPath:self.lastPath]) {
        self.pathField.stringValue = self.lastPath;
        [self analyzePath:self.lastPath];
    }
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)s { (void)s; return YES; }
- (void)onOpen:(id)s {
    (void)s;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseFiles = YES; p.canChooseDirectories = YES; p.allowsMultipleSelection = NO;
    if ([p runModal] == NSModalResponseOK) {
        NSString *path = p.URL.path;
        if ([path.pathExtension isEqualToString:@"app"]) {
            NSBundle *b = [NSBundle bundleWithPath:path];
            if (b.executablePath) path = b.executablePath;
        }
        self.pathField.stringValue = path;
    }
}
- (void)onAnalyze:(id)s { (void)s; [self analyzePath:self.pathField.stringValue]; }

- (void)analyzePath:(NSString *)path {
    if (path.length == 0 || self.analyzing) return;
    self.analyzing = YES;
    self.status.stringValue = [NSString stringWithFormat:@"analyzing %@ …", path];

    /* Heavy work runs OFF the main thread so the window stays responsive. */
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSData *data = [NSData dataWithContentsOfFile:path];
        if (!data) {
            dispatch_async(dispatch_get_main_queue(), ^{
                self.analyzing = NO; self.status.stringValue = @"cannot read file";
            });
            return;
        }
        integrated_binary_t ibin;
        if (neodax_bridge_init(&ibin, (const uint8_t *)data.bytes, (size_t)data.length) != 0) {
            dispatch_async(dispatch_get_main_queue(), ^{
                self.analyzing = NO; self.status.stringValue = @"not a Mach-O binary";
            });
            return;
        }
        static_analysis_opts_t opts; memset(&opts, 0, sizeof(opts));
        opts.num_threads = 4;
        FILE *devnull = fopen("/dev/null", "w");
        neodax_run_static_analysis(&ibin, &opts, devnull ? devnull : stderr);

        macho_binary_t *bin = &ibin.macho_bin;
        depflow_graph_t *g = ibin.dep_graph;
        if (!g) {
            neodax_bridge_free(&ibin); if (devnull) fclose(devnull);
            dispatch_async(dispatch_get_main_queue(), ^{
                self.analyzing = NO; self.status.stringValue = @"no graph";
            });
            return;
        }

        int nf = bin->nfunctions;
        std::vector<GNode> nodes(nf);
        NSMutableArray *fns = [NSMutableArray array];
        for (int i = 0; i < nf; i++) {
            char role[64];
            if (!depflow_function_role(bin, g, i, role, sizeof(role))) snprintf(role, sizeof(role), "misc");
            nodes[i].addr = bin->functions[i].start;
            nodes[i].role = rolePal(role);
            nodes[i].ring = (g->summaries && i < g->summary_count) ? g->summaries[i].ring_id : -1;
            nodes[i].name = bin->functions[i].name;

            NSMutableArray *argd = [NSMutableArray array];
            NSMutableArray *outd = [NSMutableArray array];
            int nargs = 0; int reto = 0; int recursive = 0;
            if (g->summaries && i < g->summary_count) {
                depflow_func_summary_t *sm = &g->summaries[i];
                nargs = sm->nargs; recursive = sm->is_recursive;
                { int bestc = -1;
                  for (int o = 0; o < sm->nouts; o++)
                      if (sm->outs[o].kind == DEPFLOW_VOUT_RET &&
                          sm->outs[o].origin != DEPFLOW_ORIGIN_UNKNOWN &&
                          sm->outs[o].confidence > bestc) {
                          reto = sm->outs[o].origin; bestc = sm->outs[o].confidence;
                      } }
                for (int a = 0; a < sm->nargs; a++) {
                    depflow_varg_t *ar = &sm->args[a];
                    char nm[96], info[160];
                    snprintf(nm, sizeof(nm), "arg%d  reg=%d  %s", a, ar->reg, depflow_origin_str(ar->origin));
                    info[0] = 0;
                    for (int c = 0; c < ar->nconds; c++) {
                        depflow_condition_t *cd = &ar->conds[c];
                        char one[96];
                        snprintf(one, sizeof(one), "%s %s %s", depflow_cond_str(cd->kind),
                                 cd->state ? "then" : "now", cd->tag);
                        strncat(info, one, sizeof(info) - strlen(info) - 1);
                        if (c + 1 < ar->nconds) strncat(info, "; ", sizeof(info) - strlen(info) - 1);
                    }
                    [argd addObject:@{ @"name": [NSString stringWithUTF8String:nm],
                                       @"info": [NSString stringWithUTF8String:info] }];
                }
                for (int o = 0; o < sm->nouts; o++) {
                    depflow_vout_t *ou = &sm->outs[o];
                    char nm[160], info[192];
                    const char *kind = ou->kind == DEPFLOW_VOUT_RET ? "ret"
                                     : ou->kind == DEPFLOW_VOUT_GLOBAL ? "global" : "sideeffect";
                    snprintf(nm, sizeof(nm), "out  %s  %s  0x%llx",
                             depflow_origin_str(ou->origin), kind, (unsigned long long)ou->address);
                    info[0] = 0;
                    for (int c = 0; c < ou->nconds; c++) {
                        depflow_condition_t *cd = &ou->conds[c];
                        char one[112];
                        snprintf(one, sizeof(one), "%s %s %s", depflow_cond_str(cd->kind),
                                 cd->state ? "then" : "now", cd->tag);
                        if (cd->target_func >= 0) { char h[24]; snprintf(h, sizeof(h), " ->f%d", cd->target_func); strncat(one, h, sizeof(one)-strlen(one)-1); }
                        strncat(info, one, sizeof(info) - strlen(info) - 1);
                        if (c + 1 < ou->nconds) strncat(info, "; ", sizeof(info) - strlen(info) - 1);
                    }
                    [outd addObject:@{ @"name": [NSString stringWithUTF8String:nm],
                                       @"info": [NSString stringWithUTF8String:info] }];
                }
            }

            [fns addObject:@{ @"name": [NSString stringWithUTF8String:bin->functions[i].name],
                              @"role": [NSString stringWithUTF8String:role],
                              @"roleIdx": @(nodes[i].role),
                              @"ring": @(nodes[i].ring),
                              @"recursive": @(recursive),
                              @"nargs": @(nargs),
                              @"ret": [NSString stringWithUTF8String:depflow_origin_str(reto)],
                              @"argd": argd,
                              @"outd": outd,
                              @"address": [NSString stringWithFormat:@"0x%llx", (unsigned long long)nodes[i].addr] }];
        }

        std::unordered_map<uint64_t,int> byAddr;
        for (int i = 0; i < nf; i++) byAddr[nodes[i].addr] = i;
        std::vector<GEdge> edges;
        NSMutableArray *edgeArr = [NSMutableArray array];
        for (int i = 0; i < nf; i++) {
            if (!g->summaries || i >= g->summary_count) continue;
            depflow_func_summary_t *sm = &g->summaries[i];
            for (int o = 0; o < sm->nouts; o++)
                if (sm->outs[o].origin == DEPFLOW_ORIGIN_CALLEE_RET) {
                    auto it = byAddr.find(sm->outs[o].address);
                    if (it != byAddr.end()) {
                        edges.push_back({ i, it->second, 5 });
                        [edgeArr addObject:@{ @"from": @(i), @"to": @(it->second) }];
                    }
                }
        }

        /* per-function assembly, bounded by the NEXT function start when the
         * function end is unknown (never bleeds into a neighbour) */
        if (bin->insns && bin->ninsns > 0) {
            std::vector<uint64_t> starts(nf);
            for (int i = 0; i < nf; i++) starts[i] = bin->functions[i].start;
            std::sort(starts.begin(), starts.end());
            for (int i = 0; i < nf; i++) {
                uint64_t s0 = bin->functions[i].start, e0 = bin->functions[i].end;
                if (!(e0 > s0)) {
                    e0 = UINT64_MAX;
                    for (size_t k = 0; k < starts.size(); k++)
                        if (starts[k] > s0 && starts[k] < e0) e0 = starts[k];
                }
                NSMutableString *a = [NSMutableString string];
                for (int k = 0; k < bin->ninsns; k++) {
                    macho_insn_t *in = &bin->insns[k];
                    if (in->address < s0 || in->address >= e0) continue;
                    [a appendFormat:@"0x%llx  %-12s %s\n",
                        (unsigned long long)in->address, in->mnemonic, in->operands];
                }
                [fns[i] setValue:a forKey:@"asm"];
            }
        }

        NSString *wf = [NSTemporaryDirectory() stringByAppendingPathComponent:@"neodax.workflow.hpp"];
        depflow_export_workflow(bin, g, wf.UTF8String);
        NSString *workflowAll = [NSString stringWithContentsOfFile:wf encoding:NSUTF8StringEncoding error:nil] ?: @"";
        int rings = g->ring_count, gnodes = g->node_count, gedges = g->edge_count;

        neodax_bridge_free(&ibin);
        if (devnull) fclose(devnull);

        dispatch_async(dispatch_get_main_queue(), ^{
            self.fns = fns;
            self.edges = edgeArr;
            self.workflowAll = workflowAll;

            AppDelegate *__weak ws = self;
            self.graph.onSelect = ^(int role, int ring, int func) {
                [ws updatePaneRole:role ring:ring func:func];
                ws.curFunc = func;
                if (func < 0) [ws setCodeVisible:NO];
                else if (ws.codeVisible) [ws showCodeFor:func];
            };
            self.graph.onToggleCode = ^(int func) {
                if (func < 0) return;
                if (ws.codeVisible) [ws setCodeVisible:NO];
                else { [ws showCodeFor:func]; [ws setCodeVisible:YES]; }
                NSString *key = [NSString stringWithFormat:@"f:%@", ws.fns[func][@"address"]];
                if ([ws.marked containsObject:key]) [ws.marked removeObject:key];
                else [ws.marked addObject:key];
                [ws saveMarks];
                [ws.graph setMarks:ws.marked];
            };

            [self.graph setNodes:nodes edges:edges];
            [self setCodeVisible:NO];

            NSString *mk = [@"marks:" stringByAppendingString:path];
            NSArray *saved = [[NSUserDefaults standardUserDefaults] arrayForKey:mk] ?: @[];
            self.marked = [NSMutableSet setWithArray:saved];
            [self.graph setMarks:self.marked];

            [self updatePaneRole:-1 ring:-1 func:-1];
            self.status.stringValue = [NSString stringWithFormat:
                @"%@  ·  %d functions  ·  %d rings  ·  %d nodes / %d edges      Tab:下位  Shift+Tab:上位  Q:同階層",
                path, nf, rings, gnodes, gedges];
            [self.table reloadData];
            self.lastPath = path;
            [[NSUserDefaults standardUserDefaults] setObject:path forKey:@"lastPath"];
            self.analyzing = NO;
        });
    });
}

/* ---- code pane ---- */
- (void)setCodeVisible:(BOOL)v {
    _codeVisible = v;
    self.codeScroll.hidden = !v;
    if (v) {
        self.graph.frame       = NSMakeRect(12, 268, 780, 440);
        self.tableScroll.frame = NSMakeRect(800, 268, 388, 410);
        self.curBtn.frame      = NSMakeRect(800, 682, 388, 26);
        self.codeScroll.frame  = NSMakeRect(12, 12, 1176, 248);
    } else {
        self.graph.frame       = NSMakeRect(12, 12, 780, 696);
        self.tableScroll.frame = NSMakeRect(800, 12, 388, 666);
        self.curBtn.frame      = NSMakeRect(800, 682, 388, 26);
        self.codeScroll.frame  = NSMakeRect(12, 12, 1176, 0);
    }
    [self.graph setNeedsDisplay:YES];
}

static NSString *extractFuncCode(NSString *all, NSString *addr) {
    if (all.length == 0 || addr.length == 0) return @"";
    NSArray *lines = [all componentsSeparatedByString:@"\n"];
    NSString *needle = [NSString stringWithFormat:@"[%@]", addr];   /* unique: address, not name */
    NSInteger start = -1;
    for (NSInteger i = 0; i < (NSInteger)lines.count; i++)
        if ([lines[i] containsString:needle]) { start = i; break; }
    if (start < 0) return @"";
    NSMutableArray *out = [NSMutableArray array];
    for (NSInteger i = start; i < (NSInteger)lines.count; i++) {
        [out addObject:lines[i]];
        if ([lines[i] isEqualToString:@"}"]) break;
    }
    return [out componentsJoinedByString:@"\n"];
}

- (void)showCodeFor:(int)idx {
    if (idx < 0 || idx >= (int)self.fns.count) return;
    NSString *code = extractFuncCode(self.workflowAll, self.fns[idx][@"address"]);
    if (!code.length) code = @"(no code)";
    NSString *asmS = self.fns[idx][@"asm"] ?: @"(no assembly)";
    if (self.viewMode == 0) self.flowView.string = code;
    else if (self.viewMode == 1) self.flowView.string = asmS;
    else self.flowView.string = [NSString stringWithFormat:
            @"// ---- recovered code ----\n%@\n\n// ---- assembly ----\n%@", code, asmS];
}

- (void)onViewMode:(id)s {
    (void)s;
    self.viewMode = (self.viewMode + 1) % 3;
    self.viewBtn.title = self.viewMode == 0 ? @"View: Code"
                       : self.viewMode == 1 ? @"View: Asm"
                                            : @"View: Code+Asm";
    if (self.codeVisible && self.curFunc >= 0) [self showCodeFor:self.curFunc];
}

- (void)onRowClick:(id)s {
    (void)s;
    NSInteger row = self.table.clickedRow;
    if (row < 0 || row >= (NSInteger)self.paneRows.count) return;
    NSDictionary *r = self.paneRows[row];
    if (r[@"jump"]) { [self jumpTo:r[@"jump"]]; return; }
    NSNumber *idx = r[@"idx"];
    if (idx) [self.graph selectFunction:[idx intValue]];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t { (void)t; return (NSInteger)self.paneRows.count; }
- (NSView *)tableView:(NSTableView *)t viewForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    NSTextField *tf = [t makeViewWithIdentifier:col.identifier owner:self];
    if (!tf) { tf = [NSTextField labelWithString:@""]; tf.identifier = col.identifier; }
    NSDictionary *f = row < (NSInteger)self.paneRows.count ? self.paneRows[row] : @{};
    tf.stringValue = f[col.identifier] ?: @"";
    return tf;
}

/* Inspector: children of the current selection. */
- (void)updatePaneRole:(int)role ring:(int)ring func:(int)funcIdx {
    NSMutableArray *rows = [NSMutableArray array];
    if (funcIdx >= 0 && funcIdx < (int)self.fns.count) {
        self.curBtn.hidden = YES; self.curMarkKey = nil;
        NSDictionary *f = self.fns[funcIdx];
        [rows addObject:@{ @"kind": @"function", @"name": f[@"name"], @"info": f[@"address"] }];
        [rows addObject:@{ @"kind": @"returns", @"name": f[@"ret"],
                           @"info": [NSString stringWithFormat:@"nargs=%@%@", f[@"nargs"],
                                     [f[@"recursive"] intValue] ? @"  recursive" : @""] }];
        for (NSDictionary *d in f[@"argd"])
            [rows addObject:@{ @"kind": @"arg", @"name": d[@"name"], @"info": d[@"info"] }];
        for (NSDictionary *d in f[@"outd"])
            [rows addObject:@{ @"kind": @"out", @"name": d[@"name"], @"info": d[@"info"] }];
        for (NSDictionary *e in self.edges)
            if ([e[@"from"] intValue] == funcIdx) {
                int t = [e[@"to"] intValue];
                if (t >= 0 && t < (int)self.fns.count)
                    [rows addObject:@{ @"kind": @"-> callee", @"name": self.fns[t][@"name"],
                                       @"info": self.fns[t][@"address"], @"idx": @(t) }];
            }
        for (NSDictionary *e in self.edges)
            if ([e[@"to"] intValue] == funcIdx) {
                int s2 = [e[@"from"] intValue];
                if (s2 >= 0 && s2 < (int)self.fns.count)
                    [rows addObject:@{ @"kind": @"<- caller", @"name": self.fns[s2][@"name"],
                                       @"info": self.fns[s2][@"address"], @"idx": @(s2) }];
            }
    } else {
        /* non-function selection => the global MARKS list. Marking itself is
         * done only through the "currently selected" window above the table. */
        self.curMarkKey = nil;
        if (ring >= 0) {
            self.curMarkKey = [NSString stringWithFormat:@"r:%d:%d", role, ring];
            NSString *nm = [NSString stringWithFormat:@"ring %d  (%s)", ring,
                            (role >= 0 && role < kPaletteN) ? kRoleName[role] : "?"];
            self.curBtn.title = [NSString stringWithFormat:@"%@  %@  ［%@］", @"現在選択中:",
                                 nm, [self.marked containsObject:self.curMarkKey] ? @"解除" : @"マーク"];
            self.curBtn.hidden = NO;
        } else if (role >= 0) {
            self.curMarkKey = [NSString stringWithFormat:@"g:%d", role];
            self.curBtn.title = [NSString stringWithFormat:@"%@  %s  ［%@］", @"現在選択中:",
                                 (role >= 0 && role < kPaletteN) ? kRoleName[role] : "misc",
                                 [self.marked containsObject:self.curMarkKey] ? @"解除" : @"マーク"];
            self.curBtn.hidden = NO;
        } else {
            self.curBtn.hidden = YES;
        }
        [rows addObject:@{ @"kind": @"---- marks ----", @"name": @"", @"info": @"" }];
        NSArray *sorted = [[self.marked allObjects] sortedArrayUsingSelector:@selector(compare:)];
        for (NSString *key in sorted) {
            if ([key hasPrefix:@"f:"]) {
                NSString *addr = [key substringFromIndex:2];
                NSString *nm = addr;
                for (NSDictionary *f in self.fns) if ([f[@"address"] isEqualToString:addr]) { nm = f[@"name"]; break; }
                [rows addObject:@{ @"kind": @"mark fn", @"name": nm, @"info": addr, @"jump": key }];
            } else if ([key hasPrefix:@"r:"]) {
                [rows addObject:@{ @"kind": @"mark ring", @"name": [NSString stringWithFormat:@"ring %@", [key substringFromIndex:2]], @"info": @"", @"jump": key }];
            } else if ([key hasPrefix:@"g:"]) {
                int g = [[key substringFromIndex:2] intValue];
                [rows addObject:@{ @"kind": @"mark role", @"name": [NSString stringWithUTF8String:(g >= 0 && g < kPaletteN) ? kRoleName[g] : "misc"], @"info": @"", @"jump": key }];
            }
        }
    }
    self.paneRows = rows;
    [self.table reloadData];
}

- (void)saveMarks {
    NSString *mk = [@"marks:" stringByAppendingString:self.lastPath ?: @""];
    [[NSUserDefaults standardUserDefaults] setObject:[self.marked allObjects] forKey:mk];
}

/* Marking happens here ONLY: the "currently selected" window is clicked. */
- (void)onMarkCurrent:(id)s {
    (void)s;
    if (!self.curMarkKey) return;
    if ([self.marked containsObject:self.curMarkKey]) [self.marked removeObject:self.curMarkKey];
    else [self.marked addObject:self.curMarkKey];
    [self saveMarks];
    [self.graph setMarks:self.marked];
    int role = -1, ring = -1;
    if ([self.curMarkKey hasPrefix:@"r:"]) {
        NSArray *p = [[self.curMarkKey substringFromIndex:2] componentsSeparatedByString:@":"];
        if (p.count == 2) { role = [p[0] intValue]; ring = [p[1] intValue]; }
    } else if ([self.curMarkKey hasPrefix:@"g:"]) {
        role = [[self.curMarkKey substringFromIndex:2] intValue];
    }
    [self updatePaneRole:role ring:ring func:-1];   /* refresh the button + list */
}

- (void)jumpTo:(NSString *)key {
    if ([key hasPrefix:@"f:"]) {
        NSString *addr = [key substringFromIndex:2];
        for (int i = 0; i < (int)self.fns.count; i++)
            if ([self.fns[i][@"address"] isEqualToString:addr]) { [self.graph selectFunction:i]; return; }
    } else if ([key hasPrefix:@"r:"]) {
        NSArray *p = [[key substringFromIndex:2] componentsSeparatedByString:@":"];
        if (p.count == 2) [self.graph selectRing:[p[0] intValue] ring:[p[1] intValue]];
    } else if ([key hasPrefix:@"g:"]) {
        [self.graph selectRole:[[key substringFromIndex:2] intValue]];
    }
}
@end

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *d = [[AppDelegate alloc] init];
        app.delegate = d;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        [app run];
    }
    return 0;
}
