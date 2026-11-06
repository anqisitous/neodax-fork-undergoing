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
 *   Run:    ./bin/neodax-viewer
 */

#import <Cocoa/Cocoa.h>

extern "C" {
#include "integration/neodax_bridge.h"
#include "integration/nx_deepai.h"
}

#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <cstdlib>

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
@property (copy) FnBlock onToggleView;   /* fired on ANY object click: toggle list+code view */
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
    /* 視点 (オービット) : yaw/pitch で 3D 風回転、rad はリング半径係数 */
    float _yaw, _pitch;
    NSPoint _last;
}
- (instancetype)initWithFrame:(NSRect)f {
    self = [super initWithFrame:f];
    if (self) { _scale = 2.0f; _panx = _pany = 0; _selRole = _selRing = _selFunc = -1; _yaw = 0.5f; _pitch = 0.35f; }
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

        /* local layout: 各環(ring)を円とし、メンバーを円周上に配置。
           環同士はグリッド状に並べる (2D、視点は環中心に正対)。 */
        const float PI2 = 6.2831853f;
        const float RGAPX = 3.0f, RGAPY = 3.0f;
        float packX = 0, packY = 0, rowH = 0, localW = 0;
        struct Sub { int ring; NSRect r; std::vector<int> members; };
        std::vector<Sub> subs;
        for (int r : rings) {
            std::vector<int> m;
            for (int idx : roleMem[g]) if (_nodes[idx].ring == r) m.push_back(idx);
            if (m.empty()) continue;
            int n = (int)m.size();
            float rad = (n > 1) ? fmaxf(1.1f, (float)n / PI2) : 0.0f;
            float w = (n > 1) ? 2 * rad : 1.4f;
            float h = (n > 1) ? 2 * rad : 1.4f;
            if (packX + w > 26.0f && packX > 0) { packX = 0; packY -= rowH + RGAPY; rowH = 0; }
            float ccx = packX + w * 0.5f;
            float ccy = packY - h * 0.5f;
            for (int k = 0; k < n; k++) {
                int idx = m[k];
                if (n > 1) {
                    float a = PI2 * (float)k / (float)n + 0.5f;
                    _nodes[idx].x = ccx + cosf(a) * rad;
                    _nodes[idx].y = ccy + sinf(a) * rad;
                } else {
                    _nodes[idx].x = ccx;
                    _nodes[idx].y = ccy;
                }
            }
            NSRect box = NSMakeRect(packX, packY - h, w, h);
            subs.push_back({ r, box, m });
            packX += w + RGAPX;
            if (h > rowH) rowH = h;
            if (packX > localW) localW = packX;
        }
        float localH = -(packY - rowH);

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

- (void)fitAll {
    if (_nodes.empty()) return;
    float minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
    for (auto &n : _nodes) { minx = std::min(minx, n.x); maxx = std::max(maxx, n.x);
                             miny = std::min(miny, n.y); maxy = std::max(maxy, n.y); }
    float spanx = maxx - minx, spany = maxy - miny;
    float sx = spanx > 0.001f ? (self.bounds.size.width  * 0.92f) / spanx : 2.0f;
    float sy = spany > 0.001f ? (self.bounds.size.height * 0.92f) / spany : 2.0f;
    _scale = std::min(sx, sy);
    if (_scale < 0.02f) _scale = 0.02f;
    if (_scale > 20.0f) _scale = 20.0f;
    _panx = _pany = 0;
    [self setNeedsDisplay:YES];
}

- (void)setNodes:(const std::vector<GNode> &)nodes edges:(const std::vector<GEdge> &)edges {
    _nodes = nodes; _edges = edges;
    _selRole = _selRing = _selFunc = -1;
    [self layout];
    [self fitAll];
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
- (void)centerOnRing {
    if (_selRole < 0 || _selRing < 0) { _panx = _pany = 0; return; }
    for (size_t i = 0; i < _ringBox.size(); i++)
        if (_ringRole[i] == _selRole && _ringId[i] == _selRing) {
            float cx = _ringBox[i].origin.x + _ringBox[i].size.width * 0.5f;
            float cy = _ringBox[i].origin.y + _ringBox[i].size.height * 0.5f;
            _panx = -cx * _scale; _pany = -cy * _scale;   /* 環中心に正対 */
            return;
        }
    _panx = _pany = 0;
}
- (void)keyDown:(NSEvent *)e {
    unichar c = e.charactersIgnoringModifiers.length ? [e.charactersIgnoringModifiers characterAtIndex:0] : 0;
    BOOL shift = (e.modifierFlags & NSEventModifierFlagShift) != 0;
    unsigned short kc = [e keyCode];
    if (c == 0x09 || kc == 48) { if (shift) [self ascend]; else [self descend]; [self centerOnRing]; [self setNeedsDisplay:YES]; }
    else if (c == 'q' || c == 'Q') { [self cycleSibling:shift ? -1 : 1]; [self centerOnRing]; [self setNeedsDisplay:YES]; }
    /* 矢印: 左右 = 環の円周/同階層を移動, 上下 = 降下/上昇。視点は環中心に正対。 */
    else if (kc == 123 || kc == 124) { [self cycleSibling:(kc == 124 ? 1 : -1)]; [self centerOnRing]; [self setNeedsDisplay:YES]; }
    else if (kc == 126) { [self ascend]; [self centerOnRing]; [self setNeedsDisplay:YES]; }
    else if (kc == 125) { [self descend]; [self centerOnRing]; [self setNeedsDisplay:YES]; }
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

    /* rings: リアルな陰影の 3D トーラス。メンバは環の円周上に乗る。 */
    for (size_t i = 0; i < _ringBox.size(); i++) {
        if (_ringCount[i] <= 1) continue;
        NSRect b = _ringBox[i];
        float cx = b.origin.x + b.size.width * 0.5f;
        float cy = b.origin.y + b.size.height * 0.5f;
        float Rbox = b.size.width * 0.5f;
        float tube = fmaxf(0.20f, Rbox * 0.22f);      /* チューブ半径 */
        float R = Rbox - tube;                        /* 中心線半径 = メンバの円 */
        if (R < tube * 0.6f) R = tube * 0.6f;
        bool on = (_selRole == _ringRole[i] && _selRing == _ringId[i]);
        bool mk = [_marks containsObject:[NSString stringWithFormat:@"r:%d:%d", _ringRole[i], _ringId[i]]];
        const float *pc = kPalette[_ringRole[i] < 0 ? 10 : _ringRole[i]];
        CGFloat br = pc[0], bg = pc[1], bb = pc[2];
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();

        /* 接地影 (少し下にオフセットした暗い環) */
        {
            CGFloat sh[4] = { 0, 0, 0, 0.35 };
            CGColorRef c = CGColorCreate(cs, sh);
            CGContextSaveGState(ctx);
            CGMutablePathRef sp = CGPathCreateMutable();
            CGPathAddEllipseInRect(sp, NULL, CGRectMake(cx-Rbox, cy-Rbox-0.25f, 2*Rbox, 2*Rbox));
            CGPathAddEllipseInRect(sp, NULL, CGRectMake(cx-(R-tube), cy-(R-tube)-0.25f, 2*(R-tube), 2*(R-tube)));
            CGContextAddPath(ctx, sp); CGContextClip(ctx);
            CGContextSetFillColorWithColor(ctx, c);
            CGContextFillRect(ctx, CGRectMake(cx-Rbox-1, cy-Rbox-2, 2*Rbox+2, 2*Rbox+4));
            CGContextRestoreGState(ctx);
            CGPathRelease(sp); CGColorRelease(c);
        }

        /* 環本体をクリップ */
        CGContextSaveGState(ctx);
        CGMutablePathRef path = CGPathCreateMutable();
        CGPathAddEllipseInRect(path, NULL, CGRectMake(cx-Rbox, cy-Rbox, 2*Rbox, 2*Rbox));
        CGPathAddEllipseInRect(path, NULL, CGRectMake(cx-(R-tube), cy-(R-tube), 2*(R-tube), 2*(R-tube)));
        CGContextAddPath(ctx, path); CGContextClip(ctx);

        /* 断面方向の陰影 (チューブ中央が明るい放射グラデ) */
        {
            CGFloat c0[4] = { br*0.20f, bg*0.20f, bb*0.22f, 1 };
            CGFloat c1[4] = { br*0.62f, bg*0.62f, bb*0.64f, 1 };
            CGFloat c2[4] = { fminf(1,br*1.0f+0.28f), fminf(1,bg*1.0f+0.28f), fminf(1,bb*1.0f+0.28f), 1 };
            CGFloat c3[4] = { br*0.55f, bg*0.55f, bb*0.57f, 1 };
            CGFloat c4[4] = { br*0.14f, bg*0.14f, bb*0.16f, 1 };
            CGColorRef g0=CGColorCreate(cs,c0), g1=CGColorCreate(cs,c1), g2=CGColorCreate(cs,c2), g3=CGColorCreate(cs,c3), g4=CGColorCreate(cs,c4);
            CGColorRef arr[5] = { g0,g1,g2,g3,g4 };
            CGFloat loc[5] = { 0.0f, 0.30f, 0.52f, 0.74f, 1.0f };
            CFArrayRef ca = CFArrayCreate(NULL, (const void**)arr, 5, &kCFTypeArrayCallBacks);
            CGGradientRef grad = CGGradientCreateWithColors(cs, ca, loc);
            CGPoint ctr = CGPointMake(cx, cy);
            CGContextDrawRadialGradient(ctx, grad, ctr, (CGFloat)(R-tube), ctr, (CGFloat)(R+tube), 0);
            CGGradientRelease(grad); CFRelease(ca);
            for (int k=0;k<5;k++) CGColorRelease(arr[k]);
        }
        /* 方向光のオーバーレイ (左上から照射 / 右下は影) */
        {
            CGFloat a0[4] = { 1, 1, 1, on ? 0.34f : 0.24f };
            CGFloat a1[4] = { 1, 1, 1, 0.0f };
            CGFloat a2[4] = { 0, 0, 0, on ? 0.30f : 0.40f };
            CGColorRef k0=CGColorCreate(cs,a0), k1=CGColorCreate(cs,a1), k2=CGColorCreate(cs,a2);
            CGColorRef arr[3] = { k0,k1,k2 };
            CGFloat loc[3] = { 0.0f, 0.5f, 1.0f };
            CFArrayRef ca = CFArrayCreate(NULL, (const void**)arr, 3, &kCFTypeArrayCallBacks);
            CGGradientRef grad = CGGradientCreateWithColors(cs, ca, loc);
            CGPoint p0 = CGPointMake(cx-Rbox, cy+Rbox);   /* 左上 (y up) */
            CGPoint p1 = CGPointMake(cx+Rbox, cy-Rbox);   /* 右下 */
            CGContextDrawLinearGradient(ctx, grad, p0, p1, 0);
            CGGradientRelease(grad); CFRelease(ca);
            for (int k=0;k<3;k++) CGColorRelease(arr[k]);
        }
        CGContextRestoreGState(ctx);
        CGPathRelease(path);

        /* 輪郭 (選択/マークで強調) */
        CGContextSetRGBStrokeColor(ctx, mk?1.0f:(on?1.0f:0.0f), mk?1.0f:(on?0.95f:0.0f),
                                   mk?1.0f:(on?0.6f:0.0f), mk?1.0f:(on?0.95f:0.55f));
        CGContextSetLineWidth(ctx, (on ? 2.4f : 1.2f) / _scale);
        CGContextStrokeEllipseInRect(ctx, CGRectMake(cx-Rbox, cy-Rbox, 2*Rbox, 2*Rbox));
        CGContextStrokeEllipseInRect(ctx, CGRectMake(cx-(R-tube), cy-(R-tube), 2*(R-tube), 2*(R-tube)));

        /* スペキュラハイライト (左上の細い弧) */
        {
            CGContextSetRGBStrokeColor(ctx, 1, 1, 1, on ? 0.75f : 0.45f);
            CGContextSetLineWidth(ctx, (tube * 0.5f) / _scale);
            CGContextAddArc(ctx, cx, cy, R, 1.9f, 2.7f, 0);   /* 左上の弧 */
            CGContextStrokePath(ctx);
        }
        CGColorSpaceRelease(cs);
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

    /* 全関数ノードを常時薄く描画: 解析ノードをそのまま目で見せる */
    {
        for (size_t i = 0; i < _nodes.size(); i++) {
            const GNode &n = _nodes[i];
            const float *p = kPalette[n.role < 0 ? 10 : n.role];
            float s = 0.9f / _scale;
            if (s < 0.35f) s = 0.35f;
            if (s > 3.0f) s = 3.0f;
            CGContextSetRGBFillColor(ctx, p[0], p[1], p[2], 0.55f);
            CGContextFillRect(ctx, CGRectMake(n.x - s, n.y - s, 2 * s, 2 * s));
        }
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
    [self centerOnRing];                   /* 視点は環中心に正対 */
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
    /* 右クリック / 中クリック = 視点回転 (オービット)。見切れたら自分で視点を動かす。 */
    if ([e type] == NSEventTypeRightMouseDown || [e type] == NSEventTypeOtherMouseDown) {
        _last = [self convertPoint:e.locationInWindow fromView:nil];
        return;
    }
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
    /* オブジェクトをクリック => 関数一覧とコード全体の view をトグル */
    if (self.onToggleView) self.onToggleView(_selFunc);
    [self setNeedsDisplay:YES];
}
- (void)mouseDragged:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if ([e type] == NSEventTypeRightMouseDragged || [e type] == NSEventTypeOtherMouseDragged) {
        _yaw   += (float)(p.x - _last.x) * 0.008f;   /* 横ドラッグ = yaw */
        _pitch += (float)(p.y - _last.y) * 0.008f;   /* 縦ドラッグ = pitch */
        if (_pitch >  1.4f) _pitch =  1.4f;
        if (_pitch < -1.4f) _pitch = -1.4f;
        _last = p;
        [self setNeedsDisplay:YES];
        return;
    }
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

/* ── 画面再現ビュー (キャプチャ無しで状態から描画) ─────────────── */
@interface ReplayView : NSView {
    nx_ui_layout_t _layout;
}
- (void)setLayout:(nx_ui_layout_t)l;
@end

@implementation ReplayView
- (void)setLayout:(nx_ui_layout_t)l {
    _layout = l;   /* ポインタごとコピー (所有は呼び出し側が保持) */
    [self setNeedsDisplay:YES];
}
- (BOOL)isFlipped { return NO; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    const nx_ui_layout_t *l = &_layout;
    if (l->n_nodes == 0 || !l->nodes) return;
    CGFloat sx = b.size.width / l->width;
    CGFloat sy = b.size.height / l->height;

    [[NSColor colorWithCalibratedRed:0.10 green:0.11 blue:0.13 alpha:1] setFill];
    NSRectFill(b);

    for (int i = 0; i < l->n_nodes; i++) {
        const nx_ui_node_t *n = &l->nodes[i];
        NSRect r = NSMakeRect(n->x * sx, n->y * sy, n->w * sx, n->h * sy);
        NSColor *c;
        switch (n->kind) {
            case 0: c = [NSColor colorWithCalibratedRed:0.20 green:0.22 blue:0.26 alpha:1]; break;
            case 1: c = [NSColor colorWithCalibratedRed:0.25 green:0.45 blue:0.80 alpha:1]; break;
            case 2: c = [NSColor colorWithCalibratedRed:0.30 green:0.65 blue:0.40 alpha:1]; break;
            case 3: c = [NSColor colorWithCalibratedRed:0.30 green:0.32 blue:0.36 alpha:1]; break;
            case 4: c = [NSColor colorWithCalibratedRed:0.80 green:0.25 blue:0.25 alpha:1]; break;
            case 5: c = [NSColor colorWithCalibratedRed:0.17 green:0.18 blue:0.22 alpha:1]; break;
            default: c = [NSColor colorWithCalibratedRed:0.14 green:0.15 blue:0.18 alpha:1]; break;
        }
        [c setFill]; NSRectFill(r);
        [[NSColor colorWithCalibratedRed:0.45 green:0.50 blue:0.60 alpha:1] setStroke];
        NSFrameRect(r);
        NSString *label = [NSString stringWithUTF8String:n->label];
        NSDictionary *attr = @{
            NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.95 alpha:1],
            NSFontAttributeName: [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightMedium] };
        NSRect lr = NSMakeRect(r.origin.x + 4, r.origin.y + r.size.height/2 - 7, r.size.width - 8, 16);
        [label drawInRect:lr withAttributes:attr];
    }
    [[NSColor colorWithCalibratedRed:0.60 green:0.60 blue:0.40 alpha:1] setStroke];
    for (int i = 0; i < l->n_edges; i++) {
        const nx_ui_edge_t *e = &l->edges[i];
        NSBezierPath *p = [NSBezierPath bezierPath];
        [p moveToPoint:NSMakePoint(e->x1 * sx, e->y1 * sy)];
        [p lineToPoint:NSMakePoint(e->x2 * sx, e->y2 * sy)];
        [p setLineWidth:1.5]; [p stroke];
    }
}
@end

/* ── 画面に浮遊するカスタムボタン (標準 chrome を使わない) ─────────── */
@interface FloatButton : NSView
@property (nonatomic, copy)   NSString *title;
@property (nonatomic, weak)   id        target;
@property (nonatomic, assign) SEL       action;
@property (nonatomic, assign) BOOL      active;
@end

@implementation FloatButton {
    NSTrackingArea *_track;
    BOOL _hover;
    BOOL _pressed;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = NO;
        _hover = NO;
    }
    return self;
}
- (BOOL)isFlipped { return NO; }
- (void)setTitle:(NSString *)t { _title = [t copy]; [self setNeedsDisplay:YES]; }
- (void)setActive:(BOOL)a { _active = a; [self setNeedsDisplay:YES]; }
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_track) [self removeTrackingArea:_track];
    _track = [[NSTrackingArea alloc] initWithRect:self.bounds
        options:(NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect)
        owner:self userInfo:nil];
    [self addTrackingArea:_track];
}
- (void)mouseEntered:(NSEvent *)e { (void)e; _hover = YES; [self setNeedsDisplay:YES]; }
- (void)mouseExited:(NSEvent *)e  { (void)e; _hover = NO; _pressed = NO; [self setNeedsDisplay:YES]; }
- (BOOL)acceptsFirstMouse:(NSEvent *)e { (void)e; return YES; }
- (void)mouseDown:(NSEvent *)e { (void)e; _pressed = YES; [self setNeedsDisplay:YES]; }
- (void)mouseUp:(NSEvent *)e {
    (void)e;
    _pressed = NO;
    [self setNeedsDisplay:YES];
    if (_hover && self.target && self.action) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
        [self.target performSelector:self.action withObject:self];
#pragma clang diagnostic pop
    }
}
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect r = NSInsetRect(self.bounds, 2, 2);
    CGFloat rad = r.size.height / 2.0;
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:r xRadius:rad yRadius:rad];
    CGFloat base = self.active ? 0.34 : (_hover ? 0.20 : 0.10);
    if (_pressed) base += 0.08;
    [[NSColor colorWithCalibratedWhite:1.0 alpha:base] setFill];
    [p fill];
    NSColor *edge = self.active ? [NSColor colorWithCalibratedRed:0.45 green:0.75 blue:1.0 alpha:0.9]
                                : [NSColor colorWithCalibratedWhite:1.0 alpha:0.16];
    [edge setStroke];
    [p setLineWidth:1.0];
    [p stroke];
    NSDictionary *attr = @{
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedWhite:0.97 alpha:1.0],
        NSFontAttributeName: [NSFont systemFontOfSize:12 weight:NSFontWeightMedium]
    };
    NSSize ts = [self.title sizeWithAttributes:attr];
    NSPoint tp = NSMakePoint(r.origin.x + (r.size.width - ts.width) / 2.0,
                             r.origin.y + (r.size.height - ts.height) / 2.0);
    [self.title drawAtPoint:tp withAttributes:attr];
}
@end

