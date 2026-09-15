// winid.m — print the on-screen windows owned by a pid, front to back:
//   <windowid> <x> <y> <w> <h>
//
// `own_buffer.sh` uses this so it can capture the app's *own* window with
// `screencapture -l`. A screen rectangle shows whatever happens to be frontmost,
// so a window that failed to raise is captured as some other app — which reads
// as a rendering difference that is not one.
//
// Built on demand by own_buffer.sh; not committed as a binary.
#import <Cocoa/Cocoa.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: winid <pid>\n"); return 2; }
    int want = atoi(argv[1]);
    @autoreleasepool {
        CFArrayRef list = CGWindowListCopyWindowInfo(
            kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
            kCGNullWindowID);
        if (!list) { fprintf(stderr, "no window list\n"); return 1; }
        for (NSDictionary *w in (__bridge NSArray *)list) {
            NSNumber *pid = w[(id)kCGWindowOwnerPID];
            if (!pid || pid.intValue != want) continue;
            NSDictionary *b = w[(id)kCGWindowBounds];
            printf("%d %g %g %g %g\n", [w[(id)kCGWindowNumber] intValue],
                   [b[@"X"] doubleValue], [b[@"Y"] doubleValue],
                   [b[@"Width"] doubleValue], [b[@"Height"] doubleValue]);
        }
        CFRelease(list);
    }
    return 0;
}
