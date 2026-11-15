/*
 * mkicon — NeoDAX のアプリアイコンを生成する。
 *
 * 依存を増やさないため、CoreGraphics で直接描き、PNG を書き出す。
 * 呼び出し側 (gui/build.sh) が sips で各サイズを作り、iconutil で .icns にする。
 *
 * 使い方:
 *   mkicon OUT.png SIZE
 *
 * 図柄:
 *   角丸の暗い面 + 斜めのアクセント + 「N」(NeoDAX) を太線で描く。
 *   16px でも潰れないよう、線は太く、余白を広く取る。
 */
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 角丸四角のパス */
static CGPathRef rounded_rect(double x, double y, double w, double h, double r) {
    CGMutablePathRef p = CGPathCreateMutable();
    CGPathMoveToPoint(p, NULL, x + r, y);
    CGPathAddLineToPoint(p, NULL, x + w - r, y);
    CGPathAddArcToPoint(p, NULL, x + w, y, x + w, y + r, r);
    CGPathAddLineToPoint(p, NULL, x + w, y + h - r);
    CGPathAddArcToPoint(p, NULL, x + w, y + h, x + w - r, y + h, r);
    CGPathAddLineToPoint(p, NULL, x + r, y + h);
    CGPathAddArcToPoint(p, NULL, x, y + h, x, y + h - r, r);
    CGPathAddLineToPoint(p, NULL, x, y + r);
    CGPathAddArcToPoint(p, NULL, x, y, x + r, y, r);
    CGPathCloseSubpath(p);
    return p;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: mkicon OUT.png SIZE\n");
        return 2;
    }
    const char *out = argv[1];
    int size = atoi(argv[2]);
    if (size < 8) size = 8;

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(NULL, size, size, 8, 0, cs,
                                             kCGImageAlphaPremultipliedLast);
    if (!ctx) { fprintf(stderr, "mkicon: cannot create context\n"); return 1; }

    double S = (double)size;

    /* 背景: 角丸 + 縦グラデーション (暗いスレート) */
    CGPathRef bg = rounded_rect(0, 0, S, S, S * 0.22);
    CGContextSaveGState(ctx);
    CGContextAddPath(ctx, bg);
    CGContextClip(ctx);
    {
        CGColorRef top = CGColorCreateGenericRGB(0.11, 0.13, 0.16, 1.0);
        CGColorRef bot = CGColorCreateGenericRGB(0.04, 0.05, 0.07, 1.0);
        CGColorRef colors[2] = { bot, top };
        CFArrayRef arr = CFArrayCreate(NULL, (const void **)colors, 2,
                                       &kCFTypeArrayCallBacks);
        CGFloat locs[2] = { 0.0, 1.0 };
        CGGradientRef g = CGGradientCreateWithColors(cs, arr, locs);
        CGContextDrawLinearGradient(ctx, g, CGPointMake(0, 0),
                                    CGPointMake(0, S), 0);
        CGGradientRelease(g);
        CFRelease(arr);
        CGColorRelease(top);
        CGColorRelease(bot);
    }
    /* 斜めのアクセント帯 (ティール) */
    {
        CGContextSetRGBFillColor(ctx, 0.10, 0.55, 0.60, 0.16);
        CGContextMoveToPoint(ctx, 0, S * 0.78);
        CGContextAddLineToPoint(ctx, S * 0.78, 0);
        CGContextAddLineToPoint(ctx, S, 0);
        CGContextAddLineToPoint(ctx, 0, S);
        CGContextClosePath(ctx);
        CGContextFillPath(ctx);
    }
    CGContextRestoreGState(ctx);
    CGPathRelease(bg);

    /* 「N」を太線で描く。余白を広く取り、16px でも形が残るようにする。 */
    {
        double m = S * 0.24;          /* 左右上下の余白 */
        double x0 = m, x1 = S - m;
        double y0 = m, y1 = S - m;
        CGContextSetRGBStrokeColor(ctx, 0.24, 0.86, 0.90, 1.0);
        CGContextSetLineWidth(ctx, S * 0.115);
        CGContextSetLineCap(ctx, kCGLineCapRound);
        CGContextSetLineJoin(ctx, kCGLineJoinRound);

        CGMutablePathRef n = CGPathCreateMutable();
        CGPathMoveToPoint(n, NULL, x0, y0);       /* 左下 */
        CGPathAddLineToPoint(n, NULL, x0, y1);    /* 左上 */
        CGPathAddLineToPoint(n, NULL, x1, y0);    /* 右上へ斜め */
        CGPathAddLineToPoint(n, NULL, x1, y1);    /* 右下 */
        CGContextAddPath(ctx, n);
        CGContextStrokePath(ctx);
        CGPathRelease(n);

        /* N の起点に小さなノード (NeoDAX の「点」) */
        CGContextSetRGBFillColor(ctx, 0.98, 0.98, 1.0, 1.0);
        CGContextFillEllipseInRect(ctx,
            CGRectMake(x0 - S * 0.052, y1 - S * 0.052, S * 0.104, S * 0.104));
    }

    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    if (!img) { fprintf(stderr, "mkicon: no image\n"); return 1; }

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        NULL, (const UInt8 *)out, (CFIndex)strlen(out), false);
    if (!url) { CGImageRelease(img); return 1; }
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
        url, CFSTR("public.png"), 1, NULL);
    if (!dst) { fprintf(stderr, "mkicon: cannot write %s\n", out); CFRelease(url); CGImageRelease(img); return 1; }
    CGImageDestinationAddImage(dst, img, NULL);
    int ok = CGImageDestinationFinalize(dst) ? 0 : 1;
    CFRelease(dst);
    CFRelease(url);
    CGImageRelease(img);
    if (ok) fprintf(stderr, "mkicon: wrote %s (%dpx)\n", out, size);
    return ok;
}