/* ── 背景キャンバス (角丸のダークパネル + ドラッグ移動) ─────────────── */
@interface CanvasView : NSView
@end
@implementation CanvasView
- (BOOL)isFlipped { return NO; }
- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect outer = NSInsetRect(self.bounds, 1, 1);
    NSBezierPath *p = [NSBezierPath bezierPathWithRoundedRect:outer xRadius:18 yRadius:18];
    [[NSColor colorWithCalibratedRed:0.07 green:0.08 blue:0.10 alpha:0.97] setFill];
    [p fill];
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.08] setStroke];
    [p setLineWidth:1];
    [p stroke];

    /* 外枠(角丸)の内側に、角のない四角い枠を描いて「四角いウィンドウ」に見せる */
    NSRect inner = NSInsetRect(outer, 12, 12);
    NSBezierPath *sq = [NSBezierPath bezierPathWithRect:inner];
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.14] setStroke];
    [sq setLineWidth:1];
    [sq stroke];

    /* 四隅のコーナーマーカー (四角さを強調) */
    CGFloat L = 14.0;
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.28] setStroke];
    NSBezierPath *c = [NSBezierPath bezierPath];
    [c setLineWidth:1.4];
    NSRect b = inner;
    /* 左上 */
    [c moveToPoint:NSMakePoint(NSMinX(b), NSMinY(b)+L)]; [c lineToPoint:NSMakePoint(NSMinX(b), NSMinY(b))]; [c lineToPoint:NSMakePoint(NSMinX(b)+L, NSMinY(b))];
    /* 右上 */
    [c moveToPoint:NSMakePoint(NSMaxX(b)-L, NSMinY(b))]; [c lineToPoint:NSMakePoint(NSMaxX(b), NSMinY(b))]; [c lineToPoint:NSMakePoint(NSMaxX(b), NSMinY(b)+L)];
    /* 左下 */
    [c moveToPoint:NSMakePoint(NSMinX(b), NSMaxY(b)-L)]; [c lineToPoint:NSMakePoint(NSMinX(b), NSMaxY(b))]; [c lineToPoint:NSMakePoint(NSMinX(b)+L, NSMaxY(b))];
    /* 右下 */
    [c moveToPoint:NSMakePoint(NSMaxX(b)-L, NSMaxY(b))]; [c lineToPoint:NSMakePoint(NSMaxX(b), NSMaxY(b))]; [c lineToPoint:NSMakePoint(NSMaxX(b), NSMaxY(b)-L)];
    [c stroke];
}
- (void)mouseDown:(NSEvent *)e {
    [self.window performWindowDragWithEvent:e];
}
@end

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
@property (strong) FloatButton   *viewBtn;
@property (strong) FloatButton   *goBtn;
@property (strong) FloatButton   *openBtn;
@property (strong) FloatButton   *curBtn;      // "currently selected" window (click => mark)
@property (strong) NSString      *curMarkKey;  // key of the current role/ring (nil if none)
@property (nonatomic) int          curFunc;     // currently selected function
@property (nonatomic) int          curRole;     // currently selected role (-1 none)
@property (nonatomic) int          curRing;     // currently selected ring (-1 none)
@property (nonatomic) BOOL         listCodeVisible;  // クリックで開いた一覧+コード表示
@property (nonatomic) BOOL         analyzing;
@property (strong) NSMutableArray *fns;       // {name, role, roleIdx, ring, address}
@property (strong) NSMutableArray *edges;     // {from, to}
@property (strong) NSMutableArray *paneRows;  // inspector rows {kind, name, info}
@property (strong) NSString       *lastPath;
@property (strong) ReplayView     *replayView;
@property (strong) FloatButton    *replayBtn;
@property (strong) NSTextField    *replayStateField;
@property (nonatomic) BOOL         replayVisible;
@end

