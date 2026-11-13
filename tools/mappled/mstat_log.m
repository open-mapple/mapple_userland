// mstat log: the unified log of one process, read live (OSLogStore of the whole system, as `log show` does)
#import <Foundation/Foundation.h>
#import <OSLog/OSLog.h>

int mstat_log(const char *process, unsigned seconds)
{
    @autoreleasepool {
        NSError *__autoreleasing e = nil;
        // +localStoreAndReturnError: is marked macOS only in the SDK; the class answers it on iOS too
        OSLogStore *store = nil;
        SEL sel = NSSelectorFromString(@"localStoreAndReturnError:");
        if ([OSLogStore respondsToSelector:sel]) {
            NSMethodSignature *sig = [OSLogStore methodSignatureForSelector:sel];
            NSInvocation *inv = [NSInvocation invocationWithMethodSignature:sig];
            inv.selector = sel;
            inv.target = [OSLogStore class];
            NSError *__autoreleasing *ep = &e;
            [inv setArgument:&ep atIndex:2];
            [inv invoke];
            __unsafe_unretained OSLogStore *s = nil;
            [inv getReturnValue:&s];
            store = s;
        }
        if (!store) {
            printf("log: no system log store (%s)\n", e.description.UTF8String ?: "localStoreAndReturnError: missing");
            return 1;
        }
        OSLogPosition *from = [store positionWithDate:[NSDate dateWithTimeIntervalSinceNow:-(double)seconds]];
        NSPredicate *p = [NSPredicate predicateWithFormat:@"process == %@", @(process)];
        OSLogEnumerator *en = [store entriesEnumeratorWithOptions:0 position:from predicate:p error:&e];
        if (!en) { printf("log: %s\n", e.description.UTF8String); return 1; }
        NSDateFormatter *f = [NSDateFormatter new];
        f.dateFormat = @"HH:mm:ss.SSS";
        unsigned n = 0;
        for (OSLogEntry *entry in en) {
            NSString *sub = @"", *cat = @"";
            if ([entry isKindOfClass:[OSLogEntryLog class]]) {
                sub = ((OSLogEntryLog *)entry).subsystem ?: @"";
                cat = ((OSLogEntryLog *)entry).category ?: @"";
            }
            printf("%s %s/%s %s\n", [f stringFromDate:entry.date].UTF8String, sub.UTF8String, cat.UTF8String,
                   entry.composedMessage.UTF8String);
            n++;
        }
        printf("log: %u entries of %s in the last %u s\n", n, process, seconds);
        return 0;
    }
}