@implementation AppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    /* 枠のない浮遊ウィンドウ。標準のタイトルバー/ボタンは使わない。 */
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1200, 780)
        styleMask:(NSWindowStyleMaskBorderless|NSWindowStyleMaskResizable)
        backing:NSBackingStoreBuffered defer:NO];
    self.window.opaque = NO;
    self.window.backgroundColor = [NSColor clearColor];
    self.window.hasShadow = YES;
    self.window.movableByWindowBackground = YES;
    self.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    [self.window center];
    CanvasView *cv = [[CanvasView alloc] initWithFrame:NSMakeRect(0, 0, 1200, 780)];
    self.window.contentView = cv;

    /* 浮遊する閉じるボタン (枠なしウィンドウのため) */
    FloatButton *closeBtn = [[FloatButton alloc] initWithFrame:NSMakeRect(1144, 742, 36, 26)];
    closeBtn.title = @"✕";
    closeBtn.target = self; closeBtn.action = @selector(onQuit:);
    [cv addSubview:closeBtn];

    /* 浮遊ボタン (下部に並ぶ) */
    self.goBtn = [[FloatButton alloc] initWithFrame:NSMakeRect(20, 18, 112, 34)];
    self.goBtn.title = @"Analyze";
    self.goBtn.target = self; self.goBtn.action = @selector(onAnalyze:);
    [cv addSubview:self.goBtn];

    self.openBtn = [[FloatButton alloc] initWithFrame:NSMakeRect(140, 18, 128, 34)];
    self.openBtn.title = @"Open .app";
    self.openBtn.target = self; self.openBtn.action = @selector(onOpen:);
    [cv addSubview:self.openBtn];

    self.viewBtn = [[FloatButton alloc] initWithFrame:NSMakeRect(276, 18, 132, 34)];
    self.viewBtn.title = @"View: Code";
    self.viewBtn.target = self; self.viewBtn.action = @selector(onViewMode:);
    [cv addSubview:self.viewBtn];

    self.replayBtn = [[FloatButton alloc] initWithFrame:NSMakeRect(416, 18, 118, 34)];
    self.replayBtn.title = @"Replay";
    self.replayBtn.target = self; self.replayBtn.action = @selector(onReplayToggle:);
    [cv addSubview:self.replayBtn];

    /* 浮遊するパス入力 (ダーク・角丸・枠なし) */
    self.pathField = [[NSTextField alloc] initWithFrame:NSMakeRect(548, 20, 632, 30)];
    self.pathField.placeholderString = @"/path/to/Mach-O binary, .app, or .exe";
    self.pathField.bezeled = NO; self.pathField.bordered = NO;
    self.pathField.drawsBackground = YES;
    self.pathField.backgroundColor = [NSColor colorWithCalibratedWhite:1.0 alpha:0.08];
    self.pathField.textColor = [NSColor colorWithCalibratedWhite:0.95 alpha:1.0];
    self.pathField.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.pathField.focusRingType = NSFocusRingTypeNone;
    [cv addSubview:self.pathField];

    self.replayStateField = [[NSTextField alloc] initWithFrame:NSMakeRect(548, 20, 632, 30)];
    self.replayStateField.placeholderString = @"state: /path (leveldb dir / json)";
    self.replayStateField.bezeled = NO; self.replayStateField.bordered = NO;
    self.replayStateField.drawsBackground = YES;
    self.replayStateField.backgroundColor = [NSColor colorWithCalibratedWhite:1.0 alpha:0.08];
    self.replayStateField.textColor = [NSColor colorWithCalibratedWhite:0.95 alpha:1.0];
    self.replayStateField.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
    self.replayStateField.focusRingType = NSFocusRingTypeNone;
    self.replayStateField.hidden = YES;
    [cv addSubview:self.replayStateField];

    self.status = [[NSTextField alloc] initWithFrame:NSMakeRect(24, 748, 1152, 18)];
    self.status.editable = NO; self.status.bordered = NO; self.status.bezeled = NO;
    self.status.drawsBackground = NO;
    self.status.textColor = [NSColor colorWithCalibratedWhite:0.75 alpha:1.0];
    self.status.font = [NSFont systemFontOfSize:11 weight:NSFontWeightRegular];
    self.status.stringValue = @"Tab:下位  Shift+Tab:上位  Q:同階層  クリック:最上位を選択";
    [cv addSubview:self.status];

    self.graph = [[GraphView alloc] initWithFrame:NSMakeRect(20, 64, 764, 664)];
    [cv addSubview:self.graph];

    self.replayView = [[ReplayView alloc] initWithFrame:NSMakeRect(20, 64, 764, 664)];
    self.replayView.hidden = YES;
    [cv addSubview:self.replayView];

    self.curBtn = [[FloatButton alloc] initWithFrame:NSMakeRect(804, 690, 376, 34)];
    self.curBtn.title = @"現在選択中";
    self.curBtn.target = self; self.curBtn.action = @selector(onMarkCurrent:);
    self.curBtn.hidden = YES;
    [cv addSubview:self.curBtn];

    NSScrollView *tv = [[NSScrollView alloc] initWithFrame:NSMakeRect(804, 64, 376, 618)];
    self.tableScroll = tv;
    tv.hasVerticalScroller = YES; tv.borderType = NSNoBorder;
    tv.drawsBackground = NO;
    tv.scrollerStyle = NSScrollerStyleOverlay;
    self.table = [[NSTableView alloc] initWithFrame:tv.bounds];
    self.table.backgroundColor = [NSColor clearColor];
    self.table.gridColor = [NSColor colorWithCalibratedWhite:1.0 alpha:0.08];
    self.table.style = NSTableViewStylePlain;       /* 背景なし・素のまま */
    self.table.rowSizeStyle = NSTableViewRowSizeStyleSmall;
    for (NSString *t in @[@"kind", @"name", @"info"]) {
        NSTableColumn *c = [[NSTableColumn alloc] initWithIdentifier:t];
        c.title = t; [self.table addTableColumn:c];
    }
    self.table.dataSource = self; self.table.delegate = self;
    self.table.target = self; self.table.action = @selector(onRowClick:);
    tv.documentView = self.table; [cv addSubview:tv];

    NSScrollView *fv = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 64, 1160, 0)];
    self.codeScroll = fv;
    fv.hasVerticalScroller = YES; fv.borderType = NSNoBorder;
    fv.drawsBackground = NO;
    fv.scrollerStyle = NSScrollerStyleOverlay;
    self.flowView = [[NSTextView alloc] initWithFrame:fv.bounds];
    self.flowView.editable = NO;
    self.flowView.drawsBackground = NO;
    self.flowView.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    fv.documentView = self.flowView; [cv addSubview:fv];

    self.codeVisible = NO;
    [self setCodeVisible:NO];
    [self.window makeKeyAndOrderFront:nil];

    /* 検証用: NX_SNAPSHOT=/path.png で自分の contentView を自己描画して出力。
       画面収録権限なしで GUI の見た目を確認するためのもの。 */
    const char *snap = getenv("NX_SNAPSHOT");
    if (snap) {
        NSString *path = [NSString stringWithUTF8String:snap];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3.5 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            NSView *v = self.window.contentView;
            NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
            [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
            NSData *d = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
            [d writeToFile:path atomically:YES];
            fprintf(stderr, "snapshot: %s\n", snap);
            if (getenv("NX_SNAPSHOT_EXIT")) [NSApp terminate:nil];
        });
    }

    [self.window makeKeyAndOrderFront:nil];
    [self.window makeFirstResponder:self.graph];
    [NSApp activateIgnoringOtherApps:YES];

    /* 起動時に解析するパス: NX_PATH 環境変数 > コマンドライン引数 > lastPath */
    NSString *startPath = nil;
    const char *envp = getenv("NX_PATH");
    if (envp) startPath = [NSString stringWithUTF8String:envp];
    if (!startPath) {
        NSArray *args = [NSProcessInfo processInfo].arguments;
        for (NSUInteger i = 1; i < args.count; i++) {
            if (![args[i] hasPrefix:@"-"]) { startPath = args[i]; break; }
        }
    }
    if (!startPath) startPath = [[NSUserDefaults standardUserDefaults] stringForKey:@"lastPath"];
    self.lastPath = startPath;
    if (self.lastPath.length && [[NSFileManager defaultManager] fileExistsAtPath:self.lastPath]) {
        self.pathField.stringValue = self.lastPath;
        [self analyzePath:self.lastPath];
    }
}
- (void)writeSnapshot:(NSString *)path {
    NSView *v = self.window.contentView;
    [v display];
    NSBitmapImageRep *rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
    [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];
    NSData *d = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    [d writeToFile:path atomically:YES];
    fprintf(stderr, "snapshot: %s\n", path.UTF8String);
}

/* 操作の検証: role -> ring -> function と降下し、各段をスナップショット。 */
- (void)runDrill:(NSString *)dir {
    self.graph.hidden = NO;
    [self.graph selectRole:-1];
    [self.graph setNeedsDisplay:YES];
    [self writeSnapshot:[dir stringByAppendingPathComponent:@"0_all.png"]];

    /* (role, ring) ごとのメンバー数を集計し、全 role を通じて最多の環を探す */
    int bestRole = -1, bestRing = -999, bestRingN = 0, foundFunc = -1;
    NSMutableDictionary *byRR = [NSMutableDictionary dictionary];
    for (int i = 0; i < (int)self.fns.count; i++) {
        int rr = [self.fns[i][@"roleIdx"] intValue];
        int ri = [self.fns[i][@"ring"] intValue];
        NSString *key = [NSString stringWithFormat:@"%d:%d", rr, ri];
        byRR[key] = @([byRR[key] intValue] + 1);
    }
    for (NSString *key in byRR) {
        int c = [byRR[key] intValue];
        if (c > bestRingN) {
            bestRingN = c;
            NSArray *parts = [key componentsSeparatedByString:@":"];
            bestRole = [parts[0] intValue];
            bestRing = [parts[1] intValue];
        }
    }
    for (int i = 0; i < (int)self.fns.count; i++)
        if ([self.fns[i][@"roleIdx"] intValue] == bestRole &&
            [self.fns[i][@"ring"] intValue] == bestRing) { foundFunc = i; break; }
    fprintf(stderr, "drill: role=%d ring=%d members=%d func=%d (%s)\n", bestRole, bestRing, bestRingN, foundFunc,
            foundFunc >= 0 ? [self.fns[foundFunc][@"name"] UTF8String] : "-");

    /* role 選択 */
    [self.graph selectRole:bestRole];
    [self.graph setNeedsDisplay:YES];
    [self writeSnapshot:[dir stringByAppendingPathComponent:@"1_role.png"]];

    /* ring 選択 (メンバー最多: torus が見える) */
    [self.graph selectRing:bestRole ring:bestRing];
    [self.graph setNeedsDisplay:YES];
    [self writeSnapshot:[dir stringByAppendingPathComponent:@"2_ring.png"]];

    /* function 選択 + コード表示 */
    if (foundFunc >= 0) {
        [self.graph selectFunction:foundFunc];
        [self showCodeFor:foundFunc];
        [self setCodeVisible:YES];
        [self.graph setNeedsDisplay:YES];
        [self writeSnapshot:[dir stringByAppendingPathComponent:@"3_function.png"]];
        /* クリック相当: [関数一覧] + [コード＋親/子ノード] のトグル view */
        [self toggleScopeView];
        [self.graph setNeedsDisplay:YES];
        [self writeSnapshot:[dir stringByAppendingPathComponent:@"4_scopelist.png"]];
    }
    fprintf(stderr, "drill: done\n");
    if (getenv("NX_SNAPSHOT_EXIT")) [NSApp terminate:nil];
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

            [fns addObject:[@{ @"name": [NSString stringWithUTF8String:bin->functions[i].name],
                              @"role": [NSString stringWithUTF8String:role],
                              @"roleIdx": @(nodes[i].role),
                              @"ring": @(nodes[i].ring),
                              @"recursive": @(recursive),
                              @"nargs": @(nargs),
                              @"ret": [NSString stringWithUTF8String:depflow_origin_str(reto)],
                              @"argd": argd,
                              @"outd": outd,
                              @"address": [NSString stringWithFormat:@"0x%llx", (unsigned long long)nodes[i].addr] } mutableCopy]];
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
                ws.curRole = role; ws.curRing = ring; ws.curFunc = func;
                if (ws.listCodeVisible) {
                    /* view が開いていれば、選択に追従 (関数一覧 + 親/子 + コード) */
                    [ws buildScopeList];
                    if (func >= 0) [ws showCodeFor:func];
                } else {
                    [ws updatePaneRole:role ring:ring func:func];
                    if (func < 0) [ws setCodeVisible:NO];
                    else if (ws.codeVisible) [ws showCodeFor:func];
                }
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
            /* クリック: 関数一覧とコード全体の view をトグル */
            self.graph.onToggleView = ^(int func) {
                (void)func;
                [ws toggleScopeView];
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

            const char *drill = getenv("NX_DRILL");
            if (drill) [self runDrill:[NSString stringWithUTF8String:drill]];
        });
    });
}

/* ---- code pane ---- */
- (void)setCodeVisible:(BOOL)v {
    _codeVisible = v;
    self.codeScroll.hidden = !v;
    if (v) {
        self.graph.frame       = NSMakeRect(20, 300, 764, 428);
        self.replayView.frame  = NSMakeRect(20, 300, 764, 428);
        self.tableScroll.frame = NSMakeRect(804, 300, 376, 382);
        self.curBtn.frame      = NSMakeRect(804, 690, 376, 34);
        self.codeScroll.frame  = NSMakeRect(20, 64, 1160, 228);
        [self.flowView setNeedsDisplay:YES];
    } else {
        self.graph.frame       = NSMakeRect(20, 64, 764, 664);
        self.replayView.frame  = NSMakeRect(20, 64, 764, 664);
        self.tableScroll.frame = NSMakeRect(804, 64, 376, 618);
        self.curBtn.frame      = NSMakeRect(804, 690, 376, 34);
        self.codeScroll.frame  = NSMakeRect(20, 64, 1160, 0);
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

/* 親ノード(呼び出し元) / 子ノード(呼び出し先) を一覧に積む (クリックで移動)。 */
- (void)appendParentsChildren:(int)funcIdx to:(NSMutableArray *)rows {
    if (funcIdx < 0 || funcIdx >= (int)self.fns.count) return;
    [rows addObject:@{ @"kind": @"[親ノード]", @"name": @"", @"info": @"" }];
    for (NSDictionary *e in self.edges)
        if ([e[@"to"] intValue] == funcIdx) {
            int s = [e[@"from"] intValue];
            if (s >= 0 && s < (int)self.fns.count)
                [rows addObject:@{ @"kind": @"↑ parent", @"name": self.fns[s][@"name"],
                                   @"info": self.fns[s][@"address"], @"idx": @(s) }];
        }
    [rows addObject:@{ @"kind": @"[子ノード]", @"name": @"", @"info": @"" }];
    for (NSDictionary *e in self.edges)
        if ([e[@"from"] intValue] == funcIdx) {
            int t = [e[@"to"] intValue];
            if (t >= 0 && t < (int)self.fns.count)
                [rows addObject:@{ @"kind": @"↓ child", @"name": self.fns[t][@"name"],
                                   @"info": self.fns[t][@"address"], @"idx": @(t) }];
        }
}

/* 現在の選択スコープの [関数一覧] と、選択関数の [親/子ノード] を構築。 */
- (void)buildScopeList {
    NSMutableArray *rows = [NSMutableArray array];
    [rows addObject:@{ @"kind": @"[関数一覧]", @"name": @"", @"info": @"" }];
    for (int i = 0; i < (int)self.fns.count; i++) {
        if (self.curRing >= 0) {
            if ([self.fns[i][@"roleIdx"] intValue] != self.curRole) continue;
            if ([self.fns[i][@"ring"] intValue] != self.curRing) continue;
        } else if (self.curRole >= 0) {
            if ([self.fns[i][@"roleIdx"] intValue] != self.curRole) continue;
        }
        [rows addObject:@{ @"kind": @"func", @"name": self.fns[i][@"name"],
                           @"info": self.fns[i][@"address"], @"idx": @(i) }];
    }
    [self appendParentsChildren:self.curFunc to:rows];
    self.paneRows = rows;
    [self.table reloadData];
}

/* クリックで view をトグル: [関数一覧] + [コード＋親ノード+子ノード]。 */
- (void)toggleScopeView {
    if (self.listCodeVisible) {
        self.listCodeVisible = NO;
        [self setCodeVisible:NO];
        [self updatePaneRole:self.curRole ring:self.curRing func:self.curFunc];
        return;
    }
    self.listCodeVisible = YES;
    [self buildScopeList];
    if (self.curFunc >= 0) [self showCodeFor:self.curFunc];
    [self setCodeVisible:YES];
}

- (void)onReplayToggle:(id)s {
    (void)s;
    if (!self.replayVisible) {
        NSString *p = self.replayStateField.stringValue;
        if (p.length == 0) p = [self defaultStatePath];
        [self loadReplayState:p];
        self.replayVisible = YES;
        self.replayView.hidden = NO;
        [self.replayView setNeedsDisplay:YES];
        self.replayBtn.title = @"Hide replay";
        self.replayBtn.active = YES;
        self.replayStateField.hidden = NO;
        self.pathField.hidden = YES;
    } else {
        self.replayVisible = NO;
        self.replayView.hidden = YES;
        self.replayBtn.title = @"Replay";
        self.replayBtn.active = NO;
        self.replayStateField.hidden = YES;
        self.pathField.hidden = NO;
    }
    [self.window makeFirstResponder:self.replayVisible ? self.replayView : self.graph];
}

- (void)onQuit:(id)s { (void)s; [NSApp terminate:nil]; }

- (NSString *)defaultStatePath {
    NSArray *cands = @[
        [[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/deepai-code-v2/Local Storage/leveldb"] copy],
        [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/deepai-code-v2/Local Storage"]
    ];
    for (NSString *c in cands)
        if ([[NSFileManager defaultManager] fileExistsAtPath:c]) return c;
    return cands[0];
}

- (void)loadReplayState:(NSString *)path {
    char *state = nx_deepai_load_state(path.UTF8String);
    if (!state) { self.replayStateField.stringValue = [@"(no state) " stringByAppendingString:path]; return; }
    nx_ui_layout_t L;
    if (nx_deepai_build_layout(state, &L) == 0) {
        [self.replayView setLayout:L];
        self.status.stringValue = [NSString stringWithFormat:
            @"Replay: %d nodes / %d edges / %d stuck tab(s) — state from %@",
            L.n_nodes, L.n_edges, L.n_stuck, path];
    }
    free(state);
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
